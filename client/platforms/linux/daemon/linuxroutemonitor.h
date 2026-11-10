/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LINUXROUTEMONITOR_H
#define LINUXROUTEMONITOR_H

#include <QByteArray>
#include <QHostAddress>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSocketNotifier>

#include "ipaddress.h"


class LinuxRouteMonitor final : public QObject {
  Q_OBJECT

 public:
  enum class RouteState { Absent, Owned, Foreign, Unknown };

  LinuxRouteMonitor(const QString& ifname, QObject* parent = nullptr);
  ~LinuxRouteMonitor();

  bool insertRoute(const IPAddress& prefix);
  bool deleteRoute(const IPAddress& prefix);
  RouteState routeState(const IPAddress& prefix) const;

  bool addExclusionRoute(const IPAddress& prefix);
  bool deleteExclusionRoute(const IPAddress& prefix);
  bool flushExclusionRoutes();
 private:
  struct RouteIdentity {
    unsigned int ifindex = 0;
    int type = 0;
    int protocol = 0;
    int scope = 0;
    int priority = 0;
    QHostAddress gateway;
  };
  struct ExclusionRoute {
    int references = 0;
    unsigned int ifindex = 0;
    QHostAddress gateway;
  };

  static QString addrToString(const struct sockaddr* sa);
  static QString addrToString(const QByteArray& data);
  bool rtmSendRoute(int action, int flags, int type,
                    const IPAddress& prefix,
                    const QHostAddress& gateway = QHostAddress(),
                    unsigned int gatewayIfindex = 0);
  RouteState routeState(const IPAddress& prefix,
                        const RouteIdentity& expected) const;
  unsigned int m_ifindex = 0;
  int m_nlsock = -1;
  int m_nlseq = 0;
  QSocketNotifier* m_notifier = nullptr;
  QHash<IPAddress, ExclusionRoute> m_exclusionRoutes;

 private slots:
    void nlsockReady();

};

#endif  // LINUXROUTEMONITOR_H
