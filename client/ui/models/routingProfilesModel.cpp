#include "routingProfilesModel.h"

using namespace amnezia::routing;

RoutingProfilesModel::RoutingProfilesModel(QObject *parent) : QAbstractListModel(parent)
{
}

int RoutingProfilesModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return int(m_profiles.size());
}

QVariant RoutingProfilesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_profiles.size()) {
        return QVariant();
    }
    const RoutingProfile &profile = m_profiles.at(index.row());
    switch (role) {
    case IdRole: return profile.id;
    case NameRole: return profile.name;
    case IsSelectedRole: return profile.id == m_selectedId;
    case DescriptionRole: return describe(profile);
    case HasGeoErrorRole: return !profile.geoError.isEmpty();
    case IsDownloadingRole: return m_downloadingIds.contains(profile.id);
    }
    return QVariant();
}

QHash<int, QByteArray> RoutingProfilesModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole] = "profileId";
    roles[NameRole] = "name";
    roles[IsSelectedRole] = "isSelected";
    roles[DescriptionRole] = "description";
    roles[HasGeoErrorRole] = "hasGeoError";
    roles[IsDownloadingRole] = "isDownloading";
    return roles;
}

void RoutingProfilesModel::updateModel(const QList<RoutingProfile> &profiles, const QString &selectedId, bool routingEnabled,
                                       const QStringList &downloadingIds)
{
    beginResetModel();
    m_profiles = profiles;
    m_selectedId = selectedId;
    m_routingEnabled = routingEnabled;
    m_downloadingIds = downloadingIds;
    endResetModel();
}

QString RoutingProfilesModel::describe(const RoutingProfile &profile)
{
    const int proxy = int(profile.proxySites.size() + profile.proxyIp.size());
    const int direct = int(profile.directSites.size() + profile.directIp.size());
    const int block = int(profile.blockSites.size() + profile.blockIp.size());
    QString mode = profile.globalProxy ? tr("Everything via VPN") : tr("Everything direct");
    QStringList parts;
    if (proxy > 0) {
        parts.append(tr("VPN: %1").arg(proxy));
    }
    if (direct > 0) {
        parts.append(tr("direct: %1").arg(direct));
    }
    if (block > 0) {
        parts.append(tr("blocked: %1").arg(block));
    }
    if (!parts.isEmpty()) {
        mode += QStringLiteral(" · ") + parts.join(QStringLiteral(", "));
    }
    if (!profile.geoError.isEmpty()) {
        mode += QStringLiteral("\n") + profile.geoError;
    }
    return mode;
}
