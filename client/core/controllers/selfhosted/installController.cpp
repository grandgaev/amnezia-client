#include "installController.h"

#include "core/models/protocolConfig.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>
#include <QtConcurrent>

#include "core/configurators/configuratorBase.h"
#include "core/configurators/xrayConfigurator.h"
#include "core/utils/containerEnum.h"
#include "core/utils/containers/containerUtils.h"
#include "core/utils/protocolEnum.h"
#include "core/utils/selfhosted/sshSession.h"
#include "core/installers/awgInstaller.h"
#include "core/installers/installerBase.h"
#include "core/installers/openvpnInstaller.h"
#include "core/installers/sftpInstaller.h"
#include "core/installers/socks5Installer.h"
#include "core/installers/mtProxyInstaller.h"
#include "core/installers/telemtInstaller.h"
#include "core/installers/tProxyInstaller.h"
#include "core/installers/torInstaller.h"
#include "core/installers/wireguardInstaller.h"
#include "core/installers/xrayInstaller.h"
#include "core/utils/networkUtilities.h"
#include "core/utils/api/apiUtils.h"
#include "core/repositories/secureServersRepository.h"
#include "core/repositories/secureAppSettingsRepository.h"
#include "core/utils/selfhosted/scriptsRegistry.h"
#include "core/utils/selfhosted/sshClient.h"
#include "logger.h"
#include "core/utils/protocolEnum.h"
#include "core/protocols/protocolUtils.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/constants/protocolConstants.h"
#include "core/models/containerConfig.h"
#include "core/models/protocols/mtProxyProtocolConfig.h"
#include "core/models/protocols/awgProtocolConfig.h"
#include "ui/models/protocols/wireguardConfigModel.h"
#include "core/utils/utilities.h"
#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QSysInfo>
#ifdef Q_OS_WINDOWS
    #include <windows.h>
#endif

using namespace amnezia;
using namespace ProtocolUtils;

namespace
{
    Logger logger("InstallController");

    bool dockerDaemonContainerMissing(const QString &out, const QString &containerDockerName)
    {
        if (!out.contains(QLatin1String("Error response from daemon"), Qt::CaseInsensitive)) {
            return false;
        }
        if (out.contains(QLatin1String("No such container"), Qt::CaseInsensitive)
            && out.contains(containerDockerName, Qt::CaseInsensitive)) {
            return true;
        }
        if (out.size() < 700 && out.contains(QLatin1String("is not running"), Qt::CaseInsensitive)) {
            return true;
        }
        return false;
    }

    QString buildRemoveContainerScript(const amnezia::ScriptVars &vars, bool removeDataVolume)
    {
        QString script = SshSession::replaceVars(amnezia::scriptData(SharedScriptType::remove_container), vars);
        if (removeDataVolume) {
            script += QLatin1String("\nsudo docker volume rm -f $CONTAINER_NAME-data 2>/dev/null || true");
            script = SshSession::replaceVars(script, vars);
        }
        return script;
    }
}

InstallController::InstallController(SecureServersRepository *serversRepository,
                                     SecureAppSettingsRepository* appSettingsRepository,
                                     QObject *parent)
    : QObject(parent),
      m_serversRepository(serversRepository),
      m_appSettingsRepository(appSettingsRepository),
      m_cancelInstallation(false)
{
}

InstallController::~InstallController()
{
    stopAllSftpMounts();
}

ErrorCode InstallController::setupContainer(const ServerCredentials &credentials, DockerContainer container, ContainerConfig &config,
                                            bool isUpdate)
{
    SshSession sshSession;
    ErrorCode e = ErrorCode::NoError;

    e = isUserInSudo(credentials, sshSession);
    if (e)
        return e;

    e = isServerDpkgBusy(credentials, sshSession);
    if (e)
        return e;

    e = installDockerWorker(credentials, container, sshSession);
    if (e)
        return e;

    if (!isUpdate) {
        e = isServerPortBusy(credentials, container, config, sshSession);
        if (e)
            return e;
    }

    e = prepareHostWorker(credentials, container, sshSession);
    if (e)
        return e;

    const amnezia::ScriptVars removeContainerVars =
            amnezia::genBaseVars(credentials, container, QString(), QString());
    const bool removeDataVolume = !isUpdate && (container == DockerContainer::MtProxy
            || container == DockerContainer::Telemt || container == DockerContainer::TProxy);
    sshSession.runScript(credentials, buildRemoveContainerScript(removeContainerVars, removeDataVolume));

    e = buildContainerWorker(credentials, container, config, sshSession);
    if (e)
        return e;

    e = runContainerWorker(credentials, container, config, sshSession);
    if (e)
        return e;

    e = configureContainerWorker(credentials, container, config, sshSession);
    if (e)
        return e;

    if (container == DockerContainer::Xray || container == DockerContainer::SSXray) {
        DnsSettings dnsSettings = { m_appSettingsRepository->primaryDns(), m_appSettingsRepository->secondaryDns() };
        XrayConfigurator xrayConfigurator(&sshSession);
        e = xrayConfigurator.writeServerConfigForSetup(credentials, container, config, dnsSettings);
        if (e)
            return e;
    }

    setupServerFirewall(credentials, sshSession);

    return startupContainerWorker(credentials, container, config, sshSession);
}

ErrorCode InstallController::updateServerConfig(const QString &serverId, DockerContainer container, const ContainerConfig &oldConfig,
                                                ContainerConfig &newConfig)
{
    // RandomTrailers with ranged H1-H4 and unequal S1-S4 makes amneziawg-go misclassify
    // data packets as handshakes: never apply that combination to a server.
    if (auto *awgConfig = newConfig.getAwgProtocolConfig()) {
        if (awgConfig->serverConfig.hasUnsafeRandomTrailersCombo()) {
            qWarning() << "InstallController::updateServerConfig: RandomTrailers disabled (ranged headers with unequal padding)";
            awgConfig->serverConfig.normalizeRandomTrailersCombo();
        }
    }

    if (!isUpdateDockerContainerRequired(container, oldConfig, newConfig)) {
        auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
        if (!adminConfig.has_value()) {
            return ErrorCode::InternalError;
        }
        if (container == DockerContainer::MtProxy) {
            ServerCredentials credentials = adminConfig->credentials();
            SshSession sshSession;
            MtProxyInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, newConfig);
        } else if (container == DockerContainer::Telemt) {
            ServerCredentials credentials = adminConfig->credentials();
            SshSession sshSession;
            TelemtInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, newConfig);
        } else if (container == DockerContainer::TProxy) {
            ServerCredentials credentials = adminConfig->credentials();
            SshSession sshSession;
            TProxyInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, newConfig);
        }
        adminConfig->updateContainerConfig(container, newConfig);
        m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
        return ErrorCode::NoError;
    }

    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;

    bool reinstallRequired = isReinstallContainerRequired(container, oldConfig, newConfig);
    qDebug() << "InstallController::updateServerConfig for container" << container << "reinstall required is" << reinstallRequired;

    ErrorCode errorCode = ErrorCode::NoError;
    if (reinstallRequired) {
        errorCode = setupContainer(credentials, container, newConfig, true);

        // The protocol version follows from the parameters the server now runs with
        // (a reinstall keeps them, so an AWG 2.0 parameter set stays 2.0).
        if (errorCode == ErrorCode::NoError && container == DockerContainer::Awg2) {
            if (auto* awgConfig = newConfig.getAwgProtocolConfig()) {
                awgConfig->serverConfig.protocolVersion = awgConfig->serverProtocolVersion();
            }
        }
    } else if (container != DockerContainer::Xray && container != DockerContainer::SSXray) {
        errorCode = configureContainerWorker(credentials, container, newConfig, sshSession);
        if (errorCode == ErrorCode::NoError) {
            errorCode = startupContainerWorker(credentials, container, newConfig, sshSession);
        }

        if (errorCode == ErrorCode::NoError
            && (container == DockerContainer::MtProxy || container == DockerContainer::Telemt
                || container == DockerContainer::TProxy)) {
            const QString containerName = ContainerUtils::containerToString(container);
            errorCode = sshSession.runScript(credentials, "sudo docker restart " + containerName);
        }
    }

    if (errorCode == ErrorCode::NoError) {
        if (container == DockerContainer::MtProxy) {
            MtProxyInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, newConfig);
        } else if (container == DockerContainer::Telemt) {
            TelemtInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, newConfig);
        } else if (container == DockerContainer::TProxy) {
            TProxyInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, newConfig);
        }
        if (reinstallRequired) {
            clearCachedProfile(serverId, container);
        }
        adminConfig->updateContainerConfig(container, newConfig);
        m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
    }

    return errorCode;
}

