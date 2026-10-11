#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>

#include "routingCompiler.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 5) {
        qCritical("Usage: routing_config_export <profile> <geosite> <geoip> <output>");
        return 2;
    }
    QFile source(args[1]);
    if (!source.open(QIODevice::ReadOnly)) {
        qCritical("Cannot read the test profile");
        return 2;
    }
    QString error;
    const auto profiles = amnezia::routing::parseImportData(QString::fromUtf8(source.readAll()), &error);
    if (profiles.size() != 1) {
        qCritical("Expected exactly one routing profile");
        return 2;
    }
    amnezia::routing::CompileInput input;
    input.profile = amnezia::routing::RoutingProfile::createDefault(QStringLiteral("e2e"));
    input.profile.mergeExchangeJson(profiles.first());
    input.geoSitePath = args[2];
    input.geoIpPath = args[3];
    input.bypassMode = QStringLiteral("interface");
    input.tunnelHasIpv6 = false;
    input.ipv6Blocked = true;
    input.connectionDns = { QStringLiteral("1.1.1.1") };
    const auto expanded = amnezia::routing::RoutingCompiler::expand(input);
    for (const auto &warning : expanded.warnings) {
        qWarning().noquote() << warning;
    }
    if (!expanded.warnings.isEmpty() || !expanded.hasRules()) {
        qCritical("The original profile did not fully expand its routing rules");
        return 2;
    }
    QSaveFile output(args[4]);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(QJsonDocument(amnezia::routing::RoutingCompiler::routerConfig(input, expanded)).toJson()) < 0
        || !output.commit()) {
        qCritical("Cannot write the compiled routing profile");
        return 2;
    }
    QTextStream(stdout) << "Compiled profile with " << expanded.domains.size()
                        << " domain rule groups and " << expanded.ips.size() << " IP rule groups\n";
    return 0;
}
