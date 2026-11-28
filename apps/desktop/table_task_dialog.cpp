#include "export_paths.hpp"
#include "table_task_dialog.hpp"
#include "task_center.hpp"
#include "editor.hpp"
#include "result_model.hpp"
#include "navidog/generation_configuration.hpp"
#include "navidog/local_store.hpp"
#include <QFileDialog>
#include <QApplication>
#include <QLocale>
#include <QStyledItemDelegate>
#include <QAbstractTableModel>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QFileInfo>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <QPlainTextEdit>
#include <QTableView>
#include <QTimer>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>

namespace {using A=navidog::TableAction;bool dangerous(A a){return a==A::Drop || a==A::DeleteRows || a==A::Truncate;}bool fileAction(A a){return a==A::ImportCsv || a==A::ExportCsv || a==A::ExportSql || a==A::Dictionary;}}
class GenerationRuleModel final : public QAbstractTableModel {
    struct Entry {
        navidog::TableColumn column;
        QString kind, value, distribution, reference;
        bool readOnly = false;
    };
    QVector<Entry> entries_;
    QJsonObject suggested_;
    const QStringList keys_ = navidog::generationRuleKeys();
    const QStringList labels_ = navidog::generationRuleLabels();
public:
    explicit GenerationRuleModel(QObject* parent) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : int(entries_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 5;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
        return QStringList{"字段", "类型", "生成规则", "规则参数", "分布形状 / 小时权重"}.value(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) return {};
        const auto& field = entries_[index.row()];
        if (role == Qt::ToolTipRole && !field.reference.isEmpty()) return field.reference;
        if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
        switch (index.column()) {
        case 0: return role == Qt::DisplayRole && !field.reference.isEmpty()
                       ? field.column.name + "  ↗" : field.column.name;
        case 1: return field.column.type;
        case 2: return role == Qt::EditRole ? field.kind : labels_.value(keys_.indexOf(field.kind));
        case 3: return field.kind == "foreign" ? (role == Qt::EditRole ? QString{} : field.reference) : field.value;
        case 4: return role == Qt::DisplayRole && field.distribution.isEmpty() &&
                       (field.kind == "number" || field.kind == "temporal")
                       ? QString("均匀") : field.distribution;
        }
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        auto flags = QAbstractTableModel::flags(index);
        if (!index.isValid()) return flags;
        const auto& field = entries_[index.row()];
        if (field.readOnly) return flags;
        if (index.column() == 2 ||
            (index.column() == 3 && field.kind != "foreign" && field.kind != "default" && field.kind != "null") ||
            (index.column() == 4 && (field.kind == "number" || field.kind == "temporal")))
            flags |= Qt::ItemIsEditable;
        return flags;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable)) return false;
        auto& field = entries_[index.row()];
        if (index.column() == 2) {
            if (!keys_.contains(value.toString())) return false;
            if (field.kind == value.toString()) return true;
            field.kind = value.toString();
            field.value = navidog::generationRuleExample(field.kind, field.column);
            field.distribution.clear();
            emit dataChanged(this->index(index.row(), 2), this->index(index.row(), 4));
            return true;
        }
        if (index.column() == 3) field.value = value.toString();
        else if (index.column() == 4) field.distribution = value.toString();
        else return false;
        emit dataChanged(index, index);
        return true;
    }
    void load(const navidog::TableDefinition& table, navidog::DatabaseKind driver) {
        suggested_ = navidog::suggestedGenerationRules(table, driver);
        beginResetModel();
        entries_.clear();
        for (const auto& column : table.columns) {
            const auto rule = suggested_.value(column.name).toObject();
            Entry entry;
            entry.column = column;
            entry.kind = rule.value("kind").toString();
            entry.value = rule.value("value").toString();
            entry.readOnly = column.autoIncrement || !column.generation.isEmpty() ||
                             !column.generatedExpression.isEmpty() || column.protectedReason.contains("标识") ||
                             column.protectedReason.contains("生成") || column.protectedReason.contains("计算");
            for (const auto& constraint : table.objects) {
                if (constraint.kind != navidog::TableObjectKind::ForeignKey ||
                    (!constraint.enabled && !navidog::usesNativePostgres(driver))) continue;
                const int position = constraint.columns.indexOf(column.name);
                if (position < 0) continue;
                const auto target = (constraint.referenceSchema.isEmpty() ? QString{} : constraint.referenceSchema + ".") +
                                    constraint.referenceTable;
                const auto referenced = constraint.referenceColumns.value(position);
                if (!entry.reference.isEmpty()) entry.reference += " / ";
                entry.reference += target + (referenced.isEmpty() ? QString{} : "." + referenced);
            }
            entries_ << std::move(entry);
        }
        endResetModel();
    }
    QJsonObject rules(bool editableOnly = false) const {
        QJsonObject result;
        for (const auto& field : entries_) {
            if (editableOnly && field.readOnly) continue;
            result.insert(field.column.name, QJsonObject{{"kind", field.kind}, {"value", field.value},
                                                         {"distribution", field.distribution}});
        }
        return result;
    }
    int applyConfiguration(const QJsonObject& configuration) {
        const auto compatibleRule = [&](const Entry& field) {
            auto rule = configuration.value(field.column.name).toObject();
            if (rule.value("kind") == "foreign" && field.reference.isEmpty()) rule = {};
            return rule;
        };
        int matched = 0;
        for (const auto& field : entries_)
            if (!field.readOnly && !compatibleRule(field).isEmpty()) ++matched;
        if (!configuration.isEmpty() && !matched)
            throw std::runtime_error("此配置没有可以应用到当前表的字段规则");
        for (auto& field : entries_) {
            if (field.readOnly) continue;
            auto rule = compatibleRule(field);
            if (rule.isEmpty()) rule = suggested_.value(field.column.name).toObject();
            field.kind = rule.value("kind").toString();
            field.value = rule.value("value").toString();
            field.distribution = rule.value("distribution").toString();
        }
        if (!entries_.isEmpty()) emit dataChanged(index(0, 2), index(rowCount() - 1, 4));
        return matched;
    }
};

class GenerationRuleDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
        if (index.column() == 3) return new navidog::ui::TextInput(parent);
        auto* select = new navidog::ui::Select(parent);
        if (index.column() == 4) {
            select->setEditable(true);
            select->addItems({"均匀", "偏低", "偏高", "钟形"});
            return select;
        }
        const auto keys = navidog::generationRuleKeys();
        const auto labels = navidog::generationRuleLabels();
        const bool foreign = !index.data(Qt::ToolTipRole).toString().isEmpty();
        for (int i = 0; i < keys.size(); ++i)
            if (keys[i] != "foreign" || foreign) select->addItem(labels[i], keys[i]);
        return select;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (auto* select = qobject_cast<QComboBox*>(editor)) {
            if (index.column() == 4) {
                const auto text = index.data(Qt::EditRole).toString();
                select->setEditText(text.isEmpty() ? QString("均匀") : text);
            } else select->setCurrentIndex(select->findData(index.data(Qt::EditRole)));
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        if (auto* select = qobject_cast<QComboBox*>(editor)) {
            model->setData(index, index.column() == 4 ? QVariant(select->currentText()) : select->currentData(),
                           Qt::EditRole);
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }
};

TableTaskDialog::TableTaskDialog(const navidog::ConnectionProfile& p,navidog::TableTask task,
                                 navidog::QueryService& service,QWidget* parent,QString configurationStorePath)
    :Dialog(parent),profile_(p),original_(task),service_(service),
      configurationStorePath_(std::move(configurationStorePath)) {
    if (task.action == A::GenerateData) profile_.advanced.initialSql.clear();
    if (navidog::tableActionRunsInBackground(task.action)) tasks_ = navidog::ui::backgroundTasksFor(parent);
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
    rowLimit_=new navidog::ui::NumberInput(fields_);rowLimit_->setObjectName("taskRowLimit");rowLimit_->setRange(1,1000000);rowLimit_->setValue(task.maxRows);if(task.action==A::ImportCsv)form->addRow("最大导入行数：",rowLimit_);else rowLimit_->hide();
    if (task.action == A::GenerateData) {
        resize(940, 690);
        addGenerationConfigurationControls(*form);
        generationMode_ = new navidog::ui::Select(fields_);
        generationMode_->setObjectName("generationSizeMode");
        generationMode_->addItems({"按行数", "按估算容量"});
        generationMode_->setCurrentIndex(task.generation.sizeMode == navidog::GenerationSizeMode::Capacity ? 1 : 0);
        generationSize_ = new navidog::ui::TextInput(fields_);
        generationSize_->setObjectName("generationSize");
        generationSize_->setMaxLength(60);
        generationSize_->setText(task.generation.size.isEmpty() ? QString::number(task.maxRows) : task.generation.size);
        generationSize_->setPlaceholderText(generationMode_->currentIndex() == 1
            ? "例如 20 GB、1.5 TiB" : "例如 100,000,000,000,000");
        generationSeed_ = new navidog::ui::TextInput(QString::number(task.generation.seed), fields_);
        generationSeed_->setObjectName("generationSeed");
        generationSeed_->setMaxLength(20);
        generationSeed_->setToolTip("相同规则、种子和父键候选集可复现随机值。自动递增从目标表当前最大值接续。");
        auto* sizeRow = new QWidget(fields_);
        auto* sizeLayout = new QHBoxLayout(sizeRow);
        sizeLayout->setContentsMargins(0, 0, 0, 0);
        sizeLayout->addWidget(generationMode_);
        sizeLayout->addWidget(generationSize_, 1);
        sizeLayout->addWidget(new QLabel("随机种子"));
        sizeLayout->addWidget(generationSeed_);
        form->addRow("生成规模", sizeRow);
        connect(generationMode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            const bool capacity = generationMode_->currentIndex() == 1;
            generationSize_->setPlaceholderText(capacity ? "例如 20 GB、1.5 TiB" : "例如 100,000,000,000,000");
            generationSize_->setText(capacity ? "1 GiB" : QString::number(original_.maxRows));
        });
        connect(generationSize_, &QLineEdit::textChanged, this, &TableTaskDialog::updatePreview);
        connect(generationSeed_, &QLineEdit::textChanged, this, &TableTaskDialog::updatePreview);
    }
    data_=new navidog::ui::CheckBox("包含表数据",fields_);data_->setObjectName("taskWithData");data_->setChecked(task.withData);if(task.action==A::ExportSql)form->addRow({},data_);else data_->hide();
    confirmation_=new navidog::ui::TextInput(fields_);confirmation_->setObjectName("taskConfirmName");if(dangerous(task.action)){confirmation_->setPlaceholderText("输入完整表名确认");form->addRow("确认表名：",confirmation_);}else confirmation_->hide();
    QString note;
    if(task.action==A::DeleteRows)note="执行 DELETE，保留表结构；会触发相关 DELETE 触发器，外键约束由数据库检查。";
    if(task.action==A::Drop)note=task.cascade ?
        (p.driver==navidog::DatabaseKind::Oracle ?
            "将删除表、数据及引用这张表的外键约束。" :
            "将删除表和数据。数据库也会删除依赖视图、外键等依赖对象，范围可能跨 Schema。") :
        "将删除表及其中的数据。数据库会按普通删除规则检查依赖。";
    if(task.action==A::Truncate)note="清除全部记录并重置自增计数。SQLite 使用 DELETE 与计数重置；其他引擎执行 TRUNCATE，事务和触发器行为由数据库决定。";
    if(task.action==A::CopyData || task.action==A::CopyStructure)note=p.driver==navidog::DatabaseKind::SQLite?"保留原始建表定义及索引，外键仍指向原引用对象；不复制触发器。":"按数据库 LIKE 语义复制字段与索引；不复制外键、触发器或权限。PostgreSQL 的 serial 默认值可能仍引用原序列。";
    if(task.action==A::ImportCsv)note="UTF-8 CSV，首行为目标字段名，按名称匹配并追加导入；移除生成字段等不可写列。未加引号的 \\N 表示 NULL，二进制字段使用 base64: 编码。重复键或格式错误时回滚整批；MySQL 要求 InnoDB。";
    if(task.action==A::ExportCsv)note="导出整张表，UTF-8 CSV；NULL 为未加引号的 \\N，二进制为 base64: 编码。超过单元格或页内存上限时停止，保留原输出文件。";
    if(task.action==A::ExportSql)note=p.driver==navidog::DatabaseKind::PostgreSQL?"使用 PostgreSQL 官方 pg_dump；请使用与服务器主版本匹配的客户端。按工具规则导出所选表，不导出角色和权限；依赖的外部对象需另行准备。":"导出表结构及可选数据；不包含触发器、权限与依赖对象。请在正确的数据库 / Schema 中执行脚本。";
    if(task.action==A::Optimize && p.driver==navidog::DatabaseKind::SQLite)note="SQLite 的 VACUUM 会整理整个数据库文件；请确认有足够磁盘空间。";
    if(task.action==A::Repair)note="数据库引擎可能仅对特定存储引擎支持修复；执行前请备份。维修选项："+task.option;
    if(task.action==A::GrantPrivileges)note="显示账号可见的显式表权限；角色或数据库级继承权限可能不在列表中。仅对已存在的用户 / 角色授予或撤销所选权限。";
    if (task.action == A::GenerateData)
        note = "双击单元格配置规则。外键从已有父键采样，复合外键使用同一组值。可先预览样本和容量，写入时由数据库校验完整约束。";
    if(task.action==A::BuildBI)note="按选定字段统计记录数，显示最多 20 个分组；此版提供本地分布图与数据表。";
    if(!note.isEmpty()){auto* label=new QLabel(note);label->setWordWrap(true);label->setTextFormat(Qt::PlainText);layout->addWidget(label);}
    if (task.action == A::GenerateData) {
        auto* grid = new navidog::ui::ObjectGrid(this);
        grid->setObjectName("generationRules");
        rules_ = new GenerationRuleModel(grid);
        grid->setModel(rules_);
        grid->setItemDelegate(new GenerationRuleDelegate(grid));
        grid->setSelectionMode(QAbstractItemView::SingleSelection);
        grid->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        grid->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        grid->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        grid->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        grid->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
        grid->setColumnWidth(4, 145);
        layout->addWidget(grid, 2);
        ruleHelp_ = new QLabel;
        ruleHelp_->setTextFormat(Qt::PlainText);
        ruleHelp_->setWordWrap(true);
        ruleHelp_->setObjectName("muted");
        layout->addWidget(ruleHelp_);
        connect(grid->selectionModel(), &QItemSelectionModel::currentChanged, this, &TableTaskDialog::updateRuleHelp);
        connect(rules_, &QAbstractItemModel::dataChanged, this, [this] { updateRuleHelp(); updatePreview(); });
        updateRuleHelp();
    }
    auto* sqlPreview=new SqlReadOnlyView(this);sqlPreview->setDatabaseKind(p.driver);preview_=sqlPreview;preview_->setObjectName("taskSqlPreview");preview_->setMaximumHeight(120);
    auto* sqlActions = preview_->withActions();
    if (rules_) sqlActions->hide();
    else layout->addWidget(sqlActions);
    result_=new QTableView;result_->setObjectName("taskResult");model_=new ResultModel(result_);result_->setModel(model_);result_->setEditTriggers(QAbstractItemView::NoEditTriggers);result_->horizontalHeader()->setStretchLastSection(true);result_->hide();layout->addWidget(result_,1);
    message_=new QLabel;message_->setObjectName("taskMessage");message_->setTextFormat(Qt::PlainText);message_->setWordWrap(true);layout->addWidget(message_);layout->addStretch();
    buttons_=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons_->button(QDialogButtonBox::Ok)->setText("执行");buttons_->button(QDialogButtonBox::Ok)->setProperty("primary",true);buttons_->button(QDialogButtonBox::Cancel)->setText("取消");layout->addWidget(buttons_);
    if (tasks_ || rules_) {
        taskProgress_ = new QProgressBar(this);
        taskProgress_->setObjectName("tableTaskProgress");
        taskProgress_->hide();
        layout->insertWidget(layout->indexOf(message_), taskProgress_);
    }
    if (tasks_) {
        stop_ = new navidog::ui::PushButton("取消任务", this);
        stop_->setObjectName("stopTableBackgroundTask");
        buttons_->addButton(stop_, QDialogButtonBox::ActionRole);
        stop_->hide();
        connect(stop_, &QPushButton::clicked, this, [this] { tasks_->cancel(requestId_); });
        connect(tasks_, &navidog::BackgroundTasks::changed, this, [this](const QString& id) {
            if (!backgroundRequest_ || id != requestId_) return;
            if (const auto* info = tasks_->info(id); info && info->active()) showProgress(*info);
        });
        connect(tasks_, &navidog::BackgroundTasks::finished, this,
                [this](const QString& id, const navidog::TaskResult& result) {
            if (!backgroundRequest_ || id != requestId_) return;
            const auto request = std::exchange(backgroundRequest_, {}).value();
            auto outcome = result.payload.value<navidog::TableTaskResult>();
            if (!result.payload.isValid()) {
                outcome.error = result.error.isEmpty() ? result.detail : result.error;
                outcome.cancelled = result.cancelled;
            }
            emit backgroundCompleted(request, outcome);
            finishRequest(outcome);
        });
    }
    if (rules_) {
        buttons_->button(QDialogButtonBox::Ok)->setText("生成并写入");
        estimate_ = new navidog::ui::PushButton("预览与估算", this);
        result_->horizontalHeader()->setResizeContentsPrecision(12);
        estimate_->setObjectName("estimateGeneration");
        buttons_->addButton(estimate_, QDialogButtonBox::ActionRole);
        connect(estimate_, &QPushButton::clicked, this, [this] { startTask(true); });
        connect(&service_, &navidog::QueryService::tableTaskProgress, this,
                [this](const QString& id, qint64 done, qint64 total) {
            if (running_ && !backgroundRequest_ && id == requestId_ &&
                buttons_->button(QDialogButtonBox::Cancel)->isEnabled()) {
                navidog::TaskInfo info;
                info.state = navidog::TaskState::Running;
                info.progress = {done, total, navidog::TaskUnit::Rows, "正在生成并写入"};
                showProgress(info);
            }
        });
    }
    connect(grantee_,&QLineEdit::textChanged,this,&TableTaskDialog::updatePreview);connect(granteeHost_,&QLineEdit::textChanged,this,&TableTaskDialog::updatePreview);connect(revoke_,&QCheckBox::toggled,this,&TableTaskDialog::updatePreview);connect(dimension_,&QComboBox::currentTextChanged,this,&TableTaskDialog::updatePreview);
    connect(buttons_,&QDialogButtonBox::accepted,this,&TableTaskDialog::accept);connect(buttons_,&QDialogButtonBox::rejected,this,&TableTaskDialog::reject);
    for(auto* input:{target_,file_,confirmation_})connect(input,&QLineEdit::textChanged,this,&TableTaskDialog::updatePreview);connect(table_,&QComboBox::currentTextChanged,this,&TableTaskDialog::updatePreview);connect(data_,&QCheckBox::toggled,this,&TableTaskDialog::updatePreview);
    connect(&service_,&navidog::QueryService::tableTaskCompleted,this,[this](const QString& id,const navidog::TableTaskResult& outcome){
        if(id==metadataId_){loading_=false;message_->setText(outcome.error);if(outcome.error.isEmpty()){model_->setResult(outcome.result);result_->show();}updatePreview();return;}
        if (id == requestId_) finishRequest(outcome);
    });
    if(task.node.kind!="table") {
        loading_=true;catalogId_=QUuid::createUuid().toString();
        connect(&service_,&navidog::QueryService::catalogCompleted,this,[this](const QString& id,const navidog::CatalogResult& result){if(id!=catalogId_)return;loading_=false;for(auto n:result.nodes)if(n.kind=="table")table_->addItem(n.object);message_->setText(result.error);updatePreview();});
        QTimer::singleShot(0,this,[this]{auto folder=original_.node;folder.kind="tables";if(!service_.catalog(profile_,folder,catalogId_,true)){loading_=false;message_->setText("数据库正在处理其他请求，可手动输入表名。");updatePreview();}});
    }
    if(task.action==A::GenerateData || task.action==A::BuildBI){
        metadataReady_=false;connect(&service_,&navidog::QueryService::tableDesignCompleted,this,[this](const QString& id,const navidog::TableDesignResult& result){if(id!=metadataId_)return;loading_=false;table_->setEnabled(original_.node.kind!="table");metadataReady_=result.error.isEmpty() && !result.definition.columns.isEmpty();message_->setText(result.error);if(metadataReady_){if (rules_) { rules_->load(result.definition, profile_.driver); updateRuleHelp(); }dimension_->clear();for(auto c:result.definition.columns)dimension_->addItem(c.name);}updatePreview();});
        auto* retry=new navidog::ui::PushButton("重新读取字段");retry->setObjectName("taskMetadataRetry");layout->insertWidget(2,retry);connect(retry,&QPushButton::clicked,this,&TableTaskDialog::loadTableMetadata);
        if(original_.node.kind=="table")QTimer::singleShot(0,this,&TableTaskDialog::loadTableMetadata);
        else {connect(table_,QOverload<int>::of(&QComboBox::activated),this,[this]{loadTableMetadata();});connect(table_->lineEdit(),&QLineEdit::returnPressed,this,&TableTaskDialog::loadTableMetadata);}
    }else if(task.action==A::GrantPrivileges){loading_=true;metadataId_=QUuid::createUuid().toString();QTimer::singleShot(0,this,[this]{auto view=original_;view.action=A::TablePrivileges;if(!service_.tableTask(profile_,view,metadataId_)){loading_=false;updatePreview();}});}
    updatePreview();
}
navidog::TableTask TableTaskDialog::task() const {auto t=original_;t.node.kind="table";t.node.name=t.node.object=table_->currentText();t.targetName=target_->text();t.path=file_->text();t.withData=data_->isChecked();t.maxRows=rowLimit_->value();t.dimension=dimension_->currentText();t.grantee=grantee_->text();t.granteeHost=granteeHost_->text();t.revoke=revoke_->isChecked();for(auto* check:privileges_)if(check->isChecked())t.privileges<<check->text();if (rules_) {
        t.generationRules = rules_->rules();
        t.generation.size = generationSize_->text();
        t.generation.sizeMode = generationMode_->currentIndex() == 1
            ? navidog::GenerationSizeMode::Capacity : navidog::GenerationSizeMode::Rows;
        t.generation.seed = generationSeed_->text().toULongLong();
    }
    return t;}
