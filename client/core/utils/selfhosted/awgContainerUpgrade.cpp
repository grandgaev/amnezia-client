#include "awgContainerUpgrade.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include "core/models/containerConfig.h"
#include "core/protocols/protocolUtils.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/constants/protocolConstants.h"
#include "core/utils/selfhosted/scriptsRegistry.h"
#include "core/utils/selfhosted/sshSession.h"

using namespace ProtocolUtils;

namespace amnezia
{

namespace
{
    QMap<QString, QString> parseAwgConfigMap(const QString &nativeConfig)
    {
        QMap<QString, QString> map;
        const auto lines = nativeConfig.split(QLatin1Char('\n'));
        for (const auto &line : lines) {
            const QString trimmedLine = line.trimmed();
            if (trimmedLine.startsWith(QLatin1Char('[')) && trimmedLine.endsWith(QLatin1Char(']'))) {
                continue;
            }
            const QStringList parts = trimmedLine.split(QStringLiteral(" = "));
            if (parts.size() == 2) {
                map.insert(parts.at(0).trimmed(), parts.at(1).trimmed());
            }
        }
        return map;
    }
} // namespace

AwgInterfaceSnapshot parseAwgInterfaceSnapshot(const QString &rawAwgConfig)
{
    AwgInterfaceSnapshot snapshot;
    const QStringList lines = rawAwgConfig.split(QLatin1Char('\n'));

    int peerStartLine = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == QLatin1String("[Peer]")) {
            peerStartLine = i;
            break;
        }
    }

    const int interfaceEnd = (peerStartLine >= 0) ? peerStartLine : lines.size();
    for (int i = 0; i < interfaceEnd; ++i) {
        const QString trimmedLine = lines.at(i).trimmed();
        if (trimmedLine.isEmpty() || trimmedLine.startsWith(QLatin1Char('[')) || trimmedLine.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList parts = trimmedLine.split(QStringLiteral(" = "));
        if (parts.size() != 2) {
            continue;
        }
        const QString key = parts.at(0).trimmed();
        const QString value = parts.at(1).trimmed();
        if (key == QLatin1String("PrivateKey")) {
            snapshot.privateKey = value;
        } else if (key == QLatin1String("Address")) {
            snapshot.address = value;
        } else if (key == QLatin1String("ListenPort")) {
            snapshot.listenPort = value;
        }
    }

    if (peerStartLine >= 0) {
        QStringList peerLines = lines.mid(peerStartLine);
        while (!peerLines.isEmpty() && peerLines.last().trimmed().isEmpty()) {
            peerLines.removeLast();
        }
        snapshot.peersBlock = peerLines.join(QLatin1Char('\n'));
        for (const QString &line : std::as_const(peerLines)) {
            if (line.trimmed() == QLatin1String("[Peer]")) {
                ++snapshot.peerCount;
            }
        }
    }

    snapshot.isValid =
            !snapshot.privateKey.isEmpty() && !snapshot.address.isEmpty() && !snapshot.listenPort.isEmpty();
    return snapshot;
}

