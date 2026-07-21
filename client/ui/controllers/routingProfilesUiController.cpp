#include "routingProfilesUiController.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QRegularExpression>

namespace
{
    constexpr int kDnsModeCount = 2;
    constexpr int kOrderCount = 6;
    constexpr int kDomainStrategyCount = 3;

    int clampEnum(int value, int count)
    {
        return (value < 0 || value >= count) ? 0 : value;
    }
}

RoutingProfilesUiController::RoutingProfilesUiController(SecureAppSettingsRepository *appSettingsRepository,
                                                         RoutingProfilesModel *model, QObject *parent)
    : QObject(parent), m_appSettingsRepository(appSettingsRepository), m_model(model)
{
    updateModel();
    // Top up the active profile's resolved-domain cache without wiping it, so a
    // profile that has domain rules but no cached IPs (e.g. migrated or imported on
    // an older build) becomes usable on non-xray protocols after the first launch.
    const QString activeName = m_appSettingsRepository->activeRoutingProfile().name;
    if (!activeName.isEmpty()) {
        resolveProfileDomains(activeName, false);
    }
}

bool RoutingProfilesUiController::isRoutingEnabled() const
{
    return m_appSettingsRepository->isRoutingEnabled();
}

void RoutingProfilesUiController::setRoutingEnabled(bool enabled)
{
    if (m_appSettingsRepository->isRoutingEnabled() == enabled) {
        return;
    }
    m_appSettingsRepository->setRoutingEnabled(enabled);
    emit isRoutingEnabledChanged();
}

void RoutingProfilesUiController::updateModel()
{
    m_model->updateModel(m_appSettingsRepository->routingProfiles(),
                         m_appSettingsRepository->activeRoutingProfileName());
    emit profilesChanged();
}

void RoutingProfilesUiController::beginNewProfile()
{
    m_draft = RoutingProfile();
    m_editingOriginalName.clear();
    emit draftChanged();
}

void RoutingProfilesUiController::loadProfile(int index)
{
    const QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();
    if (index < 0 || index >= profiles.size()) {
        beginNewProfile();
        return;
    }
    m_draft = profiles.at(index);
    m_editingOriginalName = m_draft.name;
    emit draftChanged();
}

void RoutingProfilesUiController::saveDraft()
{
    const QString name = m_draft.name.trimmed();
    if (name.isEmpty()) {
        emit errorOccurred(tr("Profile name cannot be empty"));
        return;
    }

    QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();

    int existingIndex = -1;
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles.at(i).name == m_editingOriginalName && !m_editingOriginalName.isEmpty()) {
            existingIndex = i;
            break;
        }
    }

    if (hasProfileNamed(name, existingIndex)) {
        emit errorOccurred(tr("A profile with this name already exists"));
        return;
    }

    m_draft.name = name;
    if (existingIndex >= 0) {
        profiles[existingIndex] = m_draft;
    } else {
        profiles.append(m_draft);
    }
    m_appSettingsRepository->setRoutingProfiles(profiles);

    // Keep the active profile pointer valid after a rename.
    if (!m_editingOriginalName.isEmpty()
        && m_appSettingsRepository->activeRoutingProfileName() == m_editingOriginalName) {
        m_appSettingsRepository->setActiveRoutingProfileName(name);
    }
    if (m_appSettingsRepository->activeRoutingProfileName().isEmpty()) {
        m_appSettingsRepository->setActiveRoutingProfileName(name);
    }

    m_editingOriginalName = name;
    resolveProfileDomains(name);
    updateModel();
    emit finished(tr("Routing profile saved"));
}

void RoutingProfilesUiController::removeProfile(int index)
{
    QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }
    const QString removedName = profiles.at(index).name;
    profiles.removeAt(index);
    m_appSettingsRepository->setRoutingProfiles(profiles);

    if (m_appSettingsRepository->activeRoutingProfileName() == removedName) {
        m_appSettingsRepository->setActiveRoutingProfileName(profiles.isEmpty() ? QString()
                                                                                : profiles.first().name);
    }
    updateModel();
}

void RoutingProfilesUiController::setActiveProfile(int index)
{
    const QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }
    m_appSettingsRepository->setActiveRoutingProfileName(profiles.at(index).name);
    resolveProfileDomains(profiles.at(index).name);
    updateModel();
}

