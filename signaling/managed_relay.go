package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"log"
	"net"
	"net/netip"
	"strconv"

	"github.com/pion/turn/v4"
)

const turnRealm = "p2premote"

type managedIceServer struct {
	URLs       []string `json:"urls"`
	Username   string   `json:"username,omitempty"`
	Credential string   `json:"credential,omitempty"`
}

type managedRelay struct {
	server   *turn.Server
	address  string
	stunURLs []string
	secret   string
}

func (r *managedRelay) password(username string) string {
	mac := hmac.New(sha256.New, []byte(r.secret))
	_, _ = mac.Write([]byte(username))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func (r *managedRelay) iceServers(s *accountSession) []managedIceServer {
	stun := r.stunURLs
	if len(stun) == 0 {
		stun = []string{"stun:" + r.address}
	}
	return []managedIceServer{
		{URLs: stun},
		{URLs: []string{"turn:" + r.address + "?transport=udp", "turn:" + r.address + "?transport=tcp"},
			Username: s.digest, Credential: r.password(s.digest)},
	}
}

var nonPublicRelayNetworks = []netip.Prefix{
	netip.MustParsePrefix("0.0.0.0/8"),
	netip.MustParsePrefix("100.64.0.0/10"),
	netip.MustParsePrefix("192.0.0.0/24"),
	netip.MustParsePrefix("192.0.2.0/24"),
	netip.MustParsePrefix("198.18.0.0/15"),
	netip.MustParsePrefix("198.51.100.0/24"),
	netip.MustParsePrefix("203.0.113.0/24"),
	netip.MustParsePrefix("240.0.0.0/4"),
}

func publicRelayIP(ip netip.Addr) bool {
	if !ip.Is4() || !ip.IsGlobalUnicast() || ip.IsPrivate() {
		return false
	}
	for _, reserved := range nonPublicRelayNetworks {
		if reserved.Contains(ip) {
			return false
		}
	}
	return true
}

func startManagedRelay(m *managedService, bind, advertise netip.Addr, port, minPort, maxPort int,
	stunURLs []string, local bool) (*managedRelay, error) {
	if !bind.Is4() || !advertise.Is4() || (local && (bind != advertise || (!bind.IsLoopback() && !bind.IsPrivate()))) ||
		(!local && !publicRelayIP(advertise)) {
		return nil, errors.New("TURN requires a public -turn-ip, or matching local IPv4 addresses in test mode")
	}
	if port < 1 || port > 65535 || minPort < 1024 || maxPort > 65535 || minPort > maxPort ||
		(port >= minPort && port <= maxPort) {
		return nil, errors.New("invalid TURN listening port or relay port range")
	}
	secret, err := makeToken(32)
	if err != nil {
		return nil, err
	}
	endpoint := net.JoinHostPort(bind.String(), strconv.Itoa(port))
	udp, err := net.ListenPacket("udp4", endpoint)
	if err != nil {
		return nil, err
	}
	tcp, err := net.Listen("tcp4", endpoint)
	if err != nil {
		_ = udp.Close()
		return nil, err
	}
	r := &managedRelay{address: net.JoinHostPort(advertise.String(), strconv.Itoa(port)), stunURLs: stunURLs, secret: secret}
	addressGenerator := func() *turn.RelayAddressGeneratorPortRange {
		return &turn.RelayAddressGeneratorPortRange{
			RelayAddress: net.IP(advertise.AsSlice()), Address: bind.String(),
			MinPort: uint16(minPort), MaxPort: uint16(maxPort),
		}
	}
	permit := func(_ net.Addr, address net.IP) bool {
		ip, ok := netip.AddrFromSlice(address)
		if !ok {
			return false
		}
		ip = ip.Unmap()
		if local {
			return ip == advertise
		}
		return publicRelayIP(ip)
	}
	r.server, err = turn.NewServer(turn.ServerConfig{
		Realm: turnRealm,
		AuthHandler: func(username, realm string, _ net.Addr) ([]byte, bool) {
			if realm != turnRealm || m.sessionByDigest(username) == nil {
				return nil, false
			}
			return turn.GenerateAuthKey(username, realm, r.password(username)), true
		},
		PacketConnConfigs: []turn.PacketConnConfig{
			{PacketConn: udp, RelayAddressGenerator: addressGenerator(), PermissionHandler: permit},
		},
		ListenerConfigs: []turn.ListenerConfig{
			{Listener: tcp, RelayAddressGenerator: addressGenerator(), PermissionHandler: permit},
		},
	})
	if err != nil {
		_ = udp.Close()
		_ = tcp.Close()
		return nil, err
	}
	log.Printf("STUN/TURN listening on %s UDP+TCP, advertising %s with UDP relay ports %d-%d",
		endpoint, r.address, minPort, maxPort)
	return r, nil
}
