#include "navidog/background_tasks.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRunnable>
#include <QScopedValueRollback>
#include <algorithm>
#include <QSet>
#include <QUuid>

namespace navidog {
namespace {
constexpr int workerCount = 2;
constexpr int activeLimit = 32;
constexpr int historyLimit = 128;

QString outputKey(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo file(path);
    auto key = file.canonicalFilePath();
    if (key.isEmpty()) {
        auto parent = QFileInfo(file.absolutePath()).canonicalFilePath();
        if (parent.isEmpty()) parent = file.absolutePath();
        key = QDir(parent).filePath(file.fileName());
    }
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return QDir::cleanPath(key);
}
}

BackgroundTasks::BackgroundTasks(QObject* parent) : QObject(parent) {
    setObjectName("backgroundTasks");
    pool_.setMaxThreadCount(workerCount);
}

BackgroundTasks::~BackgroundTasks() {
    for (const auto& entry : entries_)
        if (entry.info.active()) entry.token->store(true);
    pool_.waitForDone();
}

QString BackgroundTasks::submit(TaskSpec spec, Work work, QString* rejection) {
    const auto reject = [&](const QString& reason) {
        if (rejection) *rejection = reason;
        return QString{};
    };
    if (!work || spec.title.isEmpty()) return reject("任务定义不完整");
    if (activeCount() >= activeLimit) return reject("待处理任务过多，请等待已有任务完成");
    const auto output = outputKey(spec.output);
    if (!output.isEmpty())
        for (const auto& entry : entries_)
            if (entry.info.active() && entry.outputKey == output)
                return reject("已有任务正在使用这个输出路径");

    Entry entry;
    entry.info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.info.spec = std::move(spec);
    entry.info.submittedAt = QDateTime::currentDateTime();
    entry.work = std::move(work);
    entry.token = cancellation();
    entry.outputKey = output;
    const auto id = entry.info.id;
    entries_.insert(id, std::move(entry));
    order_ << id;
    trimHistory();
    emit changed(id);
    schedule();
    return id;
}

const TaskInfo* BackgroundTasks::info(const QString& id) const {
    const auto found = entries_.constFind(id);
    return found == entries_.cend() ? nullptr : &found->info;
}

QVector<TaskInfo> BackgroundTasks::history() const {
    QVector<TaskInfo> result;
    result.reserve(order_.size());
    for (auto it = order_.crbegin(); it != order_.crend(); ++it)
        if (const auto* task = info(*it)) result << *task;
    return result;
}

int BackgroundTasks::activeCount() const {
    int count = 0;
    for (const auto& entry : entries_)
        if (entry.info.active()) ++count;
    return count;
}

bool BackgroundTasks::cancel(const QString& id) {
    auto found = entries_.find(id);
    if (found == entries_.end() || !found->info.active() || found->info.state == TaskState::Cancelling)
        return false;
    found->token->store(true);
    if (found->info.state == TaskState::Queued) {
        TaskResult result;
        result.cancelled = true;
        result.detail = "任务已取消";
        complete(id, std::move(result));
    } else {
        found->info.state = TaskState::Cancelling;
        emit changed(id);
    }
    return true;
}

void BackgroundTasks::schedule() {
    if (scheduling_) return;
    const QScopedValueRollback<bool> guard(scheduling_, true);
    while (running_ < workerCount) {
        QSet<QString> resources;
        for (const auto& entry : entries_)
            if ((entry.info.state == TaskState::Running || entry.info.state == TaskState::Cancelling) &&
                !entry.info.spec.resource.isEmpty())
                resources.insert(entry.info.spec.resource);

        const auto next = std::find_if(order_.cbegin(), order_.cend(), [&](const QString& candidate) {
            const auto* item = info(candidate);
            return item && item->state == TaskState::Queued && !resources.contains(item->spec.resource);
        });
        if (next == order_.cend()) return;
        const auto id = *next;
        auto entry = entries_.find(id);
        entry->info.state = TaskState::Running;
        ++running_;
        auto token = entry->token;
        auto work = std::move(entry->work);
        pool_.start(QRunnable::create([this, id, token, work = std::move(work)] {
            QElapsedTimer updates;
            QString phase;
            const auto report = [this, id, &updates, &phase](const TaskProgress& progress) {
                if (updates.isValid() && updates.elapsed() < 200 && progress.phase == phase &&
                    (progress.total <= 0 || progress.done < progress.total)) return;
                updates.start();
                phase = progress.phase;
                QMetaObject::invokeMethod(this, [this, id, progress] {
                    auto item = entries_.find(id);
                    if (item == entries_.end() || !item->info.active()) return;
                    item->info.progress = progress;
                    emit changed(id);
                }, Qt::QueuedConnection);
            };
            TaskResult result;
            try {
                result = work({token, report});
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
                result.cancelled = token->load();
            } catch (...) {
                result.error = "后台任务遇到未知错误";
            }
            QMetaObject::invokeMethod(this, [this, id, result = std::move(result)]() mutable {
                complete(id, std::move(result));
            }, Qt::QueuedConnection);
        }));
        emit changed(id);
    }
}

void BackgroundTasks::complete(const QString& id, TaskResult result) {
    auto item = entries_.find(id);
    if (item == entries_.end() || !item->info.active()) return;
    if (item->info.state != TaskState::Queued) --running_;
    item->work = {};
    auto& info = item->info;
    info.finishedAt = QDateTime::currentDateTime();
    info.state = result.cancelled ? TaskState::Cancelled : !result.error.isEmpty() ?
        TaskState::Failed : TaskState::Succeeded;
    info.detail = result.detail;
    info.error = result.error;
    if (info.state == TaskState::Succeeded) info.artifacts = result.artifacts;
    emit finished(id, result);
    emit changed(id);
    trimHistory();
    schedule();
}

void BackgroundTasks::clearFinished() {
    for (int i = int(order_.size()) - 1; i >= 0; --i)
        if (const auto* task = info(order_[i]); task && !task->active()) entries_.remove(order_.takeAt(i));
    emit changed({});
}

void BackgroundTasks::trimHistory() {
    for (int i = 0; i < order_.size() && order_.size() > historyLimit;) {
        const auto* task = info(order_[i]);
        if (task && !task->active()) entries_.remove(order_.takeAt(i));
        else ++i;
    }
}

}
