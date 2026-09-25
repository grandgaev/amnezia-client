#include "vpnConnection.h"

#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <core/configurators/openVpnConfigurator.h>
#include <core/configurators/wireguardConfigurator.h>

#ifdef AMNEZIA_DESKTOP
    #include "core/utils/ipcClient.h"
    #include <core/protocols/wireGuardProtocol.h>
    #include <QRemoteObjectPendingCallWatcher>
#endif

#ifdef Q_OS_ANDROID
    #include "platforms/android/android_controller.h"
    #include <QThread>

#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    #include "platforms/ios/ios_controller.h"
#endif

#include "core/controllers/routingController.h"
#include "core/models/routing/routingCompiler.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/serverConfigUtils.h"
#include "vpnConnection.h"

using namespace ProtocolUtils;

namespace
{
    // Upper bounds for resolving the host names of address based routing
    // (protocols without domain based routing).
    constexpr int maxRoutingHostnames = 256;
    constexpr int routingResolveTimeoutMs = 4000;
    // Time given to an AmneziaWG tunnel to show traffic after a network change.
    constexpr int networkCheckIntervalMs = 20000;

    QStringList resolveHostnames(const QStringList &hostnames, int timeoutMs)
    {
        QStringList result;
        if (hostnames.isEmpty()) {
            return result;
        }
        QEventLoop loop;
        int remaining = int(hostnames.size());
        QList<int> lookupIds;
        for (const QString &host : hostnames) {
            lookupIds.append(QHostInfo::lookupHost(host, &loop, [&](const QHostInfo &info) {
                for (const QHostAddress &address : info.addresses()) {
                    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                        result.append(address.toString());
                    }
                }
                if (--remaining == 0) {
                    loop.quit();
                }
            }));
        }
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
        for (int id : lookupIds) {
            QHostInfo::abortHostLookup(id);
        }
        result.removeDuplicates();
        return result;
    }
} // namespace

VpnConnection::VpnConnection(SecureServersRepository* serversRepository, SecureAppSettingsRepository* appSettingsRepository, QObject *parent)
    : QObject(parent),
      m_serversRepository(serversRepository),
      m_appSettingsRepository(appSettingsRepository),
      m_checkTimer(this),
      m_connectionState(Vpn::ConnectionState::Disconnected)
{
#if defined(Q_OS_IOS) || defined(MACOS_NE)
    m_checkTimer.setInterval(1000);
    connect(IosController::Instance(), &IosController::connectionStateChanged, this, &VpnConnection::setConnectionState);
    connect(IosController::Instance(), &IosController::bytesChanged, this, &VpnConnection::onBytesChanged);
#endif
}

VpnConnection::~VpnConnection()
{
}

void VpnConnection::onBytesChanged(quint64 receivedBytes, quint64 sentBytes)
{
    m_rxSinceNetworkChange += receivedBytes;
    emit bytesChanged(receivedBytes, sentBytes);
}

void VpnConnection::onKillSwitchModeChanged(bool enabled)
{
#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([enabled](QSharedPointer<IpcInterfaceReplica> iface){
        QRemoteObjectPendingReply<bool> reply = iface->refreshKillSwitch(enabled);
        if (reply.waitForFinished() && reply.returnValue())
            qDebug() << "VpnConnection::onKillSwitchModeChanged: Killswitch refreshed";
        else
            qWarning() << "VpnConnection::onKillSwitchModeChanged: Failed to execute remote refreshKillSwitch call";
    });
#endif
}

