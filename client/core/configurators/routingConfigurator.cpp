#include "routingConfigurator.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace
{
    using amnezia::RoutingProfile;
    using amnezia::RuleOrder;
    using amnezia::RuleOutbound;

    constexpr QLatin1String kTagProxy("proxy");
    constexpr QLatin1String kTagDirect("direct");
    constexpr QLatin1String kTagBlock("block");

    QString outboundTag(RuleOutbound outbound)
    {
        switch (outbound) {
        case RuleOutbound::Direct: return kTagDirect;
        case RuleOutbound::Block: return kTagBlock;
        case RuleOutbound::Proxy:
        default: return kTagProxy;
        }
    }

    QString domainStrategyToString(amnezia::DomainStrategy strategy)
    {
        switch (strategy) {
        case amnezia::DomainStrategy::IPOnDemand: return QStringLiteral("IPOnDemand");
        case amnezia::DomainStrategy::AsIs: return QStringLiteral("AsIs");
        case amnezia::DomainStrategy::IPIfNonMatch:
        default: return QStringLiteral("IPIfNonMatch");
        }
    }

    QVector<RuleOutbound> orderSequence(RuleOrder order)
    {
        switch (order) {
        case RuleOrder::BlockProxyDirect: return { RuleOutbound::Block, RuleOutbound::Proxy, RuleOutbound::Direct };
        case RuleOrder::ProxyDirectBlock: return { RuleOutbound::Proxy, RuleOutbound::Direct, RuleOutbound::Block };
        case RuleOrder::ProxyBlockDirect: return { RuleOutbound::Proxy, RuleOutbound::Block, RuleOutbound::Direct };
        case RuleOrder::DirectProxyBlock: return { RuleOutbound::Direct, RuleOutbound::Proxy, RuleOutbound::Block };
        case RuleOrder::DirectBlockProxy: return { RuleOutbound::Direct, RuleOutbound::Block, RuleOutbound::Proxy };
        case RuleOrder::BlockDirectProxy:
        default: return { RuleOutbound::Block, RuleOutbound::Direct, RuleOutbound::Proxy };
        }
    }

    void sitesAndIpForOutbound(const RoutingProfile &profile, RuleOutbound outbound, QStringList &sites, QStringList &ip)
    {
        switch (outbound) {
        case RuleOutbound::Direct:
            sites = profile.directSites;
            ip = profile.directIp;
            break;
        case RuleOutbound::Block:
            sites = profile.blockSites;
            ip = profile.blockIp;
            break;
        case RuleOutbound::Proxy:
        default:
            sites = profile.proxySites;
            ip = profile.proxyIp;
            break;
        }
    }
}

namespace amnezia
{
    QJsonObject RoutingConfigurator::buildRoutingSection(const RoutingProfile &profile)
    {
        QJsonObject routing;
        routing[QStringLiteral("domainStrategy")] = domainStrategyToString(profile.domainStrategy);

        QJsonArray rules;
        const QVector<RuleOutbound> sequence = orderSequence(profile.order);
        for (const RuleOutbound outbound : sequence) {
            QStringList sites;
            QStringList ip;
            sitesAndIpForOutbound(profile, outbound, sites, ip);
            const QString tag = outboundTag(outbound);

            if (!sites.isEmpty()) {
                QJsonObject rule;
                rule[QStringLiteral("type")] = QStringLiteral("field");
                rule[QStringLiteral("domain")] = QJsonArray::fromStringList(sites);
                rule[QStringLiteral("outboundTag")] = tag;
                rules.append(rule);
            }
            if (!ip.isEmpty()) {
                QJsonObject rule;
                rule[QStringLiteral("type")] = QStringLiteral("field");
                rule[QStringLiteral("ip")] = QJsonArray::fromStringList(ip);
                rule[QStringLiteral("outboundTag")] = tag;
                rules.append(rule);
            }
        }

        routing[QStringLiteral("rules")] = rules;
        return routing;
    }

    QJsonObject RoutingConfigurator::buildDnsServer(const RoutingDnsServer &server, const QStringList &domains)
    {
        QJsonObject obj;
        const QString address = server.mode == DnsMode::DoU ? server.ip : server.domain;
        obj[QStringLiteral("address")] = address;
        if (server.mode == DnsMode::DoH && !server.ip.isEmpty()) {
            // Bootstrap IP for the DoH endpoint so the resolver does not need
            // to resolve its own hostname through the tunnel.
            obj[QStringLiteral("ip")] = server.ip;
        }
        if (!domains.isEmpty()) {
            obj[QStringLiteral("domains")] = QJsonArray::fromStringList(domains);
        }
        return obj;
    }

