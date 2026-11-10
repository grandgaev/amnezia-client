/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "iputilsmacos.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <QHostAddress>
#include <QScopeGuard>

#include "daemon/wireguardutils.h"
#include "leakdetector.h"
#include "logger.h"
#include "macosdaemon.h"

namespace {
Logger logger("IPUtilsMacos");

bool parseInterfaceAddress(const QString& configuredAddress, int family,
                           void* output, int& prefixLength,
                           QString& canonicalAddress) {
  const int separator = configuredAddress.indexOf('/');
  const QString literal =
      (separator < 0 ? configuredAddress
                     : configuredAddress.left(separator)).trimmed();
  const QHostAddress address(literal);
  const auto expectedProtocol = family == AF_INET
                                    ? QAbstractSocket::IPv4Protocol
                                    : QAbstractSocket::IPv6Protocol;
  if (address.protocol() != expectedProtocol) {
    return false;
  }

  const int maximumPrefixLength = family == AF_INET ? 32 : 128;
  prefixLength = maximumPrefixLength;
  if (separator >= 0) {
    const auto subnet = QHostAddress::parseSubnet(configuredAddress);
    if (subnet.first.protocol() != expectedProtocol || subnet.second < 0 ||
        subnet.second > maximumPrefixLength) {
      return false;
    }
    prefixLength = subnet.second;
  }

  canonicalAddress = address.toString();
  const QByteArray text = address.toString().toLatin1();
  return inet_pton(family, text.constData(), output) == 1;
}

QString addressOwnershipKey(const QByteArray& interfaceName, int family,
                            const QString& canonicalAddress,
                            int prefixLength) {
  return QStringLiteral("%1|%2|%3/%4")
      .arg(QString::fromLatin1(interfaceName))
      .arg(family)
      .arg(canonicalAddress)
      .arg(prefixLength);
}

void setPrefixMask(in6_addr& mask, int prefixLength) {
  memset(&mask, 0, sizeof(mask));
  const int wholeBytes = prefixLength / 8;
  memset(&mask, 0xff, wholeBytes);
  if (prefixLength % 8 != 0) {
    mask.s6_addr[wholeBytes] = static_cast<unsigned char>(
        0xff << (8 - prefixLength % 8));
  }
}

enum class AddressPresence { Present, Absent, Error };

AddressPresence exactInterfaceAddressPresence(
    const QByteArray& interfaceName, int family, const void* address,
    int prefixLength) {
  struct ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) != 0) {
    return AddressPresence::Error;
  }
  auto guard = qScopeGuard([&] { freeifaddrs(addresses); });

  in_addr ipv4Mask = {};
  in6_addr ipv6Mask = {};
  if (family == AF_INET) {
    ipv4Mask.s_addr = htonl(
        prefixLength == 0 ? 0 : 0xffffffffU << (32 - prefixLength));
  } else {
    setPrefixMask(ipv6Mask, prefixLength);
  }

  for (const struct ifaddrs* current = addresses; current != nullptr;
       current = current->ifa_next) {
    if (!current->ifa_name || !current->ifa_addr || !current->ifa_netmask ||
        interfaceName != current->ifa_name ||
        current->ifa_addr->sa_family != family) {
      continue;
    }
    if (family == AF_INET) {
      const auto* currentAddress =
          reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
      const auto* currentMask =
          reinterpret_cast<const sockaddr_in*>(current->ifa_netmask);
      if (memcmp(&currentAddress->sin_addr, address, sizeof(in_addr)) == 0 &&
          memcmp(&currentMask->sin_addr, &ipv4Mask, sizeof(ipv4Mask)) == 0) {
        return AddressPresence::Present;
      }
    } else {
      const auto* currentAddress =
          reinterpret_cast<const sockaddr_in6*>(current->ifa_addr);
      const auto* currentMask =
          reinterpret_cast<const sockaddr_in6*>(current->ifa_netmask);
      if (memcmp(&currentAddress->sin6_addr, address, sizeof(in6_addr)) == 0 &&
          memcmp(&currentMask->sin6_addr, &ipv6Mask, sizeof(ipv6Mask)) == 0) {
        return AddressPresence::Present;
      }
    }
  }
  return AddressPresence::Absent;
}
}  // namespace

IPUtilsMacos::IPUtilsMacos(QObject* parent) : IPUtils(parent) {
  MZ_COUNT_CTOR(IPUtilsMacos);
  logger.debug() << "IPUtilsMacos created.";
}

IPUtilsMacos::~IPUtilsMacos() {
  MZ_COUNT_DTOR(IPUtilsMacos);
  logger.debug() << "IPUtilsMacos destroyed.";
}