void RoutingProfilesUiController::importDeeplink(const QString &deeplink)
{
    bool ok = false;
    RoutingProfile imported = RoutingProfile::fromDeeplink(deeplink, ok);
    if (!ok) {
        emit errorOccurred(tr("Invalid routing profile link"));
        return;
    }
    if (imported.name.trimmed().isEmpty()) {
        imported.name = tr("Imported");
    }

    QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();
    int existingIndex = -1;
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles.at(i).name == imported.name) {
            existingIndex = i;
            break;
        }
    }
    if (existingIndex >= 0) {
        profiles[existingIndex] = imported;
    } else {
        profiles.append(imported);
    }
    m_appSettingsRepository->setRoutingProfiles(profiles);
    if (m_appSettingsRepository->activeRoutingProfileName().isEmpty()) {
        m_appSettingsRepository->setActiveRoutingProfileName(imported.name);
    }
    resolveProfileDomains(imported.name);
    updateModel();
    emit finished(tr("Routing profile imported"));
}

QString RoutingProfilesUiController::exportActiveDeeplink() const
{
    return m_appSettingsRepository->activeRoutingProfile().toDeeplink();
}

int RoutingProfilesUiController::countInvalidRules(const QString &text, bool domainSide) const
{
    int invalid = 0;
    const QStringList lines = splitLines(text);
    for (const QString &line : lines) {
        const RoutingRuleKind kind = classifyRoutingRule(line);
        if (kind == RoutingRuleKind::Invalid) {
            ++invalid;
            continue;
        }
        if (domainSide && !isDomainRule(kind)) {
            ++invalid;
        } else if (!domainSide && !isIpRule(kind)) {
            ++invalid;
        }
    }
    return invalid;
}

QStringList RoutingProfilesUiController::splitLines(const QString &text) const
{
    QStringList out;
    const QStringList raw = text.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    for (const QString &line : raw) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            out.append(trimmed);
        }
    }
    return out;
}

bool RoutingProfilesUiController::hasProfileNamed(const QString &name, int exceptIndex) const
{
    const QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();
    for (int i = 0; i < profiles.size(); ++i) {
        if (i == exceptIndex) {
            continue;
        }
        if (profiles.at(i).name == name) {
            return true;
        }
    }
    return false;
}

QString RoutingProfilesUiController::resolvableHost(const QString &rule)
{
    QString host = rule.trimmed();
    if (host.isEmpty()) {
        return {};
    }

    // domain:/full: name a single host explicitly.
    static const QStringList hostPrefixes = { QStringLiteral("domain:"), QStringLiteral("full:") };
    for (const QString &prefix : hostPrefixes) {
        if (host.startsWith(prefix, Qt::CaseInsensitive)) {
            host = host.mid(prefix.length()).trimmed();
            return host.contains(QLatin1Char('.')) ? host : QString();
        }
    }

    // Only a bare domain is resolvable; keyword:/regexp:/dotless: (also classified as
    // Domain) and geosite:/ext:/ip rules are not a single host.
    if (classifyRoutingRule(host) != RoutingRuleKind::Domain) {
        return {};
    }
    static const QStringList nonHostPrefixes = { QStringLiteral("keyword:"), QStringLiteral("regexp:"),
                                                 QStringLiteral("dotless:") };
    for (const QString &prefix : nonHostPrefixes) {
        if (host.startsWith(prefix, Qt::CaseInsensitive)) {
            return {};
        }
    }
    return host.contains(QLatin1Char('.')) ? host : QString();
}

