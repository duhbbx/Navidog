#include "main_window.hpp"
#include "navidog/database_tools.hpp"
#include "metadata_controller.hpp"
#include "source_pane.hpp"
#include "data_pane.hpp"
#include "table_pane.hpp"
#include "editor.hpp"
#include "components.hpp"
#include "object_dialogs.hpp"
#include "navigation_pane.hpp"
#include <QUuid>
#include <QTabWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QLabel>
#include <QTimer>
#include <algorithm>
SourcePane* MainWindow::createSourcePane(const navidog::ConnectionProfile& p,const navidog::CatalogNode& node,bool existing){auto* pane=new SourcePane(p,node,existing);pane->setSqlTools(*editorSqlTools_);auto index=addWorkspaceTab(pane,(existing?"设计 · ":"新建 · ")+node.object+(pane->dirty()?" *":""));tabs_->setTabIcon(index,navidog::ui::objectIcon(node.kind));tabs_->setCurrentIndex(index);connect(pane,&SourcePane::formatRequested,this,&MainWindow::formatCurrent);connect(pane,&SourcePane::saveRequested,this,[this,pane]{saveSource(pane);});connect(pane,&SourcePane::reloadRequested,this,[this,pane]{if(pane->dirty() && QMessageBox::question(this,"重新读取","放弃本地定义修改并重新读取？",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Discard)return;loadSource(pane);});connect(pane,&SourcePane::stopRequested,this,[this]{service_.cancel();});connect(pane,&SourcePane::callRequested,this,[this,pane]{callSource(pane);});connect(pane,&SourcePane::draftChanged,this,[this,pane]{auto index=tabs_->indexOf(pane);if(index>=0)tabs_->setTabText(index,(pane->baseline.exists?"设计 · ":"新建 · ")+pane->name->text()+(pane->dirty()?" *":""));});pane->setBusy(service_.busy());metadata_->updateContext(*pane);return pane;}
void MainWindow::openSource(QTreeWidgetItem* item, bool existing) {
    if (!item) return;
    auto node = item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>();
    if (!existing && (node.kind == "views" || node.kind == "materializedViews" ||
                      node.kind == "functions" || node.kind == "procedures"))
        node.kind.chop(1);
    openSource(item->data(0, Qt::UserRole).toString(), std::move(node), existing);
}

void MainWindow::openSource(const QString& id, navidog::CatalogNode node, bool existing) {
    if (service_.busy() || workspaceCloseFlow_) return;
    const auto profile = std::find_if(profiles_.cbegin(), profiles_.cend(),
                                      [&](const auto& entry) { return entry.id == id; });
    if (profile == profiles_.cend() || !navidog::supportsSourceDesign(profile->driver, node.kind)) return;
    if (existing && node.kind == "materializedView") {
        showObjectProperties(id, node);
        return;
    }
    if (existing) {
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto* pane = qobject_cast<SourcePane*>(tabs_->widget(i)); pane && pane->profileId == id &&
                pane->baseline.exists && pane->node.kind == node.kind && pane->node.database == node.database &&
                pane->node.schema == node.schema && pane->node.object == node.object && pane->node.key == node.key) {
                tabs_->setCurrentIndex(i);
                if (!pane->loaded && !pane->loading) loadSource(pane);
                return;
            }
    } else {
        if (profile->readOnly || profile->http.enabled) return;
        node.object = node.kind == "view" ? "new_view" : node.kind == "materializedView" ? "new_materialized_view" :
                      node.kind == "function" ? "new_function" : "new_procedure";
        if (navidog::usesOracleSql(profile->driver)) node.object = node.object.toUpper();
        node.name = node.object;
        node.key.clear();
        node.detail.clear();
        if (node.schema.isEmpty() && navidog::usesNativePostgres(profile->driver)) node.schema = "public";
        if (node.schema.isEmpty() && profile->driver == navidog::DatabaseKind::SQLServer) node.schema = "dbo";
    }
    auto* pane = createSourcePane(*profile, node, existing);
    if (existing) loadSource(pane);
}
void MainWindow::loadSource(SourcePane* pane) {
    if(service_.busy() || pane->property("closingWorkspace").toBool())return;
    auto profile=std::find_if(profiles_.begin(),profiles_.end(),[pane](const auto& p){return p.id==pane->profileId;});
    if(profile==profiles_.end() || !promptPassword(*profile))return;
    pane->profile=*profile;
    if(!pane->database.isEmpty() && pane->profile.driver!=navidog::DatabaseKind::SQLite &&
       !navidog::usesOracleSql(pane->profile.driver))pane->profile.database=pane->database;
    if(!service_.sourceDefinition(pane->profile,pane->baseline.node,pane->requestId))return;
    pane->loading=true;
    pane->saving=false;
    sourcePane_=pane;
    refreshWorkspaceActivity();
    pane->message->setText("正在读取完整定义和参数…");
}
void MainWindow::saveSource(SourcePane* pane) {
    if (service_.busy() || pane->property("closingWorkspace").toBool() || !pane->loaded || pane->needsReload)
        return;
    const auto node = pane->desiredNode();
    if (node.object.isEmpty()) {
        showError("对象名称为空", "请先填写对象名称");
        return;
    }
    const QPointer<SourcePane> guarded(pane);
    if (pane->baseline.exists && navidog::isMySqlFamily(pane->profile.driver) && navidog::isRoutineObject(node.kind) &&
        QMessageBox::question(this, "重建 MySQL 例程",
            "保存会先核对原定义和授权，再删除并重建例程。失败时会尝试恢复，重建期间例程会短暂不可用。继续保存？",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (!guarded) return;
    const auto target = databaseToolProfile(pane->profileId, node);
    if (!target || !guarded || pane->property("closingWorkspace").toBool() ||
        !service_.saveSource(*target, pane->baseline, node, pane->editor->toPlainText(), false, pane->requestId))
        return;
    pane->profile = *target;
    pane->saving = true;
    pane->loading = false;
    sourcePane_ = pane;
    refreshWorkspaceActivity();
    pane->message->setText("正在保存对象定义…");
}
void MainWindow::showObjectProperties(const QString& id, const navidog::CatalogNode& node) {
    if (service_.busy()) return;
    const auto target = databaseToolProfile(id, node);
    if (!target || !navidog::supportsObjectProperties(target->driver, node.kind)) return;
    auto* dialog = new navidog::ui::ObjectDetailsDialog(*target, node, this);
    const auto request = QUuid::createUuid().toString(QUuid::Id128);
    connect(&service_, &navidog::QueryService::objectDetailsCompleted, dialog,
            [dialog, request](const QString& done, const navidog::CatalogObjectDetails& details) {
                if (done == request) dialog->showDetails(details);
            });
    if (!service_.objectDetails(*target, node, request)) {
        dialog->deleteLater();
        return;
    }
    refreshWorkspaceActivity();
    dialog->show();
}

void MainWindow::changeObject(const QString& id, const navidog::CatalogNode& node,
                              navidog::CatalogObjectChange change) {
    using Change = navidog::CatalogObjectChange;
    if (service_.busy() || workspaceCloseFlow_) return;
    const auto saved = std::find_if(profiles_.cbegin(), profiles_.cend(),
                                    [&](const auto& profile) { return profile.id == id; });
    if (saved == profiles_.cend()) return;
    const auto previewProfile = navidog::scopeDatabase(*saved, node);
    const auto matches = [id, node, driver = previewProfile.driver](const SourcePane& pane) {
        const auto& stable = pane.baseline.node;
        return pane.baseline.exists && pane.profileId == id && stable.kind == node.kind &&
            stable.database == node.database && stable.schema == node.schema && stable.object == node.object &&
            (!navidog::usesNativePostgres(driver) || !navidog::isRoutineObject(node.kind) ||
             stable.key == node.key);
    };
    const auto hasDirtyDraft = [&] {
        for (int i = 0; i < tabs_->count(); ++i) {
            auto* widget = tabs_->widget(i);
            if (auto* pane = qobject_cast<SourcePane*>(widget); pane && matches(*pane) && pane->dirty())
                return true;
            if (auto* pane = qobject_cast<DataPane*>(widget); pane && pane->matchesTable(id,node) && pane->dirty())
                return true;
            if (auto* pane = qobject_cast<TablePane*>(widget); pane && pane->matchesTable(id,node) && pane->dirty())
                return true;
        }
        return false;
    };
    if (change != Change::Compile && hasDirtyDraft()) {
        showError("还有未保存的定义", "请先处理此对象的定义草稿。");
        return;
    }
    try {
        QString newName;
        if (change==Change::Rename) {
            const auto chosen = navidog::ui::chooseObjectName(this,previewProfile,node);
            if (!chosen) return;
            newName = *chosen;
        } else {
            const auto preview = navidog::catalogObjectChangeSql(previewProfile, node, change);
            if (change!=Change::Compile && !navidog::ui::confirmObjectDrop(this, previewProfile, node, change, preview))
                return;
        }
        const auto target = databaseToolProfile(id, node);
        if (!target || service_.busy() || workspaceCloseFlow_ || target->driver != previewProfile.driver ||
            (change != Change::Compile && hasDirtyDraft()))
            return;
        const auto request = QUuid::createUuid().toString(QUuid::Id128);
        if (!service_.changeObject(*target, node, change, request, newName)) return;
        auto completion = std::make_shared<QMetaObject::Connection>();
        *completion = connect(&service_, &navidog::QueryService::objectChangeCompleted, this,
            [this, request, completion, change, id, node, matches, newName](const QString& done, const navidog::CatalogObjectChangeResult& result) {
                if (done != request) return;
                disconnect(*completion);
                if (result.applied || result.cancelled) navigation_->queueRefresh(id);
                if (result.cancelled) status_->setText("操作已停止，正在刷新目录核对");
                else if (!result.error.isEmpty()) {
                    status_->setText(change==Change::Compile ? "编译失败" : change==Change::Rename ? "重命名失败" : "删除失败");
                    showError(status_->text(), result.error +
                              (result.diagnostics.isEmpty() ? QString{} : "\n" + result.diagnostics));
                } else if (change == Change::Compile) {
                    status_->setText("服务器已编译对象");
                    if (!result.diagnostics.isEmpty()) showError("服务器编译信息", result.diagnostics);
                } else if (result.applied) {
                    for (int i = tabs_->count() - 1; i >= 0; --i) {
                        auto* pane = tabs_->widget(i);
                        auto* source = qobject_cast<SourcePane*>(pane);
                        auto* data = qobject_cast<DataPane*>(pane);
                        if ((source && matches(*source) && !source->dirty()) ||
                            (data && navidog::isSourceRelation(node.kind) && data->matchesTable(id, node) && !data->dirty())) {
                            tabs_->removeTab(i);
                            pane->deleteLater();
                        } else if (change==Change::Rename) {
                            if (data && data->matchesTable(id,node)) data->invalidateTable();
                            if (auto* table = qobject_cast<TablePane*>(pane); table && table->matchesTable(id,node))
                                table->invalidateTable();
                        }
                    }
                    status_->setText(change==Change::Rename ? "已重命名为 " + newName : QString("对象已删除"));
                    if (tabs_->count() == 0) newQuery();
                }
                refreshWorkspaceActivity();
                emit queryFinished();
            });
        refreshWorkspaceActivity();
        status_->setText(change==Change::Compile ? "正在编译对象…" : change==Change::Rename ? "正在重命名对象…" : "正在删除对象…");
    } catch (const std::exception& error) {
        showError("无法操作对象", QString::fromUtf8(error.what()));
    }
}

void MainWindow::dropSource(QTreeWidgetItem* item) {
    if (item) changeObject(item->data(0, Qt::UserRole).toString(),
        item->data(0, Qt::UserRole + 2).value<navidog::CatalogNode>(), navidog::CatalogObjectChange::Drop);
}
void MainWindow::callSource(SourcePane* pane){if(tabs_->indexOf(pane)<0 || !pane->loaded || !pane->baseline.exists || pane->dirty())return;auto n=pane->baseline.node;if(navidog::isSourceRelation(n.kind)){QTreeWidgetItem item;item.setData(0,Qt::UserRole,pane->profileId);item.setData(0,Qt::UserRole+2,QVariant::fromValue(n));openObject(&item,false);return;}try{auto sql=navidog::sourceCallSql(pane->profile,pane->baseline);newQuery(sql,pane->profileId,pane->database);}catch(const std::exception& e){showError("无法生成调用 SQL",QString::fromUtf8(e.what()));}
}
