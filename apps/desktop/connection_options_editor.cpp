#include "connection_options_editor.hpp"
#include "navidog/ssh_tunnel.hpp"
#include "navidog/secret_store.hpp"
#include "navidog/credentials.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QMessageBox>
#include "navidog/http_tunnel.hpp"
namespace {
using K=navidog::DatabaseKind;
QWidget* formPage(QFormLayout*& form,QWidget* parent,bool scroll=false){auto* page=new QWidget(parent);auto* outer=new QVBoxLayout(page);outer->setContentsMargins(24,20,24,10);auto* content=new QWidget;auto* column=new QVBoxLayout(content);column->setContentsMargins(0,0,0,0);auto* fields=new QWidget;fields->setMaximumWidth(690);fields->setMinimumWidth(520);form=new QFormLayout(fields);form->setLabelAlignment(Qt::AlignRight|Qt::AlignVCenter);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setSpacing(10);auto* center=new QHBoxLayout;center->addStretch();center->addWidget(fields,1);center->addStretch();column->addLayout(center);column->addStretch();if(scroll){auto* area=new QScrollArea;area->setWidgetResizable(true);area->setFrameShape(QFrame::NoFrame);area->setWidget(content);outer->addWidget(area);}else outer->addWidget(content);return page;}
}
void ConnectionOptionsEditor::notify(){if(changed)changed();}
void ConnectionOptionsEditor::watch(QWidget* widget){if(auto* e=qobject_cast<QLineEdit*>(widget))connect(e,&QLineEdit::textChanged,this,[this]{notify();});else if(auto* c=qobject_cast<QComboBox*>(widget))connect(c,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{updateEnabled();notify();});else if(auto* s=qobject_cast<QSpinBox*>(widget))connect(s,QOverload<int>::of(&QSpinBox::valueChanged),this,[this]{notify();});else if(auto* b=qobject_cast<QCheckBox*>(widget))connect(b,&QCheckBox::toggled,this,[this]{updateEnabled();notify();});}
navidog::ui::TextInput* ConnectionOptionsEditor::input(QFormLayout* form,QString key,QString label,bool password){auto* edit=new navidog::ui::TextInput;edit->setObjectName("connection_"+key);if(password)edit->setEchoMode(QLineEdit::Password);text_[key]=edit;form->addRow(label,edit);watch(edit);return edit;}
navidog::ui::TextInput* ConnectionOptionsEditor::file(QFormLayout* form,QString key,QString label,bool directory){auto* row=new QWidget;auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->setSpacing(4);auto* edit=new navidog::ui::TextInput;edit->setObjectName("connection_"+key);text_[key]=edit;layout->addWidget(edit,1);auto* browse=new navidog::ui::PushButton("…");browse->setFixedWidth(34);layout->addWidget(browse);form->addRow(label,row);rows_[key]=row;watch(edit);connect(browse,&QPushButton::clicked,this,[edit,directory,row]{auto path=directory?QFileDialog::getExistingDirectory(row,"选择文件夹",edit->text()):QFileDialog::getOpenFileName(row,"选择文件",edit->text());if(!path.isEmpty())edit->setText(path);});return edit;}
navidog::ui::CheckBox* ConnectionOptionsEditor::check(QFormLayout* form,QString key,QString label){auto* value=new navidog::ui::CheckBox(label);value->setObjectName("connection_"+key);check_[key]=value;form->addRow(value);watch(value);return value;}
navidog::ui::NumberInput* ConnectionOptionsEditor::number(QFormLayout* form,QString key,QString label,int min,int max){auto* value=new navidog::ui::NumberInput;value->setObjectName("connection_"+key);value->setRange(min,max);value->setMaximumWidth(110);number_[key]=value;form->addRow(label,value);watch(value);return value;}
navidog::ui::Select* ConnectionOptionsEditor::select(QFormLayout* form,QString key,QString label,const QStringList& values,bool editable){auto* value=new navidog::ui::Select;value->setObjectName("connection_"+key);value->setEditable(editable);value->addItems(values);select_[key]=value;form->addRow(label,value);watch(value);return value;}
ConnectionOptionsEditor::ConnectionOptionsEditor(QWidget* parent):QObject(parent){
    advancedPage=formPage(advancedForm,parent,true);
    select(advancedForm,"charset","客户端字符集：",{"utf8mb4"},true);
    check(advancedForm,"keepAlive","保持连接间隔（秒）");number(advancedForm,"keepAliveSeconds","",1,86400);
    number(advancedForm,"connectTimeout","连接超时（秒）：",1,600);
    check(advancedForm,"readTimeoutEnabled","读取 / 查询超时（秒）");number(advancedForm,"readTimeout","",1,86400);
    check(advancedForm,"writeTimeoutEnabled","写入超时（秒）");number(advancedForm,"writeTimeout","",1,86400);
    check(advancedForm,"compression","使用压缩");check(advancedForm,"autoConnect","自动连接");
    check(advancedForm,"useSocket","使用 Socket 文件");file(advancedForm,"socket","Socket 文件：");
    check(advancedForm,"limitSessions","限制连接会话");number(advancedForm,"maxSessions","",1,16);
    input(advancedForm,"applicationName","应用名称：");
    file(advancedForm,"jdbcDriverJar","JDBC 驱动 JAR：")->setPlaceholderText("选择数据库官方 JDBC JAR");
    file(advancedForm,"oracleClient","Oracle 客户端：",true);file(advancedForm,"oracleConfig","TNS 配置目录：",true);
    initialSql_=new navidog::ui::MultiLineInput;initialSql_->setObjectName("connectionInitialSql");initialSql_->setMaximumHeight(82);initialSql_->setPlaceholderText("建立连接后执行的 SQL");advancedForm->addRow("初始查询：",initialSql_);connect(initialSql_,&QPlainTextEdit::textChanged,this,[this]{notify();});
    QFormLayout* ssl;sslPage=formPage(ssl,parent,true);tls=check(ssl,"useTls","使用 SSL");
    file(ssl,"sslKey","客户端密钥：");file(ssl,"sslCertificate","客户端证书：");file(ssl,"sslCA","CA 证书：");select(ssl,"sslVerification","服务器验证：",{"加密连接","验证 CA 证书","验证 CA 和服务器主机名"});
    input(ssl,"sslServerName","服务器主机名：")->setPlaceholderText("默认使用数据库主机名");input(ssl,"sslCiphers","指定密码（Cipher）：")->setToolTip("TLS 密码套件列表，例如 ECDHE-RSA-AES256-GCM-SHA384");input(ssl,"tlsVersions","TLS 版本：")->setPlaceholderText("例如 TLSv1.2,TLSv1.3");file(ssl,"oracleWallet","Oracle 钱包目录：",true);input(ssl,"oracleServerDN","服务器证书 DN：");
    auto* damengDirectory = file(ssl,"damengKeystoreDirectory","证书目录：",true);
    damengDirectory->setPlaceholderText("包含 .keystore 的目录");
    input(ssl,"damengKeystorePassword","密钥库密码：",true)->setPlaceholderText("留空使用达梦默认密码");
    check(ssl,"saveDamengKeystorePassword","记住密钥库密码（本机加密）");
    auto* damengTlsHelp = new QLabel("达梦服务器需启用认证并加密的 SSL 模式（ENABLE_ENCRYPT 为 1 或 5）。SSL 可以和 SSH 隧道一起使用。");
    damengTlsHelp->setWordWrap(true);
    ssl->addRow(damengTlsHelp);
    rows_["damengTlsHelp"] = damengTlsHelp;
    QFormLayout* ssh;sshPage=formPage(ssh,parent,true);check(ssh,"useSsh","使用 SSH 隧道");input(ssh,"sshHost","主机：");number(ssh,"sshPort","端口：",1,65535);input(ssh,"sshUser","用户名：");select(ssh,"sshAuthentication","验证方法：",{"密码","公钥","密码和公钥","SSH Agent"});input(ssh,"sshPassword","密码：",true);check(ssh,"saveSshPassword","保存密码");file(ssh,"sshKey","私钥：");input(ssh,"sshPassphrase","密钥密码：",true);check(ssh,"saveSshPassphrase","保存密钥密码");file(ssh,"knownHosts","known_hosts 文件：");auto* clearKey=new navidog::ui::PushButton("清除已保存的主机密钥");clearKey->setObjectName("clearSshHostKey");ssh->addRow(clearKey);connect(clearKey,&QPushButton::clicked,this,[this]{resetHostKey();});
    QFormLayout* http;httpPage=formPage(http,parent,true);check(http,"useHttp","使用 HTTP 隧道");input(http,"httpUrl","隧道网址：");check(http,"httpAuthentication","使用验证");input(http,"httpUser","用户名：");input(http,"httpPassword","密码：",true);check(http,"saveHttpPassword","保存密码");check(http,"httpBase64","用 base64 编码传出查询");auto* script=new navidog::ui::PushButton("导出隧道脚本…");script->setObjectName("exportHttpTunnelScript");http->addRow(script);auto* bridgeHelp=new QLabel("上传到启用 PDO 的 PHP 8+ 服务器，并配置 NAVIDOG_TUNNEL_USER、NAVIDOG_TUNNEL_PASSWORD、NAVIDOG_DB_HOSTS。此隧道每次请求使用独立会话；交互式事务和表设计请使用直连或 SSH。");bridgeHelp->setWordWrap(true);http->addRow(bridgeHelp);connect(script,&QPushButton::clicked,this,[this,parent]{auto path=navidog::ui::chooseExportFile(parent,"导出 Navidog HTTP 隧道脚本",outputContext ? outputContext() : navidog::ui::ExportContext{},"HTTP隧道脚本","PHP (*.php)");if(path.isEmpty())return;try{QSaveFile file(path);auto bytes=navidog::httpTunnelScript();if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())throw std::runtime_error("无法保存隧道脚本");}catch(const std::exception& e){QMessageBox::warning(parent,"导出失败",QString::fromUtf8(e.what()));}});
    check(http,"useProxy","使用 HTTP 代理");input(http,"proxyHost","代理主机：");number(http,"proxyPort","代理端口：",1,65535);input(http,"proxyUser","代理用户名：");input(http,"proxyPassword","代理密码：",true);
    notesPage=new QWidget(parent);auto* notesLayout=new QVBoxLayout(notesPage);notesLayout->setContentsMargins(16,16,16,16);notes_=new navidog::ui::MultiLineInput;notes_->setObjectName("connectionNotes");notes_->setPlaceholderText("输入连接备注…");notesLayout->addWidget(notes_);connect(notes_,&QPlainTextEdit::textChanged,this,[this]{notify();});
    savePassword=new navidog::ui::CheckBox("记住密码（本机加密）");savePassword->setObjectName("saveConnectionPassword");savePassword->setEnabled(navidog::localSecretStoreAvailable());watch(savePassword);

    oracleAddress=new navidog::ui::Select;oracleAddress->setObjectName("oracleAddressKind");oracleAddress->addItems({"基本","TNS"});watch(oracleAddress);
    oracleRole=new navidog::ui::Select;oracleRole->setObjectName("oracleRole");oracleRole->addItems({"默认","SYSDBA","SYSOPER"});watch(oracleRole);
    externalAuthentication=new navidog::ui::CheckBox("操作系统验证");externalAuthentication->setObjectName("oracleExternalAuthentication");watch(externalAuthentication);
    serviceName=new QRadioButton("服务名称");sid=new QRadioButton("SID");serviceName->setChecked(true);connect(serviceName,&QRadioButton::toggled,this,[this]{notify();});
    connect(check_["useSsh"],&QCheckBox::toggled,this,[this](bool enabled){if(enabled)check_["useHttp"]->setChecked(false);});connect(check_["useHttp"],&QCheckBox::toggled,this,[this](bool enabled){if(enabled)check_["useSsh"]->setChecked(false);});
}
void ConnectionOptionsEditor::resetHostKey(){hostKey.clear();notify();}
void ConnectionOptionsEditor::setProfile(const navidog::ConnectionProfile& p){
    text_["jdbcDriverJar"]->setText(p.jdbcDriverJar);
    kind_=p.driver;const auto& a=p.advanced;const auto& s=p.ssl;const auto& t=p.ssh;const auto& h=p.http;
    number_["connectTimeout"]->setValue(a.connectTimeout);number_["readTimeout"]->setValue(qMax(1,a.readTimeout));number_["writeTimeout"]->setValue(qMax(1,a.writeTimeout));number_["keepAliveSeconds"]->setValue(a.keepAliveSeconds);number_["maxSessions"]->setValue(qMax(1,a.maxSessions));
    check_["keepAlive"]->setChecked(a.keepAlive);check_["readTimeoutEnabled"]->setChecked(a.readTimeout>0);check_["writeTimeoutEnabled"]->setChecked(a.writeTimeout>0);check_["compression"]->setChecked(a.compression);check_["autoConnect"]->setChecked(a.autoConnect);check_["limitSessions"]->setChecked(a.maxSessions>0);check_["useSocket"]->setChecked(!a.socketFile.isEmpty());text_["socket"]->setText(a.socketFile);text_["applicationName"]->setText(a.applicationName);initialSql_->setPlainText(a.initialSql);
    updateKind(p.driver);select_["charset"]->setCurrentText(navidog::connectionCharset(p));
    tls->setChecked(p.tls);select_["sslVerification"]->setCurrentIndex(int(s.verification));text_["sslKey"]->setText(s.keyFile);text_["sslCertificate"]->setText(s.certificateFile);text_["sslCA"]->setText(s.caFile);text_["sslServerName"]->setText(s.serverName);text_["sslCiphers"]->setText(s.ciphers);text_["tlsVersions"]->setText(s.tlsVersions);text_["oracleWallet"]->setText(s.walletDirectory);text_["oracleServerDN"]->setText(s.serverDN);
    text_["damengKeystoreDirectory"]->setText(s.damengKeystoreDirectory);
    text_["damengKeystorePassword"]->setText(s.damengKeystorePassword);
    check_["saveDamengKeystorePassword"]->setChecked(s.saveDamengKeystorePassword && navidog::hasRememberedSecrets(p));
    check_["useSsh"]->setChecked(t.enabled);text_["sshHost"]->setText(t.host);number_["sshPort"]->setValue(t.port);text_["sshUser"]->setText(t.user);select_["sshAuthentication"]->setCurrentIndex(int(t.authentication));text_["sshPassword"]->setText(t.password);text_["sshKey"]->setText(t.keyFile);text_["sshPassphrase"]->setText(t.passphrase);text_["knownHosts"]->setText(t.knownHostsFile);check_["saveSshPassword"]->setChecked(t.savePassword && navidog::hasRememberedSecrets(p));check_["saveSshPassphrase"]->setChecked(t.savePassphrase && navidog::hasRememberedSecrets(p));hostKey=t.hostKey;
    check_["useHttp"]->setChecked(h.enabled);text_["httpUrl"]->setText(h.url);check_["httpAuthentication"]->setChecked(h.authentication);text_["httpUser"]->setText(h.user);text_["httpPassword"]->setText(h.password);check_["saveHttpPassword"]->setChecked(h.savePassword && navidog::hasRememberedSecrets(p));check_["httpBase64"]->setChecked(h.base64);check_["useProxy"]->setChecked(h.proxyEnabled);text_["proxyHost"]->setText(h.proxyHost);number_["proxyPort"]->setValue(h.proxyPort);text_["proxyUser"]->setText(h.proxyUser);text_["proxyPassword"]->setText(h.proxyPassword);
    oracleAddress->setCurrentIndex(p.oracle.addressKind==navidog::OracleAddressKind::TNS?1:0);serviceName->setChecked(p.oracle.addressKind!=navidog::OracleAddressKind::SID);sid->setChecked(p.oracle.addressKind==navidog::OracleAddressKind::SID);oracleRole->setCurrentIndex(int(p.oracle.role));externalAuthentication->setChecked(p.oracle.externalAuthentication);text_["oracleClient"]->setText(p.oracle.clientDirectory);text_["oracleConfig"]->setText(p.oracle.configDirectory);savePassword->setChecked(p.savePassword && navidog::hasRememberedSecrets(p));notes_->setPlainText(p.notes);updateEnabled();
}
void ConnectionOptionsEditor::apply(navidog::ConnectionProfile& p)const {
    p.jdbcDriverJar=navidog::usesJdbcDriver(kind_)?text_["jdbcDriverJar"]->text().trimmed():QString{};
    auto& a=p.advanced;a.connectTimeout=number_["connectTimeout"]->value();a.readTimeout=check_["readTimeoutEnabled"]->isChecked()?number_["readTimeout"]->value():0;a.writeTimeout=check_["writeTimeoutEnabled"]->isChecked()?number_["writeTimeout"]->value():0;a.keepAlive=check_["keepAlive"]->isChecked();a.keepAliveSeconds=number_["keepAliveSeconds"]->value();a.maxSessions=check_["limitSessions"]->isChecked()?number_["maxSessions"]->value():0;a.compression=check_["compression"]->isChecked();a.autoConnect=check_["autoConnect"]->isChecked();auto charset=select_["charset"]->currentText();if(!a.charset.isEmpty() || charset!=navidog::connectionCharset(p))a.charset=charset;a.socketFile=check_["useSocket"]->isChecked()?text_["socket"]->text().trimmed():QString{};a.applicationName=text_["applicationName"]->text();a.initialSql=initialSql_->toPlainText();
    p.tls=tls->isChecked();auto& s=p.ssl;s.verification=navidog::SslVerification(select_["sslVerification"]->currentIndex());s.keyFile=text_["sslKey"]->text().trimmed();s.certificateFile=text_["sslCertificate"]->text().trimmed();s.caFile=text_["sslCA"]->text().trimmed();s.serverName=text_["sslServerName"]->text().trimmed();s.ciphers=text_["sslCiphers"]->text().trimmed();s.tlsVersions=text_["tlsVersions"]->text().trimmed();s.walletDirectory=text_["oracleWallet"]->text().trimmed();s.serverDN=text_["oracleServerDN"]->text().trimmed();
    s.damengKeystoreDirectory = kind_ == K::Dameng ? text_["damengKeystoreDirectory"]->text().trimmed() : QString{};
    s.damengKeystorePassword = kind_ == K::Dameng && p.tls ? text_["damengKeystorePassword"]->text() : QString{};
    s.saveDamengKeystorePassword = kind_ == K::Dameng && p.tls && check_["saveDamengKeystorePassword"]->isChecked();
    if(s.saveDamengKeystorePassword && s.damengKeystorePassword.isEmpty())
        s.damengKeystorePassword = "changeit";
    auto& t=p.ssh;t.enabled=check_["useSsh"]->isChecked();t.host=text_["sshHost"]->text().trimmed();t.port=number_["sshPort"]->value();t.user=text_["sshUser"]->text().trimmed();t.authentication=navidog::SshAuthentication(select_["sshAuthentication"]->currentIndex());t.password=text_["sshPassword"]->text();t.keyFile=text_["sshKey"]->text().trimmed();t.passphrase=text_["sshPassphrase"]->text();t.knownHostsFile=text_["knownHosts"]->text().trimmed();t.hostKey=hostKey;t.savePassword=check_["saveSshPassword"]->isChecked();t.savePassphrase=check_["saveSshPassphrase"]->isChecked();
    auto& h=p.http;h.enabled=check_["useHttp"]->isChecked();h.url=text_["httpUrl"]->text().trimmed();h.authentication=check_["httpAuthentication"]->isChecked();h.user=text_["httpUser"]->text();h.password=text_["httpPassword"]->text();h.savePassword=check_["saveHttpPassword"]->isChecked();h.base64=check_["httpBase64"]->isChecked();h.proxyEnabled=check_["useProxy"]->isChecked();h.proxyHost=text_["proxyHost"]->text().trimmed();h.proxyPort=number_["proxyPort"]->value();h.proxyUser=text_["proxyUser"]->text();h.proxyPassword=text_["proxyPassword"]->text();
    p.oracle.addressKind=oracleAddress->currentIndex()==1?navidog::OracleAddressKind::TNS:sid->isChecked()?navidog::OracleAddressKind::SID:navidog::OracleAddressKind::ServiceName;p.oracle.role=navidog::OracleRole(oracleRole->currentIndex());p.oracle.externalAuthentication=externalAuthentication->isChecked();p.oracle.clientDirectory=text_["oracleClient"]->text().trimmed();p.oracle.configDirectory=text_["oracleConfig"]->text().trimmed();p.savePassword=savePassword->isChecked();p.notes=notes_->toPlainText();
}
void ConnectionOptionsEditor::updateKind(K kind){kind_=kind;const bool mysql=navidog::isMySqlFamily(kind),pg=navidog::usesNativePostgres(kind),oracle=kind==K::Oracle,obOracle=kind==K::OceanBaseOracle,local=kind==K::SQLite;auto visible=[&](QFormLayout* form,QWidget* field,bool show){field->setVisible(show);if(auto* label=form->labelForField(field))label->setVisible(show);};
    for(auto key:{"keepAlive","readTimeoutEnabled","writeTimeoutEnabled","autoConnect","limitSessions"})visible(advancedForm,check_[key],!local && (QString(key)!="keepAlive" || pg || mysql) && (QString(key)!="writeTimeoutEnabled" || pg || mysql || obOracle));
    for(auto key:{"connectTimeout","keepAliveSeconds","readTimeout","writeTimeout","maxSessions"})visible(advancedForm,number_[key],!local && (QString(key)!="keepAliveSeconds" || pg || mysql) && (QString(key)!="writeTimeout" || pg || mysql || obOracle));
    visible(advancedForm,rows_["jdbcDriverJar"],navidog::usesJdbcDriver(kind));
    visible(advancedForm,check_["compression"],mysql);visible(advancedForm,check_["useSocket"],mysql);visible(advancedForm,rows_["socket"],mysql);visible(advancedForm,rows_["oracleClient"],oracle);visible(advancedForm,rows_["oracleConfig"],oracle);visible(advancedForm,text_["applicationName"],pg || kind==K::SQLServer);visible(advancedForm,select_["charset"],mysql || pg || oracle);
    auto* charset=select_["charset"];auto previous=charset->currentText();{QSignalBlocker block(charset);charset->clear();charset->addItems(mysql?QStringList{"utf8mb4","utf8mb3","latin1","gbk","gb18030","big5","sjis","ascii"}:pg?QStringList{"UTF8","LATIN1","WIN1252","GBK","GB18030","BIG5","SJIS"}:QStringList{"UTF8"});if(charset->findText(previous)>=0)charset->setCurrentText(previous);charset->setEditable(mysql || pg);}
    auto* ssl=qobject_cast<QFormLayout*>(text_["sslCiphers"]->parentWidget()->layout());
    const bool dameng = kind == K::Dameng;
    visible(ssl,rows_["damengKeystoreDirectory"],dameng);
    visible(ssl,text_["damengKeystorePassword"],dameng);
    visible(ssl,check_["saveDamengKeystorePassword"],dameng);
    visible(ssl,rows_["damengTlsHelp"],dameng);
    visible(ssl,select_["sslVerification"],!dameng);
    for(auto key:{"sslKey","sslCertificate","sslCA"}){auto* row=rows_[key];row->setVisible(!oracle && !dameng && kind!=K::SQLServer);if(auto* form=qobject_cast<QFormLayout*>(row->parentWidget()->layout()))if(auto* label=form->labelForField(row))label->setVisible(!oracle && !dameng && kind!=K::SQLServer);}
    for(auto key:{"oracleWallet"}){auto* row=rows_[key];row->setVisible(oracle);if(auto* form=qobject_cast<QFormLayout*>(row->parentWidget()->layout()))if(auto* label=form->labelForField(row))label->setVisible(oracle);}
    for(auto key:{"sslCiphers","tlsVersions","sslServerName","oracleServerDN"}){auto* field=text_[key];bool show=QString(key)=="sslCiphers"?mysql || obOracle:QString(key)=="tlsVersions"?mysql || pg || obOracle:QString(key)=="oracleServerDN"?oracle:!oracle && !obOracle;visible(ssl,field,show && !dameng);}
    updateEnabled();
}
void ConnectionOptionsEditor::updateEnabled(){if(check_.isEmpty() || !check_.contains("useProxy"))return;bool ssh=check_["useSsh"]->isChecked(),http=check_["useHttp"]->isChecked();check_["useSsh"]->setEnabled(kind_!=K::SQLite && navidog::sshAvailable() && !http);check_["useHttp"]->setEnabled((navidog::isMySqlFamily(kind_) || navidog::usesNativePostgres(kind_) || kind_==K::SQLite) && !ssh);
    number_["readTimeout"]->setEnabled(check_["readTimeoutEnabled"]->isChecked());number_["writeTimeout"]->setEnabled(check_["writeTimeoutEnabled"]->isChecked());number_["keepAliveSeconds"]->setEnabled(check_["keepAlive"]->isChecked());number_["maxSessions"]->setEnabled(check_["limitSessions"]->isChecked());rows_["socket"]->setEnabled(check_["useSocket"]->isChecked() && !ssh && !http);check_["useSocket"]->setEnabled(!ssh && !http);
    for(auto key:{"sslKey","sslCertificate","sslCA","oracleWallet"})rows_[key]->setEnabled(tls->isChecked() && !http);for(auto key:{"sslServerName","sslCiphers","tlsVersions","oracleServerDN"})text_[key]->setEnabled(tls->isChecked() && !http);select_["sslVerification"]->setEnabled(tls->isChecked() && !http);tls->setEnabled(!http);
    rows_["damengKeystoreDirectory"]->setEnabled(tls->isChecked());
    text_["damengKeystorePassword"]->setEnabled(tls->isChecked());
    check_["saveDamengKeystorePassword"]->setEnabled(tls->isChecked() && navidog::localSecretStoreAvailable());
    for(auto key:{"sshHost","sshUser","sshPassword","sshPassphrase","knownHosts","sshKey"})text_[key]->setEnabled(ssh);number_["sshPort"]->setEnabled(ssh);select_["sshAuthentication"]->setEnabled(ssh);int method=select_["sshAuthentication"]->currentIndex();bool password=method==0 || method==2,key=method==1 || method==2;text_["sshPassword"]->setEnabled(ssh && password);rows_["sshKey"]->setEnabled(ssh && key);text_["sshPassphrase"]->setEnabled(ssh && key);check_["saveSshPassword"]->setEnabled(ssh && password && navidog::localSecretStoreAvailable());check_["saveSshPassphrase"]->setEnabled(ssh && key && navidog::localSecretStoreAvailable());
    text_["httpUrl"]->setEnabled(http);check_["httpAuthentication"]->setEnabled(http);check_["httpBase64"]->setEnabled(http);bool auth=http && check_["httpAuthentication"]->isChecked();text_["httpUser"]->setEnabled(auth);text_["httpPassword"]->setEnabled(auth);check_["saveHttpPassword"]->setEnabled(auth && navidog::localSecretStoreAvailable());check_["useProxy"]->setEnabled(http);bool proxy=http && check_["useProxy"]->isChecked();for(auto key:{"proxyHost","proxyUser","proxyPassword"})text_[key]->setEnabled(proxy);number_["proxyPort"]->setEnabled(proxy);
}
