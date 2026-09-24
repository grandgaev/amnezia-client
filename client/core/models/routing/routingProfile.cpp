#include "routingProfile.h"

#include <QHash>
#include <algorithm>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QRegularExpression>
#include <QUuid>

namespace amnezia
{
    namespace routing
    {
        namespace
        {
            // Storage keys
            constexpr char kId[] = "id";
            constexpr char kName[] = "name";
            constexpr char kGlobalProxy[] = "globalProxy";
            constexpr char kProxySites[] = "proxySites";
            constexpr char kProxyIp[] = "proxyIp";
            constexpr char kDirectSites[] = "directSites";
            constexpr char kDirectIp[] = "directIp";
            constexpr char kBlockSites[] = "blockSites";
            constexpr char kBlockIp[] = "blockIp";
            constexpr char kRouteOrder[] = "routeOrder";
            constexpr char kDomainStrategy[] = "domainStrategy";
            constexpr char kRemoteDnsType[] = "remoteDnsType";
            constexpr char kRemoteDnsDomain[] = "remoteDnsDomain";
            constexpr char kRemoteDnsIp[] = "remoteDnsIp";
            constexpr char kDomesticDnsType[] = "domesticDnsType";
            constexpr char kDomesticDnsDomain[] = "domesticDnsDomain";
            constexpr char kDomesticDnsIp[] = "domesticDnsIp";
            constexpr char kDnsHosts[] = "dnsHosts";
            constexpr char kGeoIpUrl[] = "geoIpUrl";
            constexpr char kGeoSiteUrl[] = "geoSiteUrl";
            constexpr char kFakeDns[] = "fakeDns";
            constexpr char kLastUpdated[] = "lastUpdated";
            constexpr char kGeoIpUpdatedAt[] = "geoIpUpdatedAt";
            constexpr char kGeoSiteUpdatedAt[] = "geoSiteUpdatedAt";
            constexpr char kGeoError[] = "geoError";

            QJsonArray toJsonArray(const QStringList &list)
            {
                return QJsonArray::fromStringList(list);
            }

            QStringList toStringList(const QJsonValue &value)
            {
                QStringList result;
                const QJsonArray array = value.toArray();
                for (const QJsonValue &v : array) {
                    const QString s = v.toString().trimmed();
                    if (!s.isEmpty()) {
                        result.append(s);
                    }
                }
                return result;
            }

            // Imported lists: every string may itself hold several entries
            // separated by commas or new lines (Happ behaviour).
            QStringList importedList(const QJsonValue &value)
            {
                static const QRegularExpression separators(QStringLiteral("[,\\r\\n]+"));
                QStringList result;
                if (value.isString()) {
                    for (const QString &part : value.toString().split(separators, Qt::SkipEmptyParts)) {
                        const QString s = part.trimmed();
                        if (!s.isEmpty()) {
                            result.append(s);
                        }
                    }
                    return result;
                }
                const QJsonArray array = value.toArray();
                for (const QJsonValue &v : array) {
                    for (const QString &part : v.toVariant().toString().split(separators, Qt::SkipEmptyParts)) {
                        const QString s = part.trimmed();
                        if (!s.isEmpty()) {
                            result.append(s);
                        }
                    }
                }
                return result;
            }

            bool toBool(const QJsonValue &value, bool defaultValue)
            {
                if (value.isBool()) {
                    return value.toBool();
                }
                if (value.isString()) {
                    const QString s = value.toString().trimmed().toLower();
                    if (s == QLatin1String("true") || s == QLatin1String("1")) {
                        return true;
                    }
                    if (s == QLatin1String("false") || s == QLatin1String("0")) {
                        return false;
                    }
                    return defaultValue;
                }
                if (value.isDouble()) {
                    return value.toDouble() != 0;
                }
                return defaultValue;
            }

            QString canonicalDnsType(const QString &value)
            {
                if (value.compare(QLatin1String(dnsTypeDoU), Qt::CaseInsensitive) == 0) {
                    return QString::fromLatin1(dnsTypeDoU);
                }
                return QString::fromLatin1(dnsTypeDoH);
            }

            QString matchValue(const QString &value, const QStringList &allowed)
            {
                for (const QString &a : allowed) {
                    if (a.compare(value.trimmed(), Qt::CaseInsensitive) == 0) {
                        return a;
                    }
                }
                return QString();
            }

