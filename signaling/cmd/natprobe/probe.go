package main

import (
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"slices"
	"strings"
	"time"
)

const (
	offerPrefix   = "udp1."
	probeMagic    = "PPR1"
	probeRequest  = byte(1)
	probeReply    = byte(2)
	idSize        = 16
	probeSize     = len(probeMagic) + 1 + 2*idSize + 4
	probeInterval = 250 * time.Millisecond
)

type offer struct {
	id        [idSize]byte
	endpoints []netip.AddrPort
}

func (o offer) encode() string {
	raw := append([]byte(nil), o.id[:]...)
	for _, endpoint := range o.endpoints {
		ip := endpoint.Addr().As4()
		raw = append(raw, ip[:]...)
		raw = binary.BigEndian.AppendUint16(raw, endpoint.Port())
	}
	return offerPrefix + base64.RawURLEncoding.EncodeToString(raw)
}

func decodeOffer(code string) (offer, error) {
	var result offer
	payload, ok := strings.CutPrefix(code, offerPrefix)
	if !ok || len(payload) > base64.RawURLEncoding.EncodedLen(idSize+maxEndpoints*6) {
		return result, errors.New("连接码格式不正确")
	}
	raw, err := base64.RawURLEncoding.DecodeString(payload)
	if err != nil || len(raw) < idSize+6 || (len(raw)-idSize)%6 != 0 {
		return result, errors.New("连接码格式不正确")
	}
	copy(result.id[:], raw[:idSize])
	for entry := raw[idSize:]; len(entry) != 0; entry = entry[6:] {
		endpoint := netip.AddrPortFrom(netip.AddrFrom4([4]byte(entry[:4])), binary.BigEndian.Uint16(entry[4:6]))
		if !usableEndpoint(endpoint) {
			return offer{}, errors.New("连接码包含无法探测的 IPv4 地址")
		}
		if !slices.Contains(result.endpoints, endpoint) {
			result.endpoints = append(result.endpoints, endpoint)
		}
	}
	return result, nil
}

type probePacket struct {
	kind byte
	seq  uint32
}

func (p probePacket) encode(from, to [idSize]byte) [probeSize]byte {
	var data [probeSize]byte
	copy(data[:], probeMagic)
	data[4] = p.kind
	copy(data[5:5+idSize], from[:])
	copy(data[5+idSize:5+2*idSize], to[:])
	binary.BigEndian.PutUint32(data[probeSize-4:], p.seq)
	return data
}

func decodeProbe(data []byte, from, to [idSize]byte) (probePacket, bool) {
	if len(data) != probeSize || string(data[:4]) != probeMagic ||
		(data[4] != probeRequest && data[4] != probeReply) ||
		[idSize]byte(data[5:5+idSize]) != from || [idSize]byte(data[5+idSize:5+2*idSize]) != to {
		return probePacket{}, false
	}
	return probePacket{kind: data[4], seq: binary.BigEndian.Uint32(data[probeSize-4:])}, true
}

type targetStats struct {
	sent, sendErrors, replies int
	minRTT                    time.Duration
}

type sourceStats struct {
	requests, replies, repliesSent, replyErrors int
}

type probeResult struct {
	targets          map[netip.AddrPort]*targetStats
	sources          map[netip.AddrPort]*sourceStats
	observed         map[netip.AddrPort]int
	repliesBeforeRaw int
	firstSendError   error
}

func (r *probeResult) source(endpoint netip.AddrPort) *sourceStats {
	if r.sources[endpoint] == nil {
		r.sources[endpoint] = &sourceStats{}
	}
	return r.sources[endpoint]
}