void VpnConnection::onConnectionStateChanged(Vpn::ConnectionState state)
{
#ifdef AMNEZIA_DESKTOP
    if (!m_serversRepository || !m_appSettingsRepository) {
        qCritical() << "VpnConnection::onConnectionStateChanged: repositories not initialized";
        return;
    }

    const QString defaultServerId = m_serversRepository->defaultServerId();
    DockerContainer container = DockerContainer::None;
    switch (m_serversRepository->serverKind(defaultServerId)) {
    case serverConfigUtils::ConfigType::SelfHostedAdmin: {
        const auto cfg = m_serversRepository->selfHostedAdminConfig(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::SelfHostedUser: {
        const auto cfg = m_serversRepository->selfHostedUserConfig(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::Native: {
        const auto cfg = m_serversRepository->nativeConfig(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::AmneziaPremiumV2:
    case serverConfigUtils::ConfigType::AmneziaFreeV3:
    case serverConfigUtils::ConfigType::ExternalPremium: {
        const auto cfg = m_serversRepository->apiV2Config(defaultServerId);
        if (cfg.has_value()) {
            container = cfg->defaultContainer;
        }
        break;
    }
    case serverConfigUtils::ConfigType::AmneziaPremiumV1:
    case serverConfigUtils::ConfigType::AmneziaFreeV2:
        break;
    case serverConfigUtils::ConfigType::Invalid:
    default:
        break;
    }

    IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
        switch (state) {
            case Vpn::ConnectionState::Connected: {
                iface->resetIpStack();

                auto flushDns = iface->flushDns();
                if (flushDns.waitForFinished() && flushDns.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully flushed DNS";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to flush DNS";

                if (!ContainerUtils::isAwgContainer(container) && container != DockerContainer::WireGuard) {
                    QString dns1 = m_vpnConfiguration.value(configKey::dns1).toString();
                    QString dns2 = m_vpnConfiguration.value(configKey::dns2).toString();
                    const auto routeMode = static_cast<amnezia::RouteMode>(m_vpnConfiguration.value(configKey::splitTunnelType).toInt());

#ifdef Q_OS_MACOS
                    if (routeMode != amnezia::RouteMode::VpnAllExceptSites) {
                        iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << dns1 << dns2);
                    }
#else
                    iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << dns1 << dns2);
#endif

                    // Address based routing of the routing profile (protocols without domain based routing).
                    if (routeMode != amnezia::RouteMode::VpnAllSites) {
                        iface->routeDeleteList(m_vpnProtocol->vpnGateway(), QStringList() << "0.0.0.0");
                        if (routeMode == amnezia::RouteMode::VpnOnlyForwardSites) {
                            QTimer::singleShot(1000, m_vpnProtocol.data(),
                                               [this]() { addSitesRoutes(m_vpnProtocol->vpnGateway(), amnezia::RouteMode::VpnOnlyForwardSites); });
                        } else if (routeMode == amnezia::RouteMode::VpnAllExceptSites) {
                            iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << "0.0.0.0/1");
                            iface->routeAddList(m_vpnProtocol->vpnGateway(), QStringList() << "128.0.0.0/1");

                            iface->routeAddList(m_vpnProtocol->routeGateway(), QStringList() << remoteAddress());
#ifdef Q_OS_MACOS
                            iface->routeAddList(m_vpnProtocol->routeGateway(), QStringList() << dns1 << dns2);
#endif
                            addSitesRoutes(m_vpnProtocol->routeGateway(), amnezia::RouteMode::VpnAllExceptSites);
                        }
                    }
                }
            } break;
            case Vpn::ConnectionState::Disconnected:
            case Vpn::ConnectionState::Error: {
                auto flushDns = iface->flushDns();
                if (flushDns.waitForFinished() && flushDns.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully flushed DNS";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to flush DNS";

                auto clearSavedRoutes = iface->clearSavedRoutes();
                if (clearSavedRoutes.waitForFinished() && clearSavedRoutes.returnValue())
                    qDebug() << "VpnConnection::onConnectionStateChanged: Successfully cleared saved routes";
                else
                    qWarning() << "VpnConnection::onConnectionStateChanged: Failed to clear saved routes";
            } break;
            default:
                break;
        }
    });
#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    if (state == Vpn::ConnectionState::Connected ||
        state == Vpn::ConnectionState::Connecting ||
        state == Vpn::ConnectionState::Reconnecting) {
        m_checkTimer.start();
    } else {
        m_checkTimer.stop();
    }
#endif
}

const QString &VpnConnection::remoteAddress() const
{
    return m_remoteAddress;
}

void VpnConnection::setRepositories(SecureServersRepository* serversRepository, SecureAppSettingsRepository* appSettingsRepository)
{
    m_serversRepository = serversRepository;
    m_appSettingsRepository = appSettingsRepository;
}

void VpnConnection::addSitesRoutes(const QString &gw, amnezia::RouteMode mode)
{
#ifdef AMNEZIA_DESKTOP
    Q_UNUSED(mode)

    QStringList ips;
    for (const QJsonValue &value : m_vpnConfiguration.value(configKey::splitTunnelSites).toArray()) {
        const QString ip = value.toString();
        if (NetworkUtilities::checkIpSubnetFormat(ip)) {
            ips.append(ip);
        }
    }
    ips.removeDuplicates();

    IpcClient::withInterface([&](QSharedPointer<IpcInterfaceReplica> iface) {
        iface->routeAddList(gw, ips);
    });

    // Resolve the host names again once the tunnel is up: the addresses returned
    // through the VPN DNS may differ from the ones resolved before connecting.
    const QStringList sites = m_routingHostnames;
    auto remainingLookups = QSharedPointer<int>::create(int(sites.size()));
    auto needFlush = QSharedPointer<bool>::create(false);

    for (const QString &site : sites) {
        const auto &cbResolv = [this, site, gw, ips, remainingLookups, needFlush](const QHostInfo &hostInfo) {
            QStringList newIps;
            for (const QHostAddress &addr : hostInfo.addresses()) {
                const QString ip = addr.toString();
                if (addr.protocol() == QAbstractSocket::NetworkLayerProtocol::IPv4Protocol && !ips.contains(ip) && !newIps.contains(ip)) {
                    newIps.append(ip);
                }
            }
            qDebug() << "[Routing] addSitesRoutes resolved" << site << "->" << newIps;

            if (!newIps.isEmpty()) {
                IpcClient::withInterface([gw, newIps](QSharedPointer<IpcInterfaceReplica> iface) {
                    iface->routeAddList(gw, newIps);
                });
                *needFlush = true;
            }

            if (--(*remainingLookups) > 0)
                return;

            if (!*needFlush)
                return;

            // Async flush: never waitForFinished() here — that re-enters the event loop and
            // can re-enter this QHostInfo callback until the stack overflows (0xc00000fd).
            IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> iface) {
                QRemoteObjectPendingReply<bool> reply = iface->flushDns();
                auto *watcher = new QRemoteObjectPendingCallWatcher(reply, this);
                QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
                        [](QRemoteObjectPendingCallWatcher *call) {
                            if (call->error() != QRemoteObjectPendingCall::NoError
                                || !call->returnValue().toBool()) {
                                qWarning() << "VpnConnection::addSitesRoutes: Failed to flush DNS";
                            }
                            call->deleteLater();
                        });
            });
        };
        QHostInfo::lookupHost(site, this, cbResolv);
    }
