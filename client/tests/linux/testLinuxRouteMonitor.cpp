#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTest>

#include <vector>

#include "platforms/linux/daemon/iputilslinux.h"
#include "platforms/linux/daemon/linuxroutemonitor.h"

namespace {

constexpr auto ProcessTimeoutMs = 2000;
constexpr auto NetlinkCallLimitMs = 3000;

struct CommandResult {
  bool finished = false;
  int exitCode = -1;
  QByteArray output;
};

CommandResult runIp(const QStringList& arguments) {
  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(QStandardPaths::findExecutable(QStringLiteral("ip")), arguments);
  if (!process.waitForStarted(ProcessTimeoutMs) ||
      !process.waitForFinished(ProcessTimeoutMs)) {
    process.kill();
    process.waitForFinished(ProcessTimeoutMs);
    return {};
  }
  return {true, process.exitCode(), process.readAll()};
}

bool commandSucceeded(const QStringList& arguments) {
  const CommandResult result = runIp(arguments);
  return result.finished && result.exitCode == 0;
}

CommandResult route(const QString& prefix) {
  return runIp({QStringLiteral("-4"), QStringLiteral("route"),
                QStringLiteral("show"), QStringLiteral("exact"), prefix});
}

CommandResult addresses() {
  return runIp({QStringLiteral("-4"), QStringLiteral("-o"),
                QStringLiteral("address"), QStringLiteral("show"),
                QStringLiteral("dev"), QStringLiteral("lo")});
}

template <typename Operation>
bool completesWithinDeadline(Operation operation) {
  QElapsedTimer elapsed;
  elapsed.start();
  const bool result = operation();
  return result && elapsed.elapsed() < NetlinkCallLimitMs;
}

}  // namespace

class TestLinuxRouteMonitor : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() {
    const QByteArray nonce = qgetenv("AMNEZIA_NETLINK_E2E_NONCE");
    QVERIFY2(QFileInfo::exists(QStringLiteral("/.dockerenv")),
             "This test may run only inside its disposable Docker container");
    QVERIFY2(geteuid() == 0, "The isolated test requires NET_ADMIN as root");
    QVERIFY2(nonce.size() >= 16 && nonce.size() <= 128,
             "A bounded container ownership nonce is required");

    const QFileInfo markerInfo(QStringLiteral("/run/amnezia-netlink-e2e-owner"));
    QVERIFY2(markerInfo.isFile() && !markerInfo.isSymLink() &&
                 markerInfo.ownerId() == 0,
             "The container ownership marker must be a root-owned file");
    QCOMPARE(markerInfo.permissions(),
             QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile marker(markerInfo.filePath());
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QCOMPARE(marker.readAll().trimmed(), nonce);

    const QList<QNetworkInterface> interfaces =
        QNetworkInterface::allInterfaces();
    QCOMPARE(interfaces.size(), 1);
    QCOMPARE(interfaces.first().name(), QStringLiteral("lo"));
    QVERIFY2(!QStandardPaths::findExecutable(QStringLiteral("ip")).isEmpty(),
             "The iproute2 command is required for independent state checks");
  }

  void cleanup() {
    runIp({QStringLiteral("-4"), QStringLiteral("route"),
           QStringLiteral("del"), QStringLiteral("198.51.100.0/24"),
           QStringLiteral("dev"), QStringLiteral("lo")});
    runIp({QStringLiteral("-4"), QStringLiteral("route"),
           QStringLiteral("del"), QStringLiteral("203.0.113.0/24"),
           QStringLiteral("dev"), QStringLiteral("lo")});
    runIp({QStringLiteral("-4"), QStringLiteral("address"),
           QStringLiteral("del"), QStringLiteral("192.0.2.10/24"),
           QStringLiteral("dev"), QStringLiteral("lo")});
    runIp({QStringLiteral("-4"), QStringLiteral("address"),
           QStringLiteral("del"), QStringLiteral("192.0.2.11/24"),
           QStringLiteral("dev"), QStringLiteral("lo")});
  }

