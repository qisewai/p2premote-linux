package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestInternetWebSocketRoles(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	keys := &internetKeys{HostToken: strings.Repeat("A", 43), ViewerToken: strings.Repeat("B", 42) + "A"}
	h := &hub{internet: keys}
	server := httptest.NewServer(http.HandlerFunc(h.serve))
	defer server.Close()
	url := "ws" + strings.TrimPrefix(server.URL, "http")

	if c, res, err := websocket.Dial(ctx, url, nil); err == nil || res == nil || res.StatusCode != http.StatusUnauthorized {
		if c != nil {
			_ = c.CloseNow()
		}
		t.Fatalf("missing token should be denied: %v %v", res, err)
	}
	open := func(token, kind string) *websocket.Conn {
		t.Helper()
		headers := http.Header{"Authorization": []string{"Bearer " + token}}
		c, _, err := websocket.Dial(ctx, url, &websocket.DialOptions{HTTPHeader: headers})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = c.CloseNow() })
		if err := write(ctx, c, message{Type: kind}); err != nil {
			t.Fatal(err)
		}
		return c
	}
	expect := func(c *websocket.Conn, kind string) {
		t.Helper()
		if got, err := read(ctx, c); err != nil || got.Type != kind {
			t.Fatalf("expected %s, got %+v (%v)", kind, got, err)
		}
	}

	expect(open(keys.ViewerToken, "listen_internet"), "error")
	expect(open(keys.HostToken, "join_internet"), "error")
	expect(open(keys.ViewerToken, "join_direct"), "error")
	host := open(keys.HostToken, "listen_internet")
	expect(host, "ready")
	viewer := open(keys.ViewerToken, "join_internet")
	expect(viewer, "waiting")
	expect(host, "request")
	if err := write(ctx, host, message{Type: "accept"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "accepted")
	if err := write(ctx, host, message{Type: "offer", Payload: "SDP"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "offer")
	if err := write(ctx, host, message{Type: "ice", Payload: "0\n0\ncandidate:1 1 udp 1 192.0.2.1 50000 typ host"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "ice")
	if err := write(ctx, viewer, message{Type: "answer", Payload: "SDP"}); err != nil {
		t.Fatal(err)
	}
	expect(host, "answer")
	if err := write(ctx, viewer, message{Type: "ice", Payload: "0\n0\ncandidate:2 1 udp 1 198.51.100.2 50001 typ srflx raddr 192.0.2.2 rport 50000"}); err != nil {
		t.Fatal(err)
	}
	expect(host, "ice")
	h.mu.Lock()
	hostICE, viewerICE := h.direct.hostICE, h.direct.viewerICE
	h.mu.Unlock()
	if hostICE != (iceCandidateCounts{host: 1}) || viewerICE != (iceCandidateCounts{srflx: 1}) {
		t.Fatalf("unexpected ICE counts: host %+v, viewer %+v", hostICE, viewerICE)
	}
}

func TestDirectWebSocketEndDistinguishesStopFromInterruption(t *testing.T) {
	for _, tc := range []struct {
		name      string
		closeHost func(*websocket.Conn) error
		retryable bool
	}{
		{"normal close", func(c *websocket.Conn) error { return c.Close(websocket.StatusNormalClosure, "") }, false},
		{"transport replaced", func(c *websocket.Conn) error { return c.Close(4001, "") }, true},
		{"connection lost", func(c *websocket.Conn) error { return c.CloseNow() }, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			hostIP := netip.MustParseAddr("192.168.0.32")
			viewerIP := netip.MustParseAddr("192.168.0.43")
			h := &hub{directHost: hostIP, directPeer: netip.PrefixFrom(viewerIP, 32)}
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				ip := viewerIP
				if r.URL.Path == "/host" {
					ip = hostIP
				}
				r.RemoteAddr = netip.AddrPortFrom(ip, 49500).String()
				h.serve(w, r)
			}))
			defer server.Close()
			open := func(path, command string) *websocket.Conn {
				t.Helper()
				url := "ws" + strings.TrimPrefix(server.URL, "http") + path
				c, _, err := websocket.Dial(ctx, url, nil)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { _ = c.CloseNow() })
				if err := write(ctx, c, message{Type: command}); err != nil {
					t.Fatal(err)
				}
				return c
			}
			expect := func(c *websocket.Conn, kind string, retryable bool) {
				t.Helper()
				got, err := read(ctx, c)
				if err != nil || got.Type != kind || got.Retryable != retryable {
					t.Fatalf("expected %s retryable=%v, got %+v (%v)", kind, retryable, got, err)
				}
			}
			expect(open("/viewer", "join_direct"), "error", true)
			host := open("/host", "listen_direct")
			expect(host, "ready", false)
			viewer := open("/viewer", "join_direct")
			expect(viewer, "waiting", false)
			expect(host, "request", false)
			if err := tc.closeHost(host); err != nil {
				t.Fatal(err)
			}
			expect(viewer, "ended", tc.retryable)
		})
	}
}