func probe(conn *net.UDPConn, own, peer offer, duration time.Duration) ([probeWireCount]probeResult, error) {
	var results [probeWireCount]probeResult
	for i := range results {
		results[i] = probeResult{
			targets:  make(map[netip.AddrPort]*targetStats),
			sources:  make(map[netip.AddrPort]*sourceStats),
			observed: make(map[netip.AddrPort]int),
		}
		for _, target := range peer.endpoints {
			results[i].targets[target] = &targetStats{}
		}
	}
	type sentProbe struct {
		target netip.AddrPort
		sentAt time.Time
		wire   probeWire
	}
	pending := make(map[uint32]sentProbe)
	var seq uint32
	var buffer [2048]byte
	next := time.Now()
	end := next.Add(duration)
	rawStart := next.Add(stunLeadIn(duration))
	var rawSent bool
	for time.Now().Before(end) {
		if !time.Now().Before(next) {
			for _, target := range peer.endpoints {
				for _, wire := range [...]probeWire{stunWire, rawWire} {
					if wire == rawWire && time.Now().Before(rawStart) {
						continue
					}
					seq++
					packet := probePacket{kind: probeRequest, seq: seq}
					data, err := wire.encode(packet, own.id, peer.id, netip.AddrPort{})
					if err != nil {
						return results, err
					}
					sentAt := time.Now()
					result := &results[wire]
					stats := result.targets[target]
					if _, err := conn.WriteToUDPAddrPort(data, target); err != nil {
						stats.sendErrors++
						if result.firstSendError == nil {
							result.firstSendError = err
						}
						continue
					}
					stats.sent++
					rawSent = rawSent || wire == rawWire
					pending[seq] = sentProbe{target: target, sentAt: sentAt, wire: wire}
				}
			}
			next = time.Now().Add(probeInterval)
		}
		deadline := next
		if end.Before(deadline) {
			deadline = end
		}
		if err := conn.SetReadDeadline(deadline); err != nil {
			return results, err
		}
		n, source, err := conn.ReadFromUDPAddrPort(buffer[:])
		if errors.Is(err, os.ErrDeadlineExceeded) || transientUDPError(err) {
			continue
		}
		if err != nil {
			return results, err
		}
		packet, wire, observed, ok := decodeWireProbe(buffer[:n], peer.id, own.id)
		if !ok {
			continue
		}
		source = ipv4Endpoint(source)
		result := &results[wire]
		if packet.kind == probeRequest {
			stats := result.source(source)
			stats.requests++
			reply, err := wire.encode(probePacket{kind: probeReply, seq: packet.seq}, own.id, peer.id, source)
			if err != nil {
				return results, err
			}
			if _, err := conn.WriteToUDPAddrPort(reply, source); err != nil {
				stats.replyErrors++
				if result.firstSendError == nil {
					result.firstSendError = err
				}
			} else {
				stats.repliesSent++
				rawSent = rawSent || wire == rawWire
			}
		} else if request, ok := pending[packet.seq]; ok && request.wire == wire {
			delete(pending, packet.seq)
			result.source(source).replies++
			if observed.IsValid() {
				result.observed[observed]++
				if !rawSent {
					result.repliesBeforeRaw++
				}
			}
			stats := result.targets[request.target]
			stats.replies++
			if rtt := time.Since(request.sentAt); stats.minRTT == 0 || rtt < stats.minRTT {
				stats.minRTT = rtt
			}
		}
	}
	return results, nil
}

func stunLeadIn(duration time.Duration) time.Duration {
	return min(5*time.Second, duration/3)
}

func (r probeResult) print(out io.Writer, advertised, own []netip.AddrPort) {
	var sent, replies int
	for _, target := range advertised {
		stats := r.targets[target]
		sent += stats.sent
		replies += stats.replies
		fmt.Fprintf(out, "  发往 %s：发送 %d，发送错误 %d，收到匹配回复 %d", target, stats.sent, stats.sendErrors, stats.replies)
		if stats.replies > 0 {
			fmt.Fprintf(out, "，最低 UDP RTT %s", stats.minRTT.Round(time.Microsecond))
		}
		fmt.Fprintln(out)
	}
	var sources []netip.AddrPort
	for source := range r.sources {
		sources = append(sources, source)
	}
	slices.SortFunc(sources, netip.AddrPort.Compare)
	for _, source := range sources {
		stats := r.sources[source]
		mapped := "与通告地址一致"
		if !slices.Contains(advertised, source) {
			mapped = "未出现在对端通告地址中"
		}
		fmt.Fprintf(out, "  实际对端源 %s（%s）：收到探测 %d，收到匹配回复 %d，已回复 %d，回复错误 %d\n",
			source, mapped, stats.requests, stats.replies, stats.repliesSent, stats.replyErrors)
	}
	var observed []netip.AddrPort
	for endpoint := range r.observed {
		observed = append(observed, endpoint)
	}
	slices.SortFunc(observed, netip.AddrPort.Compare)
	for _, endpoint := range observed {
		mapped := "与本机通告一致"
		if !slices.Contains(own, endpoint) {
			mapped = "与本机通告不同"
		}
		fmt.Fprintf(out, "  对端回报本机地址 %s（%s），共 %d 次\n", endpoint, mapped, r.observed[endpoint])
	}
	if r.firstSendError != nil {
		fmt.Fprintln(out, "  首个发送错误", r.firstSendError)
	}
	switch {
	case replies > 0:
		fmt.Fprintln(out, "结果：UDP 直连往返成功。")
	case len(sources) > 0:
		fmt.Fprintln(out, "结果：收到了对端探测，未收到对本端探测的回复。")
	case sent > 0:
		fmt.Fprintln(out, "结果：未收到对端探测或回复。请对照另一端报告。")
	default:
		fmt.Fprintln(out, "结果：本端没有成功发送 UDP 探测。")
	}
}
