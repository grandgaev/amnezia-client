import Foundation
import NetworkExtension

extension Constants {
    /// Provider configuration entry with the configuration of the router built into amneziawg-go
    /// (routing profiles), compact JSON. Present only when the router is used.
    static let routingConfigKey = "routing_config"
    static let routingConfigFileName = "routing_config.json"
}

extension PacketTunnelProvider {
    func startWireguard(activationAttemptId: String?,
                        errorNotifier: ErrorNotifier,
                        completionHandler: @escaping (Error?) -> Void) {
        guard let protocolConfiguration = self.protocolConfiguration as? NETunnelProviderProtocol,
              let providerConfiguration = protocolConfiguration.providerConfiguration,
              let wgConfigData: Data = providerConfiguration[Constants.wireGuardConfigKey] as? Data else {
            wg_log(.error, message: "Can't start, config missing")
            completionHandler(nil)
            return
        }

        do {
            let wgConfig = try JSONDecoder().decode(WGConfig.self, from: wgConfigData)
            let wgConfigStr = wgConfig.str

            let tunnelConfiguration = try TunnelConfiguration(fromWgQuickConfig: wgConfigStr)

            if tunnelConfiguration.peers.first!.allowedIPs
                .map({ $0.stringRepresentation })
                .joined(separator: ", ") == "0.0.0.0/0, ::/0" {
                if wgConfig.splitTunnelType == 1 {
                    for index in tunnelConfiguration.peers.indices {
                        tunnelConfiguration.peers[index].allowedIPs.removeAll()
                        var allowedIPs = [IPAddressRange]()

                        for allowedIPString in wgConfig.splitTunnelSites {
                            if let allowedIP = IPAddressRange(from: allowedIPString) {
                                allowedIPs.append(allowedIP)
                            }
                        }

                        tunnelConfiguration.peers[index].allowedIPs = allowedIPs
                    }
                } else if wgConfig.splitTunnelType == 2 {
                    for index in tunnelConfiguration.peers.indices {
                        var excludeIPs = [IPAddressRange]()

                        for excludeIPString in wgConfig.splitTunnelSites {
                            if let excludeIP = IPAddressRange(from: excludeIPString) {
                                excludeIPs.append(excludeIP)
                            }
                        }

                        tunnelConfiguration.peers[index].excludeIPs = excludeIPs
                    }
                }
            }

            wg_log(.info, message: "Starting tunnel from the " +
                   (activationAttemptId == nil ? "OS directly, rather than the app" : "app"))

            guard setWireguardRoutingConfig(providerConfiguration[Constants.routingConfigKey] as? Data) else {
                errorNotifier.notify(PacketTunnelProviderError.couldNotStartBackend)
                completionHandler(PacketTunnelProviderError.couldNotStartBackend)
                return
            }

            // Start the tunnel
            wgAdapter = WireGuardAdapter(with: self) { logLevel, message in
                wg_log(logLevel.osLogLevel, message: message)
            }

            wgAdapter?.start(tunnelConfiguration: tunnelConfiguration) { [weak self] adapterError in
                guard let adapterError else {
                    let interfaceName = self?.wgAdapter?.interfaceName ?? "unknown"
                    wg_log(.info, message: "Tunnel interface is \(interfaceName)")
                    completionHandler(nil)
                    return
                }

                switch adapterError {
                case .cannotLocateTunnelFileDescriptor:
                    wg_log(.error, staticMessage: "Starting tunnel failed: could not determine file descriptor")
                    errorNotifier.notify(PacketTunnelProviderError.couldNotDetermineFileDescriptor)
                    completionHandler(PacketTunnelProviderError.couldNotDetermineFileDescriptor)
                case .dnsResolution(let dnsErrors):
                    let hostnamesWithDnsResolutionFailure = dnsErrors.map { $0.address }
                        .joined(separator: ", ")
                    wg_log(.error, message:
                            "DNS resolution failed for the following hostnames: \(hostnamesWithDnsResolutionFailure)")
                    errorNotifier.notify(PacketTunnelProviderError.dnsResolutionFailure)
                    completionHandler(PacketTunnelProviderError.dnsResolutionFailure)
                case .setNetworkSettings(let error):
                    wg_log(.error, message:
                            "Starting tunnel failed with setTunnelNetworkSettings returning \(error.localizedDescription)")
                    errorNotifier.notify(PacketTunnelProviderError.couldNotSetNetworkSettings)
                    completionHandler(PacketTunnelProviderError.couldNotSetNetworkSettings)
                case .startWireGuardBackend(let errorCode):
                    wg_log(.error, message: "Starting tunnel failed with wgTurnOn returning \(errorCode)")
                    errorNotifier.notify(PacketTunnelProviderError.couldNotStartBackend)
                    completionHandler(PacketTunnelProviderError.couldNotStartBackend)
                case .invalidState:
                    fatalError()
                }
            }
        } catch {
            wg_log(.error, message: "Can't parse WG config: \(String(describing: error))")
            errorNotifier.notify(PacketTunnelProviderError.savedProtocolConfigurationIsInvalid)
            completionHandler(PacketTunnelProviderError.savedProtocolConfigurationIsInvalid)
            return
        }
    }

