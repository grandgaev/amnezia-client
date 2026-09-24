#include "routingUiController.h"

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QtConcurrent>

#include "core/models/routing/geoData.h"
#include "core/models/routing/ipRanges.h"
#include "core/utils/containers/containerUtils.h"
#include "core/utils/protocolEnum.h"
#include "ui/controllers/systemController.h"

using namespace amnezia::routing;

namespace
{
    RuleAction toAction(int action)
    {
        switch (action) {
        case RoutingUiController::Direct: return RuleAction::Direct;
        case RoutingUiController::Block: return RuleAction::Block;
        default: return RuleAction::Proxy;
        }
    }

    QString formatSize(qint64 bytes)
    {
        return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
    }
} // namespace

RoutingUiController::RoutingUiController(RoutingController *routingController, ServersController *serversController,
                                         RoutingProfilesModel *profilesModel, GeoTagsModel *geoTagsModel, QObject *parent)
    : QObject(parent),
      m_routingController(routingController),
      m_serversController(serversController),
      m_profilesModel(profilesModel),
      m_geoTagsModel(geoTagsModel)
{
    connect(m_routingController, &RoutingController::profilesChanged, this, [this]() {
        updateModel();
        emit profilesChanged();
        emit selectedProfileChanged();
    });
    connect(m_routingController, &RoutingController::profileChanged, this, [this](const QString &id) {
        if (id == m_currentProfileId) {
            emit currentProfileChanged();
            emit currentGeoStateChanged();
        }
    });
    connect(m_routingController, &RoutingController::routingEnabledChanged, this, [this]() {
        updateModel();
        emit routingEnabledChanged();
    });
    connect(m_routingController, &RoutingController::selectedProfileChanged, this, [this]() {
        updateModel();
        emit selectedProfileChanged();
    });
    connect(m_routingController, &RoutingController::excludedRoutesChanged, this, &RoutingUiController::excludedRoutesChanged);
    connect(m_routingController, &RoutingController::geoDownloadStateChanged, this, [this](const QString &id) {
        updateModel();
        if (id == m_currentProfileId) {
            emit currentGeoStateChanged();
        }
    });
    connect(m_routingController, &RoutingController::geoDownloadFinished, this,
            [this](const QString &id, bool success, const QString &message) {
                // Geo files updated by the user are used by the connection after a reconnect.
                if (m_userGeoUpdates.remove(id) && success && affectsActiveConnection(id)) {
                    markReconnectRequired();
                }
                if (success) {
                    emit finished(message);
                } else {
                    emit errorOccurred(message);
                }
            });

    connect(&m_geoTagsWatcher, &QFutureWatcher<QStringList>::finished, this, [this]() {
        m_geoTagsLoading = false;
        emit geoTagsLoadingChanged();
        const QStringList values = m_geoTagsWatcher.result();
        if (values.isEmpty()) {
            m_geoTagsModel->updateModel({}, {});
            emit geoTagsLoaded(false, tr("The geo file is missing or damaged. Try to download it again."));
            return;
        }
        m_geoTagsModel->updateModel(values, m_geoTagsSelected);
        emit geoTagsLoaded(true, QString());
    });

    updateModel();
}

void RoutingUiController::updateModel()
{
    QStringList downloading;
    for (const RoutingProfile &p : m_routingController->profiles()) {
        if (m_routingController->isDownloading(p.id)) {
            downloading.append(p.id);
        }
    }
    m_profilesModel->updateModel(m_routingController->profiles(), m_routingController->selectedProfileId(),
                                 m_routingController->isRoutingEnabled(), downloading);
}

bool RoutingUiController::isRoutingEnabled() const
{
    return m_routingController->isRoutingEnabled();
}

void RoutingUiController::setRoutingEnabled(bool enabled)
{
    const QString before = activeStateFingerprint();
    if (!m_routingController->setRoutingEnabled(enabled)) {
        emit errorOccurred(tr("You don't have any routing profiles. Please add or import one first."));
        emit routingEnabledChanged();
        return;
    }
    markIfChanged(before);
}