ErrorCode InstallController::updateClientConfig(const QString &serverId, DockerContainer container, ContainerConfig &newConfig)
{
    switch (m_serversRepository->serverKind(serverId)) {
    case serverConfigUtils::ConfigType::SelfHostedAdmin: {
        auto config = m_serversRepository->selfHostedAdminConfig(serverId);
        if (!config.has_value()) {
            return ErrorCode::InternalError;
        }
        config->updateContainerConfig(container, newConfig);
        m_serversRepository->editServer(serverId, config->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
        return ErrorCode::NoError;
    }
    case serverConfigUtils::ConfigType::SelfHostedUser: {
        auto config = m_serversRepository->selfHostedUserConfig(serverId);
        if (!config.has_value()) {
            return ErrorCode::InternalError;
        }
        config->updateContainerConfig(container, newConfig);
        m_serversRepository->editServer(serverId, config->toJson(), serverConfigUtils::ConfigType::SelfHostedUser);
        return ErrorCode::NoError;
    }
    case serverConfigUtils::ConfigType::Native: {
        auto config = m_serversRepository->nativeConfig(serverId);
        if (!config.has_value()) {
            return ErrorCode::InternalError;
        }
        config->updateContainerConfig(container, newConfig);
        m_serversRepository->editServer(serverId, config->toJson(), serverConfigUtils::ConfigType::Native);
        return ErrorCode::NoError;
    }
    default:
        return ErrorCode::InternalError;
    }
}

namespace
{
    QString upgradeParkedName(const QString &containerName)
    {
        return containerName + QLatin1String("-pre-upgrade");
    }

    // "docker ps -a --format '{{.Names}} {{.Status}}'" line for an exact container name.
    bool dockerContainerLineIsRunning(const QString &line)
    {
        return line.contains(QLatin1String("Up "), Qt::CaseInsensitive) || line.trimmed().endsWith(QLatin1String("Up"));
    }
}

QStringList InstallController::upgradeStateFilePaths(DockerContainer container)
{
    // Table of the state files a RefreshSoftware/UpgradeProtocol upgrade must snapshot from the
    // running container and restore onto the rebuilt one, verbatim. Extend this table (and, for
    // an AmneziaWG-shaped protocol bump, the migration branch in upgradeContainer()) when a new
    // container revision needs the same treatment.
    // The legacy AmneziaWG container (awg_legacy, wg0.conf) is not supported: it can no
    // longer be installed and its state lives in other files.
    if (container == DockerContainer::Awg2) {
        return { QString::fromLatin1(protocols::awg::serverConfigPath),
                 QString::fromLatin1(protocols::awg::serverPrivateKeyPath),
                 QString::fromLatin1(protocols::awg::serverPublicKeyPath),
                 QString::fromLatin1(protocols::awg::serverPskKeyPath),
                 QString("/opt/amnezia/%1/clientsTable").arg(ContainerUtils::containerTypeToString(container)) };
    }
    if (container == DockerContainer::Xray || container == DockerContainer::SSXray) {
        return { QString::fromLatin1(protocols::xray::serverConfigPath),
                 QString::fromLatin1(protocols::xray::uuidPath),
                 QString::fromLatin1(protocols::xray::PublicKeyPath),
                 QString::fromLatin1(protocols::xray::PrivateKeyPath),
                 QString::fromLatin1(protocols::xray::shortidPath),
                 QString("/opt/amnezia/%1/clientsTable").arg(ContainerUtils::containerTypeToString(container)) };
    }
    return {};
}

ErrorCode InstallController::snapshotUpgradeStateFiles(const ServerCredentials &credentials, DockerContainer container,
                                                       SshSession &sshSession, QMap<QString, QByteArray> &filesOut)
{
    filesOut.clear();
    const QStringList paths = upgradeStateFilePaths(container);
    for (const QString &path : paths) {
        ErrorCode errorCode = ErrorCode::NoError;
        QByteArray data = sshSession.getTextFileFromContainer(container, credentials, path, errorCode);
        if (errorCode != ErrorCode::NoError) {
            qWarning() << "InstallController::upgradeContainer: failed to snapshot" << path;
            return errorCode;
        }
        filesOut.insert(path, data);
    }
    return ErrorCode::NoError;
}

ErrorCode InstallController::restoreUpgradeStateFiles(const ServerCredentials &credentials, DockerContainer container,
                                                      SshSession &sshSession, const QMap<QString, QByteArray> &files)
{
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        ErrorCode errorCode = sshSession.uploadTextFileToContainer(container, credentials, QString::fromUtf8(it.value()), it.key());
        if (errorCode != ErrorCode::NoError) {
            qWarning() << "InstallController::upgradeContainer: failed to restore" << it.key();
            return errorCode;
        }
    }
    return ErrorCode::NoError;
}

ErrorCode InstallController::findLeftoverParkedContainer(const ServerCredentials &credentials, DockerContainer container,
                                                         SshSession &sshSession, bool &hasLeftover, bool &liveContainerRunning)
{
    hasLeftover = false;
    liveContainerRunning = false;

    const QString containerName = ContainerUtils::containerToString(container);
    const QString parkedName = upgradeParkedName(containerName);

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    // List every container (not filtered server-side): container names can be substrings of
    // each other (e.g. "amnezia-awg" of "amnezia-awg2"), so exact matching has to happen here.
    const QString script = QStringLiteral("sudo docker ps -a --format '{{.Names}} {{.Status}}'");
    ErrorCode errorCode = sshSession.runScript(credentials, script, cbReadStdOut);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    const QStringList lines = stdOut.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString name = line.section(' ', 0, 0);
        if (name == parkedName) {
            hasLeftover = true;
        } else if (name == containerName && dockerContainerLineIsRunning(line)) {
            liveContainerRunning = true;
        }
    }
    return ErrorCode::NoError;
}

ErrorCode InstallController::rollbackParkedContainer(const ServerCredentials &credentials, DockerContainer container,
                                                     SshSession &sshSession)
{
    const QString containerName = ContainerUtils::containerToString(container);
    const QString parkedName = upgradeParkedName(containerName);

    amnezia::ScriptVars vars = amnezia::genBaseVars(credentials, container, QString(), QString());
    vars.append({ { "$PARKED_CONTAINER_NAME", parkedName } });

    // Best-effort: remove whatever broken "new" container exists, bring the parked one back
    // under its real name, restore its restart policy and start it. Each line runs on its own
    // (SshSession::runScript executes scripts line by line), and failures here are logged but
    // don't stop the rest of the rollback from being attempted.
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker rm -f $CONTAINER_NAME", vars));
    ErrorCode renameError =
            sshSession.runScript(credentials, sshSession.replaceVars("sudo docker rename $PARKED_CONTAINER_NAME $CONTAINER_NAME", vars));
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker update --restart=always $CONTAINER_NAME", vars));
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker start $CONTAINER_NAME", vars));

    return renameError;
}

ErrorCode InstallController::verifyUpgradedAwgContainer(const ServerCredentials &credentials, DockerContainer container,
                                                        SshSession &sshSession, const QString &expectedPublicKey,
                                                        int expectedPeerCount)
{
    const amnezia::ScriptVars vars = amnezia::genBaseVars(credentials, container, QString(), QString());

    QString runningOut;
    auto cbRunning = [&](const QString &data, libssh::Client &) {
        runningOut += data + "\n";
        return ErrorCode::NoError;
    };
    sshSession.runScript(credentials,
                         sshSession.replaceVars("sudo docker inspect -f '{{.State.Running}}' $CONTAINER_NAME", vars),
                         cbRunning);
    if (!runningOut.contains("true")) {
        qWarning() << "InstallController::upgradeContainer: verification failed, container is not running";
        return ErrorCode::ServerContainerUpgradeVerificationFailed;
    }

    QString pubKeyOut;
    auto cbPubKey = [&](const QString &data, libssh::Client &) {
        pubKeyOut += data;
        return ErrorCode::NoError;
    };
    sshSession.runScript(
            credentials, sshSession.replaceVars("sudo docker exec -i $CONTAINER_NAME bash -c 'awg show awg0 public-key'", vars),
            cbPubKey);
    if (pubKeyOut.trimmed() != expectedPublicKey.trimmed()) {
        qWarning() << "InstallController::upgradeContainer: verification failed, public key changed";
        return ErrorCode::ServerContainerUpgradeVerificationFailed;
    }

    QString peersOut;
    auto cbPeers = [&](const QString &data, libssh::Client &) {
        peersOut += data;
        return ErrorCode::NoError;
    };
    sshSession.runScript(
            credentials, sshSession.replaceVars("sudo docker exec -i $CONTAINER_NAME bash -c 'awg show awg0 peers | wc -l'", vars),
            cbPeers);
    bool ok = false;
    const int actualPeerCount = peersOut.trimmed().toInt(&ok);
    if (!ok || actualPeerCount != expectedPeerCount) {
        qWarning() << "InstallController::upgradeContainer: verification failed, expected" << expectedPeerCount
                   << "peers, server reports" << peersOut.trimmed();
        return ErrorCode::ServerContainerUpgradeVerificationFailed;
    }

    return ErrorCode::NoError;
}

