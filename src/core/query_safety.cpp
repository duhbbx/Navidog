#include "navidog/query_safety.hpp"
#include "navidog/database.hpp"
#include "navidog/database_sql.hpp"
#include "navidog/document_commands.hpp"
#include "navidog/query_context.hpp"
#include "navidog/query_parameters.hpp"
#include "navidog/redis_commands.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace navidog {
namespace {
namespace sc = sql::compiler;
using Clock = std::chrono::steady_clock;

QString fromSql(std::string_view text) {
    return QString::fromUtf8(text.data(), int(text.size()));
}

std::optional<long long> integer(std::string_view value) {
    long long result;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) return result;
    return {};
}

// Only prove tautologies over built-in literal/boolean operators. Unknown
// (including nullable expressions) is never treated as false.
std::optional<bool> constantTruth(const sc::ASTContext& ast, sc::ExprId id,
                                  DatabaseKind kind, int depth = 0) {
    if (!id || depth >= 32) return {};
    const auto& e = ast.expression(id);
    using E = sc::ExprKind;
    if (e.kind == E::Boolean && kind != DatabaseKind::SQLite) return e.value == "TRUE";
    if (e.kind == E::Number && (isMySqlFamily(kind) || kind == DatabaseKind::SQLite)) {
        if (const auto value = integer(e.value)) return *value != 0;
    }
    if (e.kind == E::Unary && e.value == "NOT" && e.operands.size() == 1) {
        auto a = constantTruth(ast, e.operands.front(), kind, depth + 1);
        if (a) return !*a;
    }
    if (e.kind != E::Binary || e.operands.size() != 2) return {};
    if (e.value == "AND" || e.value == "OR") {
        const auto a = constantTruth(ast, e.operands[0], kind, depth + 1);
        const auto b = constantTruth(ast, e.operands[1], kind, depth + 1);
        const bool absorbing = e.value == "OR";
        if (a == absorbing || b == absorbing) return absorbing;
        if (a && b) return !absorbing;
        return {};
    }
    const auto& left = ast.expression(e.operands[0]);
    const auto& right = ast.expression(e.operands[1]);
    if (left.kind != E::Number || right.kind != E::Number) return {};
    const auto a = integer(left.value), b = integer(right.value);
    if (!a || !b) return {};
    if (e.value == "=") return *a == *b;
    if (e.value == "!=" || e.value == "<>") return *a != *b;
    if (e.value == "<") return *a < *b;
    if (e.value == "<=") return *a <= *b;
    if (e.value == ">") return *a > *b;
    if (e.value == ">=") return *a >= *b;
    return {};
}

bool previewablePredicate(const sc::ASTContext& ast, sc::ExprId id, int& budget, int depth = 0) {
    if (!id || depth >= 48 || --budget < 0) return false;
    const auto& e = ast.expression(id);
    if (e.query) return false;
    using E = sc::ExprKind;
    switch (e.kind) {
    case E::Name: case E::Boolean: case E::Null: case E::Number:
    case E::String: case E::Parameter: case E::TypedLiteral:
    case E::Between: case E::In: case E::IsNull: case E::TruthTest:
    case E::Unary: case E::Binary: case E::LikeEscape:
        return std::all_of(e.operands.begin(), e.operands.end(), [&](auto child) {
            return previewablePredicate(ast, child, budget, depth + 1);
        });
    default:
        return false;
    }
}

struct Head {
    QString operation;
    std::size_t begin = 0, end = 0;
    bool opaque = false;
};

