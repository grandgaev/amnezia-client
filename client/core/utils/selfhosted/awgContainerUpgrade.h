#ifndef AWGCONTAINERUPGRADE_H
#define AWGCONTAINERUPGRADE_H

#include <QJsonArray>
#include <QString>

#include "core/utils/containerEnum.h"
#include "core/models/protocols/awgProtocolConfig.h"

// Pure (no SSH, no key generation, no I/O) helpers behind InstallController::upgradeContainer.
// Kept free of SshSession/QtConcurrent on purpose so they can be unit tested directly - see
// client/tests/testContainerUpgrade.cpp.
namespace amnezia
{

// How InstallController::upgradeContainer should treat an existing container. The table of
// which state files matter for which container/mode lives in InstallController; this enum is
// shared with the UI (confirmation drawer) via InstallUiController.
enum class ContainerUpgradeMode {
    // Rebuild the image from a fresh base image and restore every state file verbatim.
    // Nothing changes for any user; safe for every container type.
    RefreshSoftware = 0,
    // RefreshSoftware, plus (AmneziaWG containers only) the server [Interface] gets fresh
    // AmneziaWG 3.1 parameters. Every [Peer] block and the client table are kept in place,
    // but other users' existing configs stop matching the new [Interface] params and must be
    // re-issued - see flagClientsForConfigUpdate() below.
    UpgradeProtocol = 1
};

// The parts of a running container's awg0.conf/wg0.conf an upgrade must carry over untouched:
// the keys/address/port in [Interface], and every [Peer] block verbatim, in their original
// order. isValid is false when PrivateKey/Address/ListenPort couldn't be found, which callers
// must treat as "snapshot failed" and abort the upgrade without changing anything.
struct AwgInterfaceSnapshot {
    QString privateKey;
    QString address; // e.g. "10.8.1.1/24", kept as a single value
    QString listenPort;
    QString peersBlock; // raw text starting at the first "[Peer]" line, through EOF; may be empty
    int peerCount = 0;
    bool isValid = false;
};

// Parses the [Interface] section (and everything from the first [Peer] onward) of a raw
// awg0.conf/wg0.conf. Tolerant of the "# I1 = ..." commented junk lines and of blank/missing
// values (every AWG parameter besides PrivateKey/Address/ListenPort is optional).
AwgInterfaceSnapshot parseAwgInterfaceSnapshot(const QString &rawAwgConfig);

// Rebuilds a full awg0.conf for ContainerUpgradeMode::UpgradeProtocol: PrivateKey, Address,
// ListenPort and every [Peer] block come from `snapshot` verbatim and in order; every other
// [Interface] parameter (Jc/Jmin/Jmax, S1-S4, H1-H4, HeaderProtectionKey, timers, ...) comes
// from `newParams`, in the same shape AwgInstaller::generateAwgParameters() produces for a
// fresh install. Mirrors server_scripts/awg/configure_container.sh's heredoc + the "drop
// empty-valued lines" cleanup, so the file this produces is byte-for-byte what a fresh install
// would generate for [Interface], with the old peers appended. Returns an empty string (and
// changes nothing) when `snapshot` is not valid.
QString buildUpgradedAwgConfig(const AwgInterfaceSnapshot &snapshot, const AwgServerConfig &newParams);

// Re-renders the admin's own AWG client config against `newServerParams`: the same template
// and variable substitution used at connect/export time (see WireguardConfigurator /
// AwgConfigurator), but keeping the client's key pair, tunnel IP, preshared key, server host,
// port and public key untouched. No new peer is created and none is orphaned - this is meant
// to replace the admin's cached client config in-place after a protocol upgrade.
AwgClientConfig reRenderAwgAdminClientConfig(const AwgClientConfig &oldClientConfig, DockerContainer container,
                                              const AwgServerConfig &newServerParams);

// Sets (needsUpdate=true) or clears (needsUpdate=false) userData.needsConfigUpdate on every
// clientsTable entry except the one whose clientId is `adminClientId` (the admin's own config
// is re-rendered automatically by upgradeContainer, so it must never be flagged). Entries that
// aren't JSON objects are passed through unchanged. Pure JSON transform; callers upload the
// result to /opt/amnezia/<container>/clientsTable themselves.
QJsonArray flagClientsForConfigUpdate(const QJsonArray &clientsTable, const QString &adminClientId, bool needsUpdate);

} // namespace amnezia

#endif // AWGCONTAINERUPGRADE_H
