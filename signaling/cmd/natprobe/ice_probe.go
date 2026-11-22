package main

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"net"
	"net/netip"

	"github.com/pion/stun/v3"
)

// Optional STUN attribute carrying this tool's session IDs and sequence number.
// The probe is for comparing packet formats between two copies of this tool.
const probeAttribute stun.AttrType = 0xc0f0

type probeWire int

const (
	rawWire probeWire = iota
	stunWire
	probeWireCount
)

var wireNames = [probeWireCount]string{"普通 UDP", "ICE 格式 STUN"}

func probeTransaction(packet probePacket, from, to [idSize]byte) [stun.TransactionIDSize]byte {
	if packet.kind == probeReply {
		from = to
	}
	var id [stun.TransactionIDSize]byte
	copy(id[:8], from[:8])
	binary.BigEndian.PutUint32(id[8:], packet.seq)
	return id
}

func probeIntegrity(packet probePacket, from, to [idSize]byte) stun.MessageIntegrity {
	if packet.kind == probeReply {
		to = from
	}
	return stun.NewShortTermIntegrity(base64.RawURLEncoding.EncodeToString(to[:]))
}

func (wire probeWire) encode(packet probePacket, from, to [idSize]byte, observed netip.AddrPort) ([]byte, error) {
	data := packet.encode(from, to)
	if wire == rawWire {
		return data[:], nil
	}
	transaction := &stun.Message{TransactionID: probeTransaction(packet, from, to)}
	attrs := []stun.Setter{transaction}
	if packet.kind == probeRequest {
		role := stun.AttrICEControlled
		if bytes.Compare(from[:], to[:]) > 0 {
			role = stun.AttrICEControlling
		}
		attrs = append(attrs, stun.BindingRequest,
			stun.NewUsername(hex.EncodeToString(to[:4])+":"+hex.EncodeToString(from[:4])),
			stun.RawAttribute{Type: stun.AttrPriority, Value: binary.BigEndian.AppendUint32(nil, 0x6effffff)},
			stun.RawAttribute{Type: role, Value: from[:8]})
	} else {
		attrs = append(attrs, stun.BindingSuccess, &stun.XORMappedAddress{IP: net.IP(observed.Addr().AsSlice()), Port: int(observed.Port())})
	}
	attrs = append(attrs, stun.RawAttribute{Type: probeAttribute, Value: data[:]}, probeIntegrity(packet, from, to), stun.Fingerprint)
	message, err := stun.Build(attrs...)
	if err != nil {
		return nil, err
	}
	return message.Raw, nil
}

func decodeWireProbe(data []byte, from, to [idSize]byte) (probePacket, probeWire, netip.AddrPort, bool) {
	if !stun.IsMessage(data) {
		packet, ok := decodeProbe(data, from, to)
		return packet, rawWire, netip.AddrPort{}, ok
	}
	var message stun.Message
	if stun.Decode(data, &message) != nil {
		return probePacket{}, stunWire, netip.AddrPort{}, false
	}
	attribute, err := message.Get(probeAttribute)
	if err != nil {
		return probePacket{}, stunWire, netip.AddrPort{}, false
	}
	packet, ok := decodeProbe(attribute, from, to)
	if !ok || message.TransactionID != probeTransaction(packet, from, to) ||
		probeIntegrity(packet, from, to).Check(&message) != nil || stun.Fingerprint.Check(&message) != nil {
		return probePacket{}, stunWire, netip.AddrPort{}, false
	}
	if packet.kind == probeRequest {
		return packet, stunWire, netip.AddrPort{}, message.Type == stun.BindingRequest
	}
	observed, err := mappedEndpoint(&message)
	return packet, stunWire, observed, err == nil && message.Type == stun.BindingSuccess
}
