package main

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/pion/turn/v4"
)

func testAccount(t *testing.T, account, device string) accountRequest {
	t.Helper()
	installation, err := makeToken(32)
	if err != nil {
		t.Fatal(err)
	}
	return accountRequest{Name: account, Password: "local test password", InstallationID: installation,
		DeviceName: device, Platform: "Windows x64"}
}

func TestManagedProxyRequests(t *testing.T) {
	for _, tc := range []struct {
		names []string
		want  string
	}{
		{[]string{"192.0.2.1"}, "192.0.2.1:0"},
		{[]string{"2001:db8::2"}, "[2001:db8::2]:0"},
		{[]string{"::ffff:192.0.2.1"}, "192.0.2.1:0"},
		{[]string{"fe80::1%eth0"}, ""},
		{[]string{"192.0.2.1", "192.0.2.2"}, ""},
		{[]string{"192.0.2.1, 192.0.2.2"}, ""},
		{nil, ""},
	} {
		req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
		req.Header.Set("X-Forwarded-For", "192.0.2.99")
		for _, name := range tc.names {
			req.Header.Add("X-P2PRemote-Client-IP", name)
		}
		response := httptest.NewRecorder()
		handler := managedProxyRequests(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.RemoteAddr != tc.want {
				t.Errorf("client address for %q = %q, want %q", tc.names, r.RemoteAddr, tc.want)
			}
			w.WriteHeader(http.StatusNoContent)
		}))
		handler.ServeHTTP(response, req)
		want := http.StatusNoContent
		if tc.want == "" {
			want = http.StatusBadRequest
		}
		if response.Code != want {
			t.Errorf("proxy status for %q = %d, want %d", tc.names, response.Code, want)
		}
	}
}

