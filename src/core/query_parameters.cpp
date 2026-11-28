#include "navidog/query_parameters.hpp"
#include "navidog/database_sql.hpp"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>

namespace navidog {
namespace {
using namespace sql::compiler;
using ParameterType = QueryParameterType;
constexpr int maxParameters = 256;
constexpr int maxOccurrences = 8192;

[[noreturn]] void fail(const QString& reason) {
    throw std::runtime_error(reason.toStdString());
}
QString fromSql(std::string_view source) {
    return QString::fromUtf8(source.data(), int(source.size()));
}
bool digit(char ch) { return ch >= '0' && ch <= '9'; }

struct Marker {
    std::size_t begin, end;
    QString spelling;
    int parameter = -1;
};

bool questionMarkers(DatabaseKind kind) {
    return kind == DatabaseKind::SQLite || kind == DatabaseKind::DuckDb ||
        kind == DatabaseKind::SQLServer || isMySqlFamily(kind) || isAnalyticalMySql(kind) ||
        (usesJdbcDriver(kind) && !usesPostgresCompatibleSql(kind));
}
bool colonMarkers(DatabaseKind kind) {
    return kind == DatabaseKind::SQLite || usesOracleSql(kind) || kind == DatabaseKind::Dameng;
}
bool dollarMarkers(DatabaseKind kind) {
    return usesPostgresCompatibleSql(kind) || kind == DatabaseKind::SQLite || kind == DatabaseKind::DuckDb;
}

std::optional<Marker> markerAt(DatabaseKind kind, std::string_view source, const sql::Token& token) {
    if (token.kind != sql::TokenKind::Symbol) return {};
    const auto spelling = source.substr(token.begin, token.end - token.begin);
    const auto marker = spelling.front();
    if (spelling.size() != 1) return {};
    auto end = token.end;
    if (marker == '?' && questionMarkers(kind)) {
        if (kind == DatabaseKind::SQLite || isMySqlFamily(kind))
            while (end < source.size() && digit(source[end])) ++end;
        return Marker{token.begin, end, fromSql(source.substr(token.begin, end - token.begin))};
    }
    const bool pgNumber = marker == '$' && usesPostgresCompatibleSql(kind);
    if (!(marker == ':' && colonMarkers(kind)) && !(marker == '$' && dollarMarkers(kind)) &&
        !(marker == '@' && kind == DatabaseKind::SQLite)) return {};
    const auto part = [&](unsigned char ch) {
        return digit(char(ch)) || (!pgNumber && (std::isalpha(ch) || ch == '_' || ch == '$' || ch >= 128));
    };
    while (end < source.size() && part(static_cast<unsigned char>(source[end]))) ++end;
    if (end == token.end) return {};
    if (kind == DatabaseKind::SQLite && marker == '$') {
        while (source.substr(end, 2) == "::") {
            end += 2;
            while (end < source.size() && part(static_cast<unsigned char>(source[end]))) ++end;
        }
        if (end < source.size() && source[end] == '(') {
            const auto close = source.find(')', end + 1);
            if (close != std::string_view::npos &&
                source.substr(end + 1, close - end - 1).find_first_of(" \t\r\n") == std::string_view::npos)
                end = close + 1;
        }
    }
    return Marker{token.begin, end, fromSql(source.substr(token.begin, end - token.begin))};
}

ParameterType category(const SqlType& type) {
    if (type.arrayDimensions) return ParameterType::Unknown;
    // SQLite tracks the declared spelling separately from manifest runtime types.
    if (type.sqliteAffinity && !type.opaqueName.empty()) {
        const auto name = QString::fromStdString(type.opaqueName.front()).toUpper();
        if (name.contains("INT")) return ParameterType::Integer;
        if (name.contains("CHAR") || name.contains("CLOB") || name.contains("TEXT")) return ParameterType::Text;
        if (name.contains("BLOB")) return ParameterType::Binary;
        if (name.contains("REAL") || name.contains("FLOA") || name.contains("DOUB")) return ParameterType::Real;
        return ParameterType::ExactNumber;
    }
    switch (type.family) {
    case TypeFamily::Boolean: return ParameterType::Boolean;
    case TypeFamily::Integer:
        return type.unsignedInteger.value_or(false) && type.bits.value_or(64) >= 64 ?
            ParameterType::ExactNumber : ParameterType::Integer;
    case TypeFamily::Numeric:
    case TypeFamily::Decimal: return ParameterType::ExactNumber;
    case TypeFamily::Floating: return ParameterType::Real;
    case TypeFamily::String:
    case TypeFamily::Character:
    case TypeFamily::VarCharacter: return ParameterType::Text;
    case TypeFamily::Date: return ParameterType::Date;
    case TypeFamily::Time: return ParameterType::Time;
    case TypeFamily::Timestamp: return ParameterType::Timestamp;
    case TypeFamily::Bytes:
    case TypeFamily::Binary:
    case TypeFamily::VarBinary: return ParameterType::Binary;
    default: return ParameterType::Unknown;
    }
}

struct TypeHint {
    ParameterType type = ParameterType::Unknown;
    QString reason;
    bool explicitType = false, conflict = false;
    void add(ParameterType candidate, const QString& source, bool exact) {
        if (candidate == ParameterType::Unknown || (explicitType && !exact)) return;
        if (type == ParameterType::Unknown || (exact && !explicitType)) {
            type = candidate;
            reason = source;
            explicitType = exact;
            conflict = false;
        } else if (type != candidate) conflict = true;
    }
};

// An unimplemented clause elsewhere in a statement must not lose an explicit cast.
void inferExplicitCast(Dialect dialect, std::string_view source, const std::vector<sql::Token>& tokens,
                       std::size_t position, const Marker& marker, TypeHint& hint) {
    const auto spelling = [&](std::size_t i) { return source.substr(tokens[i].begin, tokens[i].end - tokens[i].begin); };
    std::size_t next = position + 1;
    while (next < tokens.size() && tokens[next].begin < marker.end) ++next;
    if (next >= tokens.size()) return;
    std::size_t begin = next + 1, end = begin;
    if (position >= 2 && spelling(position - 1) == "(" &&
        fromSql(spelling(position - 2)).compare("CAST", Qt::CaseInsensitive) == 0 &&
        fromSql(spelling(next)).compare("AS", Qt::CaseInsensitive) == 0) {
        int depth = 0;
        while (end < tokens.size() && end - begin < 32) {
            const auto word = spelling(end);
            if (word == ")" && !depth) break;
            if (word == "(") ++depth;
            if (word == ")") --depth;
            ++end;
        }
        if (end == tokens.size() || spelling(end) != ")") return;
    } else if (dialect == Dialect::PostgreSQL && spelling(next) == "::") {
        if (end >= tokens.size() || (tokens[end].kind != sql::TokenKind::Word &&
                                      tokens[end].kind != sql::TokenKind::Identifier)) return;
        ++end;
        while (end + 1 < tokens.size() && spelling(end) == "." &&
               (tokens[end + 1].kind == sql::TokenKind::Word || tokens[end + 1].kind == sql::TokenKind::Identifier))
            end += 2;
        if (end < tokens.size() && spelling(end) == "(") {
            while (end < tokens.size() && end - begin < 16 && spelling(end) != ")") ++end;
            if (end == tokens.size() || spelling(end) != ")") return;
            ++end;
        }
        if (end < tokens.size()) {
            const auto following = fromSql(spelling(end)).toUpper();
            if (following == "PRECISION" || following == "VARYING") ++end;
            else if (following == "WITH" || following == "WITHOUT") {
                if (end + 2 >= tokens.size() || fromSql(spelling(end + 1)).toUpper() != "TIME" ||
                    fromSql(spelling(end + 2)).toUpper() != "ZONE") return;
                end += 3;
            }
        }
        if (end < tokens.size() && spelling(end) == "[") return;
    } else return;
    if (begin >= end) return;
    const auto raw = source.substr(tokens[begin].begin, tokens[end - 1].end - tokens[begin].begin);
    if (auto type = catalogTypeFromText(dialect, std::string(raw)))
        hint.add(category(*type), "显式类型转换", true);
}

void inferParameters(DatabaseKind kind, std::string input, const QHash<int,int>& byOffset,
                     const NameResolutionContext& names, const CatalogSnapshot* catalog,
                     std::chrono::steady_clock::time_point deadline, QVector<TypeHint>& hints) {
    if (input.size() > 512 * 1024 || std::chrono::steady_clock::now() >= deadline) return;
    LanguageOptions options;
    options.extension = compilerExtensionFor(kind);
    options.limits.tokens = 24000;
    options.limits.nodes = 16000;
    options.limits.diagnostics = 1;
    options.shouldStop = [deadline] { return std::chrono::steady_clock::now() >= deadline; };
    const auto compilation = compile(names.dialect, std::move(input), options, catalog, names);
    if (!compilation->syntaxValid) return;
    const auto& model = compilation->model;
    for (const auto& query : model.queries) {
        const auto hint = [&](ExprId id, ParameterType type, const QString& reason, bool exact = false) {
            if (!id || id >= query.expressions.size()) return;
            const auto& expression = query.expressions[id];
            if (expression.kind != ExprKind::Parameter || expression.localSymbol) return;
            const auto parameter = byOffset.value(int(expression.origin.begin), -1);
            if (parameter >= 0) hints[parameter].add(type, reason, exact);
        };
        const auto expressionType = [&](ExprId id) {
            return id && id < query.expressions.size() ?
                category(model.types.get(query.expressions[id].sqlType)) : ParameterType::Unknown;
        };
        hint(query.limit, ParameterType::Integer, "LIMIT / TOP / FETCH");
        hint(query.offset, ParameterType::Integer, "OFFSET");
        for (const auto& expression : query.expressions) {
            const auto& args = expression.operands;
            if (args.empty()) continue;
            if ((expression.kind == ExprKind::Cast || expression.kind == ExprKind::Convert) && expression.castType) {
                const auto type = expressionType(ExprId(&expression - query.expressions.data()));
                hint(args.front(), type, "显式类型转换", true);
            } else if (expression.kind == ExprKind::Binary && args.size() == 2) {
                const auto& op = expression.value;
                if (op == "LIKE" || op == "ILIKE" || op == "NOT LIKE" || op == "NOT ILIKE") {
                    hint(args[0], ParameterType::Text, "字符串匹配");
                    hint(args[1], ParameterType::Text, "字符串匹配");
                } else if (op == "=" || op == "<>" || op == "!=" || op == "<" || op == ">" ||
                           op == "<=" || op == ">=" || op == "IS DISTINCT FROM" || op == "IS NOT DISTINCT FROM") {
                    hint(args[0], expressionType(args[1]), "比较表达式");
                    hint(args[1], expressionType(args[0]), "比较表达式");
                }
            } else if (expression.kind == ExprKind::Between || expression.kind == ExprKind::In) {
                const auto type = expressionType(args.front());
                for (std::size_t i = 1; i < args.size(); ++i) hint(args[i], type, "比较表达式");
            }
        }
        const auto assignments = [&](const std::vector<Assignment>& list) {
            for (const auto& assignment : list)
                if (assignment.targets.size() == 1)
                    hint(assignment.value, expressionType(assignment.targets.front()), "赋值目标字段");
        };
        assignments(query.assignments);
        assignments(query.conflict.assignments);
        for (const auto& clause : query.mergeClauses) assignments(clause.assignments);

        const auto* target = query.target && query.target->definition ? query.target->definition.get() : nullptr;
        if (!target && query.targetTable && *query.targetTable < query.tables.size())
            target = query.tables[*query.targetTable].definition.get();
        if (!target) continue;
        for (const auto& row : query.rows) {
            for (std::size_t column = 0; column < row.size(); ++column) {
                const ColumnDefinition* definition = nullptr;
                if (query.columns.empty() && column < target->columns.size())
                    definition = &target->columns[column];
                else if (column < query.columns.size()) {
                    const auto& name = query.columns[column];
                    for (const auto& candidate : target->columns)
                        if (candidate.name == name) { definition = &candidate; break; }
                }
                if (definition)
                    hint(row[column], category(definition->sqlType ? *definition->sqlType :
                                                categoryType(definition->type)), "INSERT 目标字段");
            }
        }
    }
}

QByteArray driverMarker(DatabaseKind kind, int occurrence) {
    if (usesNativePostgres(kind)) return "$" + QByteArray::number(occurrence);
    if (kind == DatabaseKind::Oracle) return ":" + QByteArray::number(occurrence);
    return "?";
}
QByteArray compilerMarker(Dialect dialect, int occurrence) {
    if (dialect == Dialect::PostgreSQL) return "$" + QByteArray::number(occurrence);
    if (dialect == Dialect::Oracle) return ":" + QByteArray::number(occurrence);
    if (dialect == Dialect::SQLServer) return "@navidogBind" + QByteArray::number(occurrence);
    return "?";
}
}

bool mayHaveQueryParameters(DatabaseKind kind, const QString& text) {
    return (questionMarkers(kind) && text.contains('?')) || (colonMarkers(kind) && text.contains(':')) ||
        (dollarMarkers(kind) && text.contains('$')) ||
        (kind == DatabaseKind::SQLite && text.contains('@'));
}

QueryParameterPlan inspectQueryParameters(DatabaseKind kind, const QString& text, const QString& database,
                                          const QString& schema, MetadataSnapshotPtr metadata) {
    QueryParameterPlan plan;
    if (!mayHaveQueryParameters(kind, text)) return plan;
    const auto bytes = text.toUtf8();
    const std::string_view source(bytes.constData(), std::size_t(bytes.size()));
    const auto dialect = scriptDialect(kind);
    const auto statements = sql::splitStatements(source, dialect);
    const auto parser = compilerDialectFor(kind).value_or(Dialect::Oracle);
    const NameResolutionContext names{database.toStdString(), schema.toStdString(), parser};
    std::unique_ptr<MetadataCatalog> catalog;
    if (metadata && metadata->driver == kind) catalog = std::make_unique<MetadataCatalog>(std::move(metadata));
    const SourceBuffer positions{std::string(source)};
    QHash<QString, int> namedParameters;
    QVector<TypeHint> hints;
    int markerCount = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(220);
    for (std::size_t index = 0; index < statements.size(); ++index) {
        const auto span = statements[index];
        const auto original = sql::statementSource(source, span, dialect);
        auto tokens = sql::lex(original, dialect).tokens;
        tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [](const auto& token) {
            return token.kind == sql::TokenKind::Comment;
        }), tokens.end());
        QString first;
        if (!tokens.empty()) {
            const auto& token = tokens.front();
            first = fromSql(original.substr(token.begin, token.end - token.begin)).toUpper();
        }
        // PREPARE declares its own parameters. Routine definitions own their bodies.
        if (first == "PREPARE" || (span.compound && first != "DECLARE" && first != "BEGIN")) continue;

