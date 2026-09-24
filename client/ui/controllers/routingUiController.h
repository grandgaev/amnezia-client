#ifndef ROUTINGUICONTROLLER_H
#define ROUTINGUICONTROLLER_H

#include <QFutureWatcher>
#include <QJsonObject>
#include <QObject>
#include <QSet>

#include "core/controllers/routingController.h"
#include "core/controllers/serversController.h"
#include "ui/models/geoTagsModel.h"
#include "ui/models/routingProfilesModel.h"

class QNetworkAccessManager;

// QML facade of the routing profiles ("RoutingController" in QML).
class RoutingUiController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool routingEnabled READ isRoutingEnabled NOTIFY routingEnabledChanged)
    Q_PROPERTY(QString selectedProfileId READ selectedProfileId NOTIFY selectedProfileChanged)
    Q_PROPERTY(QString selectedProfileName READ selectedProfileName NOTIFY selectedProfileChanged)
    Q_PROPERTY(int profilesCount READ profilesCount NOTIFY profilesChanged)
    Q_PROPERTY(QString excludedRoutesText READ excludedRoutesText NOTIFY excludedRoutesChanged)
    Q_PROPERTY(QStringList geoUserAgents READ geoUserAgents CONSTANT)
    Q_PROPERTY(int geoUserAgentIndex READ geoUserAgentIndex NOTIFY geoUserAgentChanged)
    Q_PROPERTY(QStringList routeOrderNames READ routeOrderNames CONSTANT)
    Q_PROPERTY(QStringList domainStrategies READ domainStrategies CONSTANT)
    Q_PROPERTY(QString protocolSupportText READ protocolSupportText NOTIFY protocolSupportChanged)
    Q_PROPERTY(bool isAppRoutingSupported READ isAppRoutingSupported CONSTANT)

    // The profile opened in the editor pages.
    Q_PROPERTY(QString currentProfileId READ currentProfileId WRITE setCurrentProfileId NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentProfileChanged)
    Q_PROPERTY(bool currentGlobalProxy READ currentGlobalProxy NOTIFY currentProfileChanged)
    Q_PROPERTY(int currentRouteOrderIndex READ currentRouteOrderIndex NOTIFY currentProfileChanged)
    Q_PROPERTY(int currentDomainStrategyIndex READ currentDomainStrategyIndex NOTIFY currentProfileChanged)
    Q_PROPERTY(bool currentFakeDns READ currentFakeDns NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentRemoteDnsType READ currentRemoteDnsType NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentRemoteDnsDomain READ currentRemoteDnsDomain NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentRemoteDnsIp READ currentRemoteDnsIp NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentDomesticDnsType READ currentDomesticDnsType NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentDomesticDnsDomain READ currentDomesticDnsDomain NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentDomesticDnsIp READ currentDomesticDnsIp NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentGeoSiteUrl READ currentGeoSiteUrl NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentGeoIpUrl READ currentGeoIpUrl NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentGeoSiteStatus READ currentGeoSiteStatus NOTIFY currentGeoStateChanged)
    Q_PROPERTY(QString currentGeoIpStatus READ currentGeoIpStatus NOTIFY currentGeoStateChanged)
    Q_PROPERTY(QString currentGeoError READ currentGeoError NOTIFY currentGeoStateChanged)
    Q_PROPERTY(bool currentDownloading READ currentDownloading NOTIFY currentGeoStateChanged)
    Q_PROPERTY(bool geoTagsLoading READ geoTagsLoading NOTIFY geoTagsLoadingChanged)
    // Rule group (RuleActionType) opened in the rules editor page.
    Q_PROPERTY(int currentRuleAction READ currentRuleAction WRITE setCurrentRuleAction NOTIFY currentRuleActionChanged)
    // true when settings used by the active connection were changed while it was connected.
    Q_PROPERTY(bool reconnectRequired READ reconnectRequired NOTIFY reconnectRequiredChanged)

public:
    enum RuleActionType {
        Proxy = 0,
        Direct = 1,
        Block = 2
    };
    Q_ENUM(RuleActionType)

    RoutingUiController(RoutingController *routingController, ServersController *serversController,
                        RoutingProfilesModel *profilesModel, GeoTagsModel *geoTagsModel, QObject *parent = nullptr);

