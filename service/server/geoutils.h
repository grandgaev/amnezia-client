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
    }
}

#endif // GEOUTILS_H
