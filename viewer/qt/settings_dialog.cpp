#include "settings_dialog.h"
#include "app_theme.h"
#include "update_client.h"

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
#include <QStyle>
#include <QTimer>
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

SettingsDialog::SettingsDialog(QWidget* parent, UpdateClient* updater)
    : FramelessDialog(QStringLiteral("设置 · P2P Remote"), true, parent), updater_(updater) {
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

    auto addNavPage = [&](const QString& label, QScrollArea* page) {
        const int page_idx = pages_->addWidget(page);
        auto* item = new QListWidgetItem(label, nav_);
        const int nav_row = nav_->row(item);
        nav_to_page_.insert(nav_row, page_idx);
        return nav_row;
    };

    addNavHeader(nav_, QStringLiteral("软件设置"));
    const QHash<QString, int> builtin{
        {QStringLiteral("appearance"), addNavPage(QStringLiteral("外观与皮肤"), buildAppearancePage())},
        {QStringLiteral("general"), addNavPage(QStringLiteral("常规与使用"), buildGeneralPage())},
        {QStringLiteral("stream-audio"), addNavPage(QStringLiteral("画面与音频"), buildStreamAudioPage())}
    };

    auto plugins = AppSettings::discoverPlugins();
    std::stable_sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b) {
        return a.order < b.order;
    });
    struct Page {
        QString id, title;
        bool extension{};
        QVector<const PluginSettingSpec*> specs;
    };
    QVector<Page> contributed;
    for (const auto& spec : plugins) {
        if (spec.fields.isEmpty()) continue;
        if (spec.placement == PluginSettingSpec::Placement::embed) {
            addPluginCard(spec, builtin.value(spec.page));
        } else {
            const auto page = std::ranges::find(contributed, spec.page, &Page::id);
            if (page == contributed.end()) {
                contributed.append({spec.page, spec.page_title, spec.extension_page, {&spec}});
            } else {
                page->specs.append(&spec);
            }
        }
    }
    const auto addContributedPages = [&](bool extension) {
        for (const auto& page : contributed) {
            if (page.extension != extension) continue;
            auto* container = new QWidget(pages_);
            auto* layout = new QVBoxLayout(container);
            layout->setContentsMargins(22, 18, 22, 18);
            layout->setSpacing(14);
            layout->addWidget(makeSectionTitle(page.title, container));
            layout->addStretch(1);
            const int row = addNavPage(page.title, wrapScrollable(container, pages_));
            for (const auto* spec : page.specs) addPluginCard(*spec, row);
        }
    };
    addContributedPages(false);
    addNavHeader(nav_, QStringLiteral("插件与扩展"));
    addNavPage(QStringLiteral("插件总览"), buildPluginsOverviewPage(plugins));
    addContributedPages(true);

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
        if (saveAll()) accept();
    });
    connect(close_btn, &QPushButton::clicked, this, &QDialog::reject);
    connect(this, &QDialog::rejected, this, [] {
        AppTheme::apply(AppSettings::instance().data());
    });
    connect(reset_btn, &QPushButton::clicked, this, &SettingsDialog::resetAll);

    loadAppControls(AppSettings::instance().data());
    nav_->setCurrentRow(1);
}

