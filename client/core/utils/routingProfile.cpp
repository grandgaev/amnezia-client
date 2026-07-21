#include "routingProfile.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace
{
    QString dnsModeToString(amnezia::DnsMode mode)
    {
        return mode == amnezia::DnsMode::DoU ? QStringLiteral("DoU") : QStringLiteral("DoH");
    }

    amnezia::DnsMode dnsModeFromString(const QString &value)
    {
        return value.compare(QLatin1String("DoU"), Qt::CaseInsensitive) == 0 ? amnezia::DnsMode::DoU
                                                                             : amnezia::DnsMode::DoH;
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

    amnezia::DomainStrategy domainStrategyFromString(const QString &value)
    {
        if (value.compare(QLatin1String("IPOnDemand"), Qt::CaseInsensitive) == 0) {
            return amnezia::DomainStrategy::IPOnDemand;
        }
        if (value.compare(QLatin1String("AsIs"), Qt::CaseInsensitive) == 0) {
            return amnezia::DomainStrategy::AsIs;
        }
        return amnezia::DomainStrategy::IPIfNonMatch;
    }

    QStringList jsonArrayToStringList(const QJsonArray &array)
    {
        QStringList result;
        result.reserve(array.size());
        for (const QJsonValue &value : array) {
            const QString entry = value.toString().trimmed();
            if (!entry.isEmpty()) {
                result.append(entry);
            }
        }
        return result;
    }
}

namespace amnezia
{
    QJsonObject RoutingProfile::toJson() const
    {
        QJsonObject obj;
        obj[QStringLiteral("Name")] = name;
        obj[QStringLiteral("GlobalProxy")] = globalProxy ? QStringLiteral("true") : QStringLiteral("false");

        obj[QStringLiteral("RemoteDNSType")] = dnsModeToString(remoteDns.mode);
        obj[QStringLiteral("RemoteDNSDomain")] = remoteDns.domain;
        obj[QStringLiteral("RemoteDNSIP")] = remoteDns.ip;
        obj[QStringLiteral("DomesticDNSType")] = dnsModeToString(domesticDns.mode);
        obj[QStringLiteral("DomesticDNSDomain")] = domesticDns.domain;
        obj[QStringLiteral("DomesticDNSIP")] = domesticDns.ip;

        obj[QStringLiteral("Geoipurl")] = geoipUrl;
        obj[QStringLiteral("Geositeurl")] = geositeUrl;
        obj[QStringLiteral("LastUpdated")] = lastUpdated;

        obj[QStringLiteral("DnsHosts")] = dnsHosts;

        obj[QStringLiteral("DirectSites")] = QJsonArray::fromStringList(directSites);
        obj[QStringLiteral("DirectIp")] = QJsonArray::fromStringList(directIp);
        obj[QStringLiteral("ProxySites")] = QJsonArray::fromStringList(proxySites);
        obj[QStringLiteral("ProxyIp")] = QJsonArray::fromStringList(proxyIp);
        obj[QStringLiteral("BlockSites")] = QJsonArray::fromStringList(blockSites);
        obj[QStringLiteral("BlockIp")] = QJsonArray::fromStringList(blockIp);

        obj[QStringLiteral("DomainStrategy")] = domainStrategyToString(domainStrategy);
        obj[QStringLiteral("FakeDNS")] = fakeDns ? QStringLiteral("true") : QStringLiteral("false");

        return obj;
    }

    RoutingProfile RoutingProfile::fromJson(const QJsonObject &obj)
    {
        RoutingProfile profile;
        profile.name = obj.value(QStringLiteral("Name")).toString();
        profile.globalProxy = obj.value(QStringLiteral("GlobalProxy")).toString().compare(QLatin1String("true"),
                                                                                          Qt::CaseInsensitive)
                == 0;

        profile.remoteDns.mode = dnsModeFromString(obj.value(QStringLiteral("RemoteDNSType")).toString());
        profile.remoteDns.domain = obj.value(QStringLiteral("RemoteDNSDomain")).toString();
        profile.remoteDns.ip = obj.value(QStringLiteral("RemoteDNSIP")).toString();
        profile.domesticDns.mode = dnsModeFromString(obj.value(QStringLiteral("DomesticDNSType")).toString());
        profile.domesticDns.domain = obj.value(QStringLiteral("DomesticDNSDomain")).toString();
        profile.domesticDns.ip = obj.value(QStringLiteral("DomesticDNSIP")).toString();

        profile.geoipUrl = obj.value(QStringLiteral("Geoipurl")).toString();
        profile.geositeUrl = obj.value(QStringLiteral("Geositeurl")).toString();
        profile.lastUpdated = obj.value(QStringLiteral("LastUpdated")).toString();

        profile.dnsHosts = obj.value(QStringLiteral("DnsHosts")).toObject();

        profile.directSites = jsonArrayToStringList(obj.value(QStringLiteral("DirectSites")).toArray());
        profile.directIp = jsonArrayToStringList(obj.value(QStringLiteral("DirectIp")).toArray());
        profile.proxySites = jsonArrayToStringList(obj.value(QStringLiteral("ProxySites")).toArray());
        profile.proxyIp = jsonArrayToStringList(obj.value(QStringLiteral("ProxyIp")).toArray());
        profile.blockSites = jsonArrayToStringList(obj.value(QStringLiteral("BlockSites")).toArray());
        profile.blockIp = jsonArrayToStringList(obj.value(QStringLiteral("BlockIp")).toArray());

        profile.domainStrategy = domainStrategyFromString(obj.value(QStringLiteral("DomainStrategy")).toString());
        profile.fakeDns = obj.value(QStringLiteral("FakeDNS")).toString().compare(QLatin1String("true"),
                                                                                  Qt::CaseInsensitive)
                == 0;

        return profile;
    }

    QString RoutingProfile::toDeeplink() const
    {
        const QByteArray json = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
        return QStringLiteral("happ://routing/add/") + QString::fromLatin1(json.toBase64());
    }

    RoutingProfile RoutingProfile::fromDeeplink(const QString &deeplink, bool &ok)
    {
        ok = false;
        RoutingProfile profile;

        QString payload = deeplink.trimmed();
        static const QStringList prefixes = { QStringLiteral("happ://routing/add/"),
                                              QStringLiteral("happ://routing/onadd/") };
        bool matched = false;
        for (const QString &prefix : prefixes) {
            if (payload.startsWith(prefix, Qt::CaseInsensitive)) {
                payload = payload.mid(prefix.length());
                matched = true;
                break;
            }
        }
        if (!matched) {
            return profile;
        }

        const QByteArray decoded = QByteArray::fromBase64(payload.toLatin1());
        if (decoded.isEmpty()) {
            return profile;
        }

        QJsonParseError parseError {};
        const QJsonDocument doc = QJsonDocument::fromJson(decoded, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return profile;
        }

        profile = fromJson(doc.object());
        ok = true;
        return profile;
    }
}
