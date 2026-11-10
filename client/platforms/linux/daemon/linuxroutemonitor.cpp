/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "linuxroutemonitor.h"

#include <QNetworkInterface>
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>

#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <errno.h>

#include <chrono>

#include "leakdetector.h"
#include "logger.h"
#include "core/utils/networkUtilities.h"

namespace {
Logger logger("LinuxRouteMonitor");
}  // namespace


typedef struct wg_allowedip {
    uint16_t family;
    union {
        struct in_addr ip4;
        struct in6_addr ip6;
    };
    uint8_t cidr;
    struct wg_allowedip *next_allowedip;
} wg_allowedip;

static void nlmsg_append_attr(struct nlmsghdr* nlmsg, size_t maxlen,
                              int attrtype, const void* attrdata,
                              size_t attrlen);
static void nlmsg_append_attr32(struct nlmsghdr* nlmsg, size_t maxlen,
                                int attrtype, uint32_t value);

static bool buildAllowedIp(wg_allowedip* ip, const IPAddress& prefix);


LinuxRouteMonitor::LinuxRouteMonitor(const QString& ifname, QObject* parent)
    : QObject(parent) {
  MZ_COUNT_CTOR(LinuxRouteMonitor);
  logger.debug() << "LinuxRouteMonitor created.";
  m_ifindex = if_nametoindex(qPrintable(ifname));

  m_nlsock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
  if (m_nlsock < 0) {
      logger.warning() << "Failed to create netlink socket:" << strerror(errno);
  }

  struct sockaddr_nl nladdr;
  memset(&nladdr, 0, sizeof(nladdr));
  nladdr.nl_family = AF_NETLINK;
  nladdr.nl_pid = getpid();
  if (bind(m_nlsock, (struct sockaddr*)&nladdr, sizeof(nladdr)) != 0) {
      logger.warning() << "Failed to bind netlink socket:" << strerror(errno);
  }

  m_notifier = new QSocketNotifier(m_nlsock, QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this,
          &LinuxRouteMonitor::nlsockReady);
}

LinuxRouteMonitor::~LinuxRouteMonitor() {
  MZ_COUNT_DTOR(LinuxRouteMonitor);
  if (!flushExclusionRoutes()) {
      logger.warning() << "Failed to flush all owned exclusion routes";
  }
  if (m_nlsock >= 0) {
      close(m_nlsock);
  }
  logger.debug() << "WireguardUtilsLinux destroyed.";
}

// Compare memory against zero.
static int memcmpzero(const void* data, size_t len) {
  const quint8* ptr = static_cast<const quint8*>(data);
  while (len--) {
    if (*ptr++) return 1;
  }
  return 0;
}

bool LinuxRouteMonitor::insertRoute(const IPAddress& prefix) {
    logger.debug() << "Adding route to" << prefix.toString();

    const int flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    return rtmSendRoute(RTM_NEWROUTE, flags, RTN_UNICAST, prefix);
}

bool LinuxRouteMonitor::deleteRoute(const IPAddress& prefix) {
    logger.debug() << "Removing route to" << prefix.toString();

    const RouteState state = routeState(prefix);
    if (state == RouteState::Unknown) {
        return false;
    }
    if (state != RouteState::Owned) {
        return true;
    }
    const int flags = NLM_F_REQUEST | NLM_F_ACK;
    return rtmSendRoute(RTM_DELROUTE, flags, RTN_UNICAST, prefix);
}

LinuxRouteMonitor::RouteState LinuxRouteMonitor::routeState(
    const IPAddress& prefix) const {
    RouteIdentity expected;
    expected.ifindex = m_ifindex;
    expected.type = RTN_UNICAST;
    expected.protocol = RTPROT_BOOT;
    expected.scope = RT_SCOPE_UNIVERSE;
    expected.priority = 1;
    return routeState(prefix, expected);
}

