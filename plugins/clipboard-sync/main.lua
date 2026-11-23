local channel = p2p.channel
local clipboard = p2p.clipboard
local viewer = p2p.session.role() == "viewer"
local ready, retry, last_sent = false, nil, nil

local function digest(text)
    if text and #text > 0 and #text <= 60 * 1024 then
        return p2p.crypto.sha256(text)
    end
end

local function publish(text)
    local hash = digest(text)
    if not hash then
        last_sent = nil
    elseif ready and hash ~= last_sent and channel.send("text", text) then
        last_sent = hash
    end
end

p2p.session.on("connected", function()
    if not viewer or clipboard.get_text() == nil then return end
    local attempts = 1
    channel.send("hello", "")
    retry = p2p.timer.every(500, function()
        if attempts >= 12 then
            p2p.timer.cancel(retry)
        else
            attempts = attempts + 1
            channel.send("hello", "")
        end
    end)
end)

clipboard.on_change(publish)
channel.on_message(function(kind, text)
    if kind == "hello" and not viewer and text == "" and clipboard.get_text() ~= nil then
        ready = channel.send("ready", "") or ready
    elseif kind == "ready" and viewer and not ready and text == "" then
        ready = true
        if retry then p2p.timer.cancel(retry) end
        publish(clipboard.get_text())
    elseif kind == "text" and ready then
        local hash = digest(text)
        if hash and hash ~= digest(clipboard.get_text()) and clipboard.set_text(text) then
            last_sent = hash
        end
    end
end)
