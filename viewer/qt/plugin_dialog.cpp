#include "plugin_dialog.h"
#include "frameless_chrome.h"
#include "terminal_widget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTreeWidget>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

QIcon iconForKind(const QString& kind, QStyle* style) {
    if (!style) return {};
    if (kind == QStringLiteral("dir")) return style->standardIcon(QStyle::SP_DirIcon);
    if (kind == QStringLiteral("drive")) return style->standardIcon(QStyle::SP_DriveHDIcon);
    if (kind == QStringLiteral("file")) return style->standardIcon(QStyle::SP_FileIcon);
    return {};
}

QJsonArray selectedRowIds(const QTreeWidget* tree) {
    QJsonArray ids;
    for (const auto* item : tree->selectedItems()) {
        ids.append(item->data(0, Qt::UserRole).toString());
    }
    return ids;
}

void populateTableRows(QTreeWidget* tree, const QJsonValue& rows_value, QStyle* style) {
    const QSignalBlocker blocker(tree);
    tree->setUpdatesEnabled(false);
    tree->clear();
    if (rows_value.isArray()) {
        const auto rows = rows_value.toArray();
        QList<QTreeWidgetItem*> items;
        items.reserve(rows.size());
        for (const auto& entry : rows) {
            if (!entry.isObject()) continue;
            const auto obj = entry.toObject();
            auto* item = new QTreeWidgetItem;
            item->setData(0, Qt::UserRole, obj.value(QStringLiteral("id")).toString());
            const auto cells = obj.value(QStringLiteral("cells")).toArray();
            for (int col = 0; col < cells.size(); ++col) {
                item->setText(col, cells[col].toString());
            }
            const auto icon_kind = obj.value(QStringLiteral("icon")).toString();
            if (!icon_kind.isEmpty()) item->setIcon(0, iconForKind(icon_kind, style));
            items.append(item);
        }
        tree->addTopLevelItems(items);
    }
    tree->setUpdatesEnabled(true);
}

void populateComboItems(QComboBox* combo, const QJsonValue& items_value, const QJsonValue& selected_value) {
    const QSignalBlocker blocker(combo);
    combo->clear();
    if (items_value.isArray()) {
        for (const auto& entry : items_value.toArray()) {
            if (entry.isObject()) {
                const auto obj = entry.toObject();
                const auto value = obj.value(QStringLiteral("value")).toString();
                const auto label = obj.value(QStringLiteral("label")).toString(value);
                combo->addItem(label, value);
            } else if (entry.isString()) {
                const auto text = entry.toString();
                combo->addItem(text, text);
            }
        }
    }
    if (selected_value.isString()) {
        const auto target = selected_value.toString();
        const int idx = combo->findData(target);
        if (idx >= 0) combo->setCurrentIndex(idx);
    }
}

} // namespace

class PluginDialog final : public FramelessDialog {
public:
    using EventCallback = std::function<void(uint32_t, const QString&, const QString&, const QByteArray&)>;
    using ClosedCallback = std::function<void(uint32_t, bool)>;

    PluginDialog(uint32_t id, const QJsonObject& spec, EventCallback on_event,
        ClosedCallback on_closed, QWidget* parent)
        : FramelessDialog(spec.value(QStringLiteral("title")).toString(QStringLiteral("插件窗口")), true, parent),
          id_(id), on_event_(std::move(on_event)), on_closed_(std::move(on_closed)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName(QStringLiteral("pluginDialog"));

        const int min_w = std::clamp(spec.value(QStringLiteral("min_width")).toInt(520), 280, 1600);
        const int min_h = std::clamp(spec.value(QStringLiteral("min_height")).toInt(320), 180, 1000);
        setMinimumSize(min_w, min_h);
        const int width = std::clamp(spec.value(QStringLiteral("width")).toInt(820), min_w, 1600);
        const int height = std::clamp(spec.value(QStringLiteral("height")).toInt(560), min_h, 1000);
        resize(width, height);

        contentLayout()->setContentsMargins(14, 12, 14, 12);
        contentLayout()->setSpacing(10);
        const auto body = spec.value(QStringLiteral("body")).toObject();
        addNodeToLayout(body, contentLayout(), contentWidget());
    }