            QByteArray decodeBase64Loose(QString text)
            {
                text.remove(QRegularExpression(QStringLiteral("\\s")));
                if (text.isEmpty()) {
                    return {};
                }
                QByteArray raw = text.toLatin1();
                auto result = QByteArray::fromBase64Encoding(raw, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
                if (!result) {
                    result = QByteArray::fromBase64Encoding(raw, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
                }
                if (!result) {
                    return {};
                }
                return *result;
            }

            // A profile that sends only the listed entries through the VPN (what the old
            // "Only listed sites via VPN" split tunneling did).
            QJsonObject listProfile(const QStringList &entries)
            {
                QJsonArray sites;
                QJsonArray ips;
                for (const QString &entry : entries) {
                    (isIpRule(entry) ? ips : sites).append(entry);
                }
                QJsonObject object;
                object.insert(QStringLiteral("Name"), QObject::tr("Imported list"));
                object.insert(QStringLiteral("GlobalProxy"), QStringLiteral("false"));
                object.insert(QStringLiteral("ProxySites"), sites);
                object.insert(QStringLiteral("ProxyIp"), ips);
                return object;
            }

            void appendObjects(const QJsonDocument &doc, QList<QJsonObject> &out)
            {
                if (doc.isObject()) {
                    out.append(doc.object());
                } else if (doc.isArray()) {
                    // The site list export of older AmneziaVPN versions: [{"hostname": ..., "ip": ...}]
                    QStringList legacyEntries;
                    for (const QJsonValue &v : doc.array()) {
                        const QJsonObject object = v.toObject();
                        if (object.contains(QStringLiteral("hostname"))) {
                            const QString hostname = object.value(QStringLiteral("hostname")).toString().trimmed();
                            if (!hostname.isEmpty()) {
                                legacyEntries.append(hostname);
                            }
                        } else if (v.isObject()) {
                            out.append(object);
                        }
                    }
                    if (!legacyEntries.isEmpty()) {
                        out.append(listProfile(legacyEntries));
                    }
                }
            }

            bool looksLikeRule(const QString &token)
            {
                if (isIpRule(token)) {
                    return true;
                }
                static const QRegularExpression prefixed(QStringLiteral("^(geosite|domain|full|regexp|keyword|ext):.+$"),
                                                         QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression domain(QStringLiteral("^(\\*\\.)?[\\p{L}\\p{N}_-]+(\\.[\\p{L}\\p{N}_-]+)+\\.?$"));
                return prefixed.match(token).hasMatch() || domain.match(token).hasMatch();
            }

            bool parseOne(const QString &input, QList<QJsonObject> &out)
            {
                QString text = input.trimmed();
                if (text.isEmpty()) {
                    return false;
                }

                static const QString happPrefix = QStringLiteral("happ://routing/");
                if (text.startsWith(happPrefix, Qt::CaseInsensitive)) {
                    text = text.mid(happPrefix.size());
                    if (text.startsWith(QLatin1String("onadd/"), Qt::CaseInsensitive)) {
                        text = text.mid(6);
                    } else if (text.startsWith(QLatin1String("add/"), Qt::CaseInsensitive)) {
                        text = text.mid(4);
                    } else {
                        return false;
                    }
                }

                QJsonParseError error;
                QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
                if (error.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
                    const int before = out.size();
                    appendObjects(doc, out);
                    return out.size() > before;
                }

                const QByteArray decoded = decodeBase64Loose(text);
                if (decoded.isEmpty()) {
                    return false;
                }
                doc = QJsonDocument::fromJson(decoded, &error);
                if (error.error != QJsonParseError::NoError) {
                    return false;
                }
                const int before = out.size();
                appendObjects(doc, out);
                return out.size() > before;
            }
        } // namespace

        QString actionToString(RuleAction action)
        {
            switch (action) {
            case RuleAction::Proxy: return QStringLiteral("proxy");
            case RuleAction::Direct: return QStringLiteral("direct");
            case RuleAction::Block: return QStringLiteral("block");
            }
            return QString();
        }

        QStringList routeOrderValues()
        {
            return { QStringLiteral("block-proxy-direct"), QStringLiteral("block-direct-proxy"),
                     QStringLiteral("proxy-direct-block"), QStringLiteral("proxy-block-direct"),
                     QStringLiteral("direct-proxy-block"), QStringLiteral("direct-block-proxy") };
        }

        QString defaultRouteOrder()
        {
            return QStringLiteral("block-direct-proxy");
        }

        QList<RuleAction> parseRouteOrder(const QString &order)
        {
            QList<RuleAction> result;
            const QString canonical = matchValue(order, routeOrderValues());
            const QString value = canonical.isEmpty() ? defaultRouteOrder() : canonical;
            for (const QString &part : value.split(QLatin1Char('-'), Qt::SkipEmptyParts)) {
                if (part == QLatin1String("proxy")) {
                    result.append(RuleAction::Proxy);
                } else if (part == QLatin1String("direct")) {
                    result.append(RuleAction::Direct);
                } else if (part == QLatin1String("block")) {
                    result.append(RuleAction::Block);
                }
            }
            return result;
        }

        QStringList domainStrategyValues()
        {
            return { QStringLiteral("AsIs"), QStringLiteral("IPIfNonMatch"), QStringLiteral("IPOnDemand") };
        }

        QStringList lanAddresses()
        {
            return { QStringLiteral("10.0.0.0/8"),     QStringLiteral("172.16.0.0/12"), QStringLiteral("192.168.0.0/16"),
                     QStringLiteral("169.254.0.0/16"), QStringLiteral("224.0.0.0/4"),   QStringLiteral("255.255.255.255") };
        }

        RoutingProfile RoutingProfile::createDefault(const QString &name)
        {
            RoutingProfile profile;
            profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            profile.name = name;
            profile.directIp = lanAddresses();
            profile.dnsHosts.insert(QStringLiteral("cloudflare-dns.com"), QStringLiteral("1.1.1.1"));
            profile.dnsHosts.insert(QStringLiteral("dns.google"), QStringLiteral("8.8.8.8"));
            return profile;
        }

        QStringList &RoutingProfile::sites(RuleAction action)
        {
            switch (action) {
            case RuleAction::Proxy: return proxySites;
            case RuleAction::Direct: return directSites;
            case RuleAction::Block: return blockSites;
            }
            return proxySites;
        }

        QStringList &RoutingProfile::ips(RuleAction action)
        {
            switch (action) {
            case RuleAction::Proxy: return proxyIp;
            case RuleAction::Direct: return directIp;
            case RuleAction::Block: return blockIp;
            }
            return proxyIp;
        }

        const QStringList &RoutingProfile::sites(RuleAction action) const
        {
            return const_cast<RoutingProfile *>(this)->sites(action);
        }

        const QStringList &RoutingProfile::ips(RuleAction action) const
        {
            return const_cast<RoutingProfile *>(this)->ips(action);
        }

        QString RoutingProfile::rulesText(RuleAction action) const
        {
            QStringList lines = sites(action);
            lines.append(ips(action));
            return lines.join(QLatin1Char('\n'));
        }

        void RoutingProfile::setRulesText(RuleAction action, const QString &text)
        {
            QStringList newSites;
            QStringList newIps;
            for (const QString &token : splitRulesText(text)) {
                if (isIpRule(token)) {
                    if (!newIps.contains(token)) {
                        newIps.append(token);
                    }
                } else if (!newSites.contains(token)) {
                    newSites.append(token);
                }
            }
            sites(action) = newSites;
            ips(action) = newIps;
        }

        QStringList RoutingProfile::geoSiteTags() const
        {
            QStringList tags;
            for (const QStringList *list : { &proxySites, &directSites, &blockSites }) {
                for (const QString &entry : *list) {
                    if (entry.startsWith(QLatin1String("geosite:"), Qt::CaseInsensitive)) {
                        const QString tag = entry.mid(8).trimmed().toLower();
                        if (!tag.isEmpty() && !tags.contains(tag)) {
                            tags.append(tag);
                        }
                    }
                }
            }
            return tags;
        }

        QStringList RoutingProfile::geoIpTags() const
        {
            QStringList tags;
            for (const QStringList *list : { &proxyIp, &directIp, &blockIp }) {
                for (const QString &entry : *list) {
                    if (entry.startsWith(QLatin1String("geoip:"), Qt::CaseInsensitive)) {
                        const QString tag = entry.mid(6).trimmed().toLower();
                        if (!tag.isEmpty() && !tags.contains(tag)) {
                            tags.append(tag);
                        }
                    }
                }
            }
            return tags;
        }

        bool RoutingProfile::usesGeoSite() const
        {
            return !geoSiteTags().isEmpty();
        }

        bool RoutingProfile::usesGeoIp() const
        {
            return !geoIpTags().isEmpty();
        }

        QJsonObject RoutingProfile::toJson() const
        {
            QJsonObject obj;
            obj[kId] = id;
            obj[kName] = name;
            obj[kGlobalProxy] = globalProxy;
            obj[kProxySites] = toJsonArray(proxySites);
            obj[kProxyIp] = toJsonArray(proxyIp);
            obj[kDirectSites] = toJsonArray(directSites);
            obj[kDirectIp] = toJsonArray(directIp);
            obj[kBlockSites] = toJsonArray(blockSites);
            obj[kBlockIp] = toJsonArray(blockIp);
            obj[kRouteOrder] = routeOrder;
            obj[kDomainStrategy] = domainStrategy;
            obj[kRemoteDnsType] = remoteDnsType;
            obj[kRemoteDnsDomain] = remoteDnsDomain;
            obj[kRemoteDnsIp] = remoteDnsIp;
            obj[kDomesticDnsType] = domesticDnsType;
            obj[kDomesticDnsDomain] = domesticDnsDomain;
            obj[kDomesticDnsIp] = domesticDnsIp;
            obj[kDnsHosts] = QJsonObject::fromVariantMap(dnsHosts);
            obj[kGeoIpUrl] = geoIpUrl;
            obj[kGeoSiteUrl] = geoSiteUrl;
            obj[kFakeDns] = fakeDns;
            obj[kLastUpdated] = QString::number(lastUpdated);
            obj[kGeoIpUpdatedAt] = QString::number(geoIpUpdatedAt);
            obj[kGeoSiteUpdatedAt] = QString::number(geoSiteUpdatedAt);
            obj[kGeoError] = geoError;
            return obj;
        }

        RoutingProfile RoutingProfile::fromJson(const QJsonObject &json)
        {
            RoutingProfile p;
            p.id = json.value(kId).toString();
            if (p.id.isEmpty()) {
                p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            }
            p.name = json.value(kName).toString();
            p.globalProxy = json.value(kGlobalProxy).toBool(true);
            p.proxySites = toStringList(json.value(kProxySites));
            p.proxyIp = toStringList(json.value(kProxyIp));
            p.directSites = toStringList(json.value(kDirectSites));
            p.directIp = toStringList(json.value(kDirectIp));
            p.blockSites = toStringList(json.value(kBlockSites));
            p.blockIp = toStringList(json.value(kBlockIp));
            p.routeOrder = json.value(kRouteOrder).toString(defaultRouteOrder());
            p.domainStrategy = json.value(kDomainStrategy).toString(QStringLiteral("IPIfNonMatch"));
            p.remoteDnsType = canonicalDnsType(json.value(kRemoteDnsType).toString(QString::fromLatin1(dnsTypeDoU)));
            p.remoteDnsDomain = json.value(kRemoteDnsDomain).toString(p.remoteDnsDomain);
            p.remoteDnsIp = json.value(kRemoteDnsIp).toString();
            p.domesticDnsType = canonicalDnsType(json.value(kDomesticDnsType).toString(QString::fromLatin1(dnsTypeDoH)));
            p.domesticDnsDomain = json.value(kDomesticDnsDomain).toString(p.domesticDnsDomain);
            p.domesticDnsIp = json.value(kDomesticDnsIp).toString(p.domesticDnsIp);
            p.dnsHosts = json.value(kDnsHosts).toObject().toVariantMap();
            p.geoIpUrl = json.value(kGeoIpUrl).toString(p.geoIpUrl);
            p.geoSiteUrl = json.value(kGeoSiteUrl).toString(p.geoSiteUrl);
            p.fakeDns = json.value(kFakeDns).toBool(false);
            p.lastUpdated = json.value(kLastUpdated).toVariant().toLongLong();
            p.geoIpUpdatedAt = json.value(kGeoIpUpdatedAt).toVariant().toLongLong();
            p.geoSiteUpdatedAt = json.value(kGeoSiteUpdatedAt).toVariant().toLongLong();
            p.geoError = json.value(kGeoError).toString();
            return p;
        }

        QJsonObject RoutingProfile::toExchangeJson() const
        {
            QJsonObject obj;
            obj[QStringLiteral("Name")] = name;
            // Happ exports booleans as "true"/"false" strings.
            obj[QStringLiteral("GlobalProxy")] = globalProxy ? QStringLiteral("true") : QStringLiteral("false");
            obj[QStringLiteral("RemoteDNSType")] = remoteDnsType;
            obj[QStringLiteral("RemoteDNSDomain")] = remoteDnsDomain;
            obj[QStringLiteral("RemoteDNSIP")] = remoteDnsIp;
            obj[QStringLiteral("DomesticDNSType")] = domesticDnsType;
            obj[QStringLiteral("DomesticDNSDomain")] = domesticDnsDomain;
            obj[QStringLiteral("DomesticDNSIP")] = domesticDnsIp;
            obj[QStringLiteral("Geoipurl")] = geoIpUrl;
            obj[QStringLiteral("Geositeurl")] = geoSiteUrl;
            obj[QStringLiteral("LastUpdated")] = QString::number(lastUpdated);
            obj[QStringLiteral("DnsHosts")] = QJsonObject::fromVariantMap(dnsHosts);
            obj[QStringLiteral("DirectSites")] = toJsonArray(directSites);
            obj[QStringLiteral("DirectIp")] = toJsonArray(directIp);
            obj[QStringLiteral("ProxySites")] = toJsonArray(proxySites);
            obj[QStringLiteral("ProxyIp")] = toJsonArray(proxyIp);
            obj[QStringLiteral("BlockSites")] = toJsonArray(blockSites);
            obj[QStringLiteral("BlockIp")] = toJsonArray(blockIp);
            obj[QStringLiteral("DomainStrategy")] = domainStrategy;
            obj[QStringLiteral("FakeDNS")] = fakeDns ? QStringLiteral("true") : QStringLiteral("false");
            obj[QStringLiteral("RouteOrder")] = routeOrder;
            return obj;
        }

        void RoutingProfile::mergeExchangeJson(const QJsonObject &json)
        {
            // Keys are matched case-insensitively, like Happ does.
            QHash<QString, QJsonValue> v;
            for (auto it = json.constBegin(); it != json.constEnd(); ++it) {
                v.insert(it.key().toLower(), it.value());
            }
            auto has = [&v](const char *key) { return v.contains(QLatin1String(key)); };
            auto str = [&v](const char *key) { return v.value(QLatin1String(key)).toVariant().toString().trimmed(); };

            if (has("name")) {
                name = str("name");
            }
            if (name.isEmpty()) {
                name = QStringLiteral("Default");
            }
            if (has("globalproxy")) {
                globalProxy = toBool(v.value(QStringLiteral("globalproxy")), globalProxy);
            }
            if (!str("remotednstype").isEmpty()) {
                remoteDnsType = canonicalDnsType(str("remotednstype"));
            }
            if (!str("domesticdnstype").isEmpty()) {
                domesticDnsType = canonicalDnsType(str("domesticdnstype"));
            }
            if (!str("remotedns").isEmpty()) {
                remoteDnsIp = str("remotedns");
            }
            if (!str("domesticdns").isEmpty()) {
                domesticDnsIp = str("domesticdns");
            }
            if (!str("remotednsip").isEmpty()) {
                remoteDnsIp = str("remotednsip");
            }
            if (!str("domesticdnsip").isEmpty()) {
                domesticDnsIp = str("domesticdnsip");
            }
            if (!str("remotednsdomain").isEmpty()) {
                remoteDnsDomain = str("remotednsdomain");
            }
            if (!str("domesticdnsdomain").isEmpty()) {
                domesticDnsDomain = str("domesticdnsdomain");
            }
            if (!str("geoipurl").isEmpty()) {
                geoIpUrl = str("geoipurl");
            }
            if (!str("geositeurl").isEmpty()) {
                geoSiteUrl = str("geositeurl");
            }
            if (has("domainstrategy")) {
                const QString s = matchValue(str("domainstrategy"), domainStrategyValues());
                if (!s.isEmpty()) {
                    domainStrategy = s;
                }
            }
            if (has("dnshosts")) {
                const QJsonObject hosts = v.value(QStringLiteral("dnshosts")).toObject();
                if (!hosts.isEmpty()) {
                    QVariantMap map;
                    for (auto it = hosts.constBegin(); it != hosts.constEnd(); ++it) {
                        if (it.value().isArray()) {
                            QStringList values;
                            for (const QJsonValue &h : it.value().toArray()) {
                                values.append(h.toVariant().toString());
                            }
                            map.insert(it.key(), values.join(QLatin1Char(',')));
                        } else {
                            map.insert(it.key(), it.value().toVariant().toString());
                        }
                    }
                    dnsHosts = map;
                }
            }
            const QList<QPair<const char *, QStringList *>> lists = {
                { "proxysites", &proxySites },   { "proxyip", &proxyIp }, { "directsites", &directSites },
                { "directip", &directIp },       { "blocksites", &blockSites }, { "blockip", &blockIp },
            };
            for (const auto &list : lists) {
                if (has(list.first)) {
                    const QStringList values = importedList(v.value(QLatin1String(list.first)));
                    if (!values.isEmpty()) {
                        *list.second = values;
                    }
                }
            }
            // Keep IPs and sites in their proper lists, whatever the source put where.
            for (RuleAction action : { RuleAction::Proxy, RuleAction::Direct, RuleAction::Block }) {
                QStringList allEntries = sites(action);
                allEntries.append(ips(action));
                setRulesText(action, allEntries.join(QLatin1Char('\n')));
            }
            lastUpdated = v.value(QStringLiteral("lastupdated")).toVariant().toLongLong();
            fakeDns = toBool(v.value(QStringLiteral("fakedns")), false);
            if (has("routeorder")) {
                const QString order = matchValue(str("routeorder"), routeOrderValues());
                if (!order.isEmpty()) {
                    routeOrder = order;
                }
            }
        }

        bool isIpRule(const QString &token)
        {
            const QString t = token.trimmed();
            if (t.startsWith(QLatin1String("geoip:"), Qt::CaseInsensitive)) {
                return true;
            }
            static const QStringList sitePrefixes = { QStringLiteral("geosite:"), QStringLiteral("domain:"), QStringLiteral("full:"),
                                                      QStringLiteral("regexp:"),  QStringLiteral("keyword:"), QStringLiteral("ext:") };
            for (const QString &prefix : sitePrefixes) {
                if (t.startsWith(prefix, Qt::CaseInsensitive)) {
                    return false;
                }
            }
            if (t.contains(QLatin1Char('/'))) {
                const auto subnet = QHostAddress::parseSubnet(t);
                return !subnet.first.isNull() && subnet.second >= 0;
            }
            QHostAddress address;
            return address.setAddress(t);
        }

        QStringList splitRulesText(const QString &text)
        {
            static const QRegularExpression lineBreak(QStringLiteral("\\r?\\n"));
            QStringList tokens;
            for (const QString &line : text.split(lineBreak)) {
                const QString trimmedLine = line.trimmed();
                if (trimmedLine.isEmpty() || trimmedLine.startsWith(QLatin1Char('#'))) {
                    continue;
                }
                for (const QString &part : trimmedLine.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const QString token = part.trimmed();
                    if (!token.isEmpty()) {
                        tokens.append(token);
                    }
                }
            }
            return tokens;
        }

        QList<QJsonObject> parseImportData(const QString &data, QString *errorMessage)
        {
            QList<QJsonObject> result;
            if (parseOne(data, result)) {
                return result;
            }
            // Several links or base64 blobs, one per line.
            static const QRegularExpression lineBreak(QStringLiteral("\\r?\\n"));
            for (const QString &line : data.split(lineBreak, Qt::SkipEmptyParts)) {
                parseOne(line, result);
            }
            if (result.isEmpty()) {
                // A plain list of sites and addresses.
                const QStringList tokens = splitRulesText(data);
                if (!tokens.isEmpty() && std::all_of(tokens.cbegin(), tokens.cend(), looksLikeRule)) {
                    result.append(listProfile(tokens));
                }
            }
            if (result.isEmpty() && errorMessage) {
                *errorMessage = QObject::tr("Wrong data for import routing profile");
            }
            return result;
        }
    } // namespace routing
} // namespace amnezia
