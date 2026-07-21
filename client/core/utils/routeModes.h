#ifndef ROUTEMODES_H
#define ROUTEMODES_H

#include <QMetaEnum>
#include <QObject>

namespace amnezia
{
    namespace route_mode_ns
    {
        Q_NAMESPACE
        enum RouteMode {
            VpnAllSites,
            VpnOnlyForwardSites,
            VpnAllExceptSites
        };
        Q_ENUM_NS(RouteMode)
    }

    using RouteMode = route_mode_ns::RouteMode;

    namespace apps_route_mode_ns
    {
        Q_NAMESPACE
        enum AppsRouteMode {
            VpnAllApps,
            VpnOnlyForwardApps,
            VpnAllExceptApps
        };
        Q_ENUM_NS(AppsRouteMode)
    }

    using AppsRouteMode = apps_route_mode_ns::AppsRouteMode;

    namespace rule_outbound_ns
    {
        Q_NAMESPACE
        enum RuleOutbound {
            Proxy,
            Direct,
            Block
        };
        Q_ENUM_NS(RuleOutbound)
    }

    using RuleOutbound = rule_outbound_ns::RuleOutbound;

    namespace rule_order_ns
    {
        Q_NAMESPACE
        enum RuleOrder {
            BlockDirectProxy,
            BlockProxyDirect,
            ProxyDirectBlock,
            ProxyBlockDirect,
            DirectProxyBlock,
            DirectBlockProxy
        };
        Q_ENUM_NS(RuleOrder)
    }

    using RuleOrder = rule_order_ns::RuleOrder;

    namespace domain_strategy_ns
    {
        Q_NAMESPACE
        enum DomainStrategy {
            IPIfNonMatch,
            IPOnDemand,
            AsIs
        };
        Q_ENUM_NS(DomainStrategy)
    }

    using DomainStrategy = domain_strategy_ns::DomainStrategy;

    namespace dns_mode_ns
    {
        Q_NAMESPACE
        enum DnsMode {
            DoH,
            DoU
        };
        Q_ENUM_NS(DnsMode)
    }

    using DnsMode = dns_mode_ns::DnsMode;
}

#endif // ROUTEMODES_H