  void insertWaitsForSuccessfulAcknowledgement() {
    LinuxRouteMonitor monitor(QStringLiteral("lo"), this);
    const IPAddress prefix(QStringLiteral("198.51.100.0/24"));

    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Absent);
    QVERIFY(completesWithinDeadline([&] { return monitor.insertRoute(prefix); }));
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Owned);
    const CommandResult installed = route(prefix.toString());
    QVERIFY(installed.finished && installed.exitCode == 0);
    QVERIFY(installed.output.contains("198.51.100.0/24"));
    QVERIFY(installed.output.contains("dev lo"));
    QVERIFY(completesWithinDeadline([&] { return monitor.deleteRoute(prefix); }));
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Absent);
    QVERIFY(completesWithinDeadline([&] { return monitor.insertRoute(prefix); }));
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Owned);
    QVERIFY(commandSucceeded(
        {QStringLiteral("-4"), QStringLiteral("route"), QStringLiteral("del"),
         prefix.toString(), QStringLiteral("dev"), QStringLiteral("lo"),
         QStringLiteral("metric"), QStringLiteral("1"),
         QStringLiteral("proto"), QStringLiteral("boot")}));
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Absent);
    const CommandResult removed = route(prefix.toString());
    QVERIFY(removed.finished && removed.exitCode == 0);
    QVERIFY(removed.output.isEmpty());

    QVERIFY(commandSucceeded(
        {QStringLiteral("-4"), QStringLiteral("route"), QStringLiteral("add"),
         prefix.toString(), QStringLiteral("dev"), QStringLiteral("lo"),
         QStringLiteral("metric"), QStringLiteral("1"),
         QStringLiteral("proto"), QStringLiteral("static")}));
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Foreign);
    const CommandResult replacement = route(prefix.toString());
    QVERIFY(replacement.finished && replacement.exitCode == 0);
    QVERIFY(replacement.output.contains("proto static"));
    QVERIFY(completesWithinDeadline([&] { return monitor.deleteRoute(prefix); }));
    const CommandResult preserved = route(prefix.toString());
    QVERIFY(preserved.finished && preserved.exitCode == 0);
    QCOMPARE(preserved.output, replacement.output);
  }

  void collisionFailsAndPreservesForeignRoute() {
    const QString prefix = QStringLiteral("203.0.113.0/24");
    QVERIFY(commandSucceeded(
        {QStringLiteral("-4"), QStringLiteral("route"), QStringLiteral("add"),
         prefix, QStringLiteral("dev"), QStringLiteral("lo"),
         QStringLiteral("metric"), QStringLiteral("1"),
         QStringLiteral("proto"), QStringLiteral("static")}));
    const CommandResult before = route(prefix);
    QVERIFY(before.finished && before.exitCode == 0);
    QVERIFY(before.output.contains("metric 1"));
    QVERIFY(before.output.contains("proto static"));

    LinuxRouteMonitor monitor(QStringLiteral("lo"), this);
    QCOMPARE(monitor.routeState(IPAddress(prefix)),
             LinuxRouteMonitor::RouteState::Foreign);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!monitor.insertRoute(IPAddress(prefix)));
    QVERIFY(elapsed.elapsed() < NetlinkCallLimitMs);
    const CommandResult after = route(prefix);
    QVERIFY(after.finished && after.exitCode == 0);
    QCOMPARE(after.output, before.output);
    QVERIFY(completesWithinDeadline(
        [&] { return monitor.deleteRoute(IPAddress(prefix)); }));
    const CommandResult afterDelete = route(prefix);
    QVERIFY(afterDelete.finished && afterDelete.exitCode == 0);
    QCOMPARE(afterDelete.output, before.output);
    QCOMPARE(monitor.routeState(IPAddress(prefix)),
             LinuxRouteMonitor::RouteState::Foreign);
  }

  void deletingAbsentRouteIsIdempotent() {
    LinuxRouteMonitor monitor(QStringLiteral("lo"), this);
    const IPAddress prefix(QStringLiteral("198.51.100.128/25"));
    const CommandResult before = route(prefix.toString());
    QVERIFY(before.finished && before.exitCode == 0);
    QVERIFY(before.output.isEmpty());
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Absent);
    QVERIFY(completesWithinDeadline([&] { return monitor.deleteRoute(prefix); }));
    const CommandResult after = route(prefix.toString());
    QVERIFY(after.finished && after.exitCode == 0);
    QVERIFY(after.output.isEmpty());
  }

  void routeStateReportsUnknownWhenLookupCannotStart() {
    LinuxRouteMonitor monitor(QStringLiteral("lo"), this);
    const IPAddress prefix(QStringLiteral("198.51.100.192/26"));
    struct rlimit previous = {};
    QVERIFY(getrlimit(RLIMIT_NOFILE, &previous) == 0);
    struct rlimit limited = previous;
    limited.rlim_cur = qMin<rlim_t>(64, previous.rlim_cur);
    QVERIFY(setrlimit(RLIMIT_NOFILE, &limited) == 0);

    std::vector<int> descriptors;
    auto restoreLimit = qScopeGuard([&] {
      for (int descriptor : descriptors) {
        close(descriptor);
      }
      setrlimit(RLIMIT_NOFILE, &previous);
    });
    while (true) {
      const int descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (descriptor < 0) {
        break;
      }
      descriptors.push_back(descriptor);
    }
    QCOMPARE(errno, EMFILE);
    QCOMPARE(monitor.routeState(prefix), LinuxRouteMonitor::RouteState::Unknown);
  }

  void exactAliasRemovalPreservesExistingAddress() {
    QVERIFY(commandSucceeded(
        {QStringLiteral("-4"), QStringLiteral("address"),
         QStringLiteral("add"), QStringLiteral("192.0.2.10/24"),
         QStringLiteral("dev"), QStringLiteral("lo")}));

    IPUtilsLinux ipUtils(this, QStringLiteral("lo"));

    InterfaceConfig foreign;
    foreign.m_deviceIpv4Address = QStringLiteral("192.0.2.10/24");
    QElapsedTimer foreignAddElapsed;
    foreignAddElapsed.start();
    QVERIFY(!ipUtils.addInterfaceIPs(foreign));
    QVERIFY(foreignAddElapsed.elapsed() < NetlinkCallLimitMs);
    QVERIFY(completesWithinDeadline(
        [&] { return ipUtils.removeInterfaceIPs(foreign); }));
    const CommandResult foreignPreserved = addresses();
    QVERIFY(foreignPreserved.finished && foreignPreserved.exitCode == 0);
    QVERIFY(foreignPreserved.output.contains("192.0.2.10/24"));

    InterfaceConfig added;
    added.m_deviceIpv4Address = QStringLiteral("192.0.2.11/24");
    QVERIFY(completesWithinDeadline([&] { return ipUtils.addInterfaceIPs(added); }));
    QVERIFY(completesWithinDeadline([&] { return ipUtils.addInterfaceIPs(added); }));
    const CommandResult both = addresses();
    QVERIFY(both.finished && both.exitCode == 0);
    QVERIFY(both.output.contains("192.0.2.10/24"));
    QVERIFY(both.output.contains("192.0.2.11/24"));

    QVERIFY(
        completesWithinDeadline([&] { return ipUtils.removeInterfaceIPs(added); }));
    const CommandResult remaining = addresses();
    QVERIFY(remaining.finished && remaining.exitCode == 0);
    QVERIFY(remaining.output.contains("192.0.2.10/24"));
    QVERIFY(!remaining.output.contains("192.0.2.11/24"));
    QVERIFY(
        completesWithinDeadline([&] { return ipUtils.removeInterfaceIPs(added); }));
  }
};

QTEST_GUILESS_MAIN(TestLinuxRouteMonitor)
#include "testLinuxRouteMonitor.moc"