    void applyPatch(const QJsonObject& patch) {
        if (const auto title = patch.value(QStringLiteral("title")); title.isString()) {
            setDialogTitle(title.toString());
        }
        const auto controls = patch.value(QStringLiteral("controls")).isObject()
            ? patch.value(QStringLiteral("controls")).toObject()
            : patch;
        for (auto it = controls.begin(); it != controls.end(); ++it) {
            if (!it.value().isObject()) continue;
            const auto found = widgets_.find(it.key());
            if (found == widgets_.end()) continue;
            updateWidget(found->second, it.value().toObject());
        }
    }

    void closeProgrammatically() {
        programmatic_close_ = true;
        close();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (!closed_notified_) {
            closed_notified_ = true;
            if (on_closed_) on_closed_(id_, programmatic_close_);
        }
        FramelessDialog::closeEvent(event);
    }

private:
    enum class ControlKind { other, label, input, button, checkbox, combo, progress, table, terminal, spin, slider };

    struct ControlRef {
        ControlKind kind{ControlKind::other};
        QWidget* widget{};
    };

    QJsonObject snapshotValues() const {
        QJsonObject values;
        for (const auto& [id, ref] : widgets_) {
            switch (ref.kind) {
            case ControlKind::input:
                values.insert(id, static_cast<QLineEdit*>(ref.widget)->text());
                break;
            case ControlKind::checkbox:
                values.insert(id, static_cast<QCheckBox*>(ref.widget)->isChecked());
                break;
            case ControlKind::combo: {
                const auto* combo = static_cast<QComboBox*>(ref.widget);
                const auto item_value = combo->currentData().toString();
                values.insert(id, item_value.isEmpty() ? combo->currentText() : item_value);
                break;
            }
            case ControlKind::table:
                values.insert(id, selectedRowIds(static_cast<QTreeWidget*>(ref.widget)));
                break;
            case ControlKind::spin:
                values.insert(id, static_cast<QSpinBox*>(ref.widget)->value());
                break;
            case ControlKind::slider:
                values.insert(id, static_cast<QSlider*>(ref.widget)->value());
                break;
            default:
                break;
            }
        }
        return values;
    }