#else
    Q_UNUSED(gw)
    Q_UNUSED(mode)
#endif
}

QJsonObject VpnConnection::withRoutingConfiguration(const QString &serverId, const QJsonObject &vpnConfiguration)
{
    m_vpnConfiguration = vpnConfiguration;
    appendSplitTunnelingConfig(serverId);
    return m_vpnConfiguration;
}

QSharedPointer<VpnProtocol> VpnConnection::vpnProtocol() const
{
    return m_vpnProtocol;
}

void VpnConnection::disconnectSlots()
{
    if (m_vpnProtocol) {
        m_vpnProtocol->disconnect();
    }
}

ErrorCode VpnConnection::lastError() const
{
#ifdef Q_OS_ANDROID
    return ErrorCode::AndroidError;
#endif

    if (m_vpnProtocol.isNull()) {
        return ErrorCode::InternalError;
    }

    return m_vpnProtocol.data()->lastError();
}

Vpn::ConnectionState VpnConnection::connectionState() const
{
    return m_connectionState;
}

void VpnConnection::connectToVpn(const QString &serverId, DockerContainer container, const QJsonObject &vpnConfiguration)
{
    if (!m_appSettingsRepository || !m_serversRepository) {
        qCritical() << "VpnConnection::connectToVpn: repositories not initialized";
        setConnectionState(Vpn::ConnectionState::Error);
        return;
    }

#ifdef Q_OS_ANDROID
    // The Android service ignores a connect request while a tunnel is up:
    // stop the running tunnel first and connect when it is down.
    if (m_vpnProtocol && isActiveState(m_connectionState)) {
        switchAndroidTunnel(serverId, container, vpnConfiguration);
        return;
    }
#endif

    // Connecting while a tunnel is active switches to the new server/protocol/settings.
    const bool switching = isActiveState(m_connectionState);

    qDebug() << QString("Trying to connect to VPN, server id is %1, container is %2%3")
                        .arg(serverId)
                        .arg(ContainerUtils::containerToString(container))
                        .arg(switching ? ", replacing the active connection" : "");

    m_remoteAddress = NetworkUtilities::getIPAddress(vpnConfiguration.value(configKey::hostName).toString());
    if (switching) {
        // The previous tunnel reports Disconnected while it goes down.
        m_suppressDisconnected = true;
        setConnectionState(Vpn::ConnectionState::Reconnecting);
    } else {
        m_suppressDisconnected = false;
        setConnectionState(Vpn::ConnectionState::Connecting);
    }

    m_vpnConfiguration = vpnConfiguration;

#ifdef AMNEZIA_DESKTOP
    if (m_vpnProtocol) {
        // Late signals of the previous protocol must not change the state of the new connection.
        disconnect(m_vpnProtocol.data(), nullptr, this, nullptr);
        m_vpnProtocol->stop();
        m_vpnProtocol.reset();
    }
    appendKillSwitchConfig();
#endif

    appendSplitTunnelingConfig(serverId);

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS) && !defined(MACOS_NE)
    m_vpnProtocol.reset(VpnProtocol::factory(container, m_vpnConfiguration));
    if (!m_vpnProtocol) {
        setConnectionState(Vpn::ConnectionState::Error);
        return;
    }
    m_vpnProtocol->prepare();
