package main

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/curve25519"
	"golang.org/x/net/dns/dnsmessage"
)

func main() {
	switch os.Args[1] {
	case "keygen":
		var k [32]byte
		rand.Read(k[:])
		k[0] &= 248
		k[31] = (k[31] & 127) | 64
		pub, _ := curve25519.X25519(k[:], curve25519.Basepoint)
		fmt.Println(hex.EncodeToString(k[:]), hex.EncodeToString(pub), base64.StdEncoding.EncodeToString(k[:]))
	case "uapi":
		// uapi <socket> reads commands from stdin
		c, err := net.Dial("unix", os.Args[2])
		if err != nil {
			panic(err)
		}
		in, _ := io.ReadAll(os.Stdin)
		c.Write(in)
		r := bufio.NewReader(c)
		for {
			line, err := r.ReadString('\n')
			fmt.Print(line)
			if err != nil || line == "\n" {
				return
			}
		}
	case "http":
		// http <addr...>
		mux := http.NewServeMux()
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			host, _, _ := net.SplitHostPort(r.RemoteAddr)
			if r.URL.Path == "/stream" || r.URL.Path == "/idle" {
				seconds, err := strconv.Atoi(r.URL.Query().Get("seconds"))
				if err != nil || seconds < 1 || seconds > 240 {
					http.Error(w, "Invalid duration", http.StatusBadRequest)
					return
				}
				flusher := w.(http.Flusher)
				fmt.Fprintf(w, "%s 0\n", host)
				flusher.Flush()
				if r.URL.Path == "/idle" {
					select {
					case <-time.After(time.Duration(seconds) * time.Second):
						fmt.Fprintf(w, "%s 1\n", host)
					case <-r.Context().Done():
					}
					return
				}
				ticker := time.NewTicker(time.Second)
				defer ticker.Stop()
				for i := 1; i <= seconds; i++ {
					select {
					case <-ticker.C:
						if _, err := fmt.Fprintf(w, "%s %d\n", host, i); err != nil {
							return
						}
						flusher.Flush()
					case <-r.Context().Done():
						return
					}
				}
				return
			}
			if r.URL.Path == "/big" {
				buf := make([]byte, 1<<20)
				for i := 0; i < 32; i++ {
					w.Write(buf)
				}
				return
			}
			fmt.Fprintf(w, "%s", host)
		})
		for _, a := range os.Args[2:] {
			go http.ListenAndServe(a, mux)
		}
		// UDP echo on :7 of every address
		for _, a := range os.Args[2:] {
			h, _, _ := net.SplitHostPort(a)
			go func(h string) {
				pc, err := net.ListenPacket("udp", h+":7")
				if err != nil {
					return
				}
				buf := make([]byte, 2000)
				for {
					n, addr, err := pc.ReadFrom(buf)
					if err != nil {
						return
					}
					pc.WriteTo([]byte("from "+addr.(*net.UDPAddr).IP.String()+" "+string(buf[:n])), addr)
				}
			}(h)
		}
		select {}
	case "dns":
		// dns <listen> name=ip,...
		table := map[string]string{}
		for _, kv := range strings.Split(os.Args[3], ",") {
			p := strings.SplitN(kv, "=", 2)
			table[p[0]] = p[1]
		}
		pc, err := net.ListenPacket("udp", os.Args[2])
		if err != nil {
			panic(err)
		}
		buf := make([]byte, 1500)
		for {
			n, addr, err := pc.ReadFrom(buf)
			if err != nil {
				return
			}
			var p dnsmessage.Parser
			h, err := p.Start(buf[:n])
			if err != nil {
				continue
			}
			q, err := p.Question()
			if err != nil {
				continue
			}
			h.Response = true
			name := strings.TrimSuffix(q.Name.String(), ".")
			ip, ok := table[name]
			if !ok {
				h.RCode = dnsmessage.RCodeNameError
			}
			b := dnsmessage.NewBuilder(nil, h)
			b.StartQuestions()
			b.Question(q)
			b.StartAnswers()
			if ok && q.Type == dnsmessage.TypeA {
				a := netip.MustParseAddr(ip)
				b.AResource(dnsmessage.ResourceHeader{Name: q.Name, Class: dnsmessage.ClassINET, TTL: 5}, dnsmessage.AResource{A: a.As4()})
			}
			out, _ := b.Finish()
			fmt.Fprintf(os.Stderr, "%s dns query %s from %v\n", time.Now().Format("15:04:05.000"), name, addr)
			pc.WriteTo(out, addr)
		}
	}
}
