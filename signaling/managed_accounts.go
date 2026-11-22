package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"net/netip"
	"regexp"
	"strings"
	"sync"
	"time"
	"unicode"

	bolt "go.etcd.io/bbolt"
	"golang.org/x/crypto/bcrypt"
)

const accountSessionLifetime = 24 * time.Hour

var (
	accountsBucket  = []byte("accounts")
	devicesBucket   = []byte("devices")
	accountName     = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{2,31}$`)
	errAccountTaken = errors.New("账号已存在")
	errBadLogin     = errors.New("账号或密码不正确")
)

type savedDevice struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Platform string `json:"platform"`
}

type accountSession struct {
	ctx      context.Context
	cancel   context.CancelFunc
	digest   string
	account  string
	deviceID string
}

type managedService struct {
	db        *bolt.DB
	dummyHash []byte
	relay     *managedRelay
	signals   *hub

	mu              sync.Mutex
	sessions        map[string]*accountSession
	byDevice        map[string]*accountSession
	attempts        map[netip.Addr]int
	rateUntil       time.Time
	total           int
	passwordWorkers chan struct{}
}

func openManagedService(path string) (*managedService, error) {
	db, err := bolt.Open(path, 0600, &bolt.Options{Timeout: time.Second})
	if err != nil {
		return nil, err
	}
	err = db.Update(func(tx *bolt.Tx) error {
		for _, name := range [][]byte{accountsBucket, devicesBucket} {
			if _, err := tx.CreateBucketIfNotExists(name); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	dummy, err := bcrypt.GenerateFromPassword([]byte("dummy account password"), bcrypt.DefaultCost)
	if err != nil {
		_ = db.Close()
		return nil, err
	}
	m := &managedService{
		db: db, dummyHash: dummy, sessions: make(map[string]*accountSession),
		byDevice: make(map[string]*accountSession), attempts: make(map[netip.Addr]int),
		passwordWorkers: make(chan struct{}, 4),
	}
	m.signals = &hub{managed: m, deviceHosts: make(map[string]*room)}
	return m, nil
}

func (m *managedService) close() {
	m.mu.Lock()
	for _, session := range m.sessions {
		session.cancel()
	}
	m.mu.Unlock()
	_ = m.db.Close()
}

type accountRequest struct {
	Name           string `json:"username"`
	Password       string `json:"password"`
	InstallationID string `json:"installationId"`
	DeviceName     string `json:"deviceName"`
	Platform       string `json:"platform"`
}

func makeToken(size int) (string, error) {
	data := make([]byte, size)
	if _, err := rand.Read(data); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(data), nil
}

func tokenDigest(token string) string {
	hash := sha256.Sum256([]byte(token))
	return base64.RawURLEncoding.EncodeToString(hash[:])
}

func (m *managedService) issueSession(name, deviceID string) (string, *accountSession, error) {
	token, err := makeToken(32)
	if err != nil {
		return "", nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), accountSessionLifetime)
	s := &accountSession{ctx: ctx, cancel: cancel, digest: tokenDigest(token), account: name, deviceID: deviceID}
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, old := range m.sessions {
		if old.ctx.Err() != nil {
			m.removeSessionLocked(old)
		}
	}
	if old := m.byDevice[deviceID]; old != nil {
		m.removeSessionLocked(old)
	}
	m.sessions[s.digest] = s
	m.byDevice[deviceID] = s
	return token, s, nil
}

func (m *managedService) removeSessionLocked(s *accountSession) {
	s.cancel()
	delete(m.sessions, s.digest)
	if m.byDevice[s.deviceID] == s {
		delete(m.byDevice, s.deviceID)
	}
}

func (m *managedService) sessionByDigest(digest string) *accountSession {
	m.mu.Lock()
	defer m.mu.Unlock()
	s := m.sessions[digest]
	if s == nil || s.ctx.Err() != nil {
		return nil
	}
	return s
}

func (m *managedService) authenticate(r *http.Request) *accountSession {
	token, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
	if !ok || len(token) != 43 {
		return nil
	}
	return m.sessionByDigest(tokenDigest(token))
}

func (m *managedService) enroll(req accountRequest, register bool) (savedDevice, error) {
	var passwordHash []byte
	if register {
		var err error
		passwordHash, err = bcrypt.GenerateFromPassword([]byte(req.Password), bcrypt.DefaultCost)
		if err != nil {
			return savedDevice{}, err
		}
	} else {
		if err := m.db.View(func(tx *bolt.Tx) error {
			passwordHash = append([]byte(nil), tx.Bucket(accountsBucket).Get([]byte(req.Name))...)
			return nil
		}); err != nil {
			return savedDevice{}, err
		}
		hash := passwordHash
		if len(hash) == 0 {
			hash = m.dummyHash
		}
		if err := bcrypt.CompareHashAndPassword(hash, []byte(req.Password)); err != nil || len(passwordHash) == 0 {
			return savedDevice{}, errBadLogin
		}
	}
	var device savedDevice
	err := m.db.Update(func(tx *bolt.Tx) error {
		accountKey := []byte(req.Name)
		if register {
			accounts := tx.Bucket(accountsBucket)
			if accounts.Get(accountKey) != nil {
				return errAccountTaken
			}
			if err := accounts.Put(accountKey, passwordHash); err != nil {
				return err
			}
		}
		devices, err := tx.Bucket(devicesBucket).CreateBucketIfNotExists(accountKey)
		if err != nil {
			return err
		}
		installationKey := []byte(tokenDigest(req.InstallationID))
		if entry := devices.Get(installationKey); entry != nil {
			if err := json.Unmarshal(entry, &device); err != nil {
				return err
			}
		} else if device.ID, err = makeToken(18); err != nil {
			return err
		}
		device.Name, device.Platform = req.DeviceName, req.Platform
		data, err := json.Marshal(device)
		if err != nil {
			return err
		}
		return devices.Put(installationKey, data)
	})
	return device, err
}

func (m *managedService) allowPasswordAttempt(r *http.Request) bool {
	addr, _ := netip.ParseAddrPort(r.RemoteAddr)
	ip := addr.Addr().Unmap()
	now := time.Now()
	m.mu.Lock()
	defer m.mu.Unlock()
	if !now.Before(m.rateUntil) {
		clear(m.attempts)
		m.total = 0
		m.rateUntil = now.Add(time.Minute)
	}
	if m.total >= 120 || m.attempts[ip] >= 15 {
		return false
	}
	m.total++
	m.attempts[ip]++
	return true
}

func sendJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func accountError(w http.ResponseWriter, status int, text string) {
	sendJSON(w, status, map[string]string{"error": text})
}

func (m *managedService) login(w http.ResponseWriter, r *http.Request, register bool) {
	var req accountRequest
	d := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	d.DisallowUnknownFields()
	if err := d.Decode(&req); err != nil || d.Decode(new(any)) != io.EOF {
		accountError(w, http.StatusBadRequest, "账号或设备信息格式错误")
		return
	}
	req.Name = strings.ToLower(strings.TrimSpace(req.Name))
	req.DeviceName = strings.TrimSpace(req.DeviceName)
	install, _ := base64.RawURLEncoding.DecodeString(req.InstallationID)
	if !accountName.MatchString(req.Name) || len(req.Password) < 8 || len(req.Password) > 72 {
		accountError(w, http.StatusBadRequest, "账号使用 3–32 位英文、数字或 ._-，密码为 8–72 字节")
		return
	}
	if len(install) != 32 || !validDeviceLabel(req.DeviceName) || !validDeviceLabel(req.Platform) {
		accountError(w, http.StatusBadRequest, "设备信息无效")
		return
	}
	if !m.allowPasswordAttempt(r) {
		accountError(w, http.StatusTooManyRequests, "请求过于频繁，请稍后再试")
		return
	}
	select {
	case m.passwordWorkers <- struct{}{}:
		defer func() { <-m.passwordWorkers }()
	default:
		accountError(w, http.StatusTooManyRequests, "服务忙，请稍后再试")
		return
	}

	device, err := m.enroll(req, register)
	switch {
	case errors.Is(err, errAccountTaken):
		accountError(w, http.StatusConflict, err.Error())
	case errors.Is(err, errBadLogin):
		accountError(w, http.StatusUnauthorized, err.Error())
	case err != nil:
		log.Printf("account storage: %v", err)
		accountError(w, http.StatusInternalServerError, "账户存储暂不可用")
	default:
		token, s, err := m.issueSession(req.Name, device.ID)
		if err != nil {
			accountError(w, http.StatusInternalServerError, "无法创建登录会话")
			return
		}
		servers := []managedIceServer{}
		if m.relay != nil {
			servers = m.relay.iceServers(s)
		}
		sendJSON(w, http.StatusOK, map[string]any{
			"accessToken": token, "device": device, "iceServers": servers,
		})
	}
}

func validDeviceLabel(text string) bool {
	return text != "" && len(text) <= 128 && strings.IndexFunc(text, unicode.IsControl) == -1
}

func (m *managedService) devices(w http.ResponseWriter, r *http.Request) {
	s := m.authenticate(r)
	if s == nil {
		accountError(w, http.StatusUnauthorized, "登录已失效，请重新登录")
		return
	}
	type deviceStatus struct {
		savedDevice
		State string `json:"state"`
	}
	list := []deviceStatus{}
	err := m.db.View(func(tx *bolt.Tx) error {
		devices := tx.Bucket(devicesBucket).Bucket([]byte(s.account))
		return devices.ForEach(func(_, entry []byte) error {
			var device savedDevice
			if err := json.Unmarshal(entry, &device); err != nil {
				return err
			}
			list = append(list, deviceStatus{savedDevice: device})
			return nil
		})
	})
	if err != nil {
		log.Printf("device listing: %v", err)
		accountError(w, http.StatusInternalServerError, "无法读取设备列表")
		return
	}
	m.signals.mu.Lock()
	for i := range list {
		d := &list[i]
		d.State = "offline"
		if host := m.signals.deviceHosts[d.ID]; host != nil && host.host.identity.ctx.Err() == nil {
			d.State = "online"
			if host.viewer != nil {
				d.State = "busy"
			}
		}
	}
	m.signals.mu.Unlock()
	sendJSON(w, http.StatusOK, list)
}

func (m *managedService) routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v1/register", func(w http.ResponseWriter, r *http.Request) { m.login(w, r, true) })
	mux.HandleFunc("POST /api/v1/login", func(w http.ResponseWriter, r *http.Request) { m.login(w, r, false) })
	mux.HandleFunc("GET /api/v1/devices", m.devices)
	mux.HandleFunc("POST /api/v1/logout", func(w http.ResponseWriter, r *http.Request) {
		if s := m.authenticate(r); s != nil {
			m.mu.Lock()
			m.removeSessionLocked(s)
			m.mu.Unlock()
		}
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("GET /signal", m.signals.serve)
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	return mux
}
