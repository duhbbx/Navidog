#include "driver_manager_dialog.hpp"
#include "connection_dialog.hpp"
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QScrollArea>
#include <QUuid>
#include <QMessageBox>
#include "navidog/secret_store.hpp"
#include <QVBoxLayout>

namespace {
using K=navidog::DatabaseKind;
const QVector<K> kinds{K::MySQL,K::PostgreSQL,K::SQLServer,K::Oracle,K::SQLite,K::OceanBase,K::MariaDB,K::TiDB,K::Dameng,K::H2,K::Kingbase,K::UxDb,K::Redis,
                         K::PolarDbMySql,K::PolarDbPostgres,K::GaussDb,K::Db2,K::DuckDb,K::ClickHouse,K::MongoDb,K::OpenGauss,
                         K::Doris,K::StarRocks,K::CockroachDb,K::YugabyteDb};
class ConnectionDiagram final : public QWidget {
public:
    explicit ConnectionDiagram(QWidget* parent):QWidget(parent){setFixedHeight(122);}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const int c=width()/2;
        p.setPen(QPen(QColor("#b6bcc2"),2));p.drawLine(c-70,46,c+70,46);p.setBrush(QColor("#b6bcc2"));p.drawEllipse(QPointF(c,46),3,3);
        p.setPen(QPen(QColor("#919aa3"),2));p.setBrush(QColor("#f0f2f4"));p.drawRoundedRect(QRectF(c-110,25,40,35),5,5);p.drawLine(c-100,69,c-80,69);p.drawLine(c-90,60,c-90,69);
        p.drawRect(QRectF(c+70,31,40,29));p.drawEllipse(QRectF(c+70,51,40,17));p.setBrush(QColor("#d6dce1"));p.drawEllipse(QRectF(c+70,23,40,17));
        p.setPen(QColor("#69727a"));p.drawText(QRect(c-130,80,80,22),Qt::AlignCenter,"Navidog");p.drawText(QRect(c+50,80,80,22),Qt::AlignCenter,"数据库");
    }
};
QWidget* centeredForm(QFormLayout*& form,QWidget* page) {
    auto* fields=new QWidget(page);fields->setFixedWidth(470);form=new QFormLayout(fields);form->setLabelAlignment(Qt::AlignRight|Qt::AlignVCenter);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setContentsMargins(0,0,0,0);form->setHorizontalSpacing(12);form->setVerticalSpacing(10);return fields;
}
void addCentered(QVBoxLayout* layout,QWidget* widget){auto* row=new QHBoxLayout;row->addStretch();row->addWidget(widget);row->addStretch();layout->addLayout(row);}
}
ConnectionDialog::ConnectionDialog(const navidog::ConnectionProfile& original,bool editing,const QVector<navidog::ConnectionGroup>& groups,navidog::QueryService& service,QWidget* parent,bool chooseTypeFirst)
    :Dialog(parent),original_(original),service_(service),editing_(editing) {
    setObjectName("connectionDialog");setWindowTitle(editing?"编辑连接":"新建连接");resize(900,690);setMinimumSize(900,620);
    setStyleSheet(R"(
        QDialog#connectionDialog {background:white;}
        QDialog#connectionDialog QWidget {font-size:13px;color:#30343a;}
        QDialog#connectionDialog QLineEdit, QDialog#connectionDialog QComboBox, QDialog#connectionDialog QSpinBox {padding:2px 5px;min-height:19px;border:1px solid #bfc5cc;border-radius:3px;background:white;}
        QDialog#connectionDialog QLineEdit:focus {border-color:#408bce;}
        QDialog#connectionDialog QPushButton {padding:3px 14px;min-height:21px;border:1px solid #bdc4cc;border-radius:4px;background:#fafafa;}
        QDialog#connectionDialog QPushButton:hover {background:#e9f1fa;}
        QDialog#connectionDialog QPushButton[primary="true"] {background:#287bd3;border-color:#287bd3;color:white;}
        QDialog#connectionDialog QPushButton:disabled {color:#a8adb2;background:#f4f4f4;border-color:#dedede;}
        QDialog#connectionDialog QTabWidget::tab-bar {alignment:center;}
        QDialog#connectionDialog QTabWidget::pane {border-top:1px solid #d9dce0;background:white;}
        QDialog#connectionDialog QTabBar::tab {background:white;border:0;padding:9px 22px;min-width:35px;color:#646b73;}
        QDialog#connectionDialog QTabBar::tab:selected {color:#1c72bf;border-bottom:2px solid #2684d7;padding-top:9px;}
        QDialog#connectionDialog QWidget#connectionFilters {background:#f0f0f0;}
        QDialog#connectionDialog QToolButton[connectionCard="true"] {border:1px solid transparent;border-radius:6px;padding:12px 4px;background:white;}
        QDialog#connectionDialog QToolButton[connectionCard="true"]:hover {background:#f2f7fc;}
        QDialog#connectionDialog QToolButton[connectionCard="true"]:checked {background:#e5f0fc;border-color:#72a8d7;}
        QDialog#connectionDialog QLabel#connectionTitle {font-weight:600;font-size:15px;}
    )");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    pages_=new QStackedWidget;pages_->setObjectName("connectionWizardPages");layout->addWidget(pages_);
    auto* picker=new QWidget;auto* pickerLayout=new QVBoxLayout(picker);pickerLayout->setContentsMargins(0,0,0,0);pickerLayout->setSpacing(0);pages_->addWidget(picker);
    auto* body=new QWidget;auto* bodyLayout=new QHBoxLayout(body);bodyLayout->setContentsMargins(0,0,0,0);bodyLayout->setSpacing(0);pickerLayout->addWidget(body,1);
    auto* filterScroll = new QScrollArea;
    filterScroll->setWidgetResizable(true);
    filterScroll->setFrameShape(QFrame::NoFrame);
    filterScroll->setFixedWidth(190);
    auto* filters=new QWidget;filters->setObjectName("connectionFilters");auto* filterLayout=new QVBoxLayout(filters);filterLayout->setContentsMargins(22,28,16,20);filterLayout->setSpacing(15);filterLayout->addWidget(new QLabel("连接筛选"));filterScroll->setWidget(filters);bodyLayout->addWidget(filterScroll);
    auto* content=new QWidget;auto* contentLayout=new QVBoxLayout(content);contentLayout->setContentsMargins(24,22,24,20);auto* pickerTitle=new QLabel("新建连接");pickerTitle->setObjectName("connectionTitle");pickerTitle->setAlignment(Qt::AlignCenter);contentLayout->addWidget(pickerTitle);
    auto* heading=new QHBoxLayout;heading->setContentsMargins(0,24,0,8);heading->addWidget(new QLabel("选择一个连接类型："));heading->addStretch();auto* search=new navidog::ui::TextInput;search->setObjectName("connectionTypeSearch");search->setPlaceholderText("搜索连接类型");search->setClearButtonEnabled(true);search->setMaximumWidth(180);heading->addWidget(search);contentLayout->addLayout(heading);
    auto* cardScroll = new QScrollArea;
    cardScroll->setWidgetResizable(true);
    cardScroll->setFrameShape(QFrame::NoFrame);
    auto* cardPanel = new QWidget;
    auto* grid=new QGridLayout(cardPanel);
    grid->setAlignment(Qt::AlignTop);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(18);
    cardScroll->setWidget(cardPanel);
    contentLayout->addWidget(cardScroll,1);empty_=new QLabel("没有匹配的连接类型");empty_->setAlignment(Qt::AlignCenter);empty_->hide();contentLayout->addWidget(empty_);bodyLayout->addWidget(content,1);
    QVector<navidog::ui::Button*> cards;QVector<navidog::ui::CheckBox*> checks;
    for(int i=0;i<kinds.size();++i){const auto kind=kinds[i];auto* check=new navidog::ui::CheckBox(navidog::databaseKindName(kind));check->setChecked(true);filterLayout->addWidget(check);checks<<check;
        auto* card=new navidog::ui::Button(navidog::databaseKindName(kind),"choose_"+navidog::databaseKindKey(kind));card->setProperty("connectionCard",true);card->setIcon(navidog::ui::databaseIcon(kind));card->setIconSize(QSize(48,48));card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);card->setCheckable(true);card->setAutoExclusive(true);card->setMinimumSize(105,108);card->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);grid->addWidget(card,i/5,i%5);cards<<card;connect(card,&QToolButton::toggled,this,[this,kind](bool checked){if(checked && kind_!=kind && !(navidog::isOceanBase(kind_) && navidog::isOceanBase(kind)))chooseDriver(kind);});}
    filterLayout->addStretch();
    auto filter=[this,search,cards,checks,grid]{int count=0;for(int i=0;i<cards.size();++i){auto* card=cards[i];grid->removeWidget(card);bool show=checks[i]->isChecked() && card->text().contains(search->text().trimmed(),Qt::CaseInsensitive);card->setVisible(show);if(show){grid->addWidget(card,count/5,count%5);++count;}}empty_->setVisible(count==0);};
    connect(search,&QLineEdit::textChanged,this,filter);for(auto* check:checks)connect(check,&QCheckBox::toggled,this,filter);
    auto* pickerFooter=new QHBoxLayout;pickerFooter->setContentsMargins(20,12,20,12);pickerFooter->addStretch();auto* cancel=new navidog::ui::PushButton("取消");pickerFooter->addWidget(cancel);next_=new navidog::ui::PushButton("下一步");next_->setObjectName("connectionNext");next_->setProperty("primary",true);next_->setEnabled(false);pickerFooter->addWidget(next_);pickerLayout->addLayout(pickerFooter);connect(cancel,&QPushButton::clicked,this,&ConnectionDialog::reject);connect(next_,&QPushButton::clicked,this,&ConnectionDialog::showSettings);

    auto* settings=new QWidget;auto* settingsLayout=new QVBoxLayout(settings);settingsLayout->setContentsMargins(20,18,20,12);settingsLayout->setSpacing(12);pages_->addWidget(settings);
    title_=new QLabel;title_->setObjectName("connectionTitle");title_->setAlignment(Qt::AlignCenter);settingsLayout->addWidget(title_);
    tabs_=new QTabWidget;tabs_->setObjectName("connectionSettingsTabs");settingsLayout->addWidget(tabs_,1);
    generalPage_=new QWidget;auto* generalLayout=new QVBoxLayout(generalPage_);generalLayout->setContentsMargins(0,8,0,0);generalLayout->addWidget(new ConnectionDiagram(generalPage_));auto* fields=centeredForm(general_,generalPage_);addCentered(generalLayout,fields);generalLayout->addStretch();
    name_=new navidog::ui::TextInput;name_->setObjectName("connectionName");host_=new navidog::ui::TextInput;host_->setObjectName("connectionHost");port_=new navidog::ui::NumberInput;port_->setObjectName("connectionPort");port_->setRange(1,65535);port_->setFixedWidth(90);
    database_=new navidog::ui::TextInput;database_->setObjectName("databasePath");user_=new navidog::ui::TextInput;user_->setObjectName("connectionUser");password_=new navidog::ui::TextInput;password_->setObjectName("connectionPassword");password_->setEchoMode(QLineEdit::Password);
    passwordless_=new navidog::ui::CheckBox("此连接无需密码");passwordless_->setObjectName("connectionPasswordless");path_=new QWidget;auto* pathLayout=new QHBoxLayout(path_);pathLayout->setContentsMargins(0,0,0,0);pathLayout->setSpacing(6);pathLayout->addWidget(database_);browse_=new navidog::ui::PushButton("…");browse_->setObjectName("browseDatabase");browse_->setFixedWidth(34);pathLayout->addWidget(browse_);
    oceanBaseMode_=new navidog::ui::Select;
    oceanBaseMode_->setObjectName("oceanBaseTenantMode");
    oceanBaseMode_->addItem("MySQL 模式",QVariant::fromValue(K::OceanBase));
    oceanBaseMode_->addItem("Oracle 模式",QVariant::fromValue(K::OceanBaseOracle));
    general_->addRow("连接名称：",name_);general_->addRow("主机：",host_);general_->addRow("端口：",port_);databaseLabel_=new QLabel;general_->addRow(databaseLabel_,path_);general_->addRow("用户名：",user_);general_->addRow("密码：",password_);general_->addRow(passwordless_);
    extra_=new ConnectionOptionsEditor(this);for(auto* page:{extra_->advancedPage,extra_->sslPage,extra_->sshPage,extra_->httpPage,extra_->notesPage})page->hide();advancedPage_=extra_->advancedPage;auto* advanced=extra_->advancedForm;sslPage_=extra_->sslPage;tls_=extra_->tls;tls_->setObjectName("connectionTls");
    extra_->outputContext = [this] {
        navidog::ConnectionProfile value;
        value.name = name_->text();
        value.driver = kind_;
        value.database = database_->text();
        return navidog::ui::exportContext(value);
    };
    general_->addRow(extra_->savePassword);general_->insertRow(1,"连接类型：",extra_->oracleAddress);oracleIdentity_=new QWidget;auto* oracleIdentityLayout=new QHBoxLayout(oracleIdentity_);oracleIdentityLayout->setContentsMargins(0,0,0,0);oracleIdentityLayout->addWidget(extra_->serviceName);oracleIdentityLayout->addWidget(extra_->sid);oracleIdentityLayout->addStretch();general_->insertRow(5,oracleIdentity_);general_->insertRow(6,"角色：",extra_->oracleRole);general_->insertRow(7,extra_->externalAuthentication);
    general_->insertRow(1,"租户模式：",oceanBaseMode_);
    h2Mode_=new navidog::ui::Select;
    h2Mode_->setObjectName("h2ConnectionMode");
    h2Mode_->addItems({"TCP 服务", "本地文件"});
    general_->insertRow(1,"H2 连接方式：",h2Mode_);
    connect(h2Mode_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{
        if(kind_==K::H2){
            database_->clear();
            updateDriver();
        }
    });
    connect(oceanBaseMode_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index){
        if(index<0 || !navidog::isOceanBase(kind_)) return;
        const auto target=oceanBaseMode_->itemData(index).value<K>();
        if(target!=kind_) chooseDriver(target);
    });
    group_=new navidog::ui::Select;group_->setObjectName("connectionGroup");group_->addItem("我的连接",QString{});for(const auto& g:groups)group_->addItem(g.name,g.id);advanced->addRow("所属分组：",group_);
    version_=new navidog::ui::Select;version_->setObjectName("versionSelect");advanced->addRow("服务端版本：",version_);readOnly_=new navidog::ui::CheckBox("使用只读会话");readOnly_->setObjectName("connectionReadOnly");advanced->addRow(readOnly_);
    mongoAuthDatabase_ = new navidog::ui::TextInput;
    mongoAuthDatabase_->setObjectName("mongoAuthDatabase");
    mongoAuthDatabase_->setPlaceholderText("admin");
    advanced->addRow("认证数据库", mongoAuthDatabase_);
    driverNote_=new QLabel;driverNote_->setObjectName("driverStatus");driverNote_->setWordWrap(true);driverNote_->setTextFormat(Qt::PlainText);advanced->addRow("官方驱动：",driverNote_);
    databasePage_=new QWidget;auto* dbLayout=new QVBoxLayout(databasePage_);dbLayout->setContentsMargins(0,40,0,0);QFormLayout* dbForm;auto* dbFields=centeredForm(dbForm,databasePage_);addCentered(dbLayout,dbFields);initialDatabase_=new navidog::ui::TextInput;initialDatabase_->setObjectName("initialDatabase");initialDatabase_->setPlaceholderText("可留空，连接后浏览数据库");dbForm->addRow("初始数据库：",initialDatabase_);dbLayout->addStretch();
    result_=new QLabel;result_->setObjectName("connectionTestResult");result_->setWordWrap(true);result_->setTextFormat(Qt::PlainText);result_->setTextInteractionFlags(Qt::TextSelectableByMouse);result_->setMaximumHeight(85);settingsLayout->addWidget(result_);
    auto* footer=new QHBoxLayout;test_=new navidog::ui::PushButton("测试连接");test_->setObjectName("testConnection");footer->addWidget(test_);footer->addStretch();auto* settingsCancel=new navidog::ui::PushButton("取消");footer->addWidget(settingsCancel);connect(settingsCancel,&QPushButton::clicked,this,&ConnectionDialog::reject);back_=new navidog::ui::PushButton("上一步");back_->setObjectName("connectionBack");back_->setVisible(!editing);footer->addWidget(back_);buttons_=new navidog::ui::FormButtons;auto* unusedCancel=buttons_->button(QDialogButtonBox::Cancel);buttons_->removeButton(unusedCancel);delete unusedCancel;buttons_->button(QDialogButtonBox::Save)->setText("好");buttons_->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);footer->addWidget(buttons_);settingsLayout->addLayout(footer);
    auto* drivers = new navidog::ui::PushButton("驱动与运行时…");
    drivers->setObjectName("manageConnectionDriver");
    footer->insertWidget(1, drivers);
    connect(drivers, &QPushButton::clicked, this, [this] {
        if (testing_) return;
        showDriverManager(this, kind_);
        for (const auto& entry : navidog::driverRegistry()) if (entry.id == kind_) {
            driverNote_->setText(entry.provider + "\n" + entry.status);
            test_->setEnabled(entry.ready);
        }
    });
    connect(back_,&QPushButton::clicked,this,&ConnectionDialog::showPicker);connect(passwordless_,&QCheckBox::toggled,this,[this](bool checked){password_->setEnabled(!checked && kind_!=K::SQLite && !(kind_==K::Oracle && extra_->externalAuthentication->isChecked()));if(checked)extra_->savePassword->setChecked(false);extra_->savePassword->setEnabled(!checked && kind_!=K::SQLite && navidog::localSecretStoreAvailable());});
    chooseDriver(original.driver);if(chooseTypeFirst && !editing){kind_=K::Unknown;for(auto* card:cards){card->setAutoExclusive(false);card->setChecked(false);card->setAutoExclusive(true);}next_->setEnabled(false);showPicker();}else showSettings();
    connect(browse_,&QPushButton::clicked,this,[this]{auto file=QFileDialog::getOpenFileName(this,kind_==K::H2?"选择 H2 数据库":kind_==K::DuckDb?"选择 DuckDB 数据库":"选择 SQLite 数据库",database_->text(),kind_==K::H2?"H2 (*.mv.db)":kind_==K::DuckDb?"DuckDB (*.duckdb *.db);;所有文件 (*)":"SQLite (*.db *.sqlite *.sqlite3);;所有文件 (*)");if(!file.isEmpty())database_->setText(file);});
    connect(buttons_,&QDialogButtonBox::rejected,this,&ConnectionDialog::reject);connect(buttons_,&QDialogButtonBox::accepted,this,[this]{if(validate())accept();});
    connect(test_,&QPushButton::clicked,this,[this]{if(!validate(false))return;testId_=QUuid::createUuid().toString();if(service_.testConnection(profile(),testId_)){setTesting(true);result_->setStyleSheet({});result_->setText("正在测试连接…");}else result_->setText("正在处理其他数据库操作，请稍后重试。");});
    connect(&service_,&navidog::QueryService::connectionTestCompleted,this,[this](const QString& id,const navidog::QueryOutcome& outcome){if(id!=testId_)return;setTesting(false);if(!outcome.sshHostKey.isEmpty()){auto p=profile();auto warning=QString("SSH 服务器：%1:%2\n指纹：%3\n\n请通过可信渠道核对指纹。%4").arg(p.ssh.host).arg(p.ssh.port).arg(outcome.sshFingerprint,outcome.sshHostKeyMismatch?"已保存的密钥发生变化。是否替换并重新测试？":"是否信任此密钥并重新测试？");if(QMessageBox::warning(this,"核对 SSH 主机密钥",warning,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Yes){extra_->hostKey=outcome.sshHostKey;test_->click();}return;}if(outcome.result.cancelled){result_->setText("测试已停止");return;}const bool ok=outcome.result.error.isEmpty();result_->setStyleSheet(ok?"color:#24734b":"color:#a63d32");result_->setText(ok?QString("连接成功 · %1 · %2 ms").arg(outcome.serverVersion).arg(outcome.result.elapsedMs):"连接失败：\n"+outcome.result.error);});
    extra_->changed=[this]{if(!testing_)result_->clear();bool oracle=kind_==K::Oracle;bool tns=oracle && extra_->oracleAddress->currentIndex()==1;host_->setEnabled(!tns);port_->setEnabled(!tns);oracleIdentity_->setEnabled(!tns);if(oracle)databaseLabel_->setText(tns?"网络服务名：":extra_->sid->isChecked()?"SID：":"服务名称：");bool external=oracle && extra_->externalAuthentication->isChecked();user_->setEnabled(!external);password_->setEnabled(!external && !passwordless_->isChecked());extra_->savePassword->setEnabled(!external && !passwordless_->isChecked() && kind_!=K::SQLite && navidog::localSecretStoreAvailable());};
    extra_->changed();
    auto invalidate=[this]{if(!testing_)result_->clear();};for(auto* edit:{host_,database_,initialDatabase_,user_,password_})connect(edit,&QLineEdit::textChanged,this,invalidate);connect(port_,QOverload<int>::of(&QSpinBox::valueChanged),this,invalidate);connect(tls_,&QCheckBox::toggled,this,invalidate);connect(readOnly_,&QCheckBox::toggled,this,invalidate);connect(passwordless_,&QCheckBox::toggled,this,invalidate);
}
void ConnectionDialog::chooseDriver(K kind) {
    if(kind==K::Unknown)return;
    if(kind_!=K::Unknown)drafts_[int(kind_)]=profile();
    auto p=drafts_.value(int(kind),original_);
    if(!drafts_.contains(int(kind)) && navidog::isOceanBase(kind) && navidog::isOceanBase(kind_)) {
        p=drafts_.value(int(kind_));
        p.driver=kind;
        p.database.clear();
        p.user.clear();
        p.password.clear();
        p.advanced.initialSql.clear();
        p.advanced.socketFile.clear();
        p.advanced.charset.clear();
        p.advanced.keepAlive=false;
        p.advanced.compression=false;
        p.ssl.serverName.clear();
        p.http={};
        p.readOnly=false;
        p.savePassword=false;
        p.credentialSource=navidog::CredentialSource::Prompt;
    } else if(!drafts_.contains(int(kind)) && kind!=original_.driver){p.driver=kind;p.database.clear();p.password.clear();p.advanced={};p.ssl={};p.ssh={};p.http={};p.oracle={};p.jdbcDriverJar.clear();p.h2File=false;p.mongoAuthDatabase.clear();p.savePassword=false;if(kind==K::SQLServer || kind==K::Oracle)p.ssl.verification=navidog::SslVerification::Identity;p.version=navidog::DatabaseVersion::Auto;p.user=(kind==K::MySQL || kind==K::MariaDB || kind==K::TiDB)?"root":navidog::usesNativePostgres(kind)?"postgres":kind==K::H2?"sa":"";p.credentialSource=navidog::CredentialSource::Prompt;for(const auto& d:navidog::driverRegistry())if(d.id==kind)p.port=d.defaultPort?d.defaultPort:5432;if(navidog::usesNativePostgres(kind))p.database="postgres";if(kind==K::Oracle)p.database="ORCL";
        if(kind==K::ClickHouse){p.user="default";p.database="default";p.tls=false;}
        if(kind==K::MongoDb)p.tls=false;
        if(kind==K::CockroachDb){p.user="root";p.database="defaultdb";}
        if(kind==K::YugabyteDb){p.user="yugabyte";p.database="yugabyte";}
        if(kind==K::OpenGauss)p.database="postgres";
        if(navidog::isAnalyticalMySql(kind))p.user="root";
        if(kind==K::DuckDb)p.tls=false;if(navidog::usesJdbcDriver(kind) || kind==K::Redis)p.tls=false;}
    kind_=kind;
    {QSignalBlocker block(h2Mode_);h2Mode_->setCurrentIndex(p.h2File?1:0);}
    updateDriver();extra_->setProfile(p);mongoAuthDatabase_->setText(p.mongoAuthDatabase);name_->setText(p.name);host_->setText(p.host);port_->setValue(p.port);database_->setText(p.database);initialDatabase_->setText(p.database);user_->setText(p.user);password_->setText(p.password);version_->setCurrentIndex(qMax(0,version_->findData(QVariant::fromValue(p.version))));group_->setCurrentIndex(qMax(0,group_->findData(p.groupId)));tls_->setChecked(p.tls);readOnly_->setChecked(p.readOnly && readOnly_->isEnabled());passwordless_->setChecked(p.credentialSource==navidog::CredentialSource::Passwordless);password_->setEnabled(!passwordless_->isChecked());next_->setEnabled(true);
    const auto cardKind=navidog::isOceanBase(kind)?K::OceanBase:kind;
    for(auto* card:findChildren<QToolButton*>())if(card->property("connectionCard").toBool())card->setChecked(card->objectName()=="choose_"+navidog::databaseKindKey(cardKind));
}
void ConnectionDialog::updateDriver() {
    const bool h2=kind_==K::H2, h2File=h2 && h2Mode_->currentIndex()==1;
    const bool local=navidog::isFileDatabase(kind_),dbTab=navidog::isMySqlFamily(kind_) ||
        navidog::isAnalyticalMySql(kind_) || kind_==K::SQLServer;
    mongoAuthDatabase_->setVisible(kind_==K::MongoDb);
    extra_->advancedForm->labelForField(mongoAuthDatabase_)->setVisible(kind_==K::MongoDb);
    {
        QSignalBlocker block(oceanBaseMode_);
        oceanBaseMode_->setCurrentIndex(kind_==K::OceanBaseOracle?1:0);
        oceanBaseMode_->setVisible(navidog::isOceanBase(kind_));
        general_->labelForField(oceanBaseMode_)->setVisible(navidog::isOceanBase(kind_));
    }
    h2Mode_->setVisible(h2);
    general_->labelForField(h2Mode_)->setVisible(h2);
    user_->setPlaceholderText(navidog::isOceanBase(kind_)?"用户@租户 或 用户@租户#集群":QString{});
    title_->setText(QString(editing_?"编辑连接（%1）":"新建连接（%1）").arg(navidog::databaseKindName(kind_)));
    while(tabs_->count())tabs_->removeTab(0);tabs_->addTab(generalPage_,"常规");tabs_->addTab(advancedPage_,"高级");if(dbTab)tabs_->addTab(databasePage_,"数据库");if(!local && kind_!=K::Redis && !navidog::usesJdbcCatalog(kind_))tabs_->addTab(sslPage_,"SSL");if(!local && !h2File)tabs_->addTab(extra_->sshPage,"SSH");if(navidog::isMySqlFamily(kind_) || navidog::usesNativePostgres(kind_) || kind_==K::SQLite)tabs_->addTab(extra_->httpPage,"HTTP");tabs_->addTab(extra_->notesPage,"备注");extra_->updateKind(kind_);
    version_->clear();version_->addItem("自动检测",QVariant::fromValue(navidog::DatabaseVersion::Auto));for(const auto& v:navidog::versionsFor(kind_))version_->addItem(v.name,QVariant::fromValue(v.version));
    for(QWidget* field:{static_cast<QWidget*>(host_),static_cast<QWidget*>(port_),static_cast<QWidget*>(user_),static_cast<QWidget*>(password_),static_cast<QWidget*>(passwordless_)}){const bool visible=!local && !(h2File && (field==host_ || field==port_));
        field->setVisible(visible);
        if(auto* label=general_->labelForField(field))label->setVisible(visible);}
    const bool initialPath=!dbTab && !(kind_==K::OceanBaseOracle || kind_==K::Dameng);
    path_->setVisible(initialPath);databaseLabel_->setVisible(initialPath);browse_->setVisible(local || h2File);databaseLabel_->setText((local || h2File)?"数据库文件：":kind_==K::Oracle?"服务名称：":kind_==K::Redis?"数据库编号：":"初始数据库：");database_->setPlaceholderText(h2File?"选择现有 .mv.db 文件":kind_==K::DuckDb?"选择现有 DuckDB 文件":local?"选择现有 SQLite 数据库文件":kind_==K::Oracle?"例如 FREEPDB1":kind_==K::Redis?"默认 0":navidog::usesJdbcCatalog(kind_)?"输入数据库名称":"postgres");
    readOnly_->setEnabled(!navidog::usesSchemaAtConnection(kind_) && kind_!=K::SQLServer && !navidog::isCommandDatabase(kind_) && kind_!=K::ClickHouse);if(!readOnly_->isEnabled())readOnly_->setChecked(false);
    for(QWidget* field:{static_cast<QWidget*>(extra_->oracleAddress),static_cast<QWidget*>(extra_->oracleRole),static_cast<QWidget*>(extra_->externalAuthentication),oracleIdentity_}){field->setVisible(kind_==K::Oracle);if(auto* label=general_->labelForField(field))label->setVisible(kind_==K::Oracle);}extra_->savePassword->setVisible(kind_!=K::SQLite);

    for(const auto& d:navidog::driverRegistry())if(d.id==kind_){driverNote_->setText(d.provider+"\n"+d.status);test_->setEnabled(d.ready);}
    database_->setReadOnly(original_.id=="harbor-demo");browse_->setEnabled(original_.id!="harbor-demo");back_->setEnabled(original_.id!="harbor-demo");result_->clear();
}
void ConnectionDialog::showSettings(){if(kind_==K::Unknown)return;pages_->setCurrentIndex(1);name_->setFocus();buttons_->button(QDialogButtonBox::Save)->setDefault(true);}
void ConnectionDialog::showPicker(){if(testing_)return;pages_->setCurrentIndex(0);buttons_->button(QDialogButtonBox::Save)->setDefault(false);next_->setDefault(true);}
navidog::ConnectionProfile ConnectionDialog::profile() const {
    auto p=original_;extra_->apply(p);p.name=name_->text().trimmed();p.groupId=group_->currentData().toString();p.driver=kind_;p.h2File=kind_==K::H2 && h2Mode_->currentIndex()==1;p.version=version_->currentData().value<navidog::DatabaseVersion>();p.host=host_->text().trimmed();p.port=port_->value();p.database=(navidog::isMySqlFamily(kind_) || navidog::isAnalyticalMySql(kind_) || kind_==K::SQLServer?initialDatabase_:database_)->text().trimmed();p.mongoAuthDatabase=kind_==K::MongoDb?mongoAuthDatabase_->text().trimmed():QString{};p.user=user_->text().trimmed();p.password=password_->text();p.tls=tls_->isChecked();p.readOnly=readOnly_->isEnabled() && readOnly_->isChecked();p.savePassword=p.savePassword && !passwordless_->isChecked() && !(p.driver==K::Oracle && p.oracle.externalAuthentication);p.credentialSource=passwordless_->isChecked()?navidog::CredentialSource::Passwordless:(p.savePassword || p.ssh.savePassword || p.ssh.savePassphrase || p.http.savePassword || p.ssl.saveDamengKeystorePassword)?navidog::CredentialSource::LocalEncrypted:navidog::CredentialSource::Prompt;p.secretsLoaded=original_.secretsLoaded || password_->isModified() || p.password!=original_.password || p.ssh.password!=original_.ssh.password || p.ssh.passphrase!=original_.ssh.passphrase || p.http.password!=original_.http.password || p.ssl.damengKeystorePassword!=original_.ssl.damengKeystorePassword;p.credentialContainer.clear();p.credentialVariable.clear();if(p.driver==K::OceanBaseOracle || p.driver==K::Dameng)p.database.clear();if(p.driver!=original_.driver){p.detectedVersion=navidog::DatabaseVersion::Unknown;p.serverVersion.clear();}if(navidog::usesNativePostgres(p.driver) && p.database.isEmpty())p.database="postgres";if(p.h2File){p.ssh.enabled=false;if(!p.database.isEmpty())p.database=QFileInfo(p.database).absoluteFilePath();}if(navidog::usesJdbcCatalog(p.driver) || p.driver==K::Redis)p.tls=false;if(navidog::isFileDatabase(p.driver) && !p.http.enabled && !p.database.isEmpty())p.database=QFileInfo(p.database).absoluteFilePath();return p;
}
bool ConnectionDialog::validate(bool requireName) {
    auto p=profile();QString error;QWidget* focus=nullptr;
    if(requireName && p.name.isEmpty()){error="请填写连接名称。";focus=name_;}else if(kind_==K::Unknown){showPicker();return false;}else if(p.driver==K::SQLite && !p.http.enabled && (database_->text().trimmed().isEmpty() || !QFileInfo(p.database).isFile())){error="请选择已存在的 SQLite 数据库文件。";focus=database_;}else if(!navidog::isFileDatabase(p.driver) && !p.h2File && p.host.isEmpty() && !(p.driver==K::Oracle && p.oracle.addressKind==navidog::OracleAddressKind::TNS)){error="请填写主机。";focus=host_;}else if(p.driver==K::Oracle && p.database.isEmpty()){error="请填写 Oracle 服务名称。";focus=database_;}
    if(error.isEmpty()){try{navidog::validateConnectionOptions(p);return true;}catch(const std::exception& e){error=QString::fromUtf8(e.what());}}tabs_->setCurrentWidget(p.driver==K::Dameng && error.contains(".keystore")?sslPage_:navidog::usesJdbcDriver(p.driver) && error.contains("JDBC JAR")?advancedPage_:generalPage_);result_->setStyleSheet("color:#a63d32");result_->setText(error);if(focus)focus->setFocus();return false;
}
void ConnectionDialog::setTesting(bool value){testing_=value;tabs_->setEnabled(!value);back_->setEnabled(!value && original_.id!="harbor-demo");buttons_->button(QDialogButtonBox::Save)->setEnabled(!value);test_->setEnabled(!value);}
void ConnectionDialog::reject(){if(testing_)service_.cancel();Dialog::reject();}
