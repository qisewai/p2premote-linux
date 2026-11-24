local role = p2p.session.role()
local CHUNK_SIZE = 57344
-- Every full window must include a cumulative ACK, including after backoff.
local ACK_EVERY = 4
local MIN_WINDOW = ACK_EVERY
local MAX_WINDOW = 32
local RETRY_MS = 10
local MAX_LIST_JSON = 56000

-- Automatic video-protection thresholds: keep video packet loss below 1.0%
-- while dynamically maximizing file transfer throughput.
local VIDEO_LOSS_THRESHOLD = 0.01
local VIDEO_LOSS_SEVERE = 0.03
local VIDEO_LOSS_CLEAR = 0.003
local RTT_INFLATE_HOLD_MS = 35
local RTT_INFLATE_CUT_MS = 60
local MIN_PACE_BPS = 256 * 1024
local UNLIMITED_PACE_BPS = 80 * 1024 * 1024

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- Token-bucket pacer: returns wait_ms (0 means allowed to send now).
local function pacer_take(state, bytes)
    local rate = state.pace_bps or 0
    if rate <= 0 then return 0 end
    local now = p2p.timer.now_ms()
    local elapsed = now - (state.last_token_ms or now)
    if elapsed < 0 then elapsed = 0 end
    state.last_token_ms = now
    local burst = math.max(CHUNK_SIZE * 2, math.floor(rate * 0.12))
    local tokens = math.min(burst, (state.tokens or burst) + (rate * elapsed) / 1000.0)
    if tokens >= bytes then
        state.tokens = tokens - bytes
        return 0
    end
    state.tokens = tokens
    local deficit = bytes - tokens
    return clamp(math.ceil((deficit * 1000.0) / rate), 5, 100)
end

local function trim(text)
    if type(text) ~= "string" then return "" end
    local i = 1
    local j = #text
    while i <= j do
        local b = string.byte(text, i)
        if b == 32 or b == 9 or b == 10 or b == 13 then i = i + 1 else break end
    end
    while j >= i do
        local b = string.byte(text, j)
        if b == 32 or b == 9 or b == 10 or b == 13 then j = j - 1 else break end
    end
    if i > j then return "" end
    return string.sub(text, i, j)
end

local function send_json(kind, tbl)
    local payload = p2p.json.encode(tbl)
    if not payload then return false end
    return p2p.channel.send(kind, payload)
end

local function format_size(bytes)
    if type(bytes) ~= "number" or bytes < 0 then return "" end
    if bytes < 1024 then return string.format("%d B", bytes) end
    if bytes < 1024 * 1024 then return string.format("%.1f KB", bytes / 1024) end
    if bytes < 1024 * 1024 * 1024 then return string.format("%.1f MB", bytes / (1024 * 1024)) end
    return string.format("%.2f GB", bytes / (1024 * 1024 * 1024))
end

local function format_speed(bytes_per_sec)
    if bytes_per_sec < 1024 then return string.format("%.0f B/s", bytes_per_sec) end
    if bytes_per_sec < 1024 * 1024 then return string.format("%.1f KB/s", bytes_per_sec / 1024) end
    return string.format("%.2f MB/s", bytes_per_sec / (1024 * 1024))
end

local function build_quick_items(home, roots)
    local items = {}
    if type(home) == "table" then
        if home.desktop and home.desktop ~= "" then
            table.insert(items, { value = home.desktop, label = "桌面" })
        end
        if home.downloads and home.downloads ~= "" then
            table.insert(items, { value = home.downloads, label = "下载" })
        end
        if home.home and home.home ~= "" then
            table.insert(items, { value = home.home, label = "主目录" })
        end
    end
    if type(roots) == "table" then
        for _, r in ipairs(roots) do
            if type(r) == "table" and r.path and r.path ~= "" then
                table.insert(items, { value = r.path, label = "磁盘 " .. (r.name or r.path) })
            end
        end
    end
    return items
end

