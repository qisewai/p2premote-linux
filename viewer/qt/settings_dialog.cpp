#include "settings_dialog.h"
#include "app_theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QScrollArea* wrapScrollable(QWidget* inner, QWidget* parent) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    return scroll;
}

QFrame* makeCard(QWidget* parent, QVBoxLayout** out_layout = nullptr) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);
    if (out_layout) *out_layout = layout;
    return card;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel* makeMuted(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

void addNavHeader(QListWidget* nav, const QString& text) {
    auto* item = new QListWidgetItem(text, nav);
    item->setFlags(Qt::NoItemFlags);
}

} // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : FramelessDialog(QStringLiteral("设置 · P2P Remote"), true, parent) {
    setObjectName(QStringLiteral("settingsDialog"));
    setMinimumSize(700, 480);
    resize(800, 560);

    contentLayout()->setContentsMargins(0, 0, 0, 0);
    contentLayout()->setSpacing(0);

    auto* body = new QWidget(contentWidget());
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    nav_ = new QListWidget(body);
    nav_->setObjectName(QStringLiteral("settingsNav"));
    nav_->setFixedWidth(188);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_layout->addWidget(nav_);

    pages_ = new QStackedWidget(body);
    body_layout->addWidget(pages_, 1);

    contentLayout()->addWidget(body, 1);

    auto addNavPage = [&](const QString& label, QWidget* page) {
        const int page_idx = pages_->addWidget(page);
        auto* item = new QListWidgetItem(label, nav_);
        const int nav_row = nav_->row(item);
        nav_to_page_.insert(nav_row, page_idx);
    };

    addNavHeader(nav_, QStringLiteral("软件设置"));
    addNavPage(QStringLiteral("外观与皮肤"), buildAppearancePage());
    addNavPage(QStringLiteral("常规与使用"), buildGeneralPage());
    addNavPage(QStringLiteral("画面与音频"), buildStreamAudioPage());

    const auto plugins = AppSettings::discoverPlugins();
    addNavHeader(nav_, QStringLiteral("插件与扩展"));
    addNavPage(QStringLiteral("插件总览"), buildPluginsOverviewPage(plugins));
    for (const auto& spec : plugins) {
        if (spec.fields.isEmpty()) continue;
        addNavPage(spec.name, buildPluginSettingsPage(spec));
    }

    // Bottom footer bar
    auto* footer = new QFrame(contentWidget());
    footer->setObjectName(QStringLiteral("remoteStatusbar"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(16, 10, 16, 10);
    footer_layout->setSpacing(10);

    save_hint_ = makeMuted(QStringLiteral("外观设置会实时预览；插件设置将在下一次连接或插件重新读取配置时生效。"), footer);
    footer_layout->addWidget(save_hint_, 1);

    auto* reset_btn = new QPushButton(QStringLiteral("恢复默认"), footer);
    reset_btn->setProperty("variant", QStringLiteral("subtle"));
    auto* close_btn = new QPushButton(QStringLiteral("取消"), footer);
    auto* save_btn = new QPushButton(QStringLiteral("保存设置"), footer);
    save_btn->setObjectName(QStringLiteral("primary"));
    footer_layout->addWidget(reset_btn);
    footer_layout->addWidget(close_btn);
    footer_layout->addWidget(save_btn);
    contentLayout()->addWidget(footer);

    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (nav_to_page_.contains(row)) {
            pages_->setCurrentIndex(nav_to_page_.value(row));
        }
    });
    connect(save_btn, &QPushButton::clicked, this, [this] {
        saveAll();
        accept();
    });
    connect(close_btn, &QPushButton::clicked, this, [this] {
        // Revert any live appearance preview if cancelled
        AppTheme::apply(AppSettings::instance().data());
        reject();
    });
    connect(reset_btn, &QPushButton::clicked, this, &SettingsDialog::resetAll);

    loadFromSettings();
    nav_->setCurrentRow(1);
}

