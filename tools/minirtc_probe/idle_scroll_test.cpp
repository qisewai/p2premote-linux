#include "d3d11_preview.h"
#include "minirtc_call.h"
#include "qpc.h"

#include <winrt/Windows.Data.Json.h>
#include <d3d11_4.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace p2premote;
using namespace std::chrono_literals;
using namespace winrt::Windows::Data::Json;

namespace {
using Clock = std::chrono::steady_clock;

struct PeerState {
    bool connected{};
    RtcCounters stats;
};

void Relay(StreamEvents& events, MiniRtcCall& target, const wchar_t* sender, PeerState& peer) {
    for (const auto& event : events.Drain()) {
        if (event.type == StreamEventType::connected) peer.connected = true;
        if (event.type == StreamEventType::stats) peer.stats = event.counters;
        if (event.type == StreamEventType::media_lost || event.type == StreamEventType::error)
            throw std::runtime_error("MiniRTC loopback connection failed");
        if (event.type != StreamEventType::minirtc_signal || !MiniRtcCall::IsSessionSignal(event.payload)) continue;
        auto message = JsonObject::Parse(winrt::to_hstring(event.payload));
        if (message.GetNamedString(L"type") == L"join_transmission") {
            target.ReceiveSignal(R"({"type":"user_join_transmission","status":"success","user_id":"viewer","transmission_id":"host"})");
        } else {
            message.SetNamedValue(L"remote_user_id", JsonValue::CreateStringValue(sender));
            target.ReceiveSignal(winrt::to_string(message.Stringify()));
        }
    }
}

constexpr unsigned width = 1920, height = 1080;

Nv12Frame Upload(ID3D11Device* device, const std::vector<std::uint8_t>& pixels) {
    D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT};
    D3D11_SUBRESOURCE_DATA data{pixels.data(), width, static_cast<UINT>(pixels.size())};
    Nv12Frame frame{};
    frame.width = width;
    frame.height = height;
    winrt::check_hresult(device->CreateTexture2D(&desc, &data, frame.texture.put()));
    return frame;
}

Nv12Frame MakeFrame(ID3D11Device* device, int marker) {
    std::vector<std::uint8_t> pixels(width * height * 3 / 2, 128);
    for (unsigned y = 0; y < height; ++y) {
        auto row = pixels.begin() + y * width;
        std::fill_n(row, width, static_cast<std::uint8_t>(((y + marker) / 12) % 2 ? 150 : 190));
        if (y < height / 4) std::fill_n(row, width / 4, static_cast<std::uint8_t>(marker));
    }
    return Upload(device, pixels);
}

std::vector<Nv12Frame> ScrollingPage(ID3D11Device* device) {
    // A seamless 512-row document with fine text and a shaded image. Prepare
    // textures before timing so generating the workload cannot stall the sender.
    std::vector<std::uint8_t> document(512 * width, 235);
    for (unsigned y = 0; y < 512; ++y) {
        for (unsigned x = 100; x < 1830; ++x) {
            auto& pixel = document[y * width + x];
            const unsigned glyph = (x / 9) * 7919 + (y / 18) * 104729;
            if (x < 1370 && x % 9 < 7 && y % 18 > 3 && y % 18 < 15 &&
                    ((glyph ^ (glyph >> 7)) >> ((x % 9 + y % 18) % 23) & 1)) pixel = 36;
            if (x >= 1400 && y > 40 && y < 345)
                pixel = static_cast<std::uint8_t>(125 + 48 * std::sin(x * .031 + y * .039) + 58 * std::cos(x * .017 - y * .025));
        }
    }
    std::vector<Nv12Frame> frames;
    std::vector<std::uint8_t> pixels(width * height * 3 / 2, 128);
    for (unsigned phase = 0; phase < 32; ++phase) {
        for (unsigned y = 0; y < height; ++y) {
            std::copy_n(document.data() + ((y + phase * 16) % 512) * width, width, pixels.data() + y * width);
        }
        frames.push_back(Upload(device, pixels));
    }
    return frames;
}

bool Matches(const DecodedFrame& frame, int expected) {
    const auto* y = frame.y_plane;
    int stride = frame.stride_y;
    std::vector<std::uint8_t> readback;
    if (!y && frame.copy_to_nv12) {
        readback.resize(static_cast<size_t>(frame.width) * frame.height * 3 / 2);
        if (frame.copy_to_nv12(frame.owner.get(), readback.data(), readback.size()) != 0)
            throw std::runtime_error("Cannot read decoded GPU frame");
        y = readback.data();
        stride = frame.width;
    }
    return y && std::abs(y[(frame.height / 8) * stride + frame.width / 8] - expected) < 20;
}

