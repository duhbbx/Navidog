#pragma once
#include "components.hpp"
#include "navidog/query_service.hpp"
class QLabel;
class SqlReadOnlyView;
class QTableView;
class ResultModel;
class GenerationRuleModel;
class TableTaskDialog final : public navidog::ui::Dialog {
    Q_OBJECT
public:
    TableTaskDialog(const navidog::ConnectionProfile&,navidog::TableTask,navidog::QueryService&,QWidget* parent=nullptr);
    navidog::TableTask task() const;
    bool succeeded() const {return succeeded_;}
    const navidog::TableTaskResult& outcome() const{return outcome_;}
    void accept() override;
    void reject() override;
private:
    navidog::ConnectionProfile profile_;
    navidog::TableTask original_;
    navidog::QueryService& service_;
    navidog::TableTaskResult outcome_;
    QString requestId_,catalogId_,metadataId_,metadataTable_;
    bool running_=false,loading_=false,succeeded_=false,metadataReady_=true;
    QWidget* fields_;
    navidog::ui::Select *table_,*dimension_;
    navidog::ui::TextInput *grantee_,*granteeHost_;
    navidog::ui::CheckBox* revoke_;
    QVector<navidog::ui::CheckBox*> privileges_;
    GenerationRuleModel* rules_=nullptr;
    navidog::ui::TextInput *target_,*file_,*confirmation_;
    navidog::ui::NumberInput* rowLimit_;
    navidog::ui::CheckBox* data_;
    QDialogButtonBox* buttons_;
    SqlReadOnlyView* preview_;
    QLabel* message_;
    QTableView* result_;
    ResultModel* model_;
    void updatePreview();
    void loadTableMetadata();
};
