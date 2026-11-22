package main

import (
	"context"
	"encoding/json"
	"testing"
	"time"
)

func TestManagedMiniRTCNegotiationRequiresApproval(t *testing.T) {
	newClient := func(supported bool) *client {
		return &client{out: make(chan message, 8), miniRTC: supported, identity: &accountSession{ctx: context.Background()}}
	}
	host, viewer := newClient(true), newClient(true)
	expiry := time.NewTimer(time.Hour)
	defer expiry.Stop()
	r := &room{deviceID: "local-device", host: host, viewer: viewer, stage: "requested", expiration: expiry}
	h := &hub{deviceHosts: map[string]*room{r.deviceID: r}}
	join := message{Type: "minirtc", Payload: `{"type":"join_transmission","user_id":"impersonator"}`}
	if h.relay(r, viewer, join) || h.relay(r, viewer, message{Type: "accept", MiniRTC: true, Payload: "control"}) {
		t.Fatal("viewer signaled or approved a session before host authorization")
	}
	if !h.relay(r, host, message{Type: "accept", MiniRTC: true, Payload: "view"}) {
		t.Fatal("host could not approve MiniRTC")
	}
	if got := <-viewer.out; got.Type != "accepted" || !got.MiniRTC || got.Payload != "view" {
		t.Fatalf("incorrect backend or permission: %+v", got)
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

func TestManagedMiniRTCFallsBackForOldClient(t *testing.T) {
	host := &client{miniRTC: true, out: make(chan message, 2)}
	viewer := &client{out: make(chan message, 2)}
	expiry := time.NewTimer(time.Hour)
	defer expiry.Stop()
	r := &room{deviceID: "local-device", host: host, stage: "waiting", expiration: expiry}
	h := &hub{deviceHosts: map[string]*room{r.deviceID: r}}
	request(r, viewer)
	if got := <-host.out; got.MiniRTC {
		t.Fatal("negotiated MiniRTC with an old client")
	}
	<-viewer.out
	if h.relay(r, host, message{Type: "accept", MiniRTC: true, Payload: "control"}) {
		t.Fatal("accepted MiniRTC without viewer support")
	}
	if !h.relay(r, host, message{Type: "accept"}) || (<-viewer.out).MiniRTC ||
		!h.relay(r, host, message{Type: "offer", Payload: "legacy SDP"}) {
		t.Fatal("legacy signaling did not remain usable")
	}
}
