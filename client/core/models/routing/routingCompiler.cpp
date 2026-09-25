#include "routingCompiler.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QObject>
#include <QUrl>

#include "geoData.h"
#include "ipRanges.h"

namespace amnezia
{
    namespace routing
    {
        namespace
        {
            // Beyond this size domain lists are not duplicated into the Xray DNS
            // section (it only selects the resolver, routing is unaffected).
            constexpr int maxXrayDnsDomains = 5000;

            bool isValidIp(const QString &value)
            {
                QHostAddress address;
                return address.setAddress(value.trimmed());
            }

            QStringList ipList(const QString &commaSeparated)
            {
                QStringList result;
                for (const QString &part : commaSeparated.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const QString ip = part.trimmed();
                    if (isValidIp(ip) && !result.contains(ip)) {
                        result.append(ip);
                    }
                }
                return result;
            }

            bool hasPrefix(const QString &value, const char *prefix)
            {
                return value.startsWith(QLatin1String(prefix), Qt::CaseInsensitive);
            }

            QString hostOfUrl(const QString &url)
            {
                return QUrl(url.trimmed()).host();
            }

            QString portFor(const QString &dnsType)
            {
                return dnsType.compare(QLatin1String(dnsTypeDoH), Qt::CaseInsensitive) == 0 ? QStringLiteral("443")
                                                                                              : QStringLiteral("53");
            }

            // Plain host names that can be resolved at connect time.
            QString resolvableHost(const QString &rule)
            {
                QString value = rule.trimmed();
                if (hasPrefix(value, "full:")) {
                    value = value.mid(5);
                } else if (hasPrefix(value, "domain:")) {
                    value = value.mid(7);
                } else if (value.contains(QLatin1Char(':'))) {
                    return QString();
                }
                value = value.trimmed().toLower();
                if (value.startsWith(QLatin1Char('.'))) {
                    value = value.mid(1);
                }
                if (value.isEmpty() || !value.contains(QLatin1Char('.')) || value.contains(QLatin1Char('*'))
                    || value.contains(QLatin1Char('/'))) {
                    return QString();
                }
                return value;
            }
        } // namespace

        bool ExpandedProfile::hasRules() const
        {
            for (const QStringList &list : domains) {
                if (!list.isEmpty()) {
                    return true;
                }
            }
            for (const QStringList &list : ips) {
                if (!list.isEmpty()) {
                    return true;
                }
            }
            return false;
        }

        ExpandedProfile RoutingCompiler::expand(const CompileInput &input)
        {
            const RoutingProfile &profile = input.profile;
            ExpandedProfile result;
            result.order = parseRouteOrder(profile.routeOrder);

            for (RuleAction action : result.order) {
                const int key = static_cast<int>(action);
                QStringList routerDomains;
                QStringList xrayDomains;
                QStringList geoTags;

                for (const QString &raw : profile.sites(action)) {
                    const QString entry = raw.trimmed();
                    if (entry.isEmpty()) {
                        continue;
                    }
                    if (hasPrefix(entry, "geosite:")) {
                        geoTags.append(entry.mid(8).trimmed());
                    } else if (hasPrefix(entry, "domain:") || hasPrefix(entry, "full:") || hasPrefix(entry, "keyword:")
                               || hasPrefix(entry, "regexp:")) {
                        const int colon = entry.indexOf(QLatin1Char(':'));
                        const QString type = entry.left(colon).toLower();
                        QString value = entry.mid(colon + 1).trimmed();
                        if (type != QLatin1String("regexp")) {
                            value = value.toLower();
                        }
                        if (value.isEmpty()) {
                            continue;
                        }
                        routerDomains.append(type + QLatin1Char(':') + value);
                        xrayDomains.append(type + QLatin1Char(':') + value);
                    } else if (entry.contains(QLatin1Char(':'))) {
                        result.warnings.append(QObject::tr("Unsupported rule \"%1\" is ignored").arg(entry));
                    } else {
                        // Xray treats a plain string as a substring of the domain name.
                        routerDomains.append(QStringLiteral("keyword:") + entry.toLower());
                        xrayDomains.append(entry.toLower());
                    }
                }

                if (!geoTags.isEmpty()) {
                    if (input.geoSitePath.isEmpty() || !QFileInfo::exists(input.geoSitePath)) {
                        result.warnings.append(QObject::tr("The geosite file is not downloaded, rules %1 are ignored")
                                                       .arg(geoTags.join(QStringLiteral(", "))));
                    } else {
                        QStringList missing;
                        QString error;
                        const QStringList expanded = GeoData::expandSites(input.geoSitePath, geoTags, &missing, &error);
                        routerDomains.append(expanded);
                        xrayDomains.append(expanded);
                        if (!error.isEmpty()) {
                            result.warnings.append(error);
                        }
                        for (const QString &tag : missing) {
                            result.warnings.append(QObject::tr("Category geosite:%1 is not found in the geosite file").arg(tag));
                        }
                    }
                }

                IpRangeSet ranges;
                QStringList geoCodes;
                for (const QString &raw : profile.ips(action)) {
                    const QString entry = raw.trimmed();
                    if (entry.isEmpty()) {
                        continue;
                    }
                    if (hasPrefix(entry, "geoip:")) {
                        geoCodes.append(entry.mid(6).trimmed());
                    } else if (!ranges.addCidr(entry)) {
                        result.warnings.append(QObject::tr("Invalid address \"%1\" is ignored").arg(entry));
                    }
                }
                if (!geoCodes.isEmpty()) {
                    if (input.geoIpPath.isEmpty() || !QFileInfo::exists(input.geoIpPath)) {
                        result.warnings.append(QObject::tr("The geoip file is not downloaded, rules %1 are ignored")
                                                       .arg(geoCodes.join(QStringLiteral(", "))));
                    } else {
                        QStringList missing;
                        QString error;
                        const QStringList cidrs = GeoData::expandIps(input.geoIpPath, geoCodes, &missing, &error);
                        for (const QString &cidr : cidrs) {
                            ranges.addCidr(cidr);
                        }
                        if (!error.isEmpty()) {
                            result.warnings.append(error);
                        }
                        for (const QString &code : missing) {
                            result.warnings.append(QObject::tr("Category geoip:%1 is not found in the geoip file").arg(code));
                        }
                    }
                }

                routerDomains.removeDuplicates();
                xrayDomains.removeDuplicates();
                result.domains.insert(key, routerDomains);
                result.xrayDomains.insert(key, xrayDomains);
                result.ips.insert(key, ranges.toCidrs());
            }
            return result;
        }

