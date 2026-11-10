/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "iputilslinux.h"

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <QHostAddress>
#include <QScopeGuard>

#include "daemon/wireguardutils.h"
#include "leakdetector.h"
#include "logger.h"

namespace {
Logger logger("IPUtilsLinux");

constexpr auto NetlinkTimeout = std::chrono::seconds(2);
std::atomic_uint32_t netlinkSequence{1};

struct AddressRequest {
  nlmsghdr header = {};
  ifaddrmsg address = {};
  char attributes[2 * RTA_SPACE(sizeof(in6_addr))] = {};
};

bool addAttribute(AddressRequest& request, int type, const void* data,
                  size_t length) {
  const size_t attributeLength = RTA_LENGTH(length);
  const size_t alignedMessageLength = NLMSG_ALIGN(request.header.nlmsg_len);
  if (alignedMessageLength + RTA_ALIGN(attributeLength) > sizeof(request)) {
    return false;
  }

  auto* attribute = reinterpret_cast<rtattr*>(
      reinterpret_cast<char*>(&request) + alignedMessageLength);
  attribute->rta_type = type;
  attribute->rta_len = attributeLength;
  std::memcpy(RTA_DATA(attribute), data, length);
  request.header.nlmsg_len = alignedMessageLength + RTA_ALIGN(attributeLength);
  return true;
}

int openNetlinkSocket() {
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) {
    return -1;
  }

  sockaddr_nl local = {};
  local.nl_family = AF_NETLINK;
  if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool sendNetlinkRequest(int fd, const nlmsghdr* header) {
  sockaddr_nl kernel = {};
  kernel.nl_family = AF_NETLINK;
  const ssize_t sent =
      sendto(fd, header, header->nlmsg_len, 0,
             reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel));
  return sent == static_cast<ssize_t>(header->nlmsg_len);
}

int waitForNetlinkData(
    int fd, const std::chrono::steady_clock::time_point& deadline) {
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      return ETIMEDOUT;
    }

    pollfd descriptor = {fd, POLLIN, 0};
    const int result =
        poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result == 0) {
      return ETIMEDOUT;
    }
    if (result < 0) {
      return errno;
    }
    return (descriptor.revents & POLLIN) != 0 ? 0 : EIO;
  }
}

int waitForNetlinkAck(int fd, uint32_t sequence) {
  const auto deadline = std::chrono::steady_clock::now() + NetlinkTimeout;
  char buffer[8192];

  while (true) {
    const int waitResult = waitForNetlinkData(fd, deadline);
    if (waitResult != 0) {
      return waitResult;
    }

    const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno;
    }

    int remainingBytes = static_cast<int>(received);
    for (auto* message = reinterpret_cast<nlmsghdr*>(buffer);
         NLMSG_OK(message, remainingBytes);
         message = NLMSG_NEXT(message, remainingBytes)) {
      if (message->nlmsg_seq != sequence) {
        continue;
      }
      if (message->nlmsg_type != NLMSG_ERROR) {
        continue;
      }
      if (NLMSG_PAYLOAD(message, 0) < sizeof(nlmsgerr)) {
        return EPROTO;
      }

      const auto* error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(message));
      return error->error == 0 ? 0 : -error->error;
    }
  }
}

enum class AddressPresence { Present, Absent, Error };

AddressPresence exactAddressPresence(unsigned int ifindex, int family,
                                     const void* rawAddress,
                                     size_t addressLength, int prefixLength,
                                     unsigned char scope) {
  const int fd = openNetlinkSocket();
  if (fd < 0) {
    return AddressPresence::Error;
  }
  auto guard = qScopeGuard([&] { close(fd); });

  struct {
    nlmsghdr header = {};
    ifaddrmsg address = {};
  } request;
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.address));
  request.header.nlmsg_type = RTM_GETADDR;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request.header.nlmsg_seq = netlinkSequence.fetch_add(1);
  request.address.ifa_family = family;

  if (!sendNetlinkRequest(fd, &request.header)) {
    return AddressPresence::Error;
  }

  const auto deadline = std::chrono::steady_clock::now() + NetlinkTimeout;
  char buffer[8192];
  while (true) {
    if (waitForNetlinkData(fd, deadline) != 0) {
      return AddressPresence::Error;
    }

    const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      return AddressPresence::Error;
    }

    int remainingBytes = static_cast<int>(received);
    for (auto* message = reinterpret_cast<nlmsghdr*>(buffer);
         NLMSG_OK(message, remainingBytes);
         message = NLMSG_NEXT(message, remainingBytes)) {
      if (message->nlmsg_seq != request.header.nlmsg_seq) {
        continue;
      }
      if (message->nlmsg_type == NLMSG_DONE) {
        return AddressPresence::Absent;
      }
      if (message->nlmsg_type == NLMSG_ERROR) {
        return AddressPresence::Error;
      }
      if (message->nlmsg_type != RTM_NEWADDR) {
        continue;
      }

      const auto* address =
          reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(message));
      if (address->ifa_index != ifindex || address->ifa_family != family ||
          address->ifa_prefixlen != prefixLength ||
          address->ifa_scope != scope) {
        continue;
      }

      bool addressMatches = false;
      bool localMatches = family == AF_INET6;
      int attributesLength = IFA_PAYLOAD(message);
      for (auto* attribute = IFA_RTA(address);
           RTA_OK(attribute, attributesLength);
           attribute = RTA_NEXT(attribute, attributesLength)) {
        if (RTA_PAYLOAD(attribute) < addressLength) {
          continue;
        }
        if (attribute->rta_type == IFA_ADDRESS) {
          addressMatches =
              std::memcmp(RTA_DATA(attribute), rawAddress, addressLength) == 0;
        } else if (attribute->rta_type == IFA_LOCAL) {
          localMatches =
              std::memcmp(RTA_DATA(attribute), rawAddress, addressLength) == 0;
        }
      }
      if (addressMatches && localMatches) {
        return AddressPresence::Present;
      }
    }
  }
}

