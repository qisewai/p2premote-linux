package main

import "errors"

// enterManaged is called with the hub lock held. Both peers are bound to their
// devices by the bearer session, and only the authenticated owner can join.
func (h *hub) enterManaged(c *client, first message) (*room, error) {
	identity := c.identity
	if identity == nil || identity.ctx.Err() != nil {
		return nil, errors.New("登录已失效，请重新登录")
	}
	switch first.Type {
	case "listen_internet":
		if old := h.deviceHosts[identity.deviceID]; old != nil {
			if old.host.identity.ctx.Err() == nil {
				return nil, errDirectHostInUse
			}
			h.remove(old)
			old.viewer.send("ended", "设备已重新登录")
			old.host.cancel()
		}
		r := &room{deviceID: identity.deviceID, host: c, stage: "waiting"}
		h.deviceHosts[identity.deviceID] = r
		c.send("ready", "")
		return r, nil
	case "join_internet":
		if first.Payload == identity.deviceID {
			return nil, errors.New("请选择另一台设备")
		}
		r := h.deviceHosts[first.Payload]
		if r == nil || r.host.identity.account != identity.account || r.host.identity.ctx.Err() != nil {
			return nil, errors.New("设备不可连接")
		}
		if r.stage != "waiting" {
			return nil, errDirectUnavailable
		}
		request(r, c)
		h.startExpiry(r)
		return r, nil
	default:
		return nil, errors.New("需要先上线设备或连接另一台设备")
	}
}
