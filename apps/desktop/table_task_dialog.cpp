#include "export_paths.hpp"
#include "table_task_dialog.hpp"
#include "editor.hpp"
#include "result_model.hpp"
#include <QFileDialog>
#include <QApplication>
#include <QDateTime>
#include <QStyledItemDelegate>
#include <QAbstractTableModel>
#include <QRegularExpression>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTableView>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {using A=navidog::TableAction;bool dangerous(A a){return a==A::Drop || a==A::DeleteRows || a==A::Truncate;}bool fileAction(A a){return a==A::ImportCsv || a==A::ExportCsv || a==A::ExportSql || a==A::Dictionary;}}
class GenerationRuleModel final : public QAbstractTableModel {
public:
    QVector<navidog::TableColumn> columns;
    QVector<QString> kinds,values;
    explicit GenerationRuleModel(QObject* p):QAbstractTableModel(p){}
    int rowCount(const QModelIndex& p={})const override{return p.isValid()?0:columns.size();}
    int columnCount(const QModelIndex& p={})const override{return p.isValid()?0:4;}
    QVariant headerData(int n,Qt::Orientation o,int role)const override{if(role!=Qt::DisplayRole || o!=Qt::Horizontal)return {};return QStringList{"字段","类型","生成规则","起始值 / 内容"}.value(n);}
    QVariant data(const QModelIndex& i,int role)const override{if(!i.isValid() || (role!=Qt::DisplayRole && role!=Qt::EditRole))return {};if(i.column()==0)return columns[i.row()].name;if(i.column()==1)return columns[i.row()].type;if(i.column()==2){auto key=kinds[i.row()];if(role==Qt::EditRole)return key;return key=="default"?"数据库默认值":key=="sequence"?"递增整数":key=="series"?"文本加序号":key=="constant"?"固定值":"NULL";}return values[i.row()];}
    Qt::ItemFlags flags(const QModelIndex& i)const override{auto flags=QAbstractTableModel::flags(i);if(i.column()>=2 && !columns[i.row()].protectedReason.contains("生成") && !columns[i.row()].protectedReason.contains("标识"))flags|=Qt::ItemIsEditable;return flags;}
    bool setData(const QModelIndex& i,const QVariant& v,int role)override{if(role!=Qt::EditRole || !(flags(i)&Qt::ItemIsEditable))return false;if(i.column()==2)kinds[i.row()]=v.toString();else if(i.column()==3)values[i.row()]=v.toString();else return false;emit dataChanged(i,i);return true;}
    void load(const navidog::TableDefinition& d){beginResetModel();columns=d.columns;kinds.clear();values.clear();for(auto c:columns){QString kind="constant",value="sample";if(!c.defaultExpression.isEmpty() || c.primaryKey || c.protectedReason.contains("生成") || c.protectedReason.contains("标识"))kind="default";else if(c.nullable)kind="null";else if(QRegularExpression("int|number|numeric|decimal|real|float|double",QRegularExpression::CaseInsensitiveOption).match(c.type).hasMatch()){kind="sequence";value="1";}else if(QRegularExpression("date|time",QRegularExpression::CaseInsensitiveOption).match(c.type).hasMatch())value=QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss");else {kind="series";value=c.name+"_";}kinds<<kind;values<<value;}endResetModel();}
    QJsonObject rules()const{QJsonObject out;for(int i=0;i<columns.size();++i)out.insert(columns[i].name,QJsonObject{{"kind",kinds[i]},{"value",values[i]}});return out;}
};
class GenerationRuleDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent,const QStyleOptionViewItem& option,const QModelIndex& i)const override{if(i.column()!=2)return QStyledItemDelegate::createEditor(parent,option,i);auto* combo=new navidog::ui::Select(parent);for(auto pair:{qMakePair(QStringLiteral("数据库默认值"),QStringLiteral("default")),qMakePair(QStringLiteral("递增整数"),QStringLiteral("sequence")),qMakePair(QStringLiteral("文本加序号"),QStringLiteral("series")),qMakePair(QStringLiteral("固定值"),QStringLiteral("constant")),qMakePair(QStringLiteral("NULL"),QStringLiteral("null"))})combo->addItem(pair.first,pair.second);return combo;}
    void setEditorData(QWidget* editor,const QModelIndex& i)const override{if(auto* combo=qobject_cast<QComboBox*>(editor)){combo->setCurrentIndex(combo->findData(i.data(Qt::EditRole)));return;}QStyledItemDelegate::setEditorData(editor,i);}
    void setModelData(QWidget* editor,QAbstractItemModel* model,const QModelIndex& i)const override{if(auto* combo=qobject_cast<QComboBox*>(editor)){model->setData(i,combo->currentData(),Qt::EditRole);return;}QStyledItemDelegate::setModelData(editor,model,i);}
};
TableTaskDialog::TableTaskDialog(const navidog::ConnectionProfile& p,navidog::TableTask task,navidog::QueryService& service,QWidget* parent)
    :Dialog(parent),profile_(p),original_(task),service_(service) {
    setObjectName("tableTaskDialog");setWindowTitle(navidog::tableActionName(task.action));resize(680,580);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,20,20,16);layout->setSpacing(12);
    auto* context=new QLabel(p.name+" / "+task.node.database+(task.node.schema.isEmpty()?QString{}:" / "+task.node.schema));context->setWordWrap(true);context->setTextFormat(Qt::PlainText);context->setObjectName("muted");layout->addWidget(context);
    fields_=new QWidget;auto* form=new QFormLayout(fields_);form->setContentsMargins(0,0,0,0);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);layout->addWidget(fields_);
    table_=new navidog::ui::Select;table_->setObjectName("taskTable");table_->setEditable(true);table_->addItem(task.node.object);table_->setEnabled(task.node.kind!="table");form->addRow("表：",table_);
    target_=new navidog::ui::TextInput(fields_);target_->setObjectName("taskTargetName");
    if(task.action==A::Rename || task.action==A::CopyData || task.action==A::CopyStructure){target_->setText(task.targetName.isEmpty()?(task.action==A::Rename?task.node.object:task.node.object+"_copy"):task.targetName);form->addRow("新表名：",target_);}else target_->hide();
    file_=new navidog::ui::TextInput(fields_);file_->setObjectName("taskFile");file_->setText(task.path);
    if(fileAction(task.action)) {
        auto* pathRow=new QWidget;auto* line=new QHBoxLayout(pathRow);line->setContentsMargins(0,0,0,0);line->addWidget(file_);auto* browse=new navidog::ui::PushButton("选择…");browse->setObjectName("taskBrowse");line->addWidget(browse);form->addRow(task.action==A::ImportCsv?"来源文件：":"保存文件：",pathRow);
        connect(browse, &QPushButton::clicked, this, [this] {
            const auto action = original_.action;
            const auto filter = action == A::Dictionary ? "Markdown (*.md)"
                : action == A::ExportSql ? "SQL (*.sql)" : "UTF-8 CSV (*.csv)";
            const auto purpose = navidog::tableActionName(action) + "-" + table_->currentText();
            const auto context = navidog::ui::exportContext(profile_, original_.node.database,
                                                              original_.node.schema);
            const auto path = action == A::ImportCsv
                ? QFileDialog::getOpenFileName(this, "导入 CSV", {}, filter)
                : navidog::ui::chooseExportFile(this, "保存文件", context, purpose, filter);
            if (!path.isEmpty()) file_->setText(path);
        });
    }else file_->hide();
    dimension_=new navidog::ui::Select(fields_);dimension_->setObjectName("taskDimension");dimension_->setEditable(true);if(task.action==A::BuildBI)form->addRow("分组字段：",dimension_);else dimension_->hide();
    grantee_=new navidog::ui::TextInput(fields_);grantee_->setObjectName("taskGrantee");granteeHost_=new navidog::ui::TextInput("%",fields_);granteeHost_->setObjectName("taskGranteeHost");revoke_=new navidog::ui::CheckBox("撤销所选权限",fields_);
    if(task.action==A::GrantPrivileges){form->addRow("用户 / 角色：",grantee_);if(navidog::isMySqlFamily(p.driver))form->addRow("账号主机：",granteeHost_);else granteeHost_->hide();auto* choices=new QWidget;auto* line=new QHBoxLayout(choices);line->setContentsMargins(0,0,0,0);for(const auto& priv:{"SELECT","INSERT","UPDATE","DELETE","REFERENCES"}){auto* check=new navidog::ui::CheckBox(priv);privileges_<<check;line->addWidget(check);connect(check,&QCheckBox::toggled,this,&TableTaskDialog::updatePreview);}form->addRow("权限：",choices);form->addRow({},revoke_);}else {grantee_->hide();granteeHost_->hide();revoke_->hide();}
    rowLimit_=new navidog::ui::NumberInput(fields_);rowLimit_->setObjectName("taskRowLimit");rowLimit_->setRange(1,1000000);rowLimit_->setValue(task.maxRows);if(task.action==A::ImportCsv || task.action==A::GenerateData)form->addRow(task.action==A::GenerateData?"生成行数：":"最大导入行数：",rowLimit_);else rowLimit_->hide();
    data_=new navidog::ui::CheckBox("包含表数据",fields_);data_->setObjectName("taskWithData");data_->setChecked(task.withData);if(task.action==A::ExportSql)form->addRow({},data_);else data_->hide();
    confirmation_=new navidog::ui::TextInput(fields_);confirmation_->setObjectName("taskConfirmName");if(dangerous(task.action)){confirmation_->setPlaceholderText("输入完整表名确认");form->addRow("确认表名：",confirmation_);}else confirmation_->hide();
    QString note;
    if(task.action==A::DeleteRows)note="执行 DELETE，保留表结构；会触发相关 DELETE 触发器，外键约束由数据库检查。";
    if(task.action==A::Drop)note="删除表及其中的数据；不使用 CASCADE。请先处理此表未保存的编辑草稿，并自行保留需要的数据备份。";
    if(task.action==A::Truncate)note="清除全部记录并重置自增计数。SQLite 使用 DELETE 与计数重置；其他引擎执行 TRUNCATE，事务和触发器行为由数据库决定。";
    if(task.action==A::CopyData || task.action==A::CopyStructure)note=p.driver==navidog::DatabaseKind::SQLite?"保留原始建表定义及索引，外键仍指向原引用对象；不复制触发器。":"按数据库 LIKE 语义复制字段与索引；不复制外键、触发器或权限。PostgreSQL 的 serial 默认值可能仍引用原序列。";
    if(task.action==A::ImportCsv)note="UTF-8 CSV，首行为目标字段名，按名称匹配并追加导入；移除生成字段等不可写列。未加引号的 \\N 表示 NULL，二进制字段使用 base64: 编码。重复键或格式错误时回滚整批；MySQL 要求 InnoDB。";
    if(task.action==A::ExportCsv)note="导出整张表，UTF-8 CSV；NULL 为未加引号的 \\N，二进制为 base64: 编码。超过单元格或页内存上限时停止，保留原输出文件。";
    if(task.action==A::ExportSql)note=p.driver==navidog::DatabaseKind::PostgreSQL?"使用 PostgreSQL 官方 pg_dump；请使用与服务器主版本匹配的客户端。按工具规则导出所选表，不导出角色和权限；依赖的外部对象需另行准备。":"导出表结构及可选数据；不包含触发器、权限与依赖对象。请在正确的数据库 / Schema 中执行脚本。";
    if(task.action==A::Optimize && p.driver==navidog::DatabaseKind::SQLite)note="SQLite 的 VACUUM 会整理整个数据库文件；请确认有足够磁盘空间。";
    if(task.action==A::Repair)note="数据库引擎可能仅对特定存储引擎支持修复；执行前请备份。维修选项："+task.option;
    if(task.action==A::GrantPrivileges)note="显示账号可见的显式表权限；角色或数据库级继承权限可能不在列表中。仅对已存在的用户 / 角色授予或撤销所选权限。";
    if(task.action==A::GenerateData)note="逐字段设置规则；数据库默认值会省略该列。外键、唯一性和检查约束由数据库校验，失败时回滚整批。请在测试环境使用生成数据。";
    if(task.action==A::BuildBI)note="按选定字段统计记录数，显示最多 20 个分组；此版提供本地分布图与数据表。";
    if(!note.isEmpty()){auto* label=new QLabel(note);label->setWordWrap(true);label->setTextFormat(Qt::PlainText);layout->addWidget(label);}
    if(task.action==A::GenerateData){auto* grid=new QTableView;grid->setObjectName("generationRules");rules_=new GenerationRuleModel(grid);grid->setModel(rules_);grid->setItemDelegate(new GenerationRuleDelegate(grid));grid->horizontalHeader()->setStretchLastSection(true);layout->addWidget(grid,1);connect(rules_,&QAbstractItemModel::dataChanged,this,[this]{updatePreview();});}
    auto* sqlPreview=new SqlReadOnlyView(this);sqlPreview->setDatabaseKind(p.driver);preview_=sqlPreview;preview_->setObjectName("taskSqlPreview");preview_->setMaximumHeight(120);layout->addWidget(preview_->withActions());
    result_=new QTableView;result_->setObjectName("taskResult");model_=new ResultModel(result_);result_->setModel(model_);result_->setEditTriggers(QAbstractItemView::NoEditTriggers);result_->horizontalHeader()->setStretchLastSection(true);result_->hide();layout->addWidget(result_,1);
    message_=new QLabel;message_->setObjectName("taskMessage");message_->setTextFormat(Qt::PlainText);message_->setWordWrap(true);layout->addWidget(message_);layout->addStretch();
    buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons_->button(QDialogButtonBox::Ok)->setText("执行");buttons_->button(QDialogButtonBox::Ok)->setProperty("primary",true);buttons_->button(QDialogButtonBox::Cancel)->setText("取消");layout->addWidget(buttons_);
    connect(grantee_,&QLineEdit::textChanged,this,&TableTaskDialog::updatePreview);connect(granteeHost_,&QLineEdit::textChanged,this,&TableTaskDialog::updatePreview);connect(revoke_,&QCheckBox::toggled,this,&TableTaskDialog::updatePreview);connect(dimension_,&QComboBox::currentTextChanged,this,&TableTaskDialog::updatePreview);
    connect(buttons_,&QDialogButtonBox::accepted,this,&TableTaskDialog::accept);connect(buttons_,&QDialogButtonBox::rejected,this,&TableTaskDialog::reject);
    for(auto* input:{target_,file_,confirmation_})connect(input,&QLineEdit::textChanged,this,&TableTaskDialog::updatePreview);connect(table_,&QComboBox::currentTextChanged,this,&TableTaskDialog::updatePreview);connect(data_,&QCheckBox::toggled,this,&TableTaskDialog::updatePreview);
    connect(&service_,&navidog::QueryService::tableTaskCompleted,this,[this](const QString& id,const navidog::TableTaskResult& outcome){
        if(id==metadataId_){loading_=false;message_->setText(outcome.error);if(outcome.error.isEmpty()){model_->setResult(outcome.result);result_->show();}updatePreview();return;}
        if(id!=requestId_)return;if(auto* grid=findChild<QTableView*>("generationRules"))grid->setEnabled(true);running_=false;outcome_=outcome;fields_->setEnabled(true);buttons_->button(QDialogButtonBox::Cancel)->setText("关闭");
        if(!outcome.result.columns.isEmpty()){model_->setResult(outcome.result);result_->show();result_->resizeColumnsToContents();}
        if(!outcome.error.isEmpty()) {message_->setText(outcome.error+(outcome.rolledBack?"\n本批修改已回滚。":"")+(outcome.partial?"\n数据库可能已发生部分变更，请刷新核对。":"")+(outcome.commitUnknown?"\n提交结果未确认，请核对数据库后再操作。":""));updatePreview();QMessageBox::warning(this,"操作失败",message_->text());return;}
        succeeded_=outcome.applied;if(auto* grid=findChild<QTableView*>("generationRules"))grid->setEnabled(false);fields_->setEnabled(false);buttons_->button(QDialogButtonBox::Ok)->setText("完成");message_->setText(outcome.report);
        if(!outcome.result.columns.isEmpty()){model_->setResult(outcome.result);result_->show();result_->resizeColumnsToContents();}
        if(original_.action==A::Rename || original_.action==A::Drop || original_.action==A::CopyData || original_.action==A::CopyStructure || original_.action==A::ReverseModel || original_.action==A::BuildBI)QDialog::accept();
    });
    if(task.node.kind!="table") {
        loading_=true;catalogId_=QUuid::createUuid().toString();
        connect(&service_,&navidog::QueryService::catalogCompleted,this,[this](const QString& id,const navidog::CatalogResult& result){if(id!=catalogId_)return;loading_=false;for(auto n:result.nodes)if(n.kind=="table")table_->addItem(n.object);message_->setText(result.error);updatePreview();});
        QTimer::singleShot(0,this,[this]{auto folder=original_.node;folder.kind="tables";if(!service_.catalog(profile_,folder,catalogId_,true)){loading_=false;message_->setText("数据库正在处理其他请求，可手动输入表名。");updatePreview();}});
    }
    if(task.action==A::GenerateData || task.action==A::BuildBI){
        metadataReady_=false;connect(&service_,&navidog::QueryService::tableDesignCompleted,this,[this](const QString& id,const navidog::TableDesignResult& result){if(id!=metadataId_)return;loading_=false;table_->setEnabled(original_.node.kind!="table");metadataReady_=result.error.isEmpty() && !result.definition.columns.isEmpty();message_->setText(result.error);if(metadataReady_){if(rules_)rules_->load(result.definition);dimension_->clear();for(auto c:result.definition.columns)dimension_->addItem(c.name);}updatePreview();});
        auto* retry=new navidog::ui::PushButton("重新读取字段");retry->setObjectName("taskMetadataRetry");layout->insertWidget(2,retry);connect(retry,&QPushButton::clicked,this,&TableTaskDialog::loadTableMetadata);
        if(original_.node.kind=="table")QTimer::singleShot(0,this,&TableTaskDialog::loadTableMetadata);
        else {connect(table_,QOverload<int>::of(&QComboBox::activated),this,[this]{loadTableMetadata();});connect(table_->lineEdit(),&QLineEdit::returnPressed,this,&TableTaskDialog::loadTableMetadata);}
    }else if(task.action==A::GrantPrivileges){loading_=true;metadataId_=QUuid::createUuid().toString();QTimer::singleShot(0,this,[this]{auto view=original_;view.action=A::TablePrivileges;if(!service_.tableTask(profile_,view,metadataId_)){loading_=false;updatePreview();}});}
    updatePreview();
}
navidog::TableTask TableTaskDialog::task() const {auto t=original_;t.node.kind="table";t.node.name=t.node.object=table_->currentText();t.targetName=target_->text();t.path=file_->text();t.withData=data_->isChecked();t.maxRows=rowLimit_->value();t.dimension=dimension_->currentText();t.grantee=grantee_->text();t.granteeHost=granteeHost_->text();t.revoke=revoke_->isChecked();for(auto* check:privileges_)if(check->isChecked())t.privileges<<check->text();if(rules_)t.generationRules=rules_->rules();return t;}
void TableTaskDialog::updatePreview() {
    bool valid=metadataReady_;auto t=task();if((t.action==A::GenerateData || t.action==A::BuildBI) && metadataTable_!=t.node.object)valid=false;
    try{auto sql=navidog::tableTaskSql(profile_,t);preview_->setSourceSql(sql.isEmpty()?fileAction(t.action)?"-- 根据所选表的字段及原始结构处理文件。":t.action==A::GenerateData?"-- 按上方字段规则生成并在独立事务中提交。":t.action==A::ReverseModel?"-- 读取真实字段、索引与外键，生成只读模型。":"-- 读取原始表定义后在独立连接中复制。":sql+";");if(fileAction(t.action) && t.path.isEmpty())valid=false;if(dangerous(t.action) && confirmation_->text()!=t.node.object)valid=false;}
    catch(const std::exception& e){preview_->setSourceSql("-- "+QString::fromUtf8(e.what()));valid=false;}
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid && !running_ && !loading_);
}
void TableTaskDialog::loadTableMetadata() {
    if(running_ || loading_ || service_.busy() || table_->currentText().isEmpty())return;
    metadataId_=QUuid::createUuid().toString();auto current=task();metadataTable_=current.node.object;
    if(service_.tableDefinition(profile_,current.node,metadataId_)){loading_=true;metadataReady_=false;table_->setEnabled(false);message_->setText("正在读取字段…");updatePreview();}
}
void TableTaskDialog::accept() {
    if(succeeded_){QDialog::accept();return;}if(running_ || loading_)return;
    if(dangerous(original_.action) && confirmation_->text()!=task().node.object)return;
    buttons_->button(QDialogButtonBox::Ok)->setFocus();QApplication::sendPostedEvents();
    requestId_=QUuid::createUuid().toString();if(service_.tableTask(profile_,task(),requestId_)){running_=true;if(auto* grid=findChild<QTableView*>("generationRules"))grid->setEnabled(false);fields_->setEnabled(false);buttons_->button(QDialogButtonBox::Cancel)->setText("停止");message_->setText("正在执行，请稍候…");updatePreview();}
}
void TableTaskDialog::reject(){if(running_){service_.cancel();message_->setText("正在停止并核对事务结果…");return;}if(loading_)service_.cancel();Dialog::reject();}
