#pragma once

#include <p2premote/core_api.h>

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QMenu;

class PluginMenuLayout {
public:
    using Invoke = std::function<void(uint32_t)>;

    struct Button {
        QString menu;
        QString label;
        uint32_t command{};
        int checked{-1};

        bool operator==(const Button&) const = default;
    };

    void read(p2p_stream* stream);
    bool hasItems(const QString& menu) const;
    std::vector<Button> toolbar() const;
    void append(QMenu& target, const QString& menu, const Invoke& invoke, const QString& anchor = {}) const;

private:
    struct Item {
        QString plugin, menu, title, label, section;
        QStringList path;
        uint32_t id{};
        int checked{-1};
        int menu_order{}, order{};
        bool direct{};
    };

    static void addCommand(QMenu& target, const Item& item, const Invoke& invoke);
    static void appendItems(QMenu& target, const std::vector<const Item*>& items,
        qsizetype depth, const Invoke& invoke, bool separate);

    std::vector<Item> items_;
    std::vector<Button> extra_buttons_;
};
