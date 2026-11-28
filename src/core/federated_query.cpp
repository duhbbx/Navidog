#include "navidog/federated_query.hpp"
#include "navidog/catalog.hpp"
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <thread>

namespace navidog {
namespace {
using namespace sql::compiler;
using namespace sql::optimizer;
using Clock = std::chrono::steady_clock;

class ScanCancellation {
    std::mutex mutex_;
    std::condition_variable changed_;
    bool finished_ = false;
public:
    Cancellation token = cancellation();
private:
    std::thread watcher_;
public:
    ScanCancellation(Clock::time_point deadline, const Cancellation &external)
        : watcher_([this, deadline, external] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!finished_) {
                const auto now = Clock::now();
                if (now >= deadline || (external && external->load())) {
                    token->store(true);
                    return;
                }
                changed_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(5)));
            }
        }) {}
    ~ScanCancellation() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        changed_.notify_one();
        watcher_.join();
    }
};

[[noreturn]] void scanError(DiagnosticCode code, const char *message) {
    throw RelationScanError{code, message};
}

bool textColumn(const SqlType &type) {
    return type.family == TypeFamily::Character || type.family == TypeFamily::VarCharacter ||
        (type.family == TypeFamily::String && type.flavor == "TEXT");
}
bool signedFederatedInteger(const SqlType *type) {
    return type && type->family==TypeFamily::Integer && type->unsignedInteger==false &&
        (type->bits==16 || type->bits==32 || type->bits==64) &&
        !type->arrayDimensions && !type->sqliteAffinity && type->opaqueName.empty() &&
        type->flavor.empty();
}
bool mysqlFederation(DatabaseKind driver) {
    return driver == DatabaseKind::MySQL || driver == DatabaseKind::MariaDB;
}
bool mysqlOrderedFilters(const TableDefinition &table,const std::vector<RelationPredicate> &predicates) {
    return std::all_of(predicates.begin(),predicates.end(),[&](const auto &predicate) {
        return predicate.column<table.columns.size() && !predicate.numericConstant &&
               signedFederatedInteger(table.columns[predicate.column].sqlType.get());
    });
}

class NativeCursor final : public RelationCursor {
    QueryResult result_;
    const TableDefinition &table_;
    const std::vector<std::size_t> &projection_;
    DatabaseKind driver_;
    qsizetype index_ = 0;
public:
    NativeCursor(QueryResult result, const TableDefinition &table,
                 const std::vector<std::size_t> &projection, DatabaseKind driver)
        : result_(std::move(result)), table_(table), projection_(projection), driver_(driver) {}

