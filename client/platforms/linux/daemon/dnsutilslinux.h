/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DNSUTILSLINUX_H
#define DNSUTILSLINUX_H

#include <QElapsedTimer>
#include <QDBusInterface>

#include "daemon/dnsutils.h"
#include "dbustypeslinux.h"

class DnsUtilsLinux final : public DnsUtils {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(DnsUtilsLinux)

 public:
  explicit DnsUtilsLinux(QObject* parent, int timeoutMs = 5000);
  ~DnsUtilsLinux();
  bool updateResolvers(const QString& ifname,
                       const QList<QHostAddress>& resolvers) override;
  bool restoreResolvers() override;

 private:
  bool callResolver(const QString& method, const QList<QVariant>& arguments,
                    const QElapsedTimer& deadline);
  bool readLinkDomains(QMap<int, DnsLinkDomainList>& domains,
                       const QElapsedTimer& deadline);
  bool setLinkDNS(int ifindex, const QList<QHostAddress>& resolvers,
                  const QElapsedTimer& deadline);
  bool setLinkDomains(int ifindex, const DnsLinkDomainList& domains,
                      const QElapsedTimer& deadline);
  bool setLinkDefaultRoute(int ifindex, bool enable,
                           const QElapsedTimer& deadline);
  bool restoreResolvers(const QElapsedTimer& deadline);
  int remainingTime(const QElapsedTimer& deadline) const;

 private:
  int m_ifindex = 0;
  QMap<int, DnsLinkDomainList> m_linkDomains;
  QDBusInterface* m_resolver = nullptr;
  int m_timeoutMs = 0;
};

#endif  // DNSUTILSLINUX_H
