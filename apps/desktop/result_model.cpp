#include "result_model.hpp"
#include <QColor>

QVariant ResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount()) return {};
    const auto& value = result_.rows[index.row()][index.column()];
    if(role==Qt::UserRole)return value;
    const bool null = !value.isValid() || value.isNull();
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        if (null) return QString("NULL");
        if (value.userType() == QMetaType::QByteArray) return QString("[BLOB: %1 bytes]").arg(value.toByteArray().size());
        auto text = value.toString();
        const int max = role == Qt::ToolTipRole ? 1024 : 150;
        return text.size() > max ? text.left(max) + "…" : text;
    }
    if (role == Qt::ForegroundRole && null) return QColor("#8291a1");
    if (role == Qt::TextAlignmentRole) {
        const auto type = value.userType();
        return int((type == QMetaType::LongLong || type == QMetaType::Int || type == QMetaType::Double ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    }
    return {};
}
QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if(orientation==Qt::Horizontal && role==Qt::UserRole)return result_.columns.value(section);
    if(orientation==Qt::Horizontal && role==Qt::ToolTipRole)return result_.columns.value(section)+(result_.columnTypes.value(section).isEmpty()?QString{}:"\n"+result_.columnTypes.value(section));
    if (role != Qt::DisplayRole) return {};
    return orientation == Qt::Horizontal ? QVariant(result_.columns.value(section)+(result_.columnTypes.value(section).isEmpty()?QString{}:"\n"+result_.columnTypes.value(section))) : QVariant(section + 1);
}
void ResultModel::setResult(navidog::QueryResult result) { beginResetModel(); result_ = std::move(result); endResetModel(); }
