#include "wireguardConfigurator.h"

#include <QDebug>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "core/utils/containerEnum.h"
#include "core/utils/containers/containerUtils.h"
#include "core/utils/protocolEnum.h"
#include "core/utils/selfhosted/sshSession.h"
#include "core/utils/selfhosted/scriptsRegistry.h"
#include "core/utils/protocolEnum.h"
#include "core/protocols/protocolUtils.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/constants/protocolConstants.h"
#include "core/utils/utilities.h"
#include "core/models/containerConfig.h"
#include "core/models/protocols/wireGuardProtocolConfig.h"
#include "core/models/protocols/awgProtocolConfig.h"
#include <QJsonArray>

using namespace amnezia;

namespace
{
    struct PeerRemovalResult
    {
        QString config;
        int matches = 0;
    };

    PeerRemovalResult removePeerByPublicKey(const QString &config, const QString &publicKey)
    {
        QStringList keptSections;
        PeerRemovalResult result;
        const QStringList sections = config.split('[', Qt::KeepEmptyParts);
        for (const QString &section : sections) {
            bool matches = false;
            if (section.startsWith(QStringLiteral("Peer]"))) {
                for (const QString &line : section.split('\n')) {
                    const qsizetype separator = line.indexOf('=');
                    if (separator >= 0
                            && line.left(separator).trimmed() == QStringLiteral("PublicKey")
                            && line.mid(separator + 1).trimmed() == publicKey) {
                        matches = true;
                        break;
                    }
                }
            }
            if (matches) {
                ++result.matches;
            } else {
                keptSections.push_back(section);
            }
        }
        result.config = keptSections.join('[');
        return result;
    }

    ErrorCode uploadAndVerify(SshSession *sshSession, const ServerCredentials &credentials,
                              DockerContainer container, const QString &contents, const QString &path)
    {
        ErrorCode error = sshSession->uploadTextFileToContainer(container, credentials, contents, path);
        if (error != ErrorCode::NoError) {
            return error;
        }
        const QByteArray readBack = sshSession->getTextFileFromContainer(container, credentials, path, error);
        if (error != ErrorCode::NoError) {
            return error;
        }
        return readBack == contents.toUtf8() ? ErrorCode::NoError : ErrorCode::InternalError;
    }

    ErrorCode syncConfigChecked(SshSession *sshSession, const ServerCredentials &credentials,
                                DockerContainer container, const QString &configPath)
    {
        const bool isAwg2 = container == DockerContainer::Awg2;
        const QString command = isAwg2 ? QStringLiteral("awg") : QStringLiteral("wg");
        const QString iface = isAwg2 ? QStringLiteral("awg0") : QStringLiteral("wg0");
        const QString marker = QStringLiteral("AMNEZIA_GENERATED_PEER_SYNC_OK");
        const QString commandLine = QStringLiteral(
                "sudo docker exec -i $CONTAINER_NAME bash -c '%1 syncconf %2 <(%1-quick strip %3)"
                " && printf AMNEZIA_GENERATED_PEER_SYNC_OK'")
                                            .arg(command, iface, configPath);
        QString stdOut;
        const auto captureStdOut = [&stdOut](const QString &data, libssh::Client &) {
            stdOut += data;
            return ErrorCode::NoError;
        };
        const ErrorCode error = sshSession->runScript(
                credentials,
                sshSession->replaceVars(
                        commandLine, amnezia::genBaseVars(credentials, container, QString(), QString())),
                captureStdOut);
        if (error != ErrorCode::NoError) {
            return error;
        }
        return stdOut.trimmed() == marker ? ErrorCode::NoError : ErrorCode::InternalError;
    }
}

WireguardConfigurator::WireguardConfigurator(SshSession* sshSession, bool isAwg,
                                             QObject *parent)
    : ConfiguratorBase(sshSession, parent), m_isAwg(isAwg)
{
    m_serverConfigPath =
            m_isAwg ? amnezia::protocols::awg::serverConfigPath : amnezia::protocols::wireguard::serverConfigPath;
    m_serverPublicKeyPath =
            m_isAwg ? amnezia::protocols::awg::serverPublicKeyPath : amnezia::protocols::wireguard::serverPublicKeyPath;
    m_serverPskKeyPath =
            m_isAwg ? amnezia::protocols::awg::serverPskKeyPath : amnezia::protocols::wireguard::serverPskKeyPath;
    m_configTemplate = m_isAwg ? ProtocolScriptType::awg_template : ProtocolScriptType::wireguard_template;

    m_protocolName = m_isAwg ? configKey::awg : configKey::wireguard;
    m_defaultPort = m_isAwg ? protocols::awg::defaultPort : protocols::wireguard::defaultPort;
}

