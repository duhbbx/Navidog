#include "jdbc_bridge.hpp"
#include "navidog/driver_packages.hpp"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <limits>
#include <mutex>

namespace navidog {
namespace {
constexpr quint32 maxFrame = 64 * 1024 * 1024;
constexpr int maxColumns = 4096;
enum class Operation : quint8 { Cancel, Open, Close, Query, AutoCommit, Commit, Rollback, Tables, Metadata };

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}
struct Bundle {
    QString java, bridge;
};
Bundle jdbcBundle() {
    const auto base = QCoreApplication::applicationDirPath();
#if defined(Q_OS_MACOS)
    const auto installed = base + "/../Resources/jdbc";
#elif defined(Q_OS_WIN)
    const auto installed = base + "/jdbc";
#else
    const auto installed = base + "/../lib/navidog/jdbc";
#endif
    const auto managedJava = javaPackageExecutable();
    const auto externalJava = qEnvironmentVariable("NAVIDOG_JAVA");
    for (const auto& path : {installed, base + "/jdbc", base + "/../jdbc"}) {
        const QDir root(path);
#ifdef Q_OS_WIN
        const auto bundledJava = root.filePath("runtime/bin/java.exe");
#else
        const auto bundledJava = root.filePath("runtime/bin/java");
#endif
        const auto bridge = root.filePath("navidog-jdbc-bridge.jar");
        if (!QFileInfo(bridge).isFile()) continue;
        for (const auto& java : {managedJava, externalJava, bundledJava})
            if (QFileInfo(java).isAbsolute() && QFileInfo(java).isFile() && QFileInfo(java).isExecutable())
                return {QFileInfo(java).absoluteFilePath(), QFileInfo(bridge).absoluteFilePath()};
    }
    return {};
}
void configure(QDataStream& stream) {
    stream.setVersion(QDataStream::Qt_5_15);
}
struct Reply {
    QByteArray payload;
    bool done = false;
};
struct JdbcReply {
    QByteArray payload;
    bool cancelled = false;
};

// A single I/O thread owns the child process. Query workers can wait independently,
// and the child runs requests from different JDBC sessions concurrently.
class Bridge final : private QThread {
public:
    explicit Bridge(Bundle bundle) : bundle_(std::move(bundle)) { start(); }
    ~Bridge() override {
        stopping_ = true;
        wait();
    }
    bool healthy() const { return !stopping_ && !failed_; }

