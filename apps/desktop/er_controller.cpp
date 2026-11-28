#include "main_window.hpp"
#include "navigation_pane.hpp"
#include "components.hpp"
#include "er_pane.hpp"
#include "query_pane.hpp"
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QTabWidget>
#include <QTimer>
#include <algorithm>

namespace m = navidog::er;
namespace ui = navidog::ui;
using K = navidog::DatabaseKind;

ErPane* MainWindow::createErPane() {
    auto* pane = new ErPane(QDir(dataDirectory_).filePath("er-drafts"));
    addWorkspaceTab(pane,"ER 模型");
    tabs_->setCurrentWidget(pane);
    connect(pane,&ErPane::titleChanged,this,[this,pane](const QString& title) {
        const int index = tabs_->indexOf(pane);
        if (index >= 0) tabs_->setTabText(index,title);
    });
    pane->connectionName = [this](const QString& id) {
        const auto profile = std::find_if(profiles_.cbegin(),profiles_.cend(),
            [&](const auto& entry) { return entry.id == id; });
        return profile == profiles_.cend() ? QString{} : profile->name;
    };
    pane->reverseRequested = [this] { importErProject(navigation_->currentItem()); };
    pane->detailsRequested = [this,pane](int index) {
        const auto& model = pane->project();
        auto it = std::find_if(profiles_.begin(),profiles_.end(),[&](const auto& p) { return p.id == model.profileId; });
        if (it == profiles_.end() || it->driver != model.driver) {
            showError("无法读取表结构","模型记录的连接已不可用，请重新通过目标连接导入。");
            return;
        }
        if (!promptPassword(*it)) return;
        auto profile = *it;
        if (!model.database.isEmpty()) profile.database = model.database;
        profile.advanced.initialSql.clear();
        pane->loadDetails(index,std::move(profile));
    };
    pane->openSqlRequested = [this,pane](const QString& sql) {
        const auto& model = pane->project();
        QString id = model.profileId;
        const auto sameDriver = [&](const auto& p) {
            return p.driver == model.driver || (navidog::isMySqlFamily(p.driver) && navidog::isMySqlFamily(model.driver));
        };
        auto profile = std::find_if(profiles_.cbegin(),profiles_.cend(),[&](const auto& p){return p.id==id && sameDriver(p) && !p.readOnly;});
        if (profile == profiles_.cend()) {
            QStringList options;
            QVector<QString> ids;
            for (const auto& p : profiles_) if (sameDriver(p) && !p.readOnly) {
                options << QString("%1. %2").arg(options.size()+1).arg(p.name);
                ids << p.id;
            }
            if (options.isEmpty()) {
                showError("没有可用的目标连接","请选择与模型方言兼容的可写连接，或保存 SQL 文件。");
                return false;
            }
            bool ok = false;
            const auto chosen = QInputDialog::getItem(this,"打开模型 SQL","目标连接",options,0,false,&ok);
            if (!ok) return false;
            id = ids[options.indexOf(chosen)];
        }
        auto targetDatabase = model.database;
        if (model.driver == K::SQLite || id != model.profileId) targetDatabase.clear();
        newQuery(sql,id,targetDatabase,navidog::WorkspaceTabKind::Query,model.schema);
        status_->setText("已打开模型 SQL，等待手动运行");
        return true;
    };
    return pane;
}

void MainWindow::newErProject() {
    ui::Dialog dialog(this);
    dialog.setWindowTitle("新建 ER 模型");
    auto* form = new QFormLayout(&dialog);
    auto* name = new ui::TextInput;
    name->setObjectName("erProjectName");
    name->setText("ER 模型");
    auto* dialect = new ui::Select;
    dialect->setObjectName("erProjectDialect");
    auto* version = new ui::Select;
    auto* schema = new ui::TextInput;
    schema->setObjectName("erProjectSchema");
    for (K kind : {K::MySQL,K::PostgreSQL,K::SQLite,K::Oracle,K::SQLServer,K::MariaDB,K::OceanBase,K::TiDB})
        dialect->addItem(navidog::databaseKindName(kind),int(kind));
    const auto defaultSchema = [&] {
        const auto kind = K(dialect->currentData().toInt());
        schema->setText(kind == K::SQLite ? "main" : kind == K::PostgreSQL ? "public" : kind == K::SQLServer ? "dbo" : "");
        version->clear();
        version->addItem("自动 / 通用版本",int(navidog::DatabaseVersion::Auto));
        for (const auto& value : navidog::versionsFor(kind))
            if (value.version != navidog::DatabaseVersion::Auto)
                version->addItem(QString::fromUtf8(value.name),int(value.version));
    };
    connect(dialect,QOverload<int>::of(&QComboBox::currentIndexChanged),&dialog,defaultSchema);
    defaultSchema();
    form->addRow("模型名称",name);
    form->addRow("数据库方言",dialect);
    form->addRow("版本",version);
    form->addRow("默认数据库 / Schema",schema);
    auto* buttons = new ui::FormButtons;
    buttons->button(QDialogButtonBox::Save)->setText("创建模型");
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    m::Project project;
    if (!name->text().trimmed().isEmpty()) project.title = name->text().trimmed();
    project.driver = K(dialect->currentData().toInt());
    project.version = navidog::DatabaseVersion(version->currentData().toInt());
    project.schema = schema->text().trimmed();
    if (navidog::isMySqlFamily(project.driver)) project.database = project.schema;
    createErPane()->useProject(std::move(project));
}