QString buildUpgradedAwgConfig(const AwgInterfaceSnapshot &snapshot, const AwgServerConfig &newParams)
{
    if (!snapshot.isValid) {
        return QString();
    }

    QStringList lines;
    lines << QStringLiteral("[Interface]");
    lines << QStringLiteral("PrivateKey = %1").arg(snapshot.privateKey);
    lines << QStringLiteral("Address = %1").arg(snapshot.address);
    lines << QStringLiteral("ListenPort = %1").arg(snapshot.listenPort);

    const auto appendIfNotEmpty = [&lines](const QString &key, const QString &value) {
        if (!value.trimmed().isEmpty()) {
            lines << QStringLiteral("%1 = %2").arg(key, value);
        }
    };
    const auto appendCommentIfNotEmpty = [&lines](const QString &key, const QString &value) {
        if (!value.trimmed().isEmpty()) {
            lines << QStringLiteral("# %1 = %2").arg(key, value);
        }
    };

    appendIfNotEmpty(QStringLiteral("Jc"), newParams.junkPacketCount);
    appendIfNotEmpty(QStringLiteral("Jmin"), newParams.junkPacketMinSize);
    appendIfNotEmpty(QStringLiteral("Jmax"), newParams.junkPacketMaxSize);
    appendIfNotEmpty(QStringLiteral("S1"), newParams.initPacketJunkSize);
    appendIfNotEmpty(QStringLiteral("S2"), newParams.responsePacketJunkSize);
    appendIfNotEmpty(QStringLiteral("S3"), newParams.cookieReplyPacketJunkSize);
    appendIfNotEmpty(QStringLiteral("S4"), newParams.transportPacketJunkSize);
    appendIfNotEmpty(QStringLiteral("H1"), newParams.initPacketMagicHeader);
    appendIfNotEmpty(QStringLiteral("H2"), newParams.responsePacketMagicHeader);
    appendIfNotEmpty(QStringLiteral("H3"), newParams.underloadPacketMagicHeader);
    appendIfNotEmpty(QStringLiteral("H4"), newParams.transportPacketMagicHeader);
    appendIfNotEmpty(QStringLiteral("HeaderProtectionKey"), newParams.headerProtectionKey);
    appendIfNotEmpty(QStringLiteral("ContentPaddingAddition"), newParams.contentPaddingAddition);
    appendIfNotEmpty(QStringLiteral("RekeyAfterTime"), newParams.rekeyAfterTime);
    appendIfNotEmpty(QStringLiteral("RekeyTimeout"), newParams.rekeyTimeout);
    appendIfNotEmpty(QStringLiteral("RejectAfterTime"), newParams.rejectAfterTime);
    appendIfNotEmpty(QStringLiteral("KeepaliveTimeout"), newParams.keepaliveTimeout);
    appendIfNotEmpty(QStringLiteral("MaxHandshakeAttempts"), newParams.maxHandshakeAttempts);
    if (AwgProtocolConfig::isToggleEnabled(newParams.randomTrailers)) {
        lines << QStringLiteral("RandomTrailers = %1").arg(newParams.randomTrailers);
    }
    if (AwgProtocolConfig::isToggleEnabled(newParams.disableCookies)) {
        lines << QStringLiteral("DisableCookies = %1").arg(newParams.disableCookies);
    }
    appendCommentIfNotEmpty(QStringLiteral("I1"), newParams.specialJunk1);
    appendCommentIfNotEmpty(QStringLiteral("I2"), newParams.specialJunk2);
    appendCommentIfNotEmpty(QStringLiteral("I3"), newParams.specialJunk3);
    appendCommentIfNotEmpty(QStringLiteral("I4"), newParams.specialJunk4);
    appendCommentIfNotEmpty(QStringLiteral("I5"), newParams.specialJunk5);

    QString result = lines.join(QLatin1Char('\n'));
    result += QLatin1Char('\n');

    if (!snapshot.peersBlock.isEmpty()) {
        result += QLatin1Char('\n');
        result += snapshot.peersBlock;
        result += QLatin1Char('\n');
    }

    return result;
}

