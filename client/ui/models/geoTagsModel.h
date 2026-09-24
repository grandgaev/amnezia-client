#ifndef GEOTAGSMODEL_H
#define GEOTAGSMODEL_H

#include <QAbstractListModel>
#include <QSet>

// Categories of a geosite/geoip file for the tag picker.
class GeoTagsModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

public:
    enum Roles {
        ValueRole = Qt::UserRole + 1,
        HeaderRole,
        IsSelectedRole
    };

    explicit GeoTagsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void updateModel(const QStringList &values, const QStringList &selected);
    Q_INVOKABLE void setSelected(const QString &value, bool selected);

    QStringList values() const;
    // Selected values of the list, in the list order.
    QStringList selectedValues() const;
    int selectedCount() const;

signals:
    void selectionChanged();

private:
    QStringList m_values;
    QSet<QString> m_selected;
};

#endif // GEOTAGSMODEL_H