ErrorCode InstallController::upgradeContainer(const QString &serverId, DockerContainer container, amnezia::ContainerUpgradeMode mode)
{
    if (mode == amnezia::ContainerUpgradeMode::UpgradeProtocol && container != DockerContainer::Awg2) {
        return ErrorCode::NotImplementedError;
    }
    const QStringList statePaths = upgradeStateFilePaths(container);
    if (statePaths.isEmpty()) {
        return ErrorCode::NotImplementedError;
    }

    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value() || adminConfig->isReadOnly() || !adminConfig->hasCredentials()) {
        // Upgrading reconfigures the server, so it's only available with admin (write) access.
        return ErrorCode::InternalError;
    }
    const ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    if (!adminConfig->containers.contains(container)) {
        return ErrorCode::ServerContainerMissingError;
    }
    const ContainerConfig oldConfig = adminConfig->containerConfig(container);

    SshSession sshSession;
    const QString containerName = ContainerUtils::containerToString(container);
    const QString parkedName = upgradeParkedName(containerName);
    amnezia::ScriptVars baseVars = amnezia::genBaseVars(credentials, container, QString(), QString());
    amnezia::ScriptVars parkVars = baseVars;
    parkVars.append({ { "$PARKED_CONTAINER_NAME", parkedName } });

    // --- Preflight ---
    ErrorCode errorCode = isUserInSudo(credentials, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }
    errorCode = isServerDpkgBusy(credentials, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    // --- Recover from (or clean up after) an interrupted previous attempt ---
    bool hasLeftover = false;
    bool liveContainerRunning = false;
    errorCode = findLeftoverParkedContainer(credentials, container, sshSession, hasLeftover, liveContainerRunning);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }
    if (hasLeftover) {
        if (liveContainerRunning) {
            // A previous attempt got as far as commit but was interrupted before it removed the
            // parked container - finish that cleanup now.
            sshSession.runScript(credentials, sshSession.replaceVars("sudo docker rm -f $PARKED_CONTAINER_NAME", parkVars));
        } else {
            // A previous attempt was interrupted after parking but before the new container came
            // up healthy. Recover the server to its last known-good state first.
            rollbackParkedContainer(credentials, container, sshSession);
        }
    }

    // --- Snapshot state files from the running container; abort with no changes on failure ---
    QMap<QString, QByteArray> snapshotFiles;
    errorCode = snapshotUpgradeStateFiles(credentials, container, sshSession, snapshotFiles);
    if (errorCode != ErrorCode::NoError) {
        return ErrorCode::ServerContainerUpgradeSnapshotFailed;
    }
    AwgInterfaceSnapshot awgSnapshot;
    if (container == DockerContainer::Awg2) {
        const QByteArray awgConf = snapshotFiles.value(QString::fromLatin1(protocols::awg::serverConfigPath));
        if (awgConf.trimmed().isEmpty()) {
            return ErrorCode::ServerContainerUpgradeSnapshotFailed;
        }
        awgSnapshot = parseAwgInterfaceSnapshot(QString::fromUtf8(awgConf));
        if (!awgSnapshot.isValid) {
            return ErrorCode::ServerContainerUpgradeSnapshotFailed;
        }
    }

    // --- Build the new [Interface] params (UpgradeProtocol) or keep the config unchanged ---
    ContainerConfig newConfig = oldConfig;
    if (mode == amnezia::ContainerUpgradeMode::UpgradeProtocol) {
        if (auto *awgConfig = newConfig.getAwgProtocolConfig()) {
            // Port/subnet/transport are unchanged by a protocol upgrade - only the packet
            // obfuscation parameters are reset to the current fresh-install defaults.
            AwgInstaller::generateAwgParameters(awgConfig->serverConfig);
            awgConfig->serverConfig.protocolVersion = awgConfig->serverProtocolVersion();
        }
    }

    // --- Prepare host + build the new image while the old container keeps serving ---
    errorCode = prepareHostWorker(credentials, container, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }
    errorCode = buildContainerWorker(credentials, container, newConfig, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    // --- Park the old container (do not remove it yet - it's our rollback target) ---
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker update --restart=no $CONTAINER_NAME", baseVars));
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker stop $CONTAINER_NAME", baseVars));
    errorCode =
            sshSession.runScript(credentials, sshSession.replaceVars("sudo docker rename $CONTAINER_NAME $PARKED_CONTAINER_NAME", parkVars));
    if (errorCode != ErrorCode::NoError) {
        // Nothing was removed yet; the old container (still named $CONTAINER_NAME, restart=no,
        // stopped) just needs to be brought back up.
        sshSession.runScript(credentials, sshSession.replaceVars("sudo docker update --restart=always $CONTAINER_NAME", baseVars));
        sshSession.runScript(credentials, sshSession.replaceVars("sudo docker start $CONTAINER_NAME", baseVars));
        return errorCode;
    }

    // From here on, any failure must roll back to the parked container.
    const auto rollbackAndReturn = [&](ErrorCode failure) {
        rollbackParkedContainer(credentials, container, sshSession);
        return failure;
    };

    errorCode = runContainerWorker(credentials, container, newConfig, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return rollbackAndReturn(errorCode);
    }

    // --- Restore state files onto the new container ---
    QMap<QString, QByteArray> filesToRestore = snapshotFiles;
    if (mode == amnezia::ContainerUpgradeMode::UpgradeProtocol) {
        if (const auto *awgConfig = newConfig.getAwgProtocolConfig()) {
            const QString migratedConf = buildUpgradedAwgConfig(awgSnapshot, awgConfig->serverConfig);
            if (migratedConf.isEmpty()) {
                return rollbackAndReturn(ErrorCode::ServerContainerUpgradeSnapshotFailed);
            }
            filesToRestore[QString::fromLatin1(protocols::awg::serverConfigPath)] = migratedConf.toUtf8();
        }
        // Other users' configs no longer match the new [Interface] params - flag every peer
        // except the admin's own client in the client table so the share page can offer them a
        // fresh config. The admin's own config is re-rendered automatically below instead.
        const QString clientsTableKey = QString("/opt/amnezia/%1/clientsTable").arg(ContainerUtils::containerTypeToString(container));
        const QByteArray clientsTableRaw = filesToRestore.value(clientsTableKey);
        const QString adminClientId = oldConfig.protocolConfig.clientId();
        const QJsonArray flaggedClientsTable =
                flagClientsForConfigUpdate(QJsonDocument::fromJson(clientsTableRaw).array(), adminClientId, true);
        filesToRestore[clientsTableKey] = QJsonDocument(flaggedClientsTable).toJson();
    }
    errorCode = restoreUpgradeStateFiles(credentials, container, sshSession, filesToRestore);
    if (errorCode != ErrorCode::NoError) {
        return rollbackAndReturn(errorCode);
    }

    // --- Firewall + startup ---
    setupServerFirewall(credentials, sshSession);
    errorCode = startupContainerWorker(credentials, container, newConfig, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return rollbackAndReturn(errorCode);
    }

    // --- Verify ---
    if (container == DockerContainer::Awg2) {
        const QString expectedPublicKey = QString::fromUtf8(snapshotFiles.value(QString::fromLatin1(protocols::awg::serverPublicKeyPath)));
        // The startup script runs detached (docker exec -d): give the interface time to come up.
        constexpr int verifyAttempts = 15;
        constexpr int verifyIntervalMs = 1500;
        for (int attempt = 1; attempt <= verifyAttempts; ++attempt) {
            errorCode = verifyUpgradedAwgContainer(credentials, container, sshSession, expectedPublicKey, awgSnapshot.peerCount);
            if (errorCode == ErrorCode::NoError || attempt == verifyAttempts) {
                break;
            }
            QThread::msleep(verifyIntervalMs);
        }
        if (errorCode != ErrorCode::NoError) {
            return rollbackAndReturn(errorCode);
        }
    }

    // --- Commit: drop the parked container and prune the now-dangling old image ---
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker rm -f $PARKED_CONTAINER_NAME", parkVars));
    sshSession.runScript(credentials, sshSession.replaceVars("sudo docker image prune -f", baseVars));

    // --- Persist local state only after the server-side upgrade has fully committed ---
    if (mode == amnezia::ContainerUpgradeMode::UpgradeProtocol) {
        if (auto *awgConfig = newConfig.getAwgProtocolConfig()) {
            if (awgConfig->clientConfig.has_value()) {
                awgConfig->clientConfig = reRenderAwgAdminClientConfig(awgConfig->clientConfig.value(), container, awgConfig->serverConfig);
            }
        }
    }
    adminConfig->updateContainerConfig(container, newConfig);
    m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);

    return ErrorCode::NoError;
}

void InstallController::clearCachedProfile(const QString &serverId, DockerContainer container)
{
    if (ContainerUtils::containerService(container) == ServiceType::Other) {
        return;
    }

    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return;
    }

    const ContainerConfig containerConfigModel = adminConfig->containerConfig(container);

    adminConfig->clearCachedClientProfile(container);
    m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);

    emit clientRevocationRequested(serverId, containerConfigModel, container);
}

ErrorCode InstallController::validateAndPrepareConfig(const QString &serverId)
{
    const auto kind = m_serversRepository->serverKind(serverId);

    DockerContainer container = DockerContainer::None;
    ContainerConfig containerConfig;

    switch (kind) {
    case serverConfigUtils::ConfigType::SelfHostedAdmin: {
        const auto cfg = m_serversRepository->selfHostedAdminConfig(serverId);
        if (!cfg.has_value()) {
            return ErrorCode::InternalError;
        }
        container = cfg->defaultContainer;
        containerConfig = cfg->containerConfig(container);
        break;
    }
    case serverConfigUtils::ConfigType::SelfHostedUser: {
        const auto cfg = m_serversRepository->selfHostedUserConfig(serverId);
        if (!cfg.has_value()) {
            return ErrorCode::InternalError;
        }
        container = cfg->defaultContainer;
        containerConfig = cfg->containerConfig(container);
        break;
    }
    case serverConfigUtils::ConfigType::Native: {
        const auto cfg = m_serversRepository->nativeConfig(serverId);
        if (!cfg.has_value()) {
            return ErrorCode::InternalError;
        }
        container = cfg->defaultContainer;
        containerConfig = cfg->containerConfig(container);
        break;
    }
    default:
        return ErrorCode::InternalError;
    }

    if (container == DockerContainer::None) {
        return ErrorCode::NoInstalledContainersError;
    }

    if (containerConfig.protocolConfig.hasClientConfig()) {
        return ErrorCode::NoError;
    }

    if (kind != serverConfigUtils::ConfigType::SelfHostedAdmin) {
        return ErrorCode::InternalError;
    }

    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }

    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }

    SshSession sshSession;
    const QString clientName = QString("Admin [%1]").arg(QSysInfo::prettyProductName());
    const ErrorCode errorCode = processContainerForAdmin(container, containerConfig, credentials, sshSession, serverId, clientName);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    adminConfig->updateContainerConfig(container, containerConfig);
    m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);

    return ErrorCode::NoError;
}

void InstallController::validateConfig(const QString &serverId)
{
    QFuture<ErrorCode> future = QtConcurrent::run([this, serverId]() {
        return validateAndPrepareConfig(serverId);
    });

    auto *watcher = new QFutureWatcher<ErrorCode>(this);
    connect(watcher, &QFutureWatcher<ErrorCode>::finished, this, [this, watcher]() {
        ErrorCode errorCode = watcher->result();
        watcher->deleteLater();

        if (errorCode == ErrorCode::NoError) {
            emit configValidated(true);
            return;
        }

        emit validationErrorOccurred(errorCode);
        emit configValidated(false);
    });
    watcher->setFuture(future);
}

void InstallController::addEmptyServer(const ServerCredentials &credentials)
{
    SelfHostedAdminServerConfig serverConfig;
    serverConfig.hostName = credentials.hostName;
    serverConfig.userName = credentials.userName;
    serverConfig.password = credentials.secretData;
    serverConfig.port = credentials.port;
    serverConfig.description = m_serversRepository->nextAvailableServerName();
    serverConfig.displayName = serverConfig.description.isEmpty() ? serverConfig.hostName : serverConfig.description;
    serverConfig.defaultContainer = DockerContainer::None;

    m_serversRepository->addServer(QString(), serverConfig.toJson(),
                                    serverConfigUtils::ConfigType::SelfHostedAdmin);
}

