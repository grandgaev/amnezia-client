#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include "core/models/routing/geoData.h"
#include "core/models/routing/ipRanges.h"
#include "core/models/routing/routingCompiler.h"
#include "core/models/routing/routingProfile.h"
#include "secureQSettings.h"
#include "utils/testCoreController.h"
#include "vpnConnection.h"

using namespace amnezia;
using namespace amnezia::routing;

#ifdef AMNEZIA_DESKTOP
    #define AMNEZIA_TEST_BYPASS "interface"
#else
    #define AMNEZIA_TEST_BYPASS "os"
#endif

namespace
{
    // Minimal protobuf writer for geo data test files.
    void writeVarint(QByteArray &out, quint64 value)
    {
        while (value >= 0x80) {
            out.append(char((value & 0x7f) | 0x80));
            value >>= 7;
        }
        out.append(char(value));
    }

    void writeBytes(QByteArray &out, int field, const QByteArray &value)
    {
        writeVarint(out, quint64(field) << 3 | 2);
        writeVarint(out, quint64(value.size()));
        out.append(value);
    }

    void writeUint(QByteArray &out, int field, quint64 value)
    {
        writeVarint(out, quint64(field) << 3);
        writeVarint(out, value);
    }

    struct Domain
    {
        int type;
        QString value;
        QStringList attributes;
    };

    QByteArray geoSite(const QString &code, const QList<Domain> &domains)
    {
        QByteArray site;
        writeBytes(site, 1, code.toUpper().toUtf8());
        for (const Domain &d : domains) {
            QByteArray domain;
            writeUint(domain, 1, quint64(d.type));
            writeBytes(domain, 2, d.value.toUtf8());
            for (const QString &attribute : d.attributes) {
                QByteArray attr;
                writeBytes(attr, 1, attribute.toUtf8());
                writeUint(attr, 2, 1);
                writeBytes(domain, 3, attr);
            }
            writeBytes(site, 2, domain);
        }
        QByteArray list;
        writeBytes(list, 1, site);
        return list;
    }

    QByteArray geoIp(const QString &code, const QList<QPair<QByteArray, int>> &cidrs)
    {
        QByteArray entry;
        writeBytes(entry, 1, code.toUpper().toUtf8());
        for (const auto &c : cidrs) {
            QByteArray cidr;
            writeBytes(cidr, 1, c.first);
            writeUint(cidr, 2, quint64(c.second));
            writeBytes(entry, 2, cidr);
        }
        QByteArray list;
        writeBytes(list, 1, entry);
        return list;
    }

    QByteArray ipv4(int a, int b, int c, int d)
    {
        QByteArray bytes;
        bytes.append(char(a)).append(char(b)).append(char(c)).append(char(d));
        return bytes;
    }

    bool writeFile(const QString &path, const QByteArray &data)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        return file.write(data) == data.size();
    }

    QStringList toStringList(const QJsonValue &value)
    {
        QStringList result;
        for (const QJsonValue &v : value.toArray()) {
            result.append(v.toString());
        }
        return result;
    }
} // namespace

class TestRouting : public QObject
{
    Q_OBJECT

private:
    TestCoreController *m_coreController;
    SecureQSettings *m_settings;
    QSharedPointer<VpnConnection> m_vpnConnection;

    QString createActiveProfile(const QString &proxyRules, bool globalProxy)
    {
        RoutingController *routing = m_coreController->m_routingController;
        const QString id = routing->createProfile(QStringLiteral("Connection"));
        auto profile = routing->profile(id);
        profile->globalProxy = globalProxy;
        profile->directIp.clear();
        profile->dnsHosts.clear();
        profile->setRulesText(RuleAction::Proxy, proxyRules);
        routing->updateProfile(*profile);
        routing->selectProfile(id);
        routing->setRoutingEnabled(true);
        return id;
    }

private slots:
    void initTestCase()
    {
        QString testOrg = "AmneziaVPN-Test-" + QUuid::createUuid().toString();
        m_settings = new SecureQSettings(testOrg, "amnezia-client", nullptr, false);

        m_vpnConnection = QSharedPointer<VpnConnection>::create(nullptr, nullptr);

        m_coreController = new TestCoreController(m_vpnConnection, m_settings, nullptr, this);
    }

