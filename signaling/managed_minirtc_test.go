package main

import (
	"context"
	"encoding/json"
	"testing"
	"time"
)

func TestMiniRTCNegotiationRequiresApproval(t *testing.T) {
	newClient := func() *client {
		return &client{out: make(chan message, 8), identity: &accountSession{ctx: context.Background()}}
	}
	host, viewer := newClient(), newClient()
	expiry := time.NewTimer(time.Hour)
	defer expiry.Stop()
	r := &room{deviceID: "local-device", host: host, viewer: viewer, stage: "requested", expiration: expiry}
	h := &hub{deviceHosts: map[string]*room{r.deviceID: r}}
	join := message{Type: "minirtc", Payload: `{"type":"join_transmission","user_id":"impersonator"}`}
	if h.relay(r, viewer, join) || h.relay(r, viewer, message{Type: "accept", Payload: "control"}) {
		t.Fatal("viewer signaled or approved a session before host authorization")
	}
	if h.relay(r, host, message{Type: "accept"}) {
		t.Fatal("host approved without explicit control/view permission")
	}
	if !h.relay(r, host, message{Type: "accept", Payload: "view"}) {
		t.Fatal("host could not approve MiniRTC")
	}
	if got := <-viewer.out; got.Type != "accepted" || got.Payload != "view" {
		t.Fatalf("incorrect permission: %+v", got)
	}
	if !h.relay(r, viewer, join) || h.relay(r, viewer, join) {
		t.Fatal("expected one viewer join per approved room")
	}
	var routed miniRTCSignal
	got := <-host.out
	if got.Type != "minirtc" || json.Unmarshal([]byte(got.Payload), &routed) != nil ||
		routed.Type != "user_join_transmission" || routed.UserID != "viewer" || routed.TransmissionID != "host" {
		t.Fatalf("invalid routed join: %+v", got)
	}
	offer := message{Type: "minirtc", Payload: `{"type":"offer","sdp":"v=0","ice":{"forged":true},"remote_user_id":"untrusted"}`}
	if h.relay(r, viewer, offer) || !h.relay(r, host, offer) {
		t.Fatal("only the host can send an offer")
	}
	var forwarded map[string]any
	got = <-viewer.out
	if json.Unmarshal([]byte(got.Payload), &forwarded) != nil || forwarded["ice"] != nil ||
		forwarded["remote_user_id"] != "host" {
		t.Fatal("forwarded untrusted identity or ICE configuration")
	}
	answer := message{Type: "minirtc", Payload: `{"type":"answer","sdp":"v=0"}`}
	if !h.relay(r, viewer, answer) || r.stage != "answered" ||
		h.relay(r, viewer, message{Type: "minirtc", Payload: `{"type":"login","status":"success"}`}) {
		t.Fatal("incorrect answer or unsupported message accepted")
	}
}