ErrorCode InstallController::prepareContainerConfig(DockerContainer container, const ServerCredentials &credentials, ContainerConfig &containerConfig, SshSession &sshSession)
{
    if (!ContainerUtils::isSupportedByCurrentPlatform(container)) {
        return ErrorCode::NoError;
    }

    if (ContainerUtils::containerService(container) != ServiceType::Other) {
        if ((container == DockerContainer::Xray || container == DockerContainer::SSXray)
            && containerConfig.protocolConfig.hasClientConfig()) {
            return ErrorCode::NoError;
        }

        Proto protocol = ContainerUtils::defaultProtocol(container);

        DnsSettings dnsSettings = {
            m_appSettingsRepository->primaryDns(),
            m_appSettingsRepository->secondaryDns()
        };

        auto configurator = ConfiguratorBase::create(protocol, &sshSession);
        ErrorCode errorCode = ErrorCode::NoError;
        ProtocolConfig newProtocolConfig = configurator->createConfig(credentials, container, containerConfig, dnsSettings, errorCode);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }

        containerConfig.protocolConfig = newProtocolConfig;
    }

    return ErrorCode::NoError;
}

void InstallController::adminAppendRequested(const QString &serverId, DockerContainer container,
                                             const ContainerConfig &containerConfig, const QString &clientName)
{
    if (ContainerUtils::containerService(container) == ServiceType::Other
        || !containerConfig.protocolConfig.hasClientConfig()) {
        return;
    }
    QString clientId = containerConfig.protocolConfig.clientId();
    if (!clientId.isEmpty()) {
        emit clientAppendRequested(serverId, clientId, clientName, container);
    }
}

ErrorCode InstallController::processContainerForAdmin(DockerContainer container, ContainerConfig &containerConfig,
                                                      const ServerCredentials &credentials, SshSession &sshSession,
                                                      const QString &serverId, const QString &clientName)
{
    if (ContainerUtils::isSupportedByCurrentPlatform(container)) {
        ErrorCode errorCode = prepareContainerConfig(container, credentials, containerConfig, sshSession);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }
    }
    adminAppendRequested(serverId, container, containerConfig, clientName);
    return ErrorCode::NoError;
}

ErrorCode InstallController::buildContainerWorker(const ServerCredentials &credentials, DockerContainer container, const ContainerConfig &config, SshSession &sshSession)
{
    amnezia::ScriptVars baseVars = amnezia::genBaseVars(credentials, container, QString(), QString());
    
    QString dockerfilePath = "/opt/amnezia/" + ContainerUtils::containerToString(container) + "/Dockerfile";
    QString removeScript = QString("sudo rm %1").arg(dockerfilePath);
    
    ErrorCode errorCode = sshSession.runScript(credentials, sshSession.replaceVars(removeScript, baseVars));
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    errorCode = sshSession.uploadFileToHost(credentials, amnezia::scriptData(ProtocolScriptType::dockerfile, container).toUtf8(), dockerfilePath);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    amnezia::ScriptVars protocolVars = amnezia::genProtocolVarsForContainer(container, config);
    baseVars.append(protocolVars);
    ErrorCode error = sshSession.runScript(
            credentials, sshSession.replaceVars(amnezia::scriptData(SharedScriptType::build_container), baseVars), cbReadStdOut,
            cbReadStdErr);

    if (stdOut.contains("doesn't work on cgroups v2"))
        return ErrorCode::ServerDockerOnCgroupsV2;
    if (stdOut.contains("cgroup mountpoint does not exist"))
        return ErrorCode::ServerCgroupMountpoint;
    if (stdOut.contains("have reached") && stdOut.contains("pull rate limit"))
        return ErrorCode::DockerPullRateLimit;

    if (stdOut.contains("returned a non-zero code")
        || stdOut.contains("failed to solve")
        || stdOut.contains("Unable to find image")
        || stdOut.contains("Couldn't connect to server"))
        return ErrorCode::ServerDockerFailedError;

    return error;
}

ErrorCode InstallController::runContainerWorker(const ServerCredentials &credentials, DockerContainer container, ContainerConfig &config, SshSession &sshSession)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    amnezia::ScriptVars baseVars = amnezia::genBaseVars(credentials, container, QString(), QString());
    amnezia::ScriptVars protocolVars = amnezia::genProtocolVarsForContainer(container, config);
    baseVars.append(protocolVars);
    ErrorCode e = sshSession.runScript(
            credentials, sshSession.replaceVars(amnezia::scriptData(ProtocolScriptType::run_container, container), baseVars),
            cbReadStdOut);

    if (stdOut.contains("address already in use"))
        return ErrorCode::ServerPortAlreadyAllocatedError;
    if (stdOut.contains("is already in use by container"))
        return ErrorCode::ServerPortAlreadyAllocatedError;
    if (stdOut.contains("invalid publish"))
        return ErrorCode::ServerDockerFailedError;
    if (stdOut.contains("Unable to find image") || stdOut.contains("No such image"))
        return ErrorCode::ServerDockerFailedError;

    return e;
}

ErrorCode InstallController::configureContainerWorker(const ServerCredentials &credentials, DockerContainer container, ContainerConfig &config, SshSession &sshSession)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    amnezia::ScriptVars baseVars = amnezia::genBaseVars(credentials, container, QString(), QString());
    amnezia::ScriptVars protocolVars = amnezia::genProtocolVarsForContainer(container, config);
    baseVars.append(protocolVars);
    ErrorCode e = sshSession.runContainerScript(
            credentials, container,
            sshSession.replaceVars(amnezia::scriptData(ProtocolScriptType::configure_container, container), baseVars),
            cbReadStdOut, cbReadStdErr);

    if (e != ErrorCode::NoError) {
        return e;
    }

    if (dockerDaemonContainerMissing(stdOut, ContainerUtils::containerToString(container))) {
        qDebug() << "configureContainerWorker: Docker daemon reports container missing/stopped, output:" << stdOut;
        return ErrorCode::ServerContainerMissingError;
    }

    updateContainerConfigAfterInstallation(container, config, stdOut);

    if (container == DockerContainer::MtProxy) {
        MtProxyInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, config);
    } else if (container == DockerContainer::Telemt) {
        TelemtInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, config);
    } else if (container == DockerContainer::TProxy) {
        TProxyInstaller::uploadClientSettingsSnapshot(sshSession, credentials, container, config);
    }

    return ErrorCode::NoError;
}

ErrorCode InstallController::startupContainerWorker(const ServerCredentials &credentials, DockerContainer container, const ContainerConfig &config, SshSession &sshSession)
{
    QString script = amnezia::scriptData(ProtocolScriptType::container_startup, container);

    if (script.isEmpty()) {
        return ErrorCode::NoError;
    }

    amnezia::ScriptVars baseVars = amnezia::genBaseVars(credentials, container, QString(), QString());
    amnezia::ScriptVars protocolVars = amnezia::genProtocolVarsForContainer(container, config);
    baseVars.append(protocolVars);
    ErrorCode e = sshSession.uploadTextFileToContainer(container, credentials, sshSession.replaceVars(script, baseVars),
                                                                "/opt/amnezia/start.sh");
    if (e)
        return e;

    return sshSession.runScript(
            credentials,
            sshSession.replaceVars("sudo docker exec -d $CONTAINER_NAME sh -c \"chmod a+x /opt/amnezia/start.sh && "
                                            "/opt/amnezia/start.sh\"",
                                            baseVars));
}

ErrorCode InstallController::isServerPortBusy(const ServerCredentials &credentials, DockerContainer container, const ContainerConfig &config, SshSession &sshSession)
{
    if (container == DockerContainer::Dns) {
        return ErrorCode::NoError;
    }

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    const Proto protocol = ContainerUtils::defaultProtocol(container);
    QStringList fixedPorts = ContainerUtils::fixedPortsForContainer(container);

    QString port = config.protocolConfig.port();
    if (port.isEmpty()) {
        port = QString::number(ProtocolUtils::defaultPort(protocol));
    }
    if (container == DockerContainer::TProxy) {
        if (const auto *tProxyConfig = config.getTProxyProtocolConfig()) {
            const QString httpPort =
                    tProxyConfig->httpPort.isEmpty() ? QString(protocols::tProxy::defaultHttpPort) : tProxyConfig->httpPort;
            if (!fixedPorts.contains(httpPort) && httpPort != port) {
                fixedPorts.append(httpPort);
            }
        }
    }
    QString transportProto = config.protocolConfig.transportProto();
    if (transportProto.isEmpty()) {
        transportProto = ProtocolUtils::transportProtoToString(ProtocolUtils::defaultTransportProto(protocol), protocol);
    }

    // Match exact host ports in lsof output (e.g. *:80) but not prefixes like *:8025 or *:8080.
    QStringList portsToCheck;
    portsToCheck << port;
    for (const QString &fixedPort : fixedPorts) {
        if (!portsToCheck.contains(fixedPort)) {
            portsToCheck << fixedPort;
        }
    }
    QStringList portRegexParts;
    for (const QString &p : portsToCheck) {
        portRegexParts << QString(":%1([^0-9]|$)").arg(p);
    }
    const QString portRegex = portRegexParts.join(QLatin1Char('|'));

    QString script = QString("which lsof > /dev/null 2>&1 || true && sudo lsof -i -P -n 2>/dev/null | grep -E '%1'")
                               .arg(portRegex);

    if (transportProto == "tcpandudp") {
        QString tcpProtoScript = script;
        QString udpProtoScript = script;
        tcpProtoScript.append(" | grep -i tcp");
        udpProtoScript.append(" | grep -i udp");
        tcpProtoScript.append(" | grep LISTEN");

        ErrorCode errorCode = sshSession.runScript(
                credentials,
                sshSession.replaceVars(tcpProtoScript, amnezia::genBaseVars(credentials, container, QString(), QString())),
                cbReadStdOut, cbReadStdErr);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }

        errorCode = sshSession.runScript(
                credentials,
                sshSession.replaceVars(udpProtoScript, amnezia::genBaseVars(credentials, container, QString(), QString())),
                cbReadStdOut, cbReadStdErr);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }

        if (!stdOut.isEmpty()) {
            return ErrorCode::ServerPortAlreadyAllocatedError;
        }
        return ErrorCode::NoError;
    }

    script = script.append(" | grep -i %1").arg(transportProto);

    if (transportProto == "tcp") {
        script = script.append(" | grep LISTEN");
    }

    ErrorCode errorCode = sshSession.runScript(
            credentials, sshSession.replaceVars(script, amnezia::genBaseVars(credentials, container, QString(), QString())),
            cbReadStdOut, cbReadStdErr);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    if (!stdOut.isEmpty()) {
        return ErrorCode::ServerPortAlreadyAllocatedError;
    }
    return ErrorCode::NoError;
}

