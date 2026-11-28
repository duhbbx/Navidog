#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

class OperationMonitor final : public QObject {
public:
    explicit OperationMonitor(const QString& directory);
    ~OperationMonitor() override;
    void start();

private:
    class Writer;
    void updateHeartbeat();

    QString path_;
    QThread writerThread_;
    Writer* writer_ = nullptr;
    QTimer heartbeat_;
    QElapsedTimer heartbeatClock_;
};
