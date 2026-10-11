#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "core/controllers/selfhosted/installController.h"
#include "core/installers/awgInstaller.h"
#include "core/models/protocols/awgProtocolConfig.h"
#include "core/models/selfhosted/selfHostedAdminServerConfig.h"
#include "core/repositories/secureAppSettingsRepository.h"
#include "core/repositories/secureServersRepository.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/constants/protocolConstants.h"
#include "core/utils/selfhosted/awgContainerUpgrade.h"
#include "core/utils/selfhosted/sshSession.h"
#include "secureQSettings.h"

using namespace amnezia;

namespace
{
    constexpr auto kServerId = "upgrade-e2e";
    constexpr auto kContainerName = "amnezia-awg2";
    constexpr auto kParkedContainerName = "amnezia-awg2-pre-upgrade";

    class CheckContext
    {
    public:
        void require(bool condition, const QString &message)
        {
            if (condition) {
                qInfo().noquote() << "PASS:" << message;
                return;
            }
            qCritical().noquote() << "FAIL:" << message;
            ++m_failures;
        }

        int failures() const
        {
            return m_failures;
        }

    private:
        int m_failures = 0;
    };

    QString requiredEnvironment(const char *name, CheckContext &checks)
    {
        const QString value = qEnvironmentVariable(name).trimmed();
        checks.require(!value.isEmpty(),
                       QStringLiteral("environment variable %1 is configured").arg(QString::fromLatin1(name)));
        return value;
    }

    QByteArray readFile(const QDir &directory, const QString &name, CheckContext &checks)
    {
        QFile file(directory.filePath(name));
        checks.require(file.open(QIODevice::ReadOnly), QStringLiteral("fixture file %1 is readable").arg(name));
        return file.isOpen() ? file.readAll() : QByteArray();
    }

    QString trimmedFile(const QDir &directory, const QString &name, CheckContext &checks)
    {
        return QString::fromUtf8(readFile(directory, name, checks)).trimmed();
    }

    ErrorCode runRemote(SshSession &session, const ServerCredentials &credentials, const QString &command,
                        QString *output = nullptr)
    {
        QString combined;
        auto collect = [&](const QString &data, libssh::Client &) {
            combined += data;
            return ErrorCode::NoError;
        };
        const ErrorCode result = session.runScript(credentials, command, collect, collect);
        if (output) {
            *output = combined;
        }
        return result;
    }

    bool containerExists(SshSession &session, const ServerCredentials &credentials, const QString &name, bool runningOnly)
    {
        QString output;
        const QString command = runningOnly ? QStringLiteral("sudo docker ps --format '{{.Names}}'")
                                            : QStringLiteral("sudo docker ps -a --format '{{.Names}}'");
        if (runRemote(session, credentials, command, &output) != ErrorCode::NoError) {
            return false;
        }
        return output.split('\n', Qt::SkipEmptyParts).contains(name);
    }

