#ifndef ROUTINGCONFIGURATOR_H
#define ROUTINGCONFIGURATOR_H

#include <QJsonObject>
#include <QString>

#include "core/utils/routingProfile.h"

namespace amnezia
{
    // Turns a RoutingProfile into xray-core `routing`, `dns` and inbound `sniffing`
    // sections and injects them into an existing xray client config, mirroring how
    // Happ builds split tunneling entirely inside the xray routing layer.
    class RoutingConfigurator
    {
    public:
        // Parses `xrayConfig` (compact JSON string), applies the profile and returns
        // the updated compact JSON string. On any parse error the input is returned
        // unchanged and `ok` is set to false.
        static QString applyToXrayConfig(const QString &xrayConfig, const RoutingProfile &profile, bool &ok);

        // Builds just the `routing` object for the profile. Exposed for tests.
        static QJsonObject buildRoutingSection(const RoutingProfile &profile);

        // Builds just the `dns` object for the profile. Exposed for tests.
        static QJsonObject buildDnsSection(const RoutingProfile &profile);

    private:
        static QJsonObject buildDnsServer(const RoutingDnsServer &server, const QStringList &domains);
    };
}

#endif // ROUTINGCONFIGURATOR_H
