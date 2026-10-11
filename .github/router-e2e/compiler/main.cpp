#include <QCoreApplication>
#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>

#include "routingCompiler.h"

static bool downloadGeoFile(const QString &url, const QString &path)
{
    QNetworkAccessManager network;
    QNetworkRequest request{QUrl(url)};
    if (request.url().scheme() != QStringLiteral("https")) {
        return false;
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = network.get(request);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        reply->abort();
        return false;
    }
    QEventLoop loop;
    qint64 size = 0;
    bool writeOk = true;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto consume = [&]() {
        const QByteArray chunk = reply->readAll();
        size += chunk.size();
        if (size > 64 * 1024 * 1024 || output.write(chunk) != chunk.size()) {
            writeOk = false;
            reply->abort();
        } else {
            hash.addData(chunk);
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    consume();
    if (!writeOk || reply->error() != QNetworkReply::NoError || size == 0 || !output.commit()) {
        return false;
    }
    QTextStream(stdout) << QFileInfo(path).fileName() << " SHA256 " << hash.result().toHex() << '\n';
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 5) {
        qCritical("Usage: routing_config_export <profile> <geosite> <geoip> <output>, or <profile> --download-geodata <directory> <output>");
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
    if (args[2] == QStringLiteral("--download-geodata")) {
        if (!QDir().mkpath(args[3])) {
            return 2;
        }
        input.geoSitePath = QDir(args[3]).filePath(QStringLiteral("geosite.dat"));
        input.geoIpPath = QDir(args[3]).filePath(QStringLiteral("geoip.dat"));
        if (!downloadGeoFile(input.profile.geoSiteUrl, input.geoSitePath)
            || !downloadGeoFile(input.profile.geoIpUrl, input.geoIpPath)) {
            qCritical("Cannot download the profile's configured geo data");
            return 2;
        }
    } else {
        input.geoSitePath = args[2];
        input.geoIpPath = args[3];
    }
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
