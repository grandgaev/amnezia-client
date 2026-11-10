#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include "core/controllers/selfhosted/installController.h"
#include "core/controllers/selfhosted/exportController.h"
#include "core/controllers/selfhosted/usersController.h"
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
    constexpr auto kContainerLabMode = "container-lab";

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

    bool writePrivateFixture(const QDir &directory, const QString &name, const QString &value,
                             CheckContext &checks)
    {
        QFile file(directory.filePath(name));
        const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        const QByteArray data = value.toUtf8();
        const bool written = opened && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && file.write(data) == data.size() && file.flush();
        checks.require(written, QStringLiteral("private fixture %1 is written").arg(name));
        return written;
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

    bool attestContainerLab(SshSession &session, const ServerCredentials &credentials, const QString &nonce,
                            const QString &expectedState, CheckContext &checks)
    {
        QString output;
        const QString command = QStringLiteral("sudo /usr/local/bin/upgrade-e2e-attest %1 %2")
                                        .arg(nonce, expectedState);
        const bool attested = runRemote(session, credentials, command, &output) == ErrorCode::NoError
                && output.trimmed() == QLatin1String("OK");
        if (!attested) {
            const QRegularExpression diagnostic(
                    QStringLiteral("attestation failed: line=[0-9]+ criterion=[a-z-]+"));
            const auto match = diagnostic.match(output);
            if (match.hasMatch()) {
                qWarning().noquote() << match.captured();
            }
        }
        checks.require(attested, "SSH target is the owned disposable container lab");
        return attested;
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
        client.clientId = client.clientPublicKey;
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

    void requireExpectedUsers(const QJsonArray &clients, const QString &adminId, const QString &preservedUserId,
                              bool preservedUserNeedsUpdate, const QString &source, CheckContext &checks)
    {
        const QJsonObject admin = clientWithId(clients, adminId);
        const QJsonObject preservedUser = clientWithId(clients, preservedUserId);
        checks.require(clients.size() == 2, QStringLiteral("%1 has exactly two users").arg(source));
        checks.require(!admin.isEmpty() && !preservedUser.isEmpty(),
                       QStringLiteral("%1 contains both WireGuard peer identities").arg(source));
        const QJsonObject adminData = admin.value(configKey::userData).toObject();
        const QJsonObject preservedUserData = preservedUser.value(configKey::userData).toObject();
        checks.require(adminData.value(configKey::clientName).toString() == QLatin1String("Administrator"),
                       QStringLiteral("%1 preserves the admin name").arg(source));
        checks.require(preservedUserData.value(configKey::clientName).toString() == QLatin1String("Preserved user"),
                       QStringLiteral("%1 preserves the second user name").arg(source));
        checks.require(!adminData.value(configKey::needsConfigUpdate).toBool(false),
                       QStringLiteral("%1 keeps the admin config current").arg(source));
        checks.require(preservedUserData.value(configKey::needsConfigUpdate).toBool(false)
                               == preservedUserNeedsUpdate,
                       QStringLiteral("%1 has the expected second-user update state").arg(source));
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
            || scenario == QLatin1String("reissue")
            || scenario == QLatin1String("rollback") || scenario == QLatin1String("recovery");
    checks.require(validScenario, "requested scenario is supported");

    const QString host = requiredEnvironment("UPGRADE_E2E_HOST", checks);
    const QHostAddress hostAddress(host);
    const bool githubHosted = qEnvironmentVariable("GITHUB_ACTIONS") == QLatin1String("true")
            && qEnvironmentVariable("RUNNER_ENVIRONMENT") == QLatin1String("github-hosted");
    const bool containerLab = qEnvironmentVariable("UPGRADE_E2E_EXECUTION") == QLatin1String(kContainerLabMode);
    const QString labNonce = qEnvironmentVariable("UPGRADE_E2E_LAB_NONCE").trimmed();
    checks.require(qEnvironmentVariable("UPGRADE_E2E_DISPOSABLE") == QLatin1String("1"),
                   "upgrade integration is explicitly marked disposable");
    checks.require(githubHosted != containerLab,
                   "exactly one supported disposable execution environment is selected");
    if (containerLab) {
        static const QRegularExpression noncePattern(QStringLiteral("^[0-9a-f]{32,64}$"));
        checks.require(noncePattern.match(labNonce).hasMatch(), "container lab nonce is valid");
    }
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
    const QString adminClientId = trimmedFile(fixture, QStringLiteral("admin_public.key"), checks);
    const QString preservedUserId = trimmedFile(fixture, QStringLiteral("user_public.key"), checks);
    checks.require(!adminClientId.isEmpty() && !preservedUserId.isEmpty() && adminClientId != preservedUserId,
                   "fixture users have distinct WireGuard public-key identities");

    ServerCredentials credentials { host, user, password, port };
    checks.require(credentials.isValid(), "SSH credentials are valid");

    SshSession inspectionSession;
    if (containerLab
        && !attestContainerLab(inspectionSession, credentials, labNonce,
                               scenario == QLatin1String("recovery") ? QStringLiteral("recovery")
                                                                      : QStringLiteral("active"),
                               checks)) {
        return 2;
    }

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

    if (scenario == QLatin1String("recovery")) {
        checks.require(!containerExists(inspectionSession, credentials, QString::fromLatin1(kContainerName), true),
                       "interrupted fixture has no live replacement");
        checks.require(containerExists(inspectionSession, credentials, QString::fromLatin1(kParkedContainerName), false),
                       "interrupted fixture has a parked original container");
    }

    const bool upgradesProtocol = scenario == QLatin1String("protocol") || scenario == QLatin1String("reissue");
    const ContainerUpgradeMode mode = upgradesProtocol ? ContainerUpgradeMode::UpgradeProtocol
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
    if (containerLab) {
        attestContainerLab(inspectionSession, credentials, labNonce, QStringLiteral("active"), checks);
    }
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
    checks.require(originalSnapshot.peerCount == 2 && originalSnapshot.peersBlock.contains(adminClientId)
                           && originalSnapshot.peersBlock.contains(preservedUserId),
                   "fixture client identities match both WireGuard peer public keys");
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
    const QJsonArray clientsBeforeUsersUpdate = QJsonDocument::fromJson(currentClientsTable).array();
    const bool preservedUserNeedsUpdate = upgradesProtocol;
    requireExpectedUsers(clientsBeforeUsersUpdate, adminClientId, preservedUserId, preservedUserNeedsUpdate,
                         QStringLiteral("upgraded client table"), checks);

    UsersController users(&servers);
    QJsonArray usersFromController;
    QObject::connect(&users, &UsersController::clientsUpdated, &app,
                     [&](const QJsonArray &clients) { usersFromController = clients; });
    checks.require(users.updateClients(serverId, DockerContainer::Awg2) == ErrorCode::NoError,
                   "users controller reads the upgraded container through SSH");
    requireExpectedUsers(usersFromController, adminClientId, preservedUserId, preservedUserNeedsUpdate,
                         QStringLiteral("users controller result"), checks);
    const QByteArray clientsTableAfterUsersUpdate = readContainerFile("/opt/amnezia/awg/clientsTable");
    const QJsonArray clientsAfterUsersUpdate = QJsonDocument::fromJson(clientsTableAfterUsersUpdate).array();
    requireExpectedUsers(clientsAfterUsersUpdate, adminClientId, preservedUserId, preservedUserNeedsUpdate,
                         QStringLiteral("post-update client table"), checks);

    const auto persisted = servers.selfHostedAdminConfig(serverId);
    checks.require(persisted.has_value(), "server remains in the repository");
    if (persisted) {
        const ContainerConfig persistedContainer = persisted->containerConfig(DockerContainer::Awg2);
        if (upgradesProtocol) {
            const QJsonArray clients = clientsAfterUsersUpdate;
            const QJsonObject adminClient = clientWithId(clients, adminClientId);
            const QJsonObject otherClient = clientWithId(clients, preservedUserId);
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
                                   && upgraded->clientConfig->clientId == adminClientId
                                   && upgraded->clientConfig->clientPublicKey == adminClientId,
                           "admin identity is preserved locally");
            checks.require(upgraded && upgraded->clientConfig.has_value()
                                   && upgraded->clientConfig->nativeConfig.contains(QLatin1String("DNS = 1.1.1.1")),
                           "admin config is re-rendered with its DNS setting");
            if (upgraded && upgraded->clientConfig.has_value()) {
                writePrivateFixture(fixture, QStringLiteral("upgraded-admin.conf"),
                                    upgraded->clientConfig->nativeConfig, checks);
            }
        } else {
            checks.require(currentClientsTable == originalClientsTable, "client table is preserved byte for byte");
            checks.require(persistedContainer.toJson() == storedContainerBeforeUpgrade, "local config remains unchanged");
        }
    }

    if (scenario == QLatin1String("reissue") && checks.failures() == 0) {
        ExportController exporter(&servers, &appSettings);
        exporter.setReplaceClientHandler(
                [&](const QString &id, const QString &oldId, const QString &newId,
                    const QString &name, DockerContainer container) {
                    return users.replaceClient(id, oldId, newId, name, container);
                });
        exporter.setClientReplacementSnapshotHandlers(
                [&](const QString &id, DockerContainer container, QString &config, QByteArray &table) {
                    return users.snapshotClientReplacement(id, container, config, table);
                },
                [&](const QString &id, DockerContainer container, const QString &config, const QByteArray &table) {
                    return users.restoreClientReplacement(id, container, config, table);
                });
        const auto exported = exporter.reissueConnectionConfig(
                serverId, static_cast<int>(DockerContainer::Awg2), preservedUserId,
                QStringLiteral("Preserved user"));
        checks.require(exported.errorCode == ErrorCode::NoError && exported.config.startsWith(QLatin1String("vpn://")),
                       "the production Update config flow completes successfully");
        const QByteArray decoded = qUncompress(QByteArray::fromBase64(
                exported.config.mid(6).toUtf8(), QByteArray::Base64UrlEncoding));
        const QJsonDocument exportedDocument = QJsonDocument::fromJson(decoded);
        checks.require(exportedDocument.isObject(), "the reissued connection config is valid");
        const auto exportedServer = SelfHostedAdminServerConfig::fromJson(exportedDocument.object());
        const ContainerConfig exportedContainer = exportedServer.containerConfig(DockerContainer::Awg2);
        const AwgProtocolConfig *exportedAwg = exportedContainer.getAwgProtocolConfig();
        if (exportedAwg && exportedAwg->clientConfig.has_value()) {
            const auto &newClient = *exportedAwg->clientConfig;
            checks.require(!newClient.clientPublicKey.isEmpty() && newClient.clientPublicKey != preservedUserId
                                   && newClient.clientPublicKey != adminClientId,
                           "the reissued user has a fresh unique WireGuard identity");
            const auto finalConfig = readContainerFile(protocols::awg::serverConfigPath);
            const auto finalSnapshot = parseAwgInterfaceSnapshot(QString::fromUtf8(finalConfig));
            checks.require(finalSnapshot.isValid && finalSnapshot.peerCount == 2
                                   && finalSnapshot.peersBlock.contains(adminClientId)
                                   && finalSnapshot.peersBlock.contains(newClient.clientPublicKey)
                                   && !finalSnapshot.peersBlock.contains(preservedUserId),
                           "the final server config preserves admin and replaces only the stale user");
            const QJsonArray finalClients = QJsonDocument::fromJson(
                    readContainerFile("/opt/amnezia/awg/clientsTable")).array();
            requireExpectedUsers(finalClients, adminClientId, newClient.clientPublicKey, false,
                                 QStringLiteral("reissued client table"), checks);
            QString livePeers;
            checks.require(runRemote(inspectionSession, credentials,
                                     QStringLiteral("sudo docker exec amnezia-awg2 awg show awg0 peers"),
                                     &livePeers) == ErrorCode::NoError,
                           "the final live peer list is readable");
            const auto peerIds = livePeers.split('\n', Qt::SkipEmptyParts);
            checks.require(peerIds.size() == 2 && peerIds.contains(adminClientId)
                                   && peerIds.contains(newClient.clientPublicKey)
                                   && !peerIds.contains(preservedUserId),
                           "the live interface has confirmed stale-key revocation");
            writePrivateFixture(fixture, QStringLiteral("reissued-user.conf"), newClient.nativeConfig, checks);
            writePrivateFixture(fixture, QStringLiteral("reissued-user-private.key"), newClient.clientPrivateKey, checks);
        } else {
            checks.require(false, "the reissued config contains an AWG client");
        }
        const auto adminAfterReissue = servers.selfHostedAdminConfig(serverId);
        checks.require(adminAfterReissue && persisted && adminAfterReissue->toJson() == persisted->toJson(),
                       "user config replacement leaves the stored admin config unchanged");
    }

    settings.clearSettings();
    qInfo() << "Scenario" << scenario << "completed with" << checks.failures() << "failures";
    return checks.failures() == 0 ? 0 : 1;
}
