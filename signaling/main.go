package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"log"
	"net/http"
	"net/netip"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

const (
	messageLimit = 256 * 1024
	roomLifetime = 2 * time.Minute
)

type message struct {
	Type      string `json:"type"`
	Payload   string `json:"payload"`
	Retryable bool   `json:"retryable,omitempty"`
}

var (
	errDirectUnavailable = errors.New("设备未在线或正在使用中")
	errDirectHostInUse   = errors.New("设备已在线")
)

type client struct {
	out      chan message
	cancel   context.CancelFunc
	ip       netip.Addr
	identity *accountSession
}

func (c *client) send(kind string, payload string) {
	c.sendMessage(message{Type: kind, Payload: payload})
}

func (c *client) sendMessage(m message) {
	if c == nil {
		return
	}
	select {
	case c.out <- m:
	default:
		c.cancel()
	}
}

type room struct {
	deviceID     string
	host, viewer *client
	stage        string
	expiration   *time.Timer
	miniJoined   bool
	diagnostics  bool
}

type hub struct {
	mu          sync.Mutex
	managed     *managedService
	deviceHosts map[string]*room
}

func (h *hub) contains(r *room) bool {
	return h.deviceHosts[r.deviceID] == r
}

func (h *hub) remove(r *room) {
	delete(h.deviceHosts, r.deviceID)
	if r.expiration != nil {
		r.expiration.Stop()
	}
}

func (h *hub) startExpiry(r *room) {
	viewer := r.viewer
	r.expiration = time.AfterFunc(roomLifetime, func() {
		h.mu.Lock()
		defer h.mu.Unlock()
		if h.contains(r) && r.viewer == viewer && r.stage == "requested" {
			h.remove(r)
			r.host.send("error", "会话已过期，请重新连接")
			r.viewer.send("error", "会话已过期，请重新连接")
		}
	})
}

func request(r *room, viewer *client) {
	r.viewer, r.stage = viewer, "requested"
	viewer.send("waiting", "")
	kind := "request"
	if r.diagnostics {
		kind = "request_diagnostics"
	}
	r.host.send(kind, viewer.ip.String())
}

func (h *hub) enter(c *client, first message) (*room, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.enterManaged(c, first)
}

func (h *hub) relay(r *room, from *client, m message) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.contains(r) {
		return false
	}
	if r.stage == "requested" {
		if from == r.host && r.diagnostics && m.Type == "reject" {
			r.viewer.send("error", "远端未开启只读诊断")
			r.viewer, r.stage, r.diagnostics = nil, "waiting", false
			if r.expiration != nil {
				r.expiration.Stop()
				r.expiration = nil
			}
			return true
		}
		validMode := m.Payload == "diagnostics" && r.diagnostics ||
			!r.diagnostics && (m.Payload == "control" || m.Payload == "view")
		if from != r.host || m.Type != "accept" || !validMode {
			return false
		}
		r.stage = "accepted"
		if r.expiration != nil {
			r.expiration.Stop()
		}
		r.viewer.send("accepted", m.Payload)
		return true
	}
	return relayMiniRTC(r, from, m)
}

func (h *hub) leave(r *room, c *client, interrupted bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.contains(r) || (c != r.host && c != r.viewer) {
		return
	}
	h.remove(r)
	peer, reason := r.host, "Viewer 已结束会话"
	if c == r.host {
		peer, reason = r.viewer, "Host 已结束会话"
	}
	if interrupted {
		reason = "对端信令连接中断"
	}
	peer.sendMessage(message{Type: "ended", Payload: reason, Retryable: interrupted})
}

func read(ctx context.Context, ws *websocket.Conn) (message, error) {
	var m message
	kind, data, err := ws.Read(ctx)
	if err != nil {
		return m, err
	}
	if kind != websocket.MessageText {
		return m, errors.New("expected a text message")
	}
	err = json.Unmarshal(data, &m)
	return m, err
}

func write(ctx context.Context, ws *websocket.Conn, m message) error {
	data, err := json.Marshal(m)
	if err != nil {
		return err
	}
	bounded, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	return ws.Write(bounded, websocket.MessageText, data)
}