Head mutationHead(std::string_view source, sql::Dialect dialect) {
    const auto tokens = sql::lex(source, dialect).tokens;
    int depth = 0;
    bool with = false, alter = false, first = true, explain = false, analyze = false, opening = false;
    bool procedural = false;
    Head possible;
    for (const auto& t : tokens) {
        const auto spelling = source.substr(t.begin, t.end - t.begin);
        if (t.kind == sql::TokenKind::Comment) {
            const auto prefix = spelling.substr(0, 4);
            if (dialect == sql::Dialect::MySQL &&
                (prefix.substr(0, 3) == "/*!" || prefix == "/*M!")) {
                static const QRegularExpression contained(
                    R"(\b(?:UPDATE|DELETE|TRUNCATE|DROP)\b)", QRegularExpression::CaseInsensitiveOption);
                if (contained.match(fromSql(spelling)).hasMatch())
                    possible = {"可执行注释", t.begin, t.end, true};
            }
            continue;
        }
        if (t.kind == sql::TokenKind::Symbol) {
            if (spelling == "(") ++depth;
            if (spelling == ")") --depth;
            opening = spelling == "(";
            continue;
        }
        const bool firstInCte = opening && with && depth == 1;
        opening = false;
        if (t.kind != sql::TokenKind::Word || (depth != 0 && !explain && !firstInCte)) continue;
        const auto word = fromSql(spelling).toUpper();
        if (first) {
            first = false;
            with = word == "WITH";
            alter = word == "ALTER";
            explain = word == "EXPLAIN";
            procedural = word == "BEGIN" || word == "DECLARE" || word == "DO" || word == "MERGE";
        }
        if (firstInCte && (word == "DELETE" || word == "UPDATE")) {
            possible = {word, t.begin, t.end, true};
            continue;
        }
        if (word == "ANALYZE" && explain) analyze = true;
        if (depth == 0 && (word == "DELETE" || word == "UPDATE" || word == "TRUNCATE") &&
            (!explain || analyze)) {
            if (with && possible.opaque) return possible;
            return {word, t.begin, t.end, alter || explain || procedural};
        }
        if (depth == 0 && word == "DROP") return {word, t.begin, t.end, alter || explain};
        if (!with && !alter && !explain && !procedural) break;
        if (!procedural && depth == 0 && (word == "SELECT" || word == "INSERT" || word == "MERGE")) break;
    }
    return possible;
}

void planRelationalProbe(QuerySafetyFinding& finding, std::string_view source,
                         const sc::Compilation& parsed, const sc::Statement& stmt) {
    if (!stmt.target || stmt.target->name.empty() || stmt.target->query ||
        !stmt.ctes.empty() || !stmt.deleteTargets.empty() || stmt.limit.offset ||
        stmt.limit.withTies || stmt.outputInto ||
        (stmt.targetInTables ? stmt.tables.size() != 1 : !stmt.tables.empty())) return;
    const auto& target = *stmt.target;
    if (target.sampling || target.clickHouseSampling || target.jsonTable ||
        target.pivot || target.unpivot || target.rowMatch ||
        target.postgresFunction != sc::PostgresRowFunction::None) return;
    if (stmt.limit.count) {
        const auto& limit = parsed.ast.expression(stmt.limit.count);
        const auto count = limit.kind == sc::ExprKind::Number ? integer(limit.value) : std::nullopt;
        if (!count || *count < 0) return;
        finding.writeLimit = *count;
    }
    int budget = 1024;
    if (stmt.where && !previewablePredicate(parsed.ast, stmt.where, budget)) return;
    // MySQL executable comments and server hints can change a copied fragment.
    for (const auto& token : parsed.tokens)
        if (token.kind == sc::TokenKind::Comment) {
            const auto raw = source.substr(token.range.begin, token.range.end - token.range.begin);
            if (raw.substr(0, 3) == "/*!" || raw.substr(0, 4) == "/*M!") return;
        }
    const auto tableRange = target.range;
    if (tableRange.begin >= tableRange.end || tableRange.end > source.size()) return;
    QByteArray predicate;
    if (stmt.where) {
        const auto range = parsed.ast.expression(stmt.where).range;
        std::size_t start = range.begin, end = range.end;
        for (const auto& token : parsed.tokens) {
            if (token.range.begin >= range.begin) break;
            if (token.range.begin < tableRange.end) continue;
            if (token.kind == sc::TokenKind::Word && token.value == "WHERE")
                start = token.range.end;
        }
        if (start == range.begin) return;
        // The parser's expression range may omit parentheses around either
        // operand. Match the copied delimiters before issuing a probe.
        int balance = 0;
        for (const auto& token : parsed.tokens) {
            if (token.range.begin < start || token.kind == sc::TokenKind::Comment) continue;
            if (token.range.begin >= range.end) {
                if (balance == 0 || token.kind != sc::TokenKind::Symbol || token.value != ")") break;
                end = token.range.end;
            }
            if (token.kind != sc::TokenKind::Symbol) continue;
            if (token.value == "(") ++balance;
            if (token.value == ")") --balance;
        }
        if (balance || end > source.size()) return;
        predicate = QByteArray(source.data() + start, int(end - start));
        finding.predicateOffset = int(start);
    }
    finding.table = QByteArray(source.data() + tableRange.begin, int(tableRange.end - tableRange.begin));
    finding.predicate = std::move(predicate);
    finding.impact.status = ImpactStatus::Pending;
}

