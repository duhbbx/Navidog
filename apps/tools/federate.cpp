#include "navidog/federated_query.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace navidog;
namespace sc = navidog::sql::compiler;
namespace opt = navidog::sql::optimizer;

struct Source {
    ConnectionProfile profile;
    std::unique_ptr<Database> connection;
};
struct RequestedTable {
    QString source, schema, name;
};

int diagnostics(const std::vector<sc::Diagnostic> &items) {
    for (const auto &item : items)
        std::cerr << item.range.begin << ": " << item.message << '\n';
    return 1;
}

std::string readQuery(const QString &path, std::size_t maxBytes) {
    QFile file;
    if (path == "-") {
        if (!file.open(stdin, QIODevice::ReadOnly))
            throw std::runtime_error("cannot open SQL on stdin");
    } else {
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("cannot open the query file");
    }
    QByteArray query;
    while (true) {
        auto chunk = file.read(8192);
        if (file.error() != QFile::NoError)
            throw std::runtime_error("cannot read the query file");
        if (static_cast<std::size_t>(chunk.size()) > maxBytes - query.size())
            throw std::runtime_error("SQL input exceeds the source byte limit");
        if (chunk.isEmpty()) break;
        query.append(chunk);
    }
    return query.toStdString();
}

sc::TableDefinition readSqliteTable(Database &database, const RequestedTable &table,
                              const Cancellation &cancel) {
    const auto identity = database.executeBound(
        "SELECT name,type FROM pragma_table_list(?1) WHERE schema='main' AND name=?1",
        {table.name}, {2, 65536, 4096}, cancel);
    if (!identity.error.isEmpty()) throw std::runtime_error(identity.error.toStdString());
    if (identity.rows.size() != 1 || identity.rows.front().at(1).toString() != "table" ||
        table.name.startsWith("sqlite_", Qt::CaseInsensitive))
        throw std::runtime_error("a registered SQLite object must be an ordinary table in main");

    const auto columns = database.executeBound(
        "SELECT name,type FROM pragma_table_xinfo(?1,'main') WHERE hidden<>1 ORDER BY cid LIMIT 513",
        {table.name}, {513, 2 * 1024 * 1024, 4096}, cancel);
    if (!columns.error.isEmpty()) throw std::runtime_error(columns.error.toStdString());
    if (columns.rows.isEmpty() || columns.rows.size() > 512 || columns.truncated || columns.cellTruncated)
        throw std::runtime_error("SQLite table metadata is incomplete or above the column limit");

    sc::TableDefinition definition;
    definition.database = table.source.toStdString();
    definition.schema = table.schema.toStdString();
    definition.name = table.name.toStdString();
    const auto integer = std::make_shared<const sc::SqlType>(
        *sc::catalogTypeFromText(sc::Dialect::PostgreSQL, "BIGINT"));
    const auto flag = std::make_shared<const sc::SqlType>(
        *sc::catalogTypeFromText(sc::Dialect::PostgreSQL, "BOOLEAN"));
    const auto text = std::make_shared<const sc::SqlType>(
        *sc::catalogTypeFromText(sc::Dialect::PostgreSQL, "TEXT"));
    for (const auto &row : columns.rows) {
        sc::ColumnDefinition column;
        column.name = row.at(0).toString().toStdString();
        const auto declaration = row.at(1).toString().trimmed().toUpper();
        if (declaration == "BOOLEAN" || declaration == "BOOL") {
            column.type = sc::ValueType::Boolean;
            column.sqlType = flag;
        } else if (declaration.contains("INT")) {
            // SQLite's INTEGER affinity includes the entire signed 64-bit
            // storage range. Actual cells are checked by the native adapter.
            column.type = sc::ValueType::Integer;
            column.sqlType = integer;
        } else if (declaration.contains("CHAR") || declaration.contains("CLOB") || declaration.contains("TEXT")) {
            column.type = sc::ValueType::String;
            column.sqlType = text;
        }
        definition.columns.push_back(std::move(column));
    }
    return definition;
}

