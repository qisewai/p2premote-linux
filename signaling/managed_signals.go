package main

import "errors"

// enterManaged is called with the hub lock held. Only a registered installation
// can listen, while any interactive session for its account can join.
func (h *hub) enterManaged(c *client, first message) (*room, error) {
	identity := c.identity
	if identity == nil || identity.ctx.Err() != nil {
		return nil, errors.New("登录已失效，请重新登录")
	}
	switch first.Type {
	case "listen_internet":
		device, err := h.managed.currentDevice(identity.account, identity.installation)
		if err != nil {
			return nil, errors.New("无法查询本机设备")
		}
		if device.ID == "" || (identity.serviceDevice != "" && identity.serviceDevice != device.ID) {
			return nil, errDeviceRemoved
		}
		if old := h.deviceHosts[device.ID]; old != nil {
			if old.host.identity.ctx.Err() == nil {
				return nil, errDirectHostInUse
			}
			h.remove(old)
			old.viewer.send("ended", "设备已重新登录")
			old.host.cancel()
		}
		r := &room{deviceID: device.ID, host: c, stage: "waiting"}
		h.deviceHosts[device.ID] = r
		c.send("ready", "")
		return r, nil
	case "join_internet", "join_diagnostics":
		if identity.serviceDevice != "" {
			return nil, errors.New("请在客户端连接设备")
		}
		r := h.deviceHosts[first.Payload]
		if r == nil || r.host.identity.account != identity.account || r.host.identity.ctx.Err() != nil {
			return nil, errors.New("设备不可连接")
		}
		if r.host.identity.installation == identity.installation {
			return nil, errors.New("请选择另一台设备")
		}
		if r.stage != "waiting" {
			return nil, errDirectUnavailable
		}
		r.diagnostics = first.Type == "join_diagnostics"
		if r.diagnostics && r.host.identity.serviceDevice != "" {
			r.diagnostics = false
			return nil, errors.New("Diagnostics requires an interactive host")
		}
		request(r, c)
		h.startExpiry(r)
		return r, nil
	default:
		return nil, errors.New("需要先上线设备或连接另一台设备")
	}
}
