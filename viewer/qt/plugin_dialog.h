#pragma once

#include <p2premote/core_api.h>

#include <QObject>

#include <cstdint>
#include <memory>
#include <unordered_map>

class QWidget;
class PluginDialog;

class PluginDialogManager final : public QObject {
    Q_OBJECT

public:
    explicit PluginDialogManager(QWidget* parent_window, QObject* parent = nullptr);
    ~PluginDialogManager() override;

    void poll(p2p_stream* stream);
    void closeAll();

private:
    void applyCommand(const p2p_ui_command& command);
    void sendEvent(p2p_stream* stream, uint32_t dialog_id, const QString& control,
        const QString& event, const QByteArray& payload_json);

    QWidget* parent_window_{};
    p2p_stream* current_stream_{};
    std::unordered_map<uint32_t, PluginDialog*> dialogs_;
};