bool InstallController::isReinstallContainerRequired(DockerContainer container, const ContainerConfig &oldConfig, const ContainerConfig &newConfig)
{
    if (container == DockerContainer::OpenVpn) {
        const auto* oldOvpnConfig = oldConfig.getOpenVpnProtocolConfig();
        const auto* newOvpnConfig = newConfig.getOpenVpnProtocolConfig();
        
        if (oldOvpnConfig && newOvpnConfig) {
            if (!oldOvpnConfig->serverConfig.hasEqualServerSettings(newOvpnConfig->serverConfig)) {
                return true;
            }
        }
    }

    if (ContainerUtils::isAwgContainer(container)) {
        const auto* oldAwgConfig = oldConfig.getAwgProtocolConfig();
        const auto* newAwgConfig = newConfig.getAwgProtocolConfig();
        
        if (oldAwgConfig && newAwgConfig) {
            if (!oldAwgConfig->serverConfig.hasEqualServerSettings(newAwgConfig->serverConfig)) {
                return true;
            }
        }
    }

    if (container == DockerContainer::WireGuard) {
        const auto* oldWgConfig = oldConfig.getWireGuardProtocolConfig();
        const auto* newWgConfig = newConfig.getWireGuardProtocolConfig();
        
        if (oldWgConfig && newWgConfig) {
            if (!oldWgConfig->serverConfig.hasEqualServerSettings(newWgConfig->serverConfig)) {
                return true;
            }
        }
    }

    if (container == DockerContainer::Xray || container == DockerContainer::SSXray) {
        const auto *oldXrayConfig = oldConfig.getXrayProtocolConfig();
        const auto *newXrayConfig = newConfig.getXrayProtocolConfig();

        if (oldXrayConfig && newXrayConfig) {
            if (!oldXrayConfig->serverConfig.hasEqualServerSettings(newXrayConfig->serverConfig)) {
                return true;
            }
        }
    }

    if (container == DockerContainer::MtProxy) {
        const auto *oldMt = oldConfig.getMtProxyProtocolConfig();
        const auto *newMt = newConfig.getMtProxyProtocolConfig();
        if (oldMt && newMt) {
            const QString oldPort =
                    oldMt->port.isEmpty() ? QString(protocols::mtProxy::defaultPort) : oldMt->port;
            const QString newPort =
                    newMt->port.isEmpty() ? QString(protocols::mtProxy::defaultPort) : newMt->port;
            if (oldPort != newPort) {
                return true;
            }
        }
    }

    if (container == DockerContainer::Telemt) {
        const auto *oldT = oldConfig.getTelemtProtocolConfig();
        const auto *newT = newConfig.getTelemtProtocolConfig();
        if (oldT && newT) {
            const QString oldPort =
                    oldT->port.isEmpty() ? QString(protocols::telemt::defaultPort) : oldT->port;
            const QString newPort =
                    newT->port.isEmpty() ? QString(protocols::telemt::defaultPort) : newT->port;
            if (oldPort != newPort) {
                return true;
            }
        }
    }

    if (container == DockerContainer::TProxy) {
        const auto *oldP = oldConfig.getTProxyProtocolConfig();
        const auto *newP = newConfig.getTProxyProtocolConfig();
        if (oldP && newP) {
            const QString oldHttps =
                    oldP->port.isEmpty() ? QString(protocols::tProxy::defaultPort) : oldP->port;
            const QString newHttps =
                    newP->port.isEmpty() ? QString(protocols::tProxy::defaultPort) : newP->port;
            const QString oldHttp =
                    oldP->httpPort.isEmpty() ? QString(protocols::tProxy::defaultHttpPort) : oldP->httpPort;
            const QString newHttp =
                    newP->httpPort.isEmpty() ? QString(protocols::tProxy::defaultHttpPort) : newP->httpPort;
            if (oldHttps != newHttps || oldHttp != newHttp) {
                return true;
            }
        }
    }

    if (container == DockerContainer::Socks5Proxy) {
        return true;
    }

    return false;
}

void InstallController::cancelInstallation()
{
    m_cancelInstallation = true;
}

ErrorCode InstallController::installDockerWorker(const ServerCredentials &credentials, DockerContainer container, SshSession &sshSession)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &client) {
        stdOut += data + "\n";

        if (data.contains("Automatically restart Docker daemon?")) {
            return client.writeResponse("yes");
        }
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    ErrorCode error = sshSession.runScript(
            credentials,
            sshSession.replaceVars(amnezia::scriptData(SharedScriptType::install_docker),
                                            amnezia::genBaseVars(credentials, DockerContainer::None, QString(), QString())),
            cbReadStdOut, cbReadStdErr);

    qDebug().noquote() << "InstallController::installDockerWorker" << stdOut;

    if (container == DockerContainer::MtProxy || container == DockerContainer::Telemt
            || container == DockerContainer::TProxy) {
        QString conntrackOut;
        auto cbConntrack = [&](const QString &data, libssh::Client &) {
            conntrackOut += data + "\n";
            return ErrorCode::NoError;
        };
        sshSession.runScript(
                credentials,
                sshSession.replaceVars(amnezia::scriptData(SharedScriptType::install_conntrack),
                                       amnezia::genBaseVars(credentials, DockerContainer::None, QString(), QString())),
                cbConntrack, cbConntrack);
        qDebug().noquote() << "InstallController::installDockerWorker install_conntrack:" << conntrackOut;
    }

    if (container == DockerContainer::Awg2) {
        QRegularExpression kernelVersionRegex(R"(Linux\s+(\d+)\.(\d+)[^\d]*)");
        QRegularExpressionMatch match = kernelVersionRegex.match(stdOut);
        if (match.hasMatch()) {
            int majorVersion = match.captured(1).toInt();
            int minorVersion = match.captured(2).toInt();

            if (majorVersion < 4 || (majorVersion == 4 && minorVersion < 14)) {
                return ErrorCode::ServerLinuxKernelTooOld;
            }
        }
    }

    if (stdOut.contains("lock"))
        return ErrorCode::ServerPacketManagerError;
    if (stdOut.contains("Container runtime is not supported"))
        return ErrorCode::ServerContainerRuntimeNotSupported;
    
    QRegularExpression notFoundRegex(
        R"(^.*(?:sudo:|docker:).*not found.*$)",
        QRegularExpression::MultilineOption);

    if (notFoundRegex.match(stdOut).hasMatch()) {
        return ErrorCode::ServerDockerFailedError;
    }
    
    if (stdOut.contains("Container runtime service not running"))
        return ErrorCode::ContainerRuntimeServiceNotRunning;

    return error;
}

ErrorCode InstallController::prepareHostWorker(const ServerCredentials &credentials, DockerContainer container, SshSession &sshSession)
{
    // create folder on host
    return sshSession.runScript(credentials,
                                         sshSession.replaceVars(amnezia::scriptData(SharedScriptType::prepare_host),
                                                                         amnezia::genBaseVars(credentials, container, QString(), QString())));
}

ErrorCode InstallController::isUserInSudo(const ServerCredentials &credentials, SshSession &sshSession)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    const QString scriptData = amnezia::scriptData(SharedScriptType::check_user_in_sudo);
    ErrorCode error = sshSession.runScript(
            credentials,
            sshSession.replaceVars(scriptData, amnezia::genBaseVars(credentials, DockerContainer::None, QString(), QString())),
            cbReadStdOut, cbReadStdErr);

    if (credentials.userName != "root" && stdOut.contains("sudo:") && !stdOut.contains("uname:") && stdOut.contains("not found"))
        return ErrorCode::ServerSudoPackageIsNotPreinstalled;
    if (credentials.userName != "root" && !stdOut.contains("sudo") && !stdOut.contains("wheel"))
        return ErrorCode::ServerUserNotInSudo;
    if (stdOut.contains("can't cd to") || stdOut.contains("Permission denied") || stdOut.contains("No such file or directory"))
        return ErrorCode::ServerUserDirectoryNotAccessible;
    if (stdOut.contains(QRegularExpression(R"(\bsudoers\b)")) || stdOut.contains("is not allowed to") || stdOut.contains("can't do that"))
        return ErrorCode::ServerUserNotAllowedInSudoers;
    if (stdOut.contains("password is required") || stdOut.contains("authentication is required"))
        return ErrorCode::ServerUserPasswordRequired;

    return error;
}

