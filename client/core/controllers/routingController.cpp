#include "routingController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include "core/models/routing/geoData.h"
#include "logger.h"

using namespace amnezia::routing;

namespace
{
    Logger logger("RoutingController");

    const QHash<QString, QString> &userAgents()
    {
        static const QHash<QString, QString> agents = {
            { QStringLiteral("chrome-android"),
              QStringLiteral("Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Mobile "
                             "Safari/537.36") },
            { QStringLiteral("chrome-win"),
              QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 "
                             "Safari/537.36") },
            { QStringLiteral("safari-mac"),
              QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 "
                             "Safari/605.1.15") },
            { QStringLiteral("safari-ios"),
              QStringLiteral("Mozilla/5.0 (iPhone; CPU iPhone OS 17_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
                             "Version/17.6 Mobile/15E148 Safari/604.1") },
            { QStringLiteral("firefox-win"),
              QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:130.0) Gecko/20100101 Firefox/130.0") },
        };
        return agents;
    }

    QList<RoutingProfile> loadProfiles(const SecureAppSettingsRepository *repository)
    {
        QList<RoutingProfile> result;
        const QJsonArray array = repository->routingProfiles();
        for (const QJsonValue &value : array) {
            if (value.isObject()) {
                result.append(RoutingProfile::fromJson(value.toObject()));
            }
        }
        return result;
    }

    QString hostOrAddress(const QString &site)
    {
        QString s = site.trimmed();
        s.remove(QStringLiteral("https://"));
        s.remove(QStringLiteral("http://"));
        const int slash = s.indexOf(QLatin1Char('/'));
        if (slash > 0 && !isIpRule(s)) {
            s = s.left(slash);
        }
        return s;
    }
} // namespace

RoutingController::RoutingController(SecureAppSettingsRepository *appSettingsRepository, QObject *parent)
    : QObject(parent), m_appSettingsRepository(appSettingsRepository)
{
    connect(m_appSettingsRepository, &SecureAppSettingsRepository::settingsCleared, this, [this]() {
        emit profilesChanged();
        emit routingEnabledChanged(isRoutingEnabled());
        emit selectedProfileChanged(selectedProfileId());
        emit excludedRoutesChanged();
    });
}

QList<RoutingProfile> RoutingController::profiles() const
{
    return loadProfiles(m_appSettingsRepository);
}

std::optional<RoutingProfile> RoutingController::profile(const QString &id) const
{
    for (const RoutingProfile &p : profiles()) {
        if (p.id == id) {
            return p;
        }
    }
    return std::nullopt;
}

bool RoutingController::isNameTaken(const QString &name, const QString &exceptId) const
{
    const QString trimmed = name.trimmed();
    for (const RoutingProfile &p : profiles()) {
        if (p.id != exceptId && p.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString RoutingController::uniqueName(const QString &base) const
{
    QString name = base.trimmed().isEmpty() ? tr("Profile") : base.trimmed();
    if (!isNameTaken(name)) {
        return name;
    }
    for (int i = 2;; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(name).arg(i);
        if (!isNameTaken(candidate)) {
            return candidate;
        }
    }
}

void RoutingController::saveProfiles(const QList<RoutingProfile> &profiles)
{
    QJsonArray array;
    for (const RoutingProfile &p : profiles) {
        array.append(p.toJson());
    }
    m_appSettingsRepository->setRoutingProfiles(array);
    emit profilesChanged();
}

QString RoutingController::createProfile(const QString &name, QString *error)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (error) {
            *error = tr("Profile name is empty");
        }
        return QString();
    }
    if (isNameTaken(trimmed)) {
        if (error) {
            *error = tr("Profile name is already in use. Please choose another");
        }
        return QString();
    }
    QList<RoutingProfile> list = profiles();
    RoutingProfile p = RoutingProfile::createDefault(trimmed);
    list.append(p);
    saveProfiles(list);
    if (list.size() == 1) {
        // The first profile becomes the active one.
        selectProfile(p.id);
        setRoutingEnabled(true);
    }
    return p.id;
}

QString RoutingController::duplicateProfile(const QString &id)
{
    auto source = profile(id);
    if (!source) {
        return QString();
    }
    RoutingProfile copy = *source;
    copy.id = RoutingProfile::createDefault(QString()).id;
    copy.name = uniqueName(source->name);
    QList<RoutingProfile> list = profiles();
    list.append(copy);
    saveProfiles(list);

    // Reuse the downloaded geo files of the original profile.
    const QDir sourceDir(profileDirectory(id));
    const QString targetDir = profileDirectory(copy.id);
    for (const QString &file : { QStringLiteral("geosite.dat"), QStringLiteral("geoip.dat") }) {
        if (sourceDir.exists(file)) {
            QDir().mkpath(targetDir);
            QFile::copy(sourceDir.filePath(file), targetDir + QLatin1Char('/') + file);
        }
    }
    return copy.id;
}

bool RoutingController::renameProfile(const QString &id, const QString &name, QString *error)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (error) {
            *error = tr("Profile name is empty");
        }
        return false;
    }
    if (isNameTaken(trimmed, id)) {
        if (error) {
            *error = tr("Profile name is already in use. Please choose another");
        }
        return false;
    }
    auto p = profile(id);
    if (!p) {
        return false;
    }
    p->name = trimmed;
    return updateProfile(*p);
}

