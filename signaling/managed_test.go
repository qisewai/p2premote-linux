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
	return accountRequest{account, "local test password", installation, device, "Windows x64"}
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
		Token  string      `json:"accessToken"`
		Device savedDevice `json:"device"`
	}
	call := func(method, path, token string, body any, want int, response any) {
		t.Helper()
		var buffer bytes.Buffer
		if body != nil {
			if err := json.NewEncoder(&buffer).Encode(body); err != nil {
				t.Fatal(err)
			}
		}
		req, err := http.NewRequestWithContext(ctx, method, server.URL+"/api/v1/"+path, &buffer)
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
	host := open(a.Token, "listen_internet", "")
	expect(host, "ready")
	expect(open(stranger.Token, "join_internet", a.Device.ID), "error")
	var visible []state
	call("GET", "devices", stranger.Token, nil, 200, &visible)
	if len(visible) != 1 || visible[0].ID != stranger.Device.ID {
		t.Fatalf("other account can see owner device: %+v", visible)
	}
	// The same device can maintain presence while acting as Viewer.
	expect(open(b.Token, "listen_internet", ""), "ready")
	viewer := open(b.Token, "join_internet", a.Device.ID)
	expect(viewer, "waiting")
	expect(host, "request")
	call("GET", "devices", b.Token, nil, 200, &visible)
	states := map[string]string{}
	for _, d := range visible {
		states[d.ID] = d.State
	}
	if len(states) != 2 || states[a.Device.ID] != "busy" || states[b.Device.ID] != "online" {
		t.Fatalf("unexpected presence: %+v", states)
	}
	if err := write(ctx, host, message{Type: "accept"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "accepted")
	if err := write(ctx, host, message{Type: "offer", Payload: "SDP"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "offer")
	if err := write(ctx, host, message{Type: "ice", Payload: "candidate"}); err != nil {
		t.Fatal(err)
	}
	expect(viewer, "ice")
	call("POST", "logout", a.Token, nil, 204, nil)
	expect(viewer, "ended")
	call("GET", "devices", a.Token, nil, 401, nil)
	var again loginResult
	call("POST", "login", "", requestA, 200, &again)
	if again.Device.ID != a.Device.ID || again.Token == a.Token {
		t.Fatal("repeat login duplicated the device or reused its token")
	}
	server.Close()
	m.close()
	reopened, err := openManagedService(db)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.close()
	stored, err := reopened.enroll(requestA, false)
	if err != nil || stored.ID != a.Device.ID {
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
	_, session, err := m.issueSession("owner", "test-device")
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
