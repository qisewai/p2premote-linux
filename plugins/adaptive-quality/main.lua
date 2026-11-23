local enabled = p2p.config.get("enabled", false) == true
local active, bad, good = "automatic", 0, 0

local function choose(preset)
    if preset ~= active and p2p.video.set_bitrate(preset) then active = preset end
end

local function sample()
    if not enabled then return end
    local stats = p2p.session.stats()
    if stats.route == "unknown" or not stats.rtt_ms then return end
    if active == "detail" and stats.route ~= "lan" then choose("automatic") end
    if (stats.loss and stats.loss >= 0.03) or stats.rtt_ms >= 120 then
        bad, good = bad + 1, 0
        if bad >= 3 then choose("economy") end
    elseif stats.loss and stats.loss <= 0.005 and stats.rtt_ms <= 80 then
        bad, good = 0, good + 1
        if good >= 8 then
            choose(stats.route == "lan" and stats.rtt_ms < 15 and "detail" or "automatic")
        end
    else
        bad, good = 0, 0
    end
end

local menu
menu = p2p.ui.add_menu("智能画质（自动档）", function()
    enabled = not enabled
    bad, good = 0, 0
    local saved = p2p.config.set("enabled", enabled)
    p2p.ui.set_checked(menu, enabled)
    if not enabled then choose("automatic") end
    if not saved then p2p.ui.notify("智能画质设置仅在本次连接生效")
    else p2p.ui.notify(enabled and "已启用智能画质，仅在自动档生效" or "已关闭智能画质") end
end, enabled)

p2p.timer.every(1000, sample)
