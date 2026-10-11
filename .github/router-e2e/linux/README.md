# Router e2e in Linux network namespaces

Needs root, `ip netns`, `dig`, `curl`, `nc`, Go 1.25.

    git clone --depth 1 --branch v3.1.20260828 https://github.com/amnezia-vpn/amneziawg-go.git /tmp/awg
    git -C /tmp/awg apply "$PWD/recipes/awg-go-src/patches/0001-routing-router-and-transport-fixes.patch"
    (cd /tmp/awg && GOFLAGS=-mod=mod go build -o "$OLDPWD/.github/router-e2e/linux/amneziawg-go" .)
    (cd .github/router-e2e/linux && go build -o e2ehelper . && sudo ./run.sh)

Topology: `cli` namespace (AWG client + router) <-> root (AWG server) <-> `inet`
namespace (DNS, "direct"/"proxy" HTTP sites). The HTTP sites echo the client
source address: 192.168.100.2 = went direct, 10.8.1.2 = went through the tunnel.

Each run uses unique namespace, interface and socket names. Cleanup stops only
the processes started by that run. The suite checks the default NXDOMAIN block
response and the optional `blockAnswer: zero` response separately.

`.github/workflows/router-e2e-linux.yml` runs the suite on a hosted Ubuntu
runner. It also compares selected transport tests before and after the patch,
runs the router under the race detector, and vets the router for Linux, macOS,
Windows and Android. Uploaded logs are redacted first.
