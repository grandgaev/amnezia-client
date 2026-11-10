# Routing validation, 2026-10-11

This report separates executed checks from prepared harnesses. The complete acceptance matrix in `HANDOFF.md` has not passed. The active AmneziaVPN installation, host routes, PF rules and privileged service on the user's Mac were preserved.

## Source and environment

- Repository: `grandgaev/amnezia-client`, branch `claude/sweet-babbage-3wth5m`.
- Fresh checkout based on `97fb0ece1342e18a27f95479980b2b158ca614c0`; the older dirty checkout was preserved. `upstream/dev` was fetched for review and was not merged.
- Mac: Apple Silicon, macOS 26.5.1, Xcode 26.5, Qt 6.10.1, Conan 2.30.0. Builds use `nice -n 15`, two jobs and a shared module lock.
- Android: isolated ARM64 API 35 emulator, NDK 27.0.11718014, Java 17, Qt 6.10.1. The production APK and instrumentation are signed with an ephemeral lab key.
- Linux: resource-limited, labelled containers and network namespaces on the authorized personal server. Test credentials are kept outside Git; instrumentation APKs contain lab credentials and are private test artifacts.

## Executed checks

| Area | Result and boundary |
| --- | --- |
| Go router and transport | Full router and device tests with the race detector pass. `TestRouterBlockAndHosts` passes 20 repetitions. Router vet passes on native macOS and the Darwin ARM64, Windows AMD64 and Android ARM64 targets. |
| Android production service | The fresh ARM64 production APK builds and passes the isolated API 35 instrumentation test against the lab server. Real `AwgService`/`VpnService` and JNI protection are exercised. Private-server echo proves encrypted traffic from `10.77.0.4`; unmatched traffic follows the proxy default. Direct traffic matches the excluded companion application's physical source. The real control domain is blocked with NXDOMAIN. Explicit reconnect replaces a direct hosts rule with block, and a connection queued during shutdown replaces block with direct while creating a new Android VPN network handle. |
| Linux packet router | The latest backend passes 29/29 AWG 3.1 checks, 24/24 AWG 2 wire-parameter checks and 24/24 plain WireGuard wire checks with real TUN interfaces, encryption, namespaces and source-address assertions. These cover remote/direct/TCP DNS, UDP/53 interception, NXDOMAIN and zero responses, learned/CIDR/shared-address rules, TCP/UDP/ICMP, 32 MiB downloads, 50 concurrent flows and runtime disable. The AWG 3.1 run also verifies four established active/idle streams over 185 seconds and a tunneled stream surviving actual handshake renewal. Both ends use amneziawg-go 3.1; AWG 2 and WireGuard parameters are not evidence of interoperability with an older application or kernel implementation. This does not validate the full Linux client daemon or kill switch. |
| macOS build and unit tests | The universal Test application, daemon and package build. `RoutingTest`, `ContainerUpgradeTest` and `daemon_server_switch` pass. The daemon tests inject failures and verify old-file/peer/route/DNS rollback and disconnect when rollback cannot be completed. No test privileged service was installed on the user's Mac. |
| Original profile compiler | The original Happ profile compiles through the production parser/compiler with its requested geodata, without fallback warnings: 743 block domains, 693 direct domains and 7 CIDRs. |
| UI and translation | Populated Russian routing UI was inspected in the isolated Test application. QML lint reports no errors; translation update/release checks find no unfinished new Russian messages or placeholder errors. Full keyboard and scroll interaction acceptance is pending. |

## macOS direct failure

Read-only diagnostics reproduced the failure while the original VPN remained active. Unbound TCP succeeded, but TCP and UDP bound to `en0` returned `ENETUNREACH`. The routing table had tunnel split defaults and an unscoped physical default, but no usable scoped default for `en0`. An explicitly excluded destination remained reachable through the bound interface. A failed direct dial becomes a TCP reset in the packet router, explaining the browser's immediate refusal.

The updated daemon owns scoped physical-uplink defaults, validates foreign existing routes, retries transient failures, and rolls back routes when activation fails. Test PF anchors and installer cleanup are isolated from the original application's anchors. The updated privileged macOS path has not been executed with full PF/kill-switch rules, so this is a reproduced mechanism and tested code change, not confirmation that the user's Mac failure is resolved.

## Fixes discovered during acceptance