    void emitControlEvent(const QString& control_id, const QString& event_name, QJsonObject payload = {}) {
        if (!on_event_ || programmatic_close_) return;
        payload.insert(QStringLiteral("values"), snapshotValues());
        on_event_(id_, control_id, event_name, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    void registerControl(const QJsonObject& node, ControlKind kind, QWidget* widget) {
        const auto id = node.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) widgets_[id] = {kind, widget};
        if (node.contains(QStringLiteral("enabled"))) widget->setEnabled(node.value(QStringLiteral("enabled")).toBool(true));
        if (node.contains(QStringLiteral("visible"))) widget->setVisible(node.value(QStringLiteral("visible")).toBool(true));
        if (const auto tip = node.value(QStringLiteral("tooltip")).toString(); !tip.isEmpty()) widget->setToolTip(tip);
    }

    QWidget* buildWidgetNode(const QJsonObject& node, QWidget* parent) {
        const auto type = node.value(QStringLiteral("type")).toString();
        const auto id = node.value(QStringLiteral("id")).toString();

        if (type == QStringLiteral("vbox") || type == QStringLiteral("hbox")) {
            auto* container = new QWidget(parent);
            QBoxLayout* layout = type == QStringLiteral("hbox")
                ? static_cast<QBoxLayout*>(new QHBoxLayout(container))
                : static_cast<QBoxLayout*>(new QVBoxLayout(container));
            applyLayoutSettings(node, layout);
            if (const auto children = node.value(QStringLiteral("children")); children.isArray()) {
                for (const auto& child : children.toArray()) {
                    if (child.isObject()) addNodeToLayout(child.toObject(), layout, container);
                }
            }
            registerControl(node, ControlKind::other, container);
            return container;
        }

        if (type == QStringLiteral("split")) {
            const bool vertical = node.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical");
            auto* splitter = new QSplitter(vertical ? Qt::Vertical : Qt::Horizontal, parent);
            splitter->setChildrenCollapsible(false);
            if (const auto children = node.value(QStringLiteral("children")); children.isArray()) {
                for (const auto& child : children.toArray()) {
                    if (child.isObject()) {
                        if (auto* w = buildWidgetNode(child.toObject(), splitter)) splitter->addWidget(w);
                    }
                }
            }
            if (const auto sizes = node.value(QStringLiteral("sizes")); sizes.isArray()) {
                QList<int> list;
                for (const auto& s : sizes.toArray()) list.append(std::max(100, s.toInt(360)));
                if (!list.isEmpty()) splitter->setSizes(list);
            }
            registerControl(node, ControlKind::other, splitter);
            return splitter;
        }

        if (type == QStringLiteral("label")) {
            auto* label = new QLabel(node.value(QStringLiteral("text")).toString(), parent);
            label->setTextFormat(Qt::PlainText);
            if (node.value(QStringLiteral("muted")).toBool(false)) label->setObjectName(QStringLiteral("muted"));
            if (node.value(QStringLiteral("bold")).toBool(false)) {
                auto f = label->font();
                f.setBold(true);
                label->setFont(f);
            }
            if (node.value(QStringLiteral("wrap")).toBool(false)) label->setWordWrap(true);
            registerControl(node, ControlKind::label, label);
            return label;
        }

        if (type == QStringLiteral("input")) {
            auto* edit = new QLineEdit(node.value(QStringLiteral("text")).toString(), parent);
            edit->setPlaceholderText(node.value(QStringLiteral("placeholder")).toString());
            edit->setReadOnly(node.value(QStringLiteral("read_only")).toBool(false));
            registerControl(node, ControlKind::input, edit);
            if (!id.isEmpty()) {
                connect(edit, &QLineEdit::returnPressed, this, [this, id, edit] {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("text"), edit->text());
                    emitControlEvent(id, QStringLiteral("submit"), std::move(payload));
                });
            }
            return edit;
        }

        if (type == QStringLiteral("card")) {
            auto* card = new QFrame(parent);
            card->setObjectName(QStringLiteral("card"));
            auto* layout = new QVBoxLayout(card);
            layout->setContentsMargins(14, 12, 14, 12);
            layout->setSpacing(node.value(QStringLiteral("spacing")).toInt(8));
            if (const auto children = node.value(QStringLiteral("children")); children.isArray()) {
                for (const auto& child : children.toArray()) {
                    if (child.isObject()) addNodeToLayout(child.toObject(), layout, card);
                }
            }
            registerControl(node, ControlKind::other, card);
            return card;
        }

        if (type == QStringLiteral("divider")) {
            auto* line = new QFrame(parent);
            line->setObjectName(QStringLiteral("divider"));
            registerControl(node, ControlKind::other, line);
            return line;
        }

        if (type == QStringLiteral("spin")) {
            auto* spin = new QSpinBox(parent);
            spin->setRange(node.value(QStringLiteral("min")).toInt(0), node.value(QStringLiteral("max")).toInt(100));
            spin->setSingleStep(std::max(1, node.value(QStringLiteral("step")).toInt(1)));
            if (const auto suffix = node.value(QStringLiteral("suffix")).toString(); !suffix.isEmpty()) spin->setSuffix(suffix);
            spin->setValue(node.value(QStringLiteral("value")).toInt(0));
            registerControl(node, ControlKind::spin, spin);
            if (!id.isEmpty()) {
                connect(spin, &QSpinBox::valueChanged, this, [this, id](int value) {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("value"), value);
                    emitControlEvent(id, QStringLiteral("change"), std::move(payload));
                });
            }
            return spin;
        }