bool LinuxRouteMonitor::addExclusionRoute(const IPAddress& prefix) {
    logger.debug() << "Adding exclusion route for"
                   << prefix.toString();
    auto existing = m_exclusionRoutes.find(prefix);
    if (existing != m_exclusionRoutes.end()) {
        const auto gatewayAndIface = NetworkUtilities::getGatewayAndIface();
        const QHostAddress activeGateway(gatewayAndIface.first);
        const unsigned int activeIfindex = gatewayAndIface.second.index();
        if (activeGateway.isNull() || activeIfindex == 0 ||
            activeGateway.protocol() != prefix.type()) {
            return false;
        }
        RouteIdentity expected;
        expected.ifindex = existing->ifindex;
        expected.type = RTN_UNICAST;
        expected.protocol = RTPROT_BOOT;
        expected.scope = RT_SCOPE_UNIVERSE;
        expected.priority = 0;
        expected.gateway = existing->gateway;
        const RouteState state = routeState(prefix, expected);
        const bool gatewayChanged = existing->ifindex != activeIfindex ||
                                    existing->gateway != activeGateway;
        if (gatewayChanged) {
            if (state == RouteState::Unknown) {
                return false;
            }
            if (state == RouteState::Foreign) {
                m_exclusionRoutes.erase(existing);
                return false;
            }
            const bool oldRouteRemoved = state == RouteState::Owned;
            if (oldRouteRemoved) {
                const int deleteFlags = NLM_F_REQUEST | NLM_F_ACK;
                if (!rtmSendRoute(RTM_DELROUTE, deleteFlags, RTN_THROW,
                                  prefix, existing->gateway,
                                  existing->ifindex)) {
                    return false;
                }
            }
            const int addFlags =
                NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
            if (!rtmSendRoute(RTM_NEWROUTE, addFlags, RTN_THROW, prefix,
                              activeGateway, activeIfindex)) {
                if (oldRouteRemoved &&
                    rtmSendRoute(RTM_NEWROUTE, addFlags, RTN_THROW, prefix,
                                 existing->gateway, existing->ifindex)) {
                    return false;
                }
                m_exclusionRoutes.erase(existing);
                return false;
            }
            existing->gateway = activeGateway;
            existing->ifindex = activeIfindex;
            ++existing->references;
            return true;
        }
        if (state == RouteState::Owned) {
            ++existing->references;
            return true;
        }
        if (state == RouteState::Foreign) {
            m_exclusionRoutes.erase(existing);
            logger.warning() << "Refusing to replace a foreign exclusion route to"
                             << prefix.toString();
            return false;
        }
        if (state == RouteState::Unknown) {
            return false;
        }
        const int flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
        if (!rtmSendRoute(RTM_NEWROUTE, flags, RTN_THROW, prefix,
                          existing->gateway, existing->ifindex)) {
            return false;
        }
        ++existing->references;
        return true;
    }

    const auto gatewayAndIface = NetworkUtilities::getGatewayAndIface();
    const QHostAddress gateway(gatewayAndIface.first);
    const unsigned int ifindex = gatewayAndIface.second.index();
    if (gateway.isNull() || ifindex == 0 ||
        gateway.protocol() != prefix.type()) {
        logger.warning() << "No default gateway available, skipping exclusion route";
        return false;
    }
    const int flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    if (!rtmSendRoute(RTM_NEWROUTE, flags, RTN_THROW, prefix, gateway,
                      ifindex)) {
        return false;
    }
    m_exclusionRoutes.insert(prefix, ExclusionRoute{1, ifindex, gateway});
    return true;
}

