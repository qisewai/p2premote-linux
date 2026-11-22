package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
	"github.com/coder/websocket/wsjson"
)

const hostID, viewerID = "host", "viewer"

type iceServer struct {
	URLs       []string `json:"urls"`
	Username   string   `json:"username,omitempty"`
	Credential string   `json:"credential,omitempty"`
	ExpiresAt  int64    `json:"expires_at,omitempty"`
}

type iceConfiguration struct {
	Version   int         `json:"version"`
	ID        string      `json:"id"`
	ExpiresAt int64       `json:"expires_at"`
	Servers   []iceServer `json:"servers"`
}

type message struct {
	Type           string            `json:"type"`
	UserID         string            `json:"user_id,omitempty"`
	RemoteUserID   string            `json:"remote_user_id,omitempty"`
	TransmissionID string            `json:"transmission_id,omitempty"`
	Status         string            `json:"status,omitempty"`
	Reason         string            `json:"reason,omitempty"`
	SDP            string            `json:"sdp,omitempty"`
	Candidate      string            `json:"candidate,omitempty"`
	MID            *string           `json:"mid,omitempty"`
	Ufrag          *string           `json:"ufrag,omitempty"`
	IceVersion     int               `json:"ice_config_version,omitempty"`
	ICE            *iceConfiguration `json:"ice,omitempty"`
}

// A local-only signaling fixture for one pair. The ICE and media sockets are
// owned by MiniRTC on the two test computers.
type probeSignal struct {
	identities map[string]string
	ice        *iceConfiguration
	mu         sync.Mutex
	peers      map[string]*websocket.Conn
	joined     bool
}

func writeMessage(ctx context.Context, conn *websocket.Conn, msg message) error {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	return wsjson.Write(ctx, conn, msg)
}

func (s *probeSignal) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	defer conn.CloseNow()
	conn.SetReadLimit(64 * 1024)
	ctx := r.Context()
	loginCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
	var login message
	err = wsjson.Read(loginCtx, conn, &login)
	cancel()
	if err != nil {
		return
	}
	id, _, _ := strings.Cut(login.UserID, "@")
	identity, known := s.identities[id]
	if !known || login.Type != "login" || login.IceVersion != 1 ||
		subtle.ConstantTimeCompare([]byte(login.UserID), []byte(identity)) != 1 {
		_ = writeMessage(ctx, conn, message{Type: "login", Status: "fail"})
		return
	}
	s.mu.Lock()
	if s.peers[id] != nil {
		s.mu.Unlock()
		_ = writeMessage(ctx, conn, message{Type: "login", Status: "fail"})
		return
	}
	if err := writeMessage(ctx, conn, message{Type: "login", Status: "success", UserID: identity, IceVersion: 1}); err != nil {
		s.mu.Unlock()
		return
	}
	s.peers[id] = conn
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.peers, id)
		s.joined = false
		s.mu.Unlock()
	}()
	log.Printf("%s logged in", id)
	for {
		var msg message
		if err := wsjson.Read(ctx, conn, &msg); err != nil {
			return
		}
		if err := s.route(ctx, id, msg); err != nil {
			log.Printf("%s signaling ended: %v", id, err)
			return
		}
	}
}

func (s *probeSignal) route(ctx context.Context, sender string, msg message) error {
	if msg.UserID != sender || msg.TransmissionID != hostID {
		return errors.New("invalid sender or transmission")
	}
	recipient := hostID
	if sender == hostID {
		recipient = viewerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	other := s.peers[recipient]
	if msg.Type == "join_transmission" {
		if sender != viewerID || s.joined || other == nil || s.ice.ExpiresAt <= time.Now().Unix()+5 {
			return writeMessage(ctx, s.peers[sender], message{
				Type: "user_join_transmission", TransmissionID: hostID, Status: "failed", Reason: "test pair unavailable",
			})
		}
		s.joined = true
		return writeMessage(ctx, other, message{
			Type: "user_join_transmission", TransmissionID: hostID, UserID: sender, Status: "success", ICE: s.ice,
		})
	}
	if !s.joined || other == nil {
		return nil
	}
	reply := message{Type: msg.Type, TransmissionID: hostID, RemoteUserID: sender}
	switch msg.Type {
	case "offer", "answer", "new_candidate", "new_candidate_mid":
		if msg.RemoteUserID != recipient || (msg.Type == "offer" && sender != hostID) ||
			(msg.Type == "answer" && sender != viewerID) {
			return errors.New("invalid signaling target")
		}
		reply.SDP, reply.Candidate, reply.MID, reply.Ufrag = msg.SDP, msg.Candidate, msg.MID, msg.Ufrag
		if msg.Type == "offer" {
			reply.ICE = s.ice
		}
	case "user_leave_transmission", "disconnect_peer":
		s.joined = false
		reply.Type = "user_leave_transmission"
		reply.UserID = sender
	default:
		return nil
	}
	return writeMessage(ctx, other, reply)
}

func sessionCertificate() (tls.Certificate, []byte, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, nil, err
	}
	now := time.Now()
	cert := &x509.Certificate{
		SerialNumber: big.NewInt(1), NotBefore: now.Add(-time.Minute), NotAfter: now.Add(time.Hour),
		IPAddresses: []net.IP{net.IPv4(127, 0, 0, 1)}, DNSNames: []string{"localhost"},
		BasicConstraintsValid: true, IsCA: true,
		KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, cert, cert, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, nil, err
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), nil
}

