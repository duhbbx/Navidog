#pragma once
#include "navidog/estimated_plan.hpp"
#include "export_paths.hpp"
#include <QWidget>
class QTreeView;
namespace navidog::ui { class ObjectGrid; }
class QStandardItemModel;
class QPlainTextEdit;
class SqlReadOnlyView;
class QLabel;
class QToolButton;
class QLineEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QSplitter;
class FlatPlanModel;
class EstimatedPlanView final : public QWidget {
public:
  explicit EstimatedPlanView(QWidget *parent = nullptr);
  void setPlan(const navidog::EstimatedPlan &);
  void setExportContext(navidog::ui::ExportContext context) { exportContext_ = std::move(context); }
private:
  navidog::EstimatedPlan plan_;
  navidog::ui::ExportContext exportContext_;
  QTreeView *tree_;
  navidog::ui::ObjectGrid *table_;
  QStandardItemModel *model_;
  FlatPlanModel *flat_;
  QSortFilterProxyModel *proxy_,*flatProxy_;
  QStackedWidget *pages_;
  QSplitter *split_;
  QLineEdit *search_;
  QPlainTextEdit *details_,*rawDocument_;
  SqlReadOnlyView *request_;
  QLabel *status_,*source_;
  QToolButton *copy_,*save_,*raw_,*expand_,*collapse_,*detailsToggle_;
  int selected_=-1;
  void showNode(int);
};