sc::TableDefinition readServerTable(Database &database, DatabaseKind driver, const RequestedTable &table,
                                  const Cancellation &cancel) {
    const bool pg = driver == DatabaseKind::PostgreSQL;
    const char *sql = pg ?
        "SELECT a.attname,pg_catalog.format_type(a.atttypid,a.atttypmod) "
        "FROM pg_catalog.pg_attribute a "
        "JOIN pg_catalog.pg_class c ON a.attrelid=c.oid "
        "JOIN pg_catalog.pg_namespace n ON c.relnamespace=n.oid "
        "WHERE n.nspname=$1 AND c.relname=$2 AND c.relkind IN ('r','p') "
        "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum LIMIT 513" :
        "SELECT c.COLUMN_NAME,c.COLUMN_TYPE FROM information_schema.COLUMNS c "
        "JOIN information_schema.TABLES t ON BINARY t.TABLE_SCHEMA=BINARY c.TABLE_SCHEMA "
        "AND BINARY t.TABLE_NAME=BINARY c.TABLE_NAME "
        "WHERE BINARY c.TABLE_SCHEMA=BINARY ? AND BINARY c.TABLE_NAME=BINARY ? "
        "AND t.TABLE_TYPE='BASE TABLE' AND t.ENGINE='InnoDB' "
        "ORDER BY c.ORDINAL_POSITION LIMIT 513";
    const auto columns = database.executeBound(
        sql, {table.schema, table.name}, {513, 2 * 1024 * 1024, 4096}, cancel);
    if (!columns.error.isEmpty()) throw std::runtime_error(columns.error.toStdString());
    if (columns.rows.isEmpty() || columns.rows.size() > 512 || columns.truncated || columns.cellTruncated)
        throw std::runtime_error(pg ? "PostgreSQL table metadata is missing or above the column limit" :
            "MySQL federation needs a visible InnoDB table within the column limit");

    sc::TableDefinition definition;
    definition.database = table.source.toStdString();
    definition.schema = table.schema.toStdString();
    definition.name = table.name.toStdString();
    for (const auto &row : columns.rows) {
        sc::ColumnDefinition column;
        column.name = row.at(0).toString().toStdString();
        const auto native = row.at(1).toString().toStdString();
        if (const auto type = pg ? sc::catalogTypeFromText(sc::Dialect::PostgreSQL, native) :
                                  federatedMySqlInteger(native)) {
            column.type = pg ? sc::typeCategory({native}, sc::Dialect::PostgreSQL) : sc::ValueType::Integer;
            column.sqlType = std::make_shared<const sc::SqlType>(*type);
        }
        definition.columns.push_back(std::move(column));
    }
    return definition;
}

ConnectionProfile serverProfile(DatabaseKind driver, const QString &address) {
    const bool pg = driver == DatabaseKind::PostgreSQL;
    const QUrl uri(address, QUrl::StrictMode);
    if (!uri.isValid() || uri.scheme() != (pg ? "postgresql" : "mysql") || uri.hasFragment() ||
        !uri.password().isEmpty() || !uri.path().startsWith('/'))
        throw std::runtime_error("server sources need a matching URI with no password in the address");
    std::map<QString, QString> parameters;
    for (const auto &item : QUrlQuery(uri).queryItems(QUrl::FullyDecoded)) {
        if (item.second.contains(QChar(0)) || !parameters.emplace(item.first, item.second).second)
            throw std::runtime_error("invalid or repeated server connection parameter");
    }
    const auto take = [&](const QString &key, QString fallback = {}) {
        const auto it = parameters.find(key);
        if (it == parameters.end()) return fallback;
        auto value = it->second;
        parameters.erase(it);
        return value;
    };
    ConnectionProfile profile;
    profile.driver = driver;
    profile.database = uri.path(QUrl::FullyDecoded).mid(1);
    profile.host = take("host", uri.host(QUrl::FullyDecoded));
    profile.user = take("user", uri.userName(QUrl::FullyDecoded));
    bool portOk = false;
    profile.port = take("port", QString::number(uri.port(pg ? 5432 : 3306))).toInt(&portOk);
    profile.ssl.caFile = take("sslrootcert");
    const auto ssl = take("sslmode", pg && profile.host.startsWith('/') ? "disable" : "verify-full");
    if (!pg) {
        const auto variable = take("password_env");
        if (!variable.isEmpty()) {
            const QRegularExpression valid(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_]{0,127}\\z"));
            const auto key = variable.toLatin1();
            if (!valid.match(variable).hasMatch() || !qEnvironmentVariableIsSet(key.constData()))
                throw std::runtime_error("MySQL password environment variable is invalid or unset");
            profile.password = qEnvironmentVariable(key.constData());
            if (profile.password.contains(QChar(0)))
                throw std::runtime_error("MySQL password contains a NUL byte");
        }
    }
    profile.tls = ssl != "disable";
    if (ssl == "verify-full")
        profile.ssl.verification = SslVerification::Identity;
    else if (ssl == "verify-ca")
        profile.ssl.verification = SslVerification::CertificateAuthority;
    else if (ssl != "require" && ssl != "disable")
        throw std::runtime_error("unsupported server sslmode");
    if (!parameters.empty() || profile.host.isEmpty() || profile.host.contains(',') ||
        profile.host.contains(QChar(0)) || profile.user.isEmpty() || profile.user.contains(QChar(0)) ||
        profile.database.isEmpty() || profile.database.contains(QChar(0)) ||
        !portOk || profile.port < 1 || profile.port > 65535)
        throw std::runtime_error("provide an explicit server host, user, database and valid port");
    profile.readOnly = true;
    profile.advanced.readTimeout = 5;
    return profile;
}