QString RoutingUiController::selectedProfileId() const
{
    return m_routingController->selectedProfileId();
}

QString RoutingUiController::selectedProfileName() const
{
    return profileName(m_routingController->selectedProfileId());
}

int RoutingUiController::profilesCount() const
{
    return int(m_routingController->profiles().size());
}

void RoutingUiController::selectProfile(const QString &id)
{
    const QString before = activeStateFingerprint();
    m_routingController->selectProfile(id);
    markIfChanged(before);
}

QString RoutingUiController::createProfile(const QString &name)
{
    QString error;
    const QString before = activeStateFingerprint();
    const QString id = m_routingController->createProfile(name, &error);
    if (id.isEmpty()) {
        emit errorOccurred(error);
    }
    markIfChanged(before);
    return id;
}

void RoutingUiController::removeProfile(const QString &id)
{
    const QString name = profileName(id);
    const QString before = activeStateFingerprint();
    if (m_routingController->removeProfile(id)) {
        markIfChanged(before);
        emit finished(tr("Profile \"%1\" is removed").arg(name));
    }
}

QString RoutingUiController::duplicateProfile(const QString &id)
{
    return m_routingController->duplicateProfile(id);
}

bool RoutingUiController::renameProfile(const QString &id, const QString &name)
{
    QString error;
    if (!m_routingController->renameProfile(id, name, &error)) {
        emit errorOccurred(error);
        return false;
    }
    return true;
}

QString RoutingUiController::profileName(const QString &id) const
{
    auto p = m_routingController->profile(id);
    return p ? p->name : QString();
}

void RoutingUiController::importFromClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.trimmed().isEmpty()) {
        emit errorOccurred(tr("The clipboard is empty"));
        return;
    }
    const QString trimmed = text.trimmed();
    if ((trimmed.startsWith(QLatin1String("https://")) || trimmed.startsWith(QLatin1String("http://")))
        && !trimmed.contains(QLatin1Char('\n'))) {
        importFromUrl(trimmed);
        return;
    }
    importFromText(text);
}

void RoutingUiController::importFromText(const QString &text)
{
    // Conflicts left unanswered by a previous import are dropped.
    m_pendingConflicts.clear();
    const QString before = activeStateFingerprint();
    const auto outcomes = m_routingController->importData(text, false);
    markIfChanged(before);
    handleImportOutcomes(outcomes);
}

void RoutingUiController::importFromFile(const QString &fileName)
{
    // SystemController::readFile also handles the content URIs of Android.
    QByteArray data;
    if (!SystemController::readFile(fileName, data)) {
        emit errorOccurred(tr("Can't open file: %1").arg(fileName));
        return;
    }
    importFromText(QString::fromUtf8(data));
}

void RoutingUiController::importFromUrl(const QString &url)
{
    const QUrl parsed(url.trimmed());
    if (!parsed.isValid() || (parsed.scheme() != QLatin1String("https") && parsed.scheme() != QLatin1String("http"))) {
        emit errorOccurred(tr("Invalid link"));
        return;
    }
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(parsed);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Failed to download the routing profile: %1").arg(reply->errorString()));
            return;
        }
        importFromText(QString::fromUtf8(reply->readAll()));
    });
}

void RoutingUiController::handleImportOutcomes(const QList<RoutingController::ImportOutcome> &outcomes)
{
    for (const auto &outcome : outcomes) {
        switch (outcome.status) {
        case RoutingController::ImportStatus::Added:
            emit profileImported(outcome.profileId, tr("Profile \"%1\" successfully added").arg(outcome.name));
            break;
        case RoutingController::ImportStatus::Updated:
            emit profileImported(outcome.profileId, tr("Profile \"%1\" successfully updated").arg(outcome.name));
            break;
        case RoutingController::ImportStatus::NameConflict: m_pendingConflicts.append(outcome.data); break;
        case RoutingController::ImportStatus::InvalidData: emit errorOccurred(outcome.error); break;
        }
    }
    if (!m_pendingConflicts.isEmpty()) {
        RoutingProfile p;
        p.mergeExchangeJson(m_pendingConflicts.first());
        emit importConflict(p.name);
    }
}

