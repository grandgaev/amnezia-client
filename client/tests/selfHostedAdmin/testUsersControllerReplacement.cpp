#include <QTest>

#include "core/controllers/selfhosted/usersController.h"

class TestUsersControllerReplacement : public QObject
{
    Q_OBJECT

private slots:
    void removesOnlyExactPeerPublicKey()
    {
        const QString config = QStringLiteral(
                "[Interface]\nPrivateKey = server\n\n"
                "[Peer]\nPublicKey = old-key-extra\nAllowedIPs = 10.0.0.2/32\n\n"
                "[Peer]\nPublicKey = old-key\nPresharedKey = contains-old-key\nAllowedIPs = 10.0.0.3/32\n\n"
                "[Peer]\nPublicKey = other-key\nAllowedIPs = 10.0.0.4/32\n");

        const auto result = UsersController::removeWireGuardPeer(config, QStringLiteral("old-key"));

        QCOMPARE(result.matches, 1);
        QVERIFY(!result.config.contains(QStringLiteral("PublicKey = old-key\n")));
        QVERIFY(result.config.contains(QStringLiteral("PublicKey = old-key-extra")));
        QVERIFY(result.config.contains(QStringLiteral("PublicKey = other-key")));
    }

    void reportsDuplicateExactPeers()
    {
        const QString config = QStringLiteral(
                "[Interface]\nPrivateKey = server\n\n"
                "[Peer]\nPublicKey=duplicate-key\nAllowedIPs=10.0.0.2/32\n\n"
                "[Peer]\nPublicKey = duplicate-key\nAllowedIPs = 10.0.0.3/32\n");

        const auto result = UsersController::removeWireGuardPeer(config, QStringLiteral("duplicate-key"));

        QCOMPARE(result.matches, 2);
        QVERIFY(!result.config.contains(QStringLiteral("duplicate-key")));
    }
};

QTEST_MAIN(TestUsersControllerReplacement)

#include "testUsersControllerReplacement.moc"
