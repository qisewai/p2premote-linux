package main

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"strconv"
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
	MiniRTC   bool   `json:"minirtc,omitempty"`
}

var (
	errDirectUnavailable = errors.New("Host 尚未等待直连或正在使用中")
	errDirectHostInUse   = errors.New("已有 Host 在等待直连")
)

type internetRole uint8

const (
	noInternetRole internetRole = iota
	internetHost
	internetViewer
)

type internetKeys struct {
	HostToken   string `json:"hostToken"`
	ViewerToken string `json:"viewerToken"`
}

func loadInternetKeys(path string) (*internetKeys, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	keys := new(internetKeys)
	if err := json.Unmarshal(data, keys); err != nil {
		return nil, errors.New("invalid internet key file")
	}
	valid := func(token string) bool {
		bytes, err := base64.RawURLEncoding.Strict().DecodeString(token)
		return err == nil && len(bytes) == 32
	}
	if !valid(keys.HostToken) || !valid(keys.ViewerToken) || keys.HostToken == keys.ViewerToken {
		return nil, errors.New("internet keys must be distinct 32-byte base64url tokens")
	}
	return keys, nil
}

func (k *internetKeys) role(authorization string) internetRole {
	token, bearer := strings.CutPrefix(authorization, "Bearer ")
	if !bearer {
		return noInternetRole
	}
	if subtle.ConstantTimeCompare([]byte(token), []byte(k.HostToken)) == 1 {
		return internetHost
	}
	if subtle.ConstantTimeCompare([]byte(token), []byte(k.ViewerToken)) == 1 {
		return internetViewer
	}
	return noInternetRole
}

type client struct {
	out      chan message
	cancel   context.CancelFunc
	ip       netip.Addr
	role     internetRole
	identity *accountSession
	miniRTC  bool
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
	code         string
	deviceID     string
	host, viewer *client
	stage        string
	expiration   *time.Timer
	acceptedAt   time.Time
	hostICE      iceCandidateCounts
	viewerICE    iceCandidateCounts
	miniRTC      bool
	miniJoined   bool
}

type iceCandidateCounts struct {
	host, srflx, relay, other int
}

func (c *iceCandidateCounts) addTrickle(payload string) {
	_, rest, _ := strings.Cut(payload, "\n")
	_, candidate, _ := strings.Cut(rest, "\n")
	fields := strings.Fields(candidate)
	if len(fields) < 8 || !strings.HasPrefix(fields[0], "candidate:") || fields[6] != "typ" {
		c.other++
		return
	}
	switch fields[7] {
	case "host":
		c.host++
	case "srflx":
		c.srflx++
	case "relay":
		c.relay++
	default:
		c.other++
	}
}

type hub struct {
	mu          sync.Mutex
	rooms       map[string]*room
	direct      *room
	directHost  netip.Addr
	directPeer  netip.Prefix
	internet    *internetKeys
	managed     *managedService
	deviceHosts map[string]*room
}

func (h *hub) contains(r *room) bool {
	if r.deviceID != "" {
		return h.deviceHosts[r.deviceID] == r
	}
	if r.code == "" {
		return h.direct == r
	}
	return h.rooms[r.code] == r
}

func (h *hub) remove(r *room) {
	if r.deviceID != "" {
		delete(h.deviceHosts, r.deviceID)
	} else if r.code == "" {
		h.direct = nil
	} else {
		delete(h.rooms, r.code)
	}
	if r.expiration != nil {
		r.expiration.Stop()
	}
}

func (h *hub) startExpiry(r *room) {
	r.expiration = time.AfterFunc(roomLifetime, func() {
		h.mu.Lock()
		defer h.mu.Unlock()
		if h.contains(r) && (r.stage == "waiting" || r.stage == "requested") {
			h.remove(r)
			r.host.send("error", "会话已过期，请重新创建")
			r.viewer.send("error", "会话已过期，请重新连接")
		}
	})
}