- DNS static hosts used to bypass a block rule. A real Android profile replacement reproduced the problem. Block rules now take precedence, including NXDOMAIN and zero A/AAAA responses; the regression test passes with the race detector.
- The Android Conan recipe did not include the patched Go source revision in its binary identity. The cache could reuse an old library after a patch changed. All consumers now use `revision_mode`; graphs for two source revisions produce distinct Android package IDs without clearing the cache.
- Desktop server switching stages routing files and treats peer, route, DNS and process changes as one rollback boundary. Linux route deletion and platform peer-side-effect ownership are corrected. Windows UAPI and firewall failures are checked; a kill-switch mode change takes the restart path.
- Android protocol instances and state forwarding are scoped to a connection session. Reconnect waits for confirmed shutdown; timeout terminates the isolated VPN service process instead of starting a second tunnel over an unresolved shutdown.

## Evidence

Local evidence is in `/Users/admin/Projects/amnezia-routing-lab/evidence/`:

- `go-final-checks.log` and `go-final-checks.py` in the lab root identify the executed race/vet commands.
- `linux-latest/{awg31,awg2,wireguard}.log` and the corresponding `latest-*-redacted/` directories contain the latest namespace results. The executed Linux binary SHA-256 is `44ca473b76ab94a1fbfe3a7122b8502ec4680d35c38977ec53161915cafe644f`.
- `macos-final-build.log`, `macos-final-unit-tests.log` contain build, package and selected test results.
- `android-rebuild.log`, `android-conan-revision-graph-{previous,latest}.json` establish the new source-dependent Android binary identity.
- `android-router-e2e/` contains instrumentation and bounded, selected logcat output.
- `geodata/` and `original-routing.json` identify the actual compiled profile and data. Geosite SHA-256 is `645c7aeaa5fe019abdb551bdad3a7a81ad28b67ba6afdd9f0893e58cfd18b276`; GeoIP SHA-256 is `116cc0f03d48991962f7f9cdbbcf53d45d89777f9c3cd1a663210853e5df6093`.

## CI and remaining acceptance

GitHub Actions is disabled for this repository. A workflow dispatch returns HTTP 422, `Actions has been disabled for this repository`. The current GitHub OAuth credential lacks the `workflow` scope, so GitHub rejects commits that create or change workflow files. Source commits can be pushed. The prepared workflow changes are retained locally and exported to `pending-workflows.patch` in the lab; they have not run. Historical successful workflows do not establish acceptance of the current changes.

The real macOS daemon/PF harness and Windows installer/service/per-app harness are prepared but unexecuted. Windows runtime, Linux full-daemon runtime, iOS/NE device execution, IPv6 uplink transitions, physical network changes, sleep/wake, all non-WireGuard protocol families, and the complete server/protocol switching matrix remain unverified. Stored routing-profile parse errors can still discard malformed records without a surfaced error; that repository behavior was reviewed but is not corrected here. Qt's bundled macOS frameworks were built for macOS 13, so macOS 12 compatibility is not established by this build.

## Requested acceptance matrix

| Scenario | Current evidence |
| --- | --- |
| Direct / proxy / block rules and DNS | Router race tests, production profile compilation and real Linux/Android flows pass for the exercised rules. All rule kinds, route orders, DNS transports and IPv6 combinations have not been exercised on every operating system. |
| Kill switch and per-app exclusions | Android production per-app exclusion passes. Full macOS PF, Windows WFP/per-app and Linux daemon kill-switch execution remain pending. |
| Imports and Happ exchange | Production parser/compiler and routing unit tests pass; the original profile compiles with its requested geodata. Exchange with a running Happ application has not been performed. |
| AWG 3.1 / AWG 2 / WireGuard | Actual encrypted Linux flows pass with each wire-parameter set; the production Android service connects to the lab server. Older standalone client and kernel interoperability are not established. |
| Xray / SS-Xray / OpenVPN / Cloak / ShadowSocks | Compiler and fallback paths were reviewed and built where available. Real protocol traffic is not established by those checks. |
| Reconnect and server switching | Real Android profile replacement and a connect queued during shutdown pass. Desktop failure-injection tests cover rollback; the complete real server-to-server and protocol-to-protocol matrix is pending. |
| Long connections, network changes and wake | Linux active/idle streams survive 185 seconds and verified rekey. Wi-Fi/Ethernet transitions and actual sleep/wake remain pending. |
| Cross-platform CI | Source builds and local runtime checks are recorded above. Actions is disabled and workflow edits cannot be pushed with the current credential; no current all-platform green CI result exists. |
