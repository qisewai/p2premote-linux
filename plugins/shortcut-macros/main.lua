local function shortcut(label, keys, order)
    p2p.ui.add_menu({
        menu = "input", path = {"快捷键"}, section = "macros", order = order, label = label,
        on_click = function()
            if not p2p.input.send_keys(keys) then p2p.ui.notify("快捷键暂不可用") end
        end
    })
end

shortcut("远端切换窗口 (Alt+Tab)", {"Alt", "Tab"}, 20)
shortcut("远端任务管理器 (Ctrl+Shift+Esc)", {"Ctrl", "Shift", "Esc"}, 30)
shortcut("远端安全选项 (Ctrl+Alt+Del)", {"Ctrl", "Alt", "Delete"}, 40)
shortcut("锁定远端 (Win+L)", {"Win", "L"}, 50)

p2p.ui.add_menu({
    menu = "input", section = "macros", order = 60, label = "输入文本模板",
    on_click = function()
        local text = p2p.config.get("template", "您好，已收到。")
        if type(text) ~= "string" or not p2p.input.send_text(text) then
            p2p.ui.notify("文本模板为空或超出长度限制")
        end
    end
})