#elif defined Q_OS_ANDROID
    createAndroidConnections();

    m_vpnProtocol.reset(androidVpnProtocol);
#elif defined Q_OS_IOS || defined(MACOS_NE)
    Proto proto = ContainerUtils::defaultProtocol(container);
    IosController::Instance()->connectVpn(proto, m_vpnConfiguration);
    connect(&m_checkTimer, &QTimer::timeout, IosController::Instance(), &IosController::checkStatus, Qt::UniqueConnection);
    return;
#endif

    createProtocolConnections();

    if (ErrorCode err = m_vpnProtocol->start(); err != ErrorCode::NoError) {
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(err);
    }
}

void VpnConnection::createProtocolConnections()
{
    connect(m_vpnProtocol.data(), &VpnProtocol::protocolError, this, &VpnConnection::vpnProtocolError);
    connect(m_vpnProtocol.data(), &VpnProtocol::connectionStateChanged, this, &VpnConnection::setConnectionState);
    connect(m_vpnProtocol.data(), SIGNAL(bytesChanged(quint64, quint64)), this, SLOT(onBytesChanged(quint64, quint64)));

#ifdef AMNEZIA_DESKTOP
    IpcClient::withInterface([this](QSharedPointer<IpcInterfaceReplica> rep) {
        connect(rep.data(), &IpcInterfaceReplica::networkChanged, this, &VpnConnection::onNetworkChanged,
                static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
        connect(rep.data(), &IpcInterfaceReplica::wakeup, this, &VpnConnection::onNetworkChanged,
                static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
    });
#endif
}

void VpnConnection::appendKillSwitchConfig()
{
    if (!m_appSettingsRepository) {
        qCritical() << "VpnConnection::appendKillSwitchConfig: repositories not initialized";
        return;
    }

    m_vpnConfiguration.insert(configKey::killSwitchOption, QVariant(m_appSettingsRepository->isKillSwitchEnabled()).toString());
    m_vpnConfiguration.insert(configKey::allowedDnsServers, QVariant(m_appSettingsRepository->getAllowedDnsServers()).toJsonValue());
}

void VpnConnection::appendSplitTunnelingConfig(const QString &serverId)
{
    if (!m_appSettingsRepository) {
        qCritical() << "VpnConnection::appendSplitTunnelingConfig: repositories not initialized";
        return;
    }

    bool allowSiteBasedSplitTunneling = true;

    // this block is for old native configs and for old self-hosted configs
    auto protocolName = m_vpnConfiguration.value(configKey::vpnProto).toString();
    if (protocolName == ProtocolUtils::protoToString(Proto::Awg) || protocolName == ProtocolUtils::protoToString(Proto::WireGuard)) {
        allowSiteBasedSplitTunneling = false;
        auto configData = m_vpnConfiguration.value(protocolName + "_config_data").toObject();
        if (configData.value(configKey::allowedIps).isString()) {
            QJsonArray allowedIpsJsonArray = QJsonArray::fromStringList(configData.value(configKey::allowedIps).toString().split(", "));
            configData.insert(configKey::allowedIps, allowedIpsJsonArray);
            m_vpnConfiguration.insert(protocolName + "_config_data", configData);
        } else if (configData.value(configKey::allowedIps).isUndefined()) {
            auto nativeConfig = configData.value(configKey::config).toString();
            auto nativeConfigLines = nativeConfig.split("\n");
            for (auto &line : nativeConfigLines) {
                auto allowedIpsString = line.split("=", Qt::KeepEmptyParts);
                if (allowedIpsString.size() >= 2 && allowedIpsString.first().trimmed() == QStringLiteral("AllowedIPs")) {
                    QJsonArray allowedIpsJsonArray;
                    const QString allowedIps = allowedIpsString.mid(1).join(QStringLiteral("=")).trimmed();
                    for (const QString &allowedIp : allowedIps.split(",", Qt::SkipEmptyParts)) {
                        allowedIpsJsonArray.append(allowedIp.trimmed());
                    }
                    configData.insert(configKey::allowedIps, allowedIpsJsonArray);
                    m_vpnConfiguration.insert(protocolName + "_config_data", configData);
                    break;
                }
            }
        }

        if (configData.value(configKey::persistentKeepAlive).isUndefined()) {
            auto nativeConfig = configData.value(configKey::config).toString();
            auto nativeConfigLines = nativeConfig.split("\n");
            for (auto &line : nativeConfigLines) {
                auto persistentKeepaliveString = line.split("=", Qt::KeepEmptyParts);
                if (persistentKeepaliveString.size() >= 2
                        && persistentKeepaliveString.first().trimmed() == QStringLiteral("PersistentKeepalive")) {
                    configData.insert(configKey::persistentKeepAlive,
                                      persistentKeepaliveString.mid(1).join(QStringLiteral("=")).trimmed());
                    m_vpnConfiguration.insert(protocolName + "_config_data", configData);
                    break;
                }
            }
        }

        QJsonArray allowedIpsJsonArray = configData.value(configKey::allowedIps).toArray();
        if (allowedIpsJsonArray.contains("0.0.0.0/0") && allowedIpsJsonArray.contains("::/0")) {
            allowSiteBasedSplitTunneling = true;
        }
    }

    m_routingHostnames.clear();
    m_vpnConfiguration.insert(configKey::splitTunnelType, amnezia::RouteMode::VpnAllSites);
    m_vpnConfiguration.insert(configKey::splitTunnelSites, QJsonArray());
    m_vpnConfiguration.remove(configKey::routingConfig);
    appendRoutingConfig(serverId, allowSiteBasedSplitTunneling);

    amnezia::AppsRouteMode appsRouteMode = amnezia::AppsRouteMode::VpnAllApps;
    QJsonArray appsJsonArray;
    if (m_appSettingsRepository->isAppsSplitTunnelingEnabled()) {
        appsRouteMode = m_appSettingsRepository->appsRouteMode();

        auto apps = m_appSettingsRepository->vpnApps(appsRouteMode);
        for (const auto &app : apps) {
            appsJsonArray.append(app.appPath.isEmpty() ? app.packageName : app.appPath);
        }

        if (appsJsonArray.isEmpty()) {
            appsRouteMode = amnezia::AppsRouteMode::VpnAllApps;
        }
    }

    m_vpnConfiguration.insert(configKey::appSplitTunnelType, appsRouteMode);
    m_vpnConfiguration.insert(configKey::splitTunnelApps, appsJsonArray);

    qDebug() << QString("App split tunneling is %1, route mode is %2")
                        .arg(m_appSettingsRepository->isAppsSplitTunnelingEnabled() ? "enabled" : "disabled")
                        .arg(appsRouteMode);
}

void VpnConnection::appendRoutingConfig(const QString &serverId, bool fullTunnel)
{
    using namespace amnezia::routing;

    const auto profile = RoutingController::activeProfile(m_appSettingsRepository, serverId);
    if (!profile) {
        qDebug() << "Routing profiles: not used for this connection";
        return;
    }

    CompileInput input;
    input.profile = *profile;
    input.geoSitePath = RoutingController::geoFilePath(profile->id, false);
    input.geoIpPath = RoutingController::geoFilePath(profile->id, true);
    input.excludedRoutes = m_appSettingsRepository->routingExcludedRoutes();
    for (const QString &dns : { m_vpnConfiguration.value(configKey::dns1).toString(), m_vpnConfiguration.value(configKey::dns2).toString() }) {
        if (!dns.trimmed().isEmpty() && !input.connectionDns.contains(dns.trimmed())) {
            input.connectionDns.append(dns.trimmed());
        }
    }
#ifdef AMNEZIA_DESKTOP
    input.bypassMode = QStringLiteral("interface");
#else
    input.bypassMode = QStringLiteral("os");
#endif

    const ExpandedProfile expanded = RoutingCompiler::expand(input);
    for (const QString &warning : expanded.warnings) {
        qWarning() << "Routing profile" << profile->name << ":" << warning;
    }

    const QString protocolName = m_vpnConfiguration.value(configKey::vpnProto).toString();

    // AmneziaWG / WireGuard: rule based routing inside amneziawg-go.
    if (protocolName == protoToString(Proto::Awg) || protocolName == protoToString(Proto::WireGuard)) {
        const QString dnsAddress = QString::fromLatin1(routerDnsAddress);
        // Self-hosted servers give the client an IPv4 address only.
        const QString clientIp = m_vpnConfiguration.value(protocolName + "_config_data").toObject().value(configKey::clientIp).toString();
        input.tunnelHasIpv6 = clientIp.contains(QLatin1Char(':'));
#if defined(AMNEZIA_DESKTOP) && !defined(Q_OS_WIN)
        // The kill switch of the daemon blocks all IPv6 of the applications:
        // AAAA records would only make them try IPv6 first and fail.
        input.ipv6Blocked = m_appSettingsRepository && m_appSettingsRepository->isKillSwitchEnabled();
#endif
        m_vpnConfiguration.insert(configKey::routingConfig, RoutingCompiler::routerConfig(input, expanded));
        m_vpnConfiguration.insert(configKey::dns1, dnsAddress);
        m_vpnConfiguration.insert(configKey::dns2, dnsAddress);

        const QString configDataKey = protocolName + "_config_data";
        QJsonObject configData = m_vpnConfiguration.value(configDataKey).toObject();
        QJsonArray allowedIps = configData.value(configKey::allowedIps).toArray();
        if (!allowedIps.contains(QStringLiteral("0.0.0.0/0")) && !allowedIps.contains(dnsAddress + QStringLiteral("/32"))) {
            allowedIps.append(dnsAddress + QStringLiteral("/32"));
            configData.insert(configKey::allowedIps, allowedIps);
            m_vpnConfiguration.insert(configDataKey, configData);
        }
        qDebug() << "Routing profile" << profile->name << "applied by the AmneziaWG router, full tunnel:" << fullTunnel;
        return;
    }

    // Xray: routing rules of the Xray core.
    if (protocolName == protoToString(Proto::Xray) || protocolName == protoToString(Proto::SSXray)) {
        const QString configDataKey = key_proto_config_data(protocolName == protoToString(Proto::Xray) ? Proto::Xray : Proto::SSXray);
        QJsonObject configData = m_vpnConfiguration.value(configDataKey).toObject();
        QJsonObject xrayConfig = QJsonDocument::fromJson(configData.value(configKey::config).toString().toUtf8()).object();
        if (xrayConfig.isEmpty()) {
            qWarning() << "Routing profile: the Xray config is not a JSON object, routing is not applied";
            return;
        }
        RoutingCompiler::applyToXrayConfig(xrayConfig, input, expanded);
        configData.insert(configKey::config, QString::fromUtf8(QJsonDocument(xrayConfig).toJson(QJsonDocument::Compact)));
        m_vpnConfiguration.insert(configDataKey, configData);
        qDebug() << "Routing profile" << profile->name << "applied to the Xray config";
        return;
    }

    // Other protocols: address based routes only.
    if (!fullTunnel) {
        qWarning() << "Routing profile: the configuration does not route all traffic through the VPN, routing is not applied";
        return;
    }
    const IpRoutes routes = RoutingCompiler::ipRoutes(input, expanded);
    for (const QString &warning : routes.warnings) {
        qWarning() << "Routing profile" << profile->name << ":" << warning;
    }
    m_routingHostnames = routes.hostnames.mid(0, maxRoutingHostnames);
    if (routes.hostnames.size() > maxRoutingHostnames) {
        qWarning() << "Routing profile: only the first" << maxRoutingHostnames << "sites are resolved for address based routing";
    }

    QStringList addresses = routes.cidrs;
    addresses.append(resolveHostnames(m_routingHostnames, routingResolveTimeoutMs));
    addresses.removeDuplicates();
    if (addresses.isEmpty()) {
        qDebug() << "Routing profile" << profile->name << ": no address based rules, all traffic goes through the VPN";
        m_routingHostnames.clear();
        return;
    }

    QJsonArray sites = QJsonArray::fromStringList(addresses);
    if (routes.includeMode) {
        // Keep the DNS servers of the connection reachable through the tunnel.
        for (const QString &dns : input.connectionDns) {
            sites.append(dns);
        }
    }
    m_vpnConfiguration.insert(configKey::splitTunnelType,
                              routes.includeMode ? amnezia::RouteMode::VpnOnlyForwardSites : amnezia::RouteMode::VpnAllExceptSites);
    m_vpnConfiguration.insert(configKey::splitTunnelSites, sites);
    qDebug() << "Routing profile" << profile->name << "applied as" << addresses.size() << "address routes,"
             << (routes.includeMode ? "only listed via VPN" : "listed bypass the VPN");
}

#ifdef Q_OS_ANDROID
void VpnConnection::restoreConnection()
{
    createAndroidConnections();

    m_vpnProtocol.reset(androidVpnProtocol);

    createProtocolConnections();
}

void VpnConnection::createAndroidConnections()
{
    androidVpnProtocol = createDefaultAndroidVpnProtocol();

    connect(AndroidController::instance(), &AndroidController::connectionStateChanged, androidVpnProtocol,
            &AndroidVpnProtocol::setConnectionState);
    connect(AndroidController::instance(), &AndroidController::statisticsUpdated, androidVpnProtocol, &AndroidVpnProtocol::setBytesChanged);
}

AndroidVpnProtocol *VpnConnection::createDefaultAndroidVpnProtocol()
{
    return new AndroidVpnProtocol(m_vpnConfiguration);
}
#endif

QString VpnConnection::bytesPerSecToText(quint64 bytes)
{
    double mbps = bytes * 8 / 1e6;
    return QString("%1 %2").arg(QString::number(mbps, 'f', 2)).arg(tr("Mbps")); // Mbit/s
}

void VpnConnection::onNetworkChanged()
{
    if (m_vpnProtocol.isNull() || m_connectionState != Vpn::ConnectionState::Connected) {
        return;
    }

    const QString protocolName = m_vpnConfiguration.value(configKey::vpnProto).toString();
    if (protocolName == protoToString(Proto::Awg) || protocolName == protoToString(Proto::WireGuard)) {
        // AmneziaWG survives network changes: the daemon keeps the route to the
        // server up to date and the UDP socket follows the new default route. A full
        // reconnect would drop every open connection (browsers then report
        // ERR_SOCKET_NOT_CONNECTED), so only reconnect when no data comes back.
        qDebug() << "Network changed: checking the tunnel before reconnecting";
        m_rxSinceNetworkChange = 0;
        if (!m_networkCheckTimer) {
            m_networkCheckTimer = new QTimer(this);
            m_networkCheckTimer->setSingleShot(true);
            m_networkCheckTimer->setInterval(networkCheckIntervalMs);
            connect(m_networkCheckTimer, &QTimer::timeout, this, [this]() {
                if (m_connectionState == Vpn::ConnectionState::Connected && m_rxSinceNetworkChange == 0) {
                    qDebug() << "No data through the tunnel after the network change, reconnecting";
                    reconnectToVpn();
                }
            });
        }
        m_networkCheckTimer->start();
        return;
    }

    reconnectToVpn();
}

void VpnConnection::reconnectToVpn() {
    if (m_vpnProtocol.isNull())
        return;

    if (m_connectionState != Vpn::ConnectionState::Connected) {
        qWarning() << QString("Reconnect triggered on %1 during inappropriate state: %2; ignoring slot")
                              .arg(QMetaEnum::fromType<Vpn::ConnectionState>().valueToKey(m_connectionState));
        return;
    }

    qDebug() << "Reconnect triggered. Reconnecting to the server";

    m_suppressDisconnected = true;
    setConnectionState(Vpn::ConnectionState::Reconnecting);

    m_vpnProtocol->stop();
    if (ErrorCode err = m_vpnProtocol->start(); err != ErrorCode::NoError) {
        setConnectionState(Vpn::ConnectionState::Error);
        emit vpnProtocolError(err);
    }
}

void VpnConnection::disconnectFromVpn()
{
    m_suppressDisconnected = false;
#ifdef Q_OS_ANDROID
    if (m_androidSwitch.inProgress) {
        // The previous tunnel is already going down; drop the pending connection.
        cancelAndroidSwitch();
        m_vpnProtocol = nullptr;
        setConnectionState(Vpn::ConnectionState::Disconnected);
        return;
    }
#endif

#if defined(Q_OS_IOS) || defined(MACOS_NE)
    // iOS/macOS NE use IosController directly; m_vpnProtocol is not set there.
    IosController::Instance()->disconnectVpn();
    disconnect(&m_checkTimer, &QTimer::timeout, IosController::Instance(), &IosController::checkStatus);
#endif

    if (m_vpnProtocol.isNull()) {
        setConnectionState(Vpn::ConnectionState::Disconnected);
        return;
    }

    setConnectionState(Vpn::ConnectionState::Disconnecting);

#ifdef Q_OS_ANDROID
    auto *const connection = new QMetaObject::Connection;
    *connection = connect(AndroidController::instance(), &AndroidController::vpnStateChanged, this,
                          [this, connection](AndroidController::ConnectionState state) {
                              if (state == AndroidController::ConnectionState::DISCONNECTED) {
                                  setConnectionState(Vpn::ConnectionState::Disconnected);
                                  disconnect(*connection);
                                  delete connection;
                              }
                          });
#endif

    m_vpnProtocol->stop();

#if !defined(Q_OS_ANDROID) && !defined(AMNEZIA_DESKTOP)
    m_vpnProtocol->deleteLater();
#endif

    m_vpnProtocol = nullptr;
}

bool VpnConnection::isActiveState(Vpn::ConnectionState state)
{
    return state == Vpn::ConnectionState::Connected || state == Vpn::ConnectionState::Connecting
            || state == Vpn::ConnectionState::Reconnecting;
}

#ifdef Q_OS_ANDROID
void VpnConnection::switchAndroidTunnel(const QString &serverId, DockerContainer container, const QJsonObject &vpnConfiguration)
{
    // The latest request wins.
    m_androidSwitch.serverId = serverId;
    m_androidSwitch.container = container;
    m_androidSwitch.configuration = vpnConfiguration;
    if (m_androidSwitch.inProgress) {
        return;
    }
    m_androidSwitch.inProgress = true;
    const int generation = ++m_androidSwitch.generation;

    qDebug() << "Switching the active connection: stopping the current tunnel";
    m_suppressDisconnected = true;
    setConnectionState(Vpn::ConnectionState::Reconnecting);

    m_androidSwitch.stateConnection = connect(AndroidController::instance(), &AndroidController::vpnStateChanged, this,
                                              [this, generation](AndroidController::ConnectionState state) {
                                                  if (state == AndroidController::ConnectionState::DISCONNECTED) {
                                                      finishAndroidSwitch(generation);
                                                  }
                                              });
    // Do not wait forever for a tunnel that does not report its state.
    QTimer::singleShot(10000, this, [this, generation]() { finishAndroidSwitch(generation); });

    disconnect(m_vpnProtocol.data(), nullptr, this, nullptr);
    m_vpnProtocol->stop();
}

void VpnConnection::finishAndroidSwitch(int generation)
{
    if (!m_androidSwitch.inProgress || generation != m_androidSwitch.generation) {
        return;
    }
    const QString serverId = m_androidSwitch.serverId;
    const DockerContainer container = m_androidSwitch.container;
    const QJsonObject configuration = m_androidSwitch.configuration;
    cancelAndroidSwitch();
    m_vpnProtocol = nullptr;
    connectToVpn(serverId, container, configuration);
}

void VpnConnection::cancelAndroidSwitch()
{
    disconnect(m_androidSwitch.stateConnection);
    m_androidSwitch.inProgress = false;
    ++m_androidSwitch.generation;
    m_androidSwitch.configuration = QJsonObject();
}
#endif

void VpnConnection::setConnectionState(Vpn::ConnectionState state) {
    onConnectionStateChanged(state);

    if (m_suppressDisconnected) {
        // While switching, the previous tunnel reports Disconnected: keep showing Reconnecting.
        if (state == Vpn::Disconnected) {
            qDebug() << "VpnConnection: ignoring Disconnected of the previous tunnel";
            return;
        }
        if (state == Vpn::Connecting || state == Vpn::Connected || state == Vpn::Error || state == Vpn::Disconnecting) {
            m_suppressDisconnected = false;
        }
    }

    m_connectionState = state;
    emit connectionStateChanged(state);
}