func request(r *room, viewer *client) {
	r.viewer, r.stage = viewer, "requested"
	viewer.send("waiting", "")
	r.host.sendMessage(message{Type: "request", Payload: viewer.ip.String(),
		MiniRTC: r.deviceID != "" && r.host.miniRTC && viewer.miniRTC})
}

func (h *hub) enter(c *client, first message) (*room, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.managed != nil {
		return h.enterManaged(c, first)
	}
	if h.internet != nil {
		if !(c.role == internetHost && first.Type == "listen_internet") &&
			!(c.role == internetViewer && first.Type == "join_internet") {
			return nil, errors.New("公网访问密钥与连接角色不匹配")
		}
	}
	switch first.Type {
	case "listen_direct", "listen_internet":
		if first.Type == "listen_internet" && h.internet == nil {
			return nil, errors.New("此信令服务未启用公网模式")
		}
		if first.Type == "listen_direct" && (!h.directHost.IsValid() || c.ip != h.directHost) {
			return nil, errors.New("当前地址不允许开启局域网直连")
		}
		if h.direct != nil {
			return nil, errDirectHostInUse
		}
		h.direct = &room{host: c, stage: "waiting"}
		c.send("ready", "")
		return h.direct, nil
	case "join_direct", "join_internet":
		if first.Type == "join_internet" && h.internet == nil {
			return nil, errors.New("此信令服务未启用公网模式")
		}
		if first.Type == "join_direct" && (!h.directPeer.Contains(c.ip) || !privateLANAddress(c.ip)) {
			return nil, errors.New("当前地址未配置为直连主控机")
		}
		if h.direct == nil || h.direct.stage != "waiting" {
			return nil, errDirectUnavailable
		}
		request(h.direct, c)
		h.startExpiry(h.direct)
		return h.direct, nil
	case "create":
		var random [6]byte
		for {
			if _, err := rand.Read(random[:]); err != nil {
				return nil, err
			}
			code := strings.ToUpper(hex.EncodeToString(random[:]))
			if h.rooms[code] != nil {
				continue
			}
			r := &room{code: code, host: c, stage: "waiting"}
			h.rooms[code] = r
			h.startExpiry(r)
			c.send("room", code)
			return r, nil
		}
	case "join":
		r := h.rooms[strings.ToUpper(first.Payload)]
		if r == nil || r.stage != "waiting" {
			return nil, errors.New("会话代码无效或已被使用")
		}
		request(r, c)
		return r, nil
	default:
		return nil, errors.New("需要先创建或加入会话")
	}
}

func (h *hub) relay(r *room, from *client, m message) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.contains(r) {
		return false
	}
	if r.miniRTC {
		return relayMiniRTC(r, from, m)
	}
	switch {
	case from == r.host && r.stage == "requested" && m.Type == "accept":
		if m.MiniRTC && (r.deviceID == "" || !r.host.miniRTC || !r.viewer.miniRTC ||
			(m.Payload != "control" && m.Payload != "view")) {
			return false
		}
		r.miniRTC = m.MiniRTC
		r.stage = "accepted"
		r.expiration.Stop()
		if h.internet != nil {
			r.acceptedAt = time.Now()
		}
		accepted := message{Type: "accepted", MiniRTC: r.miniRTC}
		if r.miniRTC {
			accepted.Payload = m.Payload
		}
		r.viewer.sendMessage(accepted)
	case from == r.host && r.stage == "accepted" && m.Type == "offer" && m.Payload != "":
		r.stage = "offered"
		if h.internet != nil {
			log.Printf("public offer candidate lines=%d relay=%d", strings.Count(m.Payload, "a=candidate:"), strings.Count(m.Payload, " typ relay"))
		}
		r.viewer.send(m.Type, m.Payload)
	case from == r.viewer && r.stage == "offered" && m.Type == "answer" && m.Payload != "":
		r.stage = "answered"
		if h.internet != nil {
			log.Printf("public answer candidate lines=%d relay=%d", strings.Count(m.Payload, "a=candidate:"), strings.Count(m.Payload, " typ relay"))
		}
		r.host.send(m.Type, m.Payload)
	case (h.internet != nil || h.managed != nil) && from == r.host && (r.stage == "offered" || r.stage == "answered") &&
		m.Type == "ice" && len(m.Payload) > 0 && len(m.Payload) <= 4096:
		r.hostICE.addTrickle(m.Payload)
		r.viewer.send(m.Type, m.Payload)
	case (h.internet != nil || h.managed != nil) && from == r.viewer && r.stage == "answered" &&
		m.Type == "ice" && len(m.Payload) > 0 && len(m.Payload) <= 4096:
		r.viewerICE.addTrickle(m.Payload)
		r.host.send(m.Type, m.Payload)
	default:
		return false
	}
	return true
}