        QStringList RoutingCompiler::remoteDnsServers(const CompileInput &input)
        {
            QStringList servers = ipList(input.profile.remoteDnsIp);
            if (servers.isEmpty()) {
                for (const QString &dns : input.connectionDns) {
                    if (isValidIp(dns) && !servers.contains(dns.trimmed())) {
                        servers.append(dns.trimmed());
                    }
                }
            }
            if (servers.isEmpty()) {
                servers.append(QStringLiteral("1.1.1.1"));
            }
            return servers;
        }

        QStringList RoutingCompiler::domesticDnsServers(const CompileInput &input)
        {
            QStringList servers = ipList(input.profile.domesticDnsIp);
            if (servers.isEmpty()) {
                const QString host = hostOfUrl(input.profile.domesticDnsDomain);
                if (isValidIp(host)) {
                    servers.append(host);
                }
            }
            if (servers.isEmpty()) {
                servers.append(QStringLiteral("8.8.8.8"));
            }
            return servers;
        }

        QJsonObject RoutingCompiler::routerConfig(const CompileInput &input, const ExpandedProfile &expanded)
        {
            const RoutingProfile &profile = input.profile;
            QJsonObject config;
            config[QStringLiteral("version")] = 1;
            config[QStringLiteral("defaultAction")] = profile.globalProxy ? QStringLiteral("proxy") : QStringLiteral("direct");

            QJsonArray rules;
            for (RuleAction action : expanded.order) {
                const int key = static_cast<int>(action);
                const QStringList domains = expanded.domains.value(key);
                const QStringList ips = expanded.ips.value(key);
                if (domains.isEmpty() && ips.isEmpty()) {
                    continue;
                }
                QJsonObject rule;
                rule[QStringLiteral("action")] = actionToString(action);
                if (!domains.isEmpty()) {
                    rule[QStringLiteral("domains")] = QJsonArray::fromStringList(domains);
                }
                if (!ips.isEmpty()) {
                    rule[QStringLiteral("ips")] = QJsonArray::fromStringList(ips);
                }
                rules.append(rule);
            }
            if (!input.excludedRoutes.isEmpty()) {
                IpRangeSet excluded;
                for (const QString &route : input.excludedRoutes) {
                    excluded.addCidr(route);
                }
                const QStringList cidrs = excluded.toCidrs();
                if (!cidrs.isEmpty()) {
                    QJsonObject rule;
                    rule[QStringLiteral("action")] = QStringLiteral("direct");
                    rule[QStringLiteral("ips")] = QJsonArray::fromStringList(cidrs);
                    rules.append(rule);
                }
            }
            config[QStringLiteral("rules")] = rules;

            QJsonObject dns;
            dns[QStringLiteral("listen")] = QJsonArray { QString::fromLatin1(routerDnsAddress) };
            dns[QStringLiteral("remote")] = QJsonArray::fromStringList(remoteDnsServers(input));
            dns[QStringLiteral("direct")] = QJsonArray::fromStringList(domesticDnsServers(input));
            if (profile.domesticDnsType.compare(QLatin1String(dnsTypeDoH), Qt::CaseInsensitive) == 0
                && !profile.domesticDnsDomain.trimmed().isEmpty()) {
                QJsonObject doh;
                doh[QStringLiteral("url")] = profile.domesticDnsDomain.trimmed();
                const QString host = hostOfUrl(profile.domesticDnsDomain);
                QStringList addrs = ipList(profile.dnsHosts.value(host).toString());
                if (addrs.isEmpty()) {
                    addrs = domesticDnsServers(input);
                }
                doh[QStringLiteral("addrs")] = QJsonArray::fromStringList(addrs);
                dns[QStringLiteral("directDoh")] = doh;
            }
            QJsonObject hosts;
            for (auto it = profile.dnsHosts.constBegin(); it != profile.dnsHosts.constEnd(); ++it) {
                QString name = it.key().trimmed().toLower();
                if (name.startsWith(QLatin1String("full:"))) {
                    name = name.mid(5);
                } else if (name.contains(QLatin1Char(':'))) {
                    continue;
                }
                const QStringList addrs = ipList(it.value().toString());
                if (!name.isEmpty() && !addrs.isEmpty()) {
                    hosts[name] = QJsonArray::fromStringList(addrs);
                }
            }
            if (!hosts.isEmpty()) {
                dns[QStringLiteral("hosts")] = hosts;
            }
            config[QStringLiteral("dns")] = dns;
            config[QStringLiteral("bypass")] = input.bypassMode;
            if (!input.tunnelHasIpv6) {
                config[QStringLiteral("rejectProxyIpv6")] = true;
            }

            if (!input.appsMode.isEmpty() && !input.appPaths.isEmpty()) {
                QJsonObject apps;
                apps[QStringLiteral("mode")] = input.appsMode;
                apps[QStringLiteral("paths")] = QJsonArray::fromStringList(input.appPaths);
                config[QStringLiteral("apps")] = apps;
            }
            return config;
        }

