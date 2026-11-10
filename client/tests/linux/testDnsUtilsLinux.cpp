#include <net/if.h>

#include <QDBusInterface>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "platforms/linux/daemon/dnsutilslinux.h"

namespace {

class ResolveHelper {
 public:
  ResolveHelper(const QString& executable, const QString& logPath)
      : m_executable(executable), m_logPath(logPath) {}

  ~ResolveHelper() { stop(); }

  bool start() {
    m_process.start(m_executable, {m_logPath});
    if (!m_process.waitForStarted(3000) || !m_process.waitForReadyRead(3000)) {
      return false;
    }
    return m_process.readLine().trimmed() == QByteArrayLiteral("READY");
  }

  void stop() {
    if (m_process.state() == QProcess::NotRunning) {
      return;
    }
    m_process.terminate();
    if (!m_process.waitForFinished(1000)) {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
  }

  bool setFailure(const QString& method, int count, int delayMs = 0) {
    QDBusInterface control(
        QStringLiteral("org.freedesktop.resolve1"),
        QStringLiteral("/org/amnezia/TestResolve1"),
        QStringLiteral("org.amnezia.TestResolve1"),
        QDBusConnection::systemBus());
    const QDBusMessage reply =
        control.call(QStringLiteral("SetFailure"), method, count, delayMs);
    return reply.type() == QDBusMessage::ReplyMessage;
  }

 private:
  QString m_executable;
  QString m_logPath;
  QProcess m_process;
};

QStringList readLog(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  QStringList result;
  for (const QByteArray& line : file.readAll().split('\n')) {
    if (!line.trimmed().isEmpty()) {
      result.append(QString::fromUtf8(line.trimmed()));
    }
  }
  return result;
}

QStringList entriesStartingWith(const QStringList& entries, const QString& prefix) {
  QStringList result;
  for (const QString& entry : entries) {
    if (entry.startsWith(prefix)) {
      result.append(entry);
    }
  }
  return result;
}

}  // namespace

class TestDnsUtilsLinux : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() {
    m_helperPath = qEnvironmentVariable("AMNEZIA_FAKE_RESOLVE1");
    QVERIFY2(!m_helperPath.isEmpty(), "The fake resolve1 helper path is required");
  }

  void unavailableServiceFails() {
    DnsUtilsLinux dns(this, 100);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!dns.updateResolvers(QStringLiteral("lo"),
                                 {QHostAddress(QStringLiteral("1.1.1.1"))}));
    QVERIFY(elapsed.elapsed() < 500);
  }

  void partialUpdateIsCompensated() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("resolve.log"));
    ResolveHelper helper(m_helperPath, logPath);
    QVERIFY(helper.start());
    QVERIFY(helper.setFailure(QStringLiteral("SetLinkDefaultRoute"), 1));

    {
      DnsUtilsLinux dns(this, 1000);
      QVERIFY(!dns.updateResolvers(
          QStringLiteral("lo"),
          {QHostAddress(QStringLiteral("1.1.1.1"))}));
    }

    const QStringList log = readLog(logPath);
    QVERIFY(log.contains(QStringLiteral("SetLinkDNS")));
    QVERIFY(log.contains(QStringLiteral("SetLinkDefaultRoute")));
    QVERIFY(entriesStartingWith(log, QStringLiteral("SetLinkDomains:")).isEmpty());
    QVERIFY(log.contains(QStringLiteral("RevertLink")));
  }

  void failedForeignDomainUpdateRestoresOnlyAttemptedLink() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("resolve.log"));
    ResolveHelper helper(m_helperPath, logPath);
    QVERIFY(helper.start());
    QVERIFY(helper.setFailure(QStringLiteral("SetLinkDomains"), 1));

    {
      DnsUtilsLinux dns(this, 1000);
      QVERIFY(!dns.updateResolvers(
          QStringLiteral("lo"),
          {QHostAddress(QStringLiteral("1.0.0.1"))}));
    }

    const QStringList domainCalls = entriesStartingWith(
        readLog(logPath), QStringLiteral("SetLinkDomains:"));
    QCOMPARE(domainCalls.size(), 2);
    QCOMPARE(domainCalls.at(0),
             QStringLiteral("SetLinkDomains:4242:example.test=1"));
    QCOMPARE(domainCalls.at(1),
             QStringLiteral("SetLinkDomains:4242:.=1,example.test=1"));
  }

  void successfulUpdateAndRestoreUseDeterministicOrder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("resolve.log"));
    ResolveHelper helper(m_helperPath, logPath);
    QVERIFY(helper.start());

    DnsUtilsLinux dns(this, 1000);
    QVERIFY(dns.updateResolvers(
        QStringLiteral("lo"),
        {QHostAddress(QStringLiteral("1.1.1.1"))}));
    QVERIFY(dns.restoreResolvers());

    const QStringList log = readLog(logPath);
    const int vpnIfindex = static_cast<int>(if_nametoindex("lo"));
    QVERIFY(vpnIfindex > 0);
    const QStringList expected{
        QStringLiteral("GetDomains"),
        QStringLiteral("SetLinkDNS"),
        QStringLiteral("SetLinkDefaultRoute"),
        QStringLiteral("SetLinkDomains:4242:example.test=1"),
        QStringLiteral("SetLinkDomains:%1:.=1").arg(vpnIfindex),
        QStringLiteral("SetLinkDomains:4242:.=1,example.test=1"),
        QStringLiteral("RevertLink")};
    QCOMPARE(log, expected);
  }

  void failedRestoreRetainsOwnership() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("resolve.log"));
    ResolveHelper helper(m_helperPath, logPath);
    QVERIFY(helper.start());

    DnsUtilsLinux dns(this, 1000);
    QVERIFY(dns.updateResolvers(QStringLiteral("lo"),
                                {QHostAddress(QStringLiteral("9.9.9.9"))}));
    QVERIFY(helper.setFailure(QStringLiteral("RevertLink"), 1));
    QVERIFY(!dns.restoreResolvers());
    QVERIFY(dns.restoreResolvers());

    const QStringList log = readLog(logPath);
    QCOMPARE(log.count(QStringLiteral("RevertLink")), 2);
  }

  void operationUsesOneBoundedDeadline() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("resolve.log"));
    ResolveHelper helper(m_helperPath, logPath);
    QVERIFY(helper.start());
    QVERIFY(helper.setFailure(QStringLiteral("SetLinkDNS"), 1, 1000));

    QElapsedTimer elapsed;
    elapsed.start();
    {
      DnsUtilsLinux dns(this, 100);
      QVERIFY(!dns.updateResolvers(
          QStringLiteral("lo"),
          {QHostAddress(QStringLiteral("8.8.8.8"))}));
    }
    QVERIFY(elapsed.elapsed() < 700);
  }

 private:
  QString m_helperPath;
};

QTEST_GUILESS_MAIN(TestDnsUtilsLinux)
#include "testDnsUtilsLinux.moc"