bool safeMongoFilter(const QJsonValue& value, int depth = 0) {
    if (depth >= 32) return false;
    if (value.isArray()) {
        for (const auto& child : value.toArray())
            if (!safeMongoFilter(child, depth + 1)) return false;
    } else if (value.isObject()) {
        static const QStringList operators{"$and", "$or", "$nor", "$not", "$eq", "$ne",
            "$gt", "$gte", "$lt", "$lte", "$in", "$nin", "$exists", "$all", "$elemMatch",
            "$size", "$type", "$regex", "$options", "$oid", "$date", "$numberLong",
            "$numberInt", "$numberDouble", "$numberDecimal"};
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            if ((it.key().startsWith('$') && !operators.contains(it.key())) ||
                !safeMongoFilter(it.value(), depth + 1)) return false;
    }
    return true;
}

QVector<QuerySafetyFinding> inspectCommands(DatabaseKind kind, const QString& text,
                                            const QuerySafetyPolicy& policy) {
    QVector<QuerySafetyFinding> out;
    if (kind == DatabaseKind::Redis) {
        int offset = 0, index = 0, lineNumber = 1;
        for (const auto& line : text.split('\n')) {
            if (index >= 256) break;
            const auto parsed = parseRedisCommand(line);
            if (!parsed.arguments.isEmpty()) {
                const auto verb = QString::fromLatin1(parsed.arguments.front()).toUpper();
                const int keys = parsed.arguments.size() - 1;
                const bool flush = verb == "FLUSHALL" || verb == "FLUSHDB";
                if (flush || ((verb == "DEL" || verb == "UNLINK") && keys >= policy.rowThreshold)) {
                    QuerySafetyFinding item;
                    item.index = index;
                    item.line = lineNumber;
                    item.begin = offset + int(line.indexOf(line.trimmed()));
                    item.end = item.begin + verb.size();
                    item.operation = verb;
                    item.risk = QueryRisk::Destructive;
                    if (flush) {
                        item.matchCommand = verb == "FLUSHDB" ? QStringLiteral("DBSIZE") : QStringLiteral("INFO keyspace");
                        item.impact.status = ImpactStatus::Pending;
                    } else item.impact.detail = QString("命令最多指定了 %1 个键").arg(keys);
                    out << std::move(item);
                }
                ++index;
            }
            offset += int(line.size()) + 1;
            ++lineNumber;
        }
        return out;
    }
    const auto parsed = parseDocumentCommand(text);
    if (!parsed.error.isEmpty() || parsed.name.isEmpty()) return out;
    const bool deleting = parsed.name == "delete";
    const bool updating = parsed.name == "update";
    const bool dropping = parsed.name == "drop" || parsed.name == "dropDatabase";
    if (!deleting && !updating && !dropping) return out;
    const auto actions = parsed.options.value(deleting ? "deletes" : "updates").toArray();
    const auto multi = [deleting](const QJsonObject& item) {
        return deleting ? item.value("limit").toInt(-1) == 0 : item.value("multi").toBool();
    };
    QuerySafetyFinding item;
    item.begin = parsed.position;
    item.end = item.begin + parsed.name.size() + 2;
    item.line = text.left(item.begin).count('\n') + 1;
    item.operation = parsed.name;
    item.target = parsed.options.value(parsed.name).toString();
    if (dropping) item.risk = QueryRisk::Destructive;
    for (const auto& action : actions) {
        const auto row = action.toObject();
        if (multi(row) && row.value("q").isObject() && row.value("q").toObject().isEmpty())
            item.risk = QueryRisk::NoFilter;
    }
    if ((actions.size() == 1 && multi(actions.first().toObject())) || parsed.name == "drop") {
        const auto row = actions.isEmpty() ? QJsonObject{} : actions.first().toObject();
        const auto query = actions.isEmpty() ? QJsonValue(QJsonObject{}) : row.value("q");
        if (query.isObject() && !row.contains("collation") && safeMongoFilter(query)) {
            const auto command = [&](const QJsonValue& filter) {
                auto body = QJsonDocument(QJsonObject{{"query", filter},
                    {"limit", policy.rowThreshold}, {"maxTimeMS", policy.timeoutMs}}).toJson(QJsonDocument::Compact);
                const auto head = QJsonDocument(QJsonArray{item.target}).toJson(QJsonDocument::Compact);
                return QString::fromUtf8("{\"count\":" + head.mid(1, head.size()-2) + "," + body.mid(1));
            };
            item.matchCommand = command(query);
            item.totalCommand = command(QJsonObject{});
            item.impact.status = ImpactStatus::Pending;
        }
    }
    if (dropping || actions.size() > 1 || std::any_of(actions.begin(), actions.end(),
            [&](const QJsonValue& action) { return multi(action.toObject()); })) {
        if (item.impact.status != ImpactStatus::Pending)
            item.impact = {ImpactStatus::Unavailable, 0, -1, false, "此命令没有可安全生成的预查"};
        out << std::move(item);
    }
    return out;
}