bool changeInterfaceAddress(const QString& interfaceName, const QString& subnet,
                            int family, int operation,
                            QSet<QString>& ownedAddresses) {
  const int separator = subnet.indexOf('/');
  const QString addressLiteral =
      (separator < 0 ? subnet : subnet.left(separator)).trimmed();
  const QHostAddress configuredAddress(addressLiteral);
  const int maximumPrefixLength = family == AF_INET ? 32 : 128;
  const auto expectedProtocol = family == AF_INET
                                    ? QAbstractSocket::IPv4Protocol
                                    : QAbstractSocket::IPv6Protocol;
  int prefixLength = maximumPrefixLength;
  if (separator >= 0) {
    const auto parsed = QHostAddress::parseSubnet(subnet);
    if (parsed.first.protocol() != expectedProtocol || parsed.second < 0 ||
        parsed.second > maximumPrefixLength) {
      logger.error() << "Invalid interface subnet";
      return false;
    }
    prefixLength = parsed.second;
  }
  if (configuredAddress.protocol() != expectedProtocol) {
    logger.error() << "Invalid interface subnet";
    return false;
  }

  in6_addr rawAddress = {};
  const QByteArray addressText = configuredAddress.toString().toLatin1();
  const size_t addressLength =
      family == AF_INET ? sizeof(in_addr) : sizeof(in6_addr);
  if (inet_pton(family, addressText.constData(), &rawAddress) != 1) {
    logger.error() << "Failed to parse interface address";
    return false;
  }

  const QByteArray encodedInterfaceName = interfaceName.toLocal8Bit();
  if (encodedInterfaceName.isEmpty() || encodedInterfaceName.size() >= IFNAMSIZ) {
    logger.error() << "Invalid interface name";
    return false;
  }
  const unsigned int ifindex = if_nametoindex(encodedInterfaceName.constData());
  if (ifindex == 0) {
    logger.error() << "Failed to get interface index";
    return false;
  }
  const QString addressKey =
      QStringLiteral("%1|%2|%3/%4")
          .arg(ifindex)
          .arg(family)
          .arg(configuredAddress.toString())
          .arg(prefixLength);

  if (operation == RTM_DELADDR && !ownedAddresses.contains(addressKey)) {
    return true;
  }

  AddressRequest request;
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.address));
  request.header.nlmsg_type = operation;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  if (operation == RTM_NEWADDR) {
    request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
  }
  request.header.nlmsg_seq = netlinkSequence.fetch_add(1);
  request.address.ifa_family = family;
  request.address.ifa_prefixlen = prefixLength;
  request.address.ifa_scope = configuredAddress.isLoopback()
                                  ? RT_SCOPE_HOST
                                  : configuredAddress.isLinkLocal()
                                        ? RT_SCOPE_LINK
                                        : RT_SCOPE_UNIVERSE;
  request.address.ifa_index = ifindex;

  const void* addressData = &rawAddress;
  if (!addAttribute(request, IFA_ADDRESS, addressData, addressLength) ||
      (family == AF_INET &&
       !addAttribute(request, IFA_LOCAL, addressData, addressLength))) {
    logger.error() << "Failed to build interface address request";
    return false;
  }

  const int fd = openNetlinkSocket();
  if (fd < 0) {
    logger.error() << "Failed to create route netlink socket";
    return false;
  }
  auto guard = qScopeGuard([&] { close(fd); });
  if (!sendNetlinkRequest(fd, &request.header)) {
    logger.error() << "Failed to send interface address request";
    return false;
  }

  const int result = waitForNetlinkAck(fd, request.header.nlmsg_seq);
  if (result == 0) {
    if (operation == RTM_NEWADDR) {
      ownedAddresses.insert(addressKey);
    } else {
      ownedAddresses.remove(addressKey);
    }
    return true;
  }
  if (operation == RTM_DELADDR &&
      (result == ENOENT || result == ESRCH || result == EADDRNOTAVAIL)) {
    ownedAddresses.remove(addressKey);
    return true;
  }
  if (operation == RTM_NEWADDR && result == EEXIST) {
    const AddressPresence presence = exactAddressPresence(
        ifindex, family, addressData, addressLength, prefixLength,
        request.address.ifa_scope);
    if (ownedAddresses.contains(addressKey) &&
        presence == AddressPresence::Present) {
      return true;
    }
    if (presence == AddressPresence::Absent) {
      ownedAddresses.remove(addressKey);
    }
  }

  logger.error() << "Failed to change interface address, error:" << result;
  return false;
}
}

