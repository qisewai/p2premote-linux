package main

import (
	"net"
	"net/netip"
	"testing"
	"time"

	"github.com/pion/stun/v3"
)

func TestSTUNAndDirectProbeUseSameSocket(t *testing.T) {
	listen := func() *net.UDPConn {
		t.Helper()
		conn, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.MustParseAddrPort("127.0.0.1:0")))
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { conn.Close() })
		return conn
	}
	server := listen()
	if err := server.SetReadDeadline(time.Now().Add(4 * time.Second)); err != nil {
		t.Fatal(err)
	}
	serverResult := make(chan error, 1)
	go func() {
		var buf [512]byte
		for range 2 {
			n, from, err := server.ReadFromUDP(buf[:])
			if err != nil {
				serverResult <- err
				return
			}
			var request stun.Message
			if err := stun.Decode(buf[:n], &request); err != nil {
				serverResult <- err
				return
			}
			mapped := stun.XORMappedAddress{IP: from.IP, Port: from.Port}
			reply, err := stun.Build(&request, stun.BindingSuccess, &mapped)
			if err != nil {
				serverResult <- err
				return
			}
			if _, err := server.WriteToUDP(reply.Raw, from); err != nil {
				serverResult <- err
				return
			}
		}
		serverResult <- nil
	}()

	clients := [2]*net.UDPConn{listen(), listen()}
	var offers [2]offer
	for i, client := range clients {
		local := client.LocalAddr().(*net.UDPAddr).AddrPort()
		mapped, _, err := stunMapping(client, server.LocalAddr().(*net.UDPAddr).AddrPort())
		if err != nil || mapped != local {
			t.Fatalf("STUN mapping = %v, error = %v, want %v", mapped, err, local)
		}
		published := offer{id: [idSize]byte{byte(i + 1)}, endpoints: []netip.AddrPort{mapped}}
		offers[i], err = decodeOffer(published.encode())
		if err != nil || offers[i].id != published.id || offers[i].endpoints[0] != mapped {
			t.Fatalf("connection code did not preserve the socket mapping: %v", err)
		}
	}
	if err := <-serverResult; err != nil {
		t.Fatal(err)
	}
	type outcome struct {
		results [probeWireCount]probeResult
		err     error
		peer    netip.AddrPort
		local   netip.AddrPort
	}
	results := make(chan outcome, len(clients))
	for i, client := range clients {
		go func() {
			peer := offers[1-i]
			run, err := probe(client, offers[i], peer, time.Second)
			results <- outcome{results: run, err: err, peer: peer.endpoints[0], local: offers[i].endpoints[0]}
		}()
	}
	for range clients {
		got := <-results
		if got.err != nil {
			t.Fatal(got.err)
		}
		for wire, result := range got.results {
			source := result.sources[got.peer]
			if source == nil || source.requests == 0 || source.repliesSent == 0 || result.targets[got.peer].replies == 0 {
				t.Fatalf("%s did not verify both directions with peer %v", wireNames[wire], got.peer)
			}
		}
		observed := got.results[stunWire].observed
		if len(observed) != 1 || observed[got.local] == 0 {
			t.Fatalf("STUN response reported %v, want socket address %v", observed, got.local)
		}
	}
}

func TestProbeRejectsDifferentPair(t *testing.T) {
	x, y, z := [idSize]byte{1}, [idSize]byte{2}, [idSize]byte{3}
	wire := (probePacket{kind: probeRequest, seq: 10}).encode(x, y)
	if _, ok := decodeProbe(wire[:], x, z); ok {
		t.Fatal("accepted another pair's probe")
	}
}
