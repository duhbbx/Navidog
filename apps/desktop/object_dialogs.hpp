#pragma once

#include "components.hpp"
#include "navidog/object_management.hpp"

class QLabel;
class QStandardItemModel;
class SqlReadOnlyView;
namespace navidog::ui {
class ObjectDetailsDialog final : public Dialog {
public:
    ObjectDetailsDialog(const ConnectionProfile&, const CatalogNode&, QWidget* parent);
    void showDetails(const CatalogObjectDetails&);
private:
    QStandardItemModel* properties_;
    SqlReadOnlyView* sql_;
    QLabel* status_;
};
bool confirmObjectDrop(QWidget* parent, const ConnectionProfile&, const CatalogNode&, CatalogObjectChange,
                       const QString& sql);
std::optional<QString> chooseObjectName(QWidget* parent, const ConnectionProfile&, const CatalogNode&);
bool confirmObjectBatchDrop(QWidget* parent, DatabaseKind, int count, bool cascade, const QStringList& sql);
}