-- ============================================================================
-- Host Role: serves filesystem browsing & chunked upload/download over channel
-- ============================================================================
if role == "host" then
    local uploads = {}   -- id -> { fh, expected_size, received, next_seq }
    local downloads = {} -- id -> { fh, total, sent_bytes, next_seq, acked_seq, eof, pending_seq, pending_chunk, retry_timer }

    local function close_upload(id)
        local u = uploads[id]
        if u then
            if u.fh then p2p.fs.close(u.fh) end
            uploads[id] = nil
        end
    end

    local function close_download(id)
        local d = downloads[id]
        if d then
            if d.retry_timer then p2p.timer.cancel(d.retry_timer) end
            if d.fh then p2p.fs.close(d.fh) end
            downloads[id] = nil
        end
    end

    local function cleanup_all()
        for id in pairs(uploads) do close_upload(id) end
        for id in pairs(downloads) do close_download(id) end
    end

    local function pump_download(id)
        local d = downloads[id]
        if not d or not p2p.session.connected() then return end
        if d.retry_timer then
            p2p.timer.cancel(d.retry_timer)
            d.retry_timer = nil
        end

        local win = clamp(d.win or MAX_WINDOW, MIN_WINDOW, MAX_WINDOW)

        if d.pending_chunk then
            local wait_ms = pacer_take(d, #d.pending_chunk)
            if wait_ms > 0 then
                d.retry_timer = p2p.timer.after(wait_ms, function() pump_download(id) end)
                return
            end
            local frame = string.pack("<I4I4", id, d.pending_seq) .. d.pending_chunk
            if not p2p.channel.send("dn_data", frame) then
                d.retry_timer = p2p.timer.after(RETRY_MS, function() pump_download(id) end)
                return
            end
            d.sent_bytes = d.sent_bytes + #d.pending_chunk
            d.pending_chunk = nil
            d.pending_seq = nil
        end

        while not d.eof and (d.next_seq - d.acked_seq) < win do
            local wait_ms = pacer_take(d, CHUNK_SIZE)
            if wait_ms > 0 then
                d.retry_timer = p2p.timer.after(wait_ms, function() pump_download(id) end)
                return
            end
            local chunk, err = p2p.fs.read(d.fh, CHUNK_SIZE)
            if not chunk then
                send_json("dn_err", { id = id, err = err or "读取文件失败" })
                close_download(id)
                return
            end
            if #chunk == 0 then
                d.eof = true
                break
            end
            local seq = d.next_seq
            d.next_seq = seq + 1
            local frame = string.pack("<I4I4", id, seq) .. chunk
            if not p2p.channel.send("dn_data", frame) then
                d.pending_seq = seq
                d.pending_chunk = chunk
                d.retry_timer = p2p.timer.after(RETRY_MS, function() pump_download(id) end)
                return
            end
            d.sent_bytes = d.sent_bytes + #chunk
        end

        if d.eof and d.acked_seq >= d.next_seq then
            if not d.sha256 then
                d.sha256 = p2p.fs.hash(d.fh) or ""
            end
            if not send_json("dn_end", { id = id, bytes = d.sent_bytes, sha256 = d.sha256 }) then
                d.retry_timer = p2p.timer.after(RETRY_MS, function() pump_download(id) end)
                return
            end
            close_download(id)
        end
    end

    local function send_list_response(req_id, path)
        local listing, err = p2p.fs.list(path)
        if not listing then
            send_json("list_res", { req = req_id, ok = false, err = err or "无法读取目录" })
            return
        end
        local entries = listing.entries or {}
        local truncated = listing.truncated == true
        local resp = {
            req = req_id,
            ok = true,
            path = listing.path,
            parent = listing.parent or "",
            truncated = truncated,
            entries = entries
        }
        local encoded = p2p.json.encode(resp)
        -- Ensure the directory listing fits cleanly inside one 64 KiB plugin frame.
        while (not encoded or #encoded > MAX_LIST_JSON) and #entries > 1 do
            local keep = math.floor(#entries / 2)
            local trimmed = {}
            for i = 1, keep do trimmed[i] = entries[i] end
            entries = trimmed
            resp.entries = entries
            resp.truncated = true
            encoded = p2p.json.encode(resp)
        end
        if encoded then p2p.channel.send("list_res", encoded) end
    end

    p2p.session.on("connected", function()
        p2p.channel.send("ready", "1")
    end)

    p2p.session.on("disconnected", cleanup_all)

    p2p.channel.on_message(function(kind, payload)
        if kind == "hello" then
            p2p.channel.send("ready", "1")
            return
        end

        if kind == "up_data" then
            if #payload < 8 then return end
            local id, seq = string.unpack("<I4I4", payload)
            local u = uploads[id]
            if not u then return end
            if seq ~= u.next_seq then
                send_json("up_err", { id = id, err = "数据分片序号不一致" })
                close_upload(id)
                return
            end
            local data = string.sub(payload, 9)
            local ok, err = p2p.fs.write(u.fh, data)
            if not ok then
                send_json("up_err", { id = id, err = err or "写入文件失败" })
                close_upload(id)
                return
            end
            u.received = u.received + #data
            u.next_seq = u.next_seq + 1
            if (u.next_seq % ACK_EVERY) == 0 or #data < CHUNK_SIZE or u.received >= u.expected_size then
                send_json("up_ack", { id = id, seq = seq })
            end
            return
        end

        local msg = p2p.json.decode(payload)
        if type(msg) ~= "table" then return end

        if kind == "roots_req" then
            local roots = p2p.fs.roots() or {}
            local home = p2p.fs.home() or {}
            send_json("roots_res", { roots = roots, home = home })
        elseif kind == "list_req" then
            local target = msg.path
            if type(msg.dir) == "string" and type(msg.child) == "string" and msg.child ~= "" then
                local joined, err = p2p.fs.join(msg.dir, msg.child)
                if not joined then
                    send_json("list_res", { req = msg.req, ok = false, err = err or "路径无效" })
                    return
                end
                target = joined
            end
            if type(target) ~= "string" or target == "" then
                send_json("list_res", { req = msg.req, ok = false, err = "路径为空" })
                return
            end
            send_list_response(msg.req, target)
        elseif kind == "mkdir_req" then
            local joined, jerr = p2p.fs.join(msg.dir or "", msg.name or "")
            if not joined then
                send_json("mkdir_res", { req = msg.req, ok = false, err = jerr or "目录名无效" })
                return
            end
            local ok, err = p2p.fs.mkdir(joined)
            send_json("mkdir_res", { req = msg.req, ok = ok == true, err = err or "" })
        elseif kind == "up_open" then
            local id = msg.id
            if type(id) ~= "number" then return end
            close_upload(id)
            local target, jerr = p2p.fs.join(msg.dir or "", msg.name or "")
            if not target then
                send_json("up_err", { id = id, err = jerr or "目标文件名无效" })
                return
            end
            local fh, err = p2p.fs.open_write(target)
            if not fh then
                send_json("up_err", { id = id, err = err or "无法创建目标文件" })
                return
            end
            uploads[id] = {
                fh = fh,
                expected_size = tonumber(msg.size) or 0,
                received = 0,
                next_seq = 0
            }
            send_json("up_ok", { id = id })
        elseif kind == "up_end" then
            local id = msg.id
            local u = uploads[id]
            if not u then return end
            if u.received ~= u.expected_size then
                send_json("up_err", { id = id, err = "文件大小校验不一致" })
                close_upload(id)
                return
            end
            if type(msg.sha256) == "string" and #msg.sha256 == 64 then
                local actual = p2p.fs.hash(u.fh)
                if actual and actual ~= msg.sha256 then
                    send_json("up_err", { id = id, err = "文件 SHA-256 校验不一致" })
                    close_upload(id)
                    return
                end
            end
            local ok, err = p2p.fs.commit(u.fh)
            u.fh = nil
            uploads[id] = nil
            if not ok then
                send_json("up_err", { id = id, err = err or "保存文件失败" })
                return
            end
            send_json("up_done", { id = id })
        elseif kind == "up_cancel" then
            if type(msg.id) == "number" then close_upload(msg.id) end
        elseif kind == "dn_open" then
            local id = msg.id
            if type(id) ~= "number" then return end
            close_download(id)
            local target, jerr = p2p.fs.join(msg.dir or "", msg.name or "")
            if not target then
                send_json("dn_err", { id = id, err = jerr or "源文件名无效" })
                return
            end
            local fh, size_or_err = p2p.fs.open_read(target)
            if not fh then
                send_json("dn_err", { id = id, err = size_or_err or "无法读取文件" })
                return
            end
            local init_win = clamp(tonumber(msg.win) or MAX_WINDOW, MIN_WINDOW, MAX_WINDOW)
            local init_pace = math.max(0, tonumber(msg.pace) or 0)
            downloads[id] = {
                fh = fh,
                total = size_or_err or 0,
                sent_bytes = 0,
                next_seq = 0,
                acked_seq = 0,
                eof = false,
                win = init_win,
                pace_bps = init_pace,
                last_token_ms = p2p.timer.now_ms(),
                tokens = CHUNK_SIZE * 2
            }
            send_json("dn_meta", { id = id, size = size_or_err or 0 })
            pump_download(id)
        elseif kind == "dn_ack" then
            local id = msg.id
            local d = downloads[id]
            if not d then return end
            if type(msg.seq) == "number" and (msg.seq + 1) > d.acked_seq then
                d.acked_seq = msg.seq + 1
            end
            if type(msg.win) == "number" then
                d.win = clamp(math.floor(msg.win), MIN_WINDOW, MAX_WINDOW)
            end
            if type(msg.pace) == "number" then
                d.pace_bps = math.max(0, math.floor(msg.pace))
            end
            pump_download(id)
        elseif kind == "dn_pace" then
            local id = msg.id
            local d = downloads[id]
            if not d then return end
            if type(msg.win) == "number" then
                d.win = clamp(math.floor(msg.win), MIN_WINDOW, MAX_WINDOW)
            end
            if type(msg.pace) == "number" then
                d.pace_bps = math.max(0, math.floor(msg.pace))
            end
            pump_download(id)
        elseif kind == "dn_cancel" then
            if type(msg.id) == "number" then close_download(msg.id) end
        end
    end)

    return
end

-- ============================================================================
-- Viewer Role: declarative dual-pane file transfer dialog
-- ============================================================================
local dialog_id = nil
local peer_ready = false
local req_seq = 0
local transfer_seq = 0

local local_dir = ""
local local_parent = ""
local local_entries = {}
local local_quick_items = {}

local remote_dir = ""
local remote_parent = ""
local remote_entries = {}
local remote_quick_items = {}
local remote_list_req = 0

local queue = {}
local active = nil -- { id, dir="upload"|"download", name, local_path, remote_dir, fh, total, done, start_ms, last_ui_ms, next_seq, acked_seq, eof, pending_seq, pending_chunk, retry_timer, accepted, sha256, pace_bps, last_token_ms, tokens }

-- Automatic video-loss-aware AIMD controller (keeps video loss < VIDEO_LOSS_THRESHOLD).
local dyn_win = MAX_WINDOW
local dyn_pace_bps = 0 -- 0 = unlimited when video is clean
local baseline_rtt = nil
local clean_ticks = 0
local sample_prev_done = 0
local sample_prev_ms = 0
local sample_job_id = 0
local recent_bps = 0

local function update_ui(patch)
    if dialog_id then
        p2p.ui.update_dialog(dialog_id, patch)
    end
end

local function set_status(text)
    update_ui({ status_label = { text = text } })
end

local function entries_to_rows(entries)
    local rows = {}
    for _, e in ipairs(entries) do
        table.insert(rows, {
            id = e.name,
            icon = e.is_dir and "dir" or "file",
            cells = {
                e.name,
                e.is_dir and "文件夹" or format_size(e.size),
                e.mtime_text or ""
            }
        })
    end
    return rows
end

local function refresh_local(target_path)
    local path = target_path or local_dir
    if not path or path == "" then return end
    local listing, err = p2p.fs.list(path)
    if not listing then
        set_status("本机目录读取失败：" .. (err or "未知错误"))
        return
    end
    local_dir = listing.path
    local_parent = listing.parent or ""
    local_entries = listing.entries or {}
    local info = string.format("本机：%d 项%s", #local_entries, listing.truncated and "（已截断）" or "")
    update_ui({
        local_path = { text = local_dir },
        local_up = { enabled = local_parent ~= "" },
        local_table = { rows = entries_to_rows(local_entries) },
        local_info = { text = info }
    })
end

local function request_remote_list(path, child)
    if not peer_ready then
        set_status("正在等待远端文件传输插件就绪…")
        return
    end
    req_seq = req_seq + 1
    remote_list_req = req_seq
    if child and child ~= "" then
        send_json("list_req", { req = req_seq, dir = path, child = child })
    else
        send_json("list_req", { req = req_seq, path = path })
    end
end

local function set_transfer_buttons(busy)
    update_ui({
        upload_btn = { enabled = (not busy) and peer_ready },
        download_btn = { enabled = (not busy) and peer_ready },
        cancel_btn = { enabled = busy }
    })
end

local function update_progress(force)
    if not active then return end
    local now = p2p.timer.now_ms()
    if not force and (now - active.last_ui_ms) < 100 then return end
    active.last_ui_ms = now
    local pct = 100
    if active.total > 0 then
        pct = math.floor((active.done * 100) / active.total)
        if pct > 100 then pct = 100 end
    end
    local elapsed_s = math.max(0.05, (now - active.start_ms) / 1000.0)
    local speed = format_speed(active.done / elapsed_s)
    local verb = active.dir == "upload" and "上传" or "下载"
    local rem = #queue > 0 and string.format("（剩余 %d 个）", #queue) or ""
    local pacing_tag = dyn_pace_bps > 0 and " · 自动保画质" or ""
    local bar_text = string.format("%s %s · %d%% · %s%s", verb, active.name, pct, speed, pacing_tag)
    local detail = string.format("%s：%s / %s · %s %s",
        bar_text, format_size(active.done), format_size(active.total), speed, rem)
    update_ui({
        transfer_progress = { value = pct, text = bar_text },
        status_label = { text = detail }
    })
end

local start_next_transfer

local function finish_active(ok, message)
    local job = active
    active = nil
    if job then
        if job.retry_timer then p2p.timer.cancel(job.retry_timer) end
        if job.fh then p2p.fs.close(job.fh) end
    end
    if not ok then
        queue = {}
        set_transfer_buttons(false)
        update_ui({
            transfer_progress = { value = 0, text = "已停止" },
            status_label = { text = message or "传输失败" }
        })
        return
    end

    if job and job.dir == "upload" and remote_dir ~= "" then
        request_remote_list(remote_dir)
    elseif job and job.dir == "download" and local_dir ~= "" then
        refresh_local(local_dir)
    end

    if #queue > 0 then
        start_next_transfer()
    else
        set_transfer_buttons(false)
        update_ui({
            transfer_progress = { value = 100, text = "传输完成" },
            status_label = { text = message or "所有文件传输完成" }
        })
        if dialog_id then p2p.ui.notify(message or "文件传输完成") end
    end
end

local function pump_upload()
    local job = active
    if not job or job.dir ~= "upload" or not job.accepted or not p2p.session.connected() then return end
    if job.retry_timer then
        p2p.timer.cancel(job.retry_timer)
        job.retry_timer = nil
    end

    job.pace_bps = dyn_pace_bps
    local win = clamp(dyn_win, MIN_WINDOW, MAX_WINDOW)

    if job.pending_chunk then
        local wait_ms = pacer_take(job, #job.pending_chunk)
        if wait_ms > 0 then
            job.retry_timer = p2p.timer.after(wait_ms, pump_upload)
            return
        end
        local frame = string.pack("<I4I4", job.id, job.pending_seq) .. job.pending_chunk
        if not p2p.channel.send("up_data", frame) then
            job.retry_timer = p2p.timer.after(RETRY_MS, pump_upload)
            return
        end
        job.done = job.done + #job.pending_chunk
        job.pending_chunk = nil
        job.pending_seq = nil
        update_progress(false)
    end

    while not job.eof and (job.next_seq - job.acked_seq) < win do
        local wait_ms = pacer_take(job, CHUNK_SIZE)
        if wait_ms > 0 then
            job.retry_timer = p2p.timer.after(wait_ms, pump_upload)
            return
        end
        local chunk, err = p2p.fs.read(job.fh, CHUNK_SIZE)
        if not chunk then
            send_json("up_cancel", { id = job.id })
            finish_active(false, "读取本机文件失败：" .. (err or "未知错误"))
            return
        end
        if #chunk == 0 then
            job.eof = true
            break
        end
        local seq = job.next_seq
        job.next_seq = seq + 1
        local frame = string.pack("<I4I4", job.id, seq) .. chunk
        if not p2p.channel.send("up_data", frame) then
            job.pending_seq = seq
            job.pending_chunk = chunk
            job.retry_timer = p2p.timer.after(RETRY_MS, pump_upload)
            return
        end
        job.done = job.done + #chunk
        update_progress(false)
    end

    if job.eof and job.acked_seq >= job.next_seq then
        if not job.sha256 and job.fh then
            job.sha256 = p2p.fs.hash(job.fh) or ""
        end
        if not send_json("up_end", { id = job.id, bytes = job.done, sha256 = job.sha256 or "" }) then
            job.retry_timer = p2p.timer.after(RETRY_MS, pump_upload)
            return
        end
        if job.fh then
            p2p.fs.close(job.fh)
            job.fh = nil
        end
        update_progress(true)
    end
end

local function adapt_transfer_rate()
    if not p2p.session.connected() then return end
    local stats = p2p.session.stats()
    if type(stats) ~= "table" then return end

    local rtt = type(stats.rtt_ms) == "number" and stats.rtt_ms or nil
    local loss = type(stats.loss) == "number" and stats.loss or nil

    -- Track baseline RTT while idle or clean so we can detect bufferbloat early.
    if rtt and rtt > 0 then
        if not baseline_rtt or rtt < baseline_rtt then
            baseline_rtt = rtt
        elseif not active and (not loss or loss <= VIDEO_LOSS_CLEAR) then
            baseline_rtt = baseline_rtt * 0.9 + rtt * 0.1
        end
    end

    if not active then
        sample_job_id = 0
        recent_bps = 0
        if not loss or loss <= VIDEO_LOSS_CLEAR then
            dyn_win = MAX_WINDOW
            dyn_pace_bps = 0
            clean_ticks = 0
        end
        return
    end

    local now = p2p.timer.now_ms()
    if sample_job_id ~= active.id then
        sample_job_id = active.id
        sample_prev_done = active.done
        sample_prev_ms = now
        recent_bps = 0
    else
        local dt = now - sample_prev_ms
        if dt >= 100 then
            local delta = active.done - sample_prev_done
            local inst = (delta * 1000.0) / dt
            recent_bps = recent_bps > 0 and (recent_bps * 0.5 + inst * 0.5) or inst
            sample_prev_done = active.done
            sample_prev_ms = now
        end
    end

    local rtt_inflate = (rtt and baseline_rtt) and (rtt - baseline_rtt) or 0
    local prev_win, prev_pace = dyn_win, dyn_pace_bps
    local ref_bps = dyn_pace_bps > 0 and dyn_pace_bps or math.max(recent_bps, 2 * 1024 * 1024)

    if loss and loss >= VIDEO_LOSS_SEVERE then
        -- Severe video loss (>= 3%): immediate aggressive backoff.
        clean_ticks = 0
        dyn_win = clamp(math.floor(dyn_win * 0.5), MIN_WINDOW, MAX_WINDOW)
        dyn_pace_bps = math.max(MIN_PACE_BPS, math.floor(ref_bps * 0.5))
    elseif (loss and loss >= VIDEO_LOSS_THRESHOLD) or rtt_inflate >= RTT_INFLATE_CUT_MS then
        -- Above video loss threshold (>= 1%) or severe queuing delay: cut rate to keep video smooth.
        clean_ticks = 0
        dyn_win = clamp(math.floor(dyn_win * 0.75), MIN_WINDOW, MAX_WINDOW)
        dyn_pace_bps = math.max(MIN_PACE_BPS, math.floor(ref_bps * 0.72))
    elseif (loss and loss >= VIDEO_LOSS_THRESHOLD * 0.5) or rtt_inflate >= RTT_INFLATE_HOLD_MS then
        -- Approaching threshold: hold rate steady and gently trim window.
        clean_ticks = 0
        if dyn_win > 8 then dyn_win = dyn_win - 1 end
    else
        -- Video loss well below threshold: smoothly ramp throughput back up.
        clean_ticks = clean_ticks + 1
        if clean_ticks >= 2 then
            dyn_win = clamp(dyn_win + 2, MIN_WINDOW, MAX_WINDOW)
            if dyn_pace_bps > 0 then
                local next_pace = math.floor(dyn_pace_bps * 1.25 + 512 * 1024)
                if next_pace >= UNLIMITED_PACE_BPS and dyn_win >= MAX_WINDOW then
                    dyn_pace_bps = 0
                else
                    dyn_pace_bps = next_pace
                end
            end
        end
    end

    if active.dir == "download" and (dyn_win ~= prev_win or dyn_pace_bps ~= prev_pace) then
        send_json("dn_pace", { id = active.id, win = dyn_win, pace = dyn_pace_bps })
    elseif active.dir == "upload" then
        active.pace_bps = dyn_pace_bps
        if dyn_win > prev_win or (prev_pace > 0 and (dyn_pace_bps == 0 or dyn_pace_bps > prev_pace)) then
            pump_upload()
        end
    end
end

start_next_transfer = function()
    if active or #queue == 0 then return end
    local item = table.remove(queue, 1)
    transfer_seq = transfer_seq + 1
    local tid = transfer_seq
    local now = p2p.timer.now_ms()

    if item.dir == "upload" then
        local fh, size_or_err = p2p.fs.open_read(item.local_path)
        if not fh then
            finish_active(false, "无法打开本机文件：" .. (size_or_err or "未知错误"))
            return
        end
        active = {
            id = tid,
            dir = "upload",
            name = item.name,
            local_path = item.local_path,
            remote_dir = item.remote_dir,
            fh = fh,
            total = size_or_err or 0,
            done = 0,
            start_ms = now,
            last_ui_ms = 0,
            next_seq = 0,
            acked_seq = 0,
            eof = false,
            accepted = false,
            pace_bps = dyn_pace_bps,
            last_token_ms = now,
            tokens = CHUNK_SIZE * 2
        }
        set_transfer_buttons(true)
        update_progress(true)
        if not send_json("up_open", { id = tid, dir = item.remote_dir, name = item.name, size = active.total }) then
            finish_active(false, "发送上传请求失败")
        end
    else
        local local_target, jerr = p2p.fs.join(item.local_dir, item.name)
        if not local_target then
            finish_active(false, "本机保存路径无效：" .. (jerr or ""))
            return
        end
        local fh, err = p2p.fs.open_write(local_target)
        if not fh then
            finish_active(false, "无法创建本机文件：" .. (err or "未知错误"))
            return
        end
        active = {
            id = tid,
            dir = "download",
            name = item.name,
            local_path = local_target,
            remote_dir = item.remote_dir,
            fh = fh,
            total = item.size or 0,
            done = 0,
            start_ms = now,
            last_ui_ms = 0,
            next_seq = 0
        }
        set_transfer_buttons(true)
        update_progress(true)
        if not send_json("dn_open", { id = tid, dir = item.remote_dir, name = item.name, win = dyn_win, pace = dyn_pace_bps }) then
            finish_active(false, "发送下载请求失败")
        end
    end
end

local function cancel_transfer(reason)
    if active then
        local id = active.id
        local dir = active.dir
        if dir == "upload" then send_json("up_cancel", { id = id })
        else send_json("dn_cancel", { id = id }) end
        finish_active(false, reason or "已取消传输")
    end
end

local function find_entry(entries, name)
    for _, e in ipairs(entries) do
        if e.name == name then return e end
    end
    return nil
end

local function handle_dialog_event(ev)
    if ev.event == "close" and ev.control == "" then
        dialog_id = nil
        cancel_transfer("传输窗口已关闭")
        return
    end

    local ctrl = ev.control
    local values = type(ev.values) == "table" and ev.values or {}

    if ctrl == "local_quick" and ev.event == "change" then
        if type(ev.value) == "string" and ev.value ~= "" then refresh_local(ev.value) end
    elseif ctrl == "local_path" and ev.event == "submit" then
        local p = trim(ev.text)
        if p ~= "" then refresh_local(p) end
    elseif ctrl == "local_go" and ev.event == "click" then
        local p = trim(values.local_path)
        if p ~= "" then refresh_local(p) end
    elseif ctrl == "local_up" and ev.event == "click" then
        if local_parent ~= "" then refresh_local(local_parent) end
    elseif ctrl == "local_refresh" and ev.event == "click" then
        refresh_local(local_dir)
    elseif ctrl == "local_mkdir" and ev.event == "click" then
        local name = trim(ev.text)
        if name ~= "" and local_dir ~= "" then
            local joined, jerr = p2p.fs.join(local_dir, name)
            if not joined then
                set_status("新建本机文件夹失败：" .. (jerr or "名称无效"))
            else
                local ok, err = p2p.fs.mkdir(joined)
                if ok then
                    refresh_local(local_dir)
                    set_status("已创建本机文件夹：" .. name)
                else
                    set_status("新建本机文件夹失败：" .. (err or "未知错误"))
                end
            end
        end
    elseif ctrl == "local_table" and ev.event == "double_click" then
        local entry = find_entry(local_entries, ev.row)
        if entry and entry.is_dir then
            local joined = p2p.fs.join(local_dir, entry.name)
            if joined then refresh_local(joined) end
        end
    elseif ctrl == "remote_quick" and ev.event == "change" then
        if type(ev.value) == "string" and ev.value ~= "" then request_remote_list(ev.value) end
    elseif ctrl == "remote_path" and ev.event == "submit" then
        local p = trim(ev.text)
        if p ~= "" then request_remote_list(p) end
    elseif ctrl == "remote_go" and ev.event == "click" then
        local p = trim(values.remote_path)
        if p ~= "" then request_remote_list(p) end
    elseif ctrl == "remote_up" and ev.event == "click" then
        if remote_parent ~= "" then request_remote_list(remote_parent) end
    elseif ctrl == "remote_refresh" and ev.event == "click" then
        if remote_dir ~= "" then request_remote_list(remote_dir) end
    elseif ctrl == "remote_mkdir" and ev.event == "click" then
        local name = trim(ev.text)
        if name ~= "" and remote_dir ~= "" and peer_ready then
            req_seq = req_seq + 1
            send_json("mkdir_req", { req = req_seq, dir = remote_dir, name = name })
        end
    elseif ctrl == "remote_table" and ev.event == "double_click" then
        local entry = find_entry(remote_entries, ev.row)
        if entry and entry.is_dir and remote_dir ~= "" then
            request_remote_list(remote_dir, entry.name)
        end
    elseif ctrl == "upload_btn" and ev.event == "click" then
        if active then return end
        if not peer_ready or remote_dir == "" then
            set_status("远端目录尚未就绪")
            return
        end
        local selected = values.local_table
        if type(selected) ~= "table" or #selected == 0 then
            set_status("请先在左侧选择要上传的文件")
            return
        end
        local added = 0
        local skipped_dirs = 0
        for _, name in ipairs(selected) do
            local entry = find_entry(local_entries, name)
            if entry then
                if entry.is_dir then
                    skipped_dirs = skipped_dirs + 1
                else
                    local joined = p2p.fs.join(local_dir, entry.name)
                    if joined then
                        table.insert(queue, {
                            dir = "upload",
                            name = entry.name,
                            local_path = joined,
                            remote_dir = remote_dir
                        })
                        added = added + 1
                    end
                end
            end
        end
        if added == 0 then
            set_status(skipped_dirs > 0 and "暂不支持直接传输整个文件夹，请进入文件夹后选择文件" or "未选中可上传的文件")
            return
        end
        start_next_transfer()
    elseif ctrl == "download_btn" and ev.event == "click" then
        if active then return end
        if not peer_ready or local_dir == "" or remote_dir == "" then
            set_status("目录尚未就绪")
            return
        end
        local selected = values.remote_table
        if type(selected) ~= "table" or #selected == 0 then
            set_status("请先在右侧选择要下载的文件")
            return
        end
        local added = 0
        local skipped_dirs = 0
        for _, name in ipairs(selected) do
            local entry = find_entry(remote_entries, name)
            if entry then
                if entry.is_dir then
                    skipped_dirs = skipped_dirs + 1
                else
                    table.insert(queue, {
                        dir = "download",
                        name = entry.name,
                        remote_dir = remote_dir,
                        local_dir = local_dir,
                        size = entry.size or 0
                    })
                    added = added + 1
                end
            end
        end
        if added == 0 then
            set_status(skipped_dirs > 0 and "暂不支持直接传输整个文件夹，请进入文件夹后选择文件" or "未选中可下载的文件")
            return
        end
        start_next_transfer()
    elseif ctrl == "cancel_btn" and ev.event == "click" then
        cancel_transfer("已手动取消传输")
    end
end

local function open_transfer_dialog()
    if dialog_id then
        -- Bring existing state up to date by reopening or refreshing.
        refresh_local(local_dir)
        if peer_ready and remote_dir ~= "" then request_remote_list(remote_dir) end
        return
    end

    local home = p2p.fs.home() or {}
    local roots = p2p.fs.roots() or {}
    local_quick_items = build_quick_items(home, roots)
    local initial_local = home.desktop or home.home or (roots[1] and roots[1].path) or ""

    local spec = {
        title = "文件传输 · P2P Remote",
        width = 1020,
        height = 640,
        min_width = 760,
        min_height = 460,
        body = {
            type = "vbox",
            spacing = 10,
            children = {
                {
                    type = "split",
                    orientation = "horizontal",
                    stretch = 1,
                    sizes = { 490, 490 },
                    children = {
                        {
                            type = "vbox",
                            spacing = 6,
                            margins = { 0, 0, 6, 0 },
                            children = {
                                {
                                    type = "hbox",
                                    spacing = 6,
                                    children = {
                                        { type = "label", text = "本机文件", bold = true },
                                        { type = "spacer" },
                                        { type = "combo", id = "local_quick", items = local_quick_items, selected = initial_local, width = 120 },
                                        { type = "button", id = "local_up", text = "上级" },
                                        { type = "button", id = "local_refresh", text = "刷新" },
                                        {
                                            type = "button",
                                            id = "local_mkdir",
                                            text = "新建文件夹",
                                            prompt = { title = "新建本机文件夹", label = "文件夹名称：" }
                                        }
                                    }
                                },
                                {
                                    type = "hbox",
                                    spacing = 6,
                                    children = {
                                        { type = "input", id = "local_path", text = initial_local, placeholder = "输入本机路径后回车…", stretch = 1 },
                                        { type = "button", id = "local_go", text = "前往" }
                                    }
                                },
                                {
                                    type = "table",
                                    id = "local_table",
                                    columns = { "名称", "大小", "修改时间" },
                                    column_widths = { 230, 90, 140 },
                                    multi_select = true,
                                    stretch = 1
                                },
                                { type = "label", id = "local_info", text = "本机：正在加载…", muted = true }
                            }
                        },
                        {
                            type = "vbox",
                            spacing = 6,
                            margins = { 6, 0, 0, 0 },
                            children = {
                                {
                                    type = "hbox",
                                    spacing = 6,
                                    children = {
                                        { type = "label", text = "远端设备", bold = true },
                                        { type = "spacer" },
                                        { type = "combo", id = "remote_quick", items = remote_quick_items, width = 120 },
                                        { type = "button", id = "remote_up", text = "上级", enabled = false },
                                        { type = "button", id = "remote_refresh", text = "刷新" },
                                        {
                                            type = "button",
                                            id = "remote_mkdir",
                                            text = "新建文件夹",
                                            prompt = { title = "新建远端文件夹", label = "文件夹名称：" }
                                        }
                                    }
                                },
                                {
                                    type = "hbox",
                                    spacing = 6,
                                    children = {
                                        { type = "input", id = "remote_path", text = remote_dir, placeholder = "等待远端连接…", stretch = 1 },
                                        { type = "button", id = "remote_go", text = "前往" }
                                    }
                                },
                                {
                                    type = "table",
                                    id = "remote_table",
                                    columns = { "名称", "大小", "修改时间" },
                                    column_widths = { 230, 90, 140 },
                                    multi_select = true,
                                    stretch = 1
                                },
                                { type = "label", id = "remote_info", text = "远端：正在连接…", muted = true }
                            }
                        }
                    }
                },
                {
                    type = "hbox",
                    spacing = 8,
                    children = {
                        { type = "button", id = "upload_btn", text = "上传到远端 →", primary = true, enabled = peer_ready },
                        { type = "button", id = "download_btn", text = "← 下载到本机", primary = true, enabled = peer_ready },
                        { type = "spacer" },
                        { type = "button", id = "cancel_btn", text = "取消传输", enabled = false }
                    }
                },
                {
                    type = "vbox",
                    spacing = 4,
                    children = {
                        { type = "progress", id = "transfer_progress", value = 0, text = "就绪" },
                        { type = "label", id = "status_label", text = "双击文件夹进入，选中文件后点击“上传到远端”或“下载到本机”", muted = true }
                    }
                }
            }
        }
    }

    dialog_id = p2p.ui.open_dialog(spec, handle_dialog_event)
    if not dialog_id then return end

    if initial_local ~= "" then refresh_local(initial_local) end
    if peer_ready then
        send_json("roots_req", {})
    else
        p2p.channel.send("hello", "1")
    end
end

p2p.session.on("connected", function()
    p2p.ui.add_menu("文件传输…", open_transfer_dialog)
    p2p.channel.send("hello", "1")
end)

p2p.timer.every(500, adapt_transfer_rate)

p2p.session.on("disconnected", function()
    peer_ready = false
    cancel_transfer("连接已断开")
    dialog_id = nil
end)

p2p.channel.on_message(function(kind, payload)
    if kind == "ready" then
        peer_ready = true
        set_transfer_buttons(active ~= nil)
        if dialog_id then send_json("roots_req", {}) end
        return
    end

    if kind == "dn_data" then
        if #payload < 8 then return end
        local id, seq = string.unpack("<I4I4", payload)
        local job = active
        if not job or job.dir ~= "download" or job.id ~= id then return end
        if seq ~= job.next_seq then
            send_json("dn_cancel", { id = id })
            finish_active(false, "下载数据分片序号异常")
            return
        end
        local data = string.sub(payload, 9)
        local ok, err = p2p.fs.write(job.fh, data)
        if not ok then
            send_json("dn_cancel", { id = id })
            finish_active(false, "写入本机文件失败：" .. (err or "未知错误"))
            return
        end
        job.done = job.done + #data
        job.next_seq = job.next_seq + 1
        if (job.next_seq % ACK_EVERY) == 0 or #data < CHUNK_SIZE or (job.total > 0 and job.done >= job.total) then
            send_json("dn_ack", { id = id, seq = seq, win = dyn_win, pace = dyn_pace_bps })
        end
        update_progress(false)
        return
    end

    local msg = p2p.json.decode(payload)
    if type(msg) ~= "table" then return end

    if kind == "roots_res" then
        remote_quick_items = build_quick_items(msg.home, msg.roots)
        local initial_remote = (msg.home and (msg.home.desktop or msg.home.home))
            or (msg.roots and msg.roots[1] and msg.roots[1].path) or ""
        update_ui({
            remote_quick = { items = remote_quick_items, selected = initial_remote },
            upload_btn = { enabled = active == nil },
            download_btn = { enabled = active == nil }
        })
        if initial_remote ~= "" then
            request_remote_list(initial_remote)
        end
    elseif kind == "list_res" then
        if msg.req ~= remote_list_req then return end
        if not msg.ok then
            set_status("远端目录读取失败：" .. (msg.err or "未知错误"))
            return
        end
        remote_dir = msg.path or ""
        remote_parent = msg.parent or ""
        remote_entries = msg.entries or {}
        local info = string.format("远端：%d 项%s", #remote_entries, msg.truncated and "（已截断）" or "")
        update_ui({
            remote_path = { text = remote_dir },
            remote_up = { enabled = remote_parent ~= "" },
            remote_table = { rows = entries_to_rows(remote_entries) },
            remote_info = { text = info }
        })
    elseif kind == "mkdir_res" then
        if msg.ok then
            set_status("已创建远端文件夹")
            if remote_dir ~= "" then request_remote_list(remote_dir) end
        else
            set_status("新建远端文件夹失败：" .. (msg.err or "未知错误"))
        end
    elseif kind == "up_ok" then
        if active and active.dir == "upload" and active.id == msg.id then
            active.accepted = true
            pump_upload()
        end
    elseif kind == "up_ack" then
        if active and active.dir == "upload" and active.id == msg.id then
            if type(msg.seq) == "number" and (msg.seq + 1) > active.acked_seq then
                active.acked_seq = msg.seq + 1
            end
            pump_upload()
        end
    elseif kind == "up_done" then
        if active and active.dir == "upload" and active.id == msg.id then
            finish_active(true, "已上传：" .. active.name)
        end
    elseif kind == "up_err" then
        if active and active.dir == "upload" and active.id == msg.id then
            finish_active(false, "上传失败：" .. (msg.err or "未知错误"))
        end
    elseif kind == "dn_meta" then
        if active and active.dir == "download" and active.id == msg.id then
            if type(msg.size) == "number" then active.total = msg.size end
            update_progress(true)
        end
    elseif kind == "dn_end" then
        if active and active.dir == "download" and active.id == msg.id then
            local expected = tonumber(msg.bytes) or active.total
            if active.done ~= expected then
                finish_active(false, "下载文件大小校验不一致")
                return
            end
            if type(msg.sha256) == "string" and #msg.sha256 == 64 then
                local actual = p2p.fs.hash(active.fh)
                if actual and actual ~= msg.sha256 then
                    finish_active(false, "下载文件 SHA-256 校验不一致")
                    return
                end
            end
            local ok, err = p2p.fs.commit(active.fh)
            active.fh = nil
            if not ok then
                finish_active(false, "保存本机文件失败：" .. (err or "未知错误"))
                return
            end
            finish_active(true, "已下载：" .. active.name)
        end
    elseif kind == "dn_err" then
        if active and active.dir == "download" and active.id == msg.id then
            finish_active(false, "下载失败：" .. (msg.err or "未知错误"))
        end
    end
end)
