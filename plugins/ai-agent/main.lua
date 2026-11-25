local channel, timer = p2p.channel, p2p.timer
local HEADER = "<I4"
local HEADER_SIZE = string.packsize(HEADER)
local MAX_SNAPSHOT = 4096

if p2p.session.role() == "host" then
    local last_request, approval, active_request, poll_timer
    channel.on_message(function(kind, request)
        if kind == "cancel_info" then
            if approval and request == active_request then p2p.sys.cancel_info(approval) end
            return
        end
        local screen = kind == "request_screen"
        if (not screen and kind ~= "request_info") or #request ~= HEADER_SIZE
            or string.unpack(HEADER, request) == 0 then return end
        local now = timer.now_ms()
        if approval or (last_request and now - last_request < 500) then
            channel.send("busy", request)
            return
        end
        last_request = now
        local reason
        approval, reason = p2p.sys.begin_info(screen)
        if not approval then
            channel.send(reason, request)
            return
        end
        active_request = request
        poll_timer = timer.every(125, function()
            local info, status = p2p.sys.poll_info(approval, screen)
            if status == "pending" then return end
            timer.cancel(poll_timer)
            poll_timer, approval, active_request = nil, nil, nil
            if info and screen then
                local index, chunk = 0, 16 * 1024
                for first = 1, #info, chunk do
                    local last = first + chunk - 1
                    if not channel.send(last >= #info and "image_last" or "image_chunk",
                        request .. string.pack(HEADER, index) .. info:sub(first, last)) then
                        channel.send("unavailable", request)
                        break
                    end
                    index = index + 1
                end
            elseif info then
                channel.send("snapshot", request .. p2p.json.encode(info))
            else
                channel.send(status, request)
            end
        end)
    end)
    return
end

local ui = p2p.ui
local counter, pending, timeout, dialog = 0
local replies = {
    denied = "被控端拒绝了诊断请求，或尚未开启 AI Agent 诊断",
    unavailable = "被控端暂时无法提供系统摘要",
    revoked = "被控端已暂停 AI Agent 诊断",
    expired = "诊断请求未在时限内获批",
    busy = "被控端已有一条诊断请求待确认"
}

local function valid_name(value)
    if type(value) ~= "string" or #value == 0 or #value > 768 then return false end
    for i = 1, #value do
        local byte = value:byte(i)
        if byte < 32 or byte == 127 then return false end
    end
    return true
end

local function snapshot(data)
    if #data > MAX_SNAPSHOT then return nil end
    local info = p2p.json.decode(data)
    if type(info) ~= "table" or info.schema ~= 1 then return nil end
    local arch = info.architecture
    if arch ~= "x64" and arch ~= "arm64" and arch ~= "x86" and arch ~= "unknown" then return nil end
    local result = {schema = 1, architecture = arch}
    for _, key in ipairs({
        "windows_major", "windows_minor", "windows_build", "logical_processors",
        "memory_total_mib", "memory_available_mib", "uptime_seconds"
    }) do
        local value = math.tointeger(info[key])
        if not value or value < 0 or value > 9007199254740991 then return nil end
        result[key] = value
    end
    if result.windows_major == 0 or result.logical_processors == 0
        or result.memory_available_mib > result.memory_total_mib then return nil end
    local total, free = math.tointeger(info.system_disk_total_mib), math.tointeger(info.system_disk_free_mib)
    if total and free and free >= 0 and free <= total and total <= 9007199254740991 then
        result.system_disk_total_mib, result.system_disk_free_mib = total, free
    end
    for _, key in ipairs({"host_name", "host_user"}) do
        local value = info[key]
        if valid_name(value) then result[key] = value end
    end
    if type(info.host_elevated) == "boolean" then result.host_elevated = info.host_elevated end
    return result
end

local function show_snapshot(info)
    if dialog then ui.close_dialog(dialog) end
    local children = {
        {type = "label", text = string.format("Windows %d.%d（版本 %d） · %s",
            info.windows_major, info.windows_minor, info.windows_build, info.architecture)},
        {type = "label", text = string.format("逻辑处理器：%d", info.logical_processors)},
        {type = "label", text = string.format("可用内存：%d MiB / %d MiB",
            info.memory_available_mib, info.memory_total_mib)},
        {type = "label", text = string.format("开机时长：%d 秒", info.uptime_seconds)}
    }
    if info.system_disk_total_mib then
        children[#children + 1] = {type = "label", text = string.format("系统盘剩余：%d MiB / %d MiB",
            info.system_disk_free_mib, info.system_disk_total_mib)}
    end
    if info.host_name then
        children[#children + 1] = {type = "label", text = "主机名：" .. info.host_name}
    end
    if info.host_user then
        children[#children + 1] = {type = "label", text = "被控程序运行账户：" .. info.host_user}
    end
    if info.host_elevated ~= nil then
        children[#children + 1] = {type = "label", text = info.host_elevated and "被控程序已提升权限" or "被控程序未提升权限"}
    end
    children[#children + 1] = {type = "save_json", text = "导出诊断快照…", value = info}
    dialog = ui.open_dialog({
        title = "AI Agent · 系统摘要",
        min_width = 410, width = 510, min_height = 235, height = 100 + #children * 28,
        body = {type = "vbox", spacing = 8, children = children}
    }, function(event)
        if event.event == "close" then dialog = nil end
    end)
end

channel.on_message(function(kind, payload)
    if not pending or #payload < HEADER_SIZE or payload:sub(1, HEADER_SIZE) ~= pending then return end
    if kind ~= "snapshot" and not replies[kind] then return end
    if kind ~= "snapshot" and #payload ~= HEADER_SIZE then return end
    timer.cancel(timeout)
    pending, timeout = nil, nil
    if replies[kind] then
        ui.notify(replies[kind])
    else
        local info = snapshot(payload:sub(HEADER_SIZE + 1))
        if info then show_snapshot(info) else ui.notify("远端系统摘要格式无效") end
    end
end)

ui.add_menu({menu = "tools", label = "AI Agent 系统摘要…", order = 30, on_click = function()
    if pending then return end
    counter = counter % 0x7fffffff + 1
    local request = string.pack(HEADER, counter)
    if not channel.send("request_info", request) then
        ui.notify("当前会话暂时无法发送诊断请求")
        return
    end
    ui.notify("诊断请求已发送，等待被控端确认")
    pending = request
    timeout = timer.after(20000, function()
        pending, timeout = nil, nil
        ui.notify("等待诊断响应超时")
    end)
    if not timeout then pending = nil end
end})