        void RoutingCompiler::applyToXrayConfig(QJsonObject &xrayConfig, const CompileInput &input, const ExpandedProfile &expanded)
        {
            const RoutingProfile &profile = input.profile;

            // Outbounds: the server outbound becomes "proxy", plus "direct" and "block".
            QJsonArray outbounds;
            QJsonObject proxyOutbound;
            for (const QJsonValue &value : xrayConfig.value(QStringLiteral("outbounds")).toArray()) {
                QJsonObject outbound = value.toObject();
                const QString protocol = outbound.value(QStringLiteral("protocol")).toString();
                const QString tag = outbound.value(QStringLiteral("tag")).toString();
                if (tag == QLatin1String("direct") || tag == QLatin1String("block")) {
                    continue;
                }
                if (proxyOutbound.isEmpty() && protocol != QLatin1String("freedom") && protocol != QLatin1String("blackhole")) {
                    outbound[QStringLiteral("tag")] = QStringLiteral("proxy");
                    proxyOutbound = outbound;
                    continue;
                }
                outbounds.append(outbound);
            }
            if (proxyOutbound.isEmpty()) {
                return;
            }
            QJsonObject directOutbound { { QStringLiteral("protocol"), QStringLiteral("freedom") },
                                         { QStringLiteral("tag"), QStringLiteral("direct") } };
            QJsonObject blockOutbound { { QStringLiteral("protocol"), QStringLiteral("blackhole") },
                                        { QStringLiteral("tag"), QStringLiteral("block") } };
            QJsonArray ordered;
            // Xray sends unmatched traffic to the first outbound.
            if (profile.globalProxy) {
                ordered.append(proxyOutbound);
                ordered.append(directOutbound);
            } else {
                ordered.append(directOutbound);
                ordered.append(proxyOutbound);
            }
            ordered.append(blockOutbound);
            for (const QJsonValue &v : outbounds) {
                ordered.append(v);
            }
            xrayConfig[QStringLiteral("outbounds")] = ordered;

            // Sniff domains so that domain rules work for connections made by address.
            QJsonArray inbounds = xrayConfig.value(QStringLiteral("inbounds")).toArray();
            for (int i = 0; i < inbounds.size(); ++i) {
                QJsonObject inbound = inbounds.at(i).toObject();
                QJsonObject sniffing;
                sniffing[QStringLiteral("enabled")] = true;
                sniffing[QStringLiteral("destOverride")] = QJsonArray { QStringLiteral("http"), QStringLiteral("tls"),
                                                                        QStringLiteral("quic") };
                sniffing[QStringLiteral("routeOnly")] = true;
                inbound[QStringLiteral("sniffing")] = sniffing;
                inbounds[i] = inbound;
            }
            xrayConfig[QStringLiteral("inbounds")] = inbounds;

            const QStringList remote = remoteDnsServers(input);
            const QStringList domestic = domesticDnsServers(input);

            QJsonArray rules;
            rules.append(QJsonObject { { QStringLiteral("ip"), QJsonArray::fromStringList(remote) },
                                       { QStringLiteral("port"), QStringLiteral("53") },
                                       { QStringLiteral("outboundTag"), QStringLiteral("proxy") } });
            rules.append(QJsonObject { { QStringLiteral("ip"), QJsonArray::fromStringList(domestic) },
                                       { QStringLiteral("port"), portFor(profile.domesticDnsType) },
                                       { QStringLiteral("outboundTag"), QStringLiteral("direct") } });
            for (RuleAction action : expanded.order) {
                const int key = static_cast<int>(action);
                const QStringList domains = expanded.xrayDomains.value(key);
                const QStringList ips = expanded.ips.value(key);
                if (!domains.isEmpty()) {
                    rules.append(QJsonObject { { QStringLiteral("domain"), QJsonArray::fromStringList(domains) },
                                               { QStringLiteral("outboundTag"), actionToString(action) } });
                }
                if (!ips.isEmpty()) {
                    rules.append(QJsonObject { { QStringLiteral("ip"), QJsonArray::fromStringList(ips) },
                                               { QStringLiteral("outboundTag"), actionToString(action) } });
                }
            }
            if (!input.excludedRoutes.isEmpty()) {
                IpRangeSet excluded;
                for (const QString &route : input.excludedRoutes) {
                    excluded.addCidr(route);
                }
                const QStringList cidrs = excluded.toCidrs();
                if (!cidrs.isEmpty()) {
                    rules.append(QJsonObject { { QStringLiteral("ip"), QJsonArray::fromStringList(cidrs) },
                                               { QStringLiteral("outboundTag"), QStringLiteral("direct") } });
                }
            }

            QJsonObject routingSection;
            routingSection[QStringLiteral("domainStrategy")] =
                    profile.domainStrategy.isEmpty() ? QStringLiteral("IPIfNonMatch") : profile.domainStrategy;
            routingSection[QStringLiteral("rules")] = rules;
            xrayConfig[QStringLiteral("routing")] = routingSection;

            QJsonArray servers;
            for (const QString &server : remote) {
                servers.append(server);
            }
            const QStringList directDomains = expanded.xrayDomains.value(static_cast<int>(RuleAction::Direct));
            if (!directDomains.isEmpty() && directDomains.size() <= maxXrayDnsDomains) {
                servers.append(QJsonObject { { QStringLiteral("address"), domestic.first() },
                                             { QStringLiteral("port"), 53 },
                                             { QStringLiteral("domains"), QJsonArray::fromStringList(directDomains) } });
            }
            QJsonObject dns;
            dns[QStringLiteral("servers")] = servers;
            dns[QStringLiteral("queryStrategy")] = QStringLiteral("UseIPv4");
            QJsonObject hosts;
            for (auto it = profile.dnsHosts.constBegin(); it != profile.dnsHosts.constEnd(); ++it) {
                const QStringList addrs = ipList(it.value().toString());
                if (addrs.size() == 1) {
                    hosts[it.key()] = addrs.first();
                } else if (!addrs.isEmpty()) {
                    hosts[it.key()] = QJsonArray::fromStringList(addrs);
                }
            }
            if (!hosts.isEmpty()) {
                dns[QStringLiteral("hosts")] = hosts;
            }
            xrayConfig[QStringLiteral("dns")] = dns;
        }