    ContainerConfig fixtureContainerConfig(const QDir &fixture, const QString &host, CheckContext &checks)
    {
        AwgInstaller installer;
        ContainerConfig config = installer.generateConfig(DockerContainer::Awg2, 51820, TransportProto::Udp);
        AwgProtocolConfig *awg = config.getAwgProtocolConfig();
        checks.require(awg != nullptr, "generated AWG container config is valid");
        if (!awg) {
            return config;
        }

        awg->serverConfig.protocolVersion = protocols::awg::awgV2;
        awg->serverConfig.subnetAddress = QStringLiteral("10.8.1.1");
        awg->serverConfig.subnetCidr = QStringLiteral("24");
        awg->serverConfig.junkPacketCount = QStringLiteral("5");
        awg->serverConfig.junkPacketMinSize = QStringLiteral("10");
        awg->serverConfig.junkPacketMaxSize = QStringLiteral("50");
        awg->serverConfig.initPacketJunkSize = QStringLiteral("24");
        awg->serverConfig.responsePacketJunkSize = QStringLiteral("64");
        awg->serverConfig.cookieReplyPacketJunkSize = QStringLiteral("18");
        awg->serverConfig.transportPacketJunkSize = QStringLiteral("7");
        awg->serverConfig.initPacketMagicHeader = QStringLiteral("100000");
        awg->serverConfig.responsePacketMagicHeader = QStringLiteral("300000");
        awg->serverConfig.underloadPacketMagicHeader = QStringLiteral("500000");
        awg->serverConfig.transportPacketMagicHeader = QStringLiteral("700000");
        awg->serverConfig.headerProtectionKey.clear();
        awg->serverConfig.contentPaddingAddition.clear();
        awg->serverConfig.rekeyAfterTime.clear();
        awg->serverConfig.rekeyTimeout.clear();
        awg->serverConfig.rejectAfterTime.clear();
        awg->serverConfig.keepaliveTimeout.clear();
        awg->serverConfig.maxHandshakeAttempts.clear();
        awg->serverConfig.randomTrailers.clear();
        awg->serverConfig.disableCookies.clear();

        AwgClientConfig client;
        client.nativeConfig = QString::fromUtf8(readFile(fixture, QStringLiteral("admin.conf"), checks));
        client.hostName = host;
        client.port = 51820;
        client.clientIp = QStringLiteral("10.8.1.2");
        client.clientPrivateKey = trimmedFile(fixture, QStringLiteral("admin_private.key"), checks);
        client.clientPublicKey = trimmedFile(fixture, QStringLiteral("admin_public.key"), checks);
        client.serverPublicKey = trimmedFile(fixture, QStringLiteral("server_public.key"), checks);
        client.presharedKey = trimmedFile(fixture, QStringLiteral("preshared.key"), checks);
        client.clientId = QStringLiteral("admin-client");
        client.allowedIps = { QStringLiteral("0.0.0.0/0"), QStringLiteral("::/0") };
        client.persistentKeepAlive = QStringLiteral("25");
        awg->clientConfig = client;
        return config;
    }

    QJsonObject clientWithId(const QJsonArray &clients, const QString &id)
    {
        for (const QJsonValue &value : clients) {
            const QJsonObject client = value.toObject();
            if (client.value(configKey::clientId).toString() == id) {
                return client;
            }
        }
        return {};
    }
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    CheckContext checks;
    if (app.arguments().size() != 2) {
        qCritical() << "Usage: test_container_upgrade_integration"
                       " <refresh|protocol|rollback|recovery>";
        return 2;
    }
    const QString scenario = app.arguments().at(1);
    const bool validScenario = scenario == QLatin1String("refresh") || scenario == QLatin1String("protocol")
            || scenario == QLatin1String("rollback") || scenario == QLatin1String("recovery");
    checks.require(validScenario, "requested scenario is supported");

    const QString host = requiredEnvironment("UPGRADE_E2E_HOST", checks);
    const QHostAddress hostAddress(host);
    checks.require(qEnvironmentVariable("GITHUB_ACTIONS") == QLatin1String("true")
                           && qEnvironmentVariable("RUNNER_ENVIRONMENT") == QLatin1String("github-hosted")
                           && qEnvironmentVariable("UPGRADE_E2E_DISPOSABLE") == QLatin1String("1"),
                   "upgrade integration runs only on an explicitly disposable GitHub-hosted runner");
    checks.require(host == QLatin1String("localhost") || (!hostAddress.isNull() && hostAddress.isLoopback()),
                   "upgrade integration SSH target is loopback");
    if (checks.failures() != 0 || !validScenario) {
        return 2;
    }

    const QString fixturePath = requiredEnvironment("UPGRADE_E2E_FIXTURE_DIR", checks);
    const QString user = requiredEnvironment("UPGRADE_E2E_USER", checks);
    const QString password = requiredEnvironment("UPGRADE_E2E_PASSWORD", checks);
    bool portOk = false;
    const int port = qEnvironmentVariableIntValue("UPGRADE_E2E_PORT", &portOk);
    checks.require(portOk && port > 0, "SSH port is valid");
    if (checks.failures() != 0 || !validScenario) {
        return 2;
    }