bool IPUtilsMacos::addInterfaceIPs(const InterfaceConfig& config) {
  return (config.m_deviceIpv4Address.isEmpty() || addIP4AddressToDevice(config)) &&
         (config.m_deviceIpv6Address.isEmpty() || addIP6AddressToDevice(config));
}

bool IPUtilsMacos::removeInterfaceIPs(const InterfaceConfig& config) {
  const QByteArray ifname =
      MacOSDaemon::instance()->m_wgutils->interfaceName().toLatin1();
  bool removed = true;
  if (!config.m_deviceIpv4Address.isEmpty()) {
    struct ifreq request = {};
    strncpy(request.ifr_name, ifname.constData(), IFNAMSIZ);
    auto* address = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
    address->sin_family = AF_INET;
    address->sin_len = sizeof(*address);
    int prefixLength = 0;
    QString canonicalAddress;
    if (!parseInterfaceAddress(config.m_deviceIpv4Address, AF_INET,
                               &address->sin_addr, prefixLength,
                               canonicalAddress)) {
      removed = false;
    } else {
      const QString key = addressOwnershipKey(
          ifname, AF_INET, canonicalAddress, prefixLength);
      if (m_ownedAddresses.contains(key)) {
        const AddressPresence presence = exactInterfaceAddressPresence(
            ifname, AF_INET, &address->sin_addr, prefixLength);
        if (presence == AddressPresence::Absent) {
          m_ownedAddresses.remove(key);
        } else if (presence == AddressPresence::Error) {
          removed = false;
        } else {
          const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
          if (fd < 0) {
            removed = false;
          } else {
            const int result = ioctl(fd, SIOCDIFADDR, &request);
            const bool addressRemoved =
                result == 0 || errno == EADDRNOTAVAIL || errno == ENXIO;
            close(fd);
            if (addressRemoved) {
              m_ownedAddresses.remove(key);
            }
            removed = addressRemoved;
          }
        }
      }
    }
  }
  if (!config.m_deviceIpv6Address.isEmpty()) {
    struct in6_ifreq request = {};
    strncpy(request.ifr_name, ifname.constData(), IFNAMSIZ);
    request.ifr_addr.sin6_family = AF_INET6;
    request.ifr_addr.sin6_len = sizeof(request.ifr_addr);
    int prefixLength = 0;
    QString canonicalAddress;
    if (!parseInterfaceAddress(config.m_deviceIpv6Address, AF_INET6,
                               &request.ifr_addr.sin6_addr, prefixLength,
                               canonicalAddress)) {
      removed = false;
    } else {
      const QString key = addressOwnershipKey(
          ifname, AF_INET6, canonicalAddress, prefixLength);
      if (m_ownedAddresses.contains(key)) {
        const AddressPresence presence = exactInterfaceAddressPresence(
            ifname, AF_INET6, &request.ifr_addr.sin6_addr, prefixLength);
        if (presence == AddressPresence::Absent) {
          m_ownedAddresses.remove(key);
        } else if (presence == AddressPresence::Error) {
          removed = false;
        } else {
          const int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IP);
          if (fd < 0) {
            removed = false;
          } else {
            const int result = ioctl(fd, SIOCDIFADDR_IN6, &request);
            const bool addressRemoved =
                result == 0 || errno == EADDRNOTAVAIL || errno == ENXIO;
            close(fd);
            if (addressRemoved) {
              m_ownedAddresses.remove(key);
            }
            removed = addressRemoved && removed;
          }
        }
      }
    }
  }
  return removed;
}

bool IPUtilsMacos::setMTUAndUp(const InterfaceConfig& config) {
  Q_UNUSED(config);
  QString ifname = MacOSDaemon::instance()->m_wgutils->interfaceName();
  struct ifreq ifr = {};

  // Create socket file descriptor to perform the ioctl operations on
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sockfd < 0) {
    logger.error() << "Failed to create ioctl socket.";
    return false;
  }
  auto guard = qScopeGuard([&] { close(sockfd); });

  // MTU
  strncpy(ifr.ifr_name, qPrintable(ifname), IFNAMSIZ);
  ifr.ifr_mtu = config.m_deviceMTU;
  int ret = ioctl(sockfd, SIOCSIFMTU, &ifr);
  if (ret) {
    logger.error() << "Failed to set MTU -- " << config.m_deviceMTU << " -- Return code: " << ret;
    return false;
  }

  // Get the interface flags
  strncpy(ifr.ifr_name, qPrintable(ifname), IFNAMSIZ);
  ret = ioctl(sockfd, SIOCGIFFLAGS, &ifr);
  if (ret) {
    logger.error() << "Failed to get interface flags:" << strerror(errno);
    return false;
  }

  // Up
  ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
  ret = ioctl(sockfd, SIOCSIFFLAGS, &ifr);
  if (ret) {
    logger.error() << "Failed to set device up:" << strerror(errno);
    return false;
  }

  return true;
}