bool LinuxRouteMonitor::deleteExclusionRoute(const IPAddress& prefix) {
    logger.debug() << "Removing exclusion route for"
                   << prefix.toString();
    auto existing = m_exclusionRoutes.find(prefix);
    if (existing == m_exclusionRoutes.end()) {
        return true;
    }
    if (existing->references > 1) {
        --existing->references;
        return true;
    }

    RouteIdentity expected;
    expected.ifindex = existing->ifindex;
    expected.type = RTN_UNICAST;
    expected.protocol = RTPROT_BOOT;
    expected.scope = RT_SCOPE_UNIVERSE;
    expected.priority = 0;
    expected.gateway = existing->gateway;
    const RouteState state = routeState(prefix, expected);
    if (state == RouteState::Unknown) {
        return false;
    }
    if (state != RouteState::Owned) {
        m_exclusionRoutes.erase(existing);
        return true;
    }
    const int flags = NLM_F_REQUEST | NLM_F_ACK;
    if (!rtmSendRoute(RTM_DELROUTE, flags, RTN_THROW, prefix,
                      existing->gateway, existing->ifindex)) {
        return false;
    }
    m_exclusionRoutes.erase(existing);
    return true;
}

bool LinuxRouteMonitor::flushExclusionRoutes() {
    bool result = true;
    for (auto it = m_exclusionRoutes.begin(); it != m_exclusionRoutes.end();) {
        RouteIdentity expected;
        expected.ifindex = it->ifindex;
        expected.type = RTN_UNICAST;
        expected.protocol = RTPROT_BOOT;
        expected.scope = RT_SCOPE_UNIVERSE;
        expected.priority = 0;
        expected.gateway = it->gateway;
        const RouteState state = routeState(it.key(), expected);
        if (state == RouteState::Unknown) {
            result = false;
            ++it;
            continue;
        }
        if (state == RouteState::Owned) {
            const int flags = NLM_F_REQUEST | NLM_F_ACK;
            if (!rtmSendRoute(RTM_DELROUTE, flags, RTN_THROW, it.key(),
                              it->gateway, it->ifindex)) {
                result = false;
                ++it;
                continue;
            }
        }
        it = m_exclusionRoutes.erase(it);
    }
    return result;
}