void RoutingUiController::resolveImportConflict(bool replace)
{
    if (m_pendingConflicts.isEmpty()) {
        return;
    }
    QJsonObject object = m_pendingConflicts.takeFirst();
    const QString before = activeStateFingerprint();
    if (replace) {
        const auto outcome = m_routingController->importObject(object, true, false);
        markIfChanged(before);
        handleImportOutcomes({ outcome });
        return;
    }

    // Keep both: the imported profile is added under a free name.
    RoutingProfile p;
    p.mergeExchangeJson(object);
    const QString base = p.name.trimmed();
    QString name = base;
    for (int i = 2; m_routingController->isNameTaken(name); ++i) {
        name = QStringLiteral("%1 (%2)").arg(base).arg(i);
    }
    for (const QString &key : object.keys()) {
        if (key.compare(QLatin1String("name"), Qt::CaseInsensitive) == 0) {
            object.remove(key);
        }
    }
    object.insert(QStringLiteral("Name"), name);
    const auto outcome = m_routingController->importObject(object, false, false);
    markIfChanged(before);
    handleImportOutcomes({ outcome });
}

void RoutingUiController::exportToClipboard(const QString &id)
{
    const QString data = m_routingController->exportProfile(id);
    if (data.isEmpty()) {
        emit errorOccurred(tr("Export is not possible, no profile selected"));
        return;
    }
    QGuiApplication::clipboard()->setText(data);
    emit finished(tr("Routing profile is copied to the clipboard"));
}

void RoutingUiController::exportToFile(const QString &id, const QString &fileName)
{
    const QString data = m_routingController->exportProfile(id);
    if (data.isEmpty()) {
        emit errorOccurred(tr("Export is not possible, no profile selected"));
        return;
    }
    if (!SystemController::saveFile(fileName, data)) {
        return;
    }
    emit finished(tr("Routing profile is saved"));
}

QString RoutingUiController::currentProfileId() const
{
    return m_currentProfileId;
}

void RoutingUiController::setCurrentProfileId(const QString &id)
{
    if (m_currentProfileId == id) {
        return;
    }
    m_currentProfileId = id;
    emit currentProfileChanged();
    emit currentGeoStateChanged();
}

std::optional<RoutingProfile> RoutingUiController::current() const
{
    return m_routingController->profile(m_currentProfileId);
}

bool RoutingUiController::modifyCurrent(const std::function<void(RoutingProfile &)> &change)
{
    auto p = current();
    if (!p) {
        return false;
    }
    const QString before = activeStateFingerprint();
    change(*p);
    const bool updated = m_routingController->updateProfile(*p);
    markIfChanged(before);
    return updated;
}

QString RoutingUiController::currentName() const
{
    auto p = current();
    return p ? p->name : QString();
}

bool RoutingUiController::currentGlobalProxy() const
{
    auto p = current();
    return p ? p->globalProxy : true;
}

int RoutingUiController::currentRouteOrderIndex() const
{
    auto p = current();
    const int index = int(routeOrderValues().indexOf(p ? p->routeOrder : defaultRouteOrder()));
    return index < 0 ? int(routeOrderValues().indexOf(defaultRouteOrder())) : index;
}

int RoutingUiController::currentDomainStrategyIndex() const
{
    auto p = current();
    const int index = int(domainStrategyValues().indexOf(p ? p->domainStrategy : QStringLiteral("IPIfNonMatch")));
    return index < 0 ? 1 : index;
}

bool RoutingUiController::currentFakeDns() const
{
    auto p = current();
    return p && p->fakeDns;
}

QString RoutingUiController::currentRemoteDnsType() const
{
    auto p = current();
    return p ? p->remoteDnsType : QString::fromLatin1(dnsTypeDoU);
}

QString RoutingUiController::currentRemoteDnsDomain() const
{
    auto p = current();
    return p ? p->remoteDnsDomain : QString();
}

QString RoutingUiController::currentRemoteDnsIp() const
{
    auto p = current();
    return p ? p->remoteDnsIp : QString();
}

