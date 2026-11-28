#include "object_dialogs.hpp"
#include "editor.hpp"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QStandardItemModel>
#include <QTabWidget>

namespace navidog::ui {
ObjectDetailsDialog::ObjectDetailsDialog(const ConnectionProfile& profile, const CatalogNode& node,
                                         QWidget* parent) : Dialog(parent) {
    const auto name = node.name.isEmpty() ? node.object : node.name;
    setObjectName("objectPropertiesDialog");
    setWindowTitle(name + " · 属性");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 475);

    auto* layout = new QVBoxLayout(this);
    auto* sections = new QTabWidget(this);
    auto* grid = new ObjectGrid(sections);
    grid->setObjectName("objectPropertiesGrid");
    properties_ = new QStandardItemModel(grid);
    properties_->setHorizontalHeaderLabels({"属性", "值"});
    grid->setModel(properties_);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    grid->horizontalHeader()->setStretchLastSection(true);
    grid->verticalHeader()->hide();
    properties_->appendRow({new QStandardItem("名称"), new QStandardItem(name)});
    properties_->appendRow({new QStandardItem("对象类型"), new QStandardItem(catalogObjectTypeName(node.kind))});
    sections->addTab(grid, "属性");

    sql_ = new SqlReadOnlyView;
    sql_->setObjectName("objectPropertiesSql");
    sql_->setDatabaseKind(profile.driver);
    sql_->setPlaceholderText("正在读取对象定义…");
    if (node.kind == "schema" || node.kind == "database") {
        sql_->hide();
        sql_->setParent(this);
    } else sections->addTab(sql_->withActions(), "SQL 定义");
    layout->addWidget(sections, 1);

