# Routing profiles, switching servers on the fly, AmneziaWG reliability fixes

## Summary

### 1. Routing profiles (replace the site split tunneling)
Rule-based routing modelled on Happ's routing profiles, so profiles can be exchanged with Happ.
- Rule lists `Proxy / Direct / Block` × `sites / IPs`:
  - `domain:`, `full:`, `keyword:`, `regexp:`, plain domains;
  - `geosite:tag[@attr]`, IPs/CIDRs, `geoip:cc` (incl. `geoip:!cc`).
- Profile settings: `GlobalProxy` (default action), route order, domain strategy, remote/domestic DNS (DoU/DoH), DNS hosts, geo file URLs (Loyalsoldier by default, downloaded per profile with a selectable User-Agent).
- Profiles are edited as plain text (one rule per line, commas, `#` comments), with a searchable geosite/geoip category picker.
- Import from clipboard, file, URL or pasted text:
  - Happ JSON with case-insensitive keys, base64, and `happ://routing/add|onadd/` payloads;
  - plain domain/IP lists;
  - site lists exported by older AmneziaVPN versions.
- Export is Happ-compatible. No deeplink handler is registered.
- Per-server override: default profile, off, or a specific profile. Excluded routes default to the LAN ranges.
- Existing site lists are migrated into profiles, and backups from older versions are migrated on restore.
- Per-app split tunneling (Android, Windows) now lives inside the routing section.
- Settings can be changed while connected; they apply on reconnect, and a notice offers a one-tap reconnect.

How it is applied, per protocol:
- **AmneziaWG / WireGuard (all platforms):** a packet router inside amneziawg-go, added by `recipes/awg-go-src` (patch on top of upstream v3.1.20260828).
  - DNS: the OS DNS becomes `198.18.0.53`. The router answers there and intercepts all UDP/53 plus TCP/53. It resolves proxied names through the tunnel and direct names outside it (DNS-over-HTTPS supported). It learns address→rule mappings from the answers.
  - Direct egress: direct TCP, UDP and ICMP echo are re-originated from sockets that bypass the tunnel. On desktop they are bound to the uplink interface (Linux `SO_BINDTODEVICE` + fwmark, macOS `IP_BOUND_IF`, Windows `IP_UNICAST_IF`). On Android they are protected via `VpnService.protect`. In iOS/macOS Network Extensions the OS bypasses them already.
  - Kill switch: allows the router's direct traffic.
  - IPv6: when the tunnel has no IPv6, proxied IPv6 is refused at once and AAAA answers are suppressed, so browsers do not stall. Direct names get AAAA records only when direct IPv6 can leave the host (probed with a UDP connect through a bypass socket), otherwise direct IPv6 would fail at once on IPv4-only uplinks. While the desktop kill switch blocks IPv6 (macOS, Linux), no name gets AAAA records.
- **Xray / SS-Xray:** the profile is compiled into Xray routing, outbounds and DNS, with sniffing in route-only mode.
- **OpenVPN, Cloak, ShadowSocks, IKEv2:** address-based fallback (CIDRs plus resolved host names). Domain-only rules produce warnings.

### 2. Switching servers/protocols on the fly
- Selecting another server, protocol or subscription location while connected switches the connection. So do saving the settings of the protocol in use, changing the kill switch, clearing or removing the protocol in use, and the tray "connect to server" action.
- **Desktop:** restarts the protocol and ignores late signals of the previous one.
- **Android:** stops the running tunnel, waits for DISCONNECTED (with a timeout), then connects.
- **iOS/macOS NE:** stops the running tunnel and waits before starting it with the new configuration. Before this change it silently kept the old one.
- The UI shows "Reconnecting". If preparing the new configuration fails, the old tunnel stays up and the UI shows its real state.
- Every "unable to … while there is an active connection" refusal is gone:
  - destructive actions (remove or clear server, reset config, restore backup, reset settings) disconnect first, with a note in the confirmation;
  - reboot and unlink act while the tunnel still works, then disconnect;
  - everything else reconnects automatically.

### 3. One-click container upgrade without losing users
Today moving an AmneziaWG 2.0 container to 3.1 means reinstalling it, which regenerates the server keys and wipes every user. Now the server protocol page (and the home screen warning) offer **Upgrade**:
- **Upgrade to AmneziaWG 3.1** — fresh 3.1 interface parameters; server keys, PSK, every `[Peer]` and the client table are kept. The admin's own config is re-rendered in place (same keys and IP, no orphaned peer). Other users are flagged "needs an updated config" in the share page, with an **Update config** action that issues a new key under the same name and revokes the old one.
- **Update server software only** (AmneziaWG, Xray) — rebuilds the image from the latest base image and restores the state files verbatim; every config keeps working.