bool IPUtilsMacos::addIP4AddressToDevice(const InterfaceConfig& config) {
  QString ifname = MacOSDaemon::instance()->m_wgutils->interfaceName();
  struct ifaliasreq ifr;
  struct sockaddr_in* ifrAddr = (struct sockaddr_in*)&ifr.ifra_addr;
  struct sockaddr_in* ifrMask = (struct sockaddr_in*)&ifr.ifra_mask;
  struct sockaddr_in* ifrBcast = (struct sockaddr_in*)&ifr.ifra_broadaddr;

  // Name the interface and set family
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifra_name, qPrintable(ifname), IFNAMSIZ);

  ifrAddr->sin_family = AF_INET;
  ifrAddr->sin_len = sizeof(struct sockaddr_in);
  int prefixLength = 0;
  QString canonicalAddress;
  if (!parseInterfaceAddress(config.m_deviceIpv4Address, AF_INET,
                             &ifrAddr->sin_addr, prefixLength,
                             canonicalAddress)) {
    logger.error() << "Invalid IPv4 interface address";
    return false;
  }

  ifrMask->sin_family = AF_INET;
  ifrMask->sin_len = sizeof(struct sockaddr_in);
  ifrMask->sin_addr.s_addr = htonl(
      prefixLength == 0 ? 0 : 0xffffffffU << (32 - prefixLength));

  // Set the broadcast address.
  ifrBcast->sin_family = AF_INET;
  ifrBcast->sin_len = sizeof(struct sockaddr_in);
  ifrBcast->sin_addr.s_addr =
      (ifrAddr->sin_addr.s_addr | ~ifrMask->sin_addr.s_addr);

  // Create an IPv4 socket to perform the ioctl operations on
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sockfd < 0) {
    logger.error() << "Failed to create ioctl socket.";
    return false;
  }
  auto guard = qScopeGuard([&] { close(sockfd); });

  // Set ifr to interface
  int ret = ioctl(sockfd, SIOCAIFADDR, &ifr);
  if (ret) {
    if (errno == EEXIST) {
      m_ownedAddresses.remove(addressOwnershipKey(
          ifname.toLatin1(), AF_INET, canonicalAddress, prefixLength));
    }
    logger.error() << "Failed to set IPv4 interface address"
                   << "error:" << strerror(errno);
    return false;
  }
  m_ownedAddresses.insert(addressOwnershipKey(
      ifname.toLatin1(), AF_INET, canonicalAddress, prefixLength));
  return true;
}

bool IPUtilsMacos::addIP6AddressToDevice(const InterfaceConfig& config) {
  QString ifname = MacOSDaemon::instance()->m_wgutils->interfaceName();
  struct in6_aliasreq ifr6;

  // Name the interface and set family
  memset(&ifr6, 0, sizeof(ifr6));
  strncpy(ifr6.ifra_name, qPrintable(ifname), IFNAMSIZ);
  ifr6.ifra_addr.sin6_family = AF_INET6;
  ifr6.ifra_addr.sin6_len = sizeof(ifr6.ifra_addr);
  ifr6.ifra_lifetime.ia6t_vltime = ifr6.ifra_lifetime.ia6t_pltime = 0xffffffff;
  int prefixLength = 0;
  QString canonicalAddress;
  if (!parseInterfaceAddress(config.m_deviceIpv6Address, AF_INET6,
                             &ifr6.ifra_addr.sin6_addr, prefixLength,
                             canonicalAddress)) {
    logger.error() << "Invalid IPv6 interface address";
    return false;
  }
  ifr6.ifra_prefixmask.sin6_family = AF_INET6;
  ifr6.ifra_prefixmask.sin6_len = sizeof(ifr6.ifra_prefixmask);
  setPrefixMask(ifr6.ifra_prefixmask.sin6_addr, prefixLength);

  // Create IPv4 socket to perform the ioctl operations on
  int sockfd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IP);
  if (sockfd < 0) {
    logger.error() << "Failed to create ioctl socket.";
    return false;
  }
  auto guard = qScopeGuard([&] { close(sockfd); });

  // Set ifr to interface
  int ret = ioctl(sockfd, SIOCAIFADDR_IN6, &ifr6);
  if (ret) {
    if (errno == EEXIST) {
      m_ownedAddresses.remove(addressOwnershipKey(
          ifname.toLatin1(), AF_INET6, canonicalAddress, prefixLength));
    }
    logger.error() << "Failed to set IPv6 interface address"
                   << "error:" << strerror(errno);
    return false;
  }
  m_ownedAddresses.insert(addressOwnershipKey(
      ifname.toLatin1(), AF_INET6, canonicalAddress, prefixLength));
  return true;
}
