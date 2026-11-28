#include "task_center.hpp"
#include "components.hpp"

#include <QAbstractTableModel>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QUrl>

#include <algorithm>

namespace navidog::ui {
namespace {
QString stateText(TaskState state) {
    switch (state) {
    case TaskState::Queued: return "排队中";
    case TaskState::Running: return "进行中";
    case TaskState::Cancelling: return "正在取消";
    case TaskState::Succeeded: return "已完成";
    case TaskState::Failed: return "失败";
    case TaskState::Cancelled: return "已取消";
    }
    return {};
}
QString amount(qint64 value, TaskUnit unit) {
    if (unit == TaskUnit::Bytes) return QLocale().formattedDataSize(value);
    const auto count = QLocale().toString(value);
    switch (unit) {
    case TaskUnit::Rows: return count + " 行";
    case TaskUnit::Pages: return count + " 页";
    case TaskUnit::Items: return count + " 项";
    case TaskUnit::Bytes: break;
    }
    return count;
}
QString progressText(const TaskInfo& task) {
    const auto& progress = task.progress;
    if (progress.total > 0)
        return amount(progress.done, progress.unit) + " / " + amount(progress.total, progress.unit);
    if (progress.done > 0) return amount(progress.done, progress.unit);
    return task.active() ? progress.phase : QString{};
}
int percentage(const TaskInfo& info) {
    if (info.state == TaskState::Succeeded) return 100;
    if (info.progress.total <= 0) return 0;
    return int(std::clamp(100.0 * double(info.progress.done) / double(info.progress.total), 0.0, 100.0));
}
}

BackgroundTasks* backgroundTasksFor(const QWidget* widget) {
    for (; widget; widget = widget->parentWidget())
        if (auto* manager = widget->findChild<BackgroundTasks*>("backgroundTasks", Qt::FindDirectChildrenOnly))
            return manager;
    return nullptr;
}

void rememberTaskDialog(const QString& id, QDialog& dialog) {
    for (auto* widget = dialog.parentWidget(); widget; widget = widget->parentWidget())
        if (auto* center = widget->findChild<TaskCenter*>("taskCenterController", Qt::FindDirectChildrenOnly)) {
            center->rememberDialog(id, dialog);
            return;
        }
}

class TaskListModel final : public QAbstractTableModel {
    QVector<TaskInfo> rows_;
public:
    explicit TaskListModel(QObject* parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(rows_.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 4; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        return QStringList{"任务", "状态", "进度", "提交时间"}.value(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole) return {};
        const auto& task = rows_.at(index.row());
        switch (index.column()) {
        case 0: return task.spec.title;
        case 1: return stateText(task.state);
        case 2: return progressText(task);
        case 3: return QLocale().toString(task.submittedAt.time(), QLocale::ShortFormat);
        }
        return {};
    }
    QString idAt(int row) const {
        return row < 0 || row >= rows_.size() ? QString{} : rows_.at(row).id;
    }
    int indexOf(const QString& id) const {
        for (int row = 0; row < rows_.size(); ++row)
            if (rows_[row].id == id) return row;
        return -1;
    }
    void replace(QVector<TaskInfo> rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }
    void update(int row, const TaskInfo& task) {
        rows_[row] = task;
        emit dataChanged(index(row, 0), index(row, 3));
    }
};

TaskCenter::TaskCenter(BackgroundTasks& tasks, QStatusBar& status, QWidget& window)
    : QObject(&window), tasks_(tasks) {
    setObjectName("taskCenterController");
    entry_ = new PushButton("任务", &status);
    entry_->setObjectName("taskCenterButton");
    entry_->setFlat(true);
    status.addPermanentWidget(entry_);

    dialog_ = new Dialog(&window);
    dialog_->setObjectName("taskCenter");
    dialog_->setWindowTitle("后台任务");
    dialog_->setWindowModality(Qt::NonModal);
    dialog_->resize(730, 425);
    auto* layout = new QVBoxLayout(dialog_);
    grid_ = new ObjectGrid(dialog_);
    grid_->setObjectName("backgroundTaskList");
    model_ = new TaskListModel(grid_);
    grid_->setModel(model_);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
    grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        grid_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(grid_, 1);

    detail_ = new QLabel(dialog_);
    detail_->setObjectName("backgroundTaskDetail");
    detail_->setTextFormat(Qt::PlainText);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_->setWordWrap(true);
    layout->addWidget(detail_);
    progress_ = new QProgressBar(dialog_);
    progress_->setObjectName("backgroundTaskProgress");
    layout->addWidget(progress_);
    artifacts_ = new Select(dialog_);
    artifacts_->setObjectName("backgroundTaskFiles");
    artifacts_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(artifacts_);

    auto* actions = new QHBoxLayout;
    reopen_ = new PushButton("恢复任务窗口", dialog_);
    reopen_->setObjectName("restoreTaskWindow");
    stop_ = new PushButton("取消任务", dialog_);
    stop_->setObjectName("cancelBackgroundTask");
    openFile_ = new PushButton("打开文件", dialog_);
    openFile_->setObjectName("openTaskFile");
    openFolder_ = new PushButton("打开所在文件夹", dialog_);
    openFolder_->setObjectName("openTaskFolder");
    clear_ = new PushButton("清除已结束任务", dialog_);
    clear_->setObjectName("clearFinishedTasks");
    for (auto* button : {reopen_, stop_, openFile_, openFolder_}) actions->addWidget(button);
    actions->addStretch();
    actions->addWidget(clear_);
    layout->addLayout(actions);

    connect(entry_, &QPushButton::clicked, this, [this] { show(); });
    connect(grid_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { refreshSelection(); });
    connect(grid_, &QTableView::doubleClicked, this, [this] {
        if (reopen_->isEnabled()) reopen();
        else if (openFile_->isEnabled()) openArtifact(false);
    });
    connect(reopen_, &QPushButton::clicked, this, [this] { reopen(); });
    connect(stop_, &QPushButton::clicked, this, [this] { tasks_.cancel(selectedId()); });
    connect(openFile_, &QPushButton::clicked, this, [this] { openArtifact(false); });
    connect(openFolder_, &QPushButton::clicked, this, [this] { openArtifact(true); });
    connect(clear_, &QPushButton::clicked, &tasks_, &BackgroundTasks::clearFinished);
    connect(&tasks_, &BackgroundTasks::changed, this, [this](const QString& id) { refresh(id); });
    refresh({});
}

void TaskCenter::show() {
    refresh({});
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
}

void TaskCenter::rememberDialog(const QString& id, QDialog& details) {
    dialogs_.insert(id, &details);
    refreshSelection();
}

QString TaskCenter::selectedId() const { return model_->idAt(grid_->currentIndex().row()); }

void TaskCenter::refresh(const QString& id) {
    const int active = tasks_.activeCount();
    entry_->setText(active ? QString("任务 · %1").arg(active) : QString("任务"));
    entry_->setToolTip(active ? QString("%1 项后台任务正在处理").arg(active) : QString("查看后台任务和导出文件"));

    const auto selected = selectedId();
    const auto* task = tasks_.info(id);
    const int row = model_->indexOf(id);
    if (task && row >= 0) {
        model_->update(row, *task);
        if (!task->active()) dialogs_.remove(id);
        if (id == selected) refreshSelection();
        clear_->setEnabled(model_->rowCount() > active);
        return;
    }

    const QSignalBlocker hold(grid_->selectionModel());
    model_->replace(tasks_.history());
    for (auto it = dialogs_.begin(); it != dialogs_.end();) {
        if (!it.value() || !tasks_.info(it.key())) it = dialogs_.erase(it);
        else ++it;
    }
    const int selectedRow = model_->indexOf(selected);
    if (model_->rowCount()) grid_->setCurrentIndex(model_->index(std::max(0, selectedRow), 0));
    clear_->setEnabled(model_->rowCount() > active);
    refreshSelection();
}

void TaskCenter::refreshSelection() {
    const auto id = selectedId();
    const auto* item = tasks_.info(id);
    const bool running = item && item->active();
    const auto path = artifacts_->currentData().toString();
    artifacts_->clear();
    if (item)
        for (const auto& artifact : item->artifacts)
            artifacts_->addItem(QDir::toNativeSeparators(artifact), artifact);
    auto previous = artifacts_->findData(path);
    if (previous >= 0) artifacts_->setCurrentIndex(previous);
    artifacts_->setVisible(artifacts_->count() > 0);
    openFile_->setEnabled(artifacts_->count() > 0);
    openFolder_->setEnabled(artifacts_->count() > 0);
    stop_->setEnabled(running && item->state != TaskState::Cancelling);
    reopen_->setEnabled(running && dialogs_.value(id));

    progress_->setVisible(item != nullptr);
    if (!item) {
        detail_->setText("暂无后台任务");
        return;
    }
    const bool indeterminate = item->state == TaskState::Running && !item->progress.total;
    progress_->setRange(0, indeterminate ? 0 : 100);
    if (!indeterminate) progress_->setValue(percentage(*item));
    progress_->setFormat(item->state == TaskState::Running ? QString("%p%") : stateText(item->state));
    QStringList lines;
    if (!item->spec.scope.isEmpty()) lines << item->spec.scope;
    if (item->active() && !item->progress.phase.isEmpty()) lines << item->progress.phase;
    if (!item->detail.isEmpty()) lines << item->detail.left(2048);
    if (!item->error.isEmpty()) lines << item->error.left(4096);
    detail_->setText(lines.join('\n'));
}

void TaskCenter::openArtifact(bool directory) {
    const auto path = artifacts_->currentData().toString();
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    const QString target = directory && !info.isDir() ? info.absolutePath() : info.absoluteFilePath();
    if (!QFileInfo::exists(target) || !QDesktopServices::openUrl(QUrl::fromLocalFile(target)))
        detail_->setText("无法打开结果位置：" + QDir::toNativeSeparators(target));
}

void TaskCenter::reopen() {
    if (const auto window = dialogs_.value(selectedId())) {
        window->showNormal();
        window->raise();
        window->activateWindow();
    }
}
}
