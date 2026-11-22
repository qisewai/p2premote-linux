package main

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const defaultSTUN = "103.236.93.69:23969,179.253.242.112:3478"

func main() {
	bind := flag.String("bind", "0.0.0.0:0", "本地 IPv4 监听地址，可用 网卡IP:0 指定本地地址")
	servers := flag.String("stun", defaultSTUN, "STUN 地址列表，逗号分隔，最多 4 个")
	duration := flag.Duration("duration", 30*time.Second, "双端探测时长，范围 5s–5m")
	discoverOnly := flag.Bool("discover-only", false, "仅检查 STUN 与公网映射")
	flag.Parse()

	input := bufio.NewScanner(os.Stdin)
	var log bytes.Buffer
	report := io.MultiWriter(os.Stdout, &log)
	err := run(input, report, *bind, *servers, *duration, *discoverOnly)
	if err != nil {
		fmt.Fprintln(report, "测试结束：", err)
	}
	if path, saveErr := saveReport(log.Bytes()); saveErr != nil {
		fmt.Fprintln(os.Stderr, "保存报告失败：", saveErr)
	} else {
		fmt.Println("报告已保存到", path)
	}
	if info, statErr := os.Stdin.Stat(); statErr == nil && info.Mode()&os.ModeCharDevice != 0 {
		fmt.Println("按 Enter 退出。")
		input.Scan()
	}
	if err != nil {
		os.Exit(1)
	}
}

func run(input *bufio.Scanner, report io.Writer, bind, servers string, duration time.Duration, discoverOnly bool) error {
	if duration < 5*time.Second || duration > 5*time.Minute {
		return errors.New("-duration 必须在 5s–5m 之间")
	}
	nodes, err := parseSTUNServers(servers)
	if err != nil {
		return err
	}
	local, err := net.ResolveUDPAddr("udp4", bind)
	if err != nil {
		return fmt.Errorf("无效的本地 IPv4 地址 %q: %w", bind, err)
	}
	conn, err := net.ListenUDP("udp4", local)
	if err != nil {
		return fmt.Errorf("无法打开 UDP 套接字: %w", err)
	}
	defer conn.Close()

	fmt.Fprintln(report, "P2P Remote 独立 UDP 打洞测试")
	fmt.Fprintln(report, "开始时间", time.Now().Format(time.RFC3339))
	fmt.Fprintln(report, "本地 UDP 套接字", conn.LocalAddr())
	fmt.Fprintln(report, "正在查询 STUN 公网映射…")
	baseline := readMappings(conn, nodes, report)
	if len(baseline.endpoints) == 0 {
		return errors.New("所有 STUN 查询均失败，无法生成连接码")
	}
	if discoverOnly {
		return nil
	}

	own := offer{endpoints: baseline.endpoints}
	if _, err := rand.Read(own.id[:]); err != nil {
		return err
	}
	fmt.Println("\n把这一行连接码复制给另一台电脑：")
	code := own.encode()
	fmt.Println(code)
	stopKeepalive := refreshWhileWaiting(conn, nodes, baseline.byServer)
	peer, err := readPeer(input, own)
	stopKeepalive()
	if err != nil {
		return err
	}

	fmt.Fprintf(report, "\n开始直连探测，持续 %s。对端通告地址 %v\n", duration, peer.endpoints)
	fmt.Fprintf(report, "前 %s 仅发 ICE 格式 STUN，之后在同一套接字上对照普通 UDP。\n", stunLeadIn(duration))
	results, probeErr := probe(conn, own, peer, duration)
	for wire, result := range results {
		fmt.Fprintln(report, wireNames[wire])
		if probeWire(wire) == stunWire {
			fmt.Fprintf(report, "  本机发送普通 UDP 前收到 STUN 回复 %d 次\n", result.repliesBeforeRaw)
		}
		result.print(report, peer.endpoints, own.endpoints)
	}
	fmt.Fprintln(report, "\n探测后复查同一个 UDP 套接字的 STUN 映射…")
	after := readMappings(conn, nodes, report)
	for _, node := range nodes {
		before, current := baseline.byServer[node.name], after.byServer[node.name]
		if before.IsValid() && current.IsValid() && before != current {
			fmt.Fprintf(report, "  %s 的映射变更 %s → %s\n", node.name, before, current)
		}
	}
	return probeErr
}

func readPeer(input *bufio.Scanner, own offer) (offer, error) {
	fmt.Println("两台电脑互相粘贴对方的连接码并按 Enter，尽量同时开始。")
	for {
		fmt.Println("粘贴对端连接码：")
		if !input.Scan() {
			if err := input.Err(); err != nil {
				return offer{}, err
			}
			return offer{}, errors.New("未输入对端连接码")
		}
		code := strings.TrimSpace(input.Text())
		peer, err := decodeOffer(code)
		switch {
		case err != nil:
			fmt.Println(err)
		case peer.id == own.id:
			fmt.Println("这是本机的连接码，请粘贴对端的连接码。")
		default:
			return peer, nil
		}
	}
}

func saveReport(report []byte) (string, error) {
	cache, err := os.UserCacheDir()
	if err != nil {
		return "", err
	}
	directory := filepath.Join(cache, "P2PRemote", "NATProbe")
	if err := os.MkdirAll(directory, 0700); err != nil {
		return "", err
	}
	file, err := os.CreateTemp(directory, "nat-probe-"+time.Now().Format("20060102-150405")+"-*.txt")
	if err != nil {
		return "", err
	}
	_, writeErr := file.Write(report)
	return file.Name(), errors.Join(writeErr, file.Close())
}