    status_ = new QLabel("正在读取服务器目录…", this);
    status_->setObjectName("objectPropertiesStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->setMaximumHeight(90);
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ObjectDetailsDialog::showDetails(const CatalogObjectDetails& details) {
    if (!details.properties.isEmpty()) {
        properties_->removeRows(0, properties_->rowCount());
        for (const auto& property : details.properties)
            properties_->appendRow({new QStandardItem(property.first), new QStandardItem(property.second)});
    }
    sql_->setSourceSql(details.sql);
    sql_->setPlaceholderText("服务器目录未提供此对象的独立 SQL 定义");
    const auto notice = details.cancelled ? QString("读取已取消") :
        !details.error.isEmpty() ? details.error : details.warning;
    status_->setText(notice.left(1200));
    status_->setToolTip(notice);
    status_->setVisible(!notice.isEmpty());
}

bool confirmObjectDrop(QWidget* parent, const ConnectionProfile& profile, const CatalogNode& node,
                       CatalogObjectChange change, const QString& sql) {
    const bool cascade = change == CatalogObjectChange::DropCascade;
    const auto action = cascade ? QString("级联删除") : QString("删除");
    Dialog dialog(parent);
    dialog.setObjectName("sourceDropDialog");
    dialog.setWindowTitle(action + catalogObjectTypeName(node.kind));
    dialog.resize(690, 275);
    auto* layout = new QVBoxLayout(&dialog);
    auto message = QString("将%1 %2“%3”。").arg(action, catalogObjectTypeName(node.kind), node.object);
    if (cascade) message += profile.driver == DatabaseKind::Oracle ?
        "数据库会同时删除引用该对象的外键约束。" :
        "这也可能删除其他 Schema 中的依赖对象。";
    auto* summary = new QLabel(message, &dialog);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto* preview = new SqlReadOnlyView;
    preview->setObjectName("dropObjectSqlPreview");
    preview->setDatabaseKind(profile.driver);
    preview->setSourceSql(sql + ";");
    layout->addWidget(preview->withActions(), 1);

    auto* confirmation = new TextInput(&dialog);
    confirmation->setObjectName("confirmObjectDrop");
    confirmation->setPlaceholderText("输入对象完整名称");
    layout->addWidget(confirmation);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* remove = buttons->addButton(action, QDialogButtonBox::AcceptRole);
    remove->setObjectName("executeObjectDrop");
    remove->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    QObject::connect(confirmation, &QLineEdit::textChanged, remove, [remove, &node](const QString& value) {
        remove->setEnabled(value == node.object);
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (confirmation->text() == node.object) dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    confirmation->setFocus();
    return dialog.exec() == QDialog::Accepted;
}
std::optional<QString> chooseObjectName(QWidget* parent, const ConnectionProfile& profile, const CatalogNode& node) {
    Dialog dialog(parent);
    dialog.setObjectName("objectRenameDialog");
    dialog.setWindowTitle("重命名" + catalogObjectTypeName(node.kind));
    dialog.resize(695, 285);
    auto* layout = new QVBoxLayout(&dialog);
    auto* input = new TextInput(&dialog);
    input->setObjectName("objectNewName");
    input->setText(node.name);
    input->setPlaceholderText("新名称");
    layout->addWidget(input);
    auto* preview = new SqlReadOnlyView;
    preview->setObjectName("objectRenameSql");
    preview->setDatabaseKind(profile.driver);
    layout->addWidget(preview->withActions(), 1);
    auto* note = new QLabel("依赖对象或脚本中的名称引用可能需要随之更新。", &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* rename = buttons->addButton("重命名", QDialogButtonBox::AcceptRole);
    rename->setObjectName("executeObjectRename");
    layout->addWidget(buttons);
    const auto update = [&] {
        try {
            preview->setSourceSql(catalogObjectChangeSql(profile,node,CatalogObjectChange::Rename,input->text()) + ";");
            rename->setEnabled(true);
        } catch (const std::exception& error) {
            preview->setSourceSql({});
            preview->setPlaceholderText(QString::fromUtf8(error.what()));
            rename->setEnabled(false);
        }
    };
    QObject::connect(input, &QLineEdit::textChanged, &dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {if (rename->isEnabled()) dialog.accept();});
    update();
    input->selectAll();
    input->setFocus();
    if (dialog.exec()!=QDialog::Accepted) return {};
    return input->text();
}

bool confirmObjectBatchDrop(QWidget* parent, DatabaseKind driver, int count, bool cascade, const QStringList& sql) {
    Dialog dialog(parent);
    dialog.setObjectName("batchObjectDropDialog");
    const auto action = cascade ? QString("批量级联删除") : QString("批量删除");
    dialog.setWindowTitle(action);
    dialog.resize(760,380);
    auto* layout = new QVBoxLayout(&dialog);
    auto info = QString("将%1 %2 个对象。").arg(action).arg(count);
    if (cascade) info += driver==DatabaseKind::Oracle ? "引用约束将按服务器规则删除。" :
        "依赖对象也可能被删除，范围可能跨 Schema。";
    if (driver==DatabaseKind::Oracle || isMySqlFamily(driver) || usesJdbcCatalog(driver))
        info += "数据库可能逐条提交，失败时会报告已经删除的对象。";
    auto* message = new QLabel(info, &dialog);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto* preview = new SqlReadOnlyView;
    preview->setObjectName("batchDropSqlPreview");
    preview->setDatabaseKind(driver);
    preview->setSourceSql(sql.join(";\n") + ";");
    layout->addWidget(preview->withActions(),1);
    auto* input = new TextInput(&dialog);
    input->setObjectName("confirmBatchObjectDrop");
    const auto phrase = QString("删除 %1").arg(count);
    input->setPlaceholderText("输入「"+phrase+"」确认");
    layout->addWidget(input);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel,&dialog);
    auto* remove = buttons->addButton(action,QDialogButtonBox::AcceptRole);
    remove->setObjectName("executeBatchObjectDrop");
    remove->setEnabled(false);
    QObject::connect(input,&QLineEdit::textChanged,remove,[remove,phrase](const QString& text) {
        remove->setEnabled(text==phrase);
    });
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {if (input->text()==phrase) dialog.accept();});
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec()==QDialog::Accepted;
}

}