bool RoutingController::updateProfile(const RoutingProfile &profile)
{
    QList<RoutingProfile> list = profiles();
    for (RoutingProfile &p : list) {
        if (p.id == profile.id) {
            p = profile;
            saveProfiles(list);
            emit profileChanged(profile.id);
            return true;
        }
    }
    return false;
}

bool RoutingController::removeProfile(const QString &id)
{
    QList<RoutingProfile> list = profiles();
    const auto removed = std::remove_if(list.begin(), list.end(), [&id](const RoutingProfile &p) { return p.id == id; });
    if (removed == list.end()) {
        return false;
    }
    list.erase(removed, list.end());

    for (Download &download : m_downloads.take(id)) {
        if (download.reply) {
            download.reply->abort();
        }
    }
    QDir(profileDirectory(id)).removeRecursively();

    saveProfiles(list);

    QVariantMap overrides = m_appSettingsRepository->serverRoutingOverrides();
    bool overridesChanged = false;
    for (auto it = overrides.begin(); it != overrides.end();) {
        if (it.value().toString() == id) {
            it = overrides.erase(it);
            overridesChanged = true;
        } else {
            ++it;
        }
    }
    if (overridesChanged) {
        m_appSettingsRepository->setServerRoutingOverrides(overrides);
        emit serverOverridesChanged();
    }

    if (selectedProfileId() == id) {
        if (list.isEmpty()) {
            m_appSettingsRepository->setSelectedRoutingProfileId(QString());
            emit selectedProfileChanged(QString());
            setRoutingEnabled(false);
        } else {
            selectProfile(list.first().id);
        }
    }
    return true;
}

bool RoutingController::isRoutingEnabled() const
{
    return m_appSettingsRepository->isRoutingEnabled();
}

bool RoutingController::setRoutingEnabled(bool enabled)
{
    if (enabled && profiles().isEmpty()) {
        return false;
    }
    if (enabled && !profile(selectedProfileId())) {
        selectProfile(profiles().first().id);
    }
    if (m_appSettingsRepository->isRoutingEnabled() != enabled) {
        m_appSettingsRepository->setRoutingEnabled(enabled);
        emit routingEnabledChanged(enabled);
    }
    return true;
}

QString RoutingController::selectedProfileId() const
{
    return m_appSettingsRepository->selectedRoutingProfileId();
}

void RoutingController::selectProfile(const QString &id)
{
    if (!profile(id) || selectedProfileId() == id) {
        return;
    }
    m_appSettingsRepository->setSelectedRoutingProfileId(id);
    emit selectedProfileChanged(id);
    ensureGeoFiles(id);
}

QString RoutingController::serverOverride(const QString &serverId) const
{
    return m_appSettingsRepository->serverRoutingOverrides().value(serverId).toString();
}

void RoutingController::setServerOverride(const QString &serverId, const QString &value)
{
    QVariantMap overrides = m_appSettingsRepository->serverRoutingOverrides();
    if (value.isEmpty()) {
        overrides.remove(serverId);
    } else {
        overrides.insert(serverId, value);
    }
    m_appSettingsRepository->setServerRoutingOverrides(overrides);
    emit serverOverridesChanged();
    if (value != overrideOff && !value.isEmpty()) {
        ensureGeoFiles(value);
    }
}

