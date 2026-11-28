#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QVector>

class StartupTiming {
public:
    StartupTiming() { clock_.start(); }

    void mark(const char* stage) { stages_.push_back({stage, clock_.nsecsElapsed()}); }

    QByteArray report() const {
        QByteArray report = QString("Navidog %1 | %2\nstage\telapsed_ms\ttotal_ms\n")
            .arg(QCoreApplication::applicationVersion(), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
            .toUtf8();
        qint64 previous = 0;
        for (const auto& entry : stages_) {
            report += QString("%1\t%2\t%3\n")
                .arg(QString::fromLatin1(entry.stage))
                .arg((entry.nanos - previous) / 1000000.0, 0, 'f', 2)
                .arg(entry.nanos / 1000000.0, 0, 'f', 2).toUtf8();
            previous = entry.nanos;
        }
        return report;
    }

    void write(const QString& directory) {
        if (writtenStages_ == stages_.size()) return;
        const auto bytes = report();
        QSaveFile file(QDir(directory).filePath("startup.log"));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            qWarning().noquote() << "Navidog startup timing (startup.log unavailable):\n" << QString::fromUtf8(bytes);
            return;
        }
        writtenStages_ = stages_.size();
    }

private:
    struct Entry { const char* stage; qint64 nanos; };
    QElapsedTimer clock_;
    QVector<Entry> stages_;
    qsizetype writtenStages_ = 0;
};
