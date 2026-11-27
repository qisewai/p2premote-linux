#include "connection_stage_overlay.h"
#include "app_theme.h"

#include <QApplication>
#include <QImage>
#include <QPushButton>

#include <cmath>
#include <iostream>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

bool imageHasVariation(const QImage& image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return false;
    const QRgb first = image.pixel(0, 0);
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            if (image.pixel(x, y) != first) return true;
        }
    }
    return false;
}

bool testClassifyStages() {
    {
        const auto s = classifyConnectionStage(P2P_IDLE, false, QStringLiteral("正在连接设备…"));
        if (!check(s.stage == ConnectionVisualStage::signaling && s.active_step == 0 && s.animating,
                "P2P_IDLE should classify as signaling")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_WAITING, false, QStringLiteral("正在连接 192.168.1.2:27887…"));
        if (!check(s.stage == ConnectionVisualStage::signaling && s.active_step == 0 && s.animating,
                "P2P_WAITING connecting should classify as signaling")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_WAITING, false, QStringLiteral("正在验证连接凭据…"));
        if (!check(s.stage == ConnectionVisualStage::approval && s.active_step == 1 && s.animating,
                "P2P_WAITING verification should classify as approval")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_APPROVAL, false, QStringLiteral("正在连接云端信令…"));
        if (!check(s.stage == ConnectionVisualStage::signaling && s.active_step == 0 && s.animating,
                "P2P_APPROVAL with 信令 should classify as signaling")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_APPROVAL, false, QStringLiteral("等待被控端授权…"));
        if (!check(s.stage == ConnectionVisualStage::approval && s.active_step == 1 && s.warning_tone && s.animating,
                "P2P_APPROVAL waiting should classify as approval with warning tone")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_NEGOTIATING, false, QStringLiteral("认证成功，正在协商连接…"));
        if (!check(s.stage == ConnectionVisualStage::negotiating && s.active_step == 2 && s.animating,
                "P2P_NEGOTIATING should classify as negotiating")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_STREAMING, false, QStringLiteral("已建立媒体通道，等待被控端画面…"));
        if (!check(s.stage == ConnectionVisualStage::loading_video && s.active_step == 3 && !s.warning_tone && s.animating,
                "P2P_STREAMING without video should classify as loading_video")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_STREAMING, false, QStringLiteral("已连接，但暂未收到被控端画面。"));
        if (!check(s.stage == ConnectionVisualStage::loading_video && s.active_step == 3 && s.warning_tone && s.animating,
                "P2P_STREAMING delayed should raise warning tone")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_RECONNECTING, false, QStringLiteral("将在 3 秒后重连"));
        if (!check(s.stage == ConnectionVisualStage::reconnecting && s.warning_tone && s.animating,
                "P2P_RECONNECTING should classify as reconnecting")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_STOPPED, false, QStringLiteral("已断开。"));
        if (!check(s.stage == ConnectionVisualStage::stopped && s.active_step == -1 && !s.animating && !s.error_tone,
                "P2P_STOPPED normal disconnect should not set error tone")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_STOPPED, false, QStringLiteral("连接或认证超时。"));
        if (!check(s.stage == ConnectionVisualStage::stopped && s.active_step == -1 && !s.animating && s.error_tone,
                "P2P_STOPPED timeout should set error tone")) return false;
    }
    {
        const auto s = classifyConnectionStage(P2P_STREAMING, true, QStringLiteral("已连接 · H.264"));
        if (!check(s.stage == ConnectionVisualStage::ready && s.active_step == 4
                && std::abs(s.target_progress - 1.0) < 1e-6 && !s.animating,
                "P2P_STREAMING with video should classify as ready")) return false;
    }
    return true;
}

