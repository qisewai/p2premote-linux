p2p.session.on("connected", function()
    local started = p2p.timer.now_ms()
    local function update()
        local elapsed = (p2p.timer.now_ms() - started) // 100
        local text = string.format("%02d:%02d:%02d.%d",
            elapsed // 36000, elapsed // 600 % 60, elapsed // 10 % 60, elapsed % 10)
        p2p.overlay.set("clock", { text = text, monospace = true, font_size = 12 })
    end
    update()
    p2p.timer.every(100, update)
end)