    QJsonObject RoutingConfigurator::buildDnsSection(const RoutingProfile &profile)
    {
        QJsonObject dns;

        if (!profile.dnsHosts.isEmpty()) {
            dns[QStringLiteral("hosts")] = profile.dnsHosts;
        }

        QJsonArray servers;
        if (!profile.remoteDns.isEmpty()) {
            const QJsonObject remote = buildDnsServer(profile.remoteDns, profile.proxySites);
            if (!remote.value(QStringLiteral("address")).toString().isEmpty()) {
                servers.append(remote);
            }
        }
        if (!profile.domesticDns.isEmpty()) {
            const QJsonObject domestic = buildDnsServer(profile.domesticDns, profile.directSites);
            if (!domestic.value(QStringLiteral("address")).toString().isEmpty()) {
                servers.append(domestic);
            }
        }
        if (!servers.isEmpty()) {
            dns[QStringLiteral("servers")] = servers;
        }

        return dns;
    }

    QString RoutingConfigurator::applyToXrayConfig(const QString &xrayConfig, const RoutingProfile &profile, bool &ok)
    {
        ok = false;

        QJsonParseError parseError {};
        const QJsonDocument doc = QJsonDocument::fromJson(xrayConfig.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return xrayConfig;
        }

        QJsonObject root = doc.object();

        // --- outbounds: tag the existing proxy outbound, add direct + block ---
        QJsonArray outbounds = root.value(QStringLiteral("outbounds")).toArray();
        if (outbounds.isEmpty()) {
            return xrayConfig;
        }

        QJsonObject proxyOutbound = outbounds.first().toObject();
        if (proxyOutbound.value(QStringLiteral("tag")).toString().isEmpty()) {
            proxyOutbound[QStringLiteral("tag")] = kTagProxy;
        }

        QJsonObject directOutbound;
        directOutbound[QStringLiteral("protocol")] = QStringLiteral("freedom");
        directOutbound[QStringLiteral("tag")] = kTagDirect;

        QJsonObject blockOutbound;
        blockOutbound[QStringLiteral("protocol")] = QStringLiteral("blackhole");
        blockOutbound[QStringLiteral("tag")] = kTagBlock;

        // outbounds[0] is the default route for traffic that matches no rule.
        QJsonArray newOutbounds;
        if (profile.globalProxy) {
            newOutbounds.append(proxyOutbound);
            newOutbounds.append(directOutbound);
        } else {
            newOutbounds.append(directOutbound);
            newOutbounds.append(proxyOutbound);
        }
        newOutbounds.append(blockOutbound);

        // Preserve any extra outbounds that were already present (beyond the first).
        for (int i = 1; i < outbounds.size(); ++i) {
            newOutbounds.append(outbounds.at(i));
        }
        root[QStringLiteral("outbounds")] = newOutbounds;

        // --- inbounds: enable sniffing so domain rules can match ---
        QJsonArray sniffOverride = { QStringLiteral("http"), QStringLiteral("tls"), QStringLiteral("quic") };
        if (profile.fakeDns) {
            sniffOverride.append(QStringLiteral("fakedns"));
        }
        QJsonObject sniffing;
        sniffing[QStringLiteral("enabled")] = true;
        sniffing[QStringLiteral("destOverride")] = sniffOverride;
        sniffing[QStringLiteral("routeOnly")] = true;

        QJsonArray inbounds = root.value(QStringLiteral("inbounds")).toArray();
        for (int i = 0; i < inbounds.size(); ++i) {
            QJsonObject inbound = inbounds.at(i).toObject();
            inbound[QStringLiteral("sniffing")] = sniffing;
            inbounds[i] = inbound;
        }
        if (!inbounds.isEmpty()) {
            root[QStringLiteral("inbounds")] = inbounds;
        }

        // --- routing ---
        root[QStringLiteral("routing")] = buildRoutingSection(profile);

        // --- dns (only when the profile actually configures resolvers/hosts) ---
        const QJsonObject dns = buildDnsSection(profile);
        if (!dns.isEmpty()) {
            root[QStringLiteral("dns")] = dns;
        }

        ok = true;
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
}
