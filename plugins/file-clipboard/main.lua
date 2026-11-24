local clipboard, channel, fs, timer = p2p.file_clipboard, p2p.channel, p2p.fs, p2p.timer
if not clipboard.available() then return end

local viewer = p2p.session.role() == "viewer"
local MAX_ENTRIES = 512
local MAX_MANIFEST = 60 * 1024
local CHUNK = 32 * 1024
local WINDOW = 8 * CHUNK
local ID = "<I4"
local READ = "<I4I4I4i8I4"
local DATA = "<I4I4"
local ready, hello_timer = false, nil
local selection, offer_serial, advertised, advertise_timer = nil, 0, 0, nil
local remote_offer = 0
local incoming, outgoing, last_request = nil, nil, 0

-- Source paths and write timestamps stay here. The peer sees names, sizes and
-- indices into the one selection the user explicitly copied.
local function collect(selection)
    local pending, files, sources = {}, {}, {}
    local scheduled, name_bytes = #selection, 0
    for i = #selection, 1, -1 do pending[#pending + 1] = selection[i] end
    while #pending > 0 do
        local item = table.remove(pending)
        local info = fs.stat(item.path, true)
        if not info or not info.exists then return nil end
        if not info.reparse then
            name_bytes = name_bytes + #item.name
            if name_bytes > MAX_MANIFEST then return nil end
            files[#files + 1] = { item.name, info.size, info.is_dir }
            sources[#sources + 1] = { path = item.path, size = info.size, stamp = info.stamp, dir = info.is_dir }
            if info.is_dir then
                local list = fs.list(item.path, true, MAX_ENTRIES - scheduled)
                if not list or list.truncated then return nil end
                scheduled = scheduled + #list.entries
                for i = #list.entries, 1, -1 do
                    local child = list.entries[i]
                    local name = item.name .. "\\" .. child.name
                    if #name >= 4 * 260 then return nil end
                    pending[#pending + 1] = { name = name, path = item.path .. "\\" .. child.name }
                end
            end
        end
    end
    if #files > 0 then return files, sources end
end

local function stop_outgoing()
    if not outgoing then return end
    if outgoing.retry then timer.cancel(outgoing.retry) end
    if outgoing.file then fs.close(outgoing.file) end
    outgoing = nil
end

local function stop_incoming(cancel_peer)
    if not incoming then return end
    if incoming.retry then timer.cancel(incoming.retry) end
    if cancel_peer then channel.send("cancel", string.pack(ID, incoming.id)) end
    incoming = nil
end

local function advertise()
    advertise_timer = nil
    if not ready then return end
    if selection then
        if advertised == selection.id then return end
        if channel.send("offer", selection.payload) then
            advertised = selection.id
            return
        end
    elseif advertised ~= 0 then
        if channel.send("clear", string.pack(ID, advertised)) then
            advertised = 0
            return
        end
    else
        return
    end
    advertise_timer = timer.after(50, advertise)
end

clipboard.on_change(function(paths)
    stop_outgoing()
    if advertise_timer then timer.cancel(advertise_timer) end
    selection = nil
    local files, sources = collect(paths)
    if files then
        offer_serial = offer_serial + 1
        local payload = p2p.json.encode({ id = offer_serial, files = files })
        if payload and #payload <= MAX_MANIFEST then
            selection = { id = offer_serial, payload = payload, sources = sources }
        end
    end
    if #paths > 0 and not selection then p2p.log.warn("Could not describe the clipboard file selection") end
    advertise()
end)

local function send_read()
    local job = incoming
    job.retry = nil
    if timer.now_ms() >= job.deadline then
        clipboard.reply(job.id, nil)
        stop_incoming(false)
    elseif not channel.send("read", job.payload) then
        job.retry = timer.after(30, send_read)
    end
end

clipboard.on_read(function(id, offer, index, offset, size)
    stop_incoming(true)
    if not ready or offer ~= remote_offer then
        clipboard.reply(id, nil)
        return
    end
    incoming = {
        id = id, size = size, received = 0, deadline = timer.now_ms() + 12000,
        payload = string.pack(READ, id, offer, index, offset, size)
    }
    send_read()
end)

local function pump()
    local job = outgoing
    job.retry = nil
    while timer.now_ms() < job.deadline do
        if job.failed then
            if channel.send("error", string.pack(ID, job.id)) then
                stop_outgoing()
                return
            end
            break
        end
        if not job.chunk then
            local size = math.min(CHUNK, job.size - job.sent)
            local data = fs.read(job.file, size)
            if not data or #data ~= size then
                job.failed = true
            else
                job.chunk = string.pack(DATA, job.id, job.sent) .. data
                job.chunk_size = size
            end
        end
        if job.chunk then
            if not channel.send("data", job.chunk) then break end
            job.sent = job.sent + job.chunk_size
            job.chunk = nil
            job.deadline = timer.now_ms() + 12000
            if job.sent == job.size then
                stop_outgoing()
                return
            end
        end
    end
    if timer.now_ms() >= job.deadline then
        stop_outgoing()
    else
        job.retry = timer.after(30, pump)
    end
end

local function serve(payload)
    if #payload ~= string.packsize(READ) then return end
    local id, offer, index, offset, size = string.unpack(READ, payload)
    if id <= last_request or offset < 0 or size == 0 or size > WINDOW then return end
    stop_outgoing()
    last_request = id
    local job = { id = id, size = size, sent = 0, failed = true, deadline = timer.now_ms() + 12000 }
    local file = selection and selection.id == offer and selection.sources[index]
    if file and not file.dir and offset <= file.size and size <= file.size - offset then
        local fh, actual_size, stamp = fs.open_read(file.path, true)
        job.file = fh
        job.failed = not fh or actual_size ~= file.size or stamp ~= file.stamp or not fs.seek(fh, offset)
    end
    outgoing = job
    pump()
end

local function receive_data(payload)
    local job = incoming
    if not job or #payload <= string.packsize(DATA) then return end
    local id, offset, start = string.unpack(DATA, payload)
    if id ~= job.id then return end
    local data = payload:sub(start)
    if offset ~= job.received or #data ~= math.min(CHUNK, job.size - job.received) then
        clipboard.reply(id, nil)
        stop_incoming(true)
    elseif not clipboard.reply(id, data) then
        stop_incoming(true)
    else
        job.received = job.received + #data
        if job.received == job.size then stop_incoming(false) end
    end
end

p2p.session.on("connected", function()
    if not viewer then return end
    local attempts = 1
    channel.send("hello", "1")
    hello_timer = timer.every(500, function()
        attempts = attempts + 1
        if attempts <= 12 then
            channel.send("hello", "1")
        else
            timer.cancel(hello_timer)
        end
    end)
end)

p2p.session.on("disconnected", function()
    ready = false
    stop_incoming(false)
    stop_outgoing()
end)

channel.on_message(function(kind, payload)
    if kind == "hello" and not viewer and payload == "1" then
        if channel.send("ready", "1") and not ready then
            ready = true
            advertise()
        end
        return
    end
    if kind == "ready" and viewer and payload == "1" and not ready then
        ready = true
        if hello_timer then timer.cancel(hello_timer) end
        advertise()
        return
    end
    if not ready then return end
    if kind == "offer" and #payload <= MAX_MANIFEST then
        local manifest = p2p.json.decode(payload)
        if type(manifest) ~= "table" or math.type(manifest.id) ~= "integer" or
            manifest.id <= remote_offer or manifest.id > 0xffffffff or type(manifest.files) ~= "table" then return end
        if clipboard.publish(manifest.id, manifest.files) then
            stop_incoming(true)
            remote_offer = manifest.id
        end
    elseif kind == "read" then
        serve(payload)
    elseif kind == "data" then
        receive_data(payload)
    elseif #payload == string.packsize(ID) then
        local id = string.unpack(ID, payload)
        if kind == "clear" and id == remote_offer then
            clipboard.clear(id)
            stop_incoming(true)
        elseif kind == "cancel" and outgoing and outgoing.id == id then
            stop_outgoing()
        elseif kind == "error" and incoming and incoming.id == id then
            clipboard.reply(id, nil)
            stop_incoming(false)
        end
    end
end)
