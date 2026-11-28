#pragma once
#include "components.hpp"
#include "navidog/query_service.hpp"
class QLabel;
class QFormLayout;
class SqlReadOnlyView;
class QTableView;
class QProgressBar;
class ResultModel;
class GenerationRuleModel;
namespace navidog { struct GenerationConfiguration; }
class TableTaskDialog final : public navidog::ui::Dialog {
    Q_OBJECT
public:
    TableTaskDialog(const navidog::ConnectionProfile&,navidog::TableTask,navidog::QueryService&,
                    QWidget* parent=nullptr, QString configurationStorePath={});
    navidog::TableTask task() const;
    bool succeeded() const {return succeeded_;}
    const navidog::TableTaskResult& outcome() const{return outcome_;}
    void accept() override;
    void reject() override;
signals:
    void backgroundStarted(const QString& id);
    void backgroundCompleted(const navidog::TableTask& request, const navidog::TableTaskResult& outcome);
private:
    navidog::ConnectionProfile profile_;
    navidog::TableTask original_;
    navidog::QueryService& service_;
    navidog::BackgroundTasks* tasks_ = nullptr;
    std::optional<navidog::TableTask> backgroundRequest_;
    navidog::ui::PushButton* stop_ = nullptr;
    QProgressBar* taskProgress_ = nullptr;
    navidog::TableTaskResult outcome_;
    QString requestId_,catalogId_,metadataId_,metadataTable_;
    bool running_=false,loading_=false,succeeded_=false,metadataReady_=true,estimating_=false;
    QWidget* fields_;
    navidog::ui::Select *table_,*dimension_;
    navidog::ui::TextInput *grantee_,*granteeHost_;
    navidog::ui::CheckBox* revoke_;
    QVector<navidog::ui::CheckBox*> privileges_;
    GenerationRuleModel* rules_=nullptr;
    navidog::ui::Select* generationMode_=nullptr;
    navidog::ui::TextInput *generationSize_=nullptr, *generationSeed_=nullptr;
    navidog::ui::PushButton* estimate_=nullptr;
    QString configurationStorePath_;
    QWidget* configurationRow_=nullptr;
    navidog::ui::PushButton *saveConfiguration_=nullptr, *exportConfiguration_=nullptr;
    QLabel* ruleHelp_=nullptr;
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
    void startTask(bool preview);
    void updateRuleHelp();
    void addGenerationConfigurationControls(QFormLayout&);
    navidog::GenerationConfiguration generationConfiguration() const;
    void applyGenerationConfiguration(const navidog::GenerationConfiguration&);
    void saveGenerationConfiguration();
    void selectGenerationConfiguration();
    void importGenerationConfiguration();
    void exportGenerationConfiguration();
    void finishRequest(const navidog::TableTaskResult&);
    void showProgress(const navidog::TaskInfo&);
};
