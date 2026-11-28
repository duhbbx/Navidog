#include "navidog/database.hpp"
#include "collector.hpp"
#include "cancel_watch.hpp"
#include <duckdb.h>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <mutex>

namespace navidog {
namespace {
struct Instance {
    duckdb_database handle = nullptr;
    ~Instance() { duckdb_close(&handle); }
};
std::shared_ptr<Instance> openInstance(const ConnectionProfile& profile) {
    static std::mutex mutex;
    static QHash<QString, std::weak_ptr<Instance>> instances;
    const auto path = QFileInfo(profile.database).canonicalFilePath();
    if (path.isEmpty()) native::fail("请选择已存在的 DuckDB 文件");
    std::lock_guard lock(mutex);
    const auto key = path + QChar(0x1f) + (profile.readOnly ? "ro" : "rw");
    if (auto active = instances.value(key).lock()) return active;
    for (auto it = instances.begin(); it != instances.end();)
        if (it.value().expired()) it = instances.erase(it); else ++it;
    auto instance = std::make_shared<Instance>();
    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess) native::fail("无法创建 DuckDB 配置");
    duckdb_set_config(config, "access_mode", profile.readOnly ? "READ_ONLY" : "READ_WRITE");
    char* error = nullptr;
    auto opened = duckdb_open_ext(QFile::encodeName(path).constData(), &instance->handle, config, &error);
    duckdb_destroy_config(&config);
    if (opened != DuckDBSuccess) {
        const auto message = error ? QString::fromUtf8(error) : QString("无法打开 DuckDB 数据库");
        duckdb_free(error);
        native::fail(message);
    }
    duckdb_free(error);
    instances.insert(key, instance);
    return instance;
}
struct Result {
    duckdb_result value{};
    ~Result() { duckdb_destroy_result(&value); }
};
struct Prepared {
    duckdb_prepared_statement value = nullptr;
    ~Prepared() { duckdb_destroy_prepare(&value); }
};

class DuckDatabase final : public Database {
    std::shared_ptr<Instance> instance_;
    duckdb_connection connection_ = nullptr;
    bool active_ = false;

public:
    explicit DuckDatabase(const ConnectionProfile& profile) : instance_(openInstance(profile)) {
        if (duckdb_connect(instance_->handle, &connection_) != DuckDBSuccess) native::fail("无法创建 DuckDB 会话");
    }
    ~DuckDatabase() override { duckdb_disconnect(&connection_); }
    QString serverVersion() override { return QString::fromUtf8(duckdb_library_version()); }
    TransactionState transactionState() const override { return active_ ? TransactionState::Active : TransactionState::Idle; }
    QStringList tables() override { return {}; }
    QueryResult execute(const QString& sql, QueryLimits limits, const Cancellation& cancel) override {
        return executeBound(sql, {}, limits, cancel);
    }
    QueryResult executeBound(const QString& text, const QVector<QVariant>& args, QueryLimits limits,
                             const Cancellation& cancel) override {
        native::validate(text, sql::Dialect::PostgreSQL);
        native::Collector collector(limits);
        auto& out = collector.result;
        if (cancel->load()) { out.cancelled = true; return out; }
        native::CancelWatch watch(cancel, [this] { duckdb_interrupt(connection_); });
        const auto bytes = text.toUtf8();
        Prepared prepared;
        if (duckdb_prepare(connection_, bytes.constData(), &prepared.value) != DuckDBSuccess) {
            out.error = QString::fromUtf8(duckdb_prepare_error(prepared.value));
            out.cancelled = cancel->load();
            return out;
        }
        if (duckdb_nparams(prepared.value) != idx_t(args.size())) { out.error = "DuckDB 参数数量不匹配"; return out; }
        for (int i = 0; i < args.size(); ++i) {
            const auto& value = args.at(i);
            const auto index = idx_t(i + 1);
            duckdb_state bound;
            if (!value.isValid() || value.isNull()) bound = duckdb_bind_null(prepared.value, index);
            else if (value.userType() == QMetaType::QByteArray) {
                const auto data = value.toByteArray();
                bound = duckdb_bind_blob(prepared.value, index, data.constData(), data.size());
            } else if (value.userType() == QMetaType::Bool) {
                bound = duckdb_bind_boolean(prepared.value, index, value.toBool());
            } else if (value.userType() == QMetaType::Int || value.userType() == QMetaType::LongLong) {
                bound = duckdb_bind_int64(prepared.value, index, value.toLongLong());
            } else if (value.userType() == QMetaType::Double) {
                bound = duckdb_bind_double(prepared.value, index, value.toDouble());
            } else {
                const auto data = value.toString().toUtf8();
                bound = duckdb_bind_varchar_length(prepared.value, index, data.constData(), data.size());
            }
            if (bound != DuckDBSuccess) { out.error = "无法绑定 DuckDB 参数"; return out; }
        }
        Result result;
        if (duckdb_execute_prepared(prepared.value, &result.value) != DuckDBSuccess) {
            out.error = QString::fromUtf8(duckdb_result_error(&result.value));
            out.cancelled = cancel->load();
            return out;
        }
        const auto command = text.simplified().section(' ', 0, 0).remove(';').toUpper();
        if (command == "BEGIN" || command == "START") active_ = true;
        else if (command == "COMMIT" || command == "ROLLBACK" || command == "END") active_ = false;
        out.affectedRows = duckdb_rows_changed(&result.value);
        const auto columns = duckdb_column_count(&result.value);
        if (columns > 4096) { out.error = "DuckDB 结果列数超出上限"; return out; }
        for (idx_t c = 0; c < columns; ++c) out.columns << QString::fromUtf8(duckdb_column_name(&result.value, c));
        const auto count = duckdb_row_count(&result.value);
        for (idx_t r = 0; r < count && !cancel->load(); ++r) {
            if (r >= idx_t(limits.maxRows)) { out.truncated = true; break; }
            QVector<QVariant> row;
            for (idx_t c = 0; c < columns; ++c) {
                if (duckdb_value_is_null(&result.value,c,r)) { row << QVariant{}; continue; }
                if (duckdb_column_type(&result.value,c) == DUCKDB_TYPE_BLOB) {
                    auto blob = duckdb_value_blob(&result.value,c,r);
                    out.cellTruncated |= blob.size > idx_t(limits.maxCellBytes);
                    row << QByteArray(static_cast<const char*>(blob.data), int(qMin(blob.size, idx_t(limits.maxCellBytes))));
                    duckdb_free(blob.data);
                    continue;
                }
                auto data = duckdb_value_string(&result.value, c, r);
                out.cellTruncated |= data.size > idx_t(limits.maxCellBytes);
                if (data.data) row << QString::fromUtf8(data.data, int(qMin(data.size, idx_t(limits.maxCellBytes))));
                else row << QString("该类型暂无文本预览");
                duckdb_free(data.data);
            }
            if (!collector.add(std::move(row))) break;
        }
        out.cancelled = cancel->load();
        return out;
    }
};
}
std::unique_ptr<Database> connectDuckDb(const ConnectionProfile& profile) {
    return std::make_unique<DuckDatabase>(profile);
}
}