QWidget* SettingsDialog::buildAppearancePage() {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);

    layout->addWidget(makeSectionTitle(QStringLiteral("外观与皮肤"), container));
    layout->addWidget(makeMuted(QStringLiteral("统一控制主窗口、远程控制窗口、设置中心以及所有插件弹窗的皮肤风格与配色。"), container));

    QVBoxLayout* card_layout{};
    auto* card = makeCard(container, &card_layout);
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(14);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    theme_combo_ = new QComboBox(card);
    theme_combo_->addItem(QStringLiteral("深邃暗黑（默认）"), QStringLiteral("dark"));
    theme_combo_->addItem(QStringLiteral("午夜深蓝"), QStringLiteral("midnight"));
    theme_combo_->addItem(QStringLiteral("简约浅色"), QStringLiteral("light"));
    form->addRow(QStringLiteral("主题皮肤"), theme_combo_);

    accent_combo_ = new QComboBox(card);
    accent_combo_->addItem(QStringLiteral("科技蓝"), QStringLiteral("blue"));
    accent_combo_->addItem(QStringLiteral("翡翠绿"), QStringLiteral("emerald"));
    accent_combo_->addItem(QStringLiteral("星云紫"), QStringLiteral("violet"));
    accent_combo_->addItem(QStringLiteral("琥珀橙"), QStringLiteral("amber"));
    form->addRow(QStringLiteral("主题强调色"), accent_combo_);

    font_combo_ = new QComboBox(card);
    font_combo_->addItem(QStringLiteral("紧凑（13 px）"), 13);
    font_combo_->addItem(QStringLiteral("标准（14 px）"), 14);
    font_combo_->addItem(QStringLiteral("舒适（15 px）"), 15);
    form->addRow(QStringLiteral("界面字号"), font_combo_);

    card_layout->addLayout(form);
    layout->addWidget(card);
    layout->addStretch(1);

    connect(theme_combo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::previewAppearance);
    connect(accent_combo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::previewAppearance);
    connect(font_combo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::previewAppearance);

    return wrapScrollable(container, pages_);
}

QWidget* SettingsDialog::buildGeneralPage() {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);

    layout->addWidget(makeSectionTitle(QStringLiteral("常规与使用"), container));
    layout->addWidget(makeMuted(QStringLiteral("配置主窗口关闭行为、登录后本机接收策略以及默认账号服务地址。"), container));

    QVBoxLayout* card_layout{};
    auto* card = makeCard(container, &card_layout);

    close_to_tray_ = new QCheckBox(QStringLiteral("关闭主窗口时最小化到系统托盘（不退出后台接收）"), card);
    auto_receive_ = new QCheckBox(QStringLiteral("登录成功后自动开启本机远程接收"), card);
    card_layout->addWidget(close_to_tray_);
    card_layout->addWidget(auto_receive_);

    auto* divider = new QFrame(card);
    divider->setObjectName(QStringLiteral("divider"));
    card_layout->addWidget(divider);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    default_server_ = new QLineEdit(card);
    default_server_->setPlaceholderText(QStringLiteral("https://"));
    form->addRow(QStringLiteral("默认服务地址"), default_server_);
    card_layout->addLayout(form);

    layout->addWidget(card);
    layout->addStretch(1);
    return wrapScrollable(container, pages_);
}