WireguardConfigurator::ConnectionData WireguardConfigurator::genClientKeys()
{
    // TODO review
    constexpr size_t EDDSA_KEY_LENGTH = 32;

    ConnectionData connData;

    unsigned char buff[EDDSA_KEY_LENGTH];
    int ret = RAND_priv_bytes(buff, EDDSA_KEY_LENGTH);
    if (ret <= 0)
        return connData;

    EVP_PKEY *pKey = EVP_PKEY_new();
    q_check_ptr(pKey);
    pKey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, &buff[0], EDDSA_KEY_LENGTH);

    size_t keySize = EDDSA_KEY_LENGTH;

    // save private key
    unsigned char priv[EDDSA_KEY_LENGTH];
    EVP_PKEY_get_raw_private_key(pKey, priv, &keySize);
    connData.clientPrivKey = QByteArray::fromRawData((char *)priv, keySize).toBase64();

    // save public key
    unsigned char pub[EDDSA_KEY_LENGTH];
    EVP_PKEY_get_raw_public_key(pKey, pub, &keySize);
    connData.clientPubKey = QByteArray::fromRawData((char *)pub, keySize).toBase64();

    return connData;
}

QList<QHostAddress> WireguardConfigurator::getIpsFromConf(const QString &input)
{
    QRegularExpression regex("AllowedIPs = (\\d+\\.\\d+\\.\\d+\\.\\d+)");
    QRegularExpressionMatchIterator matchIterator = regex.globalMatch(input);

    QList<QHostAddress> ips;

    while (matchIterator.hasNext()) {
        QRegularExpressionMatch match = matchIterator.next();
        const QString address_string { match.captured(1) };
        const QHostAddress address { address_string };
        if (address.isNull()) {
            qWarning() << "Couldn't recognize the ip address: " << address_string;
        } else {
            ips << address;
        }
    }

    return ips;
}

