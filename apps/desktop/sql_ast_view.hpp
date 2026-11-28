#pragma once
#include "navidog/sql_analysis.hpp"
#include <QWidget>
#include <functional>
class QTreeView;
class QStandardItemModel;
class QLabel;
class SqlAstView final : public QWidget {
public:
  explicit SqlAstView(QWidget *parent = nullptr);
  ~SqlAstView() override;
  void prepareForClose();
  void setReport(const navidog::SqlExecutionAnalysis &);
  void reset();
  std::function<void(int, int, const QByteArray &)> sourceActivated;

private:
  QTreeView *tree_;
  QStandardItemModel *model_;
  QLabel *status_;
  QByteArray json_, sourceHash_;
};