Server procedure: snapshot the state files (abort without changes on failure) → build the new image while the old container keeps serving → park the old container (`-pre-upgrade`, restart policy off) → run the new one → restore state → firewall → start → verify (server public key, peer count, running; polled) → remove the parked container. Any failure after parking rolls back to the parked container; a leftover from an interrupted attempt is recovered or cleaned up on the next run. An active connection to the upgraded container reconnects automatically.

Also: the protocol version is derived from the parameters (a reinstall no longer labels 2.0 parameters as "3.1"), and RandomTrailers is never combined with ranged H1–H4 and unequal S1–S4 (fresh installs, upgrades and saved settings).

### 4. `ERR_SOCKET_NOT_CONNECTED` over AmneziaWG 3.1
Root causes found and fixed, all verified with reproductions:
- **Transport fixes in amneziawg-go** (same patch):
  - Data packets were misclassified as handshakes when RandomTrailers is combined with ranged H1–H3 and unequal S values (about 12% packet loss). The fix checks whether the packet decodes as live transport first.
  - The first packet in each direction was lost when S4 > 0: padding was cached before the TUN read.
  - A data race in the timers.
  - The UDP window was reset on every packet: endpoints were compared by pointer.
- **Backends bumped to the fixed upstream release 3.1.20260828:**
  - padding above the path MTU (about 96% loss on some paths);
  - handshakes dropped under load with DisableCookies.
- **Desktop network change / wake-up:** a full reconnect killed every open connection. For AmneziaWG the tunnel is now kept, and it is reconnected only if no data flows within 20 s.

## Backends
The patched sources are packaged once (`recipes/awg-go-src`) and consumed through a go.mod `replace`:
- `awg-go`: desktop daemon binary;
- `awg-windows`: `tunnel.dll`;
- `awg-apple`: plus `wgSetRoutingConfigFile()` for the NE;
- `awg-android`: plus a JNI socket protector.

Separate upstream PRs to amneziawg-go / -apple / -android can carry the same patches; the recipes switch back to plain tags once they are merged.

## Testing

Current results and evidence boundaries are recorded in [VALIDATION.md](VALIDATION.md).

- The router and device suites pass with the race detector. Block rules override static DNS hosts; that regression passes 20 repetitions. Router vet passes for Darwin, Windows and Android.
- The real Android ARM64 APK builds and passes an API 35 emulator test against an isolated AWG server. The test verifies encrypted proxy traffic, direct traffic, NXDOMAIN, per-app exclusion, explicit profile replacement, and a reconnect queued during shutdown. Conan source revisions now invalidate the Android backend binary.
- The latest Linux namespace suite passes 29 AWG 3.1 checks and 24 checks each with AWG 2 and plain WireGuard wire parameters. The AWG 3.1 run includes four established active/idle streams across a handshake renewal; all runs cover DNS, TCP/UDP/ICMP, throughput and parallel flows. Both ends use the current backend; this exercises the packet router, not the full Linux daemon or older standalone-client interoperability.
- The universal macOS Test app, daemon and package build. Routing, container migration and daemon rollback tests pass. Populated Russian routing UI was inspected; translation and QML lint checks pass.
- The original Happ profile compiles with its requested geodata without fallback warnings.

## Remaining acceptance

The original macOS direct failure was reproduced with read-only bound-socket and route diagnostics. The fix adds owned scoped uplink defaults and isolates Test PF/installer cleanup. The updated full privileged daemon/PF path has not run on the user's Mac; the user's active VPN remains untouched.

GitHub Actions is disabled for the repository, and the current OAuth credential cannot push workflow changes. The macOS real-daemon, Windows service/per-app, Android hosted, Linux namespace and upgrade workflows are prepared locally but have not run for this revision. Historical CI results do not validate these changes.

Windows runtime, Linux full-daemon runtime, iOS/NE device execution, physical network changes, sleep/wake, IPv6 uplink transitions and the complete non-WireGuard protocol/switching matrix remain unverified. Domain rules on OpenVPN/Cloak/ShadowSocks/IKEv2 remain address-based. On macOS, PF permits root-owned TCP/UDP while the router is active because it cannot match a specific process. See the validation report before treating the branch as accepted on all platforms.
