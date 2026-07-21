#ifndef ROUTINGPROFILESMODEL_H
#define ROUTINGPROFILESMODEL_H

#include <QAbstractListModel>
#include <QVector>

#include "core/utils/routingProfile.h"

using namespace amnezia;

class RoutingProfilesModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        IsActiveRole,
        RuleCountRole,
        GlobalProxyRole
    };

    explicit RoutingProfilesModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

public slots:
    void updateModel(const QVector<RoutingProfile> &profiles, const QString &activeName);

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    QVector<RoutingProfile> m_profiles;
    QString m_activeName;
};

#endif // ROUTINGPROFILESMODEL_H
