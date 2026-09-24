#ifndef ROUTINGPROFILESMODEL_H
#define ROUTINGPROFILESMODEL_H

#include <QAbstractListModel>

#include "core/models/routing/routingProfile.h"

class RoutingProfilesModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        IsSelectedRole,
        DescriptionRole,
        HasGeoErrorRole,
        IsDownloadingRole
    };

    explicit RoutingProfilesModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void updateModel(const QList<amnezia::routing::RoutingProfile> &profiles, const QString &selectedId, bool routingEnabled,
                     const QStringList &downloadingIds);

    static QString describe(const amnezia::routing::RoutingProfile &profile);

private:
    QList<amnezia::routing::RoutingProfile> m_profiles;
    QString m_selectedId;
    bool m_routingEnabled = false;
    QStringList m_downloadingIds;
};

#endif // ROUTINGPROFILESMODEL_H