void TableTaskDialog::updatePreview() {
    bool valid=metadataReady_;auto t=task();if((t.action==A::GenerateData || t.action==A::BuildBI) && metadataTable_!=t.node.object)valid=false;
    if (rules_) {
        bool seedValid = false;
        generationSeed_->text().toULongLong(&seedValid);
        const bool canConfigure = valid && !running_ && !loading_ && !succeeded_;
        const bool ready = canConfigure && seedValid && !generationSize_->text().trimmed().isEmpty();
        configurationRow_->setEnabled(canConfigure);
        saveConfiguration_->setEnabled(ready && !configurationStorePath_.isEmpty());
        exportConfiguration_->setEnabled(ready);
        estimate_->setEnabled(ready && !succeeded_);
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(succeeded_ ||
                                                            (ready && !profile_.readOnly && !profile_.http.enabled));
        return;
    }
    try{auto sql=navidog::tableTaskSql(profile_,t);preview_->setSourceSql(sql.isEmpty()?fileAction(t.action)?"-- 根据所选表的字段及原始结构处理文件。":t.action==A::GenerateData?"-- 按上方字段规则生成并在独立事务中提交。":t.action==A::ReverseModel?"-- 读取真实字段、索引与外键，生成只读模型。":"-- 读取原始表定义后在独立连接中复制。":sql+";");if(fileAction(t.action) && t.path.isEmpty())valid=false;if(dangerous(t.action) && confirmation_->text()!=t.node.object)valid=false;}
    catch(const std::exception& e){preview_->setSourceSql("-- "+QString::fromUtf8(e.what()));valid=false;}
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid && !running_ && !loading_);
}
void TableTaskDialog::addGenerationConfigurationControls(QFormLayout& form) {
    configurationRow_ = new QWidget(fields_);
    auto* layout = new QHBoxLayout(configurationRow_);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto add = [&](const QString& label, const char* id, void (TableTaskDialog::*action)()) {
        auto* button = new navidog::ui::PushButton(label, configurationRow_);
        button->setObjectName(id);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, button, action] {
            button->setFocus();
            QApplication::sendPostedEvents();
            try {
                (this->*action)();
            } catch (const std::exception& e) {
                message_->setText(QString::fromUtf8(e.what()));
            }
        });
        return button;
    };
    auto* select = add("选择配置…", "selectGenerationConfiguration", &TableTaskDialog::selectGenerationConfiguration);
    select->setEnabled(!configurationStorePath_.isEmpty());
    saveConfiguration_ = add("保存配置…", "saveGenerationConfiguration", &TableTaskDialog::saveGenerationConfiguration);
    add("导入配置…", "importGenerationConfiguration", &TableTaskDialog::importGenerationConfiguration);
    exportConfiguration_ = add("导出配置…", "exportGenerationConfiguration", &TableTaskDialog::exportGenerationConfiguration);
    layout->addStretch();
    form.addRow("生成配置", configurationRow_);
}
navidog::GenerationConfiguration TableTaskDialog::generationConfiguration() const {
    bool seedValid = false;
    const auto seed = generationSeed_->text().toULongLong(&seedValid);
    if (!seedValid) throw std::runtime_error("请输入有效的随机种子");
    const auto mode = generationMode_->currentIndex() == 1 ? navidog::GenerationSizeMode::Capacity
                                                           : navidog::GenerationSizeMode::Rows;
    return {{mode, generationSize_->text(), seed, false}, rules_->rules(true)};
}
void TableTaskDialog::applyGenerationConfiguration(const navidog::GenerationConfiguration& config) {
    const auto matched = rules_->applyConfiguration(config.rules);
    const bool capacity = config.request.sizeMode == navidog::GenerationSizeMode::Capacity;
    {
        const QSignalBlocker mode(generationMode_), size(generationSize_), seed(generationSeed_);
        generationMode_->setCurrentIndex(capacity ? 1 : 0);
        generationSize_->setPlaceholderText(capacity ? "例如 20 GB、1.5 TiB" : "例如 100,000,000,000,000");
        generationSize_->setText(config.request.size);
        generationSeed_->setText(QString::number(config.request.seed));
    }
    result_->hide();
    auto status = QString("已应用生成配置 · %1 条字段规则").arg(matched);
    const auto skipped = config.rules.size() - matched;
    if (skipped) status += QString(" · %1 条与当前表不匹配，已跳过").arg(skipped);
    message_->setText(status);
    updatePreview();
}
void TableTaskDialog::saveGenerationConfiguration() {
    const auto config = generationConfiguration();
    bool ok = false;
    const auto name = QInputDialog::getText(this, "保存数据生成配置", "配置名称", QLineEdit::Normal,
                                           {}, &ok).trimmed();
    if (!ok) return;
    navidog::LocalStore store(configurationStorePath_);
    if (!navidog::saveGenerationConfiguration(store, name, config)) {
        if (QMessageBox::question(this, "覆盖配置", "已存在同名配置，是否覆盖？",
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) return;
        navidog::saveGenerationConfiguration(store, name, config, true);
    }
    message_->setText("已保存生成配置 · " + name);
}
void TableTaskDialog::selectGenerationConfiguration() {
    const navidog::LocalStore store(configurationStorePath_);
    const auto entries = navidog::generationConfigurations(store);
    if (entries.isEmpty()) { message_->setText("还没有已保存的数据生成配置。"); return; }
    bool ok = false;
    const auto name = QInputDialog::getItem(this, "选择数据生成配置", "已保存的配置",
                                           entries.keys(), 0, false, &ok);
    if (ok) applyGenerationConfiguration(navidog::generationConfigurationFromJson(entries.value(name).toObject()));
}
void TableTaskDialog::importGenerationConfiguration() {
    const auto path = QFileDialog::getOpenFileName(this, "导入数据生成配置", navidog::ui::exportDirectory(this),
                                                   "数据生成配置 (*.json)");
    if (!path.isEmpty()) applyGenerationConfiguration(navidog::importGenerationConfiguration(path));
}
void TableTaskDialog::exportGenerationConfiguration() {
    const auto config = generationConfiguration();
    const auto context = navidog::ui::exportContext(profile_, original_.node.database, original_.node.schema);
    const auto path = navidog::ui::chooseExportFile(this, "导出数据生成配置", context,
                                                  "数据生成配置-" + table_->currentText(),
                                                  "数据生成配置 (*.json)");
    if (path.isEmpty()) return;
    navidog::exportGenerationConfiguration(path, config);
    message_->setText("已导出数据生成配置");
}
void TableTaskDialog::loadTableMetadata() {
    if(running_ || loading_ || service_.busy() || table_->currentText().isEmpty())return;
    metadataId_=QUuid::createUuid().toString();auto current=task();metadataTable_=current.node.object;
    if(service_.tableDefinition(profile_,current.node,metadataId_)){loading_=true;metadataReady_=false;table_->setEnabled(false);message_->setText("正在读取字段…");updatePreview();}
}
void TableTaskDialog::showProgress(const navidog::TaskInfo& info) {
    using navidog::TaskState;
    if (!taskProgress_) return;
    taskProgress_->show();
    const auto& progress = info.progress;
    taskProgress_->setRange(0, progress.total > 0 ? 100 : 0);
    if (progress.total > 0)
        taskProgress_->setValue(int(std::clamp(100.0 * double(progress.done) / double(progress.total), 0.0, 100.0)));
    if (info.state == TaskState::Queued) message_->setText("排队中，等待后台执行。");
    else if (info.state == TaskState::Cancelling) {
        message_->setText("正在取消并核对写入结果…");
        if (stop_) stop_->setEnabled(false);
    } else {
        auto message = progress.phase.isEmpty() ? QString("正在后台执行…") : progress.phase;
        if (progress.total > 0 && progress.unit == navidog::TaskUnit::Rows)
            message += QString(" · %1 / %2 行").arg(QLocale().toString(progress.done), QLocale().toString(progress.total));
        message_->setText(message);
    }
}
void TableTaskDialog::finishRequest(const navidog::TableTaskResult& outcome) {
    if (taskProgress_) taskProgress_->hide();
    if (stop_) stop_->hide();
    auto* grid = findChild<QTableView*>("generationRules");
    if (grid) grid->setEnabled(true);
    running_ = false;
    outcome_ = outcome;
    fields_->setEnabled(true);
    buttons_->button(QDialogButtonBox::Cancel)->setEnabled(true);
    buttons_->button(QDialogButtonBox::Cancel)->setText("关闭");
    if (!outcome.result.columns.isEmpty()) {
        model_->setResult(outcome.result);
        result_->show();
        if (!rules_ || outcome.result.columns.size() <= 64) result_->resizeColumnsToContents();
    }
    const bool estimated = std::exchange(estimating_, false);
    if (!outcome.error.isEmpty()) {
        message_->setText(outcome.error + (outcome.rolledBack ? "\n本批修改已回滚。" : "") +
            (outcome.partial ? "\n数据库可能已发生部分变更，请刷新核对。" : "") +
            (outcome.commitUnknown ? "\n提交结果未确认，请核对数据库后再操作。" : ""));
        if (tasks_ && !isVisible()) { QDialog::reject(); return; }
        updatePreview();
        if (!tasks_ || estimated) QMessageBox::warning(this, "操作失败", message_->text());
        return;
    }
    message_->setText(outcome.report);
    if (estimated) { updatePreview(); return; }
    succeeded_ = outcome.applied;
    if (estimate_) estimate_->setEnabled(false);
    if (grid) grid->setEnabled(false);
    fields_->setEnabled(false);
    buttons_->button(QDialogButtonBox::Ok)->setText("完成");
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(true);
    if ((tasks_ && !isVisible()) || original_.action == A::Rename || original_.action == A::Drop ||
        original_.action == A::CopyData || original_.action == A::CopyStructure ||
        original_.action == A::ReverseModel || original_.action == A::BuildBI)
        QDialog::accept();
}
void TableTaskDialog::updateRuleHelp() {
    if (!rules_) return;
    const auto* grid = findChild<QTableView*>("generationRules");
    const auto row = grid->currentIndex().row();
    const auto kind = row < 0 ? QString{} : rules_->index(row, 2).data(Qt::EditRole).toString();
    if (kind == "number")
        ruleHelp_->setText("数值范围示例 18..80。区间权重示例 18..35@70;36..59@25;60..80@5。分布形状应用于各区间内部。");
    else if (kind == "temporal")
        ruleHelp_->setText("输入日期范围或 ISO 时间。按小时分布时使用整日范围，可填 0..5@80;6..23@20。时间按 UTC 生成。");
    else if (kind == "choice")
        ruleHelp_->setText("候选项用分号分隔，例如 A@70;B@30。省略 @权重时，该项权重为 1。");
    else if (kind == "regex")
        ruleHelp_->setText("正则会生成全匹配文本，例如 [A-Z]{2}-[0-9]{4}。支持字符组、分支、分组和重复。");
    else if (kind == "foreign")
        ruleHelp_->setText("最多读取 4,096 组已有父键。复合外键的各列应一起采用此规则。");
    else if (kind == "sequence")
        ruleHelp_->setText("输入起始整数，或输入 auto 从目标表当前最大值加 1 开始。");
    else
        ruleHelp_->setText("选择字段查看规则说明。单次最多写入 1,000,000 行，更大规模可预览估算。");
}
void TableTaskDialog::accept() {
    if (succeeded_) { QDialog::accept(); return; }
    startTask(false);
}
void TableTaskDialog::startTask(bool isPreview) {
    if (running_ || loading_) return;
    if (isPreview ? (!estimate_ || !estimate_->isEnabled()) : !buttons_->button(QDialogButtonBox::Ok)->isEnabled()) return;
    if (dangerous(original_.action) && confirmation_->text() != task().node.object) return;
    // Moving focus commits an in-place editor before snapshotting the rules.
    (isPreview ? static_cast<QPushButton*>(estimate_) : buttons_->button(QDialogButtonBox::Ok))->setFocus();
    QApplication::sendPostedEvents();
    auto request = task();
    request.generation.preview = isPreview;
    if (tasks_ && !isPreview) {
        const bool fileOutput = fileAction(request.action) && request.action != A::ImportCsv;
        if (fileAction(request.action)) request.path = QFileInfo(request.path).absoluteFilePath();
        QStringList scope{profile_.name, request.node.database, request.node.schema, request.node.object};
        scope.removeAll(QString{});
        if (!request.path.isEmpty()) scope << request.path;
        const auto resource = profile_.driver == navidog::DatabaseKind::SQLite
            ? QFileInfo(profile_.database).canonicalFilePath() : profile_.id + "/" + profile_.database;
        QString rejection;
        requestId_ = tasks_->submit({navidog::tableActionName(request.action) + " · " + request.node.object,
                                     scope.join(" / "), resource, fileOutput ? request.path : QString{}},
            [profile = profile_, request, fileOutput](const navidog::TaskContext& context) {
                auto outcome = navidog::runTableTask(profile, request, context.cancel, {}, context.report);
                navidog::TaskResult result;
                result.detail = outcome.report;
                result.error = outcome.error;
                if (outcome.rolledBack) result.detail += "\n本批修改已回滚。";
                if (outcome.partial) result.detail += "\n可能发生了部分变更，请刷新核对。";
                if (outcome.commitUnknown) result.detail += "\n提交结果未确认，请核对数据库。";
                result.cancelled = outcome.cancelled && !outcome.commitUnknown && !outcome.partial;
                if (fileOutput && outcome.applied) result.artifacts << request.path;
                result.payload = QVariant::fromValue(std::move(outcome));
                return result;
            }, &rejection);
        if (requestId_.isEmpty()) { message_->setText(rejection); return; }
        backgroundRequest_ = request;
        emit backgroundStarted(requestId_);
    } else {
        requestId_ = QUuid::createUuid().toString();
        if (!service_.tableTask(profile_, request, requestId_)) {
            message_->setText("数据库正在处理其他操作。");
            return;
        }
    }
    estimating_ = isPreview;
    running_ = true;
    if (auto* grid = findChild<QTableView*>("generationRules")) grid->setEnabled(false);
    fields_->setEnabled(false);
    buttons_->button(QDialogButtonBox::Cancel)->setEnabled(true);
    buttons_->button(QDialogButtonBox::Cancel)->setText(backgroundRequest_ ? "转到后台" : "停止");
    if (backgroundRequest_) {
        stop_->show();
        stop_->setEnabled(true);
        if (const auto* info = tasks_->info(requestId_)) showProgress(*info);
    } else message_->setText(isPreview ? "正在生成样本并估算容量…" : "正在执行，请稍候…");
    updatePreview();
}
void TableTaskDialog::reject() {
    if (running_) {
        if (backgroundRequest_) { hide(); return; }
        service_.cancel();
        buttons_->button(QDialogButtonBox::Cancel)->setEnabled(false);
        message_->setText(estimating_ ? "正在停止估算…" : "正在停止并核对事务结果…");
        return;
    }
    if (loading_) service_.cancel();
    Dialog::reject();
}
