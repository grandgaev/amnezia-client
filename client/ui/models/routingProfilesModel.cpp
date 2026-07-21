#include "routingProfilesModel.h"

RoutingProfilesModel::RoutingProfilesModel(QObject *parent) : QAbstractListModel(parent)
{
}

int RoutingProfilesModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_profiles.size();
}

QVariant RoutingProfilesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_profiles.size()) {
        return QVariant();
    }

    const RoutingProfile &profile = m_profiles.at(index.row());
    switch (role) {
    case NameRole: return profile.name;
    case IsActiveRole: return profile.name == m_activeName;
    case GlobalProxyRole: return profile.globalProxy;
    case RuleCountRole:
        return profile.proxySites.size() + profile.proxyIp.size() + profile.directSites.size()
                + profile.directIp.size() + profile.blockSites.size() + profile.blockIp.size();
    default: return QVariant();
    }
}

void RoutingProfilesModel::updateModel(const QVector<RoutingProfile> &profiles, const QString &activeName)
{
    beginResetModel();
    m_profiles = profiles;
    m_activeName = activeName;
    endResetModel();
}

QHash<int, QByteArray> RoutingProfilesModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[NameRole] = "name";
    roles[IsActiveRole] = "isActive";
    roles[RuleCountRole] = "ruleCount";
    roles[GlobalProxyRole] = "globalProxy";
    return roles;
}