QString limitedSelect(DatabaseKind kind, const QByteArray& table, const QByteArray& predicate, int cap) {
    const auto from = " FROM " + QString::fromUtf8(table);
    const auto where = predicate.isEmpty() ? QString{} : " WHERE (" + QString::fromUtf8(predicate) + "\n)";
    const auto select = QStringLiteral("SELECT 1 AS navidog_row");
    QString read;
    if (kind == DatabaseKind::SQLServer)
        read = QString("SELECT TOP (%1) 1 AS navidog_row").arg(cap) + from + where;
    else if (usesOracleSql(kind) || kind == DatabaseKind::Dameng)
        read = select + from + (where.isEmpty() ? " WHERE " : where + " AND ") +
            QString("ROWNUM <= %1").arg(cap);
    else if (kind == DatabaseKind::Db2)
        read = select + from + where + QString(" FETCH FIRST %1 ROWS ONLY").arg(cap);
    else
        read = select + from + where + QString(" LIMIT %1").arg(cap);
    return "SELECT COUNT(*) FROM (" + read + ") navidog_impact";
}

int countNative(Database& db, DatabaseKind kind, const QString& command, int cap,
                const Cancellation& stop) {
    auto result = db.execute(command, {1, 8192, 8192}, stop);
    if (result.cancelled || !result.error.isEmpty() || result.cellTruncated || result.rows.isEmpty() ||
        result.rows.front().isEmpty()) throw std::runtime_error("read-only count failed");
    // The Redis result also includes the path of the response element.
    const auto cell = result.rows.front().back();
    bool ok = false;
    qint64 count = 0;
    if (kind == DatabaseKind::MongoDb) {
        const auto value = QJsonDocument::fromJson(cell.toString().toUtf8()).object().value("n");
        count = value.isDouble() ? qint64(value.toDouble()) : value.toObject().value("$numberLong").toString().toLongLong(&ok);
        ok |= value.isDouble();
    } else if (command == "DBSIZE") {
        count = cell.toLongLong(&ok);
    } else {
        static const QRegularExpression keys(R"((?:^|\n)db\d+:keys=(\d+))");
        const auto info = cell.toString();
        auto matches = keys.globalMatch(info);
        ok = info.startsWith("# Keyspace");
        while (matches.hasNext() && count < cap) {
            bool valid = false;
            const auto number = matches.next().captured(1).toLongLong(&valid);
            ok &= valid;
            if (valid) count += std::min<qint64>(number, cap);
        }
    }
    if (!ok || count < 0) throw std::runtime_error("invalid count");
    return int(std::min<qint64>(count, cap));
}
} // namespace

