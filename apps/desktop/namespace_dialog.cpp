#include "namespace_dialog.hpp"
#include "editor.hpp"
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

NamespaceDialog::NamespaceDialog(const navidog::ConnectionProfile &p,
                                 navidog::NamespaceKind kind,
                                 navidog::QueryService &service, QWidget *parent)
    : Dialog(parent), profile_(p), kind_(kind), service_(service) {
  const bool schema = kind == navidog::NamespaceKind::Schema,
             local = p.driver == navidog::DatabaseKind::SQLite,
             oracle = p.driver == navidog::DatabaseKind::Oracle;
  setObjectName("namespaceDialog");
  setWindowTitle(oracle   ? "新建用户 / Schema"
                 : schema ? "新建 Schema"
                          : "新建数据库");
  resize(640, oracle ? 640 : 570);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *tabs = new QTabWidget;
  tabs->setObjectName("namespaceTabs");
  layout->addWidget(tabs, 1);
  form_ = new QWidget;
  auto *body = new QVBoxLayout(form_);
  body->setContentsMargins(52, 28, 52, 24);
  auto *fields = new QFormLayout;
  fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  fields->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  fields->setHorizontalSpacing(12);
  fields->setVerticalSpacing(12);
  body->addLayout(fields);
  body->addStretch();
  name_ = new navidog::ui::TextInput;
  name_->setObjectName("namespaceName");
  fields->addRow(oracle   ? "用户名 / Schema："
                 : schema ? "Schema 名称："
                          : "数据库名称：",
                 name_);
  auto select = [this](const QString &id) {
    auto *combo = new navidog::ui::Select(form_);
    combo->setObjectName(id);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->addItem("");
    combo->lineEdit()->setPlaceholderText("默认");
    return combo;
  };
  owner_ = select("namespaceOwner");
  charset_ = select("namespaceCharset");
  collation_ = select("namespaceCollation");
  const bool owner =
      (schema && !oracle && p.driver != navidog::DatabaseKind::DuckDb) ||
      (!schema && navidog::usesNativePostgres(p.driver));
  const bool mysql = navidog::isMySqlFamily(p.driver);
  if (owner)
    fields->addRow("所有者：", owner_);
  else
    owner_->hide();
  if (mysql)
    fields->addRow("字符集：", charset_);
  else
    charset_->hide();
  if (mysql || (!schema && p.driver == navidog::DatabaseKind::SQLServer))
    fields->addRow("排序规则：", collation_);
  else
    collation_->hide();
  password_ = new navidog::ui::TextInput(form_);
  password_->setObjectName("namespacePassword");
  password_->setEchoMode(QLineEdit::Password);
  confirmPassword_ = new navidog::ui::TextInput(form_);
  confirmPassword_->setObjectName("namespaceConfirmPassword");
  confirmPassword_->setEchoMode(QLineEdit::Password);
  tablespace_ = select("namespaceTablespace");
  temporaryTablespace_ = select("namespaceTemporaryTablespace");
  quota_ = new navidog::ui::NumberInput(form_);
  quota_->setObjectName("namespaceQuota");
  quota_->setRange(0, 1048576);
  quota_->setSuffix(" MiB");
  quota_->setSpecialValueText("不分配配额");
  grantSession_ = new navidog::ui::CheckBox("允许登录（CREATE SESSION）", form_);
  grantSession_->setObjectName("namespaceGrantSession");
  grantSession_->setChecked(true);
  locked_ = new navidog::ui::CheckBox("锁定账户", form_);
  locked_->setObjectName("namespaceLocked");
  if (oracle) {
    name_->setPlaceholderText("保持输入的大小写，如 APP_USER");
    fields->addRow("密码：", password_);
    fields->addRow("确认密码：", confirmPassword_);
    fields->addRow("默认表空间：", tablespace_);
    fields->addRow("临时表空间：", temporaryTablespace_);
    fields->addRow("默认表空间配额：", quota_);
    fields->addRow({}, grantSession_);
    fields->addRow({}, locked_);
    auto *hint = new QLabel("连接目标 PDB "
                            "创建本地用户。仅授予勾选的登录权限；建表等权限可随"
                            "后按需授予。密码不会保存到本地。");
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    fields->addRow({}, hint);
  } else
    for (auto *control :
         QList<QWidget *>{password_, confirmPassword_, tablespace_,
                          temporaryTablespace_, quota_, grantSession_, locked_})
      control->hide();
  path_ = new navidog::ui::TextInput(form_);
  path_->setObjectName("namespaceFile");
  if (local) {
    auto *pathRow = new QWidget;
    auto *line = new QHBoxLayout(pathRow);
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(path_);
    auto *browse = new navidog::ui::PushButton("选择…");
    browse->setObjectName("namespaceBrowse");
    line->addWidget(browse);
    fields->addRow("数据库文件：", pathRow);
    connect(browse, &QPushButton::clicked, this, [this] {
      auto file = QFileDialog::getSaveFileName(
          this, "新建 SQLite 数据库", path_->text(),
          "SQLite (*.db *.sqlite *.sqlite3);;所有文件 (*)", nullptr,
          QFileDialog::DontConfirmOverwrite);
      if (!file.isEmpty())
        path_->setText(file);
    });
  } else
    path_->hide();
  if (schema) {
    auto *context =
        new QLabel((oracle ? "服务 / SID：" : "数据库：") + p.database);
    context->setObjectName("muted");
    context->setTextFormat(Qt::PlainText);
    fields->addRow({}, context);
  }
  tabs->addTab(form_, "常规");
  auto *sqlPreview = new SqlReadOnlyView(this);
  sqlPreview->setDatabaseKind(p.driver);
  preview_ = sqlPreview;
  preview_->setObjectName("namespaceSqlPreview");
  tabs->addTab(preview_->withActions(), "SQL 预览");
  auto *feedback = new QWidget;
  auto *line = new QHBoxLayout(feedback);
  line->setContentsMargins(20, 8, 20, 8);
  message_ = new QLabel;
  message_->setObjectName("namespaceMessage");
  message_->setTextFormat(Qt::PlainText);
  message_->setWordWrap(true);
  line->addWidget(message_, 1);
  retry_ = new navidog::ui::PushButton("重新加载选项");
  retry_->setObjectName("namespaceRetry");
  retry_->hide();
  line->addWidget(retry_);
  layout->addWidget(feedback);
  auto *footer = new QWidget;
  footer->setObjectName("namespaceFooter");
  auto *footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(16, 12, 16, 12);
  footerLayout->addStretch();
  buttons_ =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons_->button(QDialogButtonBox::Ok)->setText("好");
  buttons_->button(QDialogButtonBox::Ok)->setProperty("primary", true);
  buttons_->button(QDialogButtonBox::Cancel)->setText("取消");
  footerLayout->addWidget(buttons_);
  layout->addWidget(footer);
  setStyleSheet(R"(
        QDialog#namespaceDialog { background:white; }
        QTabWidget#namespaceTabs::tab-bar { alignment:center; }
        QTabWidget#namespaceTabs::pane { border-top:1px solid #d7d7d7; background:white; }
        QTabWidget#namespaceTabs QTabBar::tab { margin:6px 0; padding:4px 16px; min-width:36px; border:0; border-radius:4px; background:transparent; color:#303030; }
        QTabWidget#namespaceTabs QTabBar::tab:selected { background:#2878d8; color:white; }
        QWidget#namespaceFooter { background:#f1f1f1; border-top:1px solid #d7d7d7; }
        QWidget#namespaceFooter QPushButton { min-width:64px; padding:4px 12px; }
        QPlainTextEdit#namespaceSqlPreview { padding:12px; }
    )");
  connect(buttons_, &QDialogButtonBox::accepted, this,
          &NamespaceDialog::accept);
  connect(buttons_, &QDialogButtonBox::rejected, this,
          &NamespaceDialog::reject);
  connect(retry_, &QPushButton::clicked, this, &NamespaceDialog::loadOptions);
  for (auto *input : {name_, path_, password_, confirmPassword_})
    connect(input, &QLineEdit::textChanged, this,
            &NamespaceDialog::updatePreview);
  for (auto *combo :
       {owner_, charset_, collation_, tablespace_, temporaryTablespace_})
    connect(combo, &QComboBox::currentTextChanged, this, [this, combo] {
      if (combo == charset_)
        updateCollations();
      updatePreview();
    });
  connect(quota_, qOverload<int>(&QSpinBox::valueChanged), this,
          &NamespaceDialog::updatePreview);
  for (auto *check : {grantSession_, locked_})
    connect(check, &QCheckBox::toggled, this, &NamespaceDialog::updatePreview);
  connect(&service_, &navidog::QueryService::namespaceOptionsCompleted, this,
          [this](const QString &id, const navidog::NamespaceOptions &result) {
            if (id != optionsId_)
              return;
            loading_ = false;
            options_ = result;
            for (auto entry : {qMakePair(owner_, result.owners),
                               qMakePair(charset_, result.charsets),
                               qMakePair(tablespace_, result.tablespaces),
                               qMakePair(temporaryTablespace_,
                                         result.temporaryTablespaces)}) {
              const QSignalBlocker block(entry.first);
              auto text = entry.first->currentText();
              entry.first->clear();
              entry.first->addItem("");
              entry.first->addItems(entry.second);
              entry.first->setEditText(text);
            }
            updateCollations();
            retry_->setVisible(!result.error.isEmpty());
            message_->setText(result.error.isEmpty()
                                  ? QString{}
                                  : "选项加载失败，可保留默认值或手动填写。\n" +
                                        result.error);
            updatePreview();
          });
  connect(
      &service_, &navidog::QueryService::namespaceCompleted, this,
      [this](const QString &id, const navidog::NamespaceResult &result) {
        if (id != createId_)
          return;
        applying_ = false;
        partial_ = result.partial;
        form_->setEnabled(!partial_);
        buttons_->button(QDialogButtonBox::Cancel)->setEnabled(true);
        if (result.applied) {
          password_->clear();
          confirmPassword_->clear();
          QDialog::accept();
          return;
        }
        const auto error =
            partial_ ? result.error
            : result.cancelled
                ? QString(
                      "操作已停止，结果可能未确认，请刷新对象后核对再操作。")
                : result.error;
        message_->setText(error);
        updatePreview();
        QMessageBox::warning(this, "创建失败", error);
      });
  updatePreview();
  name_->setFocus();
  if (!local)
    QTimer::singleShot(0, this, &NamespaceDialog::loadOptions);
}
navidog::NamespaceDraft NamespaceDialog::draft() const {
  navidog::NamespaceDraft d;
  d.kind = kind_;
  d.name = name_->text();
  d.owner = owner_->currentText();
  d.charset = charset_->currentText();
  d.collation = collation_->currentText();
  d.filePath = path_->text().trimmed();
  d.password = password_->text();
  d.defaultTablespace = tablespace_->currentText();
  d.temporaryTablespace = temporaryTablespace_->currentText();
  d.quotaMiB = quota_->value();
  d.grantSession = grantSession_->isChecked();
  d.accountLocked = locked_->isChecked();
  return d;
}
void NamespaceDialog::loadOptions() {
  optionsId_ = QUuid::createUuid().toString();
  if (service_.namespaceOptions(profile_, kind_, optionsId_)) {
    loading_ = true;
    retry_->hide();
    message_->setText("正在读取服务器选项…");
    updatePreview();
  } else {
    message_->setText("正在处理其他数据库操作，请稍后重新加载。");
    retry_->show();
  }
}
void NamespaceDialog::updateCollations() {
  const QSignalBlocker blocker(collation_);
  auto old = collation_->currentText();
  collation_->clear();
  collation_->addItem("");
  for (int i = 0; i < options_.collations.size(); ++i)
    if (charset_->currentText().isEmpty() ||
        options_.collationCharsets.isEmpty() ||
        options_.collationCharsets.value(i) == charset_->currentText())
      collation_->addItem(options_.collations[i]);
  collation_->setEditText(collation_->findText(old) >= 0 ? old : QString{});
}
void NamespaceDialog::updatePreview() {
  bool valid = true;
  try {
    auto sql = navidog::namespaceSql(profile_, draft());
    if (profile_.driver == navidog::DatabaseKind::Oracle &&
        password_->text() != confirmPassword_->text())
      throw std::runtime_error("两次输入的密码不一致。");
    if (profile_.driver == navidog::DatabaseKind::SQLite) {
      if (draft().name.trimmed().isEmpty())
        throw std::runtime_error("请填写数据库名称。");
      sql =
          "-- SQLite 数据库由新文件创建，不执行 CREATE DATABASE。\n-- 文件：" +
          draft().filePath;
    }
    preview_->setSourceSql(sql);
  } catch (const std::exception &e) {
    valid = false;
    preview_->setSourceSql("-- " + QString::fromUtf8(e.what()));
  }
  buttons_->button(QDialogButtonBox::Ok)
      ->setEnabled(valid && !loading_ && !applying_ && !partial_);
}
void NamespaceDialog::accept() {
  if (loading_ || applying_ || partial_)
    return;
  if (profile_.driver == navidog::DatabaseKind::Oracle &&
      password_->text() != confirmPassword_->text()) {
    message_->setText("两次输入的密码不一致。");
    return;
  }
  try {
    navidog::namespaceSql(profile_, draft());
  } catch (const std::exception &e) {
    message_->setText(QString::fromUtf8(e.what()));
    return;
  }
  createId_ = QUuid::createUuid().toString();
  if (service_.createNamespace(profile_, draft(), createId_)) {
    applying_ = true;
    form_->setEnabled(false);
    buttons_->button(QDialogButtonBox::Cancel)->setEnabled(false);
    message_->setText("正在创建…");
    updatePreview();
  } else
    message_->setText("正在处理其他数据库操作，请稍后重试。");
}
void NamespaceDialog::reject() {
  if (applying_)
    return;
  if (loading_)
    service_.cancel();
  Dialog::reject();
}