QWidget* SettingsDialog::buildStreamAudioPage() {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);

    layout->addWidget(makeSectionTitle(QStringLiteral("画面与音频默认设置"), container));
    layout->addWidget(makeMuted(QStringLiteral("新建远程会话时自动应用以下默认画质与音频选项，会话中仍可随时临时调整。"), container));

    QVBoxLayout* video_layout{};
    auto* video_card = makeCard(container, &video_layout);
    auto* vform = new QFormLayout;
    vform->setHorizontalSpacing(18);
    vform->setVerticalSpacing(12);
    vform->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    quality_combo_ = new QComboBox(video_card);
    quality_combo_->addItem(QStringLiteral("720p"), 0);
    quality_combo_->addItem(QStringLiteral("1080p（推荐）"), 1);
    quality_combo_->addItem(QStringLiteral("1440p"), 2);
    vform->addRow(QStringLiteral("默认分辨率"), quality_combo_);

    bitrate_combo_ = new QComboBox(video_card);
    bitrate_combo_->addItem(QStringLiteral("省流 · 4 Mbps"), 0);
    bitrate_combo_->addItem(QStringLiteral("自动 · 8 Mbps"), 1);
    bitrate_combo_->addItem(QStringLiteral("清晰 · 16 Mbps"), 2);
    vform->addRow(QStringLiteral("默认画质码率"), bitrate_combo_);

    video_layout->addLayout(vform);
    show_diagnostics_ = new QCheckBox(QStringLiteral("显示画面浮层"), video_card);
    video_layout->addWidget(show_diagnostics_);
    layout->addWidget(video_card);

    QVBoxLayout* audio_layout{};
    auto* audio_card = makeCard(container, &audio_layout);
    speaker_enabled_ = new QCheckBox(QStringLiteral("默认开启远端扬声器声音"), audio_card);
    audio_layout->addWidget(speaker_enabled_);

    auto* aform = new QFormLayout;
    aform->setHorizontalSpacing(18);
    aform->setVerticalSpacing(12);
    aform->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    volume_combo_ = new QComboBox(audio_card);
    volume_combo_->addItem(QStringLiteral("100%"), 100);
    volume_combo_->addItem(QStringLiteral("75%"), 75);
    volume_combo_->addItem(QStringLiteral("50%"), 50);
    volume_combo_->addItem(QStringLiteral("25%"), 25);
    aform->addRow(QStringLiteral("默认扬声器音量"), volume_combo_);
    audio_layout->addLayout(aform);

    mic_fallback_ = new QCheckBox(QStringLiteral("远端无虚拟声卡时，将对讲麦克风声音由远端扬声器播放"), audio_card);
    audio_layout->addWidget(mic_fallback_);
    layout->addWidget(audio_card);

    layout->addStretch(1);
    return wrapScrollable(container, pages_);
}

QWidget* SettingsDialog::buildPluginsOverviewPage(const QVector<PluginSettingSpec>& plugins) {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(12);

    layout->addWidget(makeSectionTitle(QStringLiteral("插件总览与扩展说明"), container));
    layout->addWidget(makeMuted(QStringLiteral(
        "插件可在各自的 plugin.json 中声明 \"settings\" 字段（支持 bool / string / int / number / select），"
        "声明后会自动出现在左侧「插件与扩展」列表中，并与 Lua 的 p2p.config.get / set 读写同一配置文件。"), container));

    if (plugins.isEmpty()) {
        QVBoxLayout* empty_layout{};
        auto* card = makeCard(container, &empty_layout);
        empty_layout->addWidget(makeMuted(QStringLiteral("未在 plugins 目录中发现插件。"), card));
        layout->addWidget(card);
    } else {
        for (const auto& spec : plugins) {
            QVBoxLayout* card_layout{};
            auto* card = makeCard(container, &card_layout);
            card_layout->setSpacing(4);
            auto* top = new QHBoxLayout;
            auto* title = new QLabel(QStringLiteral("%1  (v%2)").arg(spec.name, spec.version), card);
            title->setObjectName(QStringLiteral("titleLabel"));
            top->addWidget(title);
            top->addStretch(1);
            auto* badge = makeMuted(spec.fields.isEmpty()
                ? QStringLiteral("无自定义配置项")
                : QStringLiteral("%1 项可配置").arg(spec.fields.size()), card);
            top->addWidget(badge);
            card_layout->addLayout(top);

            const auto meta = QStringLiteral("ID: %1  ·  角色: %2  ·  权限: %3")
                .arg(spec.id, spec.roles.join(QStringLiteral("/")), spec.permissions.join(QStringLiteral(", ")));
            card_layout->addWidget(makeMuted(meta, card));
            if (!spec.description.isEmpty()) {
                card_layout->addWidget(makeMuted(spec.description, card));
            }
            layout->addWidget(card);
        }
    }

    layout->addStretch(1);
    return wrapScrollable(container, pages_);
}

