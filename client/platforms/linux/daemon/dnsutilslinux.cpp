/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "dnsutilslinux.h"

#include <net/if.h>

#include <QDBusReply>
#include <QDBusVariant>
#include <QtDBus/QtDBus>

#include "leakdetector.h"
#include "logger.h"

constexpr const char* DBUS_RESOLVE_SERVICE = "org.freedesktop.resolve1";
constexpr const char* DBUS_RESOLVE_PATH = "/org/freedesktop/resolve1";
constexpr const char* DBUS_RESOLVE_MANAGER = "org.freedesktop.resolve1.Manager";
constexpr const char* DBUS_PROPERTY_INTERFACE =
    "org.freedesktop.DBus.Properties";

namespace {
Logger logger("DnsUtilsLinux");
}

DnsUtilsLinux::DnsUtilsLinux(QObject* parent, int timeoutMs)
    : DnsUtils(parent), m_timeoutMs(qMax(1, timeoutMs)) {
  MZ_COUNT_CTOR(DnsUtilsLinux);
  logger.debug() << "DnsUtilsLinux created.";

  QDBusConnection conn = QDBusConnection::systemBus();
  m_resolver = new QDBusInterface(DBUS_RESOLVE_SERVICE, DBUS_RESOLVE_PATH,
                                  DBUS_RESOLVE_MANAGER, conn, this);
}

DnsUtilsLinux::~DnsUtilsLinux() {
  MZ_COUNT_DTOR(DnsUtilsLinux);

  if (!restoreResolvers()) {
    logger.warning() << "Failed to restore DNS resolvers during shutdown";
  }

  logger.debug() << "DnsUtilsLinux destroyed.";
}

bool DnsUtilsLinux::updateResolvers(const QString& ifname,
                                    const QList<QHostAddress>& resolvers) {
  QElapsedTimer deadline;
  deadline.start();

  if (m_ifindex > 0 || !m_linkDomains.isEmpty()) {
    if (!restoreResolvers(deadline)) {
      logger.error() << "Unable to clean up the previous DNS configuration";
      return false;
    }
  }

  const int ifindex = if_nametoindex(qPrintable(ifname));
  if (ifindex <= 0) {
    logger.error() << "Unable to resolve ifindex for" << ifname;
    return false;
  }

  QMap<int, DnsLinkDomainList> allDomains;
  if (!readLinkDomains(allDomains, deadline)) {
    return false;
  }

  const DnsLinkDomain root(QStringLiteral("."), true);
  QMap<int, DnsLinkDomainList> displacedDomains;
  for (auto iterator = allDomains.constBegin(); iterator != allDomains.constEnd();
       ++iterator) {
    if (iterator.key() != ifindex && iterator.value().contains(root)) {
      displacedDomains.insert(iterator.key(), iterator.value());
    }
  }

  m_ifindex = ifindex;
  m_linkDomains.clear();

  bool updated = setLinkDNS(ifindex, resolvers, deadline) &&
                 setLinkDefaultRoute(ifindex, true, deadline);
  for (auto iterator = displacedDomains.constBegin();
       updated && iterator != displacedDomains.constEnd(); ++iterator) {
    DnsLinkDomainList domains = iterator.value();
    domains.removeAll(root);
    m_linkDomains.insert(iterator.key(), iterator.value());
    updated = setLinkDomains(iterator.key(), domains, deadline);
  }
  if (updated) {
    updated = setLinkDomains(ifindex, DnsLinkDomainList{root}, deadline);
  }

  if (updated) {
    return true;
  }

  if (!restoreResolvers(deadline)) {
    logger.error() << "Failed to compensate a partial DNS update";
  }
  return false;
}

bool DnsUtilsLinux::restoreResolvers() {
  QElapsedTimer deadline;
  deadline.start();
  return restoreResolvers(deadline);
}

bool DnsUtilsLinux::restoreResolvers(const QElapsedTimer& deadline) {
  bool restored = true;
  const QList<int> linkIndexes = m_linkDomains.keys();
  for (int linkIndex : linkIndexes) {
    if (!setLinkDomains(linkIndex, m_linkDomains.value(linkIndex), deadline)) {
      restored = false;
    } else {
      m_linkDomains.remove(linkIndex);
    }
  }

  if (m_ifindex > 0) {
    if (!callResolver(QStringLiteral("RevertLink"),
                      {QVariant::fromValue(m_ifindex)}, deadline)) {
      restored = false;
    } else {
      m_ifindex = 0;
    }
  }

  return restored && m_linkDomains.isEmpty() && m_ifindex == 0;
}