    JdbcReply request(Operation operation, qint32 session, const QByteArray& data,
                      int timeout, const Cancellation& token = {}) {
        std::unique_lock lock(mutex_);
        if (!healthy())
            fail(error_.isEmpty() ? QString("JDBC 辅助进程已断开") : error_);
        if (token && token->load())
            return {{}, true};
        if (data.size() > maxFrame - 9)
            fail("JDBC 请求超过大小上限");
        const auto id = nextId_++;
        const auto reply = std::make_shared<Reply>();
        pending_.emplace(id, reply);
        enqueue(id, operation, session, data);
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 cancelledAt = -1;
        while (!reply->done && healthy()) {
            const auto now = elapsed.elapsed();
            if (cancelledAt < 0 && token && token->load()) {
                cancelledAt = now;
                enqueue(id, Operation::Cancel, session, {});
            }
            if ((cancelledAt >= 0 && now - cancelledAt > 3000) ||
                (cancelledAt < 0 && timeout > 0 && now > timeout)) {
                disconnectLocked(cancelledAt >= 0 ? "JDBC 查询取消超时，辅助进程已断开"
                                                    : "JDBC 操作超时，辅助进程已断开");
                break;
            }
            ready_.wait_for(lock, std::chrono::milliseconds(25));
        }
        pending_.erase(id);
        if (cancelledAt >= 0 || (token && token->load()))
            return {{}, true};
        if (!reply->done)
            fail(error_.isEmpty() ? QString("JDBC 辅助进程已断开") : error_);
        return {std::move(reply->payload), false};
    }

private:
    void enqueue(quint32 id, Operation operation, qint32 session, const QByteArray& data) {
        QByteArray frame;
        QDataStream out(&frame, QIODevice::WriteOnly);
        configure(out);
        out << quint32(9 + data.size()) << id << quint8(operation) << session;
        frame.append(data);
        outgoing_.push_back(std::move(frame));
    }
    void disconnectLocked(const QString& error) {
        failed_ = true;
        error_ = error;
        ready_.notify_all();
    }
    void run() override {
        QProcess process;
        process.setProgram(bundle_.java);
        process.setArguments({"-Xmx384m", "-Djava.awt.headless=true", "-cp", bundle_.bridge, "app.navidog.jdbc.Bridge"});
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start();
        if (!process.waitForStarted(4000)) {
            std::lock_guard lock(mutex_);
            disconnectLocked("无法启动 Navidog 内置 Java");
            return;
        }
        QByteArray incoming, sending;
        qsizetype sent = 0;
        while (healthy() && process.state() != QProcess::NotRunning) {
            if (sending.isEmpty()) {
                std::lock_guard lock(mutex_);
                if (!outgoing_.empty()) {
                    sending = std::move(outgoing_.front());
                    outgoing_.pop_front();
                    sent = 0;
                }
            }
            if (!sending.isEmpty() && process.bytesToWrite() < 64 * 1024) {
                const auto written = process.write(sending.constData() + sent,
                    qMin<qsizetype>(64 * 1024, sending.size() - sent));
                if (written < 0)
                    break;
                sent += written;
                if (sent == sending.size())
                    sending.clear();
            }
            process.waitForReadyRead(sending.isEmpty() ? 15 : 1);
            incoming += process.readAllStandardOutput();
            process.readAllStandardError();
            while (incoming.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(incoming.constData());
                if (size < 5 || size > maxFrame) {
                    std::lock_guard lock(mutex_);
                    disconnectLocked("JDBC 辅助进程返回了无效消息");
                    break;
                }
                if (incoming.size() < 4 + qsizetype(size))
                    break;
                const auto id = qFromBigEndian<quint32>(incoming.constData() + 4);
                auto payload = incoming.mid(8, size - 4);
                incoming.remove(0, size + 4);
                std::lock_guard lock(mutex_);
                auto found = pending_.find(id);
                if (found != pending_.end()) {
                    found->second->payload = std::move(payload);
                    found->second->done = true;
                    ready_.notify_all();
                }
            }
        }
        if (process.state() != QProcess::NotRunning) {
            process.closeWriteChannel();
            if (!process.waitForFinished(400)) {
                process.kill();
                process.waitForFinished(1000);
            }
        }
        std::lock_guard lock(mutex_);
        if (!failed_)
            disconnectLocked("JDBC 辅助进程已退出");
    }