int CheckContinuous(ID3D11Device* device, MiniRtcCall& host, MiniRtcCall& viewer, const std::function<void()>& pump,
    const PeerState& host_state, const PeerState& viewer_state) {
    const auto frames = ScrollingPage(device);
    if (!host.SetVideoBitrate(8'000'000)) throw std::runtime_error("Cannot set test bitrate");
    winrt::handle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
    LARGE_INTEGER first{};
    first.QuadPart = -10'000;
    winrt::check_bool(SetWaitableTimer(timer.get(), &first, 1, nullptr, nullptr, FALSE));
    const auto start = QpcNow(), hz = QpcFrequency();
    const auto begin = start + hz, end = begin + 8 * hz;
    auto next = start, last_send = start, last_decode = begin;
    auto next_stats = begin;
    std::vector<std::uint32_t> sent_bps, received_bps;
    size_t sent = 0, decoded = 0, phase = 0;
    double worst_send = 0, worst_decode = 0;
    while (QpcNow() < end) {
        winrt::check_bool(WaitForSingleObject(timer.get(), 100) == WAIT_OBJECT_0);
        pump();
        const auto now = QpcNow();
        if (now >= next_stats) {
            if (host_state.stats.upload_bps.value_or(0) && viewer_state.stats.download_bps.value_or(0)) {
                sent_bps.push_back(*host_state.stats.upload_bps);
                received_bps.push_back(*viewer_state.stats.download_bps);
            }
            host.RequestStats();
            viewer.RequestStats();
            next_stats += hz;
        }
        if (now >= next) {
            host.Submit(frames[phase++ % frames.size()]);
            if (now >= begin) {
                ++sent;
                worst_send = std::max(worst_send, QpcMilliseconds(now - last_send));
            }
            last_send = now;
            next = std::max(next + hz / 60, now + hz / 120);
        }
        if (const auto frame = viewer.TakeFrame(); frame && frame->queued_qpc >= begin && frame->queued_qpc <= end) {
            const auto gap = QpcMilliseconds(frame->queued_qpc - last_decode);
            if (gap >= 100) {
                std::printf("slow_update gap_ms=%.2f queued_qpc=%lld\n", gap, static_cast<long long>(frame->queued_qpc));
            }
            worst_decode = std::max(worst_decode, gap);
            last_decode = frame->queued_qpc;
            ++decoded;
        }
    }
    worst_decode = std::max(worst_decode, QpcMilliseconds(end - last_decode));
    std::printf("scroll sent=%zu decoded=%zu max_submit_gap_ms=%.2f max_update_gap_ms=%.2f\n",
        sent, decoded, worst_send, worst_decode);
    if (worst_send >= 50 || sent < 450) throw std::runtime_error("Timing inconclusive: sender missed its cadence");
    if (sent_bps.size() < 4) throw std::runtime_error("Not enough paired RTP bitrate samples");
    std::ranges::sort(sent_bps);
    std::ranges::sort(received_bps);
    const auto tx = sent_bps[sent_bps.size() / 2];
    const auto rx = received_bps[received_bps.size() / 2];
    std::printf("RTP bitrate: host_tx_bps=%u viewer_rx_bps=%u\n", tx, rx);
    // Statistics windows start independently on the two peers.
    if (rx < tx * 0.75 || rx > tx * 1.25) throw std::runtime_error("Loopback RTP byte counts do not agree");
    return decoded >= 440 && worst_decode < 100 ? 0 : 1;
}

int CheckEncoderBacklog(ID3D11Device* device, MiniRtcCall& host, MiniRtcCall& viewer,
    const std::function<void()>& pump) {
    const auto still = MakeFrame(device, 32);
    const std::array updates{MakeFrame(device, 72), MakeFrame(device, 110), MakeFrame(device, 150), MakeFrame(device, 220)};
    const auto warmup = Clock::now() + 2s;
    while (true) {
        host.Submit(still);
        std::this_thread::sleep_for(25ms);
        pump();
        if (const auto frame = viewer.TakeFrame(); frame && Matches(*frame, 32)) break;
        if (Clock::now() >= warmup) throw std::runtime_error("Encoder backlog warmup failed");
    }

    // Stall GPU access briefly while the capture path submits more recent textures.
    winrt::com_ptr<ID3D11Multithread> multithread;
    winrt::check_hresult(device->QueryInterface(__uuidof(ID3D11Multithread), multithread.put_void()));
    {
        struct GpuHold {
            ID3D11Multithread& context;
            explicit GpuHold(ID3D11Multithread& context) : context(context) { context.Enter(); }
            ~GpuHold() { context.Leave(); }
        } hold{*multithread.get()};
        for (const auto& frame : updates) {
            std::this_thread::sleep_for(21ms);
            host.Submit(frame);
        }
    }

    const auto released = Clock::now();
    do {
        pump();
        if (const auto frame = viewer.TakeFrame(); frame && Matches(*frame, 220)) {
            std::printf("Newest drag frame after GPU stall: %.2f ms\n",
                std::chrono::duration<double, std::milli>(Clock::now() - released).count());
            return 0;
        }
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() - released < 500ms);
    throw std::runtime_error("Timed out waiting for the newest frame after GPU stall");
}

