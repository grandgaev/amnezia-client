#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTest>

#include "core/installers/awgInstaller.h"
#include "core/models/protocols/awgProtocolConfig.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/constants/protocolConstants.h"
#include "core/utils/selfhosted/awgContainerUpgrade.h"

using namespace amnezia;

namespace
{
    // An AWG 2.0 server config as written by an older configure_container.sh.
    const char legacyServerConfig[] = "[Interface]\n"
                                      "PrivateKey = SERVERPRIVATEKEY=\n"
                                      "Address = 10.8.1.0/24\n"
                                      "ListenPort = 51820\n"
                                      "Jc = 5\n"
                                      "Jmin = 10\n"
                                      "Jmax = 50\n"
                                      "S1 = 24\n"
                                      "S2 = 64\n"
                                      "S3 = 18\n"
                                      "S4 = 7\n"
                                      "H1 = 100000-200000\n"
                                      "H2 = 300000-400000\n"
                                      "H3 = 500000-600000\n"
                                      "H4 = 700000-800000\n"
                                      "# I1 = <b 0xdeadbeef>\n"
                                      "\n"
                                      "[Peer]\n"
                                      "PublicKey = ADMINPUBLICKEY=\n"
                                      "PresharedKey = PSK=\n"
                                      "AllowedIPs = 10.8.1.2/32\n"
                                      "\n"
                                      "[Peer]\n"
                                      "PublicKey = USERPUBLICKEY=\n"
                                      "PresharedKey = PSK=\n"
                                      "AllowedIPs = 10.8.1.3/32\n"
                                      "\n";

    AwgServerConfig freshParams()
    {
        AwgServerConfig params;
        params.port = QStringLiteral("51820");
        params.subnetAddress = QStringLiteral("10.8.1.0");
        params.subnetCidr = QStringLiteral("24");
        AwgInstaller::generateAwgParameters(params);
        return params;
    }

    QMap<QString, QString> interfaceValues(const QString &config)
    {
        QMap<QString, QString> values;
        for (const QString &line : config.split('\n')) {
            if (line.trimmed() == QLatin1String("[Peer]")) {
                break;
            }
            const QStringList parts = line.split(QStringLiteral(" = "));
            if (parts.size() == 2 && !line.startsWith('#')) {
                values.insert(parts.at(0).trimmed(), parts.at(1).trimmed());
            }
        }
        return values;
    }
} // namespace

class TestContainerUpgrade : public QObject
{
    Q_OBJECT

private slots:
    void testSnapshot()
    {
        const AwgInterfaceSnapshot snapshot = parseAwgInterfaceSnapshot(QString::fromLatin1(legacyServerConfig));
        QVERIFY(snapshot.isValid);
        QCOMPARE(snapshot.privateKey, QStringLiteral("SERVERPRIVATEKEY="));
        QCOMPARE(snapshot.address, QStringLiteral("10.8.1.0/24"));
        QCOMPARE(snapshot.listenPort, QStringLiteral("51820"));
        QCOMPARE(snapshot.peerCount, 2);
        QVERIFY(snapshot.peersBlock.startsWith(QStringLiteral("[Peer]")));
        QVERIFY(snapshot.peersBlock.contains(QStringLiteral("USERPUBLICKEY=")));

        QVERIFY(!parseAwgInterfaceSnapshot(QString()).isValid);
        QVERIFY(!parseAwgInterfaceSnapshot(QStringLiteral("[Interface]\nAddress = 10.8.1.0/24\n")).isValid);
    }

    void testUpgradedServerConfig()
    {
        const AwgInterfaceSnapshot snapshot = parseAwgInterfaceSnapshot(QString::fromLatin1(legacyServerConfig));
        const AwgServerConfig params = freshParams();
        const QString upgraded = buildUpgradedAwgConfig(snapshot, params);

        // Keys, address, port and every peer are kept verbatim and in order.
        const QMap<QString, QString> values = interfaceValues(upgraded);
        QCOMPARE(values.value("PrivateKey"), QStringLiteral("SERVERPRIVATEKEY="));
        QCOMPARE(values.value("Address"), QStringLiteral("10.8.1.0/24"));
        QCOMPARE(values.value("ListenPort"), QStringLiteral("51820"));
        QVERIFY(upgraded.contains(snapshot.peersBlock));
        QVERIFY(upgraded.indexOf("ADMINPUBLICKEY=") < upgraded.indexOf("USERPUBLICKEY="));
        QCOMPARE(parseAwgInterfaceSnapshot(upgraded).peerCount, 2);

        // The obfuscation parameters are the fresh AmneziaWG 3.1 ones.
        QCOMPARE(values.value("H1"), params.initPacketMagicHeader);
        QCOMPARE(values.value("S4"), params.transportPacketJunkSize);
        QCOMPARE(values.value("HeaderProtectionKey"), params.headerProtectionKey);
        QVERIFY(!values.value("HeaderProtectionKey").isEmpty());
        QVERIFY(!upgraded.contains(QStringLiteral("100000-200000")));

        // Nothing left unsubstituted, no empty values.
        QVERIFY(!upgraded.contains('$'));
        static const QRegularExpression emptyValue(QStringLiteral(R"(^\s*\S+\s*=\s*$)"));
        for (const QString &line : upgraded.split('\n')) {
            QVERIFY2(!emptyValue.match(line).hasMatch(), qPrintable(line));
        }

        QVERIFY(buildUpgradedAwgConfig(AwgInterfaceSnapshot(), params).isEmpty());
    }

