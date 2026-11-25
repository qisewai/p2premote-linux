#include "stream_driver.h"

#include <QString>
#include <windows.h>

namespace {

void setWait(QWinEventNotifier& notifier, HANDLE source) {
    const auto old = notifier.handle();
    if ((!old && !source) || (old && source && CompareObjectHandles(old, source))) return;
    HANDLE copy{};
    if (source) DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, FALSE,
        DUPLICATE_SAME_ACCESS);
    notifier.setHandle(copy);
    if (old) CloseHandle(old);
    if (copy) notifier.setEnabled(true);
}

void runWhile(QTimer& timer, bool running) {
    if (running && !timer.isActive()) timer.start();
    else if (!running) timer.stop();
}

} // namespace

StreamDriver::StreamDriver(p2p_account* account, bool host, const wchar_t* device, void* video_parent,
    const LaunchOptions& options, QObject* parent, bool diagnostics) : QObject(parent) {
    const p2p_connection_options_v1 settings{
        options.ice_trace.c_str(), options.latency_trace.c_str(), options.stun.c_str(), options.stun_only, host, diagnostics};
    stream_.reset(p2p_stream_create(account, host, device, video_parent, &settings));
    if (!stream_) return;

    for (uint32_t kind = 0; kind < waits_.size(); ++kind) {
        connect(&waits_[kind], &QWinEventNotifier::activated, this, [this, kind] { dispatch(kind); });
    }
    tick_.setInterval(1000);
    cursor_.setInterval(33);
    heartbeat_.setInterval(150);
    connect(&tick_, &QTimer::timeout, this, [this] { update(p2p_stream_tick(handle())); });
    connect(&cursor_, &QTimer::timeout, this, [this] { p2p_stream_poll_cursor(handle()); });
    connect(&heartbeat_, &QTimer::timeout, this, [this] { p2p_stream_heartbeat(handle()); });
    tick_.start();
}

StreamDriver::~StreamDriver() {
    for (auto& notifier : waits_) setWait(notifier, nullptr);
}

void StreamDriver::update(int result) {
    if (result != P2P_OK) {
        const auto error = QString::fromWCharArray(p2p_last_error());
        const auto reason = error.toStdWString();
        p2p_stream_stop(handle(), reason.c_str());
        emit failed(error);
    }
    syncWaits();
    publish();
    emit uiCommandsReady();
}

void StreamDriver::resize(bool drawable) {
    if (handle()) {
        const auto result = p2p_stream_resize(handle(), drawable);
        if (result != P2P_OK) update(result);
        else syncWaits();
    }
}

void StreamDriver::dispatch(uint32_t kind) {
    const auto source = p2p_stream_wait_handle(handle(), kind);
    const auto watched = waits_[kind].handle();
    const bool matched = source && watched && CompareObjectHandles(source, watched);
    // QWinEventNotifier already consumed DXGI's frame-latency semaphore credit when waking on P2P_WAIT_PRESENT;
    // dispatch even if PresentReadyFrame() already presented so the semaphore credit is always returned.
    if (matched || (kind == P2P_WAIT_PRESENT && watched)) {
        const auto result = p2p_stream_dispatch(handle(), kind);
        if (result != P2P_OK) {
            update(result);
            return;
        }
        if (p2p_stream_state(handle()) != state_ || (p2p_stream_input_ready(handle()) != 0) != input_ready_) {
            publish();
        }
        if (kind == P2P_WAIT_SESSION) emit uiCommandsReady();
    }
    syncWaits();
}

void StreamDriver::syncWaits() {
    for (uint32_t kind = 0; kind < waits_.size(); ++kind) {
        setWait(waits_[kind], p2p_stream_wait_handle(handle(), kind));
    }
}

void StreamDriver::publish() {
    p2p_stream_snapshot_v1 snapshot{};
    if (p2p_stream_snapshot(handle(), &snapshot) != P2P_OK) {
        emit failed(QString::fromWCharArray(p2p_last_error()));
        return;
    }
    state_ = snapshot.state;
    input_ready_ = snapshot.input_ready != 0;
    runWhile(cursor_, input_ready_);
    runWhile(heartbeat_, input_ready_);
    emit changed(snapshot);
}