        QHash<QString,int> namedSlots;
        QHash<int,int> numberedSlots;
        int lastSlot = 0, anonymous = 0;
        QVector<Marker> markers;
        std::size_t previous = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            if (token.begin < previous) continue;
            auto candidate = markerAt(kind, original, token);
            if (!candidate) continue;
            auto marker = std::move(*candidate);
            previous = marker.end;
            if (++markerCount > maxOccurrences) fail("SQL 参数引用过多，请分批运行");
            if (kind == DatabaseKind::OceanBaseOracle)
                fail("当前 OceanBase Oracle 驱动暂未提供参数绑定");
            if (kind == DatabaseKind::Oracle || kind == DatabaseKind::Dameng) {
                std::size_t next = i + 1;
                while (next < tokens.size() && tokens[next].begin < marker.end) ++next;
                const auto assigned = next < tokens.size() && original.substr(tokens[next].begin, 2) == ":=";
                const auto previousIsInto = i && fromSql(original.substr(tokens[i - 1].begin,
                    tokens[i - 1].end - tokens[i - 1].begin)).compare("INTO", Qt::CaseInsensitive) == 0;
                if (assigned || previousIsInto) fail("当前仅支持输入参数。请使用 SQL 或 PL/SQL 局部变量接收输出值。");
            }
            const bool sqlite = kind == DatabaseKind::SQLite;
            const bool pg = usesPostgresCompatibleSql(kind);
            const auto sigil = marker.spelling[0];
            const bool bare = marker.spelling == "?";
            const bool indexed = (sigil == '?' && !bare) || (sigil == '$' && (pg ||
                (kind == DatabaseKind::DuckDb && marker.spelling.size() > 1 &&
                 digit(marker.spelling[1].toLatin1()))));
            const auto key = usesOracleSql(kind) || kind == DatabaseKind::Dameng ?
                marker.spelling.toUpper() : marker.spelling;
            int slot = 0;
            if (indexed) {
                bool ok = false;
                slot = marker.spelling.mid(1).toInt(&ok);
                if (!ok || slot < 1 || slot > 65535 || (pg && marker.spelling[1] == '0'))
                    fail(QString("第 %1 条语句的参数编号无效（%2）").arg(index + 1).arg(marker.spelling));
                if (sqlite) lastSlot = qMax(slot, lastSlot);
            } else if (sqlite) {
                if (bare) slot = ++lastSlot;
                else {
                    slot = namedSlots.value(key);
                    if (!slot) {
                        slot = ++lastSlot;
                        namedSlots.insert(key, slot);
                    }
                }
            }
            const bool named = !bare && !indexed;
            int parameter = slot ? numberedSlots.value(slot, -1) : -1;
            if (parameter < 0 && named) parameter = namedParameters.value(key, -1);
            const auto position = positions.position(span.begin + marker.begin);
            if (parameter < 0) {
                if (plan.parameters.size() >= maxParameters) fail("单批最多输入 256 个 SQL 参数，请分批运行");
                parameter = int(plan.parameters.size());
                const auto display = bare ? QString("?%1").arg(++anonymous) : marker.spelling;
                plan.parameters << QueryParameter{display, ParameterType::Unknown, {}, {}};
                hints << TypeHint{};
            }
            if (named) namedParameters.insert(key, parameter);
            if (slot) numberedSlots.insert(slot, parameter);
            plan.parameters[parameter].locations << QueryParameterLocation{int(index + 1),
                int(position.line), int(position.byteColumn)};
            marker.parameter = parameter;
            if (std::chrono::steady_clock::now() < deadline)
                inferExplicitCast(parser, original, tokens, i, marker, hints[parameter]);
            markers << std::move(marker);
        }
        if (markers.isEmpty()) continue;
        ParameterizedStatement statement;
        statement.index = int(index);
        statement.original = fromSql(original);
        QByteArray rewritten, analysis;
        QHash<int,int> byOffset, postgresSlots;
        previous = 0;
        std::size_t tokenIndex = 0;
        const bool jdbcPostgres = usesJdbcDriver(kind) && usesPostgresCompatibleSql(kind);
        const auto copyToDriver = [&](std::size_t until) {
            auto start = previous;
            if (jdbcPostgres) {
                while (tokenIndex < tokens.size() && tokens[tokenIndex].begin < until) {
                    const auto& token = tokens[tokenIndex++];
                    if (token.begin < start || token.kind != sql::TokenKind::Symbol ||
                        original.substr(token.begin, token.end - token.begin) != "?") continue;
                    rewritten.append(original.data() + start, int(token.begin - start));
                    rewritten += "??";
                    start = token.end;
                }
            }
            rewritten.append(original.data() + start, int(until - start));
        };
        for (const auto& marker : markers) {
            int slot = int(statement.parameters.size()) + 1;
            if (usesNativePostgres(kind)) {
                const auto existing = postgresSlots.constFind(marker.parameter);
                if (existing == postgresSlots.cend()) {
                    postgresSlots.insert(marker.parameter, slot);
                    statement.parameters << marker.parameter;
                } else slot = *existing;
            } else statement.parameters << marker.parameter;
            const auto length = int(marker.begin - previous);
            copyToDriver(marker.begin);
            analysis.append(original.data() + previous, length);
            byOffset.insert(int(analysis.size()), marker.parameter);
            rewritten += driverMarker(kind, slot);
            analysis += compilerMarker(parser, slot);
            previous = marker.end;
        }
        copyToDriver(original.size());
        analysis.append(original.data() + previous, int(original.size() - previous));
        statement.sql = QString::fromUtf8(rewritten);
        plan.statements << std::move(statement);
        inferParameters(kind, analysis.toStdString(), byOffset, names,
                        catalog && catalog->current() ? catalog.get() : nullptr, deadline, hints);
    }
    for (int i = 0; i < plan.parameters.size(); ++i) {
        const auto& hint = hints[i];
        if (hint.conflict) plan.parameters[i].evidence = "各处用途的类型有冲突，请指定类型";
        else {
            plan.parameters[i].type = hint.type;
            plan.parameters[i].evidence = hint.reason;
        }
    }
    return plan;
}