bool LinuxRouteMonitor::rtmSendRoute(int action, int flags, int type,
                                     const IPAddress& prefix,
                                     const QHostAddress& gateway,
                                     unsigned int gatewayIfindex) {
    constexpr size_t rtm_max_size = sizeof(struct rtmsg) +
                                    3 * RTA_SPACE(sizeof(uint32_t)) +
                                    RTA_SPACE(sizeof(struct in6_addr));
    wg_allowedip ip;
    if (!buildAllowedIp(&ip, prefix)) {
    logger.warning() << "Invalid destination prefix";
    return false;
    }

    alignas(struct nlmsghdr) char buf[NLMSG_SPACE(rtm_max_size)];
    struct nlmsghdr* nlmsg = reinterpret_cast<struct nlmsghdr*>(buf);
    struct rtmsg* rtm = static_cast<struct rtmsg*>(NLMSG_DATA(nlmsg));

    memset(buf, 0, sizeof(buf));
    nlmsg->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlmsg->nlmsg_type = action;
    nlmsg->nlmsg_flags = flags;
    const uint32_t requestSequence = static_cast<uint32_t>(m_nlseq++);
    nlmsg->nlmsg_pid = 0;
    nlmsg->nlmsg_seq = requestSequence;
    rtm->rtm_dst_len = ip.cidr;
    rtm->rtm_family = ip.family;
    rtm->rtm_type = type;
    rtm->rtm_table = RT_TABLE_UNSPEC;
    rtm->rtm_protocol = RTPROT_BOOT;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;

    if (rtm->rtm_family == AF_INET6) {
    nlmsg_append_attr(nlmsg, sizeof(buf), RTA_DST, &ip.ip6, sizeof(ip.ip6));
    } else {
    nlmsg_append_attr(nlmsg, sizeof(buf), RTA_DST, &ip.ip4, sizeof(ip.ip4));
    }

    if (rtm->rtm_type == RTN_UNICAST) {
    const int index = static_cast<int>(m_ifindex);

    if (index <= 0) {
        logger.error() << "if_nametoindex() failed:" << strerror(errno);
        return false;
    }
    nlmsg_append_attr32(nlmsg, sizeof(buf), RTA_OIF, index);
    nlmsg_append_attr32(nlmsg, sizeof(buf), RTA_PRIORITY, 1);
    }

    if (rtm->rtm_type == RTN_THROW) {
    if (gateway.isNull() || gatewayIfindex == 0) {
        logger.warning() << "No default gateway available, skipping exclusion route";
        return false;
    }
    if (rtm->rtm_family == AF_INET &&
        gateway.protocol() == QAbstractSocket::IPv4Protocol) {
        struct in_addr ip4 = {};
        ip4.s_addr = htonl(gateway.toIPv4Address());
        nlmsg_append_attr(nlmsg, sizeof(buf), RTA_GATEWAY, &ip4, sizeof(ip4));
    } else if (rtm->rtm_family == AF_INET6 &&
               gateway.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR bytes = gateway.toIPv6Address();
        nlmsg_append_attr(nlmsg, sizeof(buf), RTA_GATEWAY, &bytes,
                          sizeof(bytes));
    } else {
        logger.warning() << "Gateway family does not match exclusion route";
        return false;
    }
    nlmsg_append_attr32(nlmsg, sizeof(buf), RTA_OIF, gatewayIfindex);
    nlmsg_append_attr32(nlmsg, sizeof(buf), RTA_PRIORITY, 0);
    rtm->rtm_type = RTN_UNICAST;
    }

    const int requestSocket = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (requestSocket < 0) {
        logger.warning() << "Failed to create route request socket:"
                         << strerror(errno);
        return false;
    }
    auto closeRequestSocket = qScopeGuard([&] { close(requestSocket); });

    struct sockaddr_nl localAddress;
    memset(&localAddress, 0, sizeof(localAddress));
    localAddress.nl_family = AF_NETLINK;
    if (bind(requestSocket, reinterpret_cast<struct sockaddr*>(&localAddress),
             sizeof(localAddress)) != 0) {
        logger.warning() << "Failed to bind route request socket:"
                         << strerror(errno);
        return false;
    }

    struct sockaddr_nl kernelAddress;
    memset(&kernelAddress, 0, sizeof(kernelAddress));
    kernelAddress.nl_family = AF_NETLINK;
    const ssize_t sent = sendto(
        requestSocket, buf, nlmsg->nlmsg_len, 0,
        reinterpret_cast<struct sockaddr*>(&kernelAddress),
        sizeof(kernelAddress));
    if (sent != static_cast<ssize_t>(nlmsg->nlmsg_len)) {
        logger.warning() << "Failed to send route request:" << strerror(errno);
        return false;
    }

    constexpr auto acknowledgementTimeout = std::chrono::seconds(2);
    const auto acknowledgementDeadline =
        std::chrono::steady_clock::now() + acknowledgementTimeout;
    for (;;) {
        const auto remainingDuration = acknowledgementDeadline -
                                       std::chrono::steady_clock::now();
        if (remainingDuration <= std::chrono::steady_clock::duration::zero()) {
            logger.warning() << "Timed out waiting for route acknowledgement";
            return false;
        }
        const auto remainingMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                remainingDuration + std::chrono::milliseconds(1));
        struct pollfd descriptor = {requestSocket, POLLIN, 0};
        const int pollResult =
            poll(&descriptor, 1, static_cast<int>(remainingMilliseconds.count()));
        if (pollResult < 0 && errno == EINTR) {
            continue;
        }
        if (pollResult < 0) {
            logger.warning() << "Failed to wait for route acknowledgement:"
                             << strerror(errno);
            return false;
        }
        if (pollResult == 0) {
            logger.warning() << "Timed out waiting for route acknowledgement";
            return false;
        }
        if (!(descriptor.revents & POLLIN)) {
            logger.warning() << "Route acknowledgement socket failed:"
                             << descriptor.revents;
            return false;
        }

        alignas(struct nlmsghdr) char replyBuffer[4096];
        const ssize_t received = recv(requestSocket, replyBuffer,
                                      sizeof(replyBuffer), MSG_DONTWAIT);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            logger.warning() << "Failed to receive route acknowledgement:"
                             << strerror(errno);
            return false;
        }
        if (received == 0) {
            logger.warning() << "Route acknowledgement socket closed";
            return false;
        }

        int remaining = static_cast<int>(received);
        for (struct nlmsghdr* reply =
                 reinterpret_cast<struct nlmsghdr*>(replyBuffer);
             NLMSG_OK(reply, remaining);
             reply = NLMSG_NEXT(reply, remaining)) {
            if (reply->nlmsg_seq != requestSequence ||
                reply->nlmsg_type != NLMSG_ERROR) {
                continue;
            }
            if (reply->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                logger.warning() << "Received a truncated route acknowledgement";
                return false;
            }
            const struct nlmsgerr* error =
                reinterpret_cast<const struct nlmsgerr*>(NLMSG_DATA(reply));
            if (error->error == 0) {
                return true;
            }
            const int errorCode = -error->error;
            if (action == RTM_DELROUTE &&
                (errorCode == ENOENT || errorCode == ESRCH)) {
                return true;
            }
            if (action == RTM_NEWROUTE && errorCode == EEXIST) {
                logger.warning() << "Refusing to replace an existing route to"
                                 << prefix.toString();
            } else {
                logger.warning() << "Route request failed:"
                                 << strerror(errorCode);
            }
            return false;
        }
        logger.warning() << "Received an unexpected route acknowledgement";
        return false;
    }
}