ErrorCode InstallController::isServerDpkgBusy(const ServerCredentials &credentials, SshSession &sshSession)
{
    m_cancelInstallation = false;
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    QFutureWatcher<ErrorCode> watcher;

    QFuture<ErrorCode> future = QtConcurrent::run([this, &stdOut, &cbReadStdOut, &cbReadStdErr, &credentials, &sshSession]() {
        // max 100 attempts
        for (int i = 0; i < 30; ++i) {
            if (m_cancelInstallation) {
                return ErrorCode::ServerCancelInstallation;
            }
            stdOut.clear();
            sshSession.runScript(
                    credentials,
                    sshSession.replaceVars(amnezia::scriptData(SharedScriptType::check_server_is_busy),
                                                    amnezia::genBaseVars(credentials, DockerContainer::None, QString(), QString())),
                    cbReadStdOut, cbReadStdErr);

            if (stdOut.contains("Packet manager not found"))
                return ErrorCode::ServerPacketManagerError;
            if (stdOut.contains("fuser not installed") || stdOut.contains("cat not installed"))
                return ErrorCode::NoError;

            if (stdOut.isEmpty()) {
                return ErrorCode::NoError;
            } else {
#ifdef MZ_DEBUG
                qDebug().noquote() << stdOut;
#endif
                emit serverIsBusy(true);
                QThread::msleep(10000);
            }
        }
        return ErrorCode::ServerPacketManagerError;
    });

    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<ErrorCode>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(future);
    wait.exec();

    emit serverIsBusy(false);

    return future.result();
}

ErrorCode InstallController::setupServerFirewall(const ServerCredentials &credentials, SshSession &sshSession)
{
    return sshSession.runScript(
            credentials,
            sshSession.replaceVars(amnezia::scriptData(SharedScriptType::setup_host_firewall),
                                            amnezia::genBaseVars(credentials, DockerContainer::None, QString(), QString())));
}

ErrorCode InstallController::rebootServer(const QString &serverId)
{
    const auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;

    QString script = QString("sudo reboot");

    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };

    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    return sshSession.runScript(credentials, script, cbReadStdOut, cbReadStdErr);
}

ErrorCode InstallController::removeAllContainers(const QString &serverId)
{
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;
    ErrorCode errorCode = sshSession.runScript(credentials, amnezia::scriptData(SharedScriptType::remove_all_containers));

    if (errorCode == ErrorCode::NoError) {
        adminConfig->containers.clear();
        adminConfig->defaultContainer = DockerContainer::None;
        m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
    }

    return errorCode;
}

ErrorCode InstallController::removeContainer(const QString &serverId, DockerContainer container)
{
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;
    const amnezia::ScriptVars removeContainerVars =
            amnezia::genBaseVars(credentials, container, QString(), QString());
    const bool removeDataVolume = (container == DockerContainer::MtProxy || container == DockerContainer::Telemt
            || container == DockerContainer::TProxy);
    ErrorCode errorCode =
            sshSession.runScript(credentials, buildRemoveContainerScript(removeContainerVars, removeDataVolume));

    if (errorCode == ErrorCode::NoError) {
        QMap<DockerContainer, ContainerConfig> containers = adminConfig->containers;
        containers.remove(container);

        DockerContainer defaultContainer = adminConfig->defaultContainer;
        if (defaultContainer == container) {
            if (containers.isEmpty()) {
                defaultContainer = DockerContainer::None;
            } else {
                defaultContainer = containers.begin().key();
            }
        }

        adminConfig->containers = containers;
        adminConfig->defaultContainer = defaultContainer;
        m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
    }

    return errorCode;
}

QScopedPointer<InstallerBase> InstallController::createInstaller(DockerContainer container)
{
    switch (container) {
    case DockerContainer::Awg: return QScopedPointer<InstallerBase>(new AwgInstaller(this));
    case DockerContainer::Awg2: return QScopedPointer<InstallerBase>(new AwgInstaller(this));
    case DockerContainer::WireGuard: return QScopedPointer<InstallerBase>(new WireguardInstaller(this));
    case DockerContainer::OpenVpn: return QScopedPointer<InstallerBase>(new OpenVpnInstaller(this));
    case DockerContainer::Xray:
    case DockerContainer::SSXray: return QScopedPointer<InstallerBase>(new XrayInstaller(this));
    case DockerContainer::TorWebSite: return QScopedPointer<InstallerBase>(new TorInstaller(this));
    case DockerContainer::Sftp: return QScopedPointer<InstallerBase>(new SftpInstaller(this));
    case DockerContainer::Socks5Proxy: return QScopedPointer<InstallerBase>(new Socks5Installer(this));
    case DockerContainer::MtProxy: return QScopedPointer<InstallerBase>(new MtProxyInstaller(this));
    case DockerContainer::Telemt: return QScopedPointer<InstallerBase>(new TelemtInstaller(this));
    case DockerContainer::TProxy: return QScopedPointer<InstallerBase>(new TProxyInstaller(this));
    default: return QScopedPointer<InstallerBase>(new InstallerBase(this));
    }
}

ContainerConfig InstallController::generateConfig(DockerContainer container, int port, TransportProto transportProto)
{
    auto installer = createInstaller(container);
    return installer->generateConfig(container, port, transportProto);
}

ErrorCode InstallController::installContainer(const ServerCredentials &credentials, DockerContainer container, int port,
                                              TransportProto transportProto, ContainerConfig &config)
{
    config = generateConfig(container, port, transportProto);
    if (container == DockerContainer::TProxy) {
        auto *tProxyConfig = config.getTProxyProtocolConfig();
        if (tProxyConfig) {
            if (!m_tproxyInstallHostname.isEmpty()) {
                tProxyConfig->hostname = m_tproxyInstallHostname;
            }
            if (!m_tproxyInstallEmail.isEmpty()) {
                tProxyConfig->acmeEmail = m_tproxyInstallEmail;
            }
        }
        m_tproxyInstallHostname.clear();
        m_tproxyInstallEmail.clear();

        // TProxy needs a hostname and ACME email before the first deploy (configure_container.sh
        // exits 1 without them). Fail fast with a clear error instead of a confusing server-side
        // failure if the install path is reached without them being supplied via setTProxyInstallHints().
        if (!tProxyConfig || tProxyConfig->hostname.isEmpty() || tProxyConfig->acmeEmail.isEmpty()) {
            return ErrorCode::InternalError;
        }
    }
    return setupContainer(credentials, container, config, false);
}

void InstallController::setTProxyInstallHints(const QString &hostname, const QString &email)
{
    m_tproxyInstallHostname = hostname;
    m_tproxyInstallEmail = email;
}


bool InstallController::isUpdateDockerContainerRequired(DockerContainer container, const ContainerConfig &oldConfig, const ContainerConfig &newConfig)
{
    if (ContainerUtils::isAwgContainer(container)) {
        const auto* oldAwgConfig = oldConfig.getAwgProtocolConfig();
        const auto* newAwgConfig = newConfig.getAwgProtocolConfig();
        
        if (oldAwgConfig && newAwgConfig) {
            if (oldAwgConfig->serverConfig.hasEqualServerSettings(newAwgConfig->serverConfig)) {
                return false;
            }
        }
    } else if (container == DockerContainer::WireGuard) {
        const auto* oldWgConfig = oldConfig.getWireGuardProtocolConfig();
        const auto* newWgConfig = newConfig.getWireGuardProtocolConfig();
        
        if (oldWgConfig && newWgConfig) {
            if (oldWgConfig->serverConfig.hasEqualServerSettings(newWgConfig->serverConfig)) {
                return false;
            }
        }
    } else if (container == DockerContainer::MtProxy) {
        const auto *oldMt = oldConfig.getMtProxyProtocolConfig();
        const auto *newMt = newConfig.getMtProxyProtocolConfig();
        if (!oldMt || !newMt) {
            return true;
        }
        return !oldMt->equalsDockerDeploymentSettings(*newMt);
    } else if (container == DockerContainer::Telemt) {
        const auto *oldT = oldConfig.getTelemtProtocolConfig();
        const auto *newT = newConfig.getTelemtProtocolConfig();
        if (!oldT || !newT) {
            return true;
        }
        return !oldT->equalsDockerDeploymentSettings(*newT);
    } else if (container == DockerContainer::TProxy) {
        const auto *oldP = oldConfig.getTProxyProtocolConfig();
        const auto *newP = newConfig.getTProxyProtocolConfig();
        if (!oldP || !newP) {
            return true;
        }
        return !oldP->equalsDockerDeploymentSettings(*newP);
    }

    return true;
}

ErrorCode InstallController::scanServerForInstalledContainers(const QString &serverId)
{
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;

    QMap<DockerContainer, ContainerConfig> installedContainers;
    ErrorCode errorCode = getAlreadyInstalledContainers(credentials, installedContainers, sshSession);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    QMap<DockerContainer, ContainerConfig> containers = adminConfig->containers;
    bool hasNewContainers = false;

    QString clientName = QString("Admin [%1]").arg(QSysInfo::prettyProductName());
    for (auto iterator = installedContainers.begin(); iterator != installedContainers.end(); iterator++) {
        if (!containers.contains(iterator.key())) {
            ContainerConfig containerConfig = iterator.value();
            errorCode = processContainerForAdmin(iterator.key(), containerConfig, credentials, sshSession,
                                                 serverId, clientName);
            if (errorCode != ErrorCode::NoError) {
                return errorCode;
            }
            containers.insert(iterator.key(), containerConfig);
            hasNewContainers = true;

            DockerContainer defaultContainer = adminConfig->defaultContainer;
            if (defaultContainer == DockerContainer::None
                && ContainerUtils::containerService(iterator.key()) != ServiceType::Other
                && ContainerUtils::isSupportedByCurrentPlatform(iterator.key())) {
                adminConfig->defaultContainer = iterator.key();
            }
        }
    }

    if (hasNewContainers) {
        adminConfig->containers = containers;
        m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
    }

    return ErrorCode::NoError;
}

