#include "main_window.hpp"
#include "data_pane.hpp"
#include "export_paths.hpp"
#include "navigation_pane.hpp"
#include "navidog/database_tools.hpp"
#include "object_dialogs.hpp"
#include "source_pane.hpp"
#include "table_pane.hpp"

#include <QBuffer>
#include <QClipboard>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QSaveFile>
#include <QTabWidget>
#include <QUuid>
#include <algorithm>

namespace {
using namespace navidog;
struct TemplateChoice { const char* title; std::optional<StatementTemplate> type; };
const TemplateChoice choices[] = {{"对象定义", {}}, {"SELECT", StatementTemplate::Select},
    {"INSERT", StatementTemplate::Insert}, {"UPDATE", StatementTemplate::Update}, {"DELETE", StatementTemplate::Delete}};

bool matchesSource(const SourcePane& pane, const QString& profileId, const CatalogNode& node) {
    const auto& saved = pane.baseline.node;
    return pane.baseline.exists && pane.profileId==profileId && saved.database==node.database &&
        saved.schema==node.schema && saved.kind==node.kind && saved.object==node.object &&
        (!isRoutineObject(node.kind) || saved.key==node.key);
}

bool matchesPane(QWidget* widget, const QString& id, const CatalogNode& node, bool requireDirty) {
    if (auto* pane=qobject_cast<SourcePane*>(widget))
        return matchesSource(*pane,id,node) && (!requireDirty || pane->dirty());
    if (node.kind!="table" && !isSourceRelation(node.kind)) return false;
    if (auto* pane=qobject_cast<DataPane*>(widget))
        return pane->matchesTable(id,node) && (!requireDirty || pane->dirty());
    if (auto* pane=qobject_cast<TablePane*>(widget))
        return pane->matchesTable(id,node) && (!requireDirty || pane->dirty());
    return false;
}
}

void MainWindow::populateBatchObjectMenu(QMenu* menu, const QString& id, const QVector<navidog::CatalogNode>& nodes) {
    const auto profile=std::find_if(profiles_.cbegin(),profiles_.cend(),[&](const auto& entry) { return entry.id==id; });
    if (profile==profiles_.cend() || nodes.size()<2) return;
    const auto& kind=nodes.front().kind;
    const auto driver=profile->driver;
    auto* heading=menu->addAction(QString("已选择 %1 个对象").arg(nodes.size()));
    heading->setEnabled(false);
    menu->addSeparator();
    auto* generate=menu->addMenu("批量生成 SQL");
    generate->setObjectName("contextGenerateBatchSql");
    auto* exportMenu=menu->addMenu("批量导出 SQL");
    exportMenu->setObjectName("contextExportBatchSql");
    for (const auto& choice : choices) {
        if (!navidog::supportsObjectBatchSql(driver,kind,choice.type)) continue;
        const auto type=choice.type;
        generate->addAction(choice.title,this,[this,id,nodes,type] { submitBatchObjectSql(id,nodes,type,false); });
        exportMenu->addAction(choice.title,this,[this,id,nodes,type] { submitBatchObjectSql(id,nodes,type,true); });
    }
    generate->setEnabled(!service_.busy() && !generate->isEmpty());
    exportMenu->setEnabled(!service_.busy() && !exportMenu->isEmpty());
    if (navidog::supportsObjectBatchDrop(driver,kind)) {
        const bool writable=!profile->readOnly && !profile->http.enabled && !service_.busy();
        menu->addSeparator();
        auto* drop=menu->addAction("批量删除…",this,[this,id,nodes] { deleteSelectedObjects(id,nodes,false); });
        drop->setObjectName("contextBatchDrop");
        drop->setEnabled(writable);
        if (navidog::supportsObjectCascade(driver,kind)) {
            auto* cascade=menu->addAction("批量级联删除…",this,[this,id,nodes] { deleteSelectedObjects(id,nodes,true); });
            cascade->setObjectName("contextBatchCascadeDrop");
            cascade->setEnabled(writable);
        }
    }
    menu->addSeparator();
    menu->addAction("复制名称",this,[nodes] {
        QStringList names;
        for (const auto& node : nodes) names << node.name;
        QApplication::clipboard()->setText(names.join('\n'));
    });
}

