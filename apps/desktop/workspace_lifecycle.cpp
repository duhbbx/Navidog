#include "main_window.hpp"
#include "query_session_controller.hpp"
#include "connection_controller.hpp"
#include "query_pane.hpp"
#include "data_pane.hpp"
#include "table_pane.hpp"
#include "source_pane.hpp"
#include "er_pane.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTimer>

namespace {
bool discardChanges(QWidget* parent,const QString& title,const QString& text) {
    QMessageBox box(QMessageBox::Question,title,text,QMessageBox::Discard|QMessageBox::Cancel,parent);
    box.setObjectName("workspaceDiscardChanges");box.setTextFormat(Qt::PlainText);
    box.button(QMessageBox::Discard)->setText("放弃修改并关闭");
    box.button(QMessageBox::Cancel)->setText("返回自行处理");
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec()==QMessageBox::Discard;
}
bool transactionOpen(const QueryPane* pane) {
    return pane->transactionState==navidog::TransactionState::Active ||
        pane->transactionState==navidog::TransactionState::Failed ||
        pane->transactionState==navidog::TransactionState::Unknown;
}
}

bool MainWindow::prepareWorkspaceClose(WorkspacePane* pane,bool transactionConfirmed) {
    const auto title=tabs_->tabText(tabs_->indexOf(pane));
    if (!transactionConfirmed && databaseConsoleHasTransaction(pane)) {
        tabs_->setCurrentWidget(pane);
        return QMessageBox::question(this, "未提交事务", "关闭命令列会回滚未提交事务。继续关闭？",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
    }
    if(auto* query=dynamic_cast<QueryPane*>(pane)) {
        if(query->kind==navidog::WorkspaceTabKind::Query && query->dirty()) {
            tabs_->setCurrentWidget(pane);
            QMessageBox box(QMessageBox::Question,"保存 SQL 草稿",QString("“%1”的 SQL 有未保存修改。关闭前是否保存到文件？").arg(title),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,this);
            box.setObjectName("workspaceSaveSql");box.setTextFormat(Qt::PlainText);
            box.button(QMessageBox::Discard)->setText("不保存并关闭");box.setDefaultButton(QMessageBox::Cancel);
            const auto choice=box.exec();
            if(choice!=QMessageBox::Save && choice!=QMessageBox::Discard)return false;
            if(choice==QMessageBox::Save && !query->saveSqlFile())return false;
        }
        if(!transactionConfirmed && transactionOpen(query)) {
            tabs_->setCurrentWidget(pane);
            QMessageBox box(QMessageBox::Warning,"未提交事务",QString("关闭“%1”将关闭查询会话并回滚未提交事务。继续关闭？").arg(title),QMessageBox::Yes|QMessageBox::Cancel,this);
            box.setObjectName("workspaceCloseTransaction");box.setTextFormat(Qt::PlainText);
            box.button(QMessageBox::Yes)->setText("回滚并关闭");box.setDefaultButton(QMessageBox::Cancel);
            if(box.exec()!=QMessageBox::Yes)return false;
        }
    } else if(auto* data=dynamic_cast<DataPane*>(pane)) {
        data->finishCellEditing();
        if(data->dirty()) {
            tabs_->setCurrentWidget(pane);
            if(!discardChanges(this,"关闭数据页",QString("“%1”仍有尚未提交的数据修改。关闭将放弃这些本地修改，不会自动提交到数据库。").arg(title)))return false;
        }
    } else if(auto* table=dynamic_cast<TablePane*>(pane)) {
        table->commitEditor();
        if(table->dirty()) {
            tabs_->setCurrentWidget(pane);
            if(!discardChanges(this,"关闭表设计",QString("放弃“%1”尚未保存的表结构修改并关闭？").arg(title)))return false;
        }
    } else if(auto* diagram=dynamic_cast<ErPane*>(pane)) {
        tabs_->setCurrentWidget(pane);
        return diagram->prepareClose();
    } else if(auto* source=dynamic_cast<SourcePane*>(pane);source && source->dirty()) {
        tabs_->setCurrentWidget(pane);
        if(!discardChanges(this,"关闭定义编辑",QString("放弃“%1”尚未保存的对象定义并关闭？").arg(title)))return false;
    }
    return true;
}

void MainWindow::disposeWorkspacePane(WorkspacePane* pane) {
    const auto index=tabs_->indexOf(pane);if(index<0)return;
    if(auto* diagram=dynamic_cast<ErPane*>(pane))diagram->forgetDraft();
    if(auto* query=dynamic_cast<QueryPane*>(pane))querySessions_->release(*query,false);
    if(auto* data=dynamic_cast<DataPane*>(pane))data->cancelLoad();
    if(auto* source=dynamic_cast<SourcePane*>(pane);source && source->loading && sourcePane_==source) {
        sourcePane_.clear();
        service_.cancel();
    }
    tabs_->removeTab(index);pane->deleteLater();
}

void MainWindow::requestCloseWorkspaceTab(WorkspacePane* pane) {
    if(!pane || tabs_->indexOf(pane)<0)return;
    closeWorkspacePanes({QPointer<WorkspacePane>(pane)});
}

void MainWindow::closeWorkspacePanes(const QVector<QPointer<WorkspacePane>>& targets,const QString& editAfter,bool transactionConfirmed) {
    if(workspaceCloseFlow_ || !pendingCloseTabs_.isEmpty()) {status_->setText("正在处理标签关闭，请稍候");return;}
    workspaceCloseFlow_=true;
    const auto restoreControls=qScopeGuard([this]{workspaceCloseFlow_=false;refreshWorkspaceActivity();});
    for(const auto& ptr:targets)if(ptr && tabs_->indexOf(ptr)>=0) {
        const auto* data=dynamic_cast<DataPane*>(ptr.data());
        const bool dataWrite=data && data->isCommitting();
        const auto* table=dynamic_cast<TablePane*>(ptr.data());
        const bool tableOperation=table && table->designInProgress();
        const auto* source=dynamic_cast<SourcePane*>(ptr.data());
        const bool sourceWrite=sourcePane_==ptr && source && source->saving;
        if(service_.busy() && (tableOperation || dataWrite || sourceWrite)) {
            tabs_->setCurrentWidget(ptr);
            QMessageBox::information(this,"标签正在处理数据","此标签正在读取或保存数据库对象。请等待操作完成，或先使用该页的停止按钮，再关闭标签。");return;
        }
        if(auto* query=dynamic_cast<QueryPane*>(ptr.data());query && query->executing && !transactionConfirmed) {
            QMessageBox box(QMessageBox::Warning,"停止查询并关闭","此查询仍在执行。停止并关闭会话可能回滚未提交事务，已经提交的修改不会撤销。是否停止并关闭？",QMessageBox::Yes|QMessageBox::Cancel,this);
            box.setObjectName("workspaceStopAndClose");box.button(QMessageBox::Yes)->setText("停止并关闭");box.setDefaultButton(QMessageBox::Cancel);
            if(box.exec()!=QMessageBox::Yes)return;
            transactionConfirmed=true;
        }
    }
    // Confirm every draft first. Cancelling a later prompt leaves every tab and
    // transaction in this batch in place; saving a chosen SQL file is retained.
    for(const auto& ptr:targets)if(ptr && tabs_->indexOf(ptr)>=0 && !prepareWorkspaceClose(ptr,transactionConfirmed)) {
        status_->setText("已取消关闭，相关标签和未提交事务已保留");return;
    }
    pendingCloseEdit_=editAfter;
    for(const auto& ptr:targets)if(ptr && tabs_->indexOf(ptr)>=0) {
        if(auto* query=dynamic_cast<QueryPane*>(ptr.data()))query->astView->prepareForClose();
        ptr->setProperty("closingWorkspace",true);ptr->setEnabled(false);if(auto* query=dynamic_cast<QueryPane*>(ptr.data()))query->editor->setReadOnly(true);pendingCloseTabs_<<ptr;
    }
    for(const auto& ptr:pendingCloseTabs_)if(auto* query=dynamic_cast<QueryPane*>(ptr.data());query && query->executing)
        querySessions_->cancel(*query);
    refreshWorkspaceActivity();
    status_->setText("正在关闭相关标签…");
    QTimer::singleShot(0,this,&MainWindow::finishPendingWorkspaceClose);
}

void MainWindow::finishPendingWorkspaceClose() {
    if(workspaceCloseFlow_ || pendingCloseTabs_.isEmpty())return;
    for(const auto& ptr:pendingCloseTabs_)if(auto* query=dynamic_cast<QueryPane*>(ptr.data());query && query->executing)return;
    const auto targets=pendingCloseTabs_;const auto editing=pendingCloseEdit_;
    pendingCloseTabs_.clear();pendingCloseEdit_.clear();
    for(const auto& ptr:targets)if(ptr) {
        ptr->setProperty("closingWorkspace",false);ptr->setEnabled(true);disposeWorkspacePane(ptr);
    }
    refreshWorkspaceActivity();status_->setText("标签已关闭");
    if(!editing.isEmpty()) {pendingEditAfterClose_=editing;QTimer::singleShot(0,this,&MainWindow::resumeConnectionEdit);}
}

void MainWindow::resumeConnectionEdit() {
    if(pendingEditAfterClose_.isEmpty())return;
    if(workspaceCloseFlow_ || !pendingCloseTabs_.isEmpty() || service_.operationBusy() || service_.catalogBusy() || QApplication::activeModalWidget()) {
        QTimer::singleShot(100,this,&MainWindow::resumeConnectionEdit);return;
    }
    const auto id=pendingEditAfterClose_;pendingEditAfterClose_.clear();connectionController_->edit(id);
}

bool MainWindow::confirmConnectionTabClosure(const QString& id) {
    QVector<QPointer<WorkspacePane>> targets;QStringList titles;bool running=false;
    for(int i=0;i<tabs_->count();++i)if(auto* pane=dynamic_cast<WorkspacePane*>(tabs_->widget(i));pane && pane->profileId==id) {
        targets<<pane;titles<<tabs_->tabText(i);
        if(auto* query=dynamic_cast<QueryPane*>(pane);query && query->executing)running=true;
    }
    if(targets.isEmpty())return true;
    QMessageBox box(QMessageBox::Question,"编辑连接前关闭标签",QString("此连接还有 %1 个打开的标签。\n可以关闭这些标签后继续编辑，或返回自行处理。\n\n关闭时会确认未保存修改；未提交的查询事务将回滚。%2").arg(targets.size()).arg(running?"正在运行的查询会先停止，已提交的修改不会撤销。":""),QMessageBox::NoButton,this);
    box.setObjectName("connectionCloseTabsPrompt");box.setTextFormat(Qt::PlainText);box.setDetailedText(titles.join('\n'));
    auto* close=box.addButton("关闭相关标签并编辑",QMessageBox::AcceptRole);close->setObjectName("closeConnectionTabsAndEdit");
    auto* manual=box.addButton("返回自行处理",QMessageBox::RejectRole);manual->setObjectName("handleConnectionTabsManually");box.setDefaultButton(manual);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if(box.clickedButton()==close)closeWorkspacePanes(targets,id,true);
    else if(box.clickedButton()==manual && targets.front())tabs_->setCurrentWidget(targets.front());
    return false;
}
