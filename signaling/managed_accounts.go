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
	accountsBucket   = []byte("accounts")
	devicesBucket    = []byte("devices")
	accountName      = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{2,31}$`)
	errAccountTaken  = errors.New("账号已存在")
	errBadLogin      = errors.New("账号或密码不正确")
	errDeviceRemoved = errors.New("本机未添加或已从账号移除")
)

type savedDevice struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Platform string `json:"platform"`
}

type accountSession struct {
	ctx           context.Context
	cancel        context.CancelFunc
	digest        string
	account       string
	installation  string
	deviceName    string
	platform      string
	serviceDevice string
}

type installationSession struct {
	account, installation string
	service               bool
}

func (s *accountSession) key() installationSession {
	return installationSession{s.account, s.installation, s.serviceDevice != ""}
}

type managedService struct {
	db        *bolt.DB
	dummyHash []byte
	relay     *managedRelay
	signals   *hub

	mu              sync.Mutex
	sessions        map[string]*accountSession
	byInstallation  map[installationSession]*accountSession
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
		byInstallation: make(map[installationSession]*accountSession), attempts: make(map[netip.Addr]int),
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
	ServiceDevice  string `json:"serviceDeviceId,omitempty"`
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

func (m *managedService) issueSession(req accountRequest) (string, *accountSession, error) {
	token, err := makeToken(32)
	if err != nil {
		return "", nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), accountSessionLifetime)
	s := &accountSession{ctx: ctx, cancel: cancel, digest: tokenDigest(token), account: req.Name,
		installation: tokenDigest(req.InstallationID), deviceName: req.DeviceName, platform: req.Platform,
		serviceDevice: req.ServiceDevice}
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, old := range m.sessions {
		if old.ctx.Err() != nil {
			m.removeSessionLocked(old)
		}
	}
	key := s.key()
	if old := m.byInstallation[key]; old != nil {
		m.removeSessionLocked(old)
	}
	m.sessions[s.digest] = s
	m.byInstallation[key] = s
	return token, s, nil
}

func (m *managedService) removeSessionLocked(s *accountSession) {
	s.cancel()
	delete(m.sessions, s.digest)
	if key := s.key(); m.byInstallation[key] == s {
		delete(m.byInstallation, key)
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

func (m *managedService) authenticateAccount(req accountRequest, register bool) error {
	if register {
		passwordHash, err := bcrypt.GenerateFromPassword([]byte(req.Password), bcrypt.DefaultCost)
		if err != nil {
			return err
		}
		return m.db.Update(func(tx *bolt.Tx) error {
			accountKey := []byte(req.Name)
			accounts := tx.Bucket(accountsBucket)
			if accounts.Get(accountKey) != nil {
				return errAccountTaken
			}
			return accounts.Put(accountKey, passwordHash)
		})
	}
	var passwordHash []byte
	if err := m.db.View(func(tx *bolt.Tx) error {
		passwordHash = append([]byte(nil), tx.Bucket(accountsBucket).Get([]byte(req.Name))...)
		return nil
	}); err != nil {
		return err
	}
	hash := passwordHash
	if len(hash) == 0 {
		hash = m.dummyHash
	}
	if err := bcrypt.CompareHashAndPassword(hash, []byte(req.Password)); err != nil || len(passwordHash) == 0 {
		return errBadLogin
	}
	return nil
}

func findDevice(tx *bolt.Tx, account, installation string) (savedDevice, error) {
	var device savedDevice
	if bucket := tx.Bucket(devicesBucket).Bucket([]byte(account)); bucket != nil {
		if data := bucket.Get([]byte(installation)); data != nil {
			err := json.Unmarshal(data, &device)
			return device, err
		}
	}
	return device, nil
}

func (m *managedService) currentDevice(account, installation string) (savedDevice, error) {
	var device savedDevice
	err := m.db.View(func(tx *bolt.Tx) error {
		var err error
		device, err = findDevice(tx, account, installation)
		return err
	})
	return device, err
}

func (m *managedService) addDevice(s *accountSession) (savedDevice, error) {
	var device savedDevice
	err := m.db.Update(func(tx *bolt.Tx) error {
		var err error
		device, err = findDevice(tx, s.account, s.installation)
		if err != nil || device.ID != "" {
			return err
		}
		devices, err := tx.Bucket(devicesBucket).CreateBucketIfNotExists([]byte(s.account))
		if err != nil {
			return err
		}
		if device.ID, err = makeToken(18); err != nil {
			return err
		}
		device.Name, device.Platform = s.deviceName, s.platform
		data, err := json.Marshal(device)
		if err != nil {
			return err
		}
		return devices.Put([]byte(s.installation), data)
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
	if !accountName.MatchString(req.Name) || len(req.Password) < 8 || len(req.Password) > 72 {
		accountError(w, http.StatusBadRequest, "账号使用 3–32 位英文、数字或 ._-，密码为 8–72 字节")
		return
	}
	if !validID(req.InstallationID, 32) || !validDeviceLabel(req.DeviceName) || !validDeviceLabel(req.Platform) ||
		(req.ServiceDevice != "" && (register || !validID(req.ServiceDevice, 18))) {
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

	err := m.authenticateAccount(req, register)
	switch {
	case errors.Is(err, errAccountTaken):
		accountError(w, http.StatusConflict, err.Error())
	case errors.Is(err, errBadLogin):
		accountError(w, http.StatusUnauthorized, err.Error())
	case err != nil:
		log.Printf("account storage: %v", err)
		accountError(w, http.StatusInternalServerError, "账户存储暂不可用")
	}
	if err != nil {
		return
	}
	device, err := m.currentDevice(req.Name, tokenDigest(req.InstallationID))
	if err != nil {
		log.Printf("current device: %v", err)
		accountError(w, http.StatusInternalServerError, "无法读取本机设备")
		return
	}
	if req.ServiceDevice != "" && req.ServiceDevice != device.ID {
		accountError(w, http.StatusForbidden, "服务绑定的设备已移除，请在客户端重新配置")
		return
	}
	token, s, err := m.issueSession(req)
	if err != nil {
		accountError(w, http.StatusInternalServerError, "无法创建登录会话")
		return
	}
	servers := []managedIceServer{}
	if m.relay != nil {
		servers = m.relay.iceServers(s)
	}
	sendJSON(w, http.StatusOK, map[string]any{
		"accessToken": token, "currentDeviceId": device.ID, "iceServers": servers,
	})
}

func validID(id string, size int) bool {
	if len(id) != base64.RawURLEncoding.EncodedLen(size) {
		return false
	}
	decoded, err := base64.RawURLEncoding.Strict().DecodeString(id)
	return err == nil && len(decoded) == size
}

func (m *managedService) hasDevice(account, deviceID string) bool {
	if account == "" || !validID(deviceID, 18) {
		return false
	}
	found := false
	_ = m.db.View(func(tx *bolt.Tx) error {
		devices := tx.Bucket(devicesBucket).Bucket([]byte(account))
		if devices == nil {
			return nil
		}
		cursor := devices.Cursor()
		for key, entry := cursor.First(); key != nil; key, entry = cursor.Next() {
			var device savedDevice
			if json.Unmarshal(entry, &device) == nil && device.ID == deviceID {
				found = true
				break
			}
		}
		return nil
	})
	return found
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
	current := ""
	m.signals.mu.Lock()
	err := m.db.View(func(tx *bolt.Tx) error {
		devices := tx.Bucket(devicesBucket).Bucket([]byte(s.account))
		if devices == nil {
			return nil
		}
		return devices.ForEach(func(key, entry []byte) error {
			var device savedDevice
			if err := json.Unmarshal(entry, &device); err != nil {
				return err
			}
			if string(key) == s.installation && (s.serviceDevice == "" || s.serviceDevice == device.ID) {
				current = device.ID
			}
			state := "offline"
			if host := m.signals.deviceHosts[device.ID]; host != nil && host.host.identity.ctx.Err() == nil {
				state = "online"
				if host.viewer != nil {
					state = "busy"
				}
			}
			list = append(list, deviceStatus{device, state})
			return nil
		})
	})
	m.signals.mu.Unlock()
	if err != nil {
		log.Printf("device listing: %v", err)
		accountError(w, http.StatusInternalServerError, "无法读取设备列表")
		return
	}
	sendJSON(w, http.StatusOK, map[string]any{"currentDeviceId": current, "devices": list})
}

func (m *managedService) deviceEditor(w http.ResponseWriter, r *http.Request) *accountSession {
	s := m.authenticate(r)
	if s == nil {
		accountError(w, http.StatusUnauthorized, "登录已失效，请重新登录")
	} else if s.serviceDevice != "" {
		accountError(w, http.StatusForbidden, "请在客户端管理设备")
		return nil
	}
	return s
}

func (m *managedService) addCurrentDevice(w http.ResponseWriter, r *http.Request) {
	s := m.deviceEditor(w, r)
	if s == nil {
		return
	}
	m.signals.mu.Lock()
	device, err := m.addDevice(s)
	m.signals.mu.Unlock()
	if err != nil {
		log.Printf("add device: %v", err)
		accountError(w, http.StatusInternalServerError, "无法添加本机")
		return
	}
	sendJSON(w, http.StatusOK, device)
}

func (m *managedService) removeDevice(w http.ResponseWriter, r *http.Request) {
	s := m.deviceEditor(w, r)
	if s == nil {
		return
	}
	id := r.PathValue("id")
	if !validID(id, 18) {
		accountError(w, http.StatusBadRequest, "设备编号无效")
		return
	}
	// Serialize removal with listen/relay. The room must be invalidated before
	// another connection can see this device.
	m.signals.mu.Lock()
	removed := false
	err := m.db.Update(func(tx *bolt.Tx) error {
		bucket := tx.Bucket(devicesBucket).Bucket([]byte(s.account))
		if bucket == nil {
			return nil
		}
		cursor := bucket.Cursor()
		for key, entry := cursor.First(); key != nil; key, entry = cursor.Next() {
			var device savedDevice
			if err := json.Unmarshal(entry, &device); err != nil {
				return err
			}
			if device.ID == id {
				removed = true
				return cursor.Delete()
			}
		}
		return nil
	})
	if err == nil && removed {
		if room := m.signals.deviceHosts[id]; room != nil {
			m.signals.remove(room)
			room.host.send("error", "本机已从账号移除，接收已停止")
			room.viewer.send("ended", "设备已从账号移除")
		}
	}
	m.signals.mu.Unlock()
	if err != nil {
		log.Printf("remove device: %v", err)
		accountError(w, http.StatusInternalServerError, "无法移除设备")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (m *managedService) routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/v2/register", func(w http.ResponseWriter, r *http.Request) { m.login(w, r, true) })
	mux.HandleFunc("POST /api/v2/login", func(w http.ResponseWriter, r *http.Request) { m.login(w, r, false) })
	mux.HandleFunc("GET /api/v2/devices", m.devices)
	mux.HandleFunc("POST /api/v2/devices/current", m.addCurrentDevice)
	mux.HandleFunc("DELETE /api/v2/devices/{id}", m.removeDevice)
	mux.HandleFunc("POST /api/v2/logout", func(w http.ResponseWriter, r *http.Request) {
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