LinuxRouteMonitor::RouteState LinuxRouteMonitor::routeState(
    const IPAddress& prefix, const RouteIdentity& expected) const {
    const int requestSocket = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (requestSocket < 0) {
        logger.warning() << "Failed to create route lookup socket:"
                         << strerror(errno);
        return RouteState::Unknown;
    }
    auto closeRequestSocket = qScopeGuard([&] { close(requestSocket); });

    struct sockaddr_nl localAddress = {};
    localAddress.nl_family = AF_NETLINK;
    if (bind(requestSocket, reinterpret_cast<struct sockaddr*>(&localAddress),
             sizeof(localAddress)) != 0) {
        logger.warning() << "Failed to bind route lookup socket:"
                         << strerror(errno);
        return RouteState::Unknown;
    }

    struct {
        struct nlmsghdr header;
        struct rtmsg route;
    } request = {};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.route.rtm_family = prefix.type() == QAbstractSocket::IPv4Protocol
                                   ? AF_INET
                                   : AF_INET6;

    struct sockaddr_nl kernelAddress = {};
    kernelAddress.nl_family = AF_NETLINK;
    const ssize_t sent = sendto(
        requestSocket, &request, request.header.nlmsg_len, 0,
        reinterpret_cast<struct sockaddr*>(&kernelAddress),
        sizeof(kernelAddress));
    if (sent != static_cast<ssize_t>(request.header.nlmsg_len)) {
        logger.warning() << "Failed to request route table:" << strerror(errno);
        return RouteState::Unknown;
    }

    bool exactRouteSeen = false;
    constexpr auto lookupTimeout = std::chrono::seconds(2);
    const auto deadline = std::chrono::steady_clock::now() + lookupTimeout;
    for (;;) {
        const auto remainingDuration = deadline - std::chrono::steady_clock::now();
        if (remainingDuration <= std::chrono::steady_clock::duration::zero()) {
            logger.warning() << "Timed out reading route table";
            return RouteState::Unknown;
        }
        const auto remainingMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                remainingDuration + std::chrono::milliseconds(1));
        struct pollfd descriptor = {requestSocket, POLLIN, 0};
        const int pollResult = poll(
            &descriptor, 1, static_cast<int>(remainingMilliseconds.count()));
        if (pollResult < 0 && errno == EINTR) {
            continue;
        }
        if (pollResult <= 0 || !(descriptor.revents & POLLIN)) {
            logger.warning() << "Failed to read route table:"
                             << (pollResult < 0 ? strerror(errno) : "timeout");
            return RouteState::Unknown;
        }

        alignas(struct nlmsghdr) char replyBuffer[16384];
        const ssize_t received = recv(requestSocket, replyBuffer,
                                      sizeof(replyBuffer), MSG_DONTWAIT);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            logger.warning() << "Failed to receive route table:"
                             << (received < 0 ? strerror(errno) : "closed");
            return RouteState::Unknown;
        }

        int remaining = static_cast<int>(received);
        for (struct nlmsghdr* message =
                 reinterpret_cast<struct nlmsghdr*>(replyBuffer);
             NLMSG_OK(message, remaining);
             message = NLMSG_NEXT(message, remaining)) {
            if (message->nlmsg_seq != request.header.nlmsg_seq) {
                continue;
            }
            if (message->nlmsg_type == NLMSG_DONE) {
                if (message->nlmsg_flags & NLM_F_DUMP_INTR) {
                    logger.warning() << "Route table dump was interrupted";
                    return RouteState::Unknown;
                }
                return exactRouteSeen ? RouteState::Foreign
                                      : RouteState::Absent;
            }
            if (message->nlmsg_type == NLMSG_ERROR) {
                if (message->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                    logger.warning() << "Received a truncated route table error";
                    return RouteState::Unknown;
                }
                const auto* error = reinterpret_cast<const struct nlmsgerr*>(
                    NLMSG_DATA(message));
                if (error->error != 0) {
                    logger.warning() << "Route table request failed:"
                                     << strerror(-error->error);
                    return RouteState::Unknown;
                }
                continue;
            }
            if (message->nlmsg_type != RTM_NEWROUTE ||
                message->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg))) {
                continue;
            }

            const auto* route = static_cast<const struct rtmsg*>(
                NLMSG_DATA(message));
            if (route->rtm_family != request.route.rtm_family ||
                route->rtm_dst_len != prefix.prefixLength()) {
                continue;
            }

            QHostAddress destination;
            QHostAddress gateway;
            unsigned int ifindex = 0;
            int priority = 0;
            bool destinationPresent = prefix.prefixLength() == 0;
            bool gatewayPresent = false;
            int attributesLength = RTM_PAYLOAD(message);
            for (const struct rtattr* attribute = RTM_RTA(route);
                 RTA_OK(attribute, attributesLength);
                 attribute = RTA_NEXT(attribute, attributesLength)) {
                const int payloadLength = RTA_PAYLOAD(attribute);
                if (attribute->rta_type == RTA_DST) {
                    if (route->rtm_family == AF_INET &&
                        payloadLength >= static_cast<int>(sizeof(struct in_addr))) {
                        struct in_addr address = {};
                        memcpy(&address, RTA_DATA(attribute), sizeof(address));
                        destination = QHostAddress(ntohl(address.s_addr));
                        destinationPresent = true;
                    } else if (route->rtm_family == AF_INET6 &&
                               payloadLength >= static_cast<int>(sizeof(struct in6_addr))) {
                        Q_IPV6ADDR address = {};
                        memcpy(&address, RTA_DATA(attribute), sizeof(address));
                        destination = QHostAddress(address);
                        destinationPresent = true;
                    }
                } else if (attribute->rta_type == RTA_GATEWAY) {
                    if (route->rtm_family == AF_INET &&
                        payloadLength >= static_cast<int>(sizeof(struct in_addr))) {
                        struct in_addr address = {};
                        memcpy(&address, RTA_DATA(attribute), sizeof(address));
                        gateway = QHostAddress(ntohl(address.s_addr));
                        gatewayPresent = true;
                    } else if (route->rtm_family == AF_INET6 &&
                               payloadLength >= static_cast<int>(sizeof(struct in6_addr))) {
                        Q_IPV6ADDR address = {};
                        memcpy(&address, RTA_DATA(attribute), sizeof(address));
                        gateway = QHostAddress(address);
                        gatewayPresent = true;
                    }
                } else if (attribute->rta_type == RTA_OIF &&
                           payloadLength >= static_cast<int>(sizeof(uint32_t))) {
                    memcpy(&ifindex, RTA_DATA(attribute), sizeof(ifindex));
                } else if (attribute->rta_type == RTA_PRIORITY &&
                           payloadLength >= static_cast<int>(sizeof(uint32_t))) {
                    uint32_t value = 0;
                    memcpy(&value, RTA_DATA(attribute), sizeof(value));
                    priority = static_cast<int>(value);
                }
            }
            if (!destinationPresent || destination != prefix.address()) {
                continue;
            }
            exactRouteSeen = true;
            const bool gatewayMatches = expected.gateway.isNull()
                                            ? !gatewayPresent
                                            : gatewayPresent &&
                                                  gateway == expected.gateway;
            if (route->rtm_type == expected.type &&
                route->rtm_protocol == expected.protocol &&
                route->rtm_scope == expected.scope &&
                ifindex == expected.ifindex && priority == expected.priority &&
                gatewayMatches) {
                return RouteState::Owned;
            }
        }
        if (remaining != 0) {
            logger.warning() << "Received a malformed route table response";
            return RouteState::Unknown;
        }
    }
}