bool mightNeedQuerySafety(DatabaseKind kind, const QString& text) {
    if (text.isEmpty()) return false;
    if (isCommandDatabase(kind)) return true;
    static const QRegularExpression candidate(
        R"(\b(?:UPDATE|DELETE|TRUNCATE|DROP)\b)", QRegularExpression::CaseInsensitiveOption);
    return candidate.match(text).hasMatch();
}

QVector<QuerySafetyFinding> inspectQuerySafety(DatabaseKind kind, const QString& text,
        const QuerySafetyPolicy& policy, const Cancellation& stop) {
    QVector<QuerySafetyFinding> out;
    if (!policy.enabled || !mightNeedQuerySafety(kind, text)) return out;
    if (isCommandDatabase(kind)) return inspectCommands(kind, text, policy);
    const auto bytes = text.toUtf8();
    const std::string_view source(bytes.constData(), std::size_t(bytes.size()));
    const auto dialect = scriptDialect(kind);
    const auto spans = sql::splitStatements(source, dialect);
    const auto frontend = compilerDialectFor(kind).value_or(sc::Dialect::Oracle);
    const auto deadline = Clock::now() + std::chrono::milliseconds(220);
    std::size_t lastByte = 0;
    int position = 0, line = 1;
    const auto advance = [&](std::size_t byte) {
        auto part = fromSql(source.substr(lastByte, byte - lastByte));
        position += int(part.size());
        line += int(part.count('\n'));
        lastByte = byte;
    };
    for (std::size_t i = 0; i < spans.size(); ++i) {
        if (stop && stop->load()) break;
        const auto span = spans[i];
        const auto original = sql::statementSource(source, span, dialect);
        const auto head = mutationHead(original, dialect);
        if (head.operation.isEmpty()) continue;
        advance(span.begin + head.begin);
        QuerySafetyFinding item;
        item.index = int(i);
        item.line = line;
        item.begin = position;
        advance(span.begin + head.end);
        item.end = position;
        item.operation = head.operation;
        item.risk = (head.operation == "TRUNCATE" || head.operation == "DROP") ?
            QueryRisk::Destructive : QueryRisk::Unverified;
        item.impact = {ImpactStatus::Unavailable, 0, -1, false, "此写入形式没有可安全生成的预查"};
        if (!head.opaque && original.size() <= 1024 * 1024 && Clock::now() < deadline) {
            sc::LanguageOptions options;
            options.extension = compilerExtensionFor(kind);
            options.limits.nodes = 12000;
            options.limits.tokens = 30000;
            options.limits.statements = 1;
            options.shouldStop = [stop, deadline] { return (stop && stop->load()) || Clock::now() >= deadline; };
            try {
                auto parsed = sc::compile(frontend, std::string(original), options);
                item.syntaxError = !parsed->syntaxValid && std::any_of(
                    parsed->diagnostics.begin(), parsed->diagnostics.end(), [](const auto& diagnostic) {
                        using Code = sc::DiagnosticCode;
                        return diagnostic.code == Code::ExpectedToken ||
                            diagnostic.code == Code::LexicalError || diagnostic.code == Code::InvalidEncoding;
                    });
                if (parsed->syntaxValid && parsed->statements.size() == 1) {
                    const auto& stmt = parsed->statements.front();
                    if (stmt.kind == sc::StatementKind::Update || stmt.kind == sc::StatementKind::Delete) {
                        if (stmt.target && !stmt.target->name.empty()) {
                            const auto& name = stmt.target->name;
                            item.target = fromSql(original.substr(name.front().range.begin,
                                name.back().range.end - name.front().range.begin)).left(240);
                        }
                        item.risk = !stmt.where ? QueryRisk::NoFilter :
                            constantTruth(parsed->ast, stmt.where, kind) == true ? QueryRisk::AlwaysTrue : QueryRisk::None;
                        planRelationalProbe(item, original, *parsed, stmt);
                    } else if (stmt.ddl && (stmt.kind == sc::StatementKind::Truncate ||
                               stmt.kind == sc::StatementKind::Drop)) {
                        const auto& ddl = *stmt.ddl;
                        if (!ddl.name.empty()) {
                            const auto range = sc::SourceRange{ddl.name.front().range.begin, ddl.name.back().range.end};
                            const auto table = original.substr(range.begin, range.end - range.begin);
                            item.target = fromSql(table).left(240);
                            if (stmt.kind == sc::StatementKind::Truncate && ddl.truncateTargets.size() == 1 &&
                                ddl.dropBehavior != sc::DropBehavior::Cascade) {
                                item.table = QByteArray(table.data(), int(table.size()));
                                if (ddl.truncateOnly) item.table.prepend("ONLY ");
                                item.impact.status = ImpactStatus::Pending;
                            }
                        }
                    }
                }
            } catch (const std::exception&) {
                // Keep the conservative lexical finding for unsupported SQL.
            }
        }
        out << std::move(item);
    }
    return out;
}

