local input, channel = p2p.input, p2p.channel

if p2p.session.role() == "host" then
    local last_caps, last_num
    channel.on_message(function(kind, value)
        if kind == "hello" and value == "" then
            last_caps, last_num = nil, nil
            channel.send("ready", "")
            return
        end
        if kind ~= "state" or #value ~= 3 then return end
        local caps, num, ime = value:sub(1, 1), value:sub(2, 2), value:sub(3, 3)
        if (caps ~= "0" and caps ~= "1") or (num ~= "0" and num ~= "1") or
            (ime ~= "0" and ime ~= "1" and ime ~= "-") then return end
        if caps ~= last_caps and input.set_lock("caps_lock", caps == "1") then last_caps = caps end
        if num ~= last_num and input.set_lock("num_lock", num == "1") then last_num = num end
        if ime ~= "-" then input.set_ime(ime == "1") end
    end)
    return
end

local enabled = p2p.config.get("enabled", false) == true
local ready, last = false, nil
local function sync()
    if not enabled then return end
    if not ready then
        channel.send("hello", "")
        return
    end
    local state = input.get_state()
    local caps = state.caps_lock and "1" or "0"
    local num = state.num_lock and "1" or "0"
    local ime = state.ime_open == nil and "-" or (state.ime_open and "1" or "0")
    local value = caps .. num .. ime
    if value ~= last and channel.send("state", value) then last = value end
end

local menu
menu = p2p.ui.add_menu("同步键盘状态到远端", function()
    enabled = not enabled
    ready, last = false, nil
    if not enabled then input.use_lock_sync(false) end
    local saved = p2p.config.set("enabled", enabled)
    p2p.ui.set_checked(menu, enabled)
    if not saved then p2p.ui.notify("键盘同步设置仅在本次连接生效")
    else p2p.ui.notify(enabled and "正在等待远端键盘插件" or "已关闭键盘状态同步") end
    sync()
end, enabled)

channel.on_message(function(kind, value)
    if enabled and not ready and kind == "ready" and value == "" then
        ready = true
        input.use_lock_sync(true)
        sync()
    end
end)

input.on_state_change(sync)
p2p.timer.every(750, sync)
