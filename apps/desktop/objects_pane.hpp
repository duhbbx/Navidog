#pragma once
#include "workspace_pane.hpp"
#include <QPoint>
namespace navidog::ui {
class ObjectGrid;
}
class QStandardItemModel;
class QSortFilterProxyModel;
class QLabel;
class QToolButton;
class ObjectsPane final : public WorkspacePane {
  Q_OBJECT
  QStandardItemModel *model_;
  QSortFilterProxyModel *proxy_;
  navidog::ui::ObjectGrid *list_;
  QLabel *context_, *count_, *details_;
  QToolButton *create_, *open_, *design_, *remove_, *refresh_;
  bool definitionOnly_ = false;
  bool busy_ = false, writable_ = true, loaded_ = false, truncated_ = false;
  int total_ = 0;
  void updateCount();
  void updateSelection();

public:
  navidog::CatalogNode folder;
  explicit ObjectsPane(QWidget *parent = nullptr);
  bool definitionOnly() const { return definitionOnly_; }
  void setContext(const navidog::ConnectionProfile &,
                  const navidog::CatalogNode &);
  void setNodes(const QVector<navidog::CatalogNode> &, bool loaded,
                bool truncated = false);
  void setBusy(bool) override;
  navidog::CatalogNode selectedNode() const;
  QJsonObject sessionState() const override;
signals:
  void actionRequested(const QString &, const navidog::CatalogNode &);
  void contextRequested(const navidog::CatalogNode &, const QPoint &);
};