    Bundle bundle_;
    std::atomic_bool stopping_{false}, failed_{false};
    std::mutex mutex_;
    std::condition_variable ready_;
    QString error_;
    quint32 nextId_ = 1;
    std::map<quint32, std::shared_ptr<Reply>> pending_;
    std::deque<QByteArray> outgoing_;
};

std::shared_ptr<Bridge> sharedBridge() {
    static std::mutex mutex;
    static std::weak_ptr<Bridge> cached;
    std::lock_guard lock(mutex);
    auto active = cached.lock();
    if (!active || !active->healthy()) {
        auto bundle = jdbcBundle();
        if (bundle.java.isEmpty())
            fail("请在「工具 → 驱动与运行时」安装 Java 25 运行时");
        active = std::make_shared<Bridge>(std::move(bundle));
        cached = active;
    }
    return active;
}

class JdbcDatabase final : public Database {
public:
    explicit JdbcDatabase(const JdbcConnectionSpec& spec)
        : bridge_(sharedBridge()), dialect_(spec.scriptDialect), readTimeout_(spec.readTimeout) {
        QByteArray data;
        QDataStream out(&data, QIODevice::WriteOnly);
        configure(out);
        out << spec.driverJar << spec.driverClass << spec.url << quint32(spec.properties.size());
        for (auto it = spec.properties.cbegin(); it != spec.properties.cend(); ++it)
            out << it.key() << it.value();
        out << quint8(spec.verification);
        exchange(Operation::Open, data, (spec.connectTimeout + 5) * 1000, {}, [&](QDataStream& in) {
            in >> session_ >> version_;
            if (session_ <= 0 || version_.isEmpty())
                fail("JDBC 辅助进程未返回有效的会话信息");
        });
    }
    ~JdbcDatabase() override {
        if (healthy()) {
            try { exchange(Operation::Close, {}, 3000, {}, [](QDataStream&) {}); }
            catch (...) {}
        }
    }
    bool healthy() const override { return !broken_ && bridge_->healthy(); }
    QString serverVersion() override { return version_; }
    TransactionState transactionState() const override {
        if (!healthy())
            return TransactionState::Unknown;
        return active_ ? TransactionState::Active : TransactionState::Idle;
    }
    void setAutoCommit(bool enabled) override {
        if (enabled == autoCommit_ || (enabled && active_))
            return;
        setDriverAutoCommit(enabled);
    }
    QueryResult transaction(TransactionAction action, const Cancellation& token) override {
        QueryResult result;
        if (token && token->load()) {
            result.cancelled = true;
            return result;
        }
        if (action == TransactionAction::Begin) {
            if (autoCommit_)
                setDriverAutoCommit(false);
            active_ = true;
        } else if (action != TransactionAction::None && !autoCommit_) {
            result.cancelled = !exchange(action == TransactionAction::Commit ? Operation::Commit : Operation::Rollback,
                {}, 0, token, [](QDataStream&) {});
            if (!result.cancelled)
                active_ = false;
        }
        return result;
    }
    QueryResult execute(const QString& query, QueryLimits limits, const Cancellation& token) override {
        return executeBound(query, {}, limits, token);
    }
    QueryResult executeBound(const QString& query, const QVector<QVariant>& parameters,
                             QueryLimits limits, const Cancellation& token) override {
        if (limits.maxRows < 1 || limits.maxRows > 100000 || limits.maxBytes < 128 ||
            limits.maxCellBytes < 1 || limits.maxCellBytes > 1048576)
            fail("JDBC 结果缓存限制无效");
        if (query.size() > 16 * 1024 * 1024 || query.contains(QChar(0)) || parameters.size() > 65535)
            fail("JDBC SQL 或参数超出限制");
        const auto bytes = query.toUtf8();
        const std::string_view source(bytes.constData(), size_t(bytes.size()));
        std::string error;
        if (!(definitionMode_ && sql::isCompoundDefinition(source, dialect_)) &&
            !sql::isSingleStatement(source, dialect_, error))
            fail(QString::fromStdString(error));
        QByteArray data;
        QDataStream out(&data, QIODevice::WriteOnly);
        configure(out);
        out << query << qint32(limits.maxRows) << qint32(qMin<qint64>(limits.maxBytes, 48 * 1024 * 1024))
            << qint32(limits.maxCellBytes) << qint32(readTimeout_) << quint32(parameters.size());
        for (const auto& param : parameters) {
            if (!param.isValid() || param.isNull()) {
                out << quint8(0);
            } else if (param.userType() == QMetaType::QByteArray) {
                out << quint8(2) << param.toByteArray();
            } else if (param.userType() == QMetaType::Bool) {
                out << quint8(3) << param.toBool();
            } else if (param.userType() == QMetaType::Int || param.userType() == QMetaType::LongLong ||
                       param.userType() == QMetaType::UInt ||
                       (param.userType() == QMetaType::ULongLong && param.toULongLong() <= quint64(std::numeric_limits<qint64>::max()))) {
                out << quint8(4) << param.toLongLong();
            } else if (param.userType() == QMetaType::Double || param.userType() == QMetaType::Float) {
                out << quint8(5) << param.toDouble();
            } else {
                out << quint8(1) << param.toString();
            }
            if (data.size() > maxFrame - 9)
                fail("JDBC 参数超过大小上限");
        }
        QueryResult result;
        result.cancelled = !exchange(Operation::Query, data, readTimeout_ > 0 ? (readTimeout_ + 4) * 1000 : 0,
            token, [&](QDataStream& in) { readResult(in, result, limits.maxRows); });
        if (!result.cancelled && !autoCommit_) {
            const auto tokens = sql::lex(source, dialect_);
            QString first;
            for (const auto& token : tokens.tokens) {
                if (token.kind != sql::TokenKind::Comment) {
                    first = QString::fromUtf8(bytes.mid(token.begin, token.end - token.begin)).toUpper();
                    break;
                }
            }
            if (first == "ROLLBACK") {
                const auto text = query.trimmed().simplified().toUpper();
                if (!text.startsWith("ROLLBACK TO") && !text.startsWith("ROLLBACK WORK TO"))
                    active_ = false;
            } else {
                active_ = first != "COMMIT";
            }
        }
        return result;
    }
    QueryResult jdbcMetadata(JdbcMetadata kind, const QString& schema, const QString& object,
                             QueryLimits limits, const Cancellation& token) override {
        if (limits.maxRows < 1 || limits.maxRows > 100000 || limits.maxBytes < 128 ||
            limits.maxCellBytes < 1 || limits.maxCellBytes > 1048576)
            fail("JDBC 元数据缓存限制无效");
        QByteArray data;
        QDataStream out(&data, QIODevice::WriteOnly);
        configure(out);
        out << quint8(kind) << schema << object << qint32(limits.maxRows)
            << qint32(qMin<qint64>(limits.maxBytes, 48 * 1024 * 1024)) << qint32(limits.maxCellBytes);
        QueryResult result;
        result.cancelled = !exchange(Operation::Metadata, data, readTimeout_ > 0 ? (readTimeout_ + 4) * 1000 : 0,
            token, [&](QDataStream& in) { readResult(in, result, limits.maxRows); });
        return result;
    }
    QStringList tables() override {
        QStringList names;
        exchange(Operation::Tables, {}, 10000, {}, [&](QDataStream& in) {
            qint32 count = 0;
            in >> count;
            if (count < 0 || count > 10000)
                fail("JDBC 返回的表数量无效");
            for (int i = 0; i < count; ++i) {
                QString name;
                in >> name;
                names << name;
            }
        });
        return names;
    }

private:
    template <typename Decode>
    bool exchange(Operation operation, const QByteArray& data, int timeout, const Cancellation& token, Decode decode) {
        if (!healthy())
            fail("JDBC 会话已断开");
        auto reply = bridge_->request(operation, session_, data, timeout, token);
        if (reply.cancelled)
            return false;
        QDataStream in(reply.payload);
        configure(in);
        quint8 status = 0;
        in >> status;
        if (status == 1 || status == 2) {
            QString message;
            in >> message;
            broken_ = status == 2;
            if (!message.isEmpty() && in.status() == QDataStream::Ok)
                fail(message);
        } else if (status == 3 && in.atEnd()) {
            return false;
        } else if (status == 0) {
            decode(in);
            if (in.status() == QDataStream::Ok && in.atEnd())
                return true;
        }
        broken_ = true;
        fail("JDBC 辅助进程响应格式无效");
    }
    void setDriverAutoCommit(bool enabled) {
        QByteArray data;
        data.append(enabled ? char(1) : char(0));
        exchange(Operation::AutoCommit, data, 0, {}, [](QDataStream&) {});
        autoCommit_ = enabled;
    }
    static void readResult(QDataStream& in, QueryResult& result, int maxRows) {
        auto readOne = [&in, maxRows](QueryResult& part) {
            qint32 columns = 0, rows = 0;
            in >> columns;
            if (columns < 0 || columns > maxColumns)
                fail("JDBC 结果列数无效");
            for (int i = 0; i < columns; ++i) {
                QString label, type;
                in >> label >> type;
                part.columns << label;
                part.columnTypes << type;
            }
            in >> rows;
            if (rows < 0 || rows > maxRows || (columns == 0 && rows > 0))
                fail("JDBC 结果行数无效");
            part.rows.reserve(rows);
            for (int i = 0; i < rows; ++i) {
                QVector<QVariant> row;
                row.reserve(columns);
                for (int j = 0; j < columns; ++j) {
                    quint8 tag = 0;
                    in >> tag;
                    switch (tag) {
                    case 0: row << QVariant{}; break;
                    case 1: { QString value; in >> value; row << value; break; }
                    case 2: { QByteArray value; in >> value; row << value; break; }
                    default: fail("JDBC 结果字段类型无效");
                    }
                }
                if (in.status() != QDataStream::Ok)
                    fail("JDBC 结果数据不完整");
                part.rows << std::move(row);
            }
            in >> part.affectedRows >> part.cachedBytes >> part.truncated >> part.cellTruncated;
        };
        qint32 count = 0;
        in >> count;
        if (count < 1 || count > 64)
            fail("JDBC 结果集数量无效");
        readOne(result);
        for (int i = 1; i < count; ++i) {
            QueryResult part;
            readOne(part);
            result.moreResults << std::move(part);
        }
    }

    std::shared_ptr<Bridge> bridge_;
    sql::Dialect dialect_;
    int readTimeout_;
    qint32 session_ = 0;
    QString version_;
    bool autoCommit_ = true, active_ = false, broken_ = false;
};
}

bool bundledJdbcAvailable() { return !jdbcBundle().java.isEmpty(); }

std::unique_ptr<Database> connectJdbc(const JdbcConnectionSpec& spec) {
    return std::make_unique<JdbcDatabase>(spec);
}
}