QString RoutingUiController::currentDomesticDnsType() const
{
    auto p = current();
    return p ? p->domesticDnsType : QString::fromLatin1(dnsTypeDoH);
}

QString RoutingUiController::currentDomesticDnsDomain() const
{
    auto p = current();
    return p ? p->domesticDnsDomain : QString();
}

QString RoutingUiController::currentDomesticDnsIp() const
{
    auto p = current();
    return p ? p->domesticDnsIp : QString();
}

QString RoutingUiController::currentGeoSiteUrl() const
{
    auto p = current();
    return p ? p->geoSiteUrl : QString();
}

QString RoutingUiController::currentGeoIpUrl() const
{
    auto p = current();
    return p ? p->geoIpUrl : QString();
}

QString RoutingUiController::geoStatus(bool geoIp) const
{
    auto p = current();
    if (!p) {
        return QString();
    }
    if (m_routingController->isDownloading(p->id)) {
        return tr("Downloading: %1%").arg(m_routingController->downloadProgress(p->id));
    }
    const qint64 updatedAt = geoIp ? p->geoIpUpdatedAt : p->geoSiteUpdatedAt;
    const QString ownFile = RoutingController::profileDirectory(p->id) + (geoIp ? QStringLiteral("/geoip.dat") : QStringLiteral("/geosite.dat"));
    if (updatedAt > 0 && QFileInfo::exists(ownFile)) {
        return tr("Last updated: %1, size: %2")
                .arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(updatedAt), QLocale::ShortFormat))
                .arg(formatSize(QFileInfo(ownFile).size()));
    }
    if (!RoutingController::bundledGeoFilePath(geoIp).isEmpty()) {
        return tr("Built-in file is used");
    }
    return tr("Last updated: never");
}

QString RoutingUiController::currentGeoSiteStatus() const
{
    return geoStatus(false);
}

QString RoutingUiController::currentGeoIpStatus() const
{
    return geoStatus(true);
}

QString RoutingUiController::currentGeoError() const
{
    auto p = current();
    return p ? p->geoError : QString();
}

bool RoutingUiController::currentDownloading() const
{
    return m_routingController->isDownloading(m_currentProfileId);
}

void RoutingUiController::setGlobalProxy(bool enabled)
{
    modifyCurrent([enabled](RoutingProfile &p) { p.globalProxy = enabled; });
}

void RoutingUiController::setRouteOrderIndex(int index)
{
    const QStringList values = routeOrderValues();
    if (index < 0 || index >= values.size()) {
        return;
    }
    modifyCurrent([&values, index](RoutingProfile &p) { p.routeOrder = values.at(index); });
}

void RoutingUiController::setDomainStrategyIndex(int index)
{
    const QStringList values = domainStrategyValues();
    if (index < 0 || index >= values.size()) {
        return;
    }
    modifyCurrent([&values, index](RoutingProfile &p) { p.domainStrategy = values.at(index); });
}

void RoutingUiController::setFakeDns(bool enabled)
{
    modifyCurrent([enabled](RoutingProfile &p) { p.fakeDns = enabled; });
}

void RoutingUiController::setRemoteDns(const QString &type, const QString &domain, const QString &ip)
{
    modifyCurrent([&](RoutingProfile &p) {
        p.remoteDnsType = type.compare(QLatin1String(dnsTypeDoH), Qt::CaseInsensitive) == 0 ? QString::fromLatin1(dnsTypeDoH)
                                                                                           : QString::fromLatin1(dnsTypeDoU);
        p.remoteDnsDomain = domain.trimmed();
        p.remoteDnsIp = ip.trimmed();
    });
}

void RoutingUiController::setDomesticDns(const QString &type, const QString &domain, const QString &ip)
{
    modifyCurrent([&](RoutingProfile &p) {
        p.domesticDnsType = type.compare(QLatin1String(dnsTypeDoH), Qt::CaseInsensitive) == 0 ? QString::fromLatin1(dnsTypeDoH)
                                                                                             : QString::fromLatin1(dnsTypeDoU);
        p.domesticDnsDomain = domain.trimmed();
        p.domesticDnsIp = ip.trimmed();
    });
}