IPUtilsLinux::IPUtilsLinux(QObject* parent, const QString& interfaceName)
    : IPUtils(parent), m_interfaceName(interfaceName) {
  MZ_COUNT_CTOR(IPUtilsLinux);
  logger.debug() << "IPUtilsLinux created.";
}

IPUtilsLinux::~IPUtilsLinux() {
  MZ_COUNT_DTOR(IPUtilsLinux);
  logger.debug() << "IPUtilsLinux destroyed.";
}

bool IPUtilsLinux::addInterfaceIPs(const InterfaceConfig& config) {
  return (config.m_deviceIpv4Address.isEmpty() || addIP4AddressToDevice(config)) &&
         (config.m_deviceIpv6Address.isEmpty() || addIP6AddressToDevice(config));
}

bool IPUtilsLinux::removeInterfaceIPs(const InterfaceConfig& config) {
  bool removed = true;
  if (!config.m_deviceIpv4Address.isEmpty()) {
    removed = changeInterfaceAddress(m_interfaceName,
                                     config.m_deviceIpv4Address, AF_INET,
                                     RTM_DELADDR, m_ownedAddresses) &&
              removed;
  }
  if (!config.m_deviceIpv6Address.isEmpty()) {
    removed = changeInterfaceAddress(m_interfaceName,
                                     config.m_deviceIpv6Address, AF_INET6,
                                     RTM_DELADDR, m_ownedAddresses) &&
              removed;
  }
  return removed;
}

bool IPUtilsLinux::setMTUAndUp(const InterfaceConfig& config) {
  // Create socket file descriptor to perform the ioctl operations on
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sockfd < 0) {
    logger.error() << "Failed to create ioctl socket.";
    return false;
  }
  auto guard = qScopeGuard([&] { close(sockfd); });

  // Setup the interface to interact with
  struct ifreq ifr = {};
  const QByteArray encodedInterfaceName = m_interfaceName.toLocal8Bit();
  if (encodedInterfaceName.isEmpty() || encodedInterfaceName.size() >= IFNAMSIZ) {
    logger.error() << "Invalid interface name";
    return false;
  }
  strncpy(ifr.ifr_name, encodedInterfaceName.constData(), IFNAMSIZ);

  // MTU
  // FIXME: We need to know how many layers deep this particular
  // interface is into a tunnel to work effectively. Otherwise
  // we will run into fragmentation issues.
  ifr.ifr_mtu = config.m_deviceMTU;
  int ret = ioctl(sockfd, SIOCSIFMTU, &ifr);
  if (ret) {
    logger.error() << "Failed to set MTU -- " << config.m_deviceMTU << " -- Return code: " << ret;
    return false;
  }

  // Up
  ret = ioctl(sockfd, SIOCGIFFLAGS, &ifr);
  if (ret) {
    return false;
  }
  ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
  ret = ioctl(sockfd, SIOCSIFFLAGS, &ifr);
  if (ret) {
    logger.error() << "Failed to set device up -- Return code: " << ret;
    return false;
  }

  return true;
}

bool IPUtilsLinux::addIP4AddressToDevice(const InterfaceConfig& config) {
  return changeInterfaceAddress(m_interfaceName, config.m_deviceIpv4Address,
                                AF_INET, RTM_NEWADDR, m_ownedAddresses);
}

bool IPUtilsLinux::addIP6AddressToDevice(const InterfaceConfig& config) {
  return changeInterfaceAddress(m_interfaceName, config.m_deviceIpv6Address,
                                AF_INET6, RTM_NEWADDR, m_ownedAddresses);
}
