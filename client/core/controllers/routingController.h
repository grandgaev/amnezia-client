#ifndef ROUTINGCONTROLLER_H
#define ROUTINGCONTROLLER_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <optional>

#include "core/models/routing/routingProfile.h"
#include "core/repositories/secureAppSettingsRepository.h"

class QNetworkAccessManager;
class QNetworkReply;

using namespace amnezia;

// Routing profiles: rule based split tunnelling by sites, addresses and
// geosite/geoip categories, modelled after the Happ client.
class RoutingController : public QObject
{
    Q_OBJECT

public:
    enum class ImportStatus {
        Added,
        Updated,
        NameConflict,
        InvalidData
    };

    struct ImportOutcome
    {
        ImportStatus status = ImportStatus::InvalidData;
        QString profileId;
        QString name;
        QJsonObject data; // imported object, kept to resolve a name conflict
        QString error;
    };

    inline static const QString overrideOff = QStringLiteral("off");

    explicit RoutingController(SecureAppSettingsRepository *appSettingsRepository, QObject *parent = nullptr);

    QList<routing::RoutingProfile> profiles() const;
    std::optional<routing::RoutingProfile> profile(const QString &id) const;
    bool isNameTaken(const QString &name, const QString &exceptId = QString()) const;

    // Returns the id of the new profile, or an empty string (error set).
    QString createProfile(const QString &name, QString *error = nullptr);
    QString duplicateProfile(const QString &id);
    bool renameProfile(const QString &id, const QString &name, QString *error = nullptr);
    bool updateProfile(const routing::RoutingProfile &profile);
    bool removeProfile(const QString &id);

    bool isRoutingEnabled() const;
    // Returns false (and does nothing) if there is no profile to enable.
    bool setRoutingEnabled(bool enabled);
    QString selectedProfileId() const;
    void selectProfile(const QString &id);

    // Per server override: "" = default profile, "off" = no routing, or a profile id.
    QString serverOverride(const QString &serverId) const;
    void setServerOverride(const QString &serverId, const QString &value);

    QStringList excludedRoutes() const;
    void setExcludedRoutes(const QStringList &routes);

    // Import of routing profiles from JSON, base64 or Happ routing links.
    QList<ImportOutcome> importData(const QString &data, bool activate);
    ImportOutcome importObject(const QJsonObject &object, bool replaceExisting, bool activate);
    QString exportProfile(const QString &id) const;

    // Geo files
    static QString profileDirectory(const QString &profileId);
    // Path of the geo file used for a profile: its own downloaded file, or the
    // file bundled with the application. Empty if none is available.
    static QString geoFilePath(const QString &profileId, bool geoIp);
    static QString bundledGeoFilePath(bool geoIp);
    void updateGeoFiles(const QString &id, bool geoSite = true, bool geoIp = true);
    void ensureGeoFiles(const QString &id);
    bool isDownloading(const QString &id) const;
    int downloadProgress(const QString &id) const;
    QString geoUserAgent() const;
    void setGeoUserAgent(const QString &userAgent);
    static QStringList geoUserAgentNames();

    void migrateLegacySplitTunneling();

    // Profile applied to a connection with the given server ("" = default server),
    // considering the global switch and the per server override.
    static std::optional<routing::RoutingProfile> activeProfile(const SecureAppSettingsRepository *repository, const QString &serverId);

signals:
    void profilesChanged();
    void profileChanged(const QString &id);
    void routingEnabledChanged(bool enabled);
    void selectedProfileChanged(const QString &id);
    void excludedRoutesChanged();
    void serverOverridesChanged();
    void geoDownloadStateChanged(const QString &id);
    void geoDownloadFinished(const QString &id, bool success, const QString &message);

private:
    struct Download
    {
        QPointer<QNetworkReply> reply;
        bool geoIp = false;
        qint64 received = 0;
        qint64 total = 0;
    };

    void saveProfiles(const QList<routing::RoutingProfile> &profiles);
    QString uniqueName(const QString &base) const;
    void startDownload(const routing::RoutingProfile &profile, bool geoIp);
    void onDownloadFinished(const QString &profileId, bool geoIp, QNetworkReply *reply);

    SecureAppSettingsRepository *m_appSettingsRepository;
    QNetworkAccessManager *m_network = nullptr;
    QHash<QString, QList<Download>> m_downloads;
};

#endif // ROUTINGCONTROLLER_H
