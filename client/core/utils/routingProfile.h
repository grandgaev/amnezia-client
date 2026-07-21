#ifndef ROUTINGPROFILE_H
#define ROUTINGPROFILE_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "core/utils/routeModes.h"

namespace amnezia
{
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