void RoutingUiController::setGeoSiteUrl(const QString &url)
{
    QString value = url.trimmed();
    if (value.isEmpty()) {
        value = QString::fromLatin1(defaultGeoSiteUrl);
    }
    if (modifyCurrent([&value](RoutingProfile &p) { p.geoSiteUrl = value; })) {
        m_userGeoUpdates.insert(m_currentProfileId);
        m_routingController->updateGeoFiles(m_currentProfileId, true, false);
    }
}

void RoutingUiController::setGeoIpUrl(const QString &url)
{
    QString value = url.trimmed();
    if (value.isEmpty()) {
        value = QString::fromLatin1(defaultGeoIpUrl);
    }
    if (modifyCurrent([&value](RoutingProfile &p) { p.geoIpUrl = value; })) {
        m_userGeoUpdates.insert(m_currentProfileId);
        m_routingController->updateGeoFiles(m_currentProfileId, false, true);
    }
}

void RoutingUiController::updateGeoFiles()
{
    if (!current()) {
        return;
    }
    m_userGeoUpdates.insert(m_currentProfileId);
    m_routingController->updateGeoFiles(m_currentProfileId, true, true);
}

QString RoutingUiController::rulesText(int action) const
{
    auto p = current();
    return p ? p->rulesText(toAction(action)) : QString();
}

QString RoutingUiController::setRulesText(int action, const QString &text)
{
    QStringList suspicious;
    for (const QString &token : splitRulesText(text)) {
        if (isIpRule(token)) {
            continue;
        }
        const int colon = token.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            const QString prefix = token.left(colon).toLower();
            static const QStringList known = { QStringLiteral("geosite"), QStringLiteral("domain"), QStringLiteral("full"),
                                               QStringLiteral("regexp"), QStringLiteral("keyword") };
            if (!known.contains(prefix)) {
                suspicious.append(token);
            }
        } else if (token.contains(QLatin1Char('/')) || token.contains(QLatin1Char(' '))
                   || (!token.contains(QLatin1Char('.')) && token.size() < 3)) {
            // Invalid CIDRs and addresses end up here as well.
            suspicious.append(token);
        }
    }
    const bool hadGeo = current() && (current()->usesGeoSite() || current()->usesGeoIp());
    modifyCurrent([&](RoutingProfile &p) { p.setRulesText(toAction(action), text); });
    if (!hadGeo) {
        m_routingController->ensureGeoFiles(m_currentProfileId);
    }
    if (suspicious.isEmpty()) {
        return QString();
    }
    return tr("These entries are not recognized and will be ignored: %1").arg(suspicious.join(QStringLiteral(", ")));
}

QString RoutingUiController::lanAddressesText() const
{
    return lanAddresses().join(QLatin1Char('\n'));
}

int RoutingUiController::rulesCount(int action) const
{
    auto p = current();
    if (!p) {
        return 0;
    }
    return int(p->sites(toAction(action)).size() + p->ips(toAction(action)).size());
}

void RoutingUiController::loadGeoTags(bool geoIp, const QString &currentText)
{
    if (m_geoTagsLoading) {
        return;
    }
    const QString path = RoutingController::geoFilePath(m_currentProfileId, geoIp);
    if (path.isEmpty()) {
        m_routingController->updateGeoFiles(m_currentProfileId, !geoIp, geoIp);
        emit geoTagsLoaded(false, tr("The geo file is not downloaded yet. The download has started, try again in a moment."));
        return;
    }
    const QString prefix = geoIp ? QStringLiteral("geoip:") : QStringLiteral("geosite:");
    m_geoTagsSelected.clear();
    for (const QString &token : splitRulesText(currentText)) {
        if (token.startsWith(prefix, Qt::CaseInsensitive)) {
            m_geoTagsSelected.append(token.toLower());
        }
    }
    m_geoTagsLoading = true;
    emit geoTagsLoadingChanged();
    m_geoTagsWatcher.setFuture(QtConcurrent::run([path, prefix]() {
        QStringList values;
        for (const QString &tag : GeoData::listTags(path)) {
            values.append(prefix + tag);
        }
        return values;
    }));
}