    /// Routing profiles: hands the configuration of the router built into amneziawg-go over to it.
    /// The configuration is written as is (the app already serialized it, it is never parsed here:
    /// the extension is memory constrained) to a private file of the extension, overwritten on every
    /// start. amneziawg-go loads it in wgTurnOn right after the tunnel device is created and before
    /// the device is up, also when the backend is restarted after a network outage. Without a
    /// configuration the router is disabled, which also clears the configuration of a previous start
    /// in the same process. The router's "direct" sockets need no protection: sockets of the
    /// extension are not routed into its own tunnel.
    private func setWireguardRoutingConfig(_ configData: Data?) -> Bool {
        let fileURL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent(Constants.routingConfigFileName, isDirectory: false)

        guard let configData, !configData.isEmpty else {
            _ = wgSetRoutingConfigFile("")
            if let fileURL, FileManager.default.fileExists(atPath: fileURL.path) {
                try? FileManager.default.removeItem(at: fileURL)
            }
            return true
        }

        guard let fileURL else {
            wg_log(.error, staticMessage: "Routing: can't locate the application support directory")
            return false
        }

        do {
            try FileManager.default.createDirectory(at: fileURL.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            var options: Data.WritingOptions = [.atomic]
#if os(iOS)
            // The backend is restarted (and the file read again) while the device may be locked
            options.insert(.completeFileProtectionUntilFirstUserAuthentication)
#endif
            try configData.write(to: fileURL, options: options)
        } catch {
            wg_log(.error, message: "Routing: can't write the router configuration: \(error.localizedDescription)")
            return false
        }

        guard wgSetRoutingConfigFile(fileURL.path) == 0 else {
            wg_log(.error, staticMessage: "Routing: amneziawg-go rejected the router configuration")
            return false
        }

        wg_log(.info, message: "Routing: router configuration of \(configData.count) bytes is used")
        return true
    }

    func handleWireguardStatusMessage(_ messageData: Data, completionHandler: ((Data?) -> Void)? = nil) {
        guard let completionHandler = completionHandler else { return }
        guard let wgAdapter = wgAdapter else {
            completionHandler(nil)
            return
        }
        wgAdapter.getRuntimeConfiguration { settings in
            guard let settings = settings else {
                completionHandler(nil)
                return
            }
            let components = settings.components(separatedBy: "\n")

            var settingsDictionary: [String: String] = [:]
            for component in components {
                let pair = component.components(separatedBy: "=")
                if pair.count == 2 {
                    settingsDictionary[pair[0]] = pair[1]
                }
            }

            let lastHandshakeString = settingsDictionary["last_handshake_time_sec"]
            let lastHandshake: Int64

            if let lastHandshakeValue = lastHandshakeString, let handshakeValue = Int64(lastHandshakeValue) {
                lastHandshake = handshakeValue
            } else {
                lastHandshake = -2  // Return an error if there is no value for `last_handshake_time_sec`
            }

            let response: [String: Any] = [
                "rx_bytes": settingsDictionary["rx_bytes"] ?? "0",
                "tx_bytes": settingsDictionary["tx_bytes"] ?? "0",
                "last_handshake_time_sec": lastHandshake
            ]

            completionHandler(try? JSONSerialization.data(withJSONObject: response, options: []))
        }
    }

    func handleWireguardAppMessage(_ messageData: Data, completionHandler: ((Data?) -> Void)? = nil) {
        guard let completionHandler = completionHandler else { return }
        if messageData.count == 1 && messageData[0] == 0 {
            wgAdapter?.getRuntimeConfiguration { settings in
                var data: Data?
                if let settings {
                    data = settings.data(using: .utf8)!
                }
                completionHandler(data)
            }
        } else if messageData.count >= 1 {
            // Updates the tunnel configuration and responds with the active configuration
            wg_log(.info, message: "Switching tunnel configuration")
            guard let configString = String(data: messageData, encoding: .utf8)
            else {
                completionHandler(nil)
                return
            }

            do {
                let tunnelConfiguration = try TunnelConfiguration(fromWgQuickConfig: configString)
                wgAdapter?.update(tunnelConfiguration: tunnelConfiguration) { [weak self] error in
                    if let error {
                        wg_log(.error, message: "Failed to switch tunnel configuration: \(error.localizedDescription)")
                        completionHandler(nil)
                        return
                    }

                    self?.wgAdapter?.getRuntimeConfiguration { settings in
                        var data: Data?
                        if let settings {
                            data = settings.data(using: .utf8)!
                        }
                        completionHandler(data)
                    }
                }
            } catch {
                completionHandler(nil)
            }
        } else {
            completionHandler(nil)
        }
    }

    func stopWireguard(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        wg_log(.info, message: "Stopping tunnel: reason: \(reason.amneziaDescription)")

        wgAdapter?.stop { error in
            ErrorNotifier.removeLastErrorFile()

            if let error {
                wg_log(.error, message: "Failed to stop WireGuard adapter: \(error.localizedDescription)")
            }
            completionHandler()

#if os(macOS)
            // HACK: This is a filthy hack to work around Apple bug 32073323 (dup'd by us as 47526107).
            // Remove it when they finally fix this upstream and the fix has been rolled out to
            // sufficient quantities of users.
            exit(0)
#endif
        }
    }
}
