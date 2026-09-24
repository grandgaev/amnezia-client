#include "geoTagsModel.h"

GeoTagsModel::GeoTagsModel(QObject *parent) : QAbstractListModel(parent)
{
}

int GeoTagsModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return int(m_values.size());
}

QVariant GeoTagsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_values.size()) {
        return QVariant();
    }
    const QString &value = m_values.at(index.row());
    switch (role) {
    case ValueRole: return value;
    case HeaderRole: {
        const int colon = value.indexOf(QLatin1Char(':'));
        return value.mid(colon + 1, 1).toUpper();
    }
    case IsSelectedRole: return m_selected.contains(value.toLower());
    }
    return QVariant();
}

QHash<int, QByteArray> GeoTagsModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[ValueRole] = "value";
    roles[HeaderRole] = "header";
    roles[IsSelectedRole] = "isSelected";
    return roles;
}

void GeoTagsModel::updateModel(const QStringList &values, const QStringList &selected)
{
    beginResetModel();
    m_values = values;
    m_selected.clear();
    for (const QString &s : selected) {
        m_selected.insert(s.toLower());
    }
    endResetModel();
    emit selectionChanged();
}

void GeoTagsModel::setSelected(const QString &value, bool selected)
{
    const int row = int(m_values.indexOf(value));
    if (row < 0) {
        return;
    }
    if (selected) {
        m_selected.insert(value.toLower());
    } else {
        m_selected.remove(value.toLower());
    }
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { IsSelectedRole });
    emit selectionChanged();
}

QStringList GeoTagsModel::values() const
{
    return m_values;
}

QStringList GeoTagsModel::selectedValues() const
{
    QStringList result;
    for (const QString &value : m_values) {
        if (m_selected.contains(value.toLower())) {
            result.append(value);
        }
    }
    return result;
}

int GeoTagsModel::selectedCount() const
{
    return int(selectedValues().size());
}