func run(ctx context.Context, port int, output, stun, turnFile string) error {
	id, err := randomHex(16)
	if err != nil {
		return err
	}
	ice := &iceConfiguration{Version: 1, ID: id, ExpiresAt: time.Now().Add(30 * time.Minute).Unix()}
	stunAddresses := strings.Split(stun, ",")
	if len(stunAddresses) > 8 {
		return errors.New("at most eight STUN addresses are supported")
	}
	stunServer := iceServer{}
	for _, address := range stunAddresses {
		address = strings.TrimSpace(address)
		host, port, err := net.SplitHostPort(address)
		portNumber, portErr := strconv.ParseUint(port, 10, 16)
		if err != nil || host == "" || portErr != nil || portNumber == 0 {
			return fmt.Errorf("invalid STUN host:port %q", address)
		}
		stunServer.URLs = append(stunServer.URLs, "stun:"+address)
	}
	ice.Servers = []iceServer{stunServer}
	if turnFile != "" {
		data, err := os.ReadFile(turnFile)
		if err != nil {
			return err
		}
		var turn iceServer
		if json.Unmarshal(data, &turn) != nil || len(turn.URLs) == 0 || len(turn.URLs) > 8 ||
			turn.Username == "" || turn.Credential == "" || turn.ExpiresAt <= time.Now().Unix()+30 {
			return errors.New("TURN config requires URLs, credentials and a fresh Unix expiry")
		}
		for _, url := range turn.URLs {
			if !strings.HasPrefix(url, "turn:") {
				return errors.New("expected a turn: URL in the TURN config")
			}
		}
		ice.ExpiresAt = min(ice.ExpiresAt, turn.ExpiresAt)
		ice.Servers = append(ice.Servers, turn)
	}
	cert, publicCert, err := sessionCertificate()
	if err != nil {
		return err
	}
	listener, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return err
	}
	defer listener.Close()
	dir, err := os.MkdirTemp(output, "minirtc-")
	if err != nil {
		return err
	}
	identities := map[string]string{}
	for _, id := range []string{hostID, viewerID} {
		secret, err := randomHex(32)
		if err != nil {
			return err
		}
		identities[id] = id + "@" + secret
		if err := os.WriteFile(filepath.Join(dir, id+".txt"), []byte(identities[id]), 0600); err != nil {
			return err
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "ca.pem"), publicCert, 0600); err != nil {
		return err
	}
	s := &probeSignal{identities: identities, ice: ice, peers: make(map[string]*websocket.Conn)}
	mux := http.NewServeMux()
	mux.Handle("GET /signal", s)
	tlsConfig := &tls.Config{Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS12}
	server := &http.Server{
		Handler: mux, ReadHeaderTimeout: 5 * time.Second, TLSConfig: tlsConfig,
		BaseContext: func(net.Listener) context.Context { return ctx },
	}
	stop := context.AfterFunc(ctx, func() { _ = server.Close() })
	defer stop()
	fmt.Printf("SIGNAL=wss://%s/signal\nSTATE_DIR=%s\n", listener.Addr(), dir)
	err = server.Serve(tls.NewListener(listener, tlsConfig))
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func randomHex(size int) (string, error) {
	b := make([]byte, size)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

func main() {
	port := flag.Int("port", 0, "loopback listen port (0 chooses a free port)")
	output := flag.String("out", ".", "existing directory in which to create session files")
	stun := flag.String("stun", "103.236.93.69:23969", "comma-separated STUN host:port addresses")
	turn := flag.String("turn-file", "", "JSON file containing temporary TURN credentials and expiry")
	flag.Parse()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	ctx, cancel := context.WithTimeout(ctx, 30*time.Minute)
	defer cancel()
	if err := run(ctx, *port, *output, *stun, *turn); err != nil {
		log.Fatal(err)
	}
}
