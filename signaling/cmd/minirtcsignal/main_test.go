package main

import (
	"context"
	"crypto/x509"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/coder/websocket/wsjson"
)

func TestProbeSignalRoutesMiniRTCNegotiation(t *testing.T) {
	identities := map[string]string{hostID: "host@test-host-token", viewerID: "viewer@test-viewer-token"}
	ice := &iceConfiguration{Version: 1, ID: "a1b2", ExpiresAt: time.Now().Add(time.Minute).Unix(),
		Servers: []iceServer{{URLs: []string{"stun:127.0.0.1:3478"}}}}
	fixture := &probeSignal{identities: identities, ice: ice, peers: make(map[string]*websocket.Conn)}
	server := httptest.NewTLSServer(fixture)
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	endpoint := "wss" + strings.TrimPrefix(server.URL, "https")
	send := func(conn *websocket.Conn, msg message) {
		t.Helper()
		if err := wsjson.Write(ctx, conn, msg); err != nil {
			t.Fatal(err)
		}
	}
	receive := func(conn *websocket.Conn, kind string) message {
		t.Helper()
		var msg message
		if err := wsjson.Read(ctx, conn, &msg); err != nil || msg.Type != kind {
			t.Fatalf("want %s, got %+v, %v", kind, msg, err)
		}
		return msg
	}
	login := func(identity, expected string) *websocket.Conn {
		t.Helper()
		conn, _, err := websocket.Dial(ctx, endpoint, &websocket.DialOptions{HTTPClient: server.Client()})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = conn.CloseNow() })
		send(conn, message{Type: "login", UserID: identity, IceVersion: 1})
		if reply := receive(conn, "login"); reply.Status != expected {
			t.Fatalf("want login %s, got %s", expected, reply.Status)
		}
		return conn
	}
	login("host@unrecognized", "fail")
	host := login(identities[hostID], "success")
	viewer := login(identities[viewerID], "success")
	send(viewer, message{Type: "join_transmission", UserID: viewerID, TransmissionID: hostID})
	if join := receive(host, "user_join_transmission"); join.UserID != viewerID || join.ICE == nil || join.ICE.ID != ice.ID {
		t.Fatalf("invalid join message: %+v", join)
	}
	send(host, message{Type: "offer", UserID: hostID, RemoteUserID: viewerID, TransmissionID: hostID, SDP: "offer-sdp"})
	if offer := receive(viewer, "offer"); offer.RemoteUserID != hostID || offer.SDP != "offer-sdp" || offer.ICE == nil {
		t.Fatalf("invalid offer: %+v", offer)
	}
	send(viewer, message{Type: "answer", UserID: viewerID, RemoteUserID: hostID, TransmissionID: hostID, SDP: "answer-sdp"})
	if answer := receive(host, "answer"); answer.RemoteUserID != viewerID || answer.SDP != "answer-sdp" {
		t.Fatalf("invalid answer: %+v", answer)
	}
	ufrag := "remote-fragment"
	send(viewer, message{Type: "new_candidate", UserID: viewerID, RemoteUserID: hostID, TransmissionID: hostID, SDP: "candidate-sdp", Ufrag: &ufrag})
	if candidate := receive(host, "new_candidate"); candidate.SDP != "candidate-sdp" || candidate.Ufrag == nil || *candidate.Ufrag != ufrag {
		t.Fatalf("invalid trickle message: %+v", candidate)
	}
}

func TestSessionCertificateUsedWithIPVerification(t *testing.T) {
	cert, publicCert, err := sessionCertificate()
	if err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(publicCert) {
		t.Fatal("invalid CA PEM")
	}
	leaf, err := x509.ParseCertificate(cert.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	if _, err := leaf.Verify(x509.VerifyOptions{Roots: pool, DNSName: "127.0.0.1"}); err != nil {
		t.Fatal(err)
	}
	if _, err := leaf.Verify(x509.VerifyOptions{Roots: pool, DNSName: "127.0.0.2"}); err == nil {
		t.Fatal("certificate accepted for the wrong address")
	}
}