QStringList RoutingController::excludedRoutes() const
{
    return m_appSettingsRepository->routingExcludedRoutes();
}

void RoutingController::setExcludedRoutes(const QStringList &routes)
{
    m_appSettingsRepository->setRoutingExcludedRoutes(routes);
    emit excludedRoutesChanged();
}

RoutingController::ImportOutcome RoutingController::importObject(const QJsonObject &object, bool replaceExisting, bool activate)
{
    ImportOutcome outcome;
    outcome.data = object;

    RoutingProfile imported = RoutingProfile::createDefault(QString());
    imported.mergeExchangeJson(object);
    outcome.name = imported.name;

    QList<RoutingProfile> list = profiles();
    for (RoutingProfile &existing : list) {
        if (existing.name.compare(imported.name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!replaceExisting) {
            outcome.status = ImportStatus::NameConflict;
            outcome.profileId = existing.id;
            return outcome;
        }
        const QString oldGeoSite = existing.geoSiteUrl;
        const QString oldGeoIp = existing.geoIpUrl;
        existing.mergeExchangeJson(object);
        saveProfiles(list);
        emit profileChanged(existing.id);
        outcome.status = ImportStatus::Updated;
        outcome.profileId = existing.id;
        if (existing.geoSiteUrl != oldGeoSite || existing.geoIpUrl != oldGeoIp) {
            updateGeoFiles(existing.id, existing.geoSiteUrl != oldGeoSite, existing.geoIpUrl != oldGeoIp);
        }
        if (activate) {
            selectProfile(existing.id);
            setRoutingEnabled(true);
        }
        return outcome;
    }

    list.append(imported);
    saveProfiles(list);
    outcome.status = ImportStatus::Added;
    outcome.profileId = imported.id;
    if (activate || list.size() == 1) {
        selectProfile(imported.id);
        setRoutingEnabled(true);
    }
    ensureGeoFiles(imported.id);
    return outcome;
}

QList<RoutingController::ImportOutcome> RoutingController::importData(const QString &data, bool activate)
{
    QList<ImportOutcome> outcomes;
    QString error;
    const QList<QJsonObject> objects = parseImportData(data, &error);
    if (objects.isEmpty()) {
        ImportOutcome outcome;
        outcome.status = ImportStatus::InvalidData;
        outcome.error = error.isEmpty() ? tr("Wrong data for import routing profile") : error;
        outcomes.append(outcome);
        return outcomes;
    }
    // A Happ "onadd" link activates the profile.
    const bool activateLink = data.trimmed().startsWith(QLatin1String("happ://routing/onadd/"), Qt::CaseInsensitive);
    for (const QJsonObject &object : objects) {
        outcomes.append(importObject(object, false, activate || activateLink));
    }
    return outcomes;
}

QString RoutingController::exportProfile(const QString &id) const
{
    auto p = profile(id);
    if (!p) {
        return QString();
    }
    return QString::fromUtf8(QJsonDocument(p->toExchangeJson()).toJson(QJsonDocument::Indented));
}

QString RoutingController::profileDirectory(const QString &profileId)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/routing/") + profileId;
}

QString RoutingController::bundledGeoFilePath(bool geoIp)
{
    const QString file = geoIp ? QStringLiteral("geoip.dat") : QStringLiteral("geosite.dat");
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QLatin1Char('/') + file,
        appDir + QStringLiteral("/../Resources/") + file,
        appDir + QStringLiteral("/../service/") + file,
        appDir + QStringLiteral("/../../service/") + file,
        appDir + QStringLiteral("/../lib/") + file,
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).canonicalFilePath();
        }
    }
    return QString();
}

QString RoutingController::geoFilePath(const QString &profileId, bool geoIp)
{
    const QString own = profileDirectory(profileId) + (geoIp ? QStringLiteral("/geoip.dat") : QStringLiteral("/geosite.dat"));
    if (QFileInfo::exists(own)) {
        return own;
    }
    return bundledGeoFilePath(geoIp);
}

bool RoutingController::isDownloading(const QString &id) const
{
    for (const Download &d : m_downloads.value(id)) {
        if (d.reply) {
            return true;
        }
    }
    return false;
}