WireguardConfigurator::ConnectionData WireguardConfigurator::prepareWireguardConfig(const ServerCredentials &credentials,
                                                                                    DockerContainer container,
                                                                                    const WireGuardServerConfig* serverConfig,
                                                                                    const AwgServerConfig* awgServerConfig,
                                                                                    const DnsSettings &dnsSettings,
                                                                                    ErrorCode &errorCode)
{
    Q_UNUSED(dnsSettings);

    WireguardConfigurator::ConnectionData connData = WireguardConfigurator::genClientKeys();
    connData.host = credentials.hostName;
    
    QString portStr = m_defaultPort;
    if (serverConfig && !serverConfig->port.isEmpty()) {
        portStr = serverConfig->port;
    } else if (awgServerConfig && !awgServerConfig->port.isEmpty()) {
        portStr = awgServerConfig->port;
    }
    connData.port = portStr;

    if (connData.clientPrivKey.isEmpty() || connData.clientPubKey.isEmpty()) {
        errorCode = ErrorCode::InternalError;
        return connData;
    }

    QString configPath = m_serverConfigPath;
    if (container == DockerContainer::Awg) {
        configPath = amnezia::protocols::awg::serverLegacyConfigPath;
    }
    QString getIpsScript = QString("cat %1 | grep AllowedIPs").arg(configPath);
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    errorCode = m_sshSession->runContainerScript(credentials, container, getIpsScript, cbReadStdOut);
    if (errorCode != ErrorCode::NoError) {
        return connData;
    }
    auto ips = getIpsFromConf(stdOut);

    QHostAddress nextIp = [&] {
        QHostAddress result;
        QHostAddress lastIp;
        QString subnetAddress = protocols::wireguard::defaultSubnetAddress;
        if (serverConfig && !serverConfig->subnetAddress.isEmpty()) {
            subnetAddress = serverConfig->subnetAddress;
        } else if (awgServerConfig && !awgServerConfig->subnetAddress.isEmpty()) {
            subnetAddress = awgServerConfig->subnetAddress;
        }
        if (ips.empty()) {
            lastIp.setAddress(subnetAddress);
        } else {
            lastIp = ips.last();
        }
        quint8 lastOctet = static_cast<quint8>(lastIp.toIPv4Address());
        switch (lastOctet) {
        case 254: result.setAddress(lastIp.toIPv4Address() + 3); break;
        case 255: result.setAddress(lastIp.toIPv4Address() + 2); break;
        default: result.setAddress(lastIp.toIPv4Address() + 1); break;
        }

        return result;
    }();

    connData.clientIP = nextIp.toString();

    // Get keys
    connData.serverPubKey =
            m_sshSession->getTextFileFromContainer(container, credentials, m_serverPublicKeyPath, errorCode);
    connData.serverPubKey.replace("\n", "");
    if (errorCode != ErrorCode::NoError) {
        return connData;
    }

    connData.pskKey = m_sshSession->getTextFileFromContainer(container, credentials, m_serverPskKeyPath, errorCode);
    connData.pskKey.replace("\n", "");

    if (errorCode != ErrorCode::NoError) {
        return connData;
    }

    const QString originalConfig = m_sshSession->getTextFileFromContainer(
            container, credentials, configPath, errorCode);
    if (errorCode != ErrorCode::NoError || originalConfig.isEmpty()) {
        if (errorCode == ErrorCode::NoError) {
            errorCode = ErrorCode::InternalError;
        }
        return connData;
    }

    // Add client to config
    QString configPart = QString("[Peer]\n"
                                 "PublicKey = %1\n"
                                 "PresharedKey = %2\n"
                                 "AllowedIPs = %3/32\n\n")
                                 .arg(connData.clientPubKey, connData.pskKey, connData.clientIP);

    const auto compensateGeneratedPeer = [&](ErrorCode failure) {
        ErrorCode cleanupError = ErrorCode::NoError;
        const QString liveConfig = m_sshSession->getTextFileFromContainer(
                container, credentials, configPath, cleanupError);
        if (cleanupError != ErrorCode::NoError) {
            return cleanupError;
        }
        const PeerRemovalResult removal = removePeerByPublicKey(liveConfig, connData.clientPubKey);
        if (removal.matches > 1) {
            return ErrorCode::InternalError;
        }
        QString cleanedConfig = removal.config;
        if (removal.matches == 0) {
            if (liveConfig == originalConfig) {
                return failure;
            }
            if (!liveConfig.startsWith(originalConfig)) {
                return ErrorCode::InternalError;
            }
            const QString appendedSuffix = liveConfig.mid(originalConfig.size());
            if (appendedSuffix.isEmpty() || !configPart.startsWith(appendedSuffix)) {
                return ErrorCode::InternalError;
            }
            cleanedConfig = originalConfig;
        }
        cleanupError = uploadAndVerify(m_sshSession, credentials, container, cleanedConfig, configPath);
        if (cleanupError == ErrorCode::NoError) {
            cleanupError = syncConfigChecked(m_sshSession, credentials, container, configPath);
        }
        return cleanupError == ErrorCode::NoError ? failure : cleanupError;
    };

    errorCode = m_sshSession->uploadTextFileToContainer(container, credentials, configPart, configPath,
                                                        libssh::ScpOverwriteMode::ScpAppendToExisting);

    if (errorCode != ErrorCode::NoError) {
        errorCode = compensateGeneratedPeer(errorCode);
        return connData;
    }

    ErrorCode readBackError = ErrorCode::NoError;
    const QString liveConfig = m_sshSession->getTextFileFromContainer(
            container, credentials, configPath, readBackError);
    const PeerRemovalResult generatedPeer = removePeerByPublicKey(liveConfig, connData.clientPubKey);
    if (readBackError != ErrorCode::NoError || generatedPeer.matches != 1
            || liveConfig.count(configPart) != 1) {
        errorCode = compensateGeneratedPeer(
                readBackError == ErrorCode::NoError ? ErrorCode::InternalError : readBackError);
        return connData;
    }

    errorCode = syncConfigChecked(m_sshSession, credentials, container, configPath);
    if (errorCode != ErrorCode::NoError) {
        errorCode = compensateGeneratedPeer(errorCode);
    }

    return connData;
}

