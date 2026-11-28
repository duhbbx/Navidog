#pragma once
#include "components.hpp"
#include "export_paths.hpp"
#include "navidog/types.hpp"
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QMap>
#include <functional>
class ConnectionOptionsEditor final:public QObject {
public:
    explicit ConnectionOptionsEditor(QWidget* parent);
    QWidget *advancedPage,*sslPage,*sshPage,*httpPage,*notesPage;
    QFormLayout* advancedForm;
    navidog::ui::CheckBox *tls,*savePassword,*externalAuthentication;
    navidog::ui::Select *oracleAddress,*oracleRole;
    QRadioButton *serviceName,*sid;
    QString hostKey;
    std::function<void()> changed;
    std::function<navidog::ui::ExportContext()> outputContext;
    void setProfile(const navidog::ConnectionProfile&);
    void apply(navidog::ConnectionProfile&)const;
    void updateKind(navidog::DatabaseKind);
    void resetHostKey();
private:
    navidog::DatabaseKind kind_=navidog::DatabaseKind::SQLite;
    QMap<QString,navidog::ui::TextInput*> text_;
    QMap<QString,navidog::ui::Select*> select_;
    QMap<QString,navidog::ui::NumberInput*> number_;
    QMap<QString,navidog::ui::CheckBox*> check_;
    QMap<QString,QWidget*> rows_;
    QPlainTextEdit *notes_,*initialSql_;
    navidog::ui::TextInput* input(QFormLayout*,QString key,QString label,bool password=false);
    navidog::ui::TextInput* file(QFormLayout*,QString key,QString label,bool directory=false);
    navidog::ui::CheckBox* check(QFormLayout*,QString key,QString label);
    navidog::ui::NumberInput* number(QFormLayout*,QString key,QString label,int min,int max);
    navidog::ui::Select* select(QFormLayout*,QString key,QString label,const QStringList&,bool editable=false);
    void watch(QWidget*);
    void updateEnabled();
    void notify();
};