bool RoutingUiController::geoTagsLoading() const
{
    return m_geoTagsLoading;
}

QString RoutingUiController::applyGeoTags(const QString &currentText) const
{
    QSet<QString> listed;
    for (const QString &value : m_geoTagsModel->values()) {
        listed.insert(value.toLower());
    }
    const QStringList selectedValues = m_geoTagsModel->selectedValues();
    QSet<QString> selected;
    for (const QString &value : selectedValues) {
        selected.insert(value.toLower());
    }

    static const QRegularExpression lineBreak(QStringLiteral("\\r?\\n"));
    QStringList lines;
    QSet<QString> present;
    for (const QString &line : currentText.split(lineBreak)) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            lines.append(line);
            continue;
        }
        QStringList kept;
        bool removed = false;
        for (const QString &part : trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString token = part.trimmed();
            if (token.isEmpty()) {
                continue;
            }
            const QString lower = token.toLower();
            if (listed.contains(lower) && !selected.contains(lower)) {
                removed = true;
                continue;
            }
            kept.append(token);
            present.insert(lower);
        }
        if (!removed) {
            lines.append(line);
        } else if (!kept.isEmpty()) {
            lines.append(kept.join(QStringLiteral(", ")));
        }
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    for (const QString &value : selectedValues) {
        if (!present.contains(value.toLower())) {
            lines.append(value);
            present.insert(value.toLower());
        }
    }
    return lines.join(QLatin1Char('\n'));
}

int RoutingUiController::currentRuleAction() const
{
    return m_currentRuleAction;
}

void RoutingUiController::setCurrentRuleAction(int action)
{
    if (action < Proxy || action > Block || action == m_currentRuleAction) {
        return;
    }
    m_currentRuleAction = action;
    emit currentRuleActionChanged();
}

bool RoutingUiController::reconnectRequired() const
{
    return m_reconnectRequired;
}

void RoutingUiController::setConnectionActive(bool active)
{
    if (m_connectionActive == active) {
        return;
    }
    m_connectionActive = active;
    // A new connection uses the current settings; without a connection there is nothing to apply.
    if (m_reconnectRequired) {
        m_reconnectRequired = false;
        emit reconnectRequiredChanged();
    }
}

void RoutingUiController::markReconnectRequired()
{
    if (!m_connectionActive) {
        return;
    }
    emit settingsChangedWhileConnected();
    if (!m_reconnectRequired) {
        m_reconnectRequired = true;
        emit reconnectRequiredChanged();
    }
}

