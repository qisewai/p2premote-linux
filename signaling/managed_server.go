package main

import (
	"crypto/tls"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

type managedConfig struct {
	db, cert, key                      string
	proxySocket                        string
	proxyGID                           int
	bind, turnBind, turnIP             netip.Addr
	port, turnPort, relayMin, relayMax int
	stunURLs                           []string
}

func runManaged(config managedConfig) error {
	if !config.bind.Is4() || config.bind.IsMulticast() || config.port < 1 || config.port > 65535 {
		return errors.New("invalid managed service bind address or port")
	}
	local := config.cert == "" && config.key == ""
	if (local && !config.bind.IsLoopback()) || (config.cert == "") != (config.key == "") {
		return errors.New("managed accounts on a network interface require both -tls-cert and -tls-key")
	}
	if config.proxySocket != "" && (!filepath.IsAbs(config.proxySocket) || config.proxyGID < 0) {
		return errors.New("-proxy-socket requires an absolute path and a nonnegative -proxy-gid")
	}
	m, err := openManagedService(config.db)
	if err != nil {
		return err
	}
	defer m.close()
	m.relay, err = startManagedRelay(m, config.turnBind, config.turnIP, config.turnPort,
		config.relayMin, config.relayMax, config.stunURLs, local)
	if err != nil {
		return err
	}
	defer m.relay.server.Close()
	address := net.JoinHostPort(config.bind.String(), strconv.Itoa(config.port))
	routes := m.routes()
	server := managedHTTPServer(routes)
	server.Addr = address
	server.TLSConfig = &tls.Config{MinVersion: tls.VersionTLS12}
	serve := func() error {
		if local {
			log.Printf("managed test service listening at http://%s", address)
			return server.ListenAndServe()
		}
		log.Printf("managed service listening on %s with HTTPS and WSS", address)
		return server.ListenAndServeTLS(config.cert, config.key)
	}
	if config.proxySocket == "" {
		return serve()
	}
	listener, err := net.Listen("unix", config.proxySocket)
	if err != nil {
		return fmt.Errorf("managed reverse proxy socket: %w", err)
	}
	defer listener.Close()
	if err := os.Chown(config.proxySocket, -1, config.proxyGID); err != nil {
		return fmt.Errorf("set managed reverse proxy socket group: %w", err)
	}
	if err := os.Chmod(config.proxySocket, 0660); err != nil {
		return fmt.Errorf("set managed reverse proxy socket mode: %w", err)
	}
	proxy := managedHTTPServer(managedProxyRequests(routes))
	defer proxy.Close()
	defer server.Close()
	result := make(chan error, 2)
	go func() { result <- serve() }()
	go func() { result <- proxy.Serve(listener) }()
	log.Printf("managed reverse proxy listening on %s", config.proxySocket)
	return <-result
}

func managedHTTPServer(handler http.Handler) *http.Server {
	return &http.Server{
		Handler: handler, ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 16 * 1024,
	}
}

// Only the permission-scoped Unix listener accepts this header. The proxy replaces it with its peer address.
func managedProxyRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		values := r.Header.Values("X-P2PRemote-Client-IP")
		if len(values) != 1 {
			http.Error(w, "invalid proxy client address", http.StatusBadRequest)
			return
		}
		ip, err := netip.ParseAddr(values[0])
		if err != nil || ip.Zone() != "" {
			http.Error(w, "invalid proxy client address", http.StatusBadRequest)
			return
		}
		r.RemoteAddr = netip.AddrPortFrom(ip.Unmap(), 0).String()
		next.ServeHTTP(w, r)
	})
}