NamespaceDropDialog::NamespaceDropDialog(navidog::DatabaseKind driver,
                                         navidog::NamespaceKind kind,
                                         const QString &name, const QString &sql,
                                         QWidget *parent, bool cascade)
    : Dialog(parent), name_(name), confirmation_(new navidog::ui::TextInput(this)) {
  const bool database = kind == navidog::NamespaceKind::Database;
  const auto object = database ? QString("数据库")
                      : driver == navidog::DatabaseKind::Oracle
                          ? QString("用户 / Schema") : QString("Schema");
  setObjectName("namespaceDropDialog");
  setWindowTitle((cascade ? "级联删除 " : "删除 ") + object);
  resize(580, 290);

  auto *layout = new QVBoxLayout(this);
  auto *warning = new QLabel(
      database
          ? QString("将永久删除数据库“%1”及其内容。此应用会关闭目标库会话并回滚未提交事务。"
                    "本地标签和草稿会保留。").arg(name)
          : cascade ? QString("将删除“%1”及其中的对象。数据库也会按方言规则处理其他 Schema 中的依赖。").arg(name)
                    : QString("将删除“%1”。数据库会拒绝删除仍有对象或被依赖的 %2。").arg(name, object));
  warning->setTextFormat(Qt::PlainText);
  warning->setWordWrap(true);
  layout->addWidget(warning);

  auto *preview = new SqlReadOnlyView;
  preview->setObjectName("dropNamespaceSqlPreview");
  preview->setDatabaseKind(driver);
  preview->setSourceSql(sql);
  preview->setMaximumHeight(95);
  layout->addWidget(preview->withActions());
  confirmation_->setObjectName("confirmNamespaceDrop");
  confirmation_->setPlaceholderText("输入完整名称以确认");
  layout->addWidget(confirmation_);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  auto *drop = buttons->addButton("删除 " + object, QDialogButtonBox::AcceptRole);
  drop->setObjectName("executeNamespaceDrop");
  drop->setEnabled(false);
  connect(confirmation_, &QLineEdit::textChanged, drop,
          [drop, name](const QString &value) { drop->setEnabled(value == name); });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
  confirmation_->setFocus();
}

void NamespaceDropDialog::done(int result) {
  if (result != QDialog::Accepted || confirmation_->text() == name_)
    Dialog::done(result);
}