QScrollArea* SettingsDialog::buildAppearancePage() {
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

QScrollArea* SettingsDialog::buildGeneralPage() {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(14);

    layout->addWidget(makeSectionTitle(QStringLiteral("常规与使用"), container));
#if defined(Q_OS_WIN)
    layout->addWidget(makeMuted(QStringLiteral("配置主窗口关闭行为、登录后本机接收策略以及默认账号服务地址。"), container));
#endif

    QVBoxLayout* card_layout{};
    auto* card = makeCard(container, &card_layout);

    close_to_tray_ = new QCheckBox(QStringLiteral("关闭主窗口时最小化到系统托盘"), card);
    card_layout->addWidget(close_to_tray_);
#if defined(Q_OS_WIN)
    auto_receive_ = new QCheckBox(QStringLiteral("登录后自动开启远程接收（本机已添加时）"), card);
    card_layout->addWidget(auto_receive_);
#endif

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

    // Software Update card
    QVBoxLayout* upd_layout{};
    auto* upd_card = makeCard(container, &upd_layout);
    auto* upd_hdr = new QHBoxLayout;
    upd_hdr->setContentsMargins(0, 0, 0, 0);
    upd_hdr->setSpacing(10);
    auto* ver_lbl = new QLabel(QStringLiteral("软件更新 · 当前版本 v%1").arg(UpdateClient::currentVersion()), upd_card);
    ver_lbl->setObjectName(QStringLiteral("cardTitle"));
    ver_lbl->setTextFormat(Qt::PlainText);
    upd_hdr->addWidget(ver_lbl, 1);

    check_update_btn_ = new QPushButton(QStringLiteral("检查更新"), upd_card);
    check_update_btn_->setProperty("variant", QStringLiteral("subtle"));
    connect(check_update_btn_, &QPushButton::clicked, this, [this] {
        if (!updater_) return;
        updater_->applySettings(auto_check_updates_->isChecked(),
                                auto_download_updates_->isChecked(),
                                update_mirror_url_->text().trimmed());
        if (updater_->state() == UpdateState::ReadyToInstall) {
            updater_->installUpdate();
        } else if (updater_->state() == UpdateState::Available) {
            updater_->downloadUpdate();
        } else {
            updater_->checkForUpdates(true);
        }
    });
    upd_hdr->addWidget(check_update_btn_);
    upd_layout->addLayout(upd_hdr);

    update_status_label_ = makeMuted(QStringLiteral("从 GitHub Releases 自动检查已签名的版本更新。"), upd_card);
    upd_layout->addWidget(update_status_label_);

    auto_check_updates_ = new QCheckBox(QStringLiteral("启动及后台定期自动检查更新"), upd_card);
    upd_layout->addWidget(auto_check_updates_);

    auto_download_updates_ = new QCheckBox(QStringLiteral("发现新版本时自动在后台下载并验签（空闲后或重启生效）"), upd_card);
    upd_layout->addWidget(auto_download_updates_);

    auto* upd_form = new QFormLayout;
    upd_form->setHorizontalSpacing(18);
    upd_form->setVerticalSpacing(10);
    upd_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    update_mirror_url_ = new QLineEdit(upd_card);
    update_mirror_url_->setPlaceholderText(QStringLiteral("留空使用默认 GitHub Releases；可填镜像前缀或自定义清单 URL"));
    upd_form->addRow(QStringLiteral("更新源/镜像"), update_mirror_url_);
    upd_layout->addLayout(upd_form);

    if (updater_) {
        connect(updater_, &UpdateClient::stateChanged, this, &SettingsDialog::syncUpdateStatus);
        connect(updater_, &UpdateClient::progressChanged, this, [this] { syncUpdateStatus(); });
        syncUpdateStatus();
    } else {
        check_update_btn_->setEnabled(false);
    }

    layout->addWidget(upd_card);
    layout->addStretch(1);
    return wrapScrollable(container, pages_);
}

void SettingsDialog::syncUpdateStatus() {
    if (!updater_ || !update_status_label_ || !check_update_btn_) return;
    const auto st = updater_->state();
    QString msg = updater_->statusMessage();
    if (st == UpdateState::Downloading && updater_->totalBytes() > 0) {
        const int pct = static_cast<int>((updater_->downloadedBytes() * 100) / updater_->totalBytes());
        msg = QStringLiteral("正在下载更新：%1%").arg(pct);
    }
    if (msg.isEmpty()) {
        msg = updater_->isConfigured()
            ? QStringLiteral("从 GitHub Releases 自动检查已签名的版本更新。")
            : QStringLiteral("当前开发构建未配置更新签名公钥。");
    }
    update_status_label_->setText(msg);

    if (st == UpdateState::Checking || st == UpdateState::Downloading ||
        st == UpdateState::Verifying || st == UpdateState::Installing) {
        check_update_btn_->setEnabled(false);
        check_update_btn_->setText(st == UpdateState::Downloading ? QStringLiteral("下载中…") : QStringLiteral("处理中…"));
    } else if (st == UpdateState::ReadyToInstall) {
        check_update_btn_->setEnabled(true);
        check_update_btn_->setText(QStringLiteral("立即重启更新"));
    } else if (st == UpdateState::Available) {
        check_update_btn_->setEnabled(true);
        check_update_btn_->setText(QStringLiteral("下载更新"));
    } else {
        check_update_btn_->setEnabled(true);
        check_update_btn_->setText(QStringLiteral("检查更新"));
    }
}

QScrollArea* SettingsDialog::buildStreamAudioPage() {
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
    bitrate_combo_->addItem(QStringLiteral("省流 · 2 Mbps"), 0);
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

QScrollArea* SettingsDialog::buildPluginsOverviewPage(const QVector<PluginSettingSpec>& plugins) {
    auto* container = new QWidget(pages_);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(12);

    layout->addWidget(makeSectionTitle(QStringLiteral("插件总览"), container));

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
            auto* title = makeSectionTitle(QStringLiteral("%1  (v%2)").arg(spec.name, spec.version), card);
            top->addWidget(title);
            top->addStretch(1);
            if (!spec.fields.isEmpty()) {
                auto* jump = new QPushButton(QStringLiteral("前往配置 →"), card);
                connect(jump, &QPushButton::clicked, this, [this, id = spec.id] { showPluginConfig(id); });
                top->addWidget(jump);
            }
            card_layout->addLayout(top);

            const auto meta = QStringLiteral("%1  ·  %2  ·  角色 %3  ·  权限 %4")
                .arg(spec.id, spec.enabled ? QStringLiteral("已启用") : QStringLiteral("已停用"),
                    spec.roles.join(QStringLiteral("/")), spec.permissions.join(QStringLiteral(", ")));
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

void SettingsDialog::addPluginCard(const PluginSettingSpec& spec, int nav_row) {
    auto* page = static_cast<QScrollArea*>(pages_->widget(nav_to_page_.value(nav_row)));
    auto* container = page->widget();
    auto* layout = static_cast<QVBoxLayout*>(container->layout());
    QVBoxLayout* card_layout{};
    auto* card = makeCard(container, &card_layout);
    card_layout->addWidget(makeSectionTitle(spec.section, card));
    if (!spec.description.isEmpty()) card_layout->addWidget(makeMuted(spec.description, card));

    PluginCardBinding binding{spec.id, card, nav_row, spec.host_config, {}};
    const auto saved = AppSettings::loadPluginConfig(spec.id, spec.host_config);

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
            lbl->setTextFormat(Qt::PlainText);
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
        PluginFieldBinding field_binding{field, control, {}};
        field_binding.initial = fieldValue(field_binding);
        binding.fields.append(std::move(field_binding));
    }

    plugin_cards_.append(std::move(binding));
    layout->insertWidget(layout->count() - 1, card);
}

void SettingsDialog::showPluginConfig(const QString& id) {
    const auto binding = std::ranges::find(plugin_cards_, id, &PluginCardBinding::id);
    if (binding == plugin_cards_.end()) return;
    nav_->setCurrentRow(binding->nav_row);
    auto* page = static_cast<QScrollArea*>(pages_->currentWidget());
    auto* card = binding->card;
    page->ensureWidgetVisible(card->layout()->itemAt(0)->widget());
    const auto highlight = [card](bool selected) {
        card->setProperty("settingFocus", selected);
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    };
    highlight(true);
    QTimer::singleShot(1400, card, [highlight] { highlight(false); });
}

void SettingsDialog::loadAppControls(const AppSettingsData& d) {
    loading_ = true;

    auto setComboData = [](QComboBox* combo, const QVariant& value) {
        const QSignalBlocker b(combo);
        const int idx = combo->findData(value);
        if (idx >= 0) combo->setCurrentIndex(idx);
    };

    setComboData(theme_combo_, d.theme);
    setComboData(accent_combo_, d.accent);
    setComboData(font_combo_, d.font_size);

    close_to_tray_->setChecked(d.close_to_tray);
    if (auto_receive_) auto_receive_->setChecked(d.auto_receive_on_login);
    default_server_->setText(d.default_server);
    if (auto_check_updates_) auto_check_updates_->setChecked(d.auto_check_updates);
    if (auto_download_updates_) auto_download_updates_->setChecked(d.auto_download_updates);
    if (update_mirror_url_) update_mirror_url_->setText(d.update_mirror_url);

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
    if (auto_receive_) d.auto_receive_on_login = auto_receive_->isChecked();
    d.default_server = default_server_->text().trimmed();
    if (auto_check_updates_) d.auto_check_updates = auto_check_updates_->isChecked();
    if (auto_download_updates_) d.auto_download_updates = auto_download_updates_->isChecked();
    if (update_mirror_url_) d.update_mirror_url = update_mirror_url_->text().trimmed();
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

QJsonValue SettingsDialog::fieldValue(const PluginFieldBinding& fb) {
    if (fb.field.type == QStringLiteral("bool")) return static_cast<QCheckBox*>(fb.widget)->isChecked();
    if (fb.field.type == QStringLiteral("int")) return static_cast<QSpinBox*>(fb.widget)->value();
    if (fb.field.type == QStringLiteral("number")) return static_cast<QDoubleSpinBox*>(fb.widget)->value();
    if (fb.field.type == QStringLiteral("select")) {
        return static_cast<QComboBox*>(fb.widget)->currentData().toString();
    }
    return static_cast<QLineEdit*>(fb.widget)->text();
}

bool SettingsDialog::saveAll() {
    for (auto& card : plugin_cards_) {
        QJsonObject updates;
        for (const auto& field : card.fields) {
            const auto value = fieldValue(field);
            if (plugin_reset_ || value != field.initial) updates.insert(field.field.key, value);
        }
        if (updates.isEmpty()) continue;
        if (!AppSettings::savePluginConfig(card.id, updates, card.host)) {
            save_hint_->setText(QStringLiteral("无法保存 %1 的设置。").arg(card.id));
            showPluginConfig(card.id);
            return false;
        }
        for (auto& field : card.fields) field.initial = fieldValue(field);
    }
    const auto next = collectAppSettings();
    AppSettings::instance().save(next);
    AppTheme::apply(next);
    return true;
}

void SettingsDialog::resetAll() {
    AppSettingsData defaults;
    defaults.default_server = AppSettings::instance().data().default_server;
    loadAppControls(defaults);
    previewAppearance();
    plugin_reset_ = true;
    save_hint_->setText(QStringLiteral("已恢复默认值，保存后生效。"));

    for (const auto& card : plugin_cards_) {
        for (const auto& fb : card.fields) {
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