    void cleanupTestCase()
    {
        m_settings->clearSettings();
        delete m_coreController;
        delete m_settings;
    }

    void init()
    {
        m_coreController->m_settingsController->clearSettings();
    }

    void testRulesText()
    {
        RoutingProfile profile = RoutingProfile::createDefault(QStringLiteral("Rules"));
        profile.setRulesText(RuleAction::Proxy, QStringLiteral("# comment\n"
                                                               "youtube.com, domain:googlevideo.com\n"
                                                               "geosite:category-ru\n"
                                                               "1.2.3.4\n"
                                                               "10.0.0.0/8\n"
                                                               "geoip:ru\n"
                                                               "2001:db8::/32\n"));
        QCOMPARE(profile.proxySites,
                 QStringList({ "youtube.com", "domain:googlevideo.com", "geosite:category-ru" }));
        QCOMPARE(profile.proxyIp, QStringList({ "1.2.3.4", "10.0.0.0/8", "geoip:ru", "2001:db8::/32" }));
        QVERIFY(profile.rulesText(RuleAction::Proxy).contains(QStringLiteral("youtube.com")));
        QCOMPARE(profile.geoSiteTags(), QStringList({ "category-ru" }));
        QCOMPARE(profile.geoIpTags(), QStringList({ "ru" }));
    }