    const QDir fixture(fixturePath);
    checks.require(fixture.exists(), "fixture directory exists");
    const QByteArray originalServerConfig = readFile(fixture, QStringLiteral("awg0.conf"), checks);
    const QByteArray originalPrivateKey = readFile(fixture, QStringLiteral("server_private.key"), checks);
    const QByteArray originalPublicKey = readFile(fixture, QStringLiteral("server_public.key"), checks);
    const QByteArray originalPresharedKey = readFile(fixture, QStringLiteral("preshared.key"), checks);
    const QByteArray originalClientsTable = readFile(fixture, QStringLiteral("clientsTable"), checks);

    ServerCredentials credentials { host, user, password, port };
    checks.require(credentials.isValid(), "SSH credentials are valid");

    SecureQSettings settings(
            QStringLiteral("AmneziaVPN-Upgrade-E2E-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            QStringLiteral("upgrade-e2e"), nullptr, false);
    settings.clearSettings();
    SecureServersRepository servers(&settings);
    SecureAppSettingsRepository appSettings(&settings);
    InstallController controller(&servers, &appSettings);

    const ContainerConfig originalContainer = fixtureContainerConfig(fixture, host, checks);
    SelfHostedAdminServerConfig server;
    server.description = QStringLiteral("Disposable upgrade e2e");
    server.hostName = host;
    server.userName = user;
    server.password = password;
    server.port = port;
    server.defaultContainer = DockerContainer::Awg2;
    server.containers.insert(DockerContainer::Awg2, originalContainer);
    const QString serverId = servers.addServer(QString::fromLatin1(kServerId), server.toJson(),
                                               serverConfigUtils::ConfigType::SelfHostedAdmin);
    const auto storedBeforeUpgrade = servers.selfHostedAdminConfig(serverId);
    checks.require(storedBeforeUpgrade.has_value(), "fixture server is stored in the real repository");
    const QJsonObject storedContainerBeforeUpgrade =
            storedBeforeUpgrade ? storedBeforeUpgrade->containerConfig(DockerContainer::Awg2).toJson() : QJsonObject();

    SshSession inspectionSession;
    if (scenario == QLatin1String("recovery")) {
        checks.require(!containerExists(inspectionSession, credentials, QString::fromLatin1(kContainerName), true),
                       "interrupted fixture has no live replacement");
        checks.require(containerExists(inspectionSession, credentials, QString::fromLatin1(kParkedContainerName), false),
                       "interrupted fixture has a parked original container");
    }

    const ContainerUpgradeMode mode = scenario == QLatin1String("protocol") ? ContainerUpgradeMode::UpgradeProtocol
                                                                            : ContainerUpgradeMode::RefreshSoftware;
    const ErrorCode result = controller.upgradeContainer(serverId, DockerContainer::Awg2, mode);
    if (scenario == QLatin1String("rollback")) {
        checks.require(result == ErrorCode::ServerPortAlreadyAllocatedError,
                       QStringLiteral("injected Docker conflict reaches rollback with error code %1")
                               .arg(static_cast<int>(result)));
    } else {
        checks.require(result == ErrorCode::NoError,
                       QStringLiteral("upgrade succeeds with error code %1").arg(static_cast<int>(result)));
    }

    checks.require(containerExists(inspectionSession, credentials, QString::fromLatin1(kContainerName), true),
                   "the service container is running");
    checks.require(!containerExists(inspectionSession, credentials, QString::fromLatin1(kParkedContainerName), false),
                   "no parked container remains");
    QString peerCountOutput;
    checks.require(runRemote(inspectionSession, credentials,
                             QStringLiteral("sudo docker exec amnezia-awg2 sh -c "
                                            "'awg show awg0 peers | wc -l'"),
                             &peerCountOutput)
                                   == ErrorCode::NoError
                           && peerCountOutput.trimmed() == QLatin1String("2"),
                   "the restored or upgraded interface has both live peers");

    ErrorCode readError = ErrorCode::NoError;
    const QByteArray currentServerConfig = inspectionSession.getTextFileFromContainer(
            DockerContainer::Awg2, credentials, QString::fromLatin1(protocols::awg::serverConfigPath), readError);
    checks.require(readError == ErrorCode::NoError, "server config is readable through SSH");
    const AwgInterfaceSnapshot originalSnapshot = parseAwgInterfaceSnapshot(QString::fromUtf8(originalServerConfig));
    const AwgInterfaceSnapshot currentSnapshot = parseAwgInterfaceSnapshot(QString::fromUtf8(currentServerConfig));
    checks.require(currentSnapshot.isValid, "upgraded server config is valid");
    checks.require(currentSnapshot.privateKey == originalSnapshot.privateKey, "server private key is preserved");
    checks.require(currentSnapshot.address == originalSnapshot.address, "server subnet is preserved");
    checks.require(currentSnapshot.listenPort == originalSnapshot.listenPort, "server port is preserved");
    checks.require(currentSnapshot.peersBlock == originalSnapshot.peersBlock, "all peer blocks are preserved verbatim");

    auto readContainerFile = [&](const char *path) {
        ErrorCode error = ErrorCode::NoError;
        const QByteArray value = inspectionSession.getTextFileFromContainer(DockerContainer::Awg2, credentials,
                                                                            QString::fromLatin1(path), error);
        checks.require(error == ErrorCode::NoError,
                       QStringLiteral("container file %1 is readable").arg(QString::fromLatin1(path)));
        return value;
    };
    checks.require(readContainerFile(protocols::awg::serverPrivateKeyPath) == originalPrivateKey,
                   "private-key file is preserved");
    checks.require(readContainerFile(protocols::awg::serverPublicKeyPath) == originalPublicKey,
                   "public-key file is preserved");
    checks.require(readContainerFile(protocols::awg::serverPskKeyPath) == originalPresharedKey,
                   "preshared-key file is preserved");
    const QByteArray currentClientsTable = readContainerFile("/opt/amnezia/awg/clientsTable");

    const auto persisted = servers.selfHostedAdminConfig(serverId);
    checks.require(persisted.has_value(), "server remains in the repository");
    if (persisted) {
        const ContainerConfig persistedContainer = persisted->containerConfig(DockerContainer::Awg2);
        if (scenario == QLatin1String("protocol")) {
            const QJsonArray clients = QJsonDocument::fromJson(currentClientsTable).array();
            checks.require(clients.size() == 2, "protocol upgrade keeps both users");
            const QJsonObject adminClient = clientWithId(clients, QStringLiteral("admin-client"));
            const QJsonObject otherClient = clientWithId(clients, QStringLiteral("user-client"));
            checks.require(!adminClient.isEmpty(), "admin client is preserved");
            checks.require(!otherClient.isEmpty(), "other user is preserved");
            checks.require(
                    !adminClient.value(configKey::userData).toObject().value(configKey::needsConfigUpdate).toBool(false),
                    "admin client remains current");
            checks.require(
                    otherClient.value(configKey::userData).toObject().value(configKey::needsConfigUpdate).toBool(false),
                    "other user is marked for a refreshed config");
            const AwgProtocolConfig *upgraded = persistedContainer.getAwgProtocolConfig();
            checks.require(upgraded && upgraded->serverConfig.hasAwg3Params(), "new protocol parameters are persisted");
            checks.require(upgraded && upgraded->clientConfig.has_value()
                                   && upgraded->clientConfig->clientId == QLatin1String("admin-client"),
                           "admin identity is preserved locally");
            checks.require(upgraded && upgraded->clientConfig.has_value()
                                   && upgraded->clientConfig->nativeConfig.contains(QLatin1String("DNS = 1.1.1.1")),
                           "admin config is re-rendered with its DNS setting");
        } else {
            checks.require(currentClientsTable == originalClientsTable, "client table is preserved byte for byte");
            checks.require(persistedContainer.toJson() == storedContainerBeforeUpgrade, "local config remains unchanged");
        }
    }

    settings.clearSettings();
    qInfo() << "Scenario" << scenario << "completed with" << checks.failures() << "failures";
    return checks.failures() == 0 ? 0 : 1;
}