func TestDirectConnectionRequiresConfiguredPeerAndApproval(t *testing.T) {
	hostIP := netip.MustParseAddr("192.168.0.43")
	viewerIP := netip.MustParseAddr("192.168.0.32")
	h := &hub{directHost: hostIP, directPeer: netip.PrefixFrom(viewerIP, 32)}
	newClient := func(ip netip.Addr) *client {
		return &client{ip: ip, out: make(chan message, 8)}
	}
	host, viewer := newClient(hostIP), newClient(viewerIP)
	other := newClient(netip.MustParseAddr("192.168.0.50"))
	if _, err := h.enter(other, message{Type: "listen_direct"}); err == nil {
		t.Fatal("another address registered as Host")
	}
	r, err := h.enter(host, message{Type: "listen_direct"})
	if err != nil {
		t.Fatal(err)
	}
	defer h.leave(r, host, false)
	if got := <-host.out; got.Type != "ready" || r.expiration != nil {
		t.Fatal("standby should have no pairing code or expiration")
	}
	if _, err := h.enter(other, message{Type: "join_direct"}); err == nil {
		t.Fatal("unconfigured address joined the direct connection")
	}
	if _, err := h.enter(viewer, message{Type: "join_direct"}); err != nil {
		t.Fatal(err)
	}
	if req := <-host.out; req.Type != "request" || req.Payload != viewerIP.String() {
		t.Fatalf("unexpected request: %+v", req)
	}
	if got := <-viewer.out; got.Type != "waiting" {
		t.Fatalf("unexpected viewer state without approval: %+v", got)
	}
	if h.relay(r, host, message{Type: "offer", Payload: "premature offer"}) {
		t.Fatal("offer admitted before approval")
	}
	if !h.relay(r, host, message{Type: "accept"}) || (<-viewer.out).Type != "accepted" {
		t.Fatal("could not approve viewer")
	}
	h.leave(r, viewer, false)
	if got := <-host.out; got.Type != "ended" || h.direct != nil {
		t.Fatal("direct connection did not clear on disconnect")
	}
}

func TestDirectLANAcceptsDifferentViewersOnTheHostSubnet(t *testing.T) {
	hostIP := netip.MustParseAddr("192.168.0.43")
	h := &hub{directHost: hostIP, directPeer: netip.MustParsePrefix("192.168.0.0/24")}
	for _, addr := range []string{"192.168.0.36", "192.168.0.98"} {
		host := &client{ip: hostIP, out: make(chan message, 8)}
		viewer := &client{ip: netip.MustParseAddr(addr), out: make(chan message, 8)}
		otherSubnet := &client{ip: netip.MustParseAddr("192.168.1.36")}
		r, err := h.enter(host, message{Type: "listen_direct"})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := h.enter(otherSubnet, message{Type: "join_direct"}); err == nil {
			t.Fatal("viewer outside the Host subnet joined")
		}
		if _, err := h.enter(viewer, message{Type: "join_direct"}); err != nil {
			t.Fatalf("viewer %s could not join: %v", viewer.ip, err)
		}
		h.leave(r, viewer, false)
	}
}

func TestRelayRequiresHostApproval(t *testing.T) {
	for _, approved := range []bool{false, true} {
		name := "before approval"
		if approved {
			name = "after approval"
		}
		t.Run(name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			h := &hub{rooms: make(map[string]*room)}
			server := httptest.NewServer(http.HandlerFunc(h.serve))
			defer server.Close()
			url := "ws" + strings.TrimPrefix(server.URL, "http")

			open := func(m message) *websocket.Conn {
				t.Helper()
				c, _, err := websocket.Dial(ctx, url, nil)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { _ = c.CloseNow() })
				if err := write(ctx, c, m); err != nil {
					t.Fatal(err)
				}
				return c
			}
			expect := func(c *websocket.Conn, kind string) message {
				t.Helper()
				got, err := read(ctx, c)
				if err != nil || got.Type != kind {
					t.Fatalf("expected %s, got %s (%v)", kind, got.Type, err)
				}
				return got
			}
			send := func(c *websocket.Conn, kind, payload string) {
				t.Helper()
				if err := write(ctx, c, message{Type: kind, Payload: payload}); err != nil {
					t.Fatal(err)
				}
			}

			host := open(message{Type: "create"})
			code := expect(host, "room").Payload
			viewer := open(message{Type: "join", Payload: code})
			expect(viewer, "waiting")
			expect(host, "request")
			expect(open(message{Type: "join", Payload: code}), "error")
			if !approved {
				send(host, "offer", "premature offer")
				expect(viewer, "ended")
				return
			}
			send(host, "accept", "")
			expect(viewer, "accepted")
			send(host, "offer", "offer SDP")
			if expect(viewer, "offer").Payload != "offer SDP" {
				t.Fatal("offer payload changed")
			}
			send(viewer, "answer", "answer SDP")
			if expect(host, "answer").Payload != "answer SDP" {
				t.Fatal("answer payload changed")
			}
			_ = viewer.CloseNow()
			expect(host, "ended")
			expect(open(message{Type: "join", Payload: code}), "error")
		})
	}
}
