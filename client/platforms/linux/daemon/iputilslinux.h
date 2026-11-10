/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IPUTILSLINUX_H
#define IPUTILSLINUX_H

#include <QSet>

#include "daemon/iputils.h"
#include "daemon/wireguardutils.h"

class IPUtilsLinux final : public IPUtils {
 public:
  explicit IPUtilsLinux(QObject* parent,
                        const QString& interfaceName = WG_INTERFACE);
  ~IPUtilsLinux();
  bool addInterfaceIPs(const InterfaceConfig& config) override;
  bool setMTUAndUp(const InterfaceConfig& config) override;
  bool removeInterfaceIPs(const InterfaceConfig& config) override;

 private:
  bool addIP4AddressToDevice(const InterfaceConfig& config);
  bool addIP6AddressToDevice(const InterfaceConfig& config);

  QString m_interfaceName;
  QSet<QString> m_ownedAddresses;
};

#endif  // IPUTILSLINUX_H
