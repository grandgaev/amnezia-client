#ifndef GEOUTILS_H
#define GEOUTILS_H

#include <QString>
#include <QStringList>

namespace amnezia
{
    namespace geoutils
    {
        // Expand geoip:<code> tokens into a list of CIDR strings using the geoip.dat
        // bundled next to the service executable (same file xray reads via
        // XRAY_LOCATION_ASSET). This lets non-xray protocols (WireGuard/AWG/OpenVPN)
        // honor geoip: split-tunneling rules as plain OS routes.
        //
        // Tokens that are not "geoip:<code>" are ignored. Inverse categories
        // ("geoip:!code") are skipped: an "everything except" set cannot be expressed
        // as a finite route list. Categories with more than maxCidrsPerRule entries are
        // skipped to avoid bloating the OS routing table (geoip:us is ~380k CIDRs);
        // pass a negative limit to disable the cap. Duplicate CIDRs are removed.
        QStringList expandGeoipRules(const QStringList &tokens, int maxCidrsPerRule = 2000);

        // Number of CIDRs a geoip:<code> category holds, or -1 if not found. Used by the
        // UI to warn that a category is too large to expand for non-xray protocols.
        int geoipCategorySize(const QString &code);

        // Expand geosite:<code> tokens into the resolvable hostnames of those categories
        // using the geosite.dat bundled next to the service executable. Only Domain and
        // Full entries (an actual host) are returned; Plain (keyword) and Regex entries
        // cannot be resolved to an IP. Non-geosite tokens are ignored. Categories with
        // more than maxDomainsPerRule resolvable hosts are skipped to keep the follow-up
        // DNS resolution bounded; pass a negative limit to disable the cap. The caller
        // (client) resolves the returned hostnames to IPs and caches them, exactly like
        // a plain domain rule, so non-xray protocols can route by them.
        QStringList expandGeositeDomains(const QStringList &tokens, int maxDomainsPerRule = 256);

        // Number of resolvable hosts a geosite:<code> category holds, or -1 if not found.
        int geositeCategorySize(const QString &code);
    }
}

#endif // GEOUTILS_H
