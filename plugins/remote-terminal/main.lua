local channel, timer = p2p.channel, p2p.timer
local CHUNK = 8192
local BASE64_CHUNK = 4 * ((CHUNK + 2) // 3)
local TICK_MS, RETRY_MS = 20, 500
local HEADER = "<I4I4"
local HEADER_BYTES = string.packsize(HEADER)
local VERSION = "2"

local function id(value)
    return math.type(value) == "integer" and value > 0 and value <= 0xffffffff
end

local function grid(value, fallback, low, high)
    return math.max(low, math.min(high, math.tointeger(value) or fallback))
end

local function decode(payload)
    local value = p2p.json.decode(payload)
    if type(value) == "table" then return value end
end

local function message(kind, value)
    return { kind = kind, payload = p2p.json.encode(value), due = 0 }
end

local function transmit(packet)
    local now = timer.now_ms()
    if now >= packet.due then
        local sent = channel.send(packet.kind, packet.payload)
        packet.due = now + (sent and RETRY_MS or TICK_MS)
    end
end

local function data_packet(kind, session, sequence, data)
    return { kind = kind, payload = string.pack(HEADER, session, sequence) .. data, due = 0 }
end

local function receive_data(kind, payload, session, expected, accept)
    if #payload <= HEADER_BYTES or #payload > HEADER_BYTES + CHUNK then return expected end
    local received_id, sequence, start = string.unpack(HEADER, payload)
    if received_id ~= session or sequence == 0 then return expected end
    if sequence == expected and accept(sequence, payload:sub(start)) then expected = expected + 1 end
    if sequence + 1 == expected then channel.send(kind, string.pack(HEADER, session, sequence)) end
    return expected
end

local function ack_matches(payload, session, sequence)
    if #payload ~= HEADER_BYTES then return false end
    local got_session, got_sequence = string.unpack(HEADER, payload)
    return got_session == session and got_sequence == sequence
end

if p2p.session.role() == "host" then
    local pty = p2p.pty
    local running, poll_timer
    local latest_request, reply = 0, nil

    local function close_active()
        if poll_timer then timer.cancel(poll_timer) end
        poll_timer = nil
        if running and running.pty then pty.close(running.id) end
        running = nil
    end

    local function poll()
        local term = running
        if term.pty and not term.output then
            local bytes, exit_code = pty.read(term.id)
            if bytes then
                term.output = data_packet("out", term.id, term.next_output, bytes)
            elseif exit_code then
                pty.close(term.id)
                term.pty = false
                term.exit = message("exit", { request = latest_request, id = term.id, code = exit_code })
            end
        end
        if term.output then transmit(term.output) end
        if term.exit then transmit(term.exit) end
    end

    channel.on_message(function(kind, payload)
        if kind == "hello" and payload == VERSION then
            channel.send("ready", VERSION)
        elseif kind == "open" then
            local req = decode(payload)
            if not req or req.version ~= VERSION or not id(req.request) or req.request < latest_request then return end
            if req.request > latest_request then
                close_active()
                latest_request = req.request
                local shell = req.shell == "cmd" and "cmd" or "powershell"
                local cols, rows = grid(req.cols, 100, 20, 400), grid(req.rows, 30, 5, 200)
                local session, err = pty.open(shell, cols, rows)
                if session then
                    running = { id = session, pty = true, next_input = 1, next_output = 1 }
                    reply = message("opened", { request = latest_request, id = session, shell = shell })
                    poll_timer = timer.every(TICK_MS, poll)
                else
                    reply = message("error", { request = latest_request, message = err or "无法启动远程终端" })
                end
            end
            if reply then channel.send(reply.kind, reply.payload) end
        elseif kind == "close" then
            local req = decode(payload)
            if req and id(req.request) then
                if req.request >= latest_request then
                    latest_request, reply = req.request, nil
                    close_active()
                end
                channel.send("closed", payload)
            end
        elseif running then
            local term = running
            if kind == "in" and term.pty then
                term.next_input = receive_data("in_ack", payload, term.id, term.next_input, function(_, bytes)
                    return pty.write(term.id, bytes)
                end)
            elseif kind == "out_ack" and term.output and ack_matches(payload, term.id, term.next_output) then
                term.output = nil
                term.next_output = term.next_output + 1
                poll()
            elseif kind == "resize" and term.pty then
                local req = decode(payload)
                if req and req.id == term.id and pty.resize(term.id, grid(req.cols, 100, 20, 400), grid(req.rows, 30, 5, 200)) then
                    channel.send("resized", payload)
                end
            elseif kind == "exit_ack" and term.exit and payload == term.exit.payload then
                close_active()
            end
        end
    end)
    p2p.session.on("disconnected", close_active)
    return
end

local dialog, session, ended_session, control, resize, input, poll_timer, pending_ui
local request = 0
local next_input, next_output, last_ui_ack = 1, 1, 0
local shell, cols, rows = "powershell", 100, 30
local ready = false

local function label()
    return shell == "cmd" and "CMD" or "PowerShell"
end

local function flush_ui()
    if pending_ui and p2p.ui.update_dialog(dialog, pending_ui) then pending_ui = nil end
end

local function update_ui(status, term)
    if not dialog then return end
    pending_ui = pending_ui or {}
    if status then pending_ui.status = { text = status } end
    if term then
        pending_ui.term = pending_ui.term or {}
        for key, value in pairs(term) do pending_ui.term[key] = value end
    end
    flush_ui()
end

local function poll()
    flush_ui()
    if control then
        if control.kind == "open" and timer.now_ms() >= control.deadline then
            update_ui(ready and "终端启动超时 · 点击「重启」重试" or "被控端未响应 · 请确认两端终端插件版本一致")
            control = ready and message("close", { request = request }) or nil
        elseif control.kind == "open" and not ready then
            transmit(control.hello)
        else
            transmit(control)
        end
    end
    if resize then transmit(resize) end
    if input then transmit(input.packet) end
    if not dialog and not control then
        timer.cancel(poll_timer)
        poll_timer = nil
    end
end

local function clear_session()
    session, ended_session, input, resize = nil, nil, nil, nil
    next_input, next_output, last_ui_ack = 1, 1, 0
end

local function request_open(selected)
    shell = selected
    request = request + 1
    clear_session()
    update_ui("正在启动 " .. label() .. "…", { input_session = 0, reset = true, focus = true })
    control = message("open", { version = VERSION, request = request, shell = shell, cols = cols, rows = rows })
    control.hello = { kind = "hello", payload = VERSION, due = 0 }
    control.deadline = timer.now_ms() + 10000
    if not poll_timer then poll_timer = timer.every(TICK_MS, poll) end
    poll()
end

local function request_resize()
    resize = message("resize", { id = session, cols = cols, rows = rows })
    transmit(resize)
end

local function on_dialog_event(ev)
    if ev.event == "close" and ev.control == "" then
        dialog, pending_ui = nil, nil
        clear_session()
        control = ready and message("close", { request = request }) or nil
        poll()
    elseif ev.control == "term" and ev.event == "data" then
        if not session or ev.session ~= session or not id(ev.seq) then return end
        if ev.seq == last_ui_ack then
            update_ui(nil, { input_ack = ev.seq })
        elseif not input and ev.seq > last_ui_ack and type(ev.data_b64) == "string" and #ev.data_b64 <= BASE64_CHUNK then
            local bytes = p2p.base64.decode(ev.data_b64)
            if bytes and #bytes > 0 and #bytes <= CHUNK then
                input = { ui_seq = ev.seq, packet = data_packet("in", session, next_input, bytes) }
                transmit(input.packet)
            end
        end
    elseif ev.control == "term" and ev.event == "resize" then
        cols, rows = grid(ev.cols, cols, 20, 400), grid(ev.rows, rows, 5, 200)
        if session then request_resize() end
    elseif ev.control == "shell" and ev.event == "change" then
        local requested = ev.value == "cmd" and "cmd" or "powershell"
        if requested ~= shell then request_open(requested) end
    elseif ev.control == "restart" and ev.event == "click" then
        request_open(shell)
    elseif ev.control == "ctrl_c" and ev.event == "click" then
        update_ui(nil, { interrupt = true, focus = true })
    elseif ev.control == "clear" and ev.event == "click" then
        update_ui(nil, { clear = true, focus = true })
    end
end

local function open_dialog()
    if dialog then
        update_ui(nil, { focus = true })
        return
    end
    dialog = p2p.ui.open_dialog({
        title = "远程终端 (Ghostty)", theme = "dark", width = 960, height = 600,
        min_width = 640, min_height = 380,
        body = {
            type = "vbox", spacing = 8, children = {
                {
                    type = "hbox", spacing = 8, children = {
                        { type = "label", text = "Shell" },
                        { type = "combo", id = "shell", selected = shell, items = {
                            { value = "powershell", label = "PowerShell" },
                            { value = "cmd", label = "命令提示符 (CMD)" }
                        } },
                        { type = "button", id = "restart", text = "重启" },
                        { type = "button", id = "ctrl_c", text = "Ctrl+C" },
                        { type = "button", id = "clear", text = "清屏" },
                        { type = "spacer" },
                        { type = "label", id = "status", text = "正在连接…", muted = true }
                    }
                },
                { type = "terminal", id = "term", stretch = 1 }
            }
        }
    }, on_dialog_event)
    if dialog then request_open(shell) end
end

channel.on_message(function(kind, payload)
    if kind == "ready" and payload == VERSION then
        ready = true
        if control and control.kind == "open" then transmit(control) end
    elseif kind == "out" and session then
        next_output = receive_data("out_ack", payload, session, next_output, function(_, bytes)
            return not pending_ui and p2p.ui.update_dialog(dialog, { term = { write_b64 = p2p.base64.encode(bytes) } })
        end)
    elseif kind == "in_ack" and input and ack_matches(payload, session, next_input) then
        last_ui_ack = input.ui_seq
        input = nil
        next_input = next_input + 1
        update_ui(nil, { input_ack = last_ui_ack })
    elseif kind == "resized" and resize and resize.payload == payload then
        resize = nil
        update_ui("已连接 · " .. label() .. " (" .. cols .. "×" .. rows .. ")")
    elseif kind == "opened" or kind == "error" or kind == "closed" or kind == "exit" then
        local response = decode(payload)
        if not response or response.request ~= request then return end
        if kind == "closed" and control and control.kind == "close" then
            control = nil
        elseif kind == "opened" and dialog and control and control.kind == "open" and id(response.id) then
            control = nil
            session = response.id
            update_ui("已连接 · " .. label(), { input_session = session, focus = true })
            request_resize()
        elseif kind == "error" and dialog and control and control.kind == "open" then
            control = nil
            update_ui("错误：" .. tostring(response.message or "启动失败"))
        elseif kind == "exit" and id(response.id) and (response.id == session or response.id == ended_session) then
            if response.id == session then
                clear_session()
                ended_session = response.id
                update_ui("进程已退出（" .. tostring(response.code) .. "）· 点击「重启」恢复", { input_session = 0 })
            end
            channel.send("exit_ack", payload)
        end
    end
end)

p2p.ui.add_menu({menu = "tools", label = "远程终端…", order = 20, on_click = open_dialog})
