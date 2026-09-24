/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WINDOWSROUTEMONITOR_H
#define WINDOWSROUTEMONITOR_H

#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2ipdef.h>

#include <QHash>
#include <QMap>
#include <QObject>

#include "ipaddress.h"

class WindowsRouteMonitor final : public QObject {
  Q_OBJECT

 public:
  WindowsRouteMonitor(quint64 luid, QObject* parent);
  ~WindowsRouteMonitor();

  void setDetaultRouteCapture(bool enable);

  bool addExclusionRoute(const IPAddress& prefix);
  bool deleteExclusionRoute(const IPAddress& prefix);
  void flushExclusionRoutes() { return flushRouteTable(m_exclusionRoutes); };

  quint64 getLuid() const { return m_luid; }

  // Tracks the interfaces of the default IPv4/IPv6 routes outside of the
  // tunnel (routing profiles: the router binds its direct sockets to them).
  void setDefaultInterfaceTracking(bool enable);
  quint32 defaultIfIndexIpv4() const { return m_defaultIfIndexIpv4; }
  quint32 defaultIfIndexIpv6() const { return m_defaultIfIndexIpv6; }

 signals:
  void defaultInterfaceChanged();

 public slots:
  void routeChanged();
  void defaultRouteChanged();

 private:
  bool isRouteExcluded(const IP_ADDRESS_PREFIX* dest) const;
  static bool routeContainsDest(const IP_ADDRESS_PREFIX* route,
                                const IP_ADDRESS_PREFIX* dest);
  static QHostAddress prefixToAddress(const IP_ADDRESS_PREFIX* dest);

  void flushRouteTable(QHash<IPAddress, MIB_IPFORWARD_ROW2*>& table);
  void updateExclusionRoute(MIB_IPFORWARD_ROW2* data, void* table);
  void updateInterfaceMetrics(int family);
  void updateCapturedRoutes(int family);
  void updateCapturedRoutes(int family, void* table);

  QHash<IPAddress, MIB_IPFORWARD_ROW2*> m_exclusionRoutes;
  QMap<quint64, ULONG> m_interfaceMetricsIpv4;
  QMap<quint64, ULONG> m_interfaceMetricsIpv6;

  // Default route cloning
  bool m_defaultRouteCapture = false;
  QHash<IPAddress, MIB_IPFORWARD_ROW2*> m_clonedRoutes;

  const quint64 m_luid = 0;
  HANDLE m_routeHandle = INVALID_HANDLE_VALUE;

  // Default interface tracking
  void updateDefaultInterfaces();
  quint32 findDefaultInterface(int family) const;
  bool m_trackDefaultInterfaces = false;
  HANDLE m_routeHandleIpv6 = INVALID_HANDLE_VALUE;
  quint32 m_defaultIfIndexIpv4 = 0;
  quint32 m_defaultIfIndexIpv6 = 0;
};

#endif /* WINDOWSROUTEMONITOR_H */