int RoutingController::downloadProgress(const QString &id) const
{
    qint64 received = 0;
    qint64 total = 0;
    for (const Download &d : m_downloads.value(id)) {
        if (d.reply) {
            received += d.received;
            total += d.total > 0 ? d.total : 0;
        }
    }
    if (total <= 0) {
        return 0;
    }
    return int(qBound(qint64(0), received * 100 / total, qint64(100)));
}

QString RoutingController::geoUserAgent() const
{
    const QString ua = m_appSettingsRepository->routingGeoUserAgent();
    return userAgents().contains(ua) ? ua : QStringLiteral("chrome-android");
}

void RoutingController::setGeoUserAgent(const QString &userAgent)
{
    m_appSettingsRepository->setRoutingGeoUserAgent(userAgent);
}

QStringList RoutingController::geoUserAgentNames()
{
    return { QStringLiteral("chrome-android"), QStringLiteral("chrome-win"), QStringLiteral("safari-mac"),
             QStringLiteral("safari-ios"), QStringLiteral("firefox-win") };
}

void RoutingController::ensureGeoFiles(const QString &id)
{
    auto p = profile(id);
    if (!p || isDownloading(id)) {
        return;
    }
    const QDir dir(profileDirectory(id));
    const bool needSite = p->usesGeoSite() && !dir.exists(QStringLiteral("geosite.dat"))
            && (p->geoSiteUrl != QLatin1String(defaultGeoSiteUrl) || bundledGeoFilePath(false).isEmpty());
    const bool needIp = p->usesGeoIp() && !dir.exists(QStringLiteral("geoip.dat"))
            && (p->geoIpUrl != QLatin1String(defaultGeoIpUrl) || bundledGeoFilePath(true).isEmpty());
    if (needSite || needIp) {
        updateGeoFiles(id, needSite, needIp);
    }
}

void RoutingController::updateGeoFiles(const QString &id, bool geoSite, bool geoIp)
{
    auto p = profile(id);
    if (!p) {
        return;
    }
    if (geoSite) {
        startDownload(*p, false);
    }
    if (geoIp) {
        startDownload(*p, true);
    }
    emit geoDownloadStateChanged(id);
}

void RoutingController::startDownload(const RoutingProfile &profile, bool geoIp)
{
    QList<Download> &downloads = m_downloads[profile.id];
    for (const Download &d : downloads) {
        if (d.reply && d.geoIp == geoIp) {
            return;
        }
    }
    const QUrl url(geoIp ? profile.geoIpUrl : profile.geoSiteUrl);
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        auto p = this->profile(profile.id);
        if (p) {
            p->geoError = geoIp ? tr("Link to the geoip file is not correct") : tr("Link to the geosite file is not correct");
            updateProfile(*p);
        }
        emit geoDownloadFinished(profile.id, false, geoIp ? tr("Link to the geoip file is not correct")
                                                          : tr("Link to the geosite file is not correct"));
        return;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgents().value(geoUserAgent()));
    request.setTransferTimeout(180000);
    QNetworkReply *reply = m_network->get(request);

    Download download;
    download.reply = reply;
    download.geoIp = geoIp;
    downloads.append(download);

    const QString profileId = profile.id;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, profileId, reply](qint64 received, qint64 total) {
        for (Download &d : m_downloads[profileId]) {
            if (d.reply == reply) {
                d.received = received;
                d.total = total;
            }
        }
        emit geoDownloadStateChanged(profileId);
    });
    connect(reply, &QNetworkReply::finished, this, [this, profileId, geoIp, reply]() { onDownloadFinished(profileId, geoIp, reply); });
    logger.info() << "Downloading" << (geoIp ? "geoip" : "geosite") << "for routing profile" << profileId << "from" << url.toString();
}