func TestManagedAccountsAndOwnedSignaling(t *testing.T) {
	db := filepath.Join(t.TempDir(), "accounts.db")
	m, err := openManagedService(db)
	if err != nil {
		t.Fatal(err)
	}
	defer m.close()
	server := httptest.NewTLSServer(m.routes())
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	type loginResult struct {
		Token   string `json:"accessToken"`
		Current string `json:"currentDeviceId"`
	}
	call := func(method, path, token string, body any, want int, response any) {
		t.Helper()
		var buffer bytes.Buffer
		if body != nil {
			if err := json.NewEncoder(&buffer).Encode(body); err != nil {
				t.Fatal(err)
			}
		}
		req, err := http.NewRequestWithContext(ctx, method, server.URL+"/api/v2/"+path, &buffer)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Authorization", "Bearer "+token)
		reply, err := server.Client().Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer reply.Body.Close()
		if reply.StatusCode != want {
			t.Fatalf("%s returned %d, want %d", path, reply.StatusCode, want)
		}
		if response != nil {
			if err := json.NewDecoder(reply.Body).Decode(response); err != nil {
				t.Fatal(err)
			}
		}
	}
	requestA := testAccount(t, "owner", "Host")
	requestB := testAccount(t, "owner", "Viewer")
	var a, b, stranger loginResult
	call("POST", "register", "", requestA, 200, &a)
	call("POST", "register", "", requestA, 409, nil)
	call("POST", "login", "", requestB, 200, &b)
	call("POST", "register", "", testAccount(t, "another", "Stranger"), 200, &stranger)
	if a.Current != "" || b.Current != "" || stranger.Current != "" {
		t.Fatal("authentication registered a device")
	}
	call("GET", "devices", "", nil, 401, nil)
	bad := requestA
	bad.Password = "wrong password"
	call("POST", "login", "", bad, 401, nil)
	url := "wss" + strings.TrimPrefix(server.URL, "https") + "/signal"
	open := func(token, kind, payload string) *websocket.Conn {
		t.Helper()
		conn, _, err := websocket.Dial(ctx, url, &websocket.DialOptions{
			HTTPClient: server.Client(), HTTPHeader: http.Header{"Authorization": []string{"Bearer " + token}},
		})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = conn.CloseNow() })
		if err := write(ctx, conn, message{Type: kind, Payload: payload}); err != nil {
			t.Fatal(err)
		}
		return conn
	}
	expect := func(c *websocket.Conn, kind string) message {
		t.Helper()
		got, err := read(ctx, c)
		if err != nil || got.Type != kind {
			t.Fatalf("want %s, got %+v (%v)", kind, got, err)
		}
		return got
	}
	type state struct {
		ID    string `json:"id"`
		State string `json:"state"`
	}
	type deviceList struct {
		Current string  `json:"currentDeviceId"`
		Devices []state `json:"devices"`
	}
	var visible deviceList
	call("GET", "devices", a.Token, nil, 200, &visible)
	if visible.Current != "" || len(visible.Devices) != 0 {
		t.Fatalf("new account has devices: %+v", visible)
	}
	expect(open(a.Token, "listen_internet", ""), "error")
	var addedA, addedB, repeated savedDevice
	call("POST", "devices/current", a.Token, nil, 200, &addedA)
	call("POST", "devices/current", a.Token, nil, 200, &repeated)
	if addedA.ID == "" || addedA != repeated {
		t.Fatalf("manual add is not idempotent: %+v, %+v", addedA, repeated)
	}
	serviceRequest := requestA
	serviceRequest.ServiceDevice = addedA.ID
	var service loginResult
	call("POST", "login", "", serviceRequest, 200, &service)
	if service.Current != addedA.ID {
		t.Fatal("service did not bind to the existing device")
	}
	call("POST", "devices/current", service.Token, nil, 403, nil)
	host := open(service.Token, "listen_internet", "")
	expect(host, "ready")
	expect(open(a.Token, "join_internet", addedA.ID), "error")
	expect(open(stranger.Token, "join_internet", addedA.ID), "error")
	expect(open(b.Token, "join_diagnostics", addedA.ID), "error")
	call("DELETE", "devices/"+addedA.ID, stranger.Token, nil, 204, nil)
	call("GET", "devices", stranger.Token, nil, 200, &visible)
	if len(visible.Devices) != 0 {
		t.Fatalf("other account can see owner device: %+v", visible)
	}
	// An unregistered installation can act as Viewer, then explicitly enroll.
	viewer := open(b.Token, "join_internet", addedA.ID)
	expect(viewer, "waiting")
	expect(host, "request")
	call("POST", "devices/current", b.Token, nil, 200, &addedB)
	expect(open(b.Token, "listen_internet", ""), "ready")
	call("GET", "devices", b.Token, nil, 200, &visible)
	states := map[string]string{}
	for _, d := range visible.Devices {
		states[d.ID] = d.State
	}
	if visible.Current != addedB.ID || len(states) != 2 || states[addedA.ID] != "busy" || states[addedB.ID] != "online" {
		t.Fatalf("unexpected presence: %+v", states)
	}
	if err := write(ctx, host, message{Type: "accept", Payload: "control"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "accepted")
	if err := write(ctx, viewer, message{Type: "minirtc", Payload: `{"type":"join_transmission"}`}); err != nil {
		t.Fatal(err)
	}
	expect(host, "minirtc")
	if err := write(ctx, host, message{Type: "minirtc", Payload: `{"type":"offer","sdp":"v=0"}`}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "minirtc")
	call("DELETE", "devices/"+addedA.ID, b.Token, nil, 204, nil)
	expect(host, "error")
	if ended := expect(viewer, "ended"); ended.Retryable {
		t.Fatal("removed device invited viewer to reconnect")
	}
	expect(open(service.Token, "listen_internet", ""), "error")
	expect(open(a.Token, "listen_internet", ""), "error")
	expect(open(b.Token, "join_internet", addedA.ID), "error")
	call("GET", "devices", a.Token, nil, 200, &visible)
	if visible.Current != "" || len(visible.Devices) != 1 || visible.Devices[0].ID != addedB.ID {
		t.Fatalf("removal did not clear only the target device: %+v", visible)
	}
	call("POST", "login", "", serviceRequest, 403, nil)
	call("POST", "logout", a.Token, nil, 204, nil)
	call("GET", "devices", a.Token, nil, 401, nil)
	var again loginResult
	call("POST", "login", "", requestA, 200, &again)
	if again.Current != "" || again.Token == a.Token {
		t.Fatal("repeat login restored the removed device or reused its token")
	}
	call("POST", "devices/current", again.Token, nil, 200, &repeated)
	if repeated.ID == "" || repeated.ID == addedA.ID {
		t.Fatal("re-added device reused its removed ID")
	}
	interactive := open(again.Token, "listen_internet", "")
	expect(interactive, "ready")
	expect(open(stranger.Token, "join_diagnostics", repeated.ID), "error")
	readOnly := open(b.Token, "join_diagnostics", repeated.ID)
	expect(readOnly, "waiting")
	expect(interactive, "request_diagnostics")
	if err := write(ctx, interactive, message{Type: "accept", Payload: "diagnostics"}); err != nil {
		t.Fatal(err)
	}
	if accepted := expect(readOnly, "accepted"); accepted.Payload != "diagnostics" {
		t.Fatalf("unexpected diagnostic mode: %+v", accepted)
	}
	expect(open(service.Token, "listen_internet", ""), "error")
	server.Close()
	m.close()
	reopened, err := openManagedService(db)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.close()
	stored, err := reopened.currentDevice(requestA.Name, tokenDigest(requestA.InstallationID))
	if err != nil || stored.ID != repeated.ID {
		t.Fatalf("device did not survive server restart: %+v (%v)", stored, err)
	}
}

