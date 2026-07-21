#ifndef ROUTINGPROFILE_H
#define ROUTINGPROFILE_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "core/utils/routeModes.h"

namespace amnezia
{
    // Classification of a single split-tunneling rule string in xray syntax.
    // Used by the UI to validate input and by non-xray fallback to decide what
    // can be honored (only plain IP/CIDR outside the xray routing layer).
    enum class RoutingRuleKind {
        Invalid,
        Domain,   // example.com, domain:, full:, keyword:, regexp:, dotless:
        Geosite,  // geosite:xxx
        Ip,       // IPv4/IPv6 address or CIDR
        Geoip,    // geoip:xxx
        Ext       // ext:file:tag / ext-ip:file:tag
    };

    RoutingRuleKind classifyRoutingRule(const QString &rule);
    bool isDomainRule(RoutingRuleKind kind);
    bool isIpRule(RoutingRuleKind kind);

    struct RoutingDnsServer
    {
        DnsMode mode = DnsMode::DoH;
        QString domain;
        QString ip;

        bool isEmpty() const { return domain.isEmpty() && ip.isEmpty(); }
    };

    struct RoutingProfile
    {
        QString name;

        bool globalProxy = false;
        RuleOrder order = RuleOrder::BlockDirectProxy;
        DomainStrategy domainStrategy = DomainStrategy::IPIfNonMatch;
        bool fakeDns = false;

        QStringList proxySites;
        QStringList proxyIp;
        QStringList directSites;
        QStringList directIp;
        QStringList blockSites;
        QStringList blockIp;

        RoutingDnsServer remoteDns;
        RoutingDnsServer domesticDns;
        QJsonObject dnsHosts;

        QString geoipUrl;
        QString geositeUrl;
        QString lastUpdated;

        bool hasRules() const
        {
            return !proxySites.isEmpty() || !proxyIp.isEmpty() || !directSites.isEmpty() || !directIp.isEmpty()
                    || !blockSites.isEmpty() || !blockIp.isEmpty();
        }

        QJsonObject toJson() const;
        static RoutingProfile fromJson(const QJsonObject &obj);

        QString toDeeplink() const;
        static RoutingProfile fromDeeplink(const QString &deeplink, bool &ok);
    };
}

#endif // ROUTINGPROFILE_H