QWidget* SettingsDialog::buildPluginSettingsPage(const PluginSettingSpec& spec) {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);

    layout->addWidget(makeSectionTitle(spec.name, container));
    const auto subtitle = spec.description.isEmpty()
        ? QStringLiteral("插件 ID：%1  ·  版本 v%2").arg(spec.id, spec.version)
        : spec.description;
    layout->addWidget(makeMuted(subtitle, container));

    QVBoxLayout* card_layout{};
    auto* card = makeCard(container, &card_layout);

    PluginPageBinding binding;
    binding.spec = spec;
    const auto saved = AppSettings::loadPluginConfig(spec.id);

    for (const auto& field : spec.fields) {
        const bool has_saved = saved.contains(field.key);
        const QJsonValue current_val = has_saved ? saved.value(field.key) : QJsonValue::fromVariant(field.default_value);

        auto* row_box = new QWidget(card);
        auto* row_layout = new QVBoxLayout(row_box);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(4);

        QWidget* control{};
        if (field.type == QStringLiteral("bool")) {
            auto* cb = new QCheckBox(field.label, row_box);
            cb->setChecked(current_val.toBool(field.default_value.toBool()));
            row_layout->addWidget(cb);
            control = cb;
        } else {
            auto* h = new QHBoxLayout;
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(12);
            auto* lbl = new QLabel(field.label, row_box);
            lbl->setMinimumWidth(140);
            h->addWidget(lbl);

            if (field.type == QStringLiteral("int")) {
                auto* sp = new QSpinBox(row_box);
                sp->setRange(static_cast<int>(field.min_value), static_cast<int>(field.max_value));
                sp->setSingleStep(std::max(1, static_cast<int>(field.step_value)));
                if (!field.suffix.isEmpty()) sp->setSuffix(field.suffix);
                sp->setValue(current_val.toInt(field.default_value.toInt()));
                h->addWidget(sp, 1);
                control = sp;
            } else if (field.type == QStringLiteral("number")) {
                auto* sp = new QDoubleSpinBox(row_box);
                sp->setRange(field.min_value, field.max_value);
                sp->setSingleStep(field.step_value > 0 ? field.step_value : 0.1);
                sp->setDecimals(field.decimals);
                if (!field.suffix.isEmpty()) sp->setSuffix(field.suffix);
                sp->setValue(current_val.toDouble(field.default_value.toDouble()));
                h->addWidget(sp, 1);
                control = sp;
            } else if (field.type == QStringLiteral("select")) {
                auto* cb = new QComboBox(row_box);
                for (const auto& opt : field.options) {
                    cb->addItem(opt.second, opt.first);
                }
                const auto cur_str = current_val.toString(field.default_value.toString());
                const int idx = cb->findData(cur_str);
                if (idx >= 0) cb->setCurrentIndex(idx);
                h->addWidget(cb, 1);
                control = cb;
            } else {
                auto* edit = new QLineEdit(row_box);
                if (!field.placeholder.isEmpty()) edit->setPlaceholderText(field.placeholder);
                if (field.secret) edit->setEchoMode(QLineEdit::Password);
                edit->setText(current_val.toString(field.default_value.toString()));
                h->addWidget(edit, 1);
                control = edit;
            }
            row_layout->addLayout(h);
        }

        if (!field.description.isEmpty()) {
            row_layout->addWidget(makeMuted(field.description, row_box));
        }

        card_layout->addWidget(row_box);
        binding.fields.append({field, control});
    }

    plugin_pages_.append(std::move(binding));
    layout->addWidget(card);
    layout->addStretch(1);
    return wrapScrollable(container, pages_);
}