int Run(std::string_view mode) {
    winrt::com_ptr<ID3D11Device> device;
    try {
        device = CreateD3d11Device(true);
    } catch (const winrt::hresult_error& error) {
        if (error.code() != DXGI_ERROR_UNSUPPORTED) throw;
        std::puts("SKIP: no hardware D3D11 device");
        return 77;
    }
    const auto host_events = std::make_shared<StreamEvents>();
    const auto viewer_events = std::make_shared<StreamEvents>();
    ConnectionOptions options;
    options.ice_trace_dir = std::filesystem::temp_directory_path() / L"P2PRemote" / L"idle-scroll-test";
    MiniRtcCall host(true, host_events, options, false, 60, device.get());
    MiniRtcCall viewer(false, viewer_events, options, false, 60, device.get());
    PeerState host_state, viewer_state;
    const auto pump = [&] {
        Relay(*host_events, viewer, L"host", host_state);
        Relay(*viewer_events, host, L"viewer", viewer_state);
    };
    const auto deadline = Clock::now() + 12s;
    while (!host_state.connected || !viewer_state.connected) {
        pump();
        if (Clock::now() >= deadline) throw std::runtime_error("Timed out connecting local peers");
        std::this_thread::sleep_for(1ms);
    }

    if (mode == "--continuous") return CheckContinuous(device.get(), host, viewer, pump, host_state, viewer_state);
    if (mode == "--encoder-backlog") return CheckEncoderBacklog(device.get(), host, viewer, pump);
    const auto still = MakeFrame(device.get(), 32);
    const auto scroll = MakeFrame(device.get(), 220);
    int misses = 0, mistimed = 0;
    std::puts("trial,repeat_to_change_ms,submit_to_decode_ms,result");
    for (int trial = 0; trial < 10;) {
        // Confirm the neutral marker on the receiver before arming each measurement.
        viewer.TakeFrame();
        const auto reset_deadline = Clock::now() + 2s;
        do {
            host.Submit(still);
            std::this_thread::sleep_for(25ms);
            pump();
            const auto decoded = viewer.TakeFrame();
            if (decoded && Matches(*decoded, 32)) break;
            if (Clock::now() >= reset_deadline) throw std::runtime_error("Neutral marker did not arrive");
        } while (true);
        std::this_thread::sleep_for(1s);
        const auto repeat = QpcNow();
        host.Submit(still);
        const int offset_ms = trial % 2 == 0 ? 2 : 250;
        const auto due = repeat + QpcFrequency() * offset_ms / 1000;
        if (offset_ms > 2) std::this_thread::sleep_for(245ms);
        while (QpcNow() < due) YieldProcessor();
        const auto changed = QpcNow();
        const double gap = QpcMilliseconds(changed - repeat);
        if (offset_ms == 2 && gap >= 4.0) {
            if (++mistimed >= 3) throw std::runtime_error("Timing inconclusive: missed the 2 ms collision window three times");
            continue;
        }
        host.Submit(scroll); // Exactly one change. A later frame must never rescue this trial.
        const auto timeout = changed + QpcFrequency() / 2;
        double latency = -1;
        while (QpcNow() <= timeout) {
            pump();
            if (const auto decoded = viewer.TakeFrame(); decoded && Matches(*decoded, 220)) {
                latency = QpcMilliseconds(decoded->queued_qpc - changed);
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        if (latency < 0) ++misses;
        std::printf("%d,%.2f,%.2f,%s\n", ++trial, gap, latency, latency < 0 ? "MISS" : "PASS");
    }
    return misses ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (argc != 1 && mode != "--continuous" && mode != "--encoder-backlog") {
            throw std::runtime_error("Usage: idle_scroll_test [--continuous | --encoder-backlog]");
        }
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        return Run(mode);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Idle scroll test failed: %s\n", error.what());
        return 1;
    }
}