QString RoutingUiController::activeStateFingerprint() const
{
    QJsonObject state;
    const QString serverId = m_serversController->getDefaultServerId();
    QString id = m_routingController->serverOverride(serverId);
    if (id.isEmpty() && m_routingController->isRoutingEnabled()) {
        id = m_routingController->selectedProfileId();
    }
    if (!id.isEmpty() && id != RoutingController::overrideOff) {
        if (auto p = m_routingController->profile(id)) {
            QJsonObject profile = p->toExchangeJson();
            profile.remove(QStringLiteral("Name"));
            profile.remove(QStringLiteral("LastUpdated"));
            state.insert(QStringLiteral("profile"), profile);
        }
    }
    state.insert(QStringLiteral("excludedRoutes"), QJsonArray::fromStringList(m_routingController->excludedRoutes()));
    return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

bool RoutingUiController::affectsActiveConnection(const QString &profileId) const
{
    const QString override = m_routingController->serverOverride(m_serversController->getDefaultServerId());
    if (override == RoutingController::overrideOff) {
        return false;
    }
    if (!override.isEmpty()) {
        return override == profileId;
    }
    return m_routingController->isRoutingEnabled() && m_routingController->selectedProfileId() == profileId;
}

void RoutingUiController::markIfChanged(const QString &fingerprintBefore)
{
    if (m_connectionActive && activeStateFingerprint() != fingerprintBefore) {
        markReconnectRequired();
    }
}

QString RoutingUiController::excludedRoutesText() const
{
    return m_routingController->excludedRoutes().join(QLatin1Char('\n'));
}

QString RoutingUiController::setExcludedRoutesText(const QString &text)
{
    QStringList valid;
    QStringList invalid;
    for (const QString &token : splitRulesText(text)) {
        IpRangeSet probe;
        if (probe.addCidr(token)) {
            if (!valid.contains(token)) {
                valid.append(token);
            }
        } else {
            invalid.append(token);
        }
    }
    const QString before = activeStateFingerprint();
    m_routingController->setExcludedRoutes(valid);
    markIfChanged(before);
    if (invalid.isEmpty()) {
        return QString();
    }
    return tr("Invalid addresses are ignored: %1").arg(invalid.join(QStringLiteral(", ")));
}

QStringList RoutingUiController::geoUserAgents() const
{
    return { tr("Chrome (Android)"), tr("Chrome (Windows)"), tr("Safari (macOS)"), tr("Safari (iOS)"), tr("Firefox (Windows)") };
}

int RoutingUiController::geoUserAgentIndex() const
{
    return int(RoutingController::geoUserAgentNames().indexOf(m_routingController->geoUserAgent()));
}

void RoutingUiController::setGeoUserAgentIndex(int index)
{
    const QStringList names = RoutingController::geoUserAgentNames();
    if (index < 0 || index >= names.size()) {
        return;
    }
    m_routingController->setGeoUserAgent(names.at(index));
    emit geoUserAgentChanged();
}

QStringList RoutingUiController::routeOrderNames() const
{
    return { tr("Block → VPN → Direct"), tr("Block → Direct → VPN"), tr("VPN → Direct → Block"),
             tr("VPN → Block → Direct"), tr("Direct → VPN → Block"), tr("Direct → Block → VPN") };
}

QStringList RoutingUiController::domainStrategies() const
{
    return domainStrategyValues();
}

QString RoutingUiController::protocolSupportText() const
{
    const QString serverId = m_serversController->getDefaultServerId();
    if (serverId.isEmpty()) {
        return QString();
    }
    const DockerContainer container = m_serversController->getDefaultContainer(serverId);
    switch (ContainerUtils::defaultProtocol(container)) {
    case Proto::Awg:
    case Proto::WireGuard:
    case Proto::Xray:
    case Proto::SSXray:
    case Proto::Unknown: return QString();
    default: break;
    }
    return tr("The current protocol (%1) routes by addresses only: IP rules, geoip categories and plain domain names are applied, "
              "while geosite categories, keyword/regexp rules and blocking require AmneziaWG, WireGuard or XRay.")
            .arg(ContainerUtils::containerHumanNames().value(container));
}

bool RoutingUiController::isAppRoutingSupported() const
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_WINDOWS)
    return true;
#else
    return false;
#endif
}

QString RoutingUiController::serverRoutingValue(const QString &serverId) const
{
    return m_routingController->serverOverride(serverId);
}

void RoutingUiController::setServerRoutingValue(const QString &serverId, const QString &value)
{
    const QString before = activeStateFingerprint();
    m_routingController->setServerOverride(serverId, value);
    markIfChanged(before);
    emit profilesChanged();
}

QString RoutingUiController::serverRoutingDescription(const QString &serverId) const
{
    const QString value = m_routingController->serverOverride(serverId);
    if (value == RoutingController::overrideOff) {
        return tr("Routing is disabled for this server");
    }
    if (value.isEmpty()) {
        if (!m_routingController->isRoutingEnabled()) {
            return tr("Default (routing is disabled)");
        }
        return tr("Default (%1)").arg(selectedProfileName());
    }
    const QString name = profileName(value);
    return name.isEmpty() ? tr("Default") : name;
}

QStringList RoutingUiController::profileIds() const
{
    QStringList ids;
    for (const RoutingProfile &p : m_routingController->profiles()) {
        ids.append(p.id);
    }
    return ids;
}