        if (type == QStringLiteral("slider")) {
            auto* slider = new QSlider(Qt::Horizontal, parent);
            slider->setRange(node.value(QStringLiteral("min")).toInt(0), node.value(QStringLiteral("max")).toInt(100));
            slider->setSingleStep(std::max(1, node.value(QStringLiteral("step")).toInt(1)));
            slider->setValue(node.value(QStringLiteral("value")).toInt(0));
            registerControl(node, ControlKind::slider, slider);
            if (!id.isEmpty()) {
                connect(slider, &QSlider::valueChanged, this, [this, id](int value) {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("value"), value);
                    emitControlEvent(id, QStringLiteral("change"), std::move(payload));
                });
            }
            return slider;
        }

        if (type == QStringLiteral("button")) {
            auto* button = new QPushButton(node.value(QStringLiteral("text")).toString(), parent);
            button->setAutoDefault(false);
            const auto variant = node.value(QStringLiteral("style")).toString();
            if (node.value(QStringLiteral("primary")).toBool(false) || variant == QStringLiteral("primary")) {
                button->setObjectName(QStringLiteral("primary"));
            } else if (!variant.isEmpty()) {
                button->setProperty("variant", variant);
            }
            registerControl(node, ControlKind::button, button);
            const auto prompt = node.value(QStringLiteral("prompt")).isObject()
                ? node.value(QStringLiteral("prompt")).toObject()
                : QJsonObject{};
            if (!id.isEmpty()) {
                connect(button, &QPushButton::clicked, this, [this, id, prompt] {
                    QJsonObject payload;
                    if (!prompt.isEmpty()) {
                        bool accepted = false;
                        const auto entered = QInputDialog::getText(this,
                            prompt.value(QStringLiteral("title")).toString(QStringLiteral("输入")),
                            prompt.value(QStringLiteral("label")).toString(QStringLiteral("请输入：")),
                            QLineEdit::Normal,
                            prompt.value(QStringLiteral("default")).toString(),
                            &accepted);
                        if (!accepted) return;
                        payload.insert(QStringLiteral("text"), entered);
                    }
                    emitControlEvent(id, QStringLiteral("click"), std::move(payload));
                });
            }
            return button;
        }

        if (type == QStringLiteral("checkbox")) {
            auto* box = new QCheckBox(node.value(QStringLiteral("text")).toString(), parent);
            box->setChecked(node.value(QStringLiteral("checked")).toBool(false));
            registerControl(node, ControlKind::checkbox, box);
            if (!id.isEmpty()) {
                connect(box, &QCheckBox::toggled, this, [this, id](bool checked) {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("checked"), checked);
                    emitControlEvent(id, QStringLiteral("change"), std::move(payload));
                });
            }
            return box;
        }

        if (type == QStringLiteral("combo")) {
            auto* combo = new QComboBox(parent);
            if (const int w = node.value(QStringLiteral("width")).toInt(0); w > 0) combo->setMinimumWidth(w);
            populateComboItems(combo, node.value(QStringLiteral("items")), node.value(QStringLiteral("selected")));
            registerControl(node, ControlKind::combo, combo);
            if (!id.isEmpty()) {
                connect(combo, &QComboBox::currentIndexChanged, this, [this, id, combo](int index) {
                    if (index < 0) return;
                    QJsonObject payload;
                    const auto item_value = combo->itemData(index).toString();
                    payload.insert(QStringLiteral("value"), item_value.isEmpty() ? combo->itemText(index) : item_value);
                    payload.insert(QStringLiteral("label"), combo->itemText(index));
                    payload.insert(QStringLiteral("index"), index);
                    emitControlEvent(id, QStringLiteral("change"), std::move(payload));
                });
            }
            return combo;
        }

        if (type == QStringLiteral("progress")) {
            auto* bar = new QProgressBar(parent);
            bar->setRange(0, 100);
            bar->setValue(std::clamp(node.value(QStringLiteral("value")).toInt(0), 0, 100));
            if (const auto fmt = node.value(QStringLiteral("text")); fmt.isString()) {
                bar->setFormat(fmt.toString());
                bar->setTextVisible(true);
            }
            registerControl(node, ControlKind::progress, bar);
            return bar;
        }

        if (type == QStringLiteral("table")) {
            auto* tree = new QTreeWidget(parent);
            tree->setRootIsDecorated(false);
            tree->setItemsExpandable(false);
            tree->setAllColumnsShowFocus(true);
            tree->setAlternatingRowColors(true);
            tree->setUniformRowHeights(true);
            tree->setSelectionBehavior(QAbstractItemView::SelectRows);
            tree->setSelectionMode(node.value(QStringLiteral("multi_select")).toBool(false)
                ? QAbstractItemView::ExtendedSelection
                : QAbstractItemView::SingleSelection);
            QStringList headers;
            if (const auto cols = node.value(QStringLiteral("columns")); cols.isArray()) {
                for (const auto& col : cols.toArray()) headers.append(col.toString());
            }
            if (headers.isEmpty()) headers.append(QStringLiteral("名称"));
            tree->setColumnCount(headers.size());
            tree->setHeaderLabels(headers);
            tree->header()->setStretchLastSection(true);
            if (const auto widths = node.value(QStringLiteral("column_widths")); widths.isArray()) {
                const auto arr = widths.toArray();
                for (int i = 0; i < arr.size() && i < headers.size(); ++i) {
                    if (const int w = arr[i].toInt(0); w > 0) tree->setColumnWidth(i, w);
                }
            }
            populateTableRows(tree, node.value(QStringLiteral("rows")), style());
            registerControl(node, ControlKind::table, tree);
            if (!id.isEmpty()) {
                connect(tree, &QTreeWidget::itemDoubleClicked, this, [this, id, tree](QTreeWidgetItem* item, int) {
                    if (!item) return;
                    QJsonObject payload;
                    payload.insert(QStringLiteral("row"), item->data(0, Qt::UserRole).toString());
                    payload.insert(QStringLiteral("rows"), selectedRowIds(tree));
                    emitControlEvent(id, QStringLiteral("double_click"), std::move(payload));
                });
                connect(tree, &QTreeWidget::itemSelectionChanged, this, [this, id, tree] {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("rows"), selectedRowIds(tree));
                    emitControlEvent(id, QStringLiteral("select"), std::move(payload));
                });
            }
            return tree;
        }

        if (type == QStringLiteral("terminal")) {
            auto* term = new GhosttyTerminalWidget(parent);
            registerControl(node, ControlKind::terminal, term);
            if (!id.isEmpty()) {
                connect(term, &GhosttyTerminalWidget::inputChunk, this,
                    [this, id](quint32 session, quint32 sequence, const QByteArray& bytes) {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("session"), static_cast<qint64>(session));
                    payload.insert(QStringLiteral("seq"), static_cast<qint64>(sequence));
                    payload.insert(QStringLiteral("data_b64"), QString::fromLatin1(bytes.toBase64()));
                    emitControlEvent(id, QStringLiteral("data"), std::move(payload));
                });
                connect(term, &GhosttyTerminalWidget::resized, this, [this, id](int cols, int rows) {
                    QJsonObject payload;
                    payload.insert(QStringLiteral("cols"), cols);
                    payload.insert(QStringLiteral("rows"), rows);
                    emitControlEvent(id, QStringLiteral("resize"), std::move(payload));
                });
            }
            return term;
        }

        return nullptr;
    }

    static void applyLayoutSettings(const QJsonObject& node, QBoxLayout* layout) {
        layout->setSpacing(std::clamp(node.value(QStringLiteral("spacing")).toInt(8), 0, 32));
        if (const auto margins = node.value(QStringLiteral("margins")); margins.isDouble()) {
            const int m = std::clamp(margins.toInt(0), 0, 32);
            layout->setContentsMargins(m, m, m, m);
        } else if (margins.isArray() && margins.toArray().size() == 4) {
            const auto a = margins.toArray();
            layout->setContentsMargins(a[0].toInt(0), a[1].toInt(0), a[2].toInt(0), a[3].toInt(0));
        } else {
            layout->setContentsMargins(0, 0, 0, 0);
        }
    }

    void addNodeToLayout(const QJsonObject& node, QBoxLayout* layout, QWidget* parent) {
        const auto type = node.value(QStringLiteral("type")).toString();
        const int stretch = std::max(0, node.value(QStringLiteral("stretch")).toInt(
            (type == QStringLiteral("table") || type == QStringLiteral("split") || type == QStringLiteral("terminal")) ? 1 : 0));
        if (type == QStringLiteral("spacer")) {
            layout->addStretch(stretch > 0 ? stretch : 1);
            return;
        }
        if ((type == QStringLiteral("vbox") || type == QStringLiteral("hbox")) &&
            node.value(QStringLiteral("id")).toString().isEmpty() &&
            !node.contains(QStringLiteral("visible")) && !node.contains(QStringLiteral("enabled"))) {
            QBoxLayout* nested = type == QStringLiteral("hbox")
                ? static_cast<QBoxLayout*>(new QHBoxLayout)
                : static_cast<QBoxLayout*>(new QVBoxLayout);
            applyLayoutSettings(node, nested);
            if (const auto children = node.value(QStringLiteral("children")); children.isArray()) {
                for (const auto& child : children.toArray()) {
                    if (child.isObject()) addNodeToLayout(child.toObject(), nested, parent);
                }
            }
            layout->addLayout(nested, stretch);
            return;
        }
        if (auto* widget = buildWidgetNode(node, parent)) {
            layout->addWidget(widget, stretch);
        }
    }

    void updateWidget(const ControlRef& ref, const QJsonObject& patch) {
        if (patch.contains(QStringLiteral("enabled"))) {
            ref.widget->setEnabled(patch.value(QStringLiteral("enabled")).toBool(true));
        }
        if (patch.contains(QStringLiteral("visible"))) {
            ref.widget->setVisible(patch.value(QStringLiteral("visible")).toBool(true));
        }
        switch (ref.kind) {
        case ControlKind::label:
            if (const auto text = patch.value(QStringLiteral("text")); text.isString()) {
                static_cast<QLabel*>(ref.widget)->setText(text.toString());
            }
            break;
        case ControlKind::input:
            if (const auto text = patch.value(QStringLiteral("text")); text.isString()) {
                auto* edit = static_cast<QLineEdit*>(ref.widget);
                if (edit->text() != text.toString()) {
                    const QSignalBlocker blocker(edit);
                    edit->setText(text.toString());
                }
            }
            break;
        case ControlKind::button:
            if (const auto text = patch.value(QStringLiteral("text")); text.isString()) {
                static_cast<QPushButton*>(ref.widget)->setText(text.toString());
            }
            break;
        case ControlKind::checkbox: {
            auto* box = static_cast<QCheckBox*>(ref.widget);
            const QSignalBlocker blocker(box);
            if (const auto text = patch.value(QStringLiteral("text")); text.isString()) box->setText(text.toString());
            if (patch.contains(QStringLiteral("checked"))) box->setChecked(patch.value(QStringLiteral("checked")).toBool(false));
            break;
        }
        case ControlKind::combo: {
            auto* combo = static_cast<QComboBox*>(ref.widget);
            if (patch.contains(QStringLiteral("items"))) {
                populateComboItems(combo, patch.value(QStringLiteral("items")), patch.value(QStringLiteral("selected")));
            } else if (const auto selected = patch.value(QStringLiteral("selected")); selected.isString()) {
                const QSignalBlocker blocker(combo);
                const int idx = combo->findData(selected.toString());
                if (idx >= 0) combo->setCurrentIndex(idx);
            }
            break;
        }
        case ControlKind::spin: {
            auto* spin = static_cast<QSpinBox*>(ref.widget);
            const QSignalBlocker blocker(spin);
            if (patch.contains(QStringLiteral("value"))) spin->setValue(patch.value(QStringLiteral("value")).toInt(spin->value()));
            break;
        }
        case ControlKind::slider: {
            auto* slider = static_cast<QSlider*>(ref.widget);
            const QSignalBlocker blocker(slider);
            if (patch.contains(QStringLiteral("value"))) slider->setValue(patch.value(QStringLiteral("value")).toInt(slider->value()));
            break;
        }
        case ControlKind::progress: {
            auto* bar = static_cast<QProgressBar*>(ref.widget);
            if (patch.contains(QStringLiteral("value"))) {
                bar->setValue(std::clamp(patch.value(QStringLiteral("value")).toInt(0), 0, 100));
            }
            if (const auto fmt = patch.value(QStringLiteral("text")); fmt.isString()) {
                bar->setFormat(fmt.toString());
                bar->setTextVisible(true);
            }
            break;
        }
        case ControlKind::table:
            if (patch.contains(QStringLiteral("rows"))) {
                populateTableRows(static_cast<QTreeWidget*>(ref.widget), patch.value(QStringLiteral("rows")), style());
            }
            break;
        case ControlKind::terminal: {
            auto* term = static_cast<GhosttyTerminalWidget*>(ref.widget);
            if (const auto session = patch.value(QStringLiteral("input_session")); session.isDouble()) {
                term->setInputSession(static_cast<quint32>(session.toInteger()));
            }
            if (patch.value(QStringLiteral("reset")).toBool()) term->resetTerminal();
            if (patch.value(QStringLiteral("clear")).toBool()) term->clearDisplay();
            if (const auto b64 = patch.value(QStringLiteral("write_b64")); b64.isString()) {
                const auto bytes = QByteArray::fromBase64(b64.toString().toLatin1());
                if (!bytes.isEmpty()) term->feed(bytes);
            }
            if (patch.value(QStringLiteral("focus")).toBool(false)) {
                term->setFocus();
            }
            if (const auto sequence = patch.value(QStringLiteral("input_ack")); sequence.isDouble()) {
                term->acknowledgeInput(static_cast<quint32>(sequence.toInteger()));
            }
            if (patch.value(QStringLiteral("interrupt")).toBool()) term->interrupt();
            break;
        }
        case ControlKind::other:
            break;
        }
    }

    uint32_t id_{};
    EventCallback on_event_;
    ClosedCallback on_closed_;
    std::unordered_map<QString, ControlRef> widgets_;
    bool programmatic_close_{};
    bool closed_notified_{};
};

