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
    return !failInterfaceCreation;
  }
  bool deleteInterface() override {
    interfaceUp = false;
    peers.clear();
    routes.clear();
    return true;
  }
  bool updatePeer(const InterfaceConfig& config) override {
    peers.insert(config.m_serverPublicKey);
    if (failedPeerKeys.contains(config.m_serverPublicKey)) {
      return false;
    }
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
    routes.insert(prefix);
    if (failedRoutes.contains(prefix)) {
      return false;
    }
    return true;
  }
  bool deleteRoutePrefix(const IPAddress& prefix) override {
    routes.remove(prefix);
    return true;
  }
  bool addExclusionRoute(const IPAddress& prefix) override {
    if (failedExclusions.contains(prefix)) {
      return false;
    }
    exclusions.insert(prefix);
    return true;
  }
  bool deleteExclusionRoute(const IPAddress& prefix) override {
    exclusions.remove(prefix);
    return true;
  }
  bool excludeLocalNetworks(const QList<IPAddress>&) override { return true; }

  bool interfaceUp = false;
  bool failInterfaceCreation = false;
  QSet<QString> peers;
  QSet<IPAddress> routes;
  QSet<IPAddress> exclusions;
  QSet<QString> failedPeerKeys;
  QSet<IPAddress> failedRoutes;
  QSet<IPAddress> failedExclusions;
};

class FakeDnsUtils final : public DnsUtils {
 public:
  explicit FakeDnsUtils(QObject* parent) : DnsUtils(parent) {}

  bool updateResolvers(const QString&,
                       const QList<QHostAddress>& resolvers) override {
    const QString primary = resolvers.isEmpty() ? QString()
                                                : resolvers.first().toString();
    currentPrimary = primary;
    if (failedResolvers.contains(primary)) {
      return false;
    }
    return true;
  }
  bool restoreResolvers() override {
    if (failRestoration) {
      return false;
    }
    currentPrimary.clear();
    return true;
  }

  QString currentPrimary;
  QSet<QString> failedResolvers;
  bool failRestoration = false;
};

class FakeIPUtils final : public IPUtils {
 public:
  explicit FakeIPUtils(QObject* parent) : IPUtils(parent) {}
  bool addInterfaceIPs(const InterfaceConfig& config) override {
    const auto addAddress = [&](const QString& address) {
      if (address.isEmpty()) {
        return true;
      }
      attemptedAddresses.append(address);
      addresses.insert(address);
      return !failAfterFirstAddress && !failedAddresses.contains(address);
    };
    return addAddress(config.m_deviceIpv4Address) &&
           addAddress(config.m_deviceIpv6Address);
  }
  bool setMTUAndUp(const InterfaceConfig& config) override {
    mtu = config.m_deviceMTU;
    return !failUp && !failedMtus.contains(config.m_deviceMTU);
  }
  bool removeInterfaceIPs(const InterfaceConfig& config) override {
    addresses.remove(config.m_deviceIpv4Address);
    addresses.remove(config.m_deviceIpv6Address);
    return true;
  }
  QSet<QString> addresses;
  QList<QString> attemptedAddresses;
  QSet<QString> failedAddresses;
  QSet<int> failedMtus;
  int mtu = 0;
  bool failAfterFirstAddress = false;
  bool failUp = false;
};

class FakeDaemon final : public Daemon {
 public:
  explicit FakeDaemon(QObject* parent = nullptr)
      : Daemon(parent), wire(this), dns(this), ip(this) {}

  FakeWireguardUtils wire;
  FakeDnsUtils dns;
  FakeIPUtils ip;
  QSet<QString> failedSwitchKeys;
  QSet<QString> failedUpKeys;
  int platformCleanupCalls = 0;

 protected:
  WireguardUtils* wgutils() const override {
    return const_cast<FakeWireguardUtils*>(&wire);
  }
  DnsUtils* dnsutils() override { return &dns; }
  bool supportIPUtils() const override { return true; }
  IPUtils* iputils() override { return &ip; }
  bool run(Op op, const InterfaceConfig& config) override {
    if (op == Down) {
      ++platformCleanupCalls;
    }
    return (op != Switch || !failedSwitchKeys.contains(config.m_serverPublicKey))
        && (op != Up || !failedUpKeys.contains(config.m_serverPublicKey));
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
  void activationFailure_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("partial interface") << QStringLiteral("interface");
    QTest::newRow("addresses") << QStringLiteral("addresses");
    QTest::newRow("interface up") << QStringLiteral("up");
    QTest::newRow("exclusion") << QStringLiteral("exclusion");
    QTest::newRow("partial peer") << QStringLiteral("peer");
    QTest::newRow("partial dns") << QStringLiteral("dns");
    QTest::newRow("partial route") << QStringLiteral("route");
    QTest::newRow("platform up") << QStringLiteral("run");
  }