int DnsUtilsLinux::remainingTime(const QElapsedTimer& deadline) const {
  return qMax(0, m_timeoutMs - static_cast<int>(deadline.elapsed()));
}

bool DnsUtilsLinux::callResolver(const QString& method,
                                 const QList<QVariant>& arguments,
                                 const QElapsedTimer& deadline) {
  if (!m_resolver->connection().isConnected() || !m_resolver->isValid()) {
    logger.error() << "The system resolver DBus service is unavailable";
    return false;
  }

  const int timeout = remainingTime(deadline);
  if (timeout <= 0) {
    logger.error() << "Timed out while updating DNS through DBus";
    return false;
  }

  m_resolver->setTimeout(timeout);
  const QDBusMessage reply = m_resolver->callWithArgumentList(
      QDBus::Block, method, arguments);
  if (reply.type() != QDBusMessage::ReplyMessage) {
    logger.error() << "Resolver DBus call failed:" << method
                   << reply.errorMessage();
    return false;
  }
  return true;
}

bool DnsUtilsLinux::setLinkDNS(int ifindex,
                               const QList<QHostAddress>& resolvers,
                               const QElapsedTimer& deadline) {
  QList<DnsResolver> resolverList;
  char ifnamebuf[IF_NAMESIZE];
  const char* ifname = if_indextoname(ifindex, ifnamebuf);
  for (const auto& ip : resolvers) {
    resolverList.append(ip);
    if (ifname) {
      logger.debug() << "Adding DNS resolver" << ip.toString() << "via"
                     << ifname;
    }
  }

  QList<QVariant> argumentList;
  argumentList << QVariant::fromValue(ifindex);
  argumentList << QVariant::fromValue(resolverList);
  return callResolver(QStringLiteral("SetLinkDNS"), argumentList, deadline);
}

bool DnsUtilsLinux::setLinkDomains(int ifindex,
                                   const DnsLinkDomainList& domains,
                                   const QElapsedTimer& deadline) {
  char ifnamebuf[IF_NAMESIZE];
  const char* ifname = if_indextoname(ifindex, ifnamebuf);
  if (ifname) {
    for (const auto& d : domains) {
      // The DNS search domains often winds up revealing user's ISP which
      // can correlate back to their location.
      logger.debug() << "Setting DNS domain:" << logger.sensitive(d.domain)
                     << "via" << ifname << (d.search ? "search" : "");
    }
  }

  QList<QVariant> argumentList;
  argumentList << QVariant::fromValue(ifindex);
  argumentList << QVariant::fromValue(domains);
  return callResolver(QStringLiteral("SetLinkDomains"), argumentList, deadline);
}

bool DnsUtilsLinux::setLinkDefaultRoute(int ifindex, bool enable,
                                        const QElapsedTimer& deadline) {
  QList<QVariant> argumentList;
  argumentList << QVariant::fromValue(ifindex);
  argumentList << QVariant::fromValue(enable);
  return callResolver(QStringLiteral("SetLinkDefaultRoute"), argumentList,
                      deadline);
}

bool DnsUtilsLinux::readLinkDomains(QMap<int, DnsLinkDomainList>& domains,
                                    const QElapsedTimer& deadline) {
  QDBusMessage message = QDBusMessage::createMethodCall(
      DBUS_RESOLVE_SERVICE, DBUS_RESOLVE_PATH, DBUS_PROPERTY_INTERFACE, "Get");
  message << QString(DBUS_RESOLVE_MANAGER);
  message << QString("Domains");
  const int timeout = remainingTime(deadline);
  if (timeout <= 0) {
    logger.error() << "Timed out while reading DNS domains through DBus";
    return false;
  }

  const QDBusMessage messageReply =
      m_resolver->connection().call(message, QDBus::Block, timeout);
  QDBusReply<QVariant> reply(messageReply);
  if (!reply.isValid()) {
    logger.error() << "Failed to read DNS domains from the DBus service"
                   << reply.error().message();
    return false;
  }

  QVariant value = reply.value();
  if (value.canConvert<QDBusVariant>()) {
    value = value.value<QDBusVariant>().variant();
  }
  if (!value.canConvert<QDBusArgument>()) {
    logger.error() << "Invalid DNS domains reply from the DBus service";
    return false;
  }

  const QDBusArgument args = value.value<QDBusArgument>();
  const DnsDomainList list = qdbus_cast<DnsDomainList>(args);
  domains.clear();
  for (const auto& d : list) {
    if (d.ifindex == 0) {
      continue;
    }
    domains[d.ifindex].append(DnsLinkDomain(d.domain, d.search));
  }
  return true;
}

static DnsMetatypeRegistrationProxy s_dnsMetatypeProxy;