int execute(const QCommandLineParser &args) {
    const auto paths = args.positionalArguments();
    const auto sqliteOptions = args.values("sqlite");
    const auto postgresOptions = args.values("postgres");
    const auto mysqlOptions = args.values("mysql");
    const auto sourceCount = sqliteOptions.size() + postgresOptions.size() + mysqlOptions.size();
    const auto tableOptions = args.values("table");
    if (paths.size() != 1 || sourceCount == 0 || sourceCount > 16 ||
        tableOptions.empty() || tableOptions.size() > 64)
        throw std::runtime_error("provide one SQL path (or -), up to 16 registered sources and up to 64 tables");
    const QRegularExpression aliasPattern(QStringLiteral("\\A[a-z_][a-z0-9_]{0,62}\\z"));
    std::map<QString, Source> sources;
    const auto registerSources = [&](const QStringList &entries, DatabaseKind driver) {
        for (const auto &entry : entries) {
            const auto separator = entry.indexOf('=');
            const auto alias = entry.left(separator);
            const auto address = entry.mid(separator + 1);
            if (separator <= 0 || !aliasPattern.match(alias).hasMatch() || address.isEmpty())
                throw std::runtime_error("sources use lowercase_alias=address");
            ConnectionProfile profile;
            if (driver != DatabaseKind::SQLite) {
                profile = serverProfile(driver, address);
            } else {
                const QFileInfo file(address);
                if (address.contains(QChar(0)) || !file.isFile())
                    throw std::runtime_error("SQLite sources need an existing file");
                profile.database = file.absoluteFilePath();
                profile.readOnly = true;
            }
            if (!sources.emplace(alias, Source{std::move(profile), {}}).second)
                throw std::runtime_error("duplicate source alias");
        }
    };
    registerSources(sqliteOptions, DatabaseKind::SQLite);
    registerSources(postgresOptions, DatabaseKind::PostgreSQL);
    registerSources(mysqlOptions, DatabaseKind::MySQL);
    std::set<std::tuple<QString, QString, QString>> uniqueTables;
    std::vector<RequestedTable> tables;
    for (const auto &entry : tableOptions) {
        const auto sourceEnd = entry.indexOf('.');
        const auto schemaEnd = entry.indexOf('.', sourceEnd + 1);
        const auto alias = entry.left(sourceEnd);
        const auto schema = entry.mid(sourceEnd + 1, schemaEnd - sourceEnd - 1);
        const auto name = entry.mid(schemaEnd + 1);
        const auto source = sources.find(alias);
        if (sourceEnd <= 0 || schemaEnd < 0 || schema.isEmpty() || name.isEmpty() ||
            schema.size() > 255 || name.size() > 255 || entry.contains(QChar(0)) || source == sources.end())
            throw std::runtime_error("tables use registered_alias.schema.table");
        if (source->second.profile.driver == DatabaseKind::SQLite && schema != "main")
            throw std::runtime_error("SQLite supports registered tables in main");
        if (!uniqueTables.emplace(alias, schema, name).second)
            throw std::runtime_error("duplicate table binding");
        tables.push_back({alias, schema, name});
    }

    opt::LocalRow parameters;
    const auto arguments=args.values("bind");
    if (arguments.size()>1024) throw std::runtime_error("at most 1024 positional bindings are supported");
    for (const auto &argument:arguments) {
        const auto value=opt::parseLocalBinding(argument.toStdString());
        if (!value) throw std::runtime_error("--bind needs an integer, exact decimal, true, false or null");
        parameters.push_back(*value);
    }
    sc::LanguageOptions options;
    options.calciteExtensions=args.isSet("calcite-extensions");
    auto query = readQuery(paths.front(), options.limits.sourceBytes);
    const auto syntax = sc::compile(sc::Dialect::PostgreSQL, query, options);
    if (!syntax->syntaxValid) return diagnostics(syntax->diagnostics);
    if (syntax->statements.size() != 1 || syntax->statements.front().kind != sc::StatementKind::Select)
        throw std::runtime_error("federated execution accepts one SELECT or VALUES query");

    sc::MemoryCatalog catalog;
    std::vector<FederatedTableBinding> bindings;
    const auto cancel = cancellation();
    for (const auto &requested : tables) {
        auto &source = sources.at(requested.source);
        const auto driver = source.profile.driver;
        if (!source.connection) {
            source.connection = connectDatabase(source.profile);
            const auto begin = [&](const char *sql) {
                const auto result = source.connection->execute(sql, {}, cancel);
                if (!result.error.isEmpty()) throw std::runtime_error(result.error.toStdString());
            };
            if (driver == DatabaseKind::PostgreSQL) begin("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
            else if (driver == DatabaseKind::MySQL) {
                begin("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
                begin("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY");
            } else begin("BEGIN");
        }
        catalog.tables.push_back(driver == DatabaseKind::SQLite ?
            readSqliteTable(*source.connection, requested, cancel) :
            readServerTable(*source.connection, driver, requested, cancel));
        bindings.push_back({requested.source.toStdString(), requested.schema.toStdString(), requested.name.toStdString(),
                            driver, source.connection.get(), requested.schema, requested.name});
    }
    const auto compiled = sc::compile(sc::Dialect::PostgreSQL, std::move(query),
                                      options, &catalog, {"", "", sc::Dialect::PostgreSQL});
    if (!compiled->ok()) return diagnostics(compiled->diagnostics);
    opt::Interpretation result;
    if (parameters.empty()) {
        result=executeFederatedQuery(compiled->model,bindings,cancel);
    } else {
        const auto prepared=opt::prepareLocalQuery(compiled->model);
        if (!prepared.ok()) return diagnostics(prepared.diagnostics);
        result=executeFederatedQuery(*prepared.query,parameters,bindings,cancel);
    }
    std::cout << opt::dumpInterpretation(result) << '\n';
    return result.ok() ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser args;
    args.setApplicationDescription("Run a bounded read-only query across explicitly registered SQLite, PostgreSQL and MySQL tables");
    args.addHelpOption();
    args.addOption({"sqlite", "Register a SQLite source. Repeat per source.", "alias=file"});
    args.addOption({"postgres", "Register a PostgreSQL source without a password in its URL.", "alias=uri"});
    args.addOption({"mysql", "Register a MySQL source, with an optional password_env URI parameter.", "alias=uri"});
    args.addOption({"bind", "Bind a positional PostgreSQL parameter. Repeat in $n order.", "integer|true|false|null"});
    args.addOption({"table", "Register a table. Repeat per table.", "alias.schema.table"});
    args.addOption({"calcite-extensions", "Enable supported Calcite syntax such as PIVOT and UNPIVOT."});
    args.addPositionalArgument("query", "SQL file or - for stdin");
    args.process(app);
    try {
        return execute(args);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