QVector<BoundQueryStatement> bindQueryParameters(const QueryParameterPlan& plan, const QVector<QVariant>& values) {
    if (plan.parameters.size() != values.size()) fail("SQL 参数数量不匹配");
    QVector<BoundQueryStatement> statements;
    statements.reserve(plan.statements.size());
    for (const auto& statement : plan.statements) {
        BoundQueryStatement bound{statement.index, statement.original, statement.sql, {}};
        qint64 bytes = 0;
        for (auto index : statement.parameters) {
            const auto& value = values.at(index);
            bytes += value.userType() == QMetaType::QByteArray ? value.toByteArray().size() :
                value.toString().size() * qint64(4);
            if (bytes > 16 * 1024 * 1024) fail("单条语句的绑定参数超过 16 MiB，请分批执行");
            bound.values << value;
        }
        statements << std::move(bound);
    }
    return statements;
}

QString queryParameterTypeName(QueryParameterType type) {
    switch (type) {
    case ParameterType::Unknown: return "待指定";
    case ParameterType::Text: return "文本";
    case ParameterType::Integer: return "整数";
    case ParameterType::ExactNumber: return "精确数值";
    case ParameterType::Real: return "浮点数";
    case ParameterType::Boolean: return "布尔值";
    case ParameterType::Date: return "日期";
    case ParameterType::Time: return "时间";
    case ParameterType::Timestamp: return "日期时间";
    case ParameterType::Binary: return "二进制（十六进制）";
    }
    return {};
}