    std::optional<LocalRow> next() override {
        if (index_ == result_.rows.size()) return std::nullopt;
        const auto &input = result_.rows.at(index_++);
        if (static_cast<std::size_t>(input.size()) != std::max<std::size_t>(1, projection_.size()))
            scanError(DiagnosticCode::InvalidQueryShape, "native federation returned an unexpected row width");
        LocalRow row;
        if (projection_.empty()) return row;
        row.reserve(projection_.size());
        for (qsizetype i = 0; i < input.size(); ++i) {
            const auto &cell = input.at(i);
            if (!cell.isValid() || cell.isNull()) {
                row.emplace_back(std::monostate{});
                continue;
            }
            const auto &type = *table_.columns.at(projection_.at(static_cast<std::size_t>(i))).sqlType;
            const auto family = type.family;
            if (textColumn(type)) {
                if ((driver_ != DatabaseKind::PostgreSQL && driver_ != DatabaseKind::SQLite) ||
                    cell.userType() != QMetaType::QString)
                    scanError(DiagnosticCode::InvalidQueryShape, "federation expected a text storage value");
                const auto utf8 = cell.toString().toUtf8();
                row.emplace_back(LocalText(std::string(utf8.constData(), utf8.size())));
                continue;
            }
            if (driver_ == DatabaseKind::SQLite) {
                if (cell.userType() != QMetaType::LongLong && cell.userType() != QMetaType::Int)
                    scanError(DiagnosticCode::InvalidQueryShape, "SQLite federation requires integer storage values");
                const auto number = static_cast<std::int64_t>(cell.toLongLong());
                if (family == TypeFamily::Boolean) {
                    if (number != 0 && number != 1)
                        scanError(DiagnosticCode::InvalidQueryShape, "SQLite Boolean federation values must be 0 or 1");
                    row.emplace_back(number == 1);
                } else row.emplace_back(number);
                continue;
            }
            const auto bytes = cell.toString().toUtf8();
            if (family == TypeFamily::Boolean) {
                if (bytes != "t" && bytes != "f")
                    scanError(DiagnosticCode::InvalidQueryShape, "PostgreSQL returned an unexpected Boolean value");
                row.emplace_back(bytes == "t");
            } else if (family == TypeFamily::Decimal && driver_ == DatabaseKind::PostgreSQL) {
                if (bytes == "NaN" || bytes == "Infinity" || bytes == "-Infinity")
                    scanError(DiagnosticCode::UnsupportedSyntax,"local federation supports finite PostgreSQL numeric values");
                auto numeric=LocalNumeric::parse(std::string_view(bytes.constData(),bytes.size()));
                if (!numeric)
                    scanError(DiagnosticCode::ResourceLimit,"PostgreSQL numeric value exceeds the local decimal range");
                row.emplace_back(std::move(*numeric));
            } else {
                std::int64_t number = 0;
                const auto *end = bytes.constData() + bytes.size();
                const auto value = std::from_chars(bytes.constData(), end, number);
                if (value.ec != std::errc{} || value.ptr != end)
                    scanError(DiagnosticCode::InvalidQueryShape, "native source returned a non-integer federation value");
                row.emplace_back(number);
            }
        }
        return row;
    }
};

class NativeRelationProvider final : public RelationProvider {
    const std::vector<FederatedTableBinding> &bindings_;
    Cancellation external_;
    const FederatedTableBinding *sourceFor(const TableDefinition &table) const {
        const auto matches = [&](const FederatedTableBinding &binding) {
            return binding.source == table.database && binding.schema == table.schema && binding.table == table.name;
        };
        const auto found = std::find_if(bindings_.begin(), bindings_.end(), matches);
        if (found == bindings_.end()) return nullptr;
        if (std::find_if(std::next(found), bindings_.end(), matches) != bindings_.end() ||
            !found->session || found->nativeTable.isEmpty() || found->nativeSchema.isEmpty())
            scanError(DiagnosticCode::InvalidQueryShape, "invalid or ambiguous native federation source");
        return &*found;
    }
public:
    NativeRelationProvider(const std::vector<FederatedTableBinding> &bindings, Cancellation external)
        : bindings_(bindings), external_(std::move(external)) {}

    bool supportsOrderedReads(const TableDefinition &table) const override {
        const auto *source = sourceFor(table);
        return source && (source->driver == DatabaseKind::PostgreSQL || mysqlFederation(source->driver));
    }

    bool supportsOrderedFilters(const TableDefinition &table,const std::vector<RelationPredicate> &predicates) const override {
        const auto *source=sourceFor(table);
        return source && (source->driver==DatabaseKind::PostgreSQL ||
                          (mysqlFederation(source->driver) && mysqlOrderedFilters(table,predicates)));
    }

    bool supportsOrderedKeyReads(const TableDefinition &table) const override {
        return supportsOrderedReads(table);
    }