ProtocolConfig WireguardConfigurator::createConfig(const ServerCredentials &credentials, DockerContainer container,
                                                    const ContainerConfig &containerConfig,
                                                    const DnsSettings &dnsSettings,
                                                    ErrorCode &errorCode)
{
    const WireGuardServerConfig* wireguardServerConfig = nullptr;
    const WireGuardClientConfig* wireguardClientConfig = nullptr;
    const AwgServerConfig* awgServerConfig = nullptr;
    const AwgClientConfig* awgClientConfig = nullptr;
    
    if (auto* wireGuardProtocolConfig = containerConfig.getWireGuardProtocolConfig()) {
        wireguardServerConfig = &wireGuardProtocolConfig->serverConfig;
        if (wireGuardProtocolConfig->clientConfig.has_value()) {
            wireguardClientConfig = &wireGuardProtocolConfig->clientConfig.value();
        }
    } else if (auto* awgProtocolConfig = containerConfig.getAwgProtocolConfig()) {
        awgServerConfig = &awgProtocolConfig->serverConfig;
        if (awgProtocolConfig->clientConfig.has_value()) {
            awgClientConfig = &awgProtocolConfig->clientConfig.value();
        }
    }
    
    amnezia::ScriptVars vars = amnezia::genBaseVars(credentials, container, dnsSettings.primaryDns, dnsSettings.secondaryDns);
    vars.append(amnezia::genProtocolVarsForContainer(container, containerConfig));
    QString scriptData = amnezia::scriptData(m_configTemplate, container);
    QString config = m_sshSession->replaceVars(scriptData, vars);

    // The template lists every possible key, but each parameter is optional -
    // drop the lines whose value came out empty
    static const QRegularExpression emptyValueLine(R"(^\s*\S+\s*=\s*$)");
    auto configTemplateLines = config.split("\n");
    configTemplateLines.removeIf([](const QString &line) { return emptyValueLine.match(line).hasMatch(); });
    config = configTemplateLines.join("\n");

    ConnectionData connData = prepareWireguardConfig(credentials, container, wireguardServerConfig, awgServerConfig, dnsSettings, errorCode);
    if (errorCode != ErrorCode::NoError) {
        return WireGuardProtocolConfig{};
    }

    config.replace("$WIREGUARD_CLIENT_PRIVATE_KEY", connData.clientPrivKey);
    config.replace("$WIREGUARD_CLIENT_IP", connData.clientIP);
    config.replace("$WIREGUARD_SERVER_PUBLIC_KEY", connData.serverPubKey);
    config.replace("$WIREGUARD_PSK", connData.pskKey);

    QString mtu = protocols::wireguard::defaultMtu;
    if (wireguardClientConfig && !wireguardClientConfig->mtu.isEmpty()) {
        mtu = wireguardClientConfig->mtu;
    } else if (awgClientConfig && !awgClientConfig->mtu.isEmpty()) {
        mtu = awgClientConfig->mtu;
    }
    
    WireGuardProtocolConfig protocolConfig;
    if (wireguardServerConfig) {
        protocolConfig.serverConfig = *wireguardServerConfig;
    }
    
    WireGuardClientConfig clientConfig;
    clientConfig.nativeConfig = config;
    clientConfig.hostName = connData.host;
    clientConfig.port = connData.port.toInt();
    clientConfig.clientIp = connData.clientIP;
    clientConfig.clientPrivateKey = connData.clientPrivKey;
    clientConfig.clientPublicKey = connData.clientPubKey;
    clientConfig.serverPublicKey = connData.serverPubKey;
    clientConfig.presharedKey = connData.pskKey;
    clientConfig.clientId = connData.clientPubKey;
    clientConfig.allowedIps = QStringList { "0.0.0.0/0", "::/0" };
    const bool useKeepAliveRange = awgServerConfig && awgServerConfig->hasAwg3Params();
    clientConfig.persistentKeepAlive = useKeepAliveRange ? protocols::awg::defaultPersistentKeepAlive
                                                         : protocols::wireguard::defaultPersistentKeepAlive;
    clientConfig.mtu = mtu;
    clientConfig.isObfuscationEnabled = false;
    
    protocolConfig.setClientConfig(clientConfig);
    
    return protocolConfig;
}

ProtocolConfig WireguardConfigurator::processConfigWithLocalSettings(const ConnectionSettings &settings,
                                                                     ProtocolConfig protocolConfig)
{
    return ConfiguratorBase::processConfigWithLocalSettings(settings, protocolConfig);
}

ProtocolConfig WireguardConfigurator::processConfigWithExportSettings(const ExportSettings &settings,
                                                                      ProtocolConfig protocolConfig)
{
    return ConfiguratorBase::processConfigWithExportSettings(settings, protocolConfig);
}