func TestManagedTURNForwardsUDPAndTCPClients(t *testing.T) {
	m, err := openManagedService(filepath.Join(t.TempDir(), "turn.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.close()
	socket, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := socket.Addr().(*net.TCPAddr).Port
	_ = socket.Close()
	ip := netip.MustParseAddr("127.0.0.1")
	relay, err := startManagedRelay(m, ip, ip, port, 45100, 45199, []string{"stun:103.236.93.69:23969"}, true)
	if err != nil {
		t.Fatal(err)
	}
	defer relay.server.Close()
	_, session, err := m.issueSession(testAccount(t, "owner", "test-device"))
	if err != nil {
		t.Fatal(err)
	}
	servers := relay.iceServers(session)
	if len(servers) < 2 || len(servers[0].URLs) != 1 || servers[0].URLs[0] != "stun:103.236.93.69:23969" {
		t.Fatalf("unexpected custom STUN servers: %+v", servers)
	}
	for _, network := range []string{"udp", "tcp"} {
		t.Run(network, func(t *testing.T) {
			var clientConn net.PacketConn
			if network == "tcp" {
				stream, err := net.DialTimeout("tcp4", relay.address, 2*time.Second)
				if err != nil {
					t.Fatal(err)
				}
				clientConn = turn.NewSTUNConn(stream)
			} else {
				clientConn, err = net.ListenPacket("udp4", "127.0.0.1:0")
				if err != nil {
					t.Fatal(err)
				}
			}
			defer clientConn.Close()
			c, err := turn.NewClient(&turn.ClientConfig{Conn: clientConn, TURNServerAddr: relay.address,
				Username: session.digest, Password: relay.password(session.digest), Realm: turnRealm})
			if err != nil {
				t.Fatal(err)
			}
			defer c.Close()
			if err := c.Listen(); err != nil {
				t.Fatal(err)
			}
			allocated, err := c.Allocate()
			if err != nil {
				t.Fatal(err)
			}
			defer allocated.Close()
			peer, err := net.ListenPacket("udp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer peer.Close()
			_ = peer.SetReadDeadline(time.Now().Add(3 * time.Second))
			_ = allocated.SetReadDeadline(time.Now().Add(3 * time.Second))
			const payload = "test media"
			if _, err := allocated.WriteTo([]byte(payload), peer.LocalAddr()); err != nil {
				t.Fatal(err)
			}
			buffer := make([]byte, 128)
			n, sender, err := peer.ReadFrom(buffer)
			if err != nil || string(buffer[:n]) != payload || sender.String() != allocated.LocalAddr().String() {
				t.Fatalf("relay did not forward to peer: %v %v", sender, err)
			}
			if _, err := peer.WriteTo(buffer[:n], sender); err != nil {
				t.Fatal(err)
			}
			n, _, err = allocated.ReadFrom(buffer)
			if err != nil || string(buffer[:n]) != payload {
				t.Fatalf("relay did not forward reply: %v", err)
			}
		})
	}
}

func TestManagedOfflineOwnedDeviceRetryable(t *testing.T) {
	m, err := openManagedService(filepath.Join(t.TempDir(), "retryable.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.close()
	server := httptest.NewTLSServer(m.routes())
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	type loginResult struct {
		Token string `json:"accessToken"`
	}
	call := func(method, path, token string, body any, want int, response any) {
		t.Helper()
		var buffer bytes.Buffer
		if body != nil {
			if err := json.NewEncoder(&buffer).Encode(body); err != nil {
				t.Fatal(err)
			}
		}
		req, err := http.NewRequestWithContext(ctx, method, server.URL+"/api/v2/"+path, &buffer)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Authorization", "Bearer "+token)
		reply, err := server.Client().Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer reply.Body.Close()
		if reply.StatusCode != want {
			t.Fatalf("%s returned %d, want %d", path, reply.StatusCode, want)
		}
		if response != nil {
			if err := json.NewDecoder(reply.Body).Decode(response); err != nil {
				t.Fatal(err)
			}
		}
	}

	var hostAuth, viewerAuth, strangerAuth loginResult
	call("POST", "register", "", testAccount(t, "owner", "Host"), 200, &hostAuth)
	call("POST", "login", "", testAccount(t, "owner", "Viewer"), 200, &viewerAuth)
	call("POST", "register", "", testAccount(t, "other", "Stranger"), 200, &strangerAuth)

	var hostDevice savedDevice
	call("POST", "devices/current", hostAuth.Token, nil, 200, &hostDevice)

	wsURL := "wss" + strings.TrimPrefix(server.URL, "https") + "/signal"
	joinAndExpectError := func(token string) message {
		t.Helper()
		conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{
			HTTPClient: server.Client(),
			HTTPHeader: http.Header{"Authorization": []string{"Bearer " + token}},
		})
		if err != nil {
			t.Fatal(err)
		}
		defer conn.CloseNow()
		if err := write(ctx, conn, message{Type: "join_internet", Payload: hostDevice.ID}); err != nil {
			t.Fatal(err)
		}
		msg, err := read(ctx, conn)
		if err != nil || msg.Type != "error" {
			t.Fatalf("expected error message, got %+v (%v)", msg, err)
		}
		return msg
	}

	// Host is registered in DB but currently offline: same account may retry.
	if msg := joinAndExpectError(viewerAuth.Token); !msg.Retryable {
		t.Fatalf("offline registered device should be retryable for owner, got %+v", msg)
	}
	// Different account must never see Retryable=true.
	if msg := joinAndExpectError(strangerAuth.Token); msg.Retryable {
		t.Fatalf("offline device must not be retryable for stranger, got %+v", msg)
	}
	// Once deleted from account, even owner must not retry.
	call("DELETE", "devices/"+hostDevice.ID, viewerAuth.Token, nil, 204, nil)
	if msg := joinAndExpectError(viewerAuth.Token); msg.Retryable {
		t.Fatalf("deleted device must not be retryable, got %+v", msg)
	}
}

func TestSignalingPingMissTolerance(t *testing.T) {
	origInterval := signalPingInterval
	origTimeout := signalPingTimeout
	origMaxMisses := signalMaxMisses
	signalPingInterval = 50 * time.Millisecond
	signalPingTimeout = 30 * time.Millisecond
	signalMaxMisses = 3
	t.Cleanup(func() {
		signalPingInterval = origInterval
		signalPingTimeout = origTimeout
		signalMaxMisses = origMaxMisses
	})

	m, err := openManagedService(filepath.Join(t.TempDir(), "ping.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer m.close()
	server := httptest.NewTLSServer(m.routes())
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	type loginResult struct {
		Token string `json:"accessToken"`
	}
	call := func(path, token string, body any, want int, response any) {
		t.Helper()
		var buffer bytes.Buffer
		if body != nil {
			if err := json.NewEncoder(&buffer).Encode(body); err != nil {
				t.Fatal(err)
			}
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, server.URL+"/api/v2/"+path, &buffer)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Authorization", "Bearer "+token)
		reply, err := server.Client().Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer reply.Body.Close()
		if reply.StatusCode != want {
			t.Fatalf("%s returned %d, want %d", path, reply.StatusCode, want)
		}
		if response != nil {
			if err := json.NewDecoder(reply.Body).Decode(response); err != nil {
				t.Fatal(err)
			}
		}
	}

	var hostAuth, viewerAuth loginResult
	call("register", "", testAccount(t, "owner", "Host"), 200, &hostAuth)
	call("login", "", testAccount(t, "owner", "Viewer"), 200, &viewerAuth)
	var hostDevice savedDevice
	call("devices/current", hostAuth.Token, nil, 200, &hostDevice)

	wsURL := "wss" + strings.TrimPrefix(server.URL, "https") + "/signal"
	dial := func(token, kind, payload string) *websocket.Conn {
		t.Helper()
		conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{
			HTTPClient: server.Client(),
			HTTPHeader: http.Header{"Authorization": []string{"Bearer " + token}},
		})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = conn.CloseNow() })
		if err := write(ctx, conn, message{Type: kind, Payload: payload}); err != nil {
			t.Fatal(err)
		}
		return conn
	}

	host := dial(hostAuth.Token, "listen_internet", "")
	if ready, err := read(ctx, host); err != nil || ready.Type != "ready" {
		t.Fatalf("expected ready, got %+v (%v)", ready, err)
	}

	// Pause reading past 1 missed ping (50ms interval + 30ms timeout = 80ms) but < 3 misses.
	time.Sleep(95 * time.Millisecond)

	// Viewer joins now; reading on host drains/responds to pending pings and still receives "request".
	viewer := dial(viewerAuth.Token, "join_internet", hostDevice.ID)
	if waitMsg, err := read(ctx, viewer); err != nil || waitMsg.Type != "waiting" {
		t.Fatalf("expected waiting, got %+v (%v)", waitMsg, err)
	}
	if reqMsg, err := read(ctx, host); err != nil || reqMsg.Type != "request" {
		t.Fatalf("host should survive single missed ping, got %+v (%v)", reqMsg, err)
	}

	// Pause reading past 3 consecutive missed pings (> 3 * 50ms + 30ms).
	time.Sleep(260 * time.Millisecond)
	readCtx, cancelRead := context.WithTimeout(ctx, time.Second)
	defer cancelRead()
	if msg, err := read(readCtx, host); err == nil {
		t.Fatalf("expected connection closed after 3 missed pings, got %+v", msg)
	}
}
