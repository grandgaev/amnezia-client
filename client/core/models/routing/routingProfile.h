#ifndef ROUTINGPROFILE_H
#define ROUTINGPROFILE_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace amnezia
{
    namespace routing
    {
        // Rule actions, as in Happ / Xray outbound tags.
        enum class RuleAction {
            Proxy,
            Direct,
            Block
        };

        QString actionToString(RuleAction action);

        // "block-direct-proxy" etc.
        QStringList routeOrderValues();
        QString defaultRouteOrder();
        QList<RuleAction> parseRouteOrder(const QString &order);

        QStringList domainStrategyValues();

        QStringList lanAddresses();

        inline constexpr char dnsTypeDoU[] = "DoU";
        inline constexpr char dnsTypeDoH[] = "DoH";

        inline constexpr char defaultGeoIpUrl[] =
                "https://github.com/Loyalsoldier/v2ray-rules-dat/releases/latest/download/geoip.dat";
        inline constexpr char defaultGeoSiteUrl[] =
                "https://github.com/Loyalsoldier/v2ray-rules-dat/releases/latest/download/geosite.dat";

        // A routing profile. The model mirrors the routing profiles of the Happ
        // client so that profiles can be exchanged between the applications.
        struct RoutingProfile
        {
            QString id;
            QString name;

            // true: traffic that matches no rule goes through the VPN;
            // false: traffic that matches no rule goes directly.
            bool globalProxy = true;

            QStringList proxySites;
            QStringList proxyIp;
            QStringList directSites;
            QStringList directIp;
            QStringList blockSites;
            QStringList blockIp;

            QString routeOrder = defaultRouteOrder();
            QString domainStrategy = QStringLiteral("IPIfNonMatch");

            // Remote DNS resolves names that go through the VPN. An empty IP
            // means "use the DNS servers of the VPN connection".
            QString remoteDnsType = QString::fromLatin1(dnsTypeDoU);
            QString remoteDnsDomain = QStringLiteral("https://cloudflare-dns.com/dns-query");
            QString remoteDnsIp;

            // Domestic DNS resolves names that go directly.
            QString domesticDnsType = QString::fromLatin1(dnsTypeDoH);
            QString domesticDnsDomain = QStringLiteral("https://dns.google/dns-query");
            QString domesticDnsIp = QStringLiteral("8.8.8.8");

            QVariantMap dnsHosts;

            QString geoIpUrl = QString::fromLatin1(defaultGeoIpUrl);
            QString geoSiteUrl = QString::fromLatin1(defaultGeoSiteUrl);

            bool fakeDns = false;
            qint64 lastUpdated = 0;

            // Local state (not exported)
            qint64 geoIpUpdatedAt = 0;   // ms since epoch
            qint64 geoSiteUpdatedAt = 0; // ms since epoch
            QString geoError;

            static RoutingProfile createDefault(const QString &name);

            QStringList &sites(RuleAction action);
            QStringList &ips(RuleAction action);
            const QStringList &sites(RuleAction action) const;
            const QStringList &ips(RuleAction action) const;

            // Plain text editing (one entry per line) of the rules of an action.
            QString rulesText(RuleAction action) const;
            void setRulesText(RuleAction action, const QString &text);

            bool usesGeoSite() const;
            bool usesGeoIp() const;
            QStringList geoSiteTags() const;
            QStringList geoIpTags() const;

            // Storage format (camelCase keys).
            QJsonObject toJson() const;
            static RoutingProfile fromJson(const QJsonObject &json);

            // Exchange format compatible with Happ (keys as exported by Happ).
            QJsonObject toExchangeJson() const;
            // Merge an imported object (case-insensitive keys, Happ semantics:
            // missing/empty values keep the current value).
            void mergeExchangeJson(const QJsonObject &json);
        };

        // Classifies one token of the rule editor: true for IP rules
        // (addresses, CIDRs, geoip:), false for site rules.
        bool isIpRule(const QString &token);

        // Splits the plain text of the editor into tokens (newline or comma
        // separated, '#' comments ignored).
        QStringList splitRulesText(const QString &text);

        // Parses any supported import format into JSON objects of profiles:
        // raw JSON (object or array), standard/URL-safe base64 of JSON, and
        // the "happ://routing/add|onadd/<base64>" payload format.
        QList<QJsonObject> parseImportData(const QString &data, QString *errorMessage = nullptr);
    } // namespace routing
} // namespace amnezia

#endif // ROUTINGPROFILE_H