void MainWindow::submitBatchObjectSql(const QString& id, const QVector<navidog::CatalogNode>& nodes,
                                      std::optional<navidog::StatementTemplate> statement, bool saveFile) {
    if (nodes.size()<2 || service_.busy() || workspaceCloseFlow_) return;
    if (nodes.size()>128) { showError("选中对象过多","单批最多处理 128 个对象"); return; }
    const auto& context=nodes.front();
    const auto profile=databaseToolProfile(id,context);
    if (!profile || !navidog::supportsObjectBatchSql(profile->driver,context.kind,statement)) return;
    QString path;
    const auto title=saveFile ? QString("批量导出 SQL") : QString("批量生成 SQL");
    if (saveFile) {
        path=navidog::ui::chooseExportFile(this,title,navidog::ui::exportContext(*profile,context.database,context.schema),
                                           title,"SQL (*.sql)");
        if (path.isEmpty()) return;
    }
    QString rejection;
    const auto job=tasks_->submit({title,QString("%1 · %2 个对象").arg(profile->name).arg(nodes.size()),{},path},
        [profile=*profile,nodes,statement,path](const navidog::TaskContext& task) {
            navidog::TaskResult result;
            if (!path.isEmpty()) {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
                navidog::writeCatalogBatchSql(profile,nodes,statement,file,task);
                if (task.cancel->load()) { result.cancelled=true; return result; }
                if (!file.commit()) throw std::runtime_error(file.errorString().toStdString());
                result.artifacts << QFileInfo(path).absoluteFilePath();
            } else {
                QBuffer temporary;
                temporary.open(QIODevice::WriteOnly);
                navidog::writeCatalogBatchSql(profile,nodes,statement,temporary,task,4*1024*1024);
                if (task.cancel->load()) { result.cancelled=true; return result; }
                result.payload=temporary.data();
            }
            result.detail=QString("已处理 %1 个对象").arg(nodes.size());
            return result;
        },&rejection);
    if (job.isEmpty()) { showError("无法启动批量 SQL 任务",rejection); return; }
    status_->setText("已提交"+title+"任务");
    if (!saveFile) {
        auto* scope=new QObject(this);
        connect(tasks_,&navidog::BackgroundTasks::finished,scope,
            [this,scope,job,id,context](const QString& done,const navidog::TaskResult& result) {
                if (done!=job) return;
                scope->deleteLater();
                if (result.cancelled) status_->setText("已取消生成 SQL");
                else if (!result.error.isEmpty()) showError("批量生成 SQL 失败",result.error);
                else {
                    newQuery(QString::fromUtf8(result.payload.toByteArray()),id,context.database,
                             navidog::WorkspaceTabKind::Query,context.schema);
                    status_->setText("已生成 SQL 草稿");
                }
            });
    }
}

void MainWindow::deleteSelectedObjects(const QString& id, const QVector<navidog::CatalogNode>& nodes, bool cascade) {
    if (nodes.size()<2 || service_.busy() || workspaceCloseFlow_) return;
    const auto saved=std::find_if(profiles_.cbegin(),profiles_.cend(),[&](const auto& profile) { return profile.id==id; });
    if (saved==profiles_.cend()) return;
    const auto haveDrafts=[&] {
        for (int i=0;i<tabs_->count();++i)
            for (const auto& node : nodes)
                if (matchesPane(tabs_->widget(i),id,node,true)) return true;
        return false;
    };
    if (haveDrafts()) {showError("还有未保存的草稿","请先处理所选对象的编辑草稿。");return;}
    try {
        const auto preview=navidog::scopeDatabase(*saved,nodes.front());
        const auto sql=navidog::catalogBatchDropSql(preview,nodes,cascade);
        if (!navidog::ui::confirmObjectBatchDrop(this,preview.driver,int(nodes.size()),cascade,sql)) return;
        const auto target=databaseToolProfile(id,nodes.front());
        if (!target || target->driver!=preview.driver || workspaceCloseFlow_ || haveDrafts()) return;
        const auto request=QUuid::createUuid().toString(QUuid::Id128);
        if (!service_.dropObjects(*target,nodes,cascade,request)) return;
        auto* scope=new QObject(this);
        connect(&service_,&navidog::QueryService::objectBatchDropCompleted,scope,
            [this,scope,id,request,total=nodes.size()](const QString& done,const navidog::CatalogBatchDropResult& result) {
                if (done!=request) return;
                scope->deleteLater();
                for (int i=tabs_->count()-1;i>=0;--i) {
                    auto* pane=tabs_->widget(i);
                    if (std::any_of(result.removed.cbegin(),result.removed.cend(),[&](const auto& node) {
                        return matchesPane(pane,id,node,false) && !matchesPane(pane,id,node,true);
                    })) {
                        tabs_->removeTab(i);
                        pane->deleteLater();
                    }
                }
                navigation_->removeTableSettings(id,result.removed);
                navigation_->queueRefresh(id);
                if (tabs_->count()==0) newQuery();
                refreshWorkspaceActivity();
                const auto summary=QString("已删除 %1 / %2 个对象").arg(result.removed.size()).arg(total);
                status_->setText(summary);
                if (!result.error.isEmpty())
                    showError("批量删除失败",summary+(result.rolledBack?" · 已回滚":"")+"\n"+result.error);
                else if (result.cancelled) status_->setText(summary+" · 已停止，正在刷新确认");
                emit queryFinished();
            });
        refreshWorkspaceActivity();
        status_->setText("正在批量删除对象…");
    } catch (const std::exception& error) {
        showError("无法批量删除",QString::fromUtf8(error.what()));
    }
}
