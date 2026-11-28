#pragma once

#include "workspace_pane.hpp"
#include "er_canvas.hpp"
#include "export_paths.hpp"

class QLabel;
class QListView;
class QComboBox;
class QLineEdit;
class QToolButton;
class QTimer;
class ErTableList;

class ErPane final : public WorkspacePane {
    Q_OBJECT
public:
    explicit ErPane(const QString& draftDirectory, QWidget* parent = nullptr);
    ~ErPane() override;
    bool dirty() const { return dirty_; }
    bool busy() const { return bool(cancellation_); }
    const navidog::er::Project& project() const { return project_; }
    void useProject(navidog::er::Project);
    void importFrom(navidog::ConnectionProfile, navidog::er::ImportOptions);
    void loadDetails(int index, navidog::ConnectionProfile);
    void openFile(const QString& path);
    void restore(const QJsonObject&);
    bool saveFile(bool choosePath = false);
    bool prepareClose();
    void forgetDraft();
    void persistDraft() const;
    QJsonObject sessionState() const override;
    std::function<void()> reverseRequested;
    std::function<QString(const QString&)> connectionName;
    std::function<void(int)> detailsRequested;
    std::function<bool(const QString&)> openSqlRequested;

signals:
    void titleChanged(const QString&);

private:
    navidog::er::Project project_;
    navidog::er::Graph graph_;
    navidog::Cancellation cancellation_;
    QString filePath_, draftDirectory_, draftId_, draftOrigin_;
    bool dirty_ = false, syncing_ = false, hasProject_ = true;
    int selected_ = -1;
    QListView* list_;
    ErTableList* tableList_;
    navidog::ui::er::Canvas* canvas_;
    QLabel *status_, *selectedInfo_, *zoom_;
    QComboBox *groups_, *layouts_, *grouping_;
    QLineEdit* search_;
    QTimer* filterTimer_;
    QToolButton *edit_, *link_, *remove_, *details_, *stop_, *save_, *ddl_, *export_;
    struct PreparedDiagram {
        navidog::er::Graph graph;
        navidog::er::Arrangement scene;
        navidog::er::SpatialIndex index;
    };
    static PreparedDiagram prepareDiagram(const navidog::er::Project&);
    void useProject(navidog::er::Project, PreparedDiagram);
    void showDiagram(PreparedDiagram, bool fit);
    void rebuild(bool fit = false);
    void refreshList();
    void select(int index, bool locate, bool reveal);
    void updateActions();
    void changed();
    bool editTable(int index, bool view);
    void addRelation();
    void groupTable();
    void removeSelected();
    void showDDL();
    void exportDocuments();
    navidog::ui::ExportContext modelExportContext() const;
    void readProject(const QString& path, bool userFile);
    void showError(const QString& title,const QString& message);
    QString draftPath() const;
    using Progress = std::function<void(const QString&)>;
    template<class Work,class Done>
    void start(const QString& label, Work work, Done done);
};
