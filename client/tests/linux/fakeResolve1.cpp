#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include "platforms/linux/daemon/dbustypeslinux.h"

namespace {

class FakeResolveState {
 public:
  explicit FakeResolveState(const QString& logPath) : m_logPath(logPath) {}

  void record(const QString& method) const {
    QFile file(m_logPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      file.write(method.toUtf8());
      file.write("\n");
    }
  }

  void configureFailure(const QString& method, int count, int delayMs) {
    m_failureMethod = method;
    m_failuresRemaining = count;
    m_delayMs = delayMs;
  }

  bool prepare(const QString& method, const QString& logEntry = QString()) {
    record(logEntry.isEmpty() ? method : logEntry);
    if (m_failureMethod != method || m_failuresRemaining <= 0) {
      return true;
    }
    --m_failuresRemaining;
    if (m_delayMs > 0) {
      QThread::msleep(static_cast<unsigned long>(m_delayMs));
    }
    return false;
  }

  DnsDomainList domains() const {
    record(QStringLiteral("GetDomains"));
    DnsDomain root;
    root.ifindex = 4242;
    root.domain = QStringLiteral(".");
    root.search = true;

    DnsDomain search;
    search.ifindex = 4242;
    search.domain = QStringLiteral("example.test");
    search.search = true;
    return {root, search};
  }

 private:
  QString m_logPath;
  QString m_failureMethod;
  int m_failuresRemaining = 0;
  int m_delayMs = 0;
};

class FakeResolveManager final : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.resolve1.Manager")
  Q_PROPERTY(DnsDomainList Domains READ domains)

 public:
  explicit FakeResolveManager(FakeResolveState* state) : m_state(state) {}

  DnsDomainList domains() const { return m_state->domains(); }

 public slots:
  void SetLinkDNS(int, const DnsResolverList&) {
    complete(QStringLiteral("SetLinkDNS"));
  }

  void SetLinkDefaultRoute(int, bool) {
    complete(QStringLiteral("SetLinkDefaultRoute"));
  }

  void SetLinkDomains(int ifindex, const DnsLinkDomainList& domains) {
    QStringList entries;
    for (const DnsLinkDomain& domain : domains) {
      entries.append(QStringLiteral("%1=%2")
                         .arg(domain.domain)
                         .arg(domain.search ? 1 : 0));
    }
    complete(QStringLiteral("SetLinkDomains"),
             QStringLiteral("SetLinkDomains:%1:%2")
                 .arg(ifindex)
                 .arg(entries.join(QLatin1Char(','))));
  }

  void RevertLink(int) { complete(QStringLiteral("RevertLink")); }

 private:
  void complete(const QString& method, const QString& logEntry = QString()) {
    if (!m_state->prepare(method, logEntry)) {
      sendErrorReply(QDBusError::Failed, QStringLiteral("Injected failure"));
    }
  }

  FakeResolveState* m_state = nullptr;
};

class FakeResolveControl final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.amnezia.TestResolve1")

 public:
  explicit FakeResolveControl(FakeResolveState* state) : m_state(state) {}

 public slots:
  void SetFailure(const QString& method, int count, int delayMs) {
    m_state->configureFailure(method, count, delayMs);
  }

 private:
  FakeResolveState* m_state = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() != 2) {
    return 2;
  }

  DnsMetatypeRegistrationProxy registration;
  FakeResolveState state(app.arguments().at(1));
  FakeResolveManager manager(&state);
  FakeResolveControl control(&state);
  QDBusConnection bus = QDBusConnection::systemBus();
  if (!bus.isConnected() ||
      !bus.registerService(QStringLiteral("org.freedesktop.resolve1")) ||
      !bus.registerObject(QStringLiteral("/org/freedesktop/resolve1"), &manager,
                          QDBusConnection::ExportAllSlots |
                              QDBusConnection::ExportAllProperties) ||
      !bus.registerObject(QStringLiteral("/org/amnezia/TestResolve1"), &control,
                          QDBusConnection::ExportAllSlots)) {
    return 3;
  }

  QTextStream(stdout) << "READY" << Qt::endl;
  return app.exec();
}

#include "fakeResolve1.moc"