func (h *hub) leave(r *room, c *client, interrupted bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.contains(r) {
		return
	}
	h.remove(r)
	if !r.acceptedAt.IsZero() {
		endedBy := "viewer"
		if c == r.host {
			endedBy = "host"
		}
		log.Printf("public session ended_by=%s stage=%s since_approval=%s trickle_host=%+v trickle_viewer=%+v",
			endedBy, r.stage, time.Since(r.acceptedAt).Round(time.Millisecond), r.hostICE, r.viewerICE)
	}
	retry := interrupted && r.code == "" && h.internet == nil
	peer, reason := r.host, "Viewer 已结束会话"
	if c == r.host {
		peer, reason = r.viewer, "Host 已结束会话"
	}
	if retry {
		reason = "对端信令连接中断"
	}
	peer.sendMessage(message{Type: "ended", Payload: reason, Retryable: retry})
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
	var role internetRole
	var identity *accountSession
	if h.managed != nil {
		identity = h.managed.authenticate(r)
		if identity == nil {
			http.Error(w, "Unauthorized", http.StatusUnauthorized)
			return
		}
	}
	if h.internet != nil {
		role = h.internet.role(r.Header.Get("Authorization"))
		if role == noInternetRole {
			http.Error(w, "Unauthorized", http.StatusUnauthorized)
			return
		}
	}
	ws, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer ws.CloseNow()
	ws.SetReadLimit(messageLimit)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if identity != nil {
		unwatch := context.AfterFunc(identity.ctx, cancel)
		defer unwatch()
	}
	initial, timeout := context.WithTimeout(ctx, 5*time.Second)
	first, err := read(initial, ws)
	timeout()
	if err != nil {
		return
	}
	address, _ := netip.ParseAddrPort(r.RemoteAddr)
	c := &client{out: make(chan message, 64), cancel: cancel, ip: address.Addr().Unmap(), role: role,
		identity: identity, miniRTC: identity != nil && first.MiniRTC}
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
		var ping <-chan time.Time
		pingTimeout := 3 * time.Second
		if session.code == "" {
			interval := 4 * time.Second
			if h.internet != nil {
				interval, pingTimeout = 30*time.Second, 5*time.Second
			}
			ticker := time.NewTicker(interval)
			defer ticker.Stop()
			ping = ticker.C
		}
		for {
			select {
			case <-ctx.Done():
				return
			case <-ping:
				bounded, cancelPing := context.WithTimeout(ctx, pingTimeout)
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

func privateLANAddress(ip netip.Addr) bool {
	return ip.Is4() && (ip.IsPrivate() || ip.IsLinkLocalUnicast())
}

func directLANPrefix(ip netip.Addr) (netip.Prefix, error) {
	if !privateLANAddress(ip) {
		return netip.Prefix{}, errors.New("-direct-lan requires a private LAN IPv4 address for -bind")
	}
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return netip.Prefix{}, err
	}
	for _, addr := range addrs {
		prefix, err := netip.ParsePrefix(addr.String())
		if err == nil && prefix.Addr() == ip {
			return prefix.Masked(), nil
		}
	}
	return netip.Prefix{}, errors.New("could not determine the subnet for -bind")
}

func main() {
	bind := flag.String("bind", "127.0.0.1", "IPv4 address to listen on")
	port := flag.Int("port", 8787, "TCP port")
	directPeer := flag.String("direct-peer", "", "LAN IPv4 address allowed to request a direct connection")
	directLAN := flag.Bool("direct-lan", false, "Allow direct Viewer requests from private addresses on the -bind subnet")
	internetKeysFile := flag.String("internet-keys", "", "JSON file containing Internet Host and Viewer access keys")
	managedDB := flag.String("managed-db", "", "Persistent accounts and devices database")
	cert := flag.String("tls-cert", "", "HTTPS certificate chain for managed mode")
	key := flag.String("tls-key", "", "HTTPS private key for managed mode")
	proxySocket := flag.String("proxy-socket", "", "Private Unix socket for a trusted reverse proxy in managed mode")
	proxyGID := flag.Int("proxy-gid", -1, "Group ID allowed to connect to the managed reverse proxy socket")
	turnIP := flag.String("turn-ip", "", "Public IPv4 address advertised by the STUN/TURN service")
	turnBind := flag.String("turn-bind", "", "IPv4 address to listen on for STUN/TURN")
	turnPort := flag.Int("turn-port", 3478, "UDP and TCP port for STUN/TURN")
	stunURLsFlag := flag.String("stun-urls", "", "Optional comma-separated STUN URLs (e.g. stun:103.236.93.69:23969) overriding default STUN")
	relayMin := flag.Int("relay-min", 40000, "First UDP relay port")
	relayMax := flag.Int("relay-max", 40199, "Last UDP relay port")
	flag.Parse()
	if *managedDB != "" {
		if *directLAN || *directPeer != "" || *internetKeysFile != "" {
			log.Fatal("managed mode requires its own listener")
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
	if *proxySocket != "" || *proxyGID != -1 {
		log.Fatal("-proxy-socket and -proxy-gid require managed mode")
	}
	if *directLAN && *directPeer != "" {
		log.Fatal("use either -direct-lan or -direct-peer")
	}
	ip, err := netip.ParseAddr(*bind)
	if err != nil || !ip.Is4() || ip.IsUnspecified() || ip.IsMulticast() || ip.String() == "255.255.255.255" {
		log.Fatal("-bind requires a specific IPv4 address, for example 127.0.0.1 or the Host LAN address")
	}
	address := net.JoinHostPort(ip.String(), strconv.Itoa(*port))
	listener, err := net.Listen("tcp4", address)
	if err != nil {
		log.Fatal(err)
	}
	mux := http.NewServeMux()
	h := &hub{rooms: make(map[string]*room)}
	if *internetKeysFile != "" {
		if !ip.IsLoopback() || *directPeer != "" || *directLAN {
			log.Fatal("-internet-keys requires a loopback -bind behind a TLS tunnel and its own listener")
		}
		h.internet, err = loadInternetKeys(*internetKeysFile)
		if err != nil {
			log.Fatal(err)
		}
		log.Printf("authenticated internet signaling enabled")
	}
	if *directLAN {
		h.directPeer, err = directLANPrefix(ip)
		if err != nil {
			log.Fatal(err)
		}
	} else if *directPeer != "" {
		peer, err := netip.ParseAddr(*directPeer)
		if err != nil || !privateLANAddress(peer) || peer == ip || !privateLANAddress(ip) {
			log.Fatal("-direct-peer requires distinct private LAN IPv4 addresses for -bind and -direct-peer")
		}
		h.directPeer = netip.PrefixFrom(peer, 32)
	}
	if h.directPeer.IsValid() {
		h.directHost = ip
		log.Printf("direct connection requests allowed from %s", h.directPeer)
	}
	mux.HandleFunc("GET /signal", h.serve)
	server := &http.Server{Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	log.Printf("signaling listening on %s", listener.Addr())
	log.Fatal(server.Serve(listener))
}