PluginDialogManager::PluginDialogManager(QWidget* parent_window, QObject* parent)
    : QObject(parent), parent_window_(parent_window) {}

PluginDialogManager::~PluginDialogManager() {
    closeAll();
}

void PluginDialogManager::poll(p2p_stream* stream) {
    if (!stream) return;
    current_stream_ = stream;
    p2p_ui_command command{};
    while (p2p_stream_poll_ui_command(stream, &command) == P2P_OK) {
        applyCommand(command);
    }
}

void PluginDialogManager::closeAll() {
    current_stream_ = nullptr;
    auto copy = std::move(dialogs_);
    dialogs_.clear();
    for (auto& [id, dialog] : copy) {
        if (dialog) dialog->closeProgrammatically();
    }
}

void PluginDialogManager::applyCommand(const p2p_ui_command& command) {
    const auto kind = QString::fromUtf8(command.kind ? command.kind : "");
    if (kind == QStringLiteral("close")) {
        if (const auto it = dialogs_.find(command.dialog_id); it != dialogs_.end()) {
            auto* dialog = it->second;
            dialogs_.erase(it);
            if (dialog) dialog->closeProgrammatically();
        }
        return;
    }

    const auto doc = QJsonDocument::fromJson(QByteArray(command.json ? command.json : "{}"));
    if (!doc.isObject()) return;
    const auto obj = doc.object();

    if (kind == QStringLiteral("open")) {
        if (const auto it = dialogs_.find(command.dialog_id); it != dialogs_.end()) {
            auto* old = it->second;
            dialogs_.erase(it);
            if (old) old->closeProgrammatically();
        }
        auto* dialog = new PluginDialog(
            command.dialog_id,
            obj,
            [this](uint32_t dialog_id, const QString& control, const QString& event, const QByteArray& payload) {
                if (current_stream_) sendEvent(current_stream_, dialog_id, control, event, payload);
            },
            [this](uint32_t dialog_id, bool programmatic) {
                dialogs_.erase(dialog_id);
                if (!programmatic && current_stream_) {
                    sendEvent(current_stream_, dialog_id, QString{}, QStringLiteral("close"), QByteArrayLiteral("{}"));
                }
            },
            parent_window_);
        dialogs_[command.dialog_id] = dialog;
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    } else if (kind == QStringLiteral("update")) {
        if (const auto it = dialogs_.find(command.dialog_id); it != dialogs_.end() && it->second) {
            it->second->applyPatch(obj);
        }
    }
}

void PluginDialogManager::sendEvent(p2p_stream* stream, uint32_t dialog_id, const QString& control,
    const QString& event, const QByteArray& payload_json) {
    const auto ctrl_utf8 = control.toUtf8();
    const auto ev_utf8 = event.toUtf8();
    p2p_stream_send_ui_event(stream, dialog_id, ctrl_utf8.constData(), ev_utf8.constData(), payload_json.constData());
}
