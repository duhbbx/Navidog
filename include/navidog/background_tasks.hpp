#pragma once

#include "types.hpp"
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QVariant>
#include <functional>

namespace navidog {

enum class TaskState { Queued, Running, Cancelling, Succeeded, Failed, Cancelled };
enum class TaskUnit { Items, Rows, Bytes, Pages };

struct TaskProgress {
    qint64 done = 0;
    qint64 total = 0; // Zero means unknown.
    TaskUnit unit = TaskUnit::Items;
    QString phase;
};

using TaskProgressCallback = std::function<void(const TaskProgress&)>;

struct TaskContext {
    Cancellation cancel;
    TaskProgressCallback report; // Valid on the worker thread until Work returns.
};

struct TaskSpec {
    QString title;
    QString scope;
    QString resource; // Tasks using the same resource run consecutively.
    QString output;   // Optional file reserved during execution.
};

struct TaskResult {
    QString detail;
    QString error;
    QStringList artifacts;
    bool cancelled = false;
    QVariant payload;
};

struct TaskInfo {
    QString id;
    TaskSpec spec;
    TaskState state = TaskState::Queued;
    TaskProgress progress;
    QString detail, error;
    QStringList artifacts;
    QDateTime submittedAt, finishedAt;
    bool active() const {
        return state == TaskState::Queued || state == TaskState::Running || state == TaskState::Cancelling;
    }
};

class BackgroundTasks final : public QObject {
    Q_OBJECT
public:
    using Work = std::function<TaskResult(const TaskContext&)>;
    explicit BackgroundTasks(QObject* parent = nullptr);
    ~BackgroundTasks() override;

    // Called on the owner's thread. Work must use independent driver sessions
    // and capture value snapshots, never widgets or live query sessions.
    QString submit(TaskSpec, Work, QString* rejection = nullptr);
    bool cancel(const QString& id);
    const TaskInfo* info(const QString& id) const;
    QVector<TaskInfo> history() const;
    int activeCount() const;
    void clearFinished();

signals:
    void changed(const QString& id);
    void finished(const QString& id, const navidog::TaskResult& result);

private:
    struct Entry {
        TaskInfo info;
        Work work;
        Cancellation token;
        QString outputKey;
    };
    QThreadPool pool_;
    QHash<QString, Entry> entries_;
    QStringList order_;
    int running_ = 0;
    bool scheduling_ = false;

    void schedule();
    void complete(const QString&, TaskResult);
    void trimHistory();
};

}