AwgClientConfig reRenderAwgAdminClientConfig(const AwgClientConfig &oldClientConfig, DockerContainer container,
                                              const AwgServerConfig &newServerParams)
{
    ServerCredentials credentials;
    credentials.hostName = oldClientConfig.hostName;

    ContainerConfig rendererInput;
    rendererInput.container = container;
    AwgProtocolConfig awgProtocolConfig;
    awgProtocolConfig.serverConfig = newServerParams;
    rendererInput.protocolConfig = ProtocolConfig(awgProtocolConfig);

    ScriptVars vars = genBaseVars(credentials, container, QString(), QString());
    vars.append(genProtocolVarsForContainer(container, rendererInput));

    QString config = SshSession::replaceVars(scriptData(ProtocolScriptType::awg_template, container), vars);

    // The template lists every possible key, but each parameter is optional - drop the lines
    // whose value came out empty, same as WireguardConfigurator::createConfig does.
    static const QRegularExpression emptyValueLine(QStringLiteral(R"(^\s*\S+\s*=\s*$)"));
    QStringList configLines = config.split(QLatin1Char('\n'));
    configLines.removeIf([](const QString &line) { return emptyValueLine.match(line).hasMatch(); });
    config = configLines.join(QLatin1Char('\n'));

    // The DNS servers are not part of the server parameters: keep the ones of the old config.
    {
        static const QRegularExpression dnsLine(QStringLiteral(R"(^\s*DNS\s*=.*$)"));
        QString oldDnsLine;
        for (const QString &line : oldClientConfig.nativeConfig.split(QLatin1Char('\n'))) {
            if (dnsLine.match(line).hasMatch()) {
                oldDnsLine = line.trimmed();
                break;
            }
        }
        QStringList renderedLines = config.split(QLatin1Char('\n'));
        for (int i = 0; i < renderedLines.size(); ++i) {
            if (dnsLine.match(renderedLines.at(i)).hasMatch()) {
                if (oldDnsLine.isEmpty()) {
                    renderedLines.removeAt(i);
                } else {
                    renderedLines[i] = oldDnsLine;
                }
                break;
            }
        }
        config = renderedLines.join(QLatin1Char('\n'));
    }

    config.replace(QStringLiteral("$WIREGUARD_CLIENT_PRIVATE_KEY"), oldClientConfig.clientPrivateKey);
    config.replace(QStringLiteral("$WIREGUARD_CLIENT_IP"), oldClientConfig.clientIp);
    config.replace(QStringLiteral("$WIREGUARD_SERVER_PUBLIC_KEY"), oldClientConfig.serverPublicKey);
    config.replace(QStringLiteral("$WIREGUARD_PSK"), oldClientConfig.presharedKey);

    const QMap<QString, QString> configMap = parseAwgConfigMap(config);

    // Start from the old config so everything not explicitly overwritten below (client id/key
    // pair, tunnel IP, server public key, PSK, host, port, MTU, allowed IPs, ...) is kept as-is.
    AwgClientConfig newClientConfig = oldClientConfig;
    newClientConfig.nativeConfig = config;
    newClientConfig.persistentKeepAlive = newServerParams.hasAwg3Params()
            ? QString(protocols::awg::defaultPersistentKeepAlive)
            : QString(protocols::wireguard::defaultPersistentKeepAlive);

    newClientConfig.junkPacketCount = configMap.value(configKey::junkPacketCount);
    newClientConfig.junkPacketMinSize = configMap.value(configKey::junkPacketMinSize);
    newClientConfig.junkPacketMaxSize = configMap.value(configKey::junkPacketMaxSize);
    newClientConfig.initPacketJunkSize = configMap.value(configKey::initPacketJunkSize);
    newClientConfig.responsePacketJunkSize = configMap.value(configKey::responsePacketJunkSize);
    newClientConfig.initPacketMagicHeader = configMap.value(configKey::initPacketMagicHeader);
    newClientConfig.responsePacketMagicHeader = configMap.value(configKey::responsePacketMagicHeader);
    newClientConfig.underloadPacketMagicHeader = configMap.value(configKey::underloadPacketMagicHeader);
    newClientConfig.transportPacketMagicHeader = configMap.value(configKey::transportPacketMagicHeader);
    newClientConfig.specialJunk1 = configMap.value(configKey::specialJunk1);
    newClientConfig.specialJunk2 = configMap.value(configKey::specialJunk2);
    newClientConfig.specialJunk3 = configMap.value(configKey::specialJunk3);
    newClientConfig.specialJunk4 = configMap.value(configKey::specialJunk4);
    newClientConfig.specialJunk5 = configMap.value(configKey::specialJunk5);
    newClientConfig.cookieReplyPacketJunkSize = configMap.value(configKey::cookieReplyPacketJunkSize);
    newClientConfig.transportPacketJunkSize = configMap.value(configKey::transportPacketJunkSize);
    newClientConfig.headerProtectionKey = configMap.value(configKey::headerProtectionKey);
    newClientConfig.contentPaddingAddition = configMap.value(configKey::contentPaddingAddition);
    newClientConfig.rekeyAfterTime = configMap.value(configKey::rekeyAfterTime);
    newClientConfig.rekeyTimeout = configMap.value(configKey::rekeyTimeout);
    newClientConfig.rejectAfterTime = configMap.value(configKey::rejectAfterTime);
    newClientConfig.keepaliveTimeout = configMap.value(configKey::keepaliveTimeout);
    newClientConfig.maxHandshakeAttempts = configMap.value(configKey::maxHandshakeAttempts);
    newClientConfig.randomTrailers = configMap.value(configKey::randomTrailers);
    newClientConfig.disableCookies = configMap.value(configKey::disableCookies);

    return newClientConfig;
}

QJsonArray flagClientsForConfigUpdate(const QJsonArray &clientsTable, const QString &adminClientId, bool needsUpdate)
{
    QJsonArray result;
    for (const QJsonValue &value : clientsTable) {
        if (!value.isObject()) {
            result.append(value);
            continue;
        }
        QJsonObject client = value.toObject();
        const QString clientId = client.value(configKey::clientId).toString();
        if (clientId.isEmpty() || (!adminClientId.isEmpty() && clientId == adminClientId)) {
            result.append(client);
            continue;
        }
        QJsonObject userData = client.value(configKey::userData).toObject();
        if (needsUpdate) {
            userData[configKey::needsConfigUpdate] = true;
        } else {
            userData.remove(configKey::needsConfigUpdate);
        }
        client[configKey::userData] = userData;
        result.append(client);
    }
    return result;
}

} // namespace amnezia