ErrorCode InstallController::installServer(const ServerCredentials &credentials, DockerContainer container, int port,
                                           TransportProto transportProto, bool &wasContainerInstalled)
{
    SshSession sshSession;
    QMap<DockerContainer, ContainerConfig> installedContainers;
    ErrorCode errorCode = getAlreadyInstalledContainers(credentials, installedContainers, sshSession);
    if (errorCode) {
        return errorCode;
    }

    wasContainerInstalled = false;
    if (!installedContainers.contains(container)) {
        ContainerConfig config;
        errorCode = installContainer(credentials, container, port, transportProto, config);
        if (errorCode) {
            return errorCode;
        }

        installedContainers.insert(container, config);
        wasContainerInstalled = true;
    }

    QMap<DockerContainer, ContainerConfig> preparedContainers;
    for (auto iterator = installedContainers.begin(); iterator != installedContainers.end(); iterator++) {
        DockerContainer container = iterator.key();
        ContainerConfig containerConfig = iterator.value();

        if (ContainerUtils::isSupportedByCurrentPlatform(container)) {
            errorCode = prepareContainerConfig(container, credentials, containerConfig, sshSession);
            if (errorCode != ErrorCode::NoError) {
                return errorCode;
            }
        }
        preparedContainers.insert(container, containerConfig);
    }

    SelfHostedAdminServerConfig serverConfig;
    serverConfig.hostName = credentials.hostName;
    serverConfig.userName = credentials.userName;
    serverConfig.password = credentials.secretData;
    serverConfig.port = credentials.port;
    serverConfig.description = m_serversRepository->nextAvailableServerName();

    for (auto iterator = preparedContainers.begin(); iterator != preparedContainers.end(); iterator++) {
        serverConfig.containers.insert(iterator.key(), iterator.value());
    }

    serverConfig.defaultContainer = container;

    serverConfig.displayName = serverConfig.description.isEmpty() ? serverConfig.hostName : serverConfig.description;

    const QString newServerId = m_serversRepository->addServer(QString(), serverConfig.toJson(),
                                                               serverConfigUtils::ConfigType::SelfHostedAdmin);
    QString clientName = QString("Admin [%1]").arg(QSysInfo::prettyProductName());
    for (auto iterator = preparedContainers.begin(); iterator != preparedContainers.end(); iterator++) {
        adminAppendRequested(newServerId, iterator.key(), iterator.value(), clientName);
    }

    return ErrorCode::NoError;
}

ErrorCode InstallController::installContainer(const QString &serverId, DockerContainer container, int port,
                                              TransportProto transportProto, bool &wasContainerInstalled)
{
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;
    
    QMap<DockerContainer, ContainerConfig> installedContainers;
    ErrorCode errorCode = getAlreadyInstalledContainers(credentials, installedContainers, sshSession);
    if (errorCode) {
        return errorCode;
    }

    wasContainerInstalled = false;
    if (!installedContainers.contains(container)) {
        ContainerConfig config;
        errorCode = installContainer(credentials, container, port, transportProto, config);
        if (errorCode) {
            return errorCode;
        }

        installedContainers.insert(container, config);
        wasContainerInstalled = true;
    }

    QString clientName = QString("Admin [%1]").arg(QSysInfo::prettyProductName());
    for (auto iterator = installedContainers.begin(); iterator != installedContainers.end(); iterator++) {
        ContainerConfig existingConfigModel = adminConfig->containerConfig(iterator.key());
        if (existingConfigModel.container == DockerContainer::None) {
            ContainerConfig containerConfig = iterator.value();
            errorCode = processContainerForAdmin(iterator.key(), containerConfig, credentials, sshSession,
                                                 serverId, clientName);
            if (errorCode != ErrorCode::NoError) {
                return errorCode;
            }
            adminConfig->updateContainerConfig(iterator.key(), containerConfig);
            m_serversRepository->editServer(serverId, adminConfig->toJson(),
                                            serverConfigUtils::ConfigType::SelfHostedAdmin);
        }
    }

    return ErrorCode::NoError;
}

ErrorCode InstallController::checkSshConnection(ServerCredentials &credentials, QString &output,
                                                std::function<QString()> passphraseCallback)
{
    SshSession sshSession;
    ErrorCode errorCode = ErrorCode::NoError;

    if (credentials.secretData.contains("BEGIN") && credentials.secretData.contains("PRIVATE KEY")) {
        if (!passphraseCallback) {
            return ErrorCode::SshPrivateKeyError;
        }

        QString decryptedPrivateKey;
        errorCode = sshSession.getDecryptedPrivateKey(credentials, decryptedPrivateKey, passphraseCallback);
        if (errorCode != ErrorCode::NoError) {
            return errorCode;
        }
        credentials.secretData = decryptedPrivateKey;
    }

    output = sshSession.checkSshConnection(credentials, errorCode);
    return errorCode;
}

bool InstallController::isServerAlreadyExists(const ServerCredentials &credentials, int &existingServerIndex)
{
    int serversCount = m_serversRepository->serversCount();
    for (int i = 0; i < serversCount; i++) {
        const QString existingServerId = m_serversRepository->serverIdAt(i);
        const auto adminConfig = m_serversRepository->selfHostedAdminConfig(existingServerId);
        if (!adminConfig.has_value()) {
            continue;
        }
        const ServerCredentials existingCredentials = adminConfig->credentials();
        if (!existingCredentials.isValid()) {
            continue;
        }
        if (credentials.hostName == existingCredentials.hostName && credentials.port == existingCredentials.port) {
            existingServerIndex = i;
            return true;
        }
    }
    existingServerIndex = -1;
    return false;
}

ErrorCode InstallController::mountSftpDrive(const ServerCredentials &credentials, const QString &port, const QString &password,
                                            const QString &username)
{
    QString mountPath;
    QString cmd;
    QString hostname = credentials.hostName;

#ifdef Q_OS_WINDOWS
    mountPath = Utils::getNextDriverLetter() + ":";
    cmd = "C:\\Program Files\\SSHFS-Win\\bin\\sshfs.exe";
#elif defined AMNEZIA_DESKTOP
    mountPath = QString("%1/sftp:%2:%3").arg(QStandardPaths::writableLocation(QStandardPaths::HomeLocation), hostname, port);
    QDir dir(mountPath);
    if (!dir.exists()) {
        dir.mkpath(mountPath);
    }

    cmd = "/usr/local/bin/sshfs";

    QSharedPointer<QProcess> process(new QProcess(this));
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process.get(), &QProcess::readyRead, this, [process, mountPath]() {
        QString s = process->readAll();
        if (s.contains("The service sshfs has been started")) {
            QDesktopServices::openUrl(QUrl("file:///" + mountPath));
        }
        qDebug() << s;
    });

    process->setProgram(cmd);

    QString args = QString("%1@%2:/ %3 "
                           "-o port=%4 "
                           "-f "
                           "-o reconnect "
                           "-o rellinks "
                           "-o fstypename=SSHFS "
                           "-o ssh_command=/usr/bin/ssh.exe "
                           "-o UserKnownHostsFile=/dev/null "
                           "-o StrictHostKeyChecking=no "
                           "-o password_stdin")
                           .arg(username, hostname, mountPath, port);

    process->setArguments(args.split(" ", Qt::SkipEmptyParts));
    process->start();
    process->waitForStarted(50);
    if (process->state() != QProcess::Running) {
        qDebug() << "mountSftpDrive process not started";
        qDebug() << args;
        return ErrorCode::ServerContainerMissingError;
    } else {
        process->write((password + "\n").toUtf8());
    }

    m_sftpMountProcesses.append(process);
#else
    Q_UNUSED(mountPath);
    Q_UNUSED(cmd);
    Q_UNUSED(password);
    return ErrorCode::NoError;
#endif

    return ErrorCode::NoError;
}

void InstallController::stopAllSftpMounts()
{
#ifdef Q_OS_WINDOWS
    for (QSharedPointer<QProcess> process : m_sftpMountProcesses) {
        Utils::signalCtrl(process->processId(), CTRL_C_EVENT);
        process->kill();
        process->waitForFinished();
    }
    m_sftpMountProcesses.clear();
#endif
}