    void testFreshParamsAreAwg31AndSafe()
    {
        AwgProtocolConfig config;
        config.serverConfig = freshParams();
        QCOMPARE(config.serverProtocolVersion(), QString::fromLatin1(protocols::awg::awgV3));
        QVERIFY(!config.serverConfig.hasUnsafeRandomTrailersCombo());
    }

    void testAdminConfigRerender()
    {
        AwgClientConfig old;
        old.hostName = QStringLiteral("203.0.113.5");
        old.port = 51820;
        old.clientIp = QStringLiteral("10.8.1.2");
        old.clientPrivateKey = QStringLiteral("CLIENTPRIVATEKEY=");
        old.clientPublicKey = QStringLiteral("ADMINPUBLICKEY=");
        old.serverPublicKey = QStringLiteral("SERVERPUBLICKEY=");
        old.presharedKey = QStringLiteral("PSK=");
        old.clientId = QStringLiteral("ADMINPUBLICKEY=");
        old.mtu = QStringLiteral("1376");
        old.initPacketMagicHeader = QStringLiteral("100000-200000");
        old.nativeConfig = QStringLiteral("[Interface]\nAddress = 10.8.1.2/32\nDNS = 1.1.1.1, 1.0.0.1\n"
                                          "PrivateKey = CLIENTPRIVATEKEY=\nH1 = 100000-200000\n\n[Peer]\n"
                                          "PublicKey = SERVERPUBLICKEY=\n");

        const AwgServerConfig params = freshParams();
        const AwgClientConfig rendered = reRenderAwgAdminClientConfig(old, DockerContainer::Awg2, params);

        // Same peer: keys, address and PSK are kept.
        QCOMPARE(rendered.clientPrivateKey, old.clientPrivateKey);
        QCOMPARE(rendered.clientPublicKey, old.clientPublicKey);
        QCOMPARE(rendered.clientId, old.clientId);
        QCOMPARE(rendered.clientIp, old.clientIp);
        QCOMPARE(rendered.presharedKey, old.presharedKey);
        QCOMPARE(rendered.serverPublicKey, old.serverPublicKey);
        QCOMPARE(rendered.mtu, old.mtu);
        QVERIFY(rendered.nativeConfig.contains(QStringLiteral("PrivateKey = CLIENTPRIVATEKEY=")));
        QVERIFY(rendered.nativeConfig.contains(QStringLiteral("Address = 10.8.1.2/32")));
        QVERIFY(rendered.nativeConfig.contains(QStringLiteral("PresharedKey = PSK=")));

        // New parameters, same DNS, nothing unsubstituted.
        QCOMPARE(rendered.initPacketMagicHeader, params.initPacketMagicHeader);
        QCOMPARE(rendered.headerProtectionKey, params.headerProtectionKey);
        QVERIFY(rendered.nativeConfig.contains(QStringLiteral("DNS = 1.1.1.1, 1.0.0.1")));
        QVERIFY2(!rendered.nativeConfig.contains('$'), qPrintable(rendered.nativeConfig));
        QVERIFY(!rendered.nativeConfig.contains(QStringLiteral("100000-200000")));

        AwgProtocolConfig check;
        check.clientConfig = rendered;
        QCOMPARE(check.clientProtocolVersion(), QString::fromLatin1(protocols::awg::awgV3));
    }

    void testClientsFlagging()
    {
        auto client = [](const QString &id, const QString &name) {
            return QJsonObject { { configKey::clientId, id }, { configKey::userData, QJsonObject { { "clientName", name } } } };
        };
        const QJsonArray table { client("ADMIN=", "Admin"), client("USER=", "User"), QStringLiteral("garbage") };

        const QJsonArray flagged = flagClientsForConfigUpdate(table, QStringLiteral("ADMIN="), true);
        QCOMPARE(flagged.size(), 3);
        QVERIFY(!flagged.at(0).toObject().value(configKey::userData).toObject().contains(configKey::needsConfigUpdate));
        QVERIFY(flagged.at(1).toObject().value(configKey::userData).toObject().value(configKey::needsConfigUpdate).toBool());
        QCOMPARE(flagged.at(1).toObject().value(configKey::userData).toObject().value("clientName").toString(), QStringLiteral("User"));
        QCOMPARE(flagged.at(2), table.at(2));

        const QJsonArray cleared = flagClientsForConfigUpdate(flagged, QStringLiteral("ADMIN="), false);
        QVERIFY(!cleared.at(1).toObject().value(configKey::userData).toObject().contains(configKey::needsConfigUpdate));
    }

    void testRandomTrailersNormalization()
    {
        AwgServerConfig config = freshParams();
        config.randomTrailers = QStringLiteral("on");
        config.initPacketMagicHeader = QStringLiteral("100000-200000");
        config.initPacketJunkSize = QStringLiteral("24");
        config.transportPacketJunkSize = QStringLiteral("7");
        QVERIFY(config.hasUnsafeRandomTrailersCombo());
        config.normalizeRandomTrailersCombo();
        QVERIFY(!config.hasUnsafeRandomTrailersCombo());
        QVERIFY(!AwgProtocolConfig::isToggleEnabled(config.randomTrailers));

        // Equal padding sizes are safe even with ranged headers.
        AwgServerConfig equal = freshParams();
        equal.randomTrailers = QStringLiteral("on");
        equal.initPacketMagicHeader = QStringLiteral("100000-200000");
        QVERIFY(!equal.hasUnsafeRandomTrailersCombo());
    }
};

QTEST_MAIN(TestContainerUpgrade)
#include "testContainerUpgrade.moc"