void RoutingProfilesUiController::resolveProfileDomains(const QString &profileName, bool refresh)
{
    QVector<RoutingProfile> profiles = m_appSettingsRepository->routingProfiles();
    int index = -1;
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles.at(i).name == profileName) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return;
    }

    if (refresh) {
        // Rebuild from scratch so IPs of removed domains drop out.
        profiles[index].resolvedProxyIp.clear();
        profiles[index].resolvedDirectIp.clear();
        m_appSettingsRepository->setRoutingProfiles(profiles);
    }

    auto kickOff = [this, profileName](const QStringList &sites, bool proxyBucket) {
        for (const QString &rule : sites) {
            const QString host = resolvableHost(rule);
            if (host.isEmpty()) {
                continue;
            }
            QHostInfo::lookupHost(host, this, [this, profileName, proxyBucket](const QHostInfo &info) {
                QString ip;
                for (const QHostAddress &addr : info.addresses()) {
                    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                        ip = addr.toString();
                        break;
                    }
                }
                if (ip.isEmpty()) {
                    return;
                }
                QVector<RoutingProfile> current = m_appSettingsRepository->routingProfiles();
                int idx = -1;
                for (int i = 0; i < current.size(); ++i) {
                    if (current.at(i).name == profileName) {
                        idx = i;
                        break;
                    }
                }
                if (idx < 0) {
                    return;
                }
                QStringList &bucket = proxyBucket ? current[idx].resolvedProxyIp : current[idx].resolvedDirectIp;
                if (!bucket.contains(ip)) {
                    bucket.append(ip);
                    m_appSettingsRepository->setRoutingProfiles(current);
                }
            });
        }
    };

    kickOff(profiles.at(index).proxySites, true);
    kickOff(profiles.at(index).directSites, false);
}

void RoutingProfilesUiController::setEditName(const QString &v)
{
    if (m_draft.name == v) return;
    m_draft.name = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditGlobalProxy(bool v)
{
    if (m_draft.globalProxy == v) return;
    m_draft.globalProxy = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditOrder(int v)
{
    const RuleOrder order = static_cast<RuleOrder>(clampEnum(v, kOrderCount));
    if (m_draft.order == order) return;
    m_draft.order = order;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditDomainStrategy(int v)
{
    const DomainStrategy strategy = static_cast<DomainStrategy>(clampEnum(v, kDomainStrategyCount));
    if (m_draft.domainStrategy == strategy) return;
    m_draft.domainStrategy = strategy;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditFakeDns(bool v)
{
    if (m_draft.fakeDns == v) return;
    m_draft.fakeDns = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditProxySites(const QString &v)
{
    m_draft.proxySites = splitLines(v);
    emit draftChanged();
}

void RoutingProfilesUiController::setEditProxyIp(const QString &v)
{
    m_draft.proxyIp = splitLines(v);
    emit draftChanged();
}

void RoutingProfilesUiController::setEditDirectSites(const QString &v)
{
    m_draft.directSites = splitLines(v);
    emit draftChanged();
}

void RoutingProfilesUiController::setEditDirectIp(const QString &v)
{
    m_draft.directIp = splitLines(v);
    emit draftChanged();
}

void RoutingProfilesUiController::setEditBlockSites(const QString &v)
{
    m_draft.blockSites = splitLines(v);
    emit draftChanged();
}

void RoutingProfilesUiController::setEditBlockIp(const QString &v)
{
    m_draft.blockIp = splitLines(v);
    emit draftChanged();
}

void RoutingProfilesUiController::setEditRemoteDnsMode(int v)
{
    m_draft.remoteDns.mode = static_cast<DnsMode>(clampEnum(v, kDnsModeCount));
    emit draftChanged();
}

void RoutingProfilesUiController::setEditRemoteDnsDomain(const QString &v)
{
    if (m_draft.remoteDns.domain == v) return;
    m_draft.remoteDns.domain = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditRemoteDnsIp(const QString &v)
{
    if (m_draft.remoteDns.ip == v) return;
    m_draft.remoteDns.ip = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditDomesticDnsMode(int v)
{
    m_draft.domesticDns.mode = static_cast<DnsMode>(clampEnum(v, kDnsModeCount));
    emit draftChanged();
}

void RoutingProfilesUiController::setEditDomesticDnsDomain(const QString &v)
{
    if (m_draft.domesticDns.domain == v) return;
    m_draft.domesticDns.domain = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditDomesticDnsIp(const QString &v)
{
    if (m_draft.domesticDns.ip == v) return;
    m_draft.domesticDns.ip = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditGeoipUrl(const QString &v)
{
    if (m_draft.geoipUrl == v) return;
    m_draft.geoipUrl = v;
    emit draftChanged();
}

void RoutingProfilesUiController::setEditGeositeUrl(const QString &v)
{
    if (m_draft.geositeUrl == v) return;
    m_draft.geositeUrl = v;
    emit draftChanged();
}
