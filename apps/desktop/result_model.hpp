#pragma once
#include "navidog/types.hpp"
#include <QAbstractTableModel>

class ResultModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : result_.rows.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : result_.columns.size(); }
    QVariant data(const QModelIndex&, int role) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
    void setResult(navidog::QueryResult result);
    const navidog::QueryResult& result() const { return result_; }
private:
    navidog::QueryResult result_;
};