void measureQueryImpact(const ConnectionProfile& original, const QString& schema,
        const QuerySafetyPolicy& policy, const QVector<BoundQueryStatement>& bindings,
        QVector<QuerySafetyFinding>& findings, const Cancellation& stop, const ImpactQueryTrace& trace) {
    auto profile = original;
    const bool reproducible = profile.advanced.initialSql.trimmed().isEmpty();
    profile.advanced.initialSql.clear(); // Never replay user startup SQL.
    const int timeout = (policy.timeoutMs + 999) / 1000;
    const auto bounded = [timeout](int current) { return current > 0 ? std::min(current, timeout) : timeout; };
    profile.advanced.connectTimeout = bounded(profile.advanced.connectTimeout);
    profile.advanced.readTimeout = bounded(profile.advanced.readTimeout);
    profile.advanced.writeTimeout = bounded(profile.advanced.writeTimeout);
    if (isMySqlFamily(profile.driver) || usesNativePostgres(profile.driver) ||
        profile.driver == DatabaseKind::SQLite || profile.driver == DatabaseKind::DuckDb)
        profile.readOnly = true;
    const int cap = policy.rowThreshold;
    std::unique_ptr<Database> db;
    for (auto& item : findings) {
        if (item.impact.status != ImpactStatus::Pending) continue;
        item.impact = {ImpactStatus::Unavailable, 0, -1, false, "未取得影响范围"};
        const auto bound = std::find_if(bindings.begin(), bindings.end(),
            [&](const auto& statement) { return statement.index == item.index; });
        QVector<QVariant> values;
        auto predicate = item.predicate;
        if (!predicate.isEmpty() && !rewriteQueryPredicate(profile.driver, predicate,
                item.predicateOffset, bound == bindings.end() ? nullptr : &*bound, values)) {
            item.impact.detail = "需要已绑定且可映射的条件参数";
            continue;
        }
        if (!reproducible || item.index > 0 || stop->load()) {
            item.impact.detail = item.index > 0 ? "批次前序语句可能改变数据或会话，单独运行此句可预查" :
                "当前会话环境无法复现或预查已超时";
            continue;
        }
        try {
            if (!db) {
                db = connectDatabase(profile);
                if (!schema.isEmpty()) applyQuerySchema(*db, profile, schema, stop);
                if (profile.driver == DatabaseKind::Oracle) {
                    const auto readOnly = db->execute("SET TRANSACTION READ ONLY", {}, stop);
                    if (!readOnly.error.isEmpty() || readOnly.cancelled) throw std::runtime_error("read-only transaction unavailable");
                }
            }
            const auto count = [&](bool filtered, int limit) {
                if (stop->load()) throw std::runtime_error("impact preview stopped");
                if (limit == 0) return 0;
                const bool native = !item.matchCommand.isEmpty();
                const auto sql = native ? (filtered ? item.matchCommand : item.totalCommand) :
                    limitedSelect(profile.driver, item.table, filtered ? predicate : QByteArray{}, limit);
                if (trace) trace(item.index, filtered ? ImpactQueryKind::MatchingRows : ImpactQueryKind::TotalRows,
                                 queryTraceText(sql));
                if (native) return countNative(*db, profile.driver, sql, cap, stop);
                const QueryLimits limits{1, 4096, 64};
                const auto result = filtered && !values.isEmpty() ? db->executeBound(sql, values, limits, stop) :
                    db->execute(sql, limits, stop);
                if (!result.error.isEmpty() || result.cancelled || stop->load() ||
                    result.rows.size() != 1 || result.rows.front().size() != 1)
                    throw std::runtime_error("impact read failed");
                bool converted = false;
                const auto rows = result.rows.front().front().toLongLong(&converted);
                if (!converted || rows < 0 || rows > limit) throw std::runtime_error("invalid impact count");
                return int(rows);
            };
            const auto matchCap = int(std::min<qint64>(cap, item.writeLimit.value_or(cap)));
            const auto matched = count(true, matchCap);
            item.impact = {ImpactStatus::Measured, matched, -1,
                matched >= cap && item.writeLimit.value_or(qint64(cap) + 1) > cap, {}};
            if (matched > 0 && matched < cap &&
                (!item.totalCommand.isEmpty() || !item.table.isEmpty())) {
                const int totalLimit = item.totalCommand.isEmpty()
                    ? std::max(cap, matched * 100 / policy.percentThreshold + 1) : cap;
                const bool sameQuery = (predicate.isEmpty() && !item.table.isEmpty() && !item.writeLimit) ||
                    (!item.totalCommand.isEmpty() && item.totalCommand == item.matchCommand);
                const auto total = sameQuery ? matched : count(false, totalLimit);
                if (total >= matched && total < totalLimit) item.impact.total = total;
            }
        } catch (const std::exception&) {
            item.impact.detail = item.impact.status == ImpactStatus::Measured
                ? "未取得目标总量" : "预查失败、超时或没有读取权限";
            db.reset();
        }
    }
}