void InstallController::updateContainerConfigAfterInstallation(DockerContainer container, ContainerConfig &containerConfig, const QString &stdOut)
{
    Proto mainProto = ContainerUtils::defaultProtocol(container);

    if (container == DockerContainer::TorWebSite) {
        if (auto* torProtocolConfig = containerConfig.getTorProtocolConfig()) {
            qDebug() << "amnezia-tor onions" << stdOut;

            QString onion = stdOut;
            onion.replace("\n", "");
            torProtocolConfig->serverConfig.site = onion;
        }
    } else if (container == DockerContainer::MtProxy) {
        if (auto* mtProxyConfig = containerConfig.getMtProxyProtocolConfig()) {
            qDebug() << "amnezia mtproxy" << stdOut;

            static const QRegularExpression reSecret(
                    QStringLiteral(R"(\[\*\]\s+Secret:\s+([0-9a-fA-F]{32}))"),
                    QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression reTgLink(QStringLiteral(R"(\[\*\]\s+tg://\s+link:\s+(tg://proxy\?[^\s]+))"));
            static const QRegularExpression reTmeLink(
                    QStringLiteral(R"(\[\*\]\s+t\.me\s+link:\s+(https://t\.me/proxy\?[^\s]+))"));

            const QRegularExpressionMatch mSecret = reSecret.match(stdOut);
            const QRegularExpressionMatch mTgLink = reTgLink.match(stdOut);
            const QRegularExpressionMatch mTmeLink = reTmeLink.match(stdOut);

            if (mSecret.hasMatch()) {
                mtProxyConfig->secret = mSecret.captured(1);
            }
            if (mTgLink.hasMatch()) {
                mtProxyConfig->tgLink = mTgLink.captured(1);
            }
            if (mTmeLink.hasMatch()) {
                mtProxyConfig->tmeLink = mTmeLink.captured(1);
            }
        }
    } else if (container == DockerContainer::Telemt) {
        if (auto *telemtConfig = containerConfig.getTelemtProtocolConfig()) {
            qDebug() << "amnezia-telemt configure stdout" << stdOut;

            static const QRegularExpression reSecret(
                    QStringLiteral(R"(\[\*\]\s+Secret:\s+([0-9a-fA-F]{32}))"),
                    QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression reTgLink(QStringLiteral(R"(\[\*\]\s+tg://\s+link:\s+(tg://proxy\?[^\s]+))"));
            static const QRegularExpression reTmeLink(
                    QStringLiteral(R"(\[\*\]\s+t\.me\s+link:\s+(https://t\.me/proxy\?[^\s]+))"));

            const QRegularExpressionMatch mSecret = reSecret.match(stdOut);
            const QRegularExpressionMatch mTgLink = reTgLink.match(stdOut);
            const QRegularExpressionMatch mTmeLink = reTmeLink.match(stdOut);

            if (mSecret.hasMatch()) {
                telemtConfig->secret = mSecret.captured(1);
            }
            if (mTgLink.hasMatch()) {
                telemtConfig->tgLink = mTgLink.captured(1);
            }
            if (mTmeLink.hasMatch()) {
                telemtConfig->tmeLink = mTmeLink.captured(1);
            }
        }
    } else if (container == DockerContainer::TProxy) {
        if (auto *tProxyConfig = containerConfig.getTProxyProtocolConfig()) {
            static const QRegularExpression reSecret(
                    QStringLiteral(R"(\[\*\]\s+Secret:\s+([0-9a-fA-F]{32}))"),
                    QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression reTgLink(QStringLiteral(R"(\[\*\]\s+tg://\s+link:\s+(tg://webproxy\?[^\s]+))"));
            static const QRegularExpression reTmeLink(
                    QStringLiteral(R"(\[\*\]\s+t\.me\s+link:\s+(https://t\.me/webproxy\?[^\s]+))"));
            static const QRegularExpression reHost(QStringLiteral(R"(\[\*\]\s+Hostname:\s+(\S+))"));

            const QRegularExpressionMatch mSecret = reSecret.match(stdOut);
            const QRegularExpressionMatch mTgLink = reTgLink.match(stdOut);
            const QRegularExpressionMatch mTmeLink = reTmeLink.match(stdOut);
            const QRegularExpressionMatch mHost = reHost.match(stdOut);

            if (mSecret.hasMatch()) {
                tProxyConfig->secret = mSecret.captured(1);
            }
            if (mTgLink.hasMatch()) {
                tProxyConfig->tgLink = mTgLink.captured(1);
            }
            if (mTmeLink.hasMatch()) {
                tProxyConfig->tmeLink = mTmeLink.captured(1);
            }
            if (mHost.hasMatch() && tProxyConfig->hostname.isEmpty()) {
                tProxyConfig->hostname = mHost.captured(1);
            }
        }
    }
}

ErrorCode InstallController::getAlreadyInstalledContainers(const ServerCredentials &credentials,
                                                           QMap<DockerContainer, ContainerConfig> &installedContainers, SshSession &sshSession)
{
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };
    auto cbReadStdErr = [&](const QString &data, libssh::Client &) {
        stdOut += data + "\n";
        return ErrorCode::NoError;
    };

    QString script = QString("sudo docker ps --format '{{.Names}} {{.Ports}}'");
    ErrorCode errorCode = sshSession.runScript(credentials, script, cbReadStdOut, cbReadStdErr);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }

    const static QRegularExpression containerAndPortRegExp("(amnezia[-a-z0-9]*).*?:([0-9]*)->[0-9]*/(udp|tcp).*");
    const static QRegularExpression torOrDnsRegExp("(amnezia-(?:torwebsite|dns)).*?([0-9]*)/(udp|tcp).*");

    QStringList containerInfos = stdOut.split("\n");
    for (const QString &containerInfo : containerInfos) {
        if (containerInfo.isEmpty()) {
            continue;
        }

        QRegularExpressionMatch containerAndPortMatch = containerAndPortRegExp.match(containerInfo);
        if (containerAndPortMatch.hasMatch()) {
            QString name = containerAndPortMatch.captured(1);
            QString portStr = containerAndPortMatch.captured(2);
            QString transportProtoStr = containerAndPortMatch.captured(3);
            DockerContainer container = ContainerUtils::containerFromString(name);

            if (container == DockerContainer::None || ContainerUtils::isUnsupportedContainer(container)) {
                continue;
            }

            int port = portStr.toInt();
            TransportProto transportProto = ProtocolUtils::transportProtoFromString(transportProtoStr);

            auto installer = createInstaller(container);
            ContainerConfig config = installer->createBaseConfig(container, port, transportProto);
            if (container == DockerContainer::TProxy) {
                if (auto *tProxyConfig = config.getTProxyProtocolConfig()) {
                    TProxyInstaller::applyDockerPublishedPorts(containerInfo, *tProxyConfig);
                }
            }
            ErrorCode extractError = installer->extractConfigFromContainer(container, credentials, &sshSession, config);

            if (extractError != ErrorCode::NoError && extractError != ErrorCode::ServerContainerMissingError) {
                return extractError;
            }

            installedContainers.insert(container, config);
        }

        QRegularExpressionMatch torOrDnsRegMatch = torOrDnsRegExp.match(containerInfo);
        if (torOrDnsRegMatch.hasMatch()) {
            QString name = torOrDnsRegMatch.captured(1);
            QString portStr = torOrDnsRegMatch.captured(2);
            QString transportProtoStr = torOrDnsRegMatch.captured(3);
            DockerContainer container = ContainerUtils::containerFromString(name);

            if (container == DockerContainer::None || ContainerUtils::isUnsupportedContainer(container)) {
                continue;
            }

            int port = portStr.toInt();
            TransportProto transportProto = ProtocolUtils::transportProtoFromString(transportProtoStr);

            auto installer = createInstaller(container);
            ContainerConfig config = installer->createBaseConfig(container, port, transportProto);
            if (container == DockerContainer::TProxy) {
                if (auto *tProxyConfig = config.getTProxyProtocolConfig()) {
                    TProxyInstaller::applyDockerPublishedPorts(containerInfo, *tProxyConfig);
                }
            }
            ErrorCode extractError = installer->extractConfigFromContainer(container, credentials, &sshSession, config);

            if (extractError != ErrorCode::NoError && extractError != ErrorCode::ServerContainerMissingError) {
                return extractError;
            }

            installedContainers.insert(container, config);
        }
    }

    return ErrorCode::NoError;
}

ErrorCode InstallController::setDockerContainerEnabledState(const QString &serverId, DockerContainer container, bool enabled)
{
    if (container != DockerContainer::MtProxy && container != DockerContainer::Telemt
            && container != DockerContainer::TProxy) {
        return ErrorCode::InternalError;
    }
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    const QString containerName = ContainerUtils::containerToString(container);
    SshSession sshSession;
    const QString script = enabled ? QStringLiteral("sudo docker start %1").arg(containerName)
                                   : QStringLiteral("sudo docker stop %1").arg(containerName);
    const ErrorCode runError = sshSession.runScript(credentials, script);
    if (runError != ErrorCode::NoError) {
        return runError;
    }
    ContainerConfig currentConfig = adminConfig->containerConfig(container);
    bool persist = false;
    if (auto *mtConfig = currentConfig.getMtProxyProtocolConfig()) {
        mtConfig->isEnabled = enabled;
        persist = true;
    } else if (auto *telemtConfig = currentConfig.getTelemtProtocolConfig()) {
        telemtConfig->isEnabled = enabled;
        persist = true;
    } else if (auto *tProxyConfig = currentConfig.getTProxyProtocolConfig()) {
        tProxyConfig->isEnabled = enabled;
        persist = true;
    }
    if (persist) {
        adminConfig->updateContainerConfig(container, currentConfig);
        m_serversRepository->editServer(serverId, adminConfig->toJson(), serverConfigUtils::ConfigType::SelfHostedAdmin);
    }
    return ErrorCode::NoError;
}

ErrorCode InstallController::queryDockerContainerStatus(const QString &serverId, DockerContainer container, int &statusOut)
{
    statusOut = 3;
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    const QString containerName = ContainerUtils::containerToString(container);
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };
    SshSession sshSession;
    const QString script = QStringLiteral(
            "sudo docker inspect --format '{{.State.Status}}' %1 2>/dev/null || echo 'not_found'")
            .arg(containerName);
    const ErrorCode errorCode = sshSession.runScript(credentials, script, cbReadStdOut);
    if (errorCode != ErrorCode::NoError) {
        return errorCode;
    }
    const QString status = stdOut.trimmed();
    if (status == QLatin1String("running")) {
        statusOut = 1;
    } else if (status == QLatin1String("not_found") || status.isEmpty()) {
        statusOut = 0;
    } else if (status == QLatin1String("exited") || status == QLatin1String("created")
               || status == QLatin1String("paused")) {
        statusOut = 2;
    } else {
        statusOut = 3;
    }
    return ErrorCode::NoError;
}

ErrorCode InstallController::queryMtProxyDiagnostics(const QString &serverId, DockerContainer container, int listenPort,
                                                     MtProxyContainerDiagnostics &out)
{
    out = {};
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return ErrorCode::InternalError;
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return ErrorCode::InternalError;
    }
    SshSession sshSession;
    return MtProxyInstaller::queryDiagnostics(sshSession, credentials, container, listenPort, out);
}

QString InstallController::fetchDockerContainerSecret(const QString &serverId, DockerContainer container)
{
    if (container != DockerContainer::MtProxy && container != DockerContainer::Telemt
            && container != DockerContainer::TProxy) {
        return {};
    }
    auto adminConfig = m_serversRepository->selfHostedAdminConfig(serverId);
    if (!adminConfig.has_value()) {
        return {};
    }
    ServerCredentials credentials = adminConfig->credentials();
    if (!credentials.isValid()) {
        return {};
    }
    const QString containerName = ContainerUtils::containerToString(container);
    QString stdOut;
    auto cbReadStdOut = [&](const QString &data, libssh::Client &) {
        stdOut += data;
        return ErrorCode::NoError;
    };
    SshSession sshSession;
    const QString path = QStringLiteral("/data/secret");
    const QString cmd = QStringLiteral("sudo docker exec %1 cat %2").arg(containerName, path);
    const ErrorCode errorCode = sshSession.runScript(credentials, cmd, cbReadStdOut);
    if (errorCode != ErrorCode::NoError) {
        return {};
    }
    const QString secret = stdOut.trimmed();
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-fA-F]{32}$"));
    return hex32.match(secret).hasMatch() ? secret : QString();
}
