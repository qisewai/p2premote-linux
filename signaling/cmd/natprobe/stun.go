package main

import (
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"runtime"
	"slices"
	"strings"
	"syscall"
	"time"

	"github.com/pion/stun/v3"
)

const maxEndpoints = 4

type stunNode struct {
	name       string
	address    netip.AddrPort
	resolveErr error
}

type mappings struct {
	byServer  map[string]netip.AddrPort
	endpoints []netip.AddrPort
}

func parseSTUNServers(value string) ([]stunNode, error) {
	var nodes []stunNode
	seen := make(map[string]bool)
	for _, name := range strings.Split(value, ",") {
		name = strings.TrimPrefix(strings.TrimSpace(name), "stun:")
		if name == "" || seen[name] {
			continue
		}
		if len(nodes) == maxEndpoints {
			return nil, fmt.Errorf("最多可指定 %d 个 STUN 地址", maxEndpoints)
		}
		seen[name] = true
		address, err := net.ResolveUDPAddr("udp4", name)
		node := stunNode{name: name, resolveErr: err}
		if err == nil {
			node.address = ipv4Endpoint(address.AddrPort())
			if !usableEndpoint(node.address) {
				node.resolveErr = errors.New("需要指定 IPv4 主机及非零端口")
			}
		}
		nodes = append(nodes, node)
	}
	if len(nodes) == 0 {
		return nil, errors.New("请通过 -stun 指定至少一个 STUN 地址")
	}
	return nodes, nil
}

func readMappings(conn *net.UDPConn, nodes []stunNode, output io.Writer) mappings {
	result := mappings{byServer: make(map[string]netip.AddrPort)}
	for _, node := range nodes {
		if node.resolveErr != nil {
			fmt.Fprintf(output, "  %s 失败 (%v)\n", node.name, node.resolveErr)
			continue
		}
		mapped, elapsed, err := stunMapping(conn, node.address)
		if err != nil {
			fmt.Fprintf(output, "  %s 失败 (%v)\n", node.name, err)
			continue
		}
		fmt.Fprintf(output, "  %s → %s，STUN RTT %s\n", node.name, mapped, elapsed.Round(time.Millisecond))
		result.byServer[node.name] = mapped
		if !slices.Contains(result.endpoints, mapped) {
			result.endpoints = append(result.endpoints, mapped)
		}
	}
	if len(result.byServer) > 1 {
		if len(result.endpoints) == 1 {
			fmt.Fprintln(output, "  本轮收到回复的 STUN 地址看到相同的公网映射。")
		} else {
			fmt.Fprintln(output, "  本轮至少两个 STUN 地址看到不同的公网映射。")
		}
	}
	return result
}

func stunMapping(conn *net.UDPConn, server netip.AddrPort) (netip.AddrPort, time.Duration, error) {
	var buf [2048]byte
	for attempt := 0; attempt < 2; attempt++ {
		request, err := stun.Build(stun.TransactionID, stun.BindingRequest)
		if err != nil {
			return netip.AddrPort{}, 0, err
		}
		started := time.Now()
		deadline := started.Add(1200 * time.Millisecond)
		if err = conn.SetReadDeadline(deadline); err != nil {
			return netip.AddrPort{}, 0, err
		}
		if _, err = conn.WriteToUDPAddrPort(request.Raw, server); err != nil {
			return netip.AddrPort{}, 0, err
		}
		for time.Now().Before(deadline) {
			n, source, err := conn.ReadFromUDPAddrPort(buf[:])
			if errors.Is(err, os.ErrDeadlineExceeded) {
				break
			}
			if transientUDPError(err) {
				continue
			}
			if err != nil {
				return netip.AddrPort{}, 0, err
			}
			if ipv4Endpoint(source) != server || !stun.IsMessage(buf[:n]) {
				continue
			}
			var response stun.Message
			if stun.Decode(buf[:n], &response) != nil || response.TransactionID != request.TransactionID {
				continue
			}
			if response.Type != stun.BindingSuccess {
				return netip.AddrPort{}, 0, fmt.Errorf("STUN 返回 %s", response.Type)
			}
			mapped, err := mappedEndpoint(&response)
			return mapped, time.Since(started), err
		}
	}
	return netip.AddrPort{}, 0, errors.New("等待 STUN 回复超时")
}

func mappedEndpoint(response *stun.Message) (netip.AddrPort, error) {
	var xor stun.XORMappedAddress
	var ip net.IP
	var port int
	if xor.GetFrom(response) == nil {
		ip, port = xor.IP, xor.Port
	} else {
		var mapped stun.MappedAddress
		if mapped.GetFrom(response) == nil {
			ip, port = mapped.IP, mapped.Port
		}
	}
	if addr, ok := netip.AddrFromSlice(ip); ok && port > 0 && port <= 65535 {
		endpoint := netip.AddrPortFrom(addr.Unmap(), uint16(port))
		if usableEndpoint(endpoint) {
			return endpoint, nil
		}
	}
	return netip.AddrPort{}, errors.New("STUN 回复中没有可用的 IPv4 映射")
}

func refreshWhileWaiting(conn *net.UDPConn, nodes []stunNode, advertised map[string]netip.AddrPort) func() {
	stop, stopped := make(chan struct{}), make(chan struct{})
	go func() {
		defer close(stopped)
		ticker := time.NewTicker(8 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-ticker.C:
				request, err := stun.Build(stun.TransactionID, stun.BindingRequest)
				if err != nil {
					continue
				}
				for _, node := range nodes {
					if advertised[node.name].IsValid() {
						_, _ = conn.WriteToUDPAddrPort(request.Raw, node.address)
					}
				}
			}
		}
	}()
	return func() {
		close(stop)
		<-stopped
	}
}

func ipv4Endpoint(endpoint netip.AddrPort) netip.AddrPort {
	return netip.AddrPortFrom(endpoint.Addr().Unmap(), endpoint.Port())
}

func usableEndpoint(endpoint netip.AddrPort) bool {
	address := endpoint.Addr()
	return endpoint.Port() != 0 && address.Is4() &&
		(address.IsGlobalUnicast() || address.IsLoopback() || address.IsLinkLocalUnicast())
}

func transientUDPError(err error) bool {
	// Windows can report an ICMP port-unreachable as WSAECONNRESET on an unconnected UDP socket.
	return errors.Is(err, syscall.ECONNREFUSED) || errors.Is(err, syscall.ECONNRESET) ||
		(runtime.GOOS == "windows" && errors.Is(err, syscall.Errno(10054)))
}