func (h *hub) serve(w http.ResponseWriter, r *http.Request) {
	identity := h.managed.authenticate(r)
	if identity == nil {
		http.Error(w, "Unauthorized", http.StatusUnauthorized)
		return
	}
	ws, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer ws.CloseNow()
	ws.SetReadLimit(messageLimit)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	unwatch := context.AfterFunc(identity.ctx, cancel)
	defer unwatch()

	initial, timeout := context.WithTimeout(ctx, 5*time.Second)
	first, err := read(initial, ws)
	timeout()
	if err != nil {
		return
	}
	address, _ := netip.ParseAddrPort(r.RemoteAddr)
	c := &client{out: make(chan message, 64), cancel: cancel, ip: address.Addr().Unmap(), identity: identity}
	session, err := h.enter(c, first)
	if err != nil {
		retry := errors.Is(err, errDirectUnavailable) || errors.Is(err, errDirectHostInUse)
		_ = write(ctx, ws, message{Type: "error", Payload: err.Error(), Retryable: retry})
		return
	}
	interrupted := false
	defer func() { h.leave(session, c, interrupted) }()

	writerDone := make(chan struct{})
	go func() {
		defer close(writerDone)
		defer cancel()
		ticker := time.NewTicker(4 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				bounded, cancelPing := context.WithTimeout(ctx, 3*time.Second)
				err := ws.Ping(bounded)
				cancelPing()
				if err != nil {
					return
				}
			case next := <-c.out:
				if write(ctx, ws, next) != nil || next.Type == "ended" || next.Type == "error" {
					return
				}
			}
		}
	}()
	defer func() { cancel(); <-writerDone }()

	for {
		m, err := read(ctx, ws)
		if err != nil {
			interrupted = websocket.CloseStatus(err) != websocket.StatusNormalClosure
			return
		}
		if m.Type == "stop" {
			return
		}
		if !h.relay(session, c, m) {
			return
		}
	}
}

func main() {
	bind := flag.String("bind", "127.0.0.1", "IPv4 address to listen on")
	port := flag.Int("port", 8788, "TCP port")
	managedDB := flag.String("managed-db", "", "Persistent accounts and devices database")
	cert := flag.String("tls-cert", "", "HTTPS certificate chain")
	key := flag.String("tls-key", "", "HTTPS private key")
	proxySocket := flag.String("proxy-socket", "", "Private Unix socket for a trusted reverse proxy")
	proxyGID := flag.Int("proxy-gid", -1, "Group ID allowed to connect to the reverse proxy socket")
	turnIP := flag.String("turn-ip", "", "Public IPv4 address advertised by the STUN/TURN service")
	turnBind := flag.String("turn-bind", "", "IPv4 address to listen on for STUN/TURN")
	turnPort := flag.Int("turn-port", 3478, "UDP and TCP port for STUN/TURN")
	stunURLsFlag := flag.String("stun-urls", "", "Optional comma-separated STUN URLs (e.g. stun:103.236.93.69:23969) overriding default STUN")
	relayMin := flag.Int("relay-min", 40000, "First UDP relay port")
	relayMax := flag.Int("relay-max", 40199, "Last UDP relay port")
	flag.Parse()
	if *managedDB == "" {
		log.Fatal("-managed-db is required")
	}
	ip, _ := netip.ParseAddr(*bind)
	relay, _ := netip.ParseAddr(*turnIP)
	relayBind := ip
	if *turnBind != "" {
		relayBind, _ = netip.ParseAddr(*turnBind)
	}
	var stunURLs []string
	for _, item := range strings.Split(*stunURLsFlag, ",") {
		if u := strings.TrimSpace(item); u != "" {
			if !strings.HasPrefix(u, "stun:") {
				log.Fatalf("invalid -stun-urls entry %q: must start with stun:", u)
			}
			stunURLs = append(stunURLs, u)
		}
	}
	log.Fatal(runManaged(managedConfig{db: *managedDB, cert: *cert, key: *key, bind: ip, port: *port,
		proxySocket: *proxySocket, proxyGID: *proxyGID,
		turnBind: relayBind, turnIP: relay, turnPort: *turnPort, relayMin: *relayMin, relayMax: *relayMax,
		stunURLs: stunURLs}))
}
