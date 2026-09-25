#ifndef AWGINSTALLER_H
#define AWGINSTALLER_H

#include "installerBase.h"

class AwgInstaller : public InstallerBase
{
    Q_OBJECT
public:
    explicit AwgInstaller(QObject *parent = nullptr);

    amnezia::ContainerConfig generateConfig(amnezia::DockerContainer container, int port, amnezia::TransportProto transportProto) override;
    amnezia::ErrorCode extractConfigFromContainer(amnezia::DockerContainer container, const amnezia::ServerCredentials &credentials,
                                         SshSession* serverController, amnezia::ContainerConfig &config) override;

    // The same [Interface] parameter set a fresh install picks, exposed so a protocol
    // upgrade of an existing container (see InstallController::upgradeContainer) can reuse
    // it verbatim instead of duplicating the defaults. Pure/testable: no I/O.
    static void generateAwgParameters(amnezia::AwgServerConfig &serverConfig);
};

#endif // AWGINSTALLER_H

