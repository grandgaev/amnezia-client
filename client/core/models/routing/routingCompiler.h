#ifndef ROUTINGCOMPILER_H
#define ROUTINGCOMPILER_H

#include <QHash>
#include <QJsonObject>
#include <QStringList>

#include "routingProfile.h"

namespace amnezia
{
    namespace routing
    {
        // Address announced to the OS as DNS server when the AmneziaWG router is
        // active. It must not be a real, well-known resolver: browsers upgrade
        // well-known resolvers (1.1.1.1, 8.8.8.8, ...) to DNS-over-HTTPS, which
        // would bypass the domain based routing.
        inline constexpr char routerDnsAddress[] = "198.18.0.53";

        struct CompileInput
        {
            RoutingProfile profile;
            QString geoSitePath;
            QString geoIpPath;
            QStringList excludedRoutes;
            // DNS servers of the VPN connection (used when the profile does not
            // define its own remote DNS).
            QStringList connectionDns;
            // "os" when the platform guarantees that sockets opened by the tunnel
            // implementation bypass the tunnel (Network Extension, Android with
            // protected sockets), "interface" when the daemon provides the
            // interface to bind to.
            QString bypassMode = QStringLiteral("os");
            // Per-application routing handled by the router (Windows only):
            // "include" = only these apps use the VPN, "exclude" = these apps bypass it.
            QString appsMode;
            QStringList appPaths;
            // false when the tunnel carries no IPv6 (proxied IPv6 is then
            // refused at once so that applications fall back to IPv4).
            bool tunnelHasIpv6 = true;
        };

        struct ExpandedProfile
        {
            QList<RuleAction> order;
            QHash<int, QStringList> domains; // RuleAction -> domain rules with explicit prefixes
            QHash<int, QStringList> ips;     // RuleAction -> CIDRs
            QHash<int, QStringList> xrayDomains; // RuleAction -> domain rules in Xray syntax
            QStringList warnings;
            bool hasRules() const;
        };

        struct IpRoutes
        {
            bool includeMode = false; // true: only the listed addresses go through the VPN
            QStringList cidrs;
            QStringList hostnames; // names that must be resolved to addresses at connect time
            QStringList warnings;
        };

        class RoutingCompiler
        {
        public:
            static ExpandedProfile expand(const CompileInput &input);

            // Configuration of the AmneziaWG router (amneziawg-go "router" package).
            static QJsonObject routerConfig(const CompileInput &input, const ExpandedProfile &expanded);

            // Adds routing, DNS and outbounds of the profile to an Xray client config.
            static void applyToXrayConfig(QJsonObject &xrayConfig, const CompileInput &input, const ExpandedProfile &expanded);

            // Address based routes for protocols that cannot route by domain
            // (OpenVPN, Cloak, ShadowSocks, IKEv2).
            static IpRoutes ipRoutes(const CompileInput &input, const ExpandedProfile &expanded);

            static QStringList remoteDnsServers(const CompileInput &input);
            static QStringList domesticDnsServers(const CompileInput &input);
        };
    } // namespace routing
} // namespace amnezia

#endif // ROUTINGCOMPILER_H
