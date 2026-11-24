local function shortcut(label, keys)
    p2p.ui.add_menu(label, function()
        if not p2p.input.send_keys(keys) then p2p.ui.notify("快捷键暂不可用") end
    end)
end

shortcut("远端切换窗口 (Alt+Tab)", {"Alt", "Tab"})
shortcut("远端任务管理器 (Ctrl+Shift+Esc)", {"Ctrl", "Shift", "Esc"})
shortcut("远端安全选项 (Ctrl+Alt+Del)", {"Ctrl", "Alt", "Delete"})
shortcut("锁定远端 (Win+L)", {"Win", "L"})

p2p.ui.add_menu("输入文本模板", function()
    local text = p2p.config.get("template", "您好，已收到。")
    if type(text) ~= "string" or not p2p.input.send_text(text) then
        p2p.ui.notify("文本模板为空或超出长度限制")
    end
end)
