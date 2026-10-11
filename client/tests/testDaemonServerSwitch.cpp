#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "daemon/daemon.h"

namespace {

class FakeWireguardUtils final : public WireguardUtils {
 public:
  explicit FakeWireguardUtils(QObject* parent) : WireguardUtils(parent) {}

  bool interfaceExists() override { return interfaceUp; }
  bool addInterface(const InterfaceConfig&) override {
    interfaceUp = true;
    return true;
  }
  bool deleteInterface() override {
    interfaceUp = false;
    peers.clear();
    routes.clear();
    exclusions.clear();
    return true;
  }
  bool updatePeer(const InterfaceConfig& config) override {
    if (failedPeerKeys.contains(config.m_serverPublicKey)) {
      return false;
    }
    peers.insert(config.m_serverPublicKey);
    return true;
  }
  bool deletePeer(const InterfaceConfig& config) override {
    peers.remove(config.m_serverPublicKey);
    return true;
  }
  QList<PeerStatus> getPeerStatus() override {
    QList<PeerStatus> result;
    for (const QString& peer : peers) {
      result.append(PeerStatus(peer));
    }
    return result;
  }
  bool updateRoutePrefix(const IPAddress& prefix) override {
    if (failedRoutes.contains(prefix)) {
      return false;
    }
    routes.insert(prefix);
    return true;
  }
  bool deleteRoutePrefix(const IPAddress& prefix) override {
    routes.remove(prefix);
    return true;
  }
  bool addExclusionRoute(const IPAddress& prefix) override {
    exclusions.insert(prefix);
    return true;
  }
  bool deleteExclusionRoute(const IPAddress& prefix) override {
    exclusions.remove(prefix);
    return true;
  }
  bool excludeLocalNetworks(const QList<IPAddress>&) override { return true; }

  bool interfaceUp = false;
  QSet<QString> peers;
  QSet<IPAddress> routes;
  QSet<IPAddress> exclusions;
  QSet<QString> failedPeerKeys;
  QSet<IPAddress> failedRoutes;
};

class FakeDnsUtils final : public DnsUtils {
 public:
  explicit FakeDnsUtils(QObject* parent) : DnsUtils(parent) {}

  bool updateResolvers(const QString&,
                       const QList<QHostAddress>& resolvers) override {
    const QString primary = resolvers.isEmpty() ? QString()
                                                : resolvers.first().toString();
    if (failedResolvers.contains(primary)) {
      return false;
    }
    currentPrimary = primary;
    return true;
  }
  bool restoreResolvers() override {
    currentPrimary.clear();
    return true;
  }

  QString currentPrimary;
  QSet<QString> failedResolvers;
};

class FakeDaemon final : public Daemon {
 public:
  explicit FakeDaemon(QObject* parent = nullptr)
      : Daemon(parent), wire(this), dns(this) {}

  FakeWireguardUtils wire;
  FakeDnsUtils dns;
  QSet<QString> failedSwitchKeys;

 protected:
  WireguardUtils* wgutils() const override {
    return const_cast<FakeWireguardUtils*>(&wire);
  }
  DnsUtils* dnsutils() override { return &dns; }
  bool run(Op op, const InterfaceConfig& config) override {
    return op != Switch || !failedSwitchKeys.contains(config.m_serverPublicKey);
  }
};

InterfaceConfig makeConfig(const QString& key, const QString& dns,
                           const QString& route, const QString& exclusion,
                           const QString& routingConfig) {
  InterfaceConfig config;
  config.m_hopType = InterfaceConfig::SingleHop;
  config.m_privateKey = "private";
  config.m_deviceIpv4Address = "10.0.0.2";
  config.m_deviceIpv6Address = "fd00::2";
  config.m_serverIpv4Gateway = "10.0.0.1";
  config.m_serverIpv6Gateway = "fd00::1";
  config.m_serverPublicKey = key;
  config.m_serverIpv4AddrIn = "192.0.2.1";
  config.m_serverPort = 51820;
  config.m_killSwitchEnabled = false;
  config.m_primaryDnsServer = dns;
  config.m_allowedIPAddressRanges = {IPAddress(route)};
  config.m_excludedAddresses = {exclusion};
  config.m_routingConfig = routingConfig;
  return config;
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

}  // namespace

class TestDaemonServerSwitch : public QObject {
  Q_OBJECT

 private slots:
  void rollback_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("new peer") << QStringLiteral("peer");
    QTest::newRow("route") << QStringLiteral("route");
    QTest::newRow("dns") << QStringLiteral("dns");
    QTest::newRow("platform switch") << QStringLiteral("run");
  }

  void rollback() {
    QFETCH(QString, failure);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());

    const InterfaceConfig oldConfig =
        makeConfig("old-key", "9.9.9.9", "10.0.0.0/8",
                   "198.51.100.1/32", R"({"profile":"old"})");
    const InterfaceConfig newConfig =
        makeConfig("new-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.2/32", R"({"profile":"new"})");

    FakeDaemon daemon;
    QVERIFY(daemon.activate(oldConfig));
    if (failure == "peer") {
      daemon.wire.failedPeerKeys.insert(newConfig.m_serverPublicKey);
    } else if (failure == "route") {
      daemon.wire.failedRoutes.insert(newConfig.m_allowedIPAddressRanges.first());
    } else if (failure == "dns") {
      daemon.dns.failedResolvers.insert(newConfig.m_primaryDnsServer);
    } else {
      daemon.failedSwitchKeys.insert(newConfig.m_serverPublicKey);
    }

    QVERIFY(!daemon.activate(newConfig));
    QVERIFY(daemon.wire.peers ==
            QSet<QString>({oldConfig.m_serverPublicKey}));
    QVERIFY(daemon.wire.routes ==
            QSet<IPAddress>({oldConfig.m_allowedIPAddressRanges.first()}));
    QVERIFY(daemon.wire.exclusions ==
            QSet<IPAddress>({IPAddress(oldConfig.m_excludedAddresses.first())}));
    QCOMPARE(daemon.dns.currentPrimary, oldConfig.m_primaryDnsServer);
    QCOMPARE(readFile(Daemon::routingConfigFilePath()),
             oldConfig.m_routingConfig.toUtf8());
    QVERIFY(daemon.wire.interfaceUp);
    QVERIFY(daemon.deactivate(false));
  }

  void rollbackFailureDisconnects() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());

    const InterfaceConfig oldConfig =
        makeConfig("old-key", "9.9.9.9", "10.0.0.0/8",
                   "198.51.100.1/32", R"({"profile":"old"})");
    const InterfaceConfig newConfig =
        makeConfig("new-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.2/32", R"({"profile":"new"})");

    FakeDaemon daemon;
    QVERIFY(daemon.activate(oldConfig));
    daemon.wire.failedRoutes.insert(newConfig.m_allowedIPAddressRanges.first());
    daemon.wire.failedPeerKeys.insert(oldConfig.m_serverPublicKey);
    QSignalSpy disconnected(&daemon, &Daemon::disconnected);

    QVERIFY(!daemon.activate(newConfig));
    QCOMPARE(disconnected.count(), 1);
    QVERIFY(!daemon.wire.interfaceUp);
    QVERIFY(daemon.wire.peers.isEmpty());
    QVERIFY(!QFile::exists(Daemon::routingConfigFilePath()));
  }
};

QTEST_MAIN(TestDaemonServerSwitch)
#include "testDaemonServerSwitch.moc"