public slots:
    bool isRoutingEnabled() const;
    void setRoutingEnabled(bool enabled);
    QString selectedProfileId() const;
    QString selectedProfileName() const;
    int profilesCount() const;
    void selectProfile(const QString &id);

    QString createProfile(const QString &name);
    void removeProfile(const QString &id);
    QString duplicateProfile(const QString &id);
    bool renameProfile(const QString &id, const QString &name);
    QString profileName(const QString &id) const;

    // Import from the clipboard, a local file or an http(s) URL.
    void importFromClipboard();
    void importFromText(const QString &text);
    void importFromFile(const QString &fileName);
    void importFromUrl(const QString &url);
    void resolveImportConflict(bool replace);
    void exportToClipboard(const QString &id);
    void exportToFile(const QString &id, const QString &fileName);

    QString currentProfileId() const;
    void setCurrentProfileId(const QString &id);
    QString currentName() const;
    bool currentGlobalProxy() const;
    int currentRouteOrderIndex() const;
    int currentDomainStrategyIndex() const;
    bool currentFakeDns() const;
    QString currentRemoteDnsType() const;
    QString currentRemoteDnsDomain() const;
    QString currentRemoteDnsIp() const;
    QString currentDomesticDnsType() const;
    QString currentDomesticDnsDomain() const;
    QString currentDomesticDnsIp() const;
    QString currentGeoSiteUrl() const;
    QString currentGeoIpUrl() const;
    QString currentGeoSiteStatus() const;
    QString currentGeoIpStatus() const;
    QString currentGeoError() const;
    bool currentDownloading() const;

    void setGlobalProxy(bool enabled);
    void setRouteOrderIndex(int index);
    void setDomainStrategyIndex(int index);
    void setFakeDns(bool enabled);
    void setRemoteDns(const QString &type, const QString &domain, const QString &ip);
    void setDomesticDns(const QString &type, const QString &domain, const QString &ip);
    void setGeoSiteUrl(const QString &url);
    void setGeoIpUrl(const QString &url);
    void updateGeoFiles();

    QString rulesText(int action) const;
    // Returns a warning text for suspicious entries (empty if everything is fine).
    QString setRulesText(int action, const QString &text);
    QString lanAddressesText() const;
    int rulesCount(int action) const;

    void loadGeoTags(bool geoIp, const QString &currentText);
    bool geoTagsLoading() const;
    // Merges the tags selected in GeoTagsModel into the rules text: adds the selected
    // tags that are missing and removes the listed tags that were deselected.
    QString applyGeoTags(const QString &currentText) const;

    int currentRuleAction() const;
    void setCurrentRuleAction(int action);

    bool reconnectRequired() const;
    // Called with the connection state (true = connected). A new state clears reconnectRequired.
    void setConnectionActive(bool active);
    // Marks a change of related settings made elsewhere (e.g. app split tunneling).
    void markReconnectRequired();

    QString excludedRoutesText() const;
    QString setExcludedRoutesText(const QString &text);

    QStringList geoUserAgents() const;
    int geoUserAgentIndex() const;
    void setGeoUserAgentIndex(int index);
    QStringList routeOrderNames() const;
    QStringList domainStrategies() const;

    QString protocolSupportText() const;
    bool isAppRoutingSupported() const;

    // Per server override
    QString serverRoutingValue(const QString &serverId) const;
    void setServerRoutingValue(const QString &serverId, const QString &value);
    QString serverRoutingDescription(const QString &serverId) const;
    QStringList profileIds() const;

signals:
    void routingEnabledChanged();
    void selectedProfileChanged();
    void profilesChanged();
    void excludedRoutesChanged();
    void geoUserAgentChanged();
    void protocolSupportChanged();
    void currentProfileChanged();
    void currentGeoStateChanged();
    void geoTagsLoadingChanged();
    void geoTagsLoaded(bool success, const QString &message);
    void settingsChangedWhileConnected();
    void currentRuleActionChanged();
    void reconnectRequiredChanged();

    void importConflict(const QString &name);
    void profileImported(const QString &id, const QString &message);
    void errorOccurred(const QString &message);
    void finished(const QString &message);

private:
    void updateModel();
    void handleImportOutcomes(const QList<RoutingController::ImportOutcome> &outcomes);
    bool modifyCurrent(const std::function<void(amnezia::routing::RoutingProfile &)> &change);
    std::optional<amnezia::routing::RoutingProfile> current() const;
    QString geoStatus(bool geoIp) const;
    // Snapshot of the routing settings used by a connection to the default server.
    QString activeStateFingerprint() const;
    bool affectsActiveConnection(const QString &profileId) const;
    void markIfChanged(const QString &fingerprintBefore);

    RoutingController *m_routingController;
    ServersController *m_serversController;
    RoutingProfilesModel *m_profilesModel;
    GeoTagsModel *m_geoTagsModel;
    QNetworkAccessManager *m_network = nullptr;

    QString m_currentProfileId;
    QList<QJsonObject> m_pendingConflicts;
    QFutureWatcher<QStringList> m_geoTagsWatcher;
    QStringList m_geoTagsSelected;
    bool m_geoTagsLoading = false;
    int m_currentRuleAction = Proxy;
    bool m_connectionActive = false;
    bool m_reconnectRequired = false;
    QSet<QString> m_userGeoUpdates;
};

#endif // ROUTINGUICONTROLLER_H
