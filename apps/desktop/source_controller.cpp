#include "main_window.hpp"
#include "metadata_controller.hpp"
#include "source_pane.hpp"
#include "editor.hpp"
#include "components.hpp"
#include <QTabWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QInputDialog>
#include <QLabel>
#include <QTimer>
#include <algorithm>
SourcePane* MainWindow::createSourcePane(const navidog::ConnectionProfile& p,const navidog::CatalogNode& node,bool existing){auto* pane=new SourcePane(p,node,existing);pane->setSqlTools(*editorSqlTools_);auto index=addWorkspaceTab(pane,(existing?"设计 · ":"新建 · ")+node.object+(pane->dirty()?" *":""));tabs_->setTabIcon(index,navidog::ui::objectIcon(node.kind));tabs_->setCurrentIndex(index);connect(pane,&SourcePane::formatRequested,this,&MainWindow::formatCurrent);connect(pane,&SourcePane::saveRequested,this,[this,pane]{saveSource(pane);});connect(pane,&SourcePane::reloadRequested,this,[this,pane]{if(pane->dirty() && QMessageBox::question(this,"重新读取","放弃本地定义修改并重新读取？",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Discard)return;loadSource(pane);});connect(pane,&SourcePane::stopRequested,this,[this]{service_.cancel();});connect(pane,&SourcePane::callRequested,this,[this,pane]{callSource(pane);});connect(pane,&SourcePane::draftChanged,this,[this,pane]{auto index=tabs_->indexOf(pane);if(index>=0)tabs_->setTabText(index,(pane->baseline.exists?"设计 · ":"新建 · ")+pane->name->text()+(pane->dirty()?" *":""));});pane->setBusy(service_.busy());metadata_->updateContext(*pane);return pane;}
void MainWindow::openSource(QTreeWidgetItem* item,bool existing){if(!item || service_.busy())return;auto node=item->data(0,Qt::UserRole+2).value<navidog::CatalogNode>();auto id=item->data(0,Qt::UserRole).toString();auto profile=std::find_if(profiles_.begin(),profiles_.end(),[&](auto p){return p.id==id;});if(profile==profiles_.end())return;if(node.kind.endsWith('s'))node.kind.chop(1);if(!navidog::supportsSourceDesign(profile->driver,node.kind))return;
    if(existing)for(int i=0;i<tabs_->count();++i)if(auto* pane=dynamic_cast<SourcePane*>(tabs_->widget(i));pane && pane->profileId==id && pane->baseline.exists && pane->node.kind==node.kind && pane->node.database==node.database && pane->node.schema==node.schema && pane->node.object==node.object && pane->node.key==node.key){tabs_->setCurrentIndex(i);if(!pane->loaded && !pane->loading)loadSource(pane);return;}
    if(!existing){node.object=node.name=node.kind=="view"?"new_view":node.kind=="function"?"new_function":"new_procedure";node.key.clear();node.detail.clear();if(navidog::usesOracleSql(profile->driver))node.object=node.name=node.object.toUpper();if(profile->driver==navidog::DatabaseKind::PostgreSQL && node.schema.isEmpty())node.schema="public";if(profile->driver==navidog::DatabaseKind::SQLServer && node.schema.isEmpty())node.schema="dbo";}
    auto* pane=createSourcePane(*profile,node,existing);if(existing)loadSource(pane);
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
void MainWindow::saveSource(SourcePane* pane,bool remove){if(service_.busy() || pane->property("closingWorkspace").toBool() || !pane->loaded || pane->needsReload)return;if(pane->desiredNode().object.isEmpty()){showError("对象名称为空","请先填写对象名称");return;}auto profile=std::find_if(profiles_.begin(),profiles_.end(),[&](auto p){return p.id==pane->profileId;});if(profile==profiles_.end() || !promptPassword(*profile))return;
    if(!remove && pane->baseline.exists && (navidog::isMySqlFamily(profile->driver)) && pane->node.kind!="view" && QMessageBox::question(this,"重建 MySQL 例程","MySQL 需要删除后重建才能修改例程正文。将核对原定义、保留授权，创建失败时尝试恢复原定义和授权。此过程不能放进事务，执行期间例程会短暂不可用。继续保存？",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
    pane->profile=*profile;if(!pane->database.isEmpty() && profile->driver!=navidog::DatabaseKind::SQLite && !navidog::usesOracleSql(profile->driver))pane->profile.database=pane->database;pane->saving=true;pane->loading=false;sourcePane_=pane;if(service_.saveSource(pane->profile,pane->baseline,pane->desiredNode(),pane->editor->toPlainText(),remove,pane->requestId)){refreshWorkspaceActivity();pane->message->setText(remove?"正在删除对象…":"正在保存对象定义…");}}
void MainWindow::dropSource(QTreeWidgetItem* item){if(service_.busy() || !item)return;auto node=item->data(0,Qt::UserRole+2).value<navidog::CatalogNode>();bool ok=false;auto typed=QInputDialog::getText(this,"删除数据库对象","此操作会删除对象。请输入完整名称确认："+node.object,QLineEdit::Normal,{},&ok);if(!ok || typed!=node.object)return;openSource(item,true);auto* pane=dynamic_cast<SourcePane*>(tabs_->currentWidget());if(!pane)return;if(pane->dirty()){showError("尚有定义草稿","请先保存或放弃此对象的草稿，再删除对象");return;}if(pane->loaded)saveSource(pane,true);else{auto id=pane->requestId;auto connection=std::make_shared<QMetaObject::Connection>();*connection=connect(&service_,&navidog::QueryService::sourceDesignCompleted,pane,[this,pane,id,connection](const QString& completed,const navidog::SourceDesignResult& result){if(completed!=id)return;disconnect(*connection);if(result.error.isEmpty() && !result.cancelled)QTimer::singleShot(0,pane,[this,pane]{if(tabs_->indexOf(pane)>=0)saveSource(pane,true);});});}}
void MainWindow::callSource(SourcePane* pane){if(tabs_->indexOf(pane)<0 || !pane->loaded || !pane->baseline.exists || pane->dirty())return;auto n=pane->baseline.node;if(n.kind=="view"){QTreeWidgetItem item;item.setData(0,Qt::UserRole,pane->profileId);item.setData(0,Qt::UserRole+2,QVariant::fromValue(n));openObject(&item,false);return;}try{auto sql=navidog::sourceCallSql(pane->profile,pane->baseline);newQuery(sql,pane->profileId,pane->database);}catch(const std::exception& e){showError("无法生成调用 SQL",QString::fromUtf8(e.what()));}
}