        IpRoutes RoutingCompiler::ipRoutes(const CompileInput &input, const ExpandedProfile &expanded)
        {
            const RoutingProfile &profile = input.profile;
            IpRoutes result;
            result.includeMode = !profile.globalProxy;
            const RuleAction listed = result.includeMode ? RuleAction::Proxy : RuleAction::Direct;
            const int key = static_cast<int>(listed);

            IpRangeSet ranges;
            for (const QString &cidr : expanded.ips.value(key)) {
                ranges.addCidr(cidr);
            }
            if (!result.includeMode) {
                for (const QString &route : input.excludedRoutes) {
                    ranges.addCidr(route);
                }
            }
            result.cidrs = ranges.toCidrs();

            for (const QString &site : profile.sites(listed)) {
                const QString host = resolvableHost(site);
                if (!host.isEmpty()) {
                    if (!result.hostnames.contains(host)) {
                        result.hostnames.append(host);
                    }
                } else {
                    result.warnings.append(
                            QObject::tr("Rule \"%1\" needs domain based routing, which is not supported by this protocol").arg(site));
                }
            }
            if (!profile.blockSites.isEmpty() || !profile.blockIp.isEmpty()) {
                result.warnings.append(QObject::tr("Block rules are not supported by this protocol"));
            }
            return result;
        }
    } // namespace routing
} // namespace amnezia