QString queryRiskMessage(QueryRisk risk) {
    switch (risk) {
    case QueryRisk::NoFilter: return "缺少筛选条件，可能影响整个表或集合。";
    case QueryRisk::AlwaysTrue: return "筛选条件恒为真，可能影响整个表。";
    case QueryRisk::Destructive: return "此语句可能清除数据或删除对象。";
    case QueryRisk::Unverified: return "检测到写入语句，本地无法确认其限制条件。";
    default: return {};
    }
}

QString queryImpactMessage(const QueryImpact& impact) {
    if (impact.status != ImpactStatus::Measured)
        return impact.detail.isEmpty() ? QStringLiteral("未取得影响范围") : impact.detail;
    auto message = impact.atLeast ? QString("预查至少匹配 %1 条").arg(impact.matched) :
        QString("预查匹配 %1 条").arg(impact.matched);
    if (impact.total > 0)
        message += QString(" / 共 %1 条（%2%）").arg(impact.total)
            .arg(qint64(impact.matched) * 100 / impact.total);
    if (!impact.detail.isEmpty()) message += " · " + impact.detail;
    return message;
}

bool largeQueryImpact(const QueryImpact& impact, const QuerySafetyPolicy& policy) {
    return impact.status == ImpactStatus::Measured &&
        (impact.matched >= policy.rowThreshold ||
        (impact.total > 0 && qint64(impact.matched) * 100 >= qint64(impact.total) * policy.percentThreshold));
}

bool requiresQueryConfirmation(const QuerySafetyFinding& finding, const QuerySafetyPolicy& policy) {
    return policy.enabled && policy.confirm && (finding.risk != QueryRisk::None ||
        largeQueryImpact(finding.impact, policy) ||
        (policy.previewBeforeRun && policy.confirmUnmeasured &&
         finding.impact.status != ImpactStatus::Measured));
}
} // namespace navidog