    void testHappImport_data()
    {
        QTest::addColumn<QString>("data");

        QJsonObject happ;
        happ["Name"] = "RuBypass";
        happ["GlobalProxy"] = "true";
        happ["DirectSites"] = QJsonArray { "geosite:category-ru", "domain:yandex.ru" };
        happ["DirectIp"] = QJsonArray { "geoip:ru", "192.168.0.0/16" };
        happ["ProxySites"] = QJsonArray { "full:rutracker.org" };
        happ["BlockSites"] = QJsonArray { "geosite:category-ads-all" };
        happ["RouteOrder"] = "direct-proxy-block";
        happ["DomainStrategy"] = "AsIs";
        happ["RemoteDNSType"] = "DoH";
        happ["RemoteDNSDomain"] = "https://cloudflare-dns.com/dns-query";
        happ["RemoteDNSIP"] = "1.1.1.1";
        happ["DomesticDNSType"] = "DoU";
        happ["DomesticDNSIP"] = "77.88.8.8";
        happ["DnsHosts"] = QJsonObject { { "cloudflare-dns.com", "1.1.1.1" } };
        happ["Geoipurl"] = "https://example.com/geoip.dat";
        happ["Geositeurl"] = "https://example.com/geosite.dat";

        const QByteArray json = QJsonDocument(happ).toJson(QJsonDocument::Compact);
        QTest::newRow("json") << QString::fromUtf8(json);
        QTest::newRow("base64") << QString::fromLatin1(json.toBase64());
        QTest::newRow("base64url") << QString::fromLatin1(json.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        QTest::newRow("happ link") << QStringLiteral("happ://routing/add/") + QString::fromLatin1(json.toBase64());
        QTest::newRow("lowercase keys") << QString::fromUtf8(json).toLower();
    }

    void testHappImport()
    {
        QFETCH(QString, data);
        QString error;
        const QList<QJsonObject> objects = parseImportData(data, &error);
        QVERIFY2(objects.size() == 1, qPrintable(error));

        RoutingProfile profile = RoutingProfile::createDefault(QString());
        profile.mergeExchangeJson(objects.first());
        QCOMPARE(profile.name.toLower(), QStringLiteral("rubypass"));
        QVERIFY(profile.globalProxy);
        QCOMPARE(profile.directSites, QStringList({ "geosite:category-ru", "domain:yandex.ru" }));
        QCOMPARE(profile.directIp, QStringList({ "geoip:ru", "192.168.0.0/16" }));
        QCOMPARE(profile.proxySites, QStringList({ "full:rutracker.org" }));
        QCOMPARE(profile.blockSites, QStringList({ "geosite:category-ads-all" }));
        QCOMPARE(profile.routeOrder, QStringLiteral("direct-proxy-block"));
        QCOMPARE(profile.remoteDnsType, QStringLiteral("DoH"));
        QCOMPARE(profile.remoteDnsIp, QStringLiteral("1.1.1.1"));
        QCOMPARE(profile.domesticDnsType, QStringLiteral("DoU"));
        QCOMPARE(profile.domesticDnsIp, QStringLiteral("77.88.8.8"));
        QCOMPARE(profile.dnsHosts.value("cloudflare-dns.com").toString(), QStringLiteral("1.1.1.1"));
        QCOMPARE(profile.geoIpUrl, QStringLiteral("https://example.com/geoip.dat"));
        QCOMPARE(profile.geoSiteUrl, QStringLiteral("https://example.com/geosite.dat"));
    }

    void testExportRoundTrip()
    {
        RoutingProfile profile = RoutingProfile::createDefault(QStringLiteral("Round trip"));
        profile.globalProxy = false;
        profile.setRulesText(RuleAction::Proxy, QStringLiteral("youtube.com\n8.8.8.0/24"));
        profile.setRulesText(RuleAction::Block, QStringLiteral("keyword:ads"));
        profile.routeOrder = QStringLiteral("proxy-direct-block");

        const QJsonObject exported = profile.toExchangeJson();
        QCOMPARE(exported.value("Name").toString(), QStringLiteral("Round trip"));
        QCOMPARE(exported.value("GlobalProxy").toString(), QStringLiteral("false"));

        RoutingProfile imported = RoutingProfile::createDefault(QString());
        imported.mergeExchangeJson(exported);
        QCOMPARE(imported.name, profile.name);
        QCOMPARE(imported.globalProxy, profile.globalProxy);
        QCOMPARE(imported.proxySites, profile.proxySites);
        QCOMPARE(imported.proxyIp, profile.proxyIp);
        QCOMPARE(imported.blockSites, profile.blockSites);
        QCOMPARE(imported.routeOrder, profile.routeOrder);

        const RoutingProfile stored = RoutingProfile::fromJson(profile.toJson());
        QCOMPARE(stored.id, profile.id);
        QCOMPARE(stored.proxySites, profile.proxySites);
        QCOMPARE(stored.directIp, profile.directIp);
    }

    void testListImports()
    {
        // Plain text list
        QList<QJsonObject> objects = parseImportData(QStringLiteral("youtube.com\n# comment\n1.2.3.0/24, domain:ggpht.com\n"));
        QCOMPARE(objects.size(), 1);
        RoutingProfile profile = RoutingProfile::createDefault(QString());
        profile.mergeExchangeJson(objects.first());
        QVERIFY(!profile.globalProxy);
        QCOMPARE(profile.proxySites, QStringList({ "youtube.com", "domain:ggpht.com" }));
        QCOMPARE(profile.proxyIp, QStringList({ "1.2.3.0/24" }));

        // Site list of older AmneziaVPN versions
        objects = parseImportData(QStringLiteral(R"([{"hostname": "example.com", "ip": "93.184.216.34"}, {"hostname": "1.2.3.4"}])"));
        QCOMPARE(objects.size(), 1);
        profile = RoutingProfile::createDefault(QString());
        profile.mergeExchangeJson(objects.first());
        QCOMPARE(profile.proxySites, QStringList({ "example.com" }));
        QCOMPARE(profile.proxyIp, QStringList({ "1.2.3.4" }));

        // Garbage
        QString error;
        QVERIFY(parseImportData(QStringLiteral("this is not a profile"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void testIpRanges()
    {
        IpRangeSet set;
        set.addCidr(QStringLiteral("10.0.0.0/9"));
        set.addCidr(QStringLiteral("10.128.0.0/9"));
        set.addCidr(QStringLiteral("192.168.1.1"));
        set.addCidr(QStringLiteral("2001:db8::/32"));
        QCOMPARE(set.toCidrs(), QStringList({ "10.0.0.0/8", "192.168.1.1/32", "2001:db8::/32" }));

        IpRangeSet v4;
        v4.addCidr(QStringLiteral("0.0.0.0/1"));
        const QStringList complement = v4.complement().toCidrs();
        QVERIFY(complement.contains(QStringLiteral("128.0.0.0/1")));
        QVERIFY(complement.contains(QStringLiteral("::/0")));
    }

    void testGeoData()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sitePath = dir.filePath("geosite.dat");
        const QString ipPath = dir.filePath("geoip.dat");

        QByteArray sites = geoSite("test", { { 2, "example.com", {} },
                                             { 3, "full.example.org", {} },
                                             { 0, "tracker", { "ads" } },
                                             { 1, "^re\\.example\\.net$", {} } });
        sites.append(geoSite("other", { { 2, "other.com", {} } }));
        QVERIFY(writeFile(sitePath, sites));
        QVERIFY(writeFile(ipPath, geoIp("xx", { { ipv4(5, 6, 0, 0), 16 }, { ipv4(7, 0, 0, 0), 8 } })));

        QVERIFY(GeoData::validate(sites, false));
        QCOMPARE(GeoData::listTags(sitePath), QStringList({ "other", "test" }));

        QStringList missing;
        QStringList expanded = GeoData::expandSites(sitePath, { "test", "absent" }, &missing);
        QCOMPARE(expanded, QStringList({ "domain:example.com", "full:full.example.org", "keyword:tracker", "regexp:^re\\.example\\.net$" }));
        QCOMPARE(missing, QStringList({ "absent" }));

        expanded = GeoData::expandSites(sitePath, { "test@ads" });
        QCOMPARE(expanded, QStringList({ "keyword:tracker" }));
        expanded = GeoData::expandSites(sitePath, { "test@!ads" });
        QCOMPARE(expanded.size(), 3);

        QCOMPARE(GeoData::expandIps(ipPath, { "xx" }), QStringList({ "5.6.0.0/16", "7.0.0.0/8" }));
        const QStringList reversed = GeoData::expandIps(ipPath, { "!xx" });
        QVERIFY(!reversed.contains(QStringLiteral("7.0.0.0/8")));
        QVERIFY(reversed.contains(QStringLiteral("8.0.0.0/5")));
    }

    void testRouterConfig()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sitePath = dir.filePath("geosite.dat");
        const QString ipPath = dir.filePath("geoip.dat");
        QVERIFY(writeFile(sitePath, geoSite("ru", { { 2, "yandex.ru", {} } })));
        QVERIFY(writeFile(ipPath, geoIp("ru", { { ipv4(5, 3, 0, 0), 16 } })));

        CompileInput input;
        input.profile = RoutingProfile::createDefault(QStringLiteral("Compile"));
        input.profile.globalProxy = true;
        input.profile.directIp.clear();
        input.profile.setRulesText(RuleAction::Direct, QStringLiteral("geosite:ru\ngeoip:ru\nvk.com"));
        input.profile.setRulesText(RuleAction::Block, QStringLiteral("full:ads.example.com\n6.6.6.6"));
        input.profile.setRulesText(RuleAction::Proxy, QStringLiteral("regexp:^.*\\.onion$"));
        input.profile.dnsHosts.clear();
        input.geoSitePath = sitePath;
        input.geoIpPath = ipPath;
        input.excludedRoutes = QStringList({ "192.168.0.0/16" });
        input.connectionDns = QStringList({ "10.8.1.1" });

        const ExpandedProfile expanded = RoutingCompiler::expand(input);
        QVERIFY2(expanded.warnings.isEmpty(), qPrintable(expanded.warnings.join("; ")));
        const QJsonObject config = RoutingCompiler::routerConfig(input, expanded);

        QCOMPARE(config.value("defaultAction").toString(), QStringLiteral("proxy"));
        const QJsonArray rules = config.value("rules").toArray();
        QCOMPARE(rules.size(), 4);
        // Default order: block, direct, proxy, then the excluded routes.
        QCOMPARE(rules.at(0).toObject().value("action").toString(), QStringLiteral("block"));
        QCOMPARE(toStringList(rules.at(0).toObject().value("domains")), QStringList({ "full:ads.example.com" }));
        QCOMPARE(toStringList(rules.at(0).toObject().value("ips")), QStringList({ "6.6.6.6/32" }));
        QCOMPARE(rules.at(1).toObject().value("action").toString(), QStringLiteral("direct"));
        const QStringList directDomains = toStringList(rules.at(1).toObject().value("domains"));
        QVERIFY(directDomains.contains(QStringLiteral("domain:yandex.ru")));
        // Plain entries match as keywords, as in Xray.
        QVERIFY(directDomains.contains(QStringLiteral("keyword:vk.com")));
        QCOMPARE(toStringList(rules.at(1).toObject().value("ips")), QStringList({ "5.3.0.0/16" }));
        QCOMPARE(rules.at(2).toObject().value("action").toString(), QStringLiteral("proxy"));
        QCOMPARE(rules.at(3).toObject().value("action").toString(), QStringLiteral("direct"));
        QCOMPARE(toStringList(rules.at(3).toObject().value("ips")), QStringList({ "192.168.0.0/16" }));

        const QJsonObject dns = config.value("dns").toObject();
        QCOMPARE(toStringList(dns.value("listen")), QStringList({ QString::fromLatin1(routerDnsAddress) }));
        QCOMPARE(toStringList(dns.value("remote")), QStringList({ "10.8.1.1" }));
        QVERIFY(!toStringList(dns.value("direct")).isEmpty());
        QCOMPARE(config.value("bypass").toString(), QStringLiteral("os"));
    }

    void testXrayConfig()
    {
        QJsonObject xray = QJsonDocument::fromJson(R"({
            "inbounds": [{"protocol": "socks", "listen": "127.0.0.1", "port": 10808}],
            "outbounds": [{"protocol": "vless", "settings": {}}]
        })").object();

        CompileInput input;
        input.profile = RoutingProfile::createDefault(QStringLiteral("Xray"));
        input.profile.globalProxy = false;
        input.profile.directIp.clear();
        input.profile.setRulesText(RuleAction::Proxy, QStringLiteral("youtube.com\n8.8.8.8"));
        input.connectionDns = QStringList({ "1.1.1.1" });

        const ExpandedProfile expanded = RoutingCompiler::expand(input);
        RoutingCompiler::applyToXrayConfig(xray, input, expanded);

        const QJsonArray outbounds = xray.value("outbounds").toArray();
        QStringList tags;
        for (const QJsonValue &o : outbounds) {
            tags.append(o.toObject().value("tag").toString());
        }
        // Not global proxy: the first outbound (the default one) is "direct".
        QCOMPARE(tags.first(), QStringLiteral("direct"));
        QVERIFY(tags.contains(QStringLiteral("proxy")));
        QVERIFY(tags.contains(QStringLiteral("block")));

        const QJsonArray rules = xray.value("routing").toObject().value("rules").toArray();
        bool hasProxyDomain = false;
        bool hasProxyIp = false;
        for (const QJsonValue &r : rules) {
            const QJsonObject rule = r.toObject();
            if (rule.value("outboundTag").toString() != QLatin1String("proxy")) {
                continue;
            }
            hasProxyDomain |= toStringList(rule.value("domain")).contains(QStringLiteral("youtube.com"));
            hasProxyIp |= toStringList(rule.value("ip")).contains(QStringLiteral("8.8.8.8/32"));
        }
        QVERIFY(hasProxyDomain);
        QVERIFY(hasProxyIp);
        QVERIFY(xray.value("inbounds").toArray().first().toObject().value("sniffing").toObject().value("enabled").toBool());
    }

    void testIpRoutes()
    {
        CompileInput input;
        input.profile = RoutingProfile::createDefault(QStringLiteral("Routes"));
        input.profile.globalProxy = false;
        input.profile.directIp.clear();
        input.profile.setRulesText(RuleAction::Proxy, QStringLiteral("example.com\n8.8.8.0/24\nkeyword:abc"));
        const IpRoutes routes = RoutingCompiler::ipRoutes(input, RoutingCompiler::expand(input));
        QVERIFY(routes.includeMode);
        QCOMPARE(routes.cidrs, QStringList({ "8.8.8.0/24" }));
        QCOMPARE(routes.hostnames, QStringList({ "example.com" }));
        QVERIFY(!routes.warnings.isEmpty());
    }

    void testControllerProfiles()
    {
        RoutingController *routing = m_coreController->m_routingController;
        RoutingUiController *ui = m_coreController->m_routingUiController;

        QString error;
        const QString first = routing->createProfile(QStringLiteral("First"), &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        QCOMPARE(routing->selectedProfileId(), first);
        QVERIFY(routing->isRoutingEnabled());
        QVERIFY(routing->createProfile(QStringLiteral("first"), &error).isEmpty());

        const QString second = routing->duplicateProfile(first);
        QVERIFY(!second.isEmpty());
        QVERIFY(routing->renameProfile(second, QStringLiteral("Second")));
        QCOMPARE(m_coreController->m_routingProfilesModel->rowCount(), 2);

        ui->setCurrentProfileId(second);
        QVERIFY(ui->setRulesText(RoutingUiController::Proxy, QStringLiteral("youtube.com\n1.1.1.1")).isEmpty());
        QCOMPARE(ui->rulesCount(RoutingUiController::Proxy), 2);
        QVERIFY(!ui->setRulesText(RoutingUiController::Direct, QStringLiteral("256.1.1.1/40")).isEmpty());

        routing->selectProfile(second);
        QCOMPARE(RoutingController::activeProfile(m_coreController->m_appSettingsRepository, QString())->id, second);
        routing->setServerOverride(QStringLiteral("server-1"), RoutingController::overrideOff);
        QVERIFY(!RoutingController::activeProfile(m_coreController->m_appSettingsRepository, QStringLiteral("server-1")).has_value());
        routing->setServerOverride(QStringLiteral("server-2"), first);
        QCOMPARE(RoutingController::activeProfile(m_coreController->m_appSettingsRepository, QStringLiteral("server-2"))->id, first);

        const QString exported = routing->exportProfile(second);
        QVERIFY(exported.contains(QStringLiteral("youtube.com")));

        QVERIFY(routing->removeProfile(second));
        QCOMPARE(routing->selectedProfileId(), first);

        // Import with a name conflict, then replace.
        QSignalSpy conflictSpy(ui, &RoutingUiController::importConflict);
        QSignalSpy importedSpy(ui, &RoutingUiController::profileImported);
        ui->importFromText(QStringLiteral(R"({"Name": "First", "ProxySites": ["example.org"]})"));
        QCOMPARE(conflictSpy.count(), 1);
        ui->resolveImportConflict(true);
        QCOMPARE(importedSpy.count(), 1);
        QCOMPARE(routing->profile(first)->proxySites, QStringList({ "example.org" }));

        routing->setRoutingEnabled(false);
        ui->importFromText(QStringLiteral("happ://routing/onadd/")
                           + QString::fromLatin1(QByteArray(R"({"Name": "Activated"})").toBase64()));
        QCOMPARE(importedSpy.count(), 2);
        QVERIFY(routing->isRoutingEnabled());
        QCOMPARE(routing->profile(routing->selectedProfileId())->name, QStringLiteral("Activated"));
    }

    void testConnectionRouterMode()
    {
        createActiveProfile(QStringLiteral("youtube.com\n8.8.8.0/24"), false);
        const QJsonObject awgData { { "allowed_ips", QJsonArray { "0.0.0.0/0", "::/0" } } };
        const QJsonObject config { { "vpnProto", "awg" }, { "dns1", "1.1.1.1" }, { "dns2", "1.0.0.1" }, { "awg_config_data", awgData } };

        const QJsonObject result = m_vpnConnection->withRoutingConfiguration(QStringLiteral("server"), config);
        const QJsonObject router = result.value("routing_config").toObject();
        QVERIFY(!router.isEmpty());
        QCOMPARE(router.value("defaultAction").toString(), QStringLiteral("direct"));
        QCOMPARE(router.value("bypass").toString(), QStringLiteral(AMNEZIA_TEST_BYPASS));
        QCOMPARE(toStringList(router.value("dns").toObject().value("remote")), QStringList({ "1.1.1.1", "1.0.0.1" }));
        QCOMPARE(result.value("dns1").toString(), QString::fromLatin1(routerDnsAddress));
        QCOMPARE(result.value("dns2").toString(), QString::fromLatin1(routerDnsAddress));
        QCOMPARE(result.value("splitTunnelType").toInt(), 0);

        // A tunnel that does not cover everything gets a route for the DNS address.
        const QJsonObject partial { { "vpnProto", "wireguard" }, { "dns1", "10.0.0.1" },
                                    { "wireguard_config_data", QJsonObject { { "allowed_ips", QJsonArray { "10.0.0.0/8" } } } } };
        const QJsonObject partialResult = m_vpnConnection->withRoutingConfiguration(QStringLiteral("server"), partial);
        QVERIFY(partialResult.value("wireguard_config_data").toObject().value("allowed_ips").toArray().contains(QStringLiteral("198.18.0.53/32")));

        // Routing disabled for this server.
        m_coreController->m_routingController->setServerOverride(QStringLiteral("server"), RoutingController::overrideOff);
        const QJsonObject off = m_vpnConnection->withRoutingConfiguration(QStringLiteral("server"), config);
        QVERIFY(!off.contains("routing_config"));
        QCOMPARE(off.value("dns1").toString(), QStringLiteral("1.1.1.1"));
    }

    void testConnectionXrayMode()
    {
        createActiveProfile(QStringLiteral("youtube.com"), false);
        const QString xray = QStringLiteral(R"({"inbounds":[{"protocol":"socks","port":10808}],"outbounds":[{"protocol":"vless"}]})");
        const QJsonObject config { { "vpnProto", "xray" }, { "dns1", "1.1.1.1" }, { "xray_config_data", QJsonObject { { "config", xray } } } };

        const QJsonObject result = m_vpnConnection->withRoutingConfiguration(QStringLiteral("server"), config);
        QVERIFY(!result.contains("routing_config"));
        const QJsonObject xrayConfig =
                QJsonDocument::fromJson(result.value("xray_config_data").toObject().value("config").toString().toUtf8()).object();
        QVERIFY(xrayConfig.contains("routing"));
        QCOMPARE(xrayConfig.value("outbounds").toArray().first().toObject().value("tag").toString(), QStringLiteral("direct"));
        QCOMPARE(result.value("dns1").toString(), QStringLiteral("1.1.1.1"));
    }

    void testConnectionAddressMode()
    {
        createActiveProfile(QStringLiteral("8.8.8.0/24\n9.9.9.9"), false);
        const QJsonObject config { { "vpnProto", "openvpn" }, { "dns1", "1.1.1.1" }, { "dns2", "1.0.0.1" } };

        const QJsonObject result = m_vpnConnection->withRoutingConfiguration(QStringLiteral("server"), config);
        QCOMPARE(result.value("splitTunnelType").toInt(), int(amnezia::RouteMode::VpnOnlyForwardSites));
        const QStringList sites = toStringList(result.value("splitTunnelSites"));
        QVERIFY(sites.contains(QStringLiteral("8.8.8.0/24")));
        QVERIFY(sites.contains(QStringLiteral("9.9.9.9/32")));
        QVERIFY(sites.contains(QStringLiteral("1.1.1.1")));
    }

    void testLegacyMigration()
    {
        SecureAppSettingsRepository *repository = m_coreController->m_appSettingsRepository;
        repository->setLegacySplitTunnelingMigrated(false);
        repository->addVpnSites(RouteMode::VpnOnlyForwardSites,
                                QMap<QString, QStringList> { { QStringLiteral("example.com"), QStringList { QStringLiteral("93.184.216.34") } },
                                                             { QStringLiteral("10.1.2.3"), QStringList() } });
        repository->setRouteMode(RouteMode::VpnOnlyForwardSites);
        repository->setSitesSplitTunnelingEnabled(true);

        m_coreController->m_routingController->migrateLegacySplitTunneling();

        QVERIFY(repository->isLegacySplitTunnelingMigrated());
        QVERIFY(repository->isRoutingEnabled());
        QVERIFY(!repository->isSitesSplitTunnelingEnabled());
        const auto profile = RoutingController::activeProfile(repository, QString());
        QVERIFY(profile.has_value());
        QVERIFY(!profile->globalProxy);
        QCOMPARE(profile->proxySites, QStringList({ "domain:example.com" }));
        QCOMPARE(profile->proxyIp, QStringList({ "10.1.2.3" }));
        QVERIFY(profile->directIp.contains(QStringLiteral("192.168.0.0/16")));

        // Idempotent
        m_coreController->m_routingController->migrateLegacySplitTunneling();
        QCOMPARE(m_coreController->m_routingController->profiles().size(), 1);
    }
};

QTEST_MAIN(TestRouting)
#include "testRouting.moc"
