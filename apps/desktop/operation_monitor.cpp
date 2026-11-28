#include "operation_monitor.hpp"
#include "navidog/operation_timing.hpp"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSaveFile>
#include <utility>

namespace {
constexpr int heartbeatIntervalMs = 250;
constexpr int writeIntervalMs = 2000;
}

class OperationMonitor::Writer final : public QObject {
public:
    explicit Writer(QString path) : path_(std::move(path)) {}

    void initialize() {
        QFile lastLog(path_);
        if (lastLog.open(QIODevice::ReadOnly) &&
            lastLog.size() <= navidog::operationLogMaxFileBytes)
            navidog::restoreOperationLog(lastLog.read(navidog::operationLogMaxFileBytes));
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &Writer::flush);
        timer->start(writeIntervalMs);
    }

    void flush() {
        const auto snapshot = navidog::operationLogSnapshot(savedRevision_);
        if (snapshot.text.isEmpty())
            return;
        QSaveFile file(path_);
        if (file.open(QIODevice::WriteOnly) &&
            file.write(snapshot.text) == snapshot.text.size() && file.commit())
            savedRevision_ = snapshot.revision;
    }

private:
    QString path_;
    quint64 savedRevision_ = 0;
};

OperationMonitor::OperationMonitor(const QString& directory)
    : path_(QDir(directory).filePath("operation-timing.log")) {}

OperationMonitor::~OperationMonitor() {
    heartbeat_.stop();
    if (writer_) {
        QMetaObject::invokeMethod(writer_, [writer = writer_] { writer->flush(); },
                                  Qt::BlockingQueuedConnection);
        writerThread_.quit();
        writerThread_.wait();
    }
}

void OperationMonitor::start() {
    if (writer_)
        return;
    writer_ = new Writer(path_);
    writer_->moveToThread(&writerThread_);
    connect(&writerThread_, &QThread::started, writer_, &Writer::initialize);
    connect(&writerThread_, &QThread::finished, writer_, &QObject::deleteLater);
    writerThread_.setObjectName("navidog-operation-log");
    writerThread_.start();

    heartbeat_.setInterval(heartbeatIntervalMs);
    heartbeat_.setTimerType(Qt::PreciseTimer);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        const auto extraDelay = heartbeatClock_.restart() - heartbeatIntervalMs;
        navidog::recordOperationTiming(navidog::TimedOperation::UiEventLoopDelay, extraDelay);
    });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, &OperationMonitor::updateHeartbeat);
    updateHeartbeat();
}

void OperationMonitor::updateHeartbeat() {
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) {
        heartbeatClock_.start();
        heartbeat_.start();
    } else {
        heartbeat_.stop();
    }
}