    std::unique_ptr<RelationCursor> open(const RelationScanRequest &request) override {
        const auto &table = request.table;
        const auto *source = sourceFor(table);
        if (!source) return {};
        const bool pg = source->driver == DatabaseKind::PostgreSQL;
        const bool mysql = mysqlFederation(source->driver);
        if (!pg && !mysql && source->driver != DatabaseKind::SQLite)
            scanError(DiagnosticCode::UnsupportedSyntax, "native federation supports PostgreSQL, MySQL, MariaDB and SQLite sessions");
        if (!pg && request.inheritance != TableInheritance::Default)
            scanError(DiagnosticCode::UnsupportedSyntax, "this source cannot honor a PostgreSQL inheritance scan");
        if (request.shouldStop())
            scanError(DiagnosticCode::Stopped, "federated query cancelled before scan");
        if (request.page && ((!pg && !mysql) || request.page->order.empty() ||
            request.page->prefixRows>request.maxRows || (mysql && !mysqlOrderedFilters(table,request.predicates))))
            scanError(DiagnosticCode::InvalidQueryShape,"invalid ordered federation scan");
        const auto quote = [&](const QString &name) { return quoteIdentifier(name, source->driver); };
        QStringList columns;
        bool selectText = false;
        for (auto ordinal : request.columns) {
            const auto &column = table.columns.at(ordinal);
            if (column.sqlType->family==TypeFamily::Decimal && !pg)
                scanError(DiagnosticCode::UnsupportedSyntax,"native numeric federation requires PostgreSQL");
            if (textColumn(*column.sqlType)) {
                if (mysql)
                    scanError(DiagnosticCode::UnsupportedSyntax, "MySQL federation requires a native signed integer column");
                selectText = true;
            } else if (column.sqlType->family == TypeFamily::String) {
                scanError(DiagnosticCode::UnsupportedSyntax, "native federation requires text or character columns");
            }
            columns << quote(QString::fromStdString(column.name));
        }
        if (columns.empty()) columns << "1 AS navidog_federated_row";
        QString sql = "SELECT " + columns.join(", ") + " FROM ";
        if (request.inheritance == TableInheritance::Only) sql += "ONLY ";
        sql += quote(source->nativeSchema) + "." + quote(source->nativeTable);
        if (request.inheritance == TableInheritance::IncludeDescendants) sql += " *";
        QVector<QVariant> parameters;
        bool haveWhere=false;
        const auto predicateSeparator=[&] {
            sql+=haveWhere?" AND ":" WHERE ";
            haveWhere=true;
        };
        const auto parameter=[&](QVariant value,const QString &postgresType) {
            parameters.push_back(std::move(value));
            return pg?QStringLiteral("$%1::%2").arg(parameters.size()).arg(postgresType):
                QStringLiteral("CAST(? AS SIGNED)");
        };
        if (pg || mysql) {
            for (const auto &predicate:request.predicates) {
                const auto &column=table.columns.at(predicate.column);
                if (mysql && !signedFederatedInteger(column.sqlType.get())) continue;
                predicateSeparator();
                sql += quote(QString::fromStdString(column.name));
                const char *op = nullptr;
                switch (predicate.kind) {
                case RelationPredicateKind::Equal: op = "="; break;
                case RelationPredicateKind::NotEqual: op = "<>"; break;
                case RelationPredicateKind::Less: op = "<"; break;
                case RelationPredicateKind::LessEqual: op = "<="; break;
                case RelationPredicateKind::Greater: op = ">"; break;
                case RelationPredicateKind::GreaterEqual: op = ">="; break;
                case RelationPredicateKind::IsNull: sql += " IS NULL"; break;
                case RelationPredicateKind::IsNotNull: sql += " IS NOT NULL"; break;
                }
                if (op) {
                    const bool numeric=column.sqlType && column.sqlType->family==TypeFamily::Decimal;
                    QVariant value;
                    if (predicate.numericConstant) {
                        if (!numeric || predicate.constantIsNull)
                            scanError(DiagnosticCode::InvalidQueryShape,"invalid exact decimal scan binding");
                        value=QString::fromStdString(predicate.numericConstant->decimal());
                    } else if (!predicate.constantIsNull) {
                        value=QVariant::fromValue<qlonglong>(predicate.constant);
                    }
                    sql += QStringLiteral(" %1 %2").arg(QString::fromLatin1(op),
                        parameter(std::move(value),numeric?QStringLiteral("numeric"):QStringLiteral("bigint")));
                }
            }
        }
        if ((pg || mysql) && request.keySet) {
            const auto &keys = *request.keySet;
            if(keys.columns.empty() || keys.columns.size()>RelationKeySet::maxWidth ||
               keys.keys.size()>RelationKeySet::maxKeys)
                scanError(DiagnosticCode::InvalidQueryShape, "invalid federated scan key set");
            QStringList names,casts;
            for(const auto& entry:keys.columns) {
                if(entry.ordinal>=table.columns.size())
                    scanError(DiagnosticCode::InvalidQueryShape,"federated scan key is outside the column list");
                const auto& column=table.columns[entry.ordinal];
                const auto* type=column.sqlType.get();
                if(!signedFederatedInteger(type) || type->bits!=entry.width)
                    scanError(DiagnosticCode::InvalidQueryShape,"federated scan key requires an exact signed integer type");
                names<<quote(QString::fromStdString(column.name));
                switch(entry.width) {
                case 16: casts<<"smallint";break;
                case 32: casts<<"integer";break;
                case 64: casts<<"bigint";break;
                default: scanError(DiagnosticCode::InvalidQueryShape,"unsupported federated scan key width");
                }
            }
            predicateSeparator();
            if(keys.keys.empty())sql+="FALSE";
            else {
                const bool multi=keys.columns.size()>1;
                QStringList tuples;
                for(const auto& key:keys.keys) {
                    QStringList placeholders;
                    for(std::size_t i=0;i<keys.columns.size();++i) {
                        const auto width=keys.columns[i].width;
                        if(width<64) {
                            const auto edge=std::int64_t{1}<<(width-1);
                            if(key[i]< -edge || key[i]>=edge)
                                scanError(DiagnosticCode::InvalidQueryShape,"federated scan key exceeds the bound column width");
                        }
                        placeholders<<parameter(QVariant::fromValue<qlonglong>(key[i]),casts[int(i)]);
                    }
                    tuples<<(multi?"("+placeholders.join(", ")+")":placeholders.front());
                }
                sql+=(multi?"("+names.join(", ")+")":names.front())+" IN ("+tuples.join(", ")+")";
            }
        }
        if (request.page) {
            QStringList order;
            for (const auto &key : request.page->order) {
                const auto &column=table.columns.at(key.column);
                const auto name=quote(QString::fromStdString(column.name));
                if (mysql && !signedFederatedInteger(column.sqlType.get()))
                    scanError(DiagnosticCode::UnsupportedSyntax,"MySQL ordered federation requires a signed integer key");
                if (mysql && key.nullsFirst==key.descending) {
                    const bool excludesNull=std::any_of(request.predicates.begin(),request.predicates.end(),[&](const auto &predicate) {
                        return predicate.column==key.column && predicate.kind!=RelationPredicateKind::IsNull;
                    }) || (request.keySet && std::any_of(request.keySet->columns.begin(),request.keySet->columns.end(),
                          [&](const auto &field) { return field.ordinal==key.column; }));
                    if (!excludesNull) order << "("+name+" IS NULL)"+(key.nullsFirst?" DESC":" ASC");
                }
                auto item=name+(key.descending?" DESC":" ASC");
                if (pg) item+=key.nullsFirst?" NULLS FIRST":" NULLS LAST";
                order << item;
            }
            sql += " ORDER BY " + order.join(", ");
        }
        const auto limit = request.page ? request.page->prefixRows : request.maxRows + 1;
        sql += " LIMIT " + QString::number(static_cast<qulonglong>(limit));

        if (request.maxBytes < 256)
            scanError(DiagnosticCode::ResourceLimit, "native federation scan has insufficient byte budget");
        QueryLimits limits;
        limits.maxRows = static_cast<int>(request.maxRows);
        limits.maxBytes = static_cast<qint64>(request.maxBytes / 2);
        limits.maxCellBytes = selectText ? 4096 : 64;
        limits.rejectInvalidTextEncoding = selectText;
        ScanCancellation cancellation(request.deadline, external_);
        auto result = parameters.empty() ? source->session->execute(sql, limits, cancellation.token)
            : source->session->executeBound(sql, parameters, limits, cancellation.token);
        if (result.cancelled || cancellation.token->load()) {
            if (!(external_ && external_->load()) && Clock::now() >= request.deadline)
                scanError(DiagnosticCode::ResourceLimit, "native federation scan exceeded its time budget");
            scanError(DiagnosticCode::Stopped, "native federation scan cancelled");
        }
        if (!result.error.isEmpty())
            throw RelationScanError{DiagnosticCode::InvalidQueryShape, result.error.toStdString()};
        if (result.truncated || result.cellTruncated)
            scanError(DiagnosticCode::ResourceLimit, "native federation scan exceeded its row or byte budget");
        if (result.columns.size() != columns.size() || ((pg || mysql) && result.columnTypes.size() != columns.size()) ||
            !result.moreResults.empty())
            scanError(DiagnosticCode::InvalidQueryShape, "native federation scan schema changed");
        for (std::size_t index = 0; index < request.columns.size(); ++index) {
            const auto &column = table.columns.at(request.columns[index]);
            if (result.columns.at(index).toStdString() != column.name)
                scanError(DiagnosticCode::InvalidQueryShape, "native federation scan column name changed");
            if (pg) {
                const auto type = catalogTypeFromText(Dialect::PostgreSQL, result.columnTypes.at(index).toStdString());
                if (!type || type->family != column.sqlType->family || type->bits != column.sqlType->bits ||
                    type->arrayDimensions || !type->opaqueName.empty() ||
                    (type->family==TypeFamily::Decimal &&
                     (type->precision!=column.sqlType->precision || type->scale!=column.sqlType->scale)) ||
                    (textColumn(*column.sqlType) && (type->flavor != column.sqlType->flavor ||
                                                    type->length != column.sqlType->length)))
                    scanError(DiagnosticCode::InvalidQueryShape, "native federation scan column type changed");
            } else if (mysql) {
                const auto type = federatedMySqlInteger(result.columnTypes.at(index).toStdString());
                if (!type || column.sqlType->family != TypeFamily::Integer)
                    scanError(DiagnosticCode::UnsupportedSyntax, "MySQL federation requires a native signed integer column");
                if (column.sqlType->bits != type->bits)
                    scanError(DiagnosticCode::InvalidQueryShape, "MySQL federation scan column type changed");
            }
        }
        return std::make_unique<NativeCursor>(std::move(result), table, request.columns, source->driver);
    }
};
} // namespace

