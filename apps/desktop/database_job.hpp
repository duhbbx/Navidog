#pragma once
#include <QThread>
#include <memory>
#include <optional>
#include <type_traits>

template<class Work, class Done>
void runDatabaseJob(QObject* owner, Work work, Done done) {
    struct Result { std::optional<std::invoke_result_t<Work>> value; QString error; };
    auto result = std::make_shared<Result>();
    auto* thread = QThread::create([result, work = std::move(work)] {
        try { result->value = work(); }
        catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()); }
    });
    QObject::connect(thread, &QThread::finished, owner, [result, done = std::move(done)]() mutable {
        done(std::move(result->value), result->error);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
