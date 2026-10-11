# Routing profiles — cross-platform contract

Repository: grandgaev/amnezia-client, branch claude/sweet-babbage-3wth5m (AmneziaVPN client, Qt 6 / QML).

## What the feature is

"Routing profiles" (port of the Happ client's routing profiles) replace the old site-based split tunneling.
A profile has rule lists proxy/direct/block × sites/IPs (`domain:`, `full:`, `keyword:`, `regexp:`, plain
domains, `geosite:tag[@attr]`, IPs/CIDRs, `geoip:cc`), `GlobalProxy` (default action for unmatched traffic:
true = via VPN, false = direct), route order, remote/domestic DNS, DNS hosts, geo file URLs.
Compilation lives in `client/core/models/routing/` (RoutingProfile, GeoData, IpRangeSet, RoutingCompiler).
Storage/controller: `client/core/controllers/routingController.*`, repository `SecureAppSettingsRepository`
(routingProfiles, isRoutingEnabled, selectedRoutingProfileId, routingExcludedRoutes, serverRoutingOverrides).
Changes made while connected apply after reconnect.

## How a connection applies a profile (VpnConnection::appendRoutingConfig — owned by the coordinator)

`m_vpnConfiguration` is the top-level JSON handed to every platform backend
(desktop: `client/mozilla/localsocketcontroller.cpp` for AWG/WG, `core/protocols/*` otherwise;
iOS/macOS NE: `client/platforms/ios/ios_controller.mm`; Android: JSON forwarded to Kotlin).

1. **AmneziaWG / WireGuard ("router" mode)** — handled by a packet router built into amneziawg-go.
   The coordinator puts into `m_vpnConfiguration`:
   - `"routing_config"`: JSON object (the router config, see below). Present ONLY in router mode.
   - `"dns1"` = `"dns2"` = `"198.18.0.53"` (the router's DNS VIP; the real DNS servers of the connection
     are inside `routing_config.dns.remote`). The OS DNS of the tunnel must be exactly 198.18.0.53.
   - `"<proto>_config_data".allowed_ips` (proto = `awg` or `wireguard`) contains `"198.18.0.53/32"` when
     it does not already contain 0.0.0.0/0 — the VIP must be routed into the tunnel interface.
   - `"splitTunnelType"` = 0 and `"splitTunnelSites"` = [] (site split tunneling of the platform is off).
   - `"appSplitTunnelType"` / `"splitTunnelApps"` unchanged: per-app split tunneling (Android, Windows)
     keeps working through the existing platform code.
   The platform backend must:
   a. write `routing_config` (compact JSON) to a file readable by the process that runs amneziawg-go
      (private location, overwrite on each connect);
   b. make amneziawg-go load it: UAPI device-level key `routing_config_file=<absolute path>`
      (an empty value disables the router). Device-level keys must appear BEFORE the first
      `public_key=` line of a `set=1` operation (after it, keys are peer keys), or be sent in a separate
      `set=1` operation. In Go: `device.Router().Configure(path)` (package
      `github.com/amnezia-vpn/amneziawg-go/v3/device`); `Configure("")` disables it.
   c. make sure sockets the router opens for "direct" traffic bypass the tunnel:
      - `routing_config.bypass == "os"` (set by the coordinator on iOS, macOS Network Extension, Android):
        the OS guarantees it. iOS/macOS NE: sockets of the extension process are not routed into its
        own tunnel. Android: every socket must be passed to `VpnService.protect(fd)` — amneziawg-go has
        `router.SetSocketProtector(func(fd int) bool)` (package `github.com/amnezia-vpn/amneziawg-go/v3/router`),
        called for every socket the router creates before connect/bind.
      - `routing_config.bypass == "interface"` (desktop daemon: Linux, macOS classic daemon, Windows):
        the daemon tells the router which physical interface to use, via UAPI device keys, and must
        update them when the default route/interface changes:
          `routing_bypass_ifindex=<n>`   IPv4 interface index (macOS IP_BOUND_IF, Windows IP_UNICAST_IF)
          `routing_bypass_ifindex6=<n>`  IPv6 interface index (macOS IPV6_BOUND_IF, Windows IPV6_UNICAST_IF)
          `routing_bypass_ifname=<name>` Linux SO_BINDTODEVICE
          `routing_bypass_fwmark=<n>`    Linux SO_MARK (use the fwmark the daemon already uses so that
                                          amneziawg-go's own UDP socket bypasses the tunnel)
        Go equivalents: `Router().SetBypassInterface(uint32)`, `SetBypassInterface6(uint32)`,
        `SetBypassInterfaceName(string)`, `SetBypassMark(uint32)`.
        Without any bypass information the router falls back to sending "direct" traffic through the VPN.
   d. kill switch / firewalls must not block the router's direct egress on the physical interface.
2. **Xray / SS-Xray**: the coordinator rewrites the xray JSON (`<proto>_config_data.config` string) with
   routing/outbounds/dns — nothing to do on the platforms (the direct outbound's sockets are already
   bound/protected like the proxy outbound's).
3. **OpenVPN, Cloak, ShadowSocks, IKEv2**: address based fallback through the existing keys
   `splitTunnelType` (1 = only listed via VPN, 2 = all except listed) + `splitTunnelSites` (CIDRs/IPs,
   hostnames already resolved by the coordinator). Existing platform code handles it.

## Router config (`routing_config`) JSON — produced by RoutingCompiler::routerConfig
```json
{
  "version": 1,
  "defaultAction": "proxy",               // or "direct"
  "rules": [ {"action": "block|direct|proxy", "domains": ["domain:x.com","full:a.b","keyword:k","regexp:re"], "ips": ["1.2.3.0/24"]} ],
  "dns": { "listen": ["198.18.0.53"], "remote": ["10.8.1.1"], "direct": ["8.8.8.8"],
           "hosts": {"name": ["1.2.3.4"]}, "directDoh": {"url": "https://dns.google/dns-query", "addrs": ["8.8.8.8"]} },
  "bypass": "os"                          // or "interface"
}
```

## Patched amneziawg-go
Upstream v3.1.20260828 + `recipes/awg-go-src/patches/0001-routing-router-and-transport-fixes.patch`

Conan package `awg-go-src/3.1.20260828` (recipe `recipes/awg-go-src`) contains the patched source tree in
`<package_folder>/src`. Backends consume it by appending to their go.mod:
`replace github.com/amnezia-vpn/amneziawg-go/v3 => <package_folder>/src` and building with
`GOFLAGS=-mod=mod` (go.sum gets the missing entries, e.g. gvisor). The coordinator wires the recipes
(awg-go, awg-windows) — agents working on awg-apple / awg-android write their own patches under
`recipes/awg-apple/patches/` / `recipes/awg-android/patches/` (git format, applied with `git apply` in
`source()`), and update their recipe to tool_require awg-go-src and add the replace line.

## Build / test environment
The original sandbox paths no longer exist. Rebuild the patched sources with:
`git clone --depth 1 --branch v3.1.20260828 https://github.com/amnezia-vpn/amneziawg-go.git && git -C amneziawg-go apply recipes/awg-go-src/patches/0001-routing-router-and-transport-fixes.patch`
(see docs/routing/HANDOFF.md for the full workflow).