static void nlmsg_append_attr(struct nlmsghdr* nlmsg, size_t maxlen,
                              int attrtype, const void* attrdata,
                              size_t attrlen) {
    size_t newlen = NLMSG_ALIGN(nlmsg->nlmsg_len) + RTA_SPACE(attrlen);
    if (newlen <= maxlen) {
    char* buf = reinterpret_cast<char*>(nlmsg) + NLMSG_ALIGN(nlmsg->nlmsg_len);
    struct rtattr* attr = reinterpret_cast<struct rtattr*>(buf);
    attr->rta_type = attrtype;
    attr->rta_len = RTA_LENGTH(attrlen);
    memcpy(RTA_DATA(attr), attrdata, attrlen);
    nlmsg->nlmsg_len = newlen;
    }
}

static void nlmsg_append_attr32(struct nlmsghdr* nlmsg, size_t maxlen,
                                int attrtype, uint32_t value) {
    nlmsg_append_attr(nlmsg, maxlen, attrtype, &value, sizeof(value));
}

void LinuxRouteMonitor::nlsockReady() {
    char buf[1024];
    ssize_t len = recv(m_nlsock, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0) {
    return;
    }

    struct nlmsghdr* nlmsg = (struct nlmsghdr*)buf;
    while (NLMSG_OK(nlmsg, len)) {
    if (nlmsg->nlmsg_type == NLMSG_DONE) {
        return;
    }
    if (nlmsg->nlmsg_type != NLMSG_ERROR) {
        nlmsg = NLMSG_NEXT(nlmsg, len);
        continue;
    }
    struct nlmsgerr* err = static_cast<struct nlmsgerr*>(NLMSG_DATA(nlmsg));
    if (err->error != 0) {
        logger.debug() << "Netlink request failed:" << strerror(-err->error);
    }
    nlmsg = NLMSG_NEXT(nlmsg, len);
    }
}

static bool buildAllowedIp(wg_allowedip* ip,
                                         const IPAddress& prefix) {
    const char* addrString = qPrintable(prefix.address().toString());
    if (prefix.type() == QAbstractSocket::IPv4Protocol) {
    ip->family = AF_INET;
    ip->cidr = prefix.prefixLength();
    return inet_pton(AF_INET, addrString, &ip->ip4) == 1;
    }
    if (prefix.type() == QAbstractSocket::IPv6Protocol) {
    ip->family = AF_INET6;
    ip->cidr = prefix.prefixLength();
    return inet_pton(AF_INET6, addrString, &ip->ip6) == 1;
    }
    return false;
}