void MainWindow::openErProject() {
    const auto path = QFileDialog::getOpenFileName(this,"打开 ER 模型",{},"Navidog ER (*.naver.json *.json)");
    if (!path.isEmpty()) createErPane()->openFile(path);
}

void MainWindow::importErProject(QTreeWidgetItem* context) {
    if (auto* existing = findChild<QDialog*>("erImportDialog")) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    const auto selectedId = context ? context->data(0,Qt::UserRole).toString() : QString{};
    const auto node = context ? context->data(0,Qt::UserRole+2).value<navidog::CatalogNode>() : navidog::CatalogNode{};
    auto* dialog = new ui::Dialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("从数据库逆向导入 ER 模型");
    dialog->setObjectName("erImportDialog");
    auto* form = new QFormLayout(dialog);
    auto* connections = new ui::Select;
    connections->setObjectName("erImportConnection");
    for (const auto& p : profiles_)
        if (m::supportsReverse(p.driver)) connections->addItem(p.name,p.id);
    if (!connections->count()) {
        delete dialog;
        showError("没有数据库连接","请先通过连接管理创建数据库连接。");
        return;
    }
    const int defaultConnection = connections->findData(selectedId);
    if (defaultConnection >= 0) connections->setCurrentIndex(defaultConnection);
    auto* physicalDatabase = new ui::TextInput;
    physicalDatabase->setObjectName("erImportDatabase");
    auto* schema = new ui::TextInput;
    schema->setObjectName("erImportSchema");
    auto* contains = new ui::TextInput;
    contains->setObjectName("erImportFilter");
    contains->setPlaceholderText("留空导入整个范围");
    auto* names = new ui::TextInput;
    names->setObjectName("erImportNames");
    names->setPlaceholderText("可选，多个表名按逗号分隔");
    auto* view = new ui::CheckBox("包含视图");
    view->setChecked(true);
    const auto defaults = [this,connections,physicalDatabase,schema,names,selectedId,node] {
        const auto currentId = connections->currentData().toString();
        const auto p = std::find_if(profiles_.cbegin(),profiles_.cend(),[&](const auto& one) {return one.id==currentId;});
        if (p == profiles_.cend()) return;
        const auto k = p->driver;
        const bool specific = selectedId == currentId;
        physicalDatabase->setText(specific && !node.database.isEmpty() && k != K::SQLite ? node.database : p->database);
        physicalDatabase->setEnabled(k != K::SQLite && k != K::Oracle);
        schema->setEnabled(k != K::SQLite && !navidog::isMySqlFamily(k));
        schema->setPlaceholderText("留空读取连接的当前 Schema");
        schema->setText(specific ? node.schema : QString{});
        if (specific && (node.kind == "table" || node.kind == "view"))
            names->setText(navidog::quoteIdentifier(node.object,K::PostgreSQL));
        else names->clear();
    };
    connect(connections,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,defaults);
    defaults();
    form->addRow("连接",connections);
    form->addRow("数据库",physicalDatabase);
    form->addRow("Schema",schema);
    form->addRow("表名包含",contains);
    form->addRow("仅指定对象",names);
    form->addRow(view);
    auto* buttons = new ui::FormButtons;
    auto* import = buttons->button(QDialogButtonBox::Save);
    import->setText("导入模型");
    form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,dialog,
            [this,dialog,import,connections,physicalDatabase,schema,contains,names,view] {
        const auto id = connections->currentData().toString();
        const auto db = physicalDatabase->text().trimmed();
        m::ImportOptions request;
        try {
            request.schema = schema->text().trimmed();
            request.nameContains = contains->text().trimmed();
            request.exactNames = navidog::parseIdentifierList(names->text());
            request.includeViews = view->isChecked();
        } catch (const std::exception& e) {
            QMessageBox::warning(dialog,"对象名称无效",QString::fromUtf8(e.what()));
            return;
        }
        import->setEnabled(false);
        // Let the confirmation button finish dispatching its mouse release
        // before hiding the native dialog, then open the tab after it is gone.
        QMetaObject::invokeMethod(dialog,[this,dialog,id,db,request=std::move(request)]() mutable {
            if (!dialog->isVisible()) return;
            const bool restoreFocus = dialog->isActiveWindow();
            connect(dialog,&QObject::destroyed,this,[this,id,db,restoreFocus,request=std::move(request)]() mutable {
                if (restoreFocus)
                    activateWindow();
                auto profile = std::find_if(profiles_.begin(),profiles_.end(),[&](const auto& p){return p.id==id;});
                if (profile == profiles_.end() || !promptPassword(*profile)) return;
                auto scoped = *profile;
                if (scoped.driver != K::SQLite && scoped.driver != K::Oracle) scoped.database = db;
                scoped.advanced.initialSql.clear();
                if (scoped.driver == K::SQLite) scoped.readOnly = true;
                if (navidog::isMySqlFamily(scoped.driver)) request.schema = scoped.database;
                auto* pane = createErPane();
                if (restoreFocus)
                    navigation_->focusTree();
                pane->importFrom(std::move(scoped),std::move(request));
            },Qt::QueuedConnection);
            dialog->accept();
        },Qt::QueuedConnection);
    });
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    dialog->show();
}