void SettingsDialog::loadFromSettings() {
    loading_ = true;
    const auto& d = AppSettings::instance().data();

    auto setComboData = [](QComboBox* combo, const QVariant& value) {
        const QSignalBlocker b(combo);
        const int idx = combo->findData(value);
        if (idx >= 0) combo->setCurrentIndex(idx);
    };

    setComboData(theme_combo_, d.theme);
    setComboData(accent_combo_, d.accent);
    setComboData(font_combo_, d.font_size);

    close_to_tray_->setChecked(d.close_to_tray);
    auto_receive_->setChecked(d.auto_receive_on_login);
    default_server_->setText(d.default_server);

    setComboData(quality_combo_, d.default_quality);
    setComboData(bitrate_combo_, d.default_bitrate);
    show_diagnostics_->setChecked(d.show_diagnostics);
    speaker_enabled_->setChecked(d.speaker_enabled);
    setComboData(volume_combo_, d.speaker_volume);
    mic_fallback_->setChecked(d.mic_fallback);
    loading_ = false;
}

AppSettingsData SettingsDialog::collectAppSettings() const {
    AppSettingsData d;
    d.theme = theme_combo_->currentData().toString();
    d.accent = accent_combo_->currentData().toString();
    d.font_size = font_combo_->currentData().toInt();
    d.close_to_tray = close_to_tray_->isChecked();
    d.auto_receive_on_login = auto_receive_->isChecked();
    d.default_server = default_server_->text().trimmed();
    d.default_quality = quality_combo_->currentData().toInt();
    d.default_bitrate = bitrate_combo_->currentData().toInt();
    d.show_diagnostics = show_diagnostics_->isChecked();
    d.speaker_enabled = speaker_enabled_->isChecked();
    d.speaker_volume = volume_combo_->currentData().toInt();
    d.mic_fallback = mic_fallback_->isChecked();
    return d;
}

void SettingsDialog::previewAppearance() {
    if (loading_) return;
    AppTheme::apply(collectAppSettings());
}

void SettingsDialog::saveAll() {
    const auto next = collectAppSettings();
    AppSettings::instance().save(next);
    AppTheme::apply(next);

    for (const auto& page : plugin_pages_) {
        QJsonObject cfg = AppSettings::loadPluginConfig(page.spec.id);
        for (const auto& fb : page.fields) {
            if (!fb.widget) continue;
            if (fb.field.type == QStringLiteral("bool")) {
                cfg.insert(fb.field.key, static_cast<QCheckBox*>(fb.widget)->isChecked());
            } else if (fb.field.type == QStringLiteral("int")) {
                cfg.insert(fb.field.key, static_cast<QSpinBox*>(fb.widget)->value());
            } else if (fb.field.type == QStringLiteral("number")) {
                cfg.insert(fb.field.key, static_cast<QDoubleSpinBox*>(fb.widget)->value());
            } else if (fb.field.type == QStringLiteral("select")) {
                cfg.insert(fb.field.key, static_cast<QComboBox*>(fb.widget)->currentData().toString());
            } else {
                cfg.insert(fb.field.key, static_cast<QLineEdit*>(fb.widget)->text());
            }
        }
        AppSettings::savePluginConfig(page.spec.id, cfg);
    }
}

void SettingsDialog::resetAll() {
    AppSettings::instance().resetDefaults();
    loadFromSettings();
    AppTheme::apply(AppSettings::instance().data());

    for (const auto& page : plugin_pages_) {
        for (const auto& fb : page.fields) {
            if (!fb.widget) continue;
            if (fb.field.type == QStringLiteral("bool")) {
                static_cast<QCheckBox*>(fb.widget)->setChecked(fb.field.default_value.toBool());
            } else if (fb.field.type == QStringLiteral("int")) {
                static_cast<QSpinBox*>(fb.widget)->setValue(fb.field.default_value.toInt());
            } else if (fb.field.type == QStringLiteral("number")) {
                static_cast<QDoubleSpinBox*>(fb.widget)->setValue(fb.field.default_value.toDouble());
            } else if (fb.field.type == QStringLiteral("select")) {
                auto* cb = static_cast<QComboBox*>(fb.widget);
                const int idx = cb->findData(fb.field.default_value.toString());
                if (idx >= 0) cb->setCurrentIndex(idx);
            } else {
                static_cast<QLineEdit*>(fb.widget)->setText(fb.field.default_value.toString());
            }
        }
    }
}
