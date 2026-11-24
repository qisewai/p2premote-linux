local previous_seconds, previous_frames, fps = 0, 0, "—"
local routes = { lan = "局域网", direct = "直连", relay = "中继" }

local function bandwidth(bps)
    if not bps then return "—" end
    if bps < 1000000 then return string.format("%.1f Kbps", bps / 1000) end
    return string.format("%.1f Mbps", bps / 1000000)
end

local function milliseconds(ms)
    return ms and string.format("%.1f ms", ms) or "—"
end

p2p.timer.every(250, function()
    local stats = p2p.session.stats()
    if not stats.width or stats.seconds <= previous_seconds then return end
    if stats.presented >= previous_frames then
        fps = string.format("%.0f", (stats.presented - previous_frames) / (stats.seconds - previous_seconds))
    end
    previous_frames, previous_seconds = stats.presented, stats.seconds

    local elapsed = math.floor(stats.seconds)
    local route = routes[stats.route] or "连接中"
    if stats.protocol ~= "" then route = string.upper(stats.protocol) .. " " .. route end
    local loss = stats.loss and string.format("%.1f%%", stats.loss * 100) or "—"
    local text = string.format(
        "%02d:%02d:%02d  ·  %s\r\n%d×%d  ·  %s fps\r\nRTT %s  ·  丢包 %s\r\n↓ %s  ↑ %s\r\n帧等待 %s",
        elapsed // 3600, elapsed // 60 % 60, elapsed % 60, route, stats.width, stats.height, fps,
        milliseconds(stats.rtt_ms), loss, bandwidth(stats.download_bps), bandwidth(stats.upload_bps),
        milliseconds(stats.frame_wait_ms))
    p2p.overlay.set("default", { text = text })
end)