std::optional<SqlType> federatedMySqlInteger(std::string nativeType) {
    auto type = catalogTypeFromText(Dialect::MySQL, std::move(nativeType));
    if (!type || type->family != TypeFamily::Integer || type->unsignedInteger != false ||
        !type->bits || type->arrayDimensions || !type->opaqueName.empty())
        return {};
    // PostgreSQL has signed types at 16, 32 and 64 bits. TINYINT and
    // MEDIUMINT fit losslessly into the next wider representation.
    type->bits = *type->bits <= 16 ? 16 : *type->bits <= 32 ? 32 : 64;
    return type;
}

static InterpreterLimits sourceLimits(const Cancellation &cancel,InterpreterLimits limits) {
    limits.shouldStop = [cancel, original = std::move(limits.shouldStop)] {
        return (cancel && cancel->load()) || (original && original());
    };
    return limits;
}

sql::optimizer::Interpretation executeFederatedQuery(
    const ModelProgram &model, const std::vector<FederatedTableBinding> &bindings,
    const Cancellation &cancel, InterpreterLimits limits) {
    NativeRelationProvider provider(bindings, cancel);
    return interpretWithRelations(model, provider, sourceLimits(cancel,std::move(limits)));
}

sql::optimizer::Interpretation executeFederatedQuery(
    const PreparedLocalQuery &query,const LocalRow &parameters,
    const std::vector<FederatedTableBinding> &bindings,const Cancellation &cancel,
    InterpreterLimits limits) {
    NativeRelationProvider provider(bindings,cancel);
    return executeLocalQueryWithRelations(query,parameters,provider,sourceLimits(cancel,std::move(limits)));
}
} // namespace navidog