  void activationFailure() {
    QFETCH(QString, failure);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());
    const InterfaceConfig config =
        makeConfig("new-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.2/32", R"({"profile":"new"})");
    FakeDaemon daemon;
    daemon.wire.failInterfaceCreation = failure == "interface";
    daemon.ip.failAfterFirstAddress = failure == "addresses";
    daemon.ip.failUp = failure == "up";
    if (failure == "exclusion") {
      daemon.wire.failedExclusions.insert(IPAddress(config.m_excludedAddresses.first()));
    } else if (failure == "peer") {
      daemon.wire.failedPeerKeys.insert(config.m_serverPublicKey);
    } else if (failure == "dns") {
      daemon.dns.failedResolvers.insert(config.m_primaryDnsServer);
    } else if (failure == "route") {
      daemon.wire.failedRoutes.insert(config.m_allowedIPAddressRanges.first());
    } else if (failure == "run") {
      daemon.failedUpKeys.insert(config.m_serverPublicKey);
    }
    QSignalSpy failed(&daemon, &Daemon::activationFailure);
    QVERIFY(!daemon.activate(config));
    QCOMPARE(failed.count(), 1);
    QVERIFY(!daemon.wire.interfaceUp);
    QVERIFY(daemon.wire.peers.isEmpty());
    QVERIFY(daemon.wire.routes.isEmpty());
    QVERIFY(daemon.wire.exclusions.isEmpty());
    QVERIFY(daemon.dns.currentPrimary.isEmpty());
    QVERIFY(!QFile::exists(Daemon::routingConfigFilePath()));
    QCOMPARE(daemon.platformCleanupCalls, failure == "run" ? 1 : 0);
  }

  void activationRollbackFailureDisconnects() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());
    const InterfaceConfig config =
        makeConfig("new-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.2/32", R"({"profile":"new"})");
    FakeDaemon daemon;
    daemon.dns.failedResolvers.insert(config.m_primaryDnsServer);
    daemon.dns.failRestoration = true;
    QSignalSpy disconnected(&daemon, &Daemon::disconnected);
    QSignalSpy fatal(&daemon, &Daemon::backendFailure);
    QVERIFY(!daemon.activate(config));
    QCOMPARE(disconnected.count(), 1);
    QCOMPARE(fatal.count(), 1);
    QVERIFY(!daemon.wire.interfaceUp);
    QVERIFY(daemon.wire.peers.isEmpty());
    QVERIFY(daemon.wire.exclusions.isEmpty());
    QVERIFY(!QFile::exists(Daemon::routingConfigFilePath()));
  }

  void additionalHopRollbackRestoresInterface_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("partial address mutation") << QStringLiteral("addresses");
    QTest::newRow("partial MTU mutation") << QStringLiteral("mtu");
    QTest::newRow("DNS after interface mutation") << QStringLiteral("dns");
  }

  void additionalHopRollbackRestoresInterface() {
    QFETCH(QString, failure);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());
    InterfaceConfig entry =
        makeConfig("entry-key", "9.9.9.9", "10.0.0.0/8",
                   "198.51.100.1/32", R"({"profile":"entry"})");
    entry.m_hopType = InterfaceConfig::MultiHopEntry;
    entry.m_deviceIpv4Address = "10.1.0.2/24";
    entry.m_deviceIpv6Address = "fd01::2/64";
    entry.m_deviceMTU = 1420;
    InterfaceConfig exit =
        makeConfig("exit-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.1/32", R"({"profile":"exit"})");
    exit.m_hopType = InterfaceConfig::MultiHopExit;
    exit.m_deviceIpv4Address = "10.1.0.3/24";
    exit.m_deviceIpv6Address = "fd01::3/64";
    exit.m_deviceMTU = 1280;
    FakeDaemon daemon;
    QVERIFY(daemon.activate(entry));
    if (failure == "addresses") {
      daemon.ip.failedAddresses.insert(exit.m_deviceIpv4Address);
    } else if (failure == "mtu") {
      daemon.ip.failedMtus.insert(exit.m_deviceMTU);
    } else {
      daemon.dns.failedResolvers.insert(exit.m_primaryDnsServer);
    }
    QVERIFY(!daemon.activate(exit));
    QVERIFY(daemon.ip.attemptedAddresses.contains(exit.m_deviceIpv4Address));
    if (failure != "addresses") {
      QVERIFY(daemon.ip.attemptedAddresses.contains(exit.m_deviceIpv6Address));
    }
    QVERIFY(daemon.wire.interfaceUp);
    QCOMPARE(daemon.ip.addresses,
             QSet<QString>({entry.m_deviceIpv4Address, entry.m_deviceIpv6Address}));
    QCOMPARE(daemon.ip.mtu, entry.m_deviceMTU);
    QCOMPARE(daemon.wire.peers, QSet<QString>({entry.m_serverPublicKey}));
    QCOMPARE(daemon.wire.routes, QSet<IPAddress>({entry.m_allowedIPAddressRanges.first()}));
    QCOMPARE(daemon.wire.exclusions,
             QSet<IPAddress>({IPAddress(entry.m_excludedAddresses.first())}));
    QCOMPARE(readFile(Daemon::routingConfigFilePath()), entry.m_routingConfig.toUtf8());
    QVERIFY(daemon.deactivate(false));
    QVERIFY(daemon.wire.exclusions.isEmpty());
  }

  void additionalHopAddsDistinctSameSubnetAliases() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());
    InterfaceConfig entry =
        makeConfig("entry-key", "9.9.9.9", "10.0.0.0/8",
                   "198.51.100.1/32", R"({"profile":"entry"})");
    entry.m_hopType = InterfaceConfig::MultiHopEntry;
    entry.m_deviceIpv4Address = "10.1.0.2/24";
    entry.m_deviceIpv6Address = "fd01::2/64";
    InterfaceConfig exit =
        makeConfig("exit-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.2/32", R"({"profile":"exit"})");
    exit.m_hopType = InterfaceConfig::MultiHopExit;
    exit.m_deviceIpv4Address = "10.1.0.3/24";
    exit.m_deviceIpv6Address = "fd01::3/64";

    FakeDaemon daemon;
    QVERIFY(daemon.activate(entry));
    QVERIFY(daemon.activate(exit));
    QCOMPARE(daemon.ip.addresses,
             QSet<QString>({entry.m_deviceIpv4Address,
                            entry.m_deviceIpv6Address,
                            exit.m_deviceIpv4Address,
                            exit.m_deviceIpv6Address}));
    QVERIFY(daemon.deactivate(false));
  }

  void additionalHopDoesNotSuppressPrefixChange() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("AMNEZIA_ROUTING_CONFIG_TEST_DIR", dir.path().toUtf8());
    InterfaceConfig entry =
        makeConfig("entry-key", "9.9.9.9", "10.0.0.0/8",
                   "198.51.100.1/32", R"({"profile":"entry"})");
    entry.m_hopType = InterfaceConfig::MultiHopEntry;
    entry.m_deviceIpv4Address = "10.1.0.2/32";
    InterfaceConfig exit =
        makeConfig("exit-key", "1.1.1.1", "203.0.113.0/24",
                   "198.51.100.2/32", R"({"profile":"exit"})");
    exit.m_hopType = InterfaceConfig::MultiHopExit;
    exit.m_deviceIpv4Address = "10.1.0.2/24";

    FakeDaemon daemon;
    QVERIFY(daemon.activate(entry));
    daemon.ip.failedAddresses.insert(exit.m_deviceIpv4Address);
    QVERIFY(!daemon.activate(exit));
    QVERIFY(daemon.ip.attemptedAddresses.contains(exit.m_deviceIpv4Address));
    QVERIFY(daemon.ip.addresses.contains(entry.m_deviceIpv4Address));
    QVERIFY(!daemon.ip.addresses.contains(exit.m_deviceIpv4Address));
    QVERIFY(daemon.deactivate(false));
  }

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