void RoutingController::onDownloadFinished(const QString &profileId, bool geoIp, QNetworkReply *reply)
{
    reply->deleteLater();
    QList<Download> &downloads = m_downloads[profileId];
    for (int i = 0; i < downloads.size(); ++i) {
        if (downloads[i].reply == reply) {
            downloads.removeAt(i);
            break;
        }
    }
    if (downloads.isEmpty()) {
        m_downloads.remove(profileId);
    }

    auto p = profile(profileId);
    if (!p) {
        emit geoDownloadStateChanged(profileId);
        return;
    }

    QString error;
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            emit geoDownloadStateChanged(profileId);
            return;
        }
        error = tr("Failed to download geo files: %1").arg(reply->errorString());
    } else {
        const QByteArray data = reply->readAll();
        if (GeoData::validate(data, geoIp, &error)) {
            QDir().mkpath(profileDirectory(profileId));
            QSaveFile file(profileDirectory(profileId) + (geoIp ? QStringLiteral("/geoip.dat") : QStringLiteral("/geosite.dat")));
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
                error = tr("Failed to save the geo file");
            }
        }
    }

    if (error.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (geoIp) {
            p->geoIpUpdatedAt = now;
        } else {
            p->geoSiteUpdatedAt = now;
        }
        p->geoError.clear();
    } else {
        p->geoError = error;
        logger.warning() << "Geo file download failed for routing profile" << profileId << ":" << error;
    }
    updateProfile(*p);
    emit geoDownloadStateChanged(profileId);
    if (!isDownloading(profileId)) {
        emit geoDownloadFinished(profileId, error.isEmpty(), error.isEmpty() ? tr("Geo files are downloaded") : error);
    }
}

void RoutingController::migrateLegacySplitTunneling()
{
    if (m_appSettingsRepository->isLegacySplitTunnelingMigrated()) {
        return;
    }
    m_appSettingsRepository->setLegacySplitTunnelingMigrated(true);

    struct LegacyList
    {
        RouteMode mode;
        QString name;
        bool globalProxy;
    };
    const QList<LegacyList> legacyLists = {
        { RouteMode::VpnOnlyForwardSites, tr("Only listed sites via VPN"), false },
        { RouteMode::VpnAllExceptSites, tr("All sites via VPN except listed"), true },
    };

    QList<RoutingProfile> list = profiles();
    QString selectedId;
    const RouteMode legacyMode = m_appSettingsRepository->routeMode();
    for (const LegacyList &legacy : legacyLists) {
        const QVariantMap sites = m_appSettingsRepository->vpnSites(legacy.mode);
        if (sites.isEmpty()) {
            continue;
        }
        RoutingProfile p = RoutingProfile::createDefault(legacy.name);
        p.globalProxy = legacy.globalProxy;
        const RuleAction action = legacy.globalProxy ? RuleAction::Direct : RuleAction::Proxy;
        QStringList entries;
        for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
            const QString site = hostOrAddress(it.key());
            if (site.isEmpty()) {
                continue;
            }
            entries.append(isIpRule(site) ? site : QStringLiteral("domain:") + site.toLower());
        }
        if (!legacy.globalProxy) {
            // Keep local networks reachable when only listed sites use the VPN.
            p.directIp = lanAddresses();
        }
        QStringList current = p.sites(action);
        current.append(p.ips(action));
        current.append(entries);
        p.setRulesText(action, current.join(QLatin1Char('\n')));
        list.append(p);
        if (legacy.mode == legacyMode || selectedId.isEmpty()) {
            selectedId = p.id;
        }
    }
    if (list.size() == profiles().size()) {
        return;
    }
    saveProfiles(list);
    m_appSettingsRepository->setSelectedRoutingProfileId(selectedId);
    m_appSettingsRepository->setRoutingEnabled(m_appSettingsRepository->isSitesSplitTunnelingEnabled());
    m_appSettingsRepository->setSitesSplitTunnelingEnabled(false);
    logger.info() << "Migrated legacy split tunneling site lists into routing profiles";
}

std::optional<RoutingProfile> RoutingController::activeProfile(const SecureAppSettingsRepository *repository, const QString &serverId)
{
    if (!repository) {
        return std::nullopt;
    }
    const QString override = repository->serverRoutingOverrides().value(serverId).toString();
    if (override == overrideOff) {
        return std::nullopt;
    }
    QString id = override;
    if (id.isEmpty()) {
        if (!repository->isRoutingEnabled()) {
            return std::nullopt;
        }
        id = repository->selectedRoutingProfileId();
    }
    for (const RoutingProfile &p : loadProfiles(repository)) {
        if (p.id == id) {
            return p;
        }
    }
    return std::nullopt;
}