std::optional<QVariant> parseQueryParameterValue(QueryParameterType type, const QString& input, bool isNull) {
    if (isNull) return QVariant{};
    if (type == ParameterType::Unknown) return {};
    if (type == ParameterType::Text) return input.isEmpty() ? QStringLiteral("") : input;
    const auto text = input.trimmed();
    if (type == ParameterType::Integer) {
        bool ok = false;
        const auto number = text.toLongLong(&ok);
        if (ok) return QVariant::fromValue(number);
    } else if (type == ParameterType::ExactNumber) {
        static const QRegularExpression pattern(QStringLiteral(R"(^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$)"));
        if (pattern.match(text).hasMatch()) return text;
    } else if (type == ParameterType::Real) {
        bool ok = false;
        const auto number = text.toDouble(&ok);
        if (ok && std::isfinite(number)) return number;
    } else if (type == ParameterType::Boolean) {
        const auto value = text.toLower();
        if (value == "true" || value == "1") return QVariant(true);
        if (value == "false" || value == "0") return QVariant(false);
    } else if (type == ParameterType::Binary) {
        static const QRegularExpression hex(QStringLiteral(R"(^(?:[0-9a-fA-F]{2})*$)"));
        if (hex.match(text).hasMatch())
            return text.isEmpty() ? QByteArray("", 0) : QByteArray::fromHex(text.toLatin1());
    } else if (type == ParameterType::Date) {
        if (QDate::fromString(text, Qt::ISODate).isValid()) return text;
    } else if (type == ParameterType::Time) {
        if (QTime::fromString(text, Qt::ISODateWithMs).isValid()) return text;
    } else if (type == ParameterType::Timestamp) {
        if (QDateTime::fromString(text, Qt::ISODateWithMs).isValid()) return text;
    }
    return {};
}
}
