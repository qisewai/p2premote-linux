#include "plugin_menu.h"

#include <QMenu>
#include <QSet>

#include <algorithm>
#include <tuple>

namespace {

constexpr size_t toolbar_limit = 2;

QString menuText(QString label) {
    return label.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

void separateFromPreviousSection(QMenu& menu) {
    const auto actions = menu.actions();
    if (!actions.isEmpty() && !actions.back()->isSeparator()) menu.addSeparator();
}

} // namespace

void PluginMenuLayout::read(p2p_stream* stream) {
    items_.clear();
    extra_buttons_.clear();
    if (!stream) return;

    const auto count = p2p_stream_read_menu(stream);
    items_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        p2p_menu_item entry{};
        if (p2p_stream_menu_item(stream, i, &entry) != P2P_OK) continue;
        items_.push_back({QString::fromUtf8(entry.plugin_id), QString::fromUtf8(entry.menu),
            QString::fromWCharArray(entry.menu_title), QString::fromWCharArray(entry.label),
            QString::fromUtf8(entry.section),
            QString::fromWCharArray(entry.path).split(QChar(0x1f), Qt::SkipEmptyParts),
            entry.id, entry.checked, entry.menu_order, entry.order, entry.toolbar_button != 0});
    }
    std::ranges::sort(items_, [](const Item& a, const Item& b) {
        return std::tie(a.order, a.plugin, a.id) < std::tie(b.order, b.plugin, b.id);
    });

    std::vector<const Item*> candidates;
    for (const auto& item : items_) {
        if (item.direct || !item.title.isEmpty()) candidates.push_back(&item);
    }
    std::ranges::sort(candidates, [](const Item* a, const Item* b) {
        return std::tuple(a->direct ? a->order : a->menu_order, a->plugin, a->id) <
            std::tuple(b->direct ? b->order : b->menu_order, b->plugin, b->id);
    });
    QSet<QString> folders;
    for (const auto* item : candidates) {
        if (item->direct) {
            extra_buttons_.push_back({item->menu, menuText(item->label), item->id, item->checked});
        } else if (!folders.contains(item->menu)) {
            folders.insert(item->menu);
            extra_buttons_.push_back({item->menu, menuText(item->title)});
        }
    }
}

bool PluginMenuLayout::hasItems(const QString& menu) const {
    return std::ranges::any_of(items_, [&menu](const Item& item) { return item.menu == menu && !item.direct; });
}

std::vector<PluginMenuLayout::Button> PluginMenuLayout::toolbar() const {
    const auto visible = std::min(toolbar_limit, extra_buttons_.size());
    std::vector<Button> buttons(extra_buttons_.begin(), extra_buttons_.begin() + visible);
    if (visible < extra_buttons_.size()) buttons.push_back({QStringLiteral("more"), QStringLiteral("更多")});
    return buttons;
}

void PluginMenuLayout::append(QMenu& target, const QString& menu, const Invoke& invoke, const QString& anchor) const {
    if (menu == QStringLiteral("more")) {
        for (size_t i = toolbar_limit; i < extra_buttons_.size(); ++i) {
            const auto& button = extra_buttons_[i];
            if (button.command) {
                const auto item = std::ranges::find(items_, button.command, &Item::id);
                if (item != items_.end()) addCommand(target, *item, invoke);
            } else {
                append(*target.addMenu(button.label), button.menu, invoke);
            }
        }
        return;
    }

    std::vector<const Item*> items;
    for (const auto& item : items_) {
        if (item.menu != menu || item.direct) continue;
        const bool anchored = menu == QStringLiteral("display") &&
            (item.section == QStringLiteral("resolution") || item.section == QStringLiteral("bitrate") ||
             item.section == QStringLiteral("overlays"));
        if (anchor.isEmpty() ? !anchored : item.section == anchor) items.push_back(&item);
    }
    appendItems(target, items, 0, invoke, anchor.isEmpty());
}

void PluginMenuLayout::addCommand(QMenu& target, const Item& item, const Invoke& invoke) {
    auto* action = target.addAction(menuText(item.label));
    action->setCheckable(item.checked >= 0);
    action->setChecked(item.checked > 0);
    QObject::connect(action, &QAction::triggered, &target, [invoke, id = item.id] { invoke(id); });
}

void PluginMenuLayout::appendItems(QMenu& target, const std::vector<const Item*>& items,
    qsizetype depth, const Invoke& invoke, bool separate) {
    struct Row {
        QString section;
        QString submenu;
        std::vector<const Item*> entries;
    };
    std::vector<Row> rows;
    for (const auto* item : items) {
        if (depth == item->path.size()) {
            rows.push_back({item->section, {}, {item}});
            continue;
        }
        const auto& title = item->path[depth];
        const auto row = std::ranges::find(rows, title, &Row::submenu);
        if (row == rows.end()) rows.push_back({item->section, title, {item}});
        else row->entries.push_back(item);
    }

    QSet<QString> rendered;
    for (const auto& first : rows) {
        if (rendered.contains(first.section)) continue;
        rendered.insert(first.section);
        if (separate) separateFromPreviousSection(target);
        separate = true;
        for (const auto& row : rows) {
            if (row.section != first.section) continue;
            if (row.submenu.isEmpty()) addCommand(target, *row.entries.front(), invoke);
            else appendItems(*target.addMenu(menuText(row.submenu)), row.entries, depth + 1, invoke, false);
        }
    }
}
