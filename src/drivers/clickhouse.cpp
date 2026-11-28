#include "navidog/database.hpp"
#include "collector.hpp"
#include <clickhouse/client.h>
#include <QDateTime>
#include <cstring>

namespace navidog {
namespace {
template<class T> T scalar(const clickhouse::ItemView& item) {
    T value{};
    if (item.data.size() == sizeof(value)) std::memcpy(&value, item.data.data(), sizeof(value));
    return value;
}
QVariant cellValue(const clickhouse::ColumnRef& column, size_t row, native::Collector& output) {
    using T = clickhouse::Type;
    try {
        const auto item = column->GetItem(row);
        switch (item.type) {
        case T::Void: return {};
        case T::String: case T::FixedString: {
            const auto length = qMin(item.data.size(), size_t(output.limits.maxCellBytes));
            output.result.cellTruncated |= length < item.data.size();
            const QByteArray raw(item.data.data(), int(length));
            const auto text = QString::fromUtf8(raw);
            return text.toUtf8() == raw ? QVariant(text) : QVariant(raw);
        }
        case T::Int8: return qlonglong(scalar<int8_t>(item));
        case T::Int16: return qlonglong(scalar<int16_t>(item));
        case T::Int32: return qlonglong(scalar<int32_t>(item));
        case T::Int64: return qlonglong(scalar<int64_t>(item));
        case T::Bool: case T::UInt8: return qulonglong(scalar<uint8_t>(item));
        case T::UInt16: return qulonglong(scalar<uint16_t>(item));
        case T::UInt32: return qulonglong(scalar<uint32_t>(item));
        case T::UInt64: return qulonglong(scalar<uint64_t>(item));
        case T::Float32: return double(scalar<float>(item));
        case T::Float64: return scalar<double>(item);
        case T::Date: return QDate(1970,1,1).addDays(scalar<uint16_t>(item)).toString(Qt::ISODate);
        case T::Date32: return QDate(1970,1,1).addDays(scalar<int32_t>(item)).toString(Qt::ISODate);
        case T::DateTime: return QDateTime::fromSecsSinceEpoch(scalar<uint32_t>(item), Qt::UTC).toString(Qt::ISODate);
        default: break;
        }
    } catch (const clickhouse::UnimplementedError&) {
    }
    return QString("该类型暂无文本预览 · %1").arg(QString::fromStdString(column->Type()->GetName()));
}

clickhouse::ClientOptions options(const ConnectionProfile& p) {
    using namespace std::chrono;
    clickhouse::ClientOptions out;
    out.SetHost((p.transportHost.isEmpty() ? p.host : p.transportHost).toStdString())
        .SetPort(uint16_t(p.transportPort ? p.transportPort : p.port))
        .SetDefaultDatabase((p.database.isEmpty() ? QString("default") : p.database).toStdString())
        .SetUser((p.user.isEmpty() ? QString("default") : p.user).toStdString())
        .SetPassword(p.password.toStdString())
        .SetSendRetries(0)
        .SetConnectionConnectTimeout(seconds(p.advanced.connectTimeout))
        .SetConnectionRecvTimeout(seconds(qBound(1, p.advanced.readTimeout ? p.advanced.readTimeout : 15, 600)))
        .SetConnectionSendTimeout(seconds(qBound(1, p.advanced.writeTimeout, 600)));
    if (p.tls) {
#ifdef NAVIDOG_CLICKHOUSE_TLS
        clickhouse::ClientOptions::SSLOptions tls;
        if (!p.ssl.caFile.isEmpty()) tls.SetPathToCAFiles({p.ssl.caFile.toStdString()});
        out.SetSSLOptions(tls);
#else
        native::fail("当前 ClickHouse 驱动未编译 TLS 组件");
#endif
    }
    return out;
}
class ClickHouseDatabase final : public Database {
    clickhouse::Client client_;
    bool healthy_ = true;
public:
    explicit ClickHouseDatabase(const ConnectionProfile& profile) : client_(options(profile)) { client_.Ping(); }
    QString serverVersion() override {
        const auto info = client_.GetServerInfo();
        return QString("%1.%2.%3").arg(info.version_major).arg(info.version_minor).arg(info.version_patch);
    }
    bool healthy() const override { return healthy_; }
    QStringList tables() override { return {}; }
    TransactionState transactionState() const override { return TransactionState::Unavailable; }
    QueryResult execute(const QString& statement, QueryLimits limits, const Cancellation& cancel) override {
        native::validate(statement, sql::Dialect::MySQL);
        native::Collector buffer(limits);
        auto& out = buffer.result;
        if (cancel->load()) { out.cancelled = true; return out; }
        try {
            client_.SelectCancelable(statement.toStdString(), [&](const clickhouse::Block& block) {
                if (cancel->load()) return false;
                const auto columns = block.GetColumnCount();
                if (columns > 4096) { out.error = "ClickHouse 结果列数超出上限"; return false; }
                if (out.columns.isEmpty())
                    for (size_t column = 0; column < columns; ++column) {
                        out.columns << QString::fromStdString(block.GetColumnName(column));
                        out.columnTypes << QString::fromStdString(block[column]->Type()->GetName());
                    }
                for (size_t row = 0; row < block.GetRowCount(); ++row) {
                    if (cancel->load()) return false;
                    if (out.rows.size() >= limits.maxRows) { out.truncated = true; return false; }
                    QVector<QVariant> values;
                    for (size_t column = 0; column < columns; ++column)
                        values << cellValue(block[column], row, buffer);
                    if (!buffer.add(std::move(values))) return false;
                }
                return true;
            });
        } catch (const std::exception& error) {
            healthy_ = false;
            if (out.error.isEmpty() && !cancel->load() && !out.truncated) out.error = QString::fromUtf8(error.what());
        }
        out.cancelled = cancel->load();
        return out;
    }
};
}
std::unique_ptr<Database> connectClickHouse(const ConnectionProfile& profile) {
    return std::make_unique<ClickHouseDatabase>(profile);
}
}
