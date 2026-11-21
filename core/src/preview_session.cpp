#include <p2premote/preview_session.h>

#include "d3d11_preview.h"
#include "preview_stats.h"
#include "qpc.h"

#include <stdexcept>

namespace p2premote {

struct PreviewSession::Impl {
    std::unique_ptr<D3d11Preview> renderer;
    std::unique_ptr<IVideoCapturer> capture;
    PreviewStats stats;
    bool drawable{true};
    bool redraw{};
};

PreviewSession::PreviewSession() : impl_(std::make_unique<Impl>()) {}
PreviewSession::~PreviewSession() = default;

void PreviewSession::Start(HMONITOR monitor, HWND preview_window) {
    if (IsRunning()) {
        throw std::logic_error("Stop the current preview before starting another.");
    }
    auto renderer = std::make_unique<D3d11Preview>(preview_window);
    auto capture = StartMonitorCapture(monitor, renderer->Device());
    impl_->stats.Start();
    impl_->redraw = false;
    impl_->renderer = std::move(renderer);
    impl_->capture = std::move(capture);
}

void PreviewSession::Stop() noexcept {
    if (impl_->capture) {
        if (impl_->capture->HasFrame()) {
            impl_->stats.Dropped();
        }
        impl_->stats.Stop();
        impl_->capture.reset();
        impl_->renderer.reset();
    }
}

bool PreviewSession::IsRunning() const noexcept {
    return impl_->capture != nullptr;
}

HANDLE PreviewSession::CaptureEvent() const noexcept {
    return impl_->capture ? impl_->capture->ReadyEvent() : nullptr;
}

HANDLE PreviewSession::PresentEvent() const noexcept {
    if (!impl_->capture || !impl_->drawable) {
        return nullptr;
    }
    if (impl_->capture->HasFrame() || (impl_->redraw && impl_->renderer->HasImage())) {
        return impl_->renderer->PresentEvent();
    }
    return nullptr;
}

bool PreviewSession::ProcessCapture() {
    if (impl_->capture->IsSourceClosed()) {
        return false;
    }
    const auto progress = impl_->capture->Poll();
    impl_->stats.Captured(progress.acquired, progress.dropped, progress.width, progress.height,
        impl_->capture->HasFrame());
    return true;
}

void PreviewSession::Present() {
    auto frame = impl_->capture->TakeFrame();
    PresentedFrameTiming timing;
    if (frame) {
        timing.metadata = frame->metadata;
        timing.ready_qpc = frame->ready_qpc;
        timing.queue_depth = 1;
        impl_->stats.Dequeued();
    }
    timing.draw_begin_qpc = QpcNow();
    try {
        impl_->renderer->ResizeToWindow();
        if (frame) {
            impl_->renderer->Upload(*frame);
            frame.reset();
        }
        impl_->renderer->Draw();
        timing.submit_qpc = QpcNow();
        const auto displayed = impl_->renderer->Present();
        timing.present_return_qpc = QpcNow();
        impl_->redraw = false;
        if (timing.metadata.frame_id) {
            if (displayed) {
                impl_->stats.Presented(timing);
            } else {
                impl_->stats.Dropped();
            }
        }
    } catch (...) {
        if (timing.metadata.frame_id) {
            impl_->stats.Dropped();
        }
        throw;
    }
}

void PreviewSession::OnPreviewResize(bool drawable) noexcept {
    impl_->drawable = drawable;
    impl_->redraw = true;
}

PreviewStatistics PreviewSession::Statistics() const {
    return impl_->stats.Snapshot();
}

void PreviewSession::ExportCsv(const std::filesystem::path& path) const {
    if (IsRunning()) {
        throw std::logic_error("Stop the preview before exporting timings.");
    }
    impl_->stats.ExportCsv(path);
}

} // namespace p2premote
