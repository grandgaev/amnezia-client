/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef INTERFACECONFIG_H
#define INTERFACECONFIG_H

#include <QList>
#include <QMap>
#include <QString>
#include <QMap>
#include "ipaddress.h"

class QJsonObject;

class InterfaceConfig {
  Q_GADGET

 public:
  InterfaceConfig() {}

  enum HopType { SingleHop, MultiHopEntry, MultiHopExit };
  Q_ENUM(HopType)

  HopType m_hopType;
  QString m_privateKey;
  QString m_deviceIpv4Address;
  QString m_deviceIpv6Address;
  QString m_serverIpv4Gateway;
  QString m_serverIpv6Gateway;
  QString m_serverPublicKey;
  QString m_serverIpv4AddrIn;
  QString m_serverPskKey;
  QString m_serverIpv6AddrIn;
  QString m_primaryDnsServer;
  QString m_secondaryDnsServer;
  int m_serverPort = 0;
  int m_deviceMTU = 1420;
  QString m_persistentKeepalive;
  QList<IPAddress> m_allowedIPAddressRanges;
  QStringList m_excludedAddresses;
  QStringList m_vpnDisabledApps;
  QStringList m_allowedDnsServers;
  bool m_killSwitchEnabled;
#if defined(MZ_ANDROID) || defined(MZ_IOS)
  QString m_installationId;
#endif

  QString m_junkPacketCount;
  QString m_junkPacketMinSize;
  QString m_junkPacketMaxSize;
  QString m_initPacketJunkSize;
  QString m_responsePacketJunkSize;
  QString m_cookieReplyPacketJunkSize;
  QString m_transportPacketJunkSize;
  QString m_initPacketMagicHeader;
  QString m_responsePacketMagicHeader;
  QString m_underloadPacketMagicHeader;
  QString m_transportPacketMagicHeader;
  QMap<QString, QString> m_specialJunk;

  QString m_headerProtectionKey;
  QString m_contentPaddingAddition;
  QString m_rekeyAfterTime;
  QString m_rekeyTimeout;
  QString m_rejectAfterTime;
  QString m_keepaliveTimeout;
  QString m_maxHandshakeAttempts;
  QString m_randomTrailers;
  QString m_disableCookies;

  // Routing profiles: configuration of the packet router built into
  // amneziawg-go, as compact JSON. Empty when the router is not used.
  QString m_routingConfig;

  bool hasRoutingConfig() const { return !m_routingConfig.isEmpty(); }

  // Sockets the router opens for "direct" traffic are bound to the physical
  // interface described here, so that they do not loop back into the tunnel.
  // Zero/empty values mean "not set".
  struct RoutingBypass {
    QString ifname;        // Linux: SO_BINDTODEVICE
    quint32 fwmark = 0;    // Linux: SO_MARK
    quint32 ifindex4 = 0;  // macOS: IP_BOUND_IF, Windows: IP_UNICAST_IF
    quint32 ifindex6 = 0;  // macOS: IPV6_BOUND_IF, Windows: IPV6_UNICAST_IF

    bool operator==(const RoutingBypass& other) const {
      return ifname == other.ifname && fwmark == other.fwmark &&
             ifindex4 == other.ifindex4 && ifindex6 == other.ifindex6;
    }
    bool operator!=(const RoutingBypass& other) const {
      return !operator==(other);
    }
  };

  // UAPI "set" operations for the router. They only contain device-level
  // keys and are sent as operations of their own: in a "set" operation every
  // key following the first public_key= line belongs to that peer.
  //
  // Loads the router configuration from configFile. Returns an empty string
  // if the path cannot be expressed in UAPI.
  static QString routingUapiSet(const QString& configFile,
                                const RoutingBypass& bypass);
  // Updates the bypass interface of an active router.
  static QString routingBypassUapiSet(const RoutingBypass& bypass);
  // Disables the router.
  static QString routingDisableUapiSet();

  QJsonObject toJson() const;
  QString toWgConf(
      const QMap<QString, QString>& extra = QMap<QString, QString>()) const;

  // Converts awg-quick on/off (and 0/1/true/false) to UAPI 1/0.
  // amneziawg-go uses strconv.ParseBool and rejects "on"/"off".
  static QString awgBoolToUapi(const QString& value);

 private:
  static QString routingBypassUapiLines(const RoutingBypass& bypass);
};

#endif  // INTERFACECONFIG_H
