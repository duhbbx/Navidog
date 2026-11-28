#pragma once
#include "components.hpp"
#include "connection_options_editor.hpp"
#include "navidog/query_service.hpp"
#include <QLabel>
#include <QFormLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QMap>

// A draft wizard: testing creates an isolated connection; saving never connects.
class ConnectionDialog final : public navidog::ui::Dialog {
public:
    ConnectionDialog(const navidog::ConnectionProfile&,bool editing,const QVector<navidog::ConnectionGroup>&,navidog::QueryService&,QWidget* parent=nullptr,bool chooseTypeFirst=false);
    navidog::ConnectionProfile profile() const;
    void reject() override;
private:
    navidog::ConnectionProfile original_;
    navidog::QueryService& service_;
    QString testId_;
    bool testing_=false,editing_=false;
    navidog::DatabaseKind kind_=navidog::DatabaseKind::Unknown;
    QMap<int,navidog::ConnectionProfile> drafts_;
    navidog::ui::TextInput *name_,*host_,*database_,*initialDatabase_,*user_,*password_;
    navidog::ui::Select *version_,*group_,*oceanBaseMode_,*h2Mode_;
    navidog::ui::NumberInput* port_;
    navidog::ui::CheckBox *tls_,*readOnly_,*passwordless_;
    navidog::ui::PushButton *browse_,*test_,*back_,*next_;
    navidog::ui::FormButtons* buttons_;
    QLabel *title_,*driverNote_,*databaseLabel_,*result_,*empty_;
    QWidget *generalPage_,*advancedPage_,*databasePage_,*sslPage_,*path_;
    ConnectionOptionsEditor* extra_;
    QWidget* oracleIdentity_;
    QStackedWidget* pages_;
    QTabWidget* tabs_;
    QFormLayout* general_;
    void chooseDriver(navidog::DatabaseKind);
    void updateDriver();
    void showSettings();
    void showPicker();
    bool validate(bool requireName=true);
    void setTesting(bool);
};