bool testTransitionsAndImmediateHide() {
    QWidget host;
    host.resize(1160, 720);
    ConnectionStageOverlay overlay(&host);
    overlay.setGeometry(host.rect());
    host.show();
    QApplication::processEvents();

    // 1. Connecting stages & smooth progress interpolation.
    overlay.setSessionState(P2P_WAITING, false, QStringLiteral("正在连接 192.168.1.2:27887…"), true);
    if (!check(overlay.isVisible() && overlay.isAnimationRunning(),
            "Overlay should be visible and animating when connecting")) return false;
    if (!check(overlay.stage() == ConnectionVisualStage::signaling,
            "Initial stage should be signaling")) return false;

    const qreal initial_progress = overlay.displayProgress();
    for (int i = 0; i < 10; ++i) overlay.advanceAnimation(0.02);
    const qreal p_signaling = overlay.displayProgress();
    if (!check(p_signaling > initial_progress && p_signaling < 0.35,
            "Signaling progress should advance smoothly")) return false;

    // Identical poll tick must be a strict no-op (no reset).
    overlay.setSessionState(P2P_WAITING, false, QStringLiteral("正在连接 192.168.1.2:27887…"), true);
    if (!check(std::abs(overlay.displayProgress() - p_signaling) < 1e-9,
            "Identical state poll must not reset displayProgress")) return false;

    overlay.setSessionState(P2P_APPROVAL, false, QStringLiteral("等待被控端授权…"), true);
    for (int i = 0; i < 12; ++i) overlay.advanceAnimation(0.02);
    const qreal p_approval = overlay.displayProgress();
    if (!check(p_approval > p_signaling, "Approval progress should increase past signaling")) return false;

    overlay.setSessionState(P2P_NEGOTIATING, false, QStringLiteral("认证成功，正在协商连接…"), true);
    for (int i = 0; i < 12; ++i) overlay.advanceAnimation(0.02);
    const qreal p_neg = overlay.displayProgress();
    if (!check(p_neg > p_approval, "Negotiating progress should increase past approval")) return false;

    overlay.setSessionState(P2P_STREAMING, false, QStringLiteral("已建立媒体通道，等待被控端画面…"), true);
    for (int i = 0; i < 12; ++i) overlay.advanceAnimation(0.02);
    const qreal p_wait_video = overlay.displayProgress();
    if (!check(p_wait_video > p_neg, "Video loading progress should increase past negotiating")) return false;

    // 2. Visibility pause/resume while connecting.
    host.hide();
    QApplication::processEvents();
    if (!check(!overlay.isAnimationRunning(), "Hiding host/overlay must stop animation timer")) return false;
    host.show();
    QApplication::processEvents();
    if (!check(overlay.isAnimationRunning(), "Showing overlay while connecting must resume animation timer")) return false;

    // 3. First-frame arrival hides overlay immediately (zero delay / no post-frame fade).
    overlay.setSessionState(P2P_NEGOTIATING, true, QStringLiteral("认证成功，正在协商连接…"), true);
    if (!check(!overlay.isVisible() && !overlay.isAnimationRunning() && overlay.stage() == ConnectionVisualStage::ready,
            "First frame must hide overlay and stop animation timer immediately")) return false;
    if (!check(!overlay.reconnectButton()->isVisible(),
            "Reconnect button must remain hidden when video is ready")) return false;

    // 4. Stopped state shows immediately with timer stopped and working reconnect button.
    int reconnect_clicks = 0;
    QObject::connect(&overlay, &ConnectionStageOverlay::reconnectRequested, &overlay, [&] {
        ++reconnect_clicks;
    });

    overlay.setSessionState(P2P_STOPPED, false, QStringLiteral("已断开，可以重新连接。"), true);
    if (!check(overlay.isVisible() && overlay.stage() == ConnectionVisualStage::stopped,
            "Stopped state should show stopped overlay immediately")) return false;
    if (!check(!overlay.isAnimationRunning(),
            "Stopped overlay must not run animation timer")) return false;
    if (!check(overlay.reconnectButton()->isVisible(),
            "Reconnect button must be visible when can_reconnect is true")) return false;

    overlay.reconnectButton()->click();
    if (!check(reconnect_clicks == 1, "Clicking reconnect button must emit reconnectRequested")) return false;

    overlay.setSessionState(P2P_STOPPED, false, QStringLiteral("初始化失败。"), false);
    if (!check(!overlay.reconnectButton()->isVisible(),
            "Reconnect button must hide when can_reconnect is false")) return false;

    // 5. Reconnect from stopped resumes connecting state cleanly.
    overlay.setSessionState(P2P_WAITING, false, QStringLiteral("正在连接…"), true);
    if (!check(overlay.isVisible() && overlay.stage() == ConnectionVisualStage::signaling
            && !overlay.reconnectButton()->isVisible() && overlay.isAnimationRunning(),
            "Restarting from stopped must hide reconnect button and resume connecting animation")) return false;

    return true;
}

bool testOffscreenRenderingThemes() {
    const QStringList themes = {
        QStringLiteral("dark"),
        QStringLiteral("midnight"),
        QStringLiteral("light")
    };
    const QList<QSize> viewports = {
        QSize(1160, 720),
        QSize(480, 320)
    };

    for (const auto& theme_name : themes) {
        AppSettingsData settings;
        settings.theme = theme_name;
        settings.accent = theme_name == QStringLiteral("light") ? QStringLiteral("emerald") : QStringLiteral("blue");
        AppTheme::apply(settings);

        for (const auto& size : viewports) {
            QWidget host;
            host.resize(size);
            ConnectionStageOverlay overlay(&host);
            overlay.setGeometry(host.rect());
            overlay.refreshTheme();
            host.show();

            // Render connecting state.
            overlay.setSessionState(P2P_NEGOTIATING, false, QStringLiteral("认证成功，正在协商连接…"), true);
            for (int i = 0; i < 10; ++i) overlay.advanceAnimation(0.02);
            QImage connecting_img(size, QImage::Format_ARGB32_Premultiplied);
            connecting_img.fill(Qt::transparent);
            overlay.render(&connecting_img);
            if (!check(imageHasVariation(connecting_img), "Connecting overlay render must produce visible pixels")) return false;

            // Render reconnecting state.
            overlay.setSessionState(P2P_RECONNECTING, false, QStringLiteral("将在 2 秒后重连…"), true);
            overlay.advanceAnimation(0.05);
            QImage reconn_img(size, QImage::Format_ARGB32_Premultiplied);
            reconn_img.fill(Qt::transparent);
            overlay.render(&reconn_img);
            if (!check(imageHasVariation(reconn_img), "Reconnecting overlay render must produce visible pixels")) return false;

            // Render stopped state.
            overlay.setSessionState(P2P_STOPPED, false, QStringLiteral("连接超时。"), true);
            QImage stopped_img(size, QImage::Format_ARGB32_Premultiplied);
            stopped_img.fill(Qt::transparent);
            overlay.render(&stopped_img);
            if (!check(imageHasVariation(stopped_img), "Stopped overlay render must produce visible pixels")) return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    AppTheme::apply(AppSettingsData{});

    if (!testClassifyStages()) return 1;
    if (!testTransitionsAndImmediateHide()) return 1;
    if (!testOffscreenRenderingThemes()) return 1;
    return 0;
}
