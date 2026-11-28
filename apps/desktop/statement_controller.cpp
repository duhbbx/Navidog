#include "main_window.hpp"
#include <QTreeWidgetItem>
#include <QLabel>
#include <QUuid>
#include <algorithm>
void MainWindow::generateStatement(QTreeWidgetItem* item,navidog::StatementTemplate kind){
    if(!item || service_.busy())return;auto id=item->data(0,Qt::UserRole).toString();auto found=std::find_if(profiles_.begin(),profiles_.end(),[&](const auto& p){return p.id==id;});if(found==profiles_.end() || !promptPassword(*found))return;
    auto profile=*found;auto node=item->data(0,Qt::UserRole+2).value<navidog::CatalogNode>();auto request=QUuid::createUuid().toString();auto* scope=new QObject(this);
    connect(&service_,&navidog::QueryService::tableDesignCompleted,scope,[this,scope,request,profile,kind](const QString& response,const navidog::TableDesignResult& result){
        if(response!=request)return;scope->deleteLater();refreshWorkspaceActivity();if(result.cancelled){status_->setText("已停止生成 SQL");return;}if(!result.error.isEmpty()){showError("读取字段失败",result.error);return;}
        try{newQuery(navidog::statementTemplate(profile,result.definition,kind),profile.id,result.definition.node.database);status_->setText("已生成 SQL 草稿，尚未执行");}catch(const std::exception& e){showError("生成 SQL 失败",QString::fromUtf8(e.what()));}
    });
    if(service_.tableDefinition(profile,node,request)){refreshWorkspaceActivity();status_->setText("正在读取字段并生成 SQL…");}else scope->deleteLater();
}
