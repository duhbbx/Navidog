#include "navidog/catalog.hpp"
#include "jdbc_catalog.hpp"
#include "extended_catalog.hpp"
#include "navidog/redis_commands.hpp"
#include "navidog/table_design.hpp"
#include "navidog/source_design.hpp"
#include <QRegularExpression>
#include <algorithm>
#include <stdexcept>
namespace navidog {
namespace {
QString sqlLiteral(QString s, const ConnectionProfile &p) {
  if (navidog::isMySqlFamily(p.driver))
    return s.isEmpty()
               ? QString("''")
               : "CONVERT(0x" + QString::fromLatin1(s.toUtf8().toHex()) +
                     " USING utf8mb4)";
  if (usesNativePostgres(p.driver))
    s.replace("\\", "\\\\");
  s.replace('\'', "''");
  return (p.driver == navidog::DatabaseKind::SQLServer    ? "N'"
          : usesNativePostgres(p.driver) ? "E'"
                                                         : "'") +
         s + "'";
}
bool mysql(const ConnectionProfile &p) {
  return navidog::isMySqlFamily(p.driver);
}
CatalogNode child(const CatalogNode &parent, QString kind, QString name,
                  bool expandable = true, QString detail = {}) {
  CatalogNode node = parent;
  node.kind = kind;
  node.name = name;
  node.expandable = expandable;
  node.detail = detail;
  if (kind == "database") {
    node.database = name;
    node.schema.clear();
    node.object.clear();
  } else if (kind == "schema") {
    node.schema = name;
    node.object.clear();
  } else if (kind == "table" || kind == "materializedView" || kind == "view" || kind == "function" ||
             kind == "procedure")
    node.object = name;
  return node;
}
} // namespace
QString quoteIdentifier(QString name, navidog::DatabaseKind driver) {
  if (driver == navidog::DatabaseKind::SQLServer) {
    name.replace("]", "]]");
    return "[" + name + "]";
  }
  auto q = (navidog::isMySqlFamily(driver) || isAnalyticalMySql(driver) || driver == DatabaseKind::ClickHouse) ? QString("`") : QString("\"");
  name.replace(q, q + q);
  return q + name + q;
}
static QString qualified(const ConnectionProfile &p, const CatalogNode &node) {
  QStringList parts;
  if (mysql(p))
    parts << node.database;
  else if (!node.schema.isEmpty())
    parts << node.schema;
  parts << node.object;
  QStringList quoted;
  for (const auto &part : parts)
    if (!part.isEmpty())
      quoted << quoteIdentifier(part, p.driver);
  return quoted.join('.');
}
QString previewSql(const ConnectionProfile &p, const CatalogNode &node) {
  if (usesExtendedCatalog(p.driver) || p.driver == DatabaseKind::MongoDb)
    return extendedPreview(p,node);
  if (p.driver == DatabaseKind::Redis)
    return "TYPE " + quoteRedisArgument(QByteArray::fromBase64(node.key.toLatin1()));
  auto name = qualified(p, node);
  if (p.driver == navidog::DatabaseKind::SQLServer)
    return "SELECT TOP (100) * FROM " + name + ";";
  if (navidog::usesSchemaAtConnection(p.driver) && !usesPostgresCompatibleSql(p.driver))
    return "SELECT * FROM " + name + " FETCH FIRST 100 ROWS ONLY";
  return "SELECT * FROM " + name + " LIMIT 100;";
}
QString definitionSql(const ConnectionProfile &p, const CatalogNode &n) {
  if (usesExtendedCatalog(p.driver) || p.driver == DatabaseKind::MongoDb ||
      p.driver == DatabaseKind::OpenGauss)
    return extendedDefinition(p,n);
  auto literal = [&p](QString s) { return sqlLiteral(s, p); };
  if (n.kind == "database") {
    const auto name = literal(n.database);
    if (mysql(p))
      return "SELECT SCHEMA_NAME, DEFAULT_CHARACTER_SET_NAME, "
             "DEFAULT_COLLATION_NAME FROM information_schema.SCHEMATA WHERE "
             "SCHEMA_NAME = " +
             name + ";";
    if (usesNativePostgres(p.driver))
      return "SELECT datname, pg_get_userbyid(datdba) AS owner, "
             "pg_encoding_to_char(encoding) AS encoding, datcollate, datctype, "
             "datconnlimit FROM pg_database WHERE datname = " +
             name + ";";
    if (p.driver == DatabaseKind::SQLServer)
      return "SELECT name, state_desc, collation_name, recovery_model_desc, "
             "compatibility_level, is_read_only FROM sys.databases WHERE name "
             "= " +
             name + ";";
  }
  if (n.kind == "schema") {
    const auto name = literal(n.schema);
    if (usesNativePostgres(p.driver))
      return "SELECT nspname, pg_get_userbyid(nspowner) AS owner FROM "
             "pg_namespace WHERE nspname = " +
             name + ";";
    if (p.driver == DatabaseKind::SQLServer)
      return "SELECT name, USER_NAME(principal_id) AS owner FROM sys.schemas "
             "WHERE name = " +
             name + ";";
    if (navidog::usesSchemaAtConnection(p.driver))
      return "SELECT username, user_id, created FROM all_users WHERE username "
             "= " +
             name + ";";
  }
  if (p.driver == navidog::DatabaseKind::SQLite)
    return "SELECT sql FROM sqlite_schema WHERE name = " + literal(n.name) +
           ";";
  if (mysql(p))
    return "SHOW CREATE " +
           QString(n.kind == "view"        ? "VIEW "
                   : n.kind == "function"  ? "FUNCTION "
                   : n.kind == "procedure" ? "PROCEDURE "
                                           : "TABLE ") +
           qualified(p, n) + ";";
  if (p.driver == navidog::DatabaseKind::SQLServer) {
    if (n.kind == "table")
      return "SELECT c.name AS column_name, TYPE_NAME(c.user_type_id) AS "
             "data_type, c.max_length, c.is_nullable, d.definition AS "
             "default_value FROM sys.columns c LEFT JOIN "
             "sys.default_constraints d ON d.object_id=c.default_object_id "
             "WHERE c.object_id=OBJECT_ID(" +
             literal(qualified(p, n)) + ") ORDER BY c.column_id;";
    return "SELECT OBJECT_DEFINITION(OBJECT_ID(" + literal(qualified(p, n)) +
           ")) AS definition;";
  }
  if (p.driver == DatabaseKind::Dameng) {
    const auto owner = literal(n.schema), name = literal(n.object);
    if (n.kind == "view")
      return "SELECT text FROM all_views WHERE owner=" + owner + " AND view_name=" + name;
    if (n.kind == "function" || n.kind == "procedure")
      return "SELECT text FROM all_source WHERE owner=" + owner + " AND name=" + name +
             " AND type=" + literal(n.kind.toUpper()) + " ORDER BY line";
    return "SELECT column_name, data_type, nullable, data_default FROM all_tab_columns WHERE owner=" +
           owner + " AND table_name=" + name + " ORDER BY column_id";
  }
  if (navidog::usesOracleSql(p.driver))
    return "SELECT DBMS_METADATA.GET_DDL(" + literal(n.kind == "materializedView" ? "MATERIALIZED_VIEW" : n.kind.toUpper()) + ", " +
           literal(n.object) + ", " + literal(n.schema) +
           ") AS definition FROM dual";
  if (isRoutineObject(n.kind)) {
    if (!QRegularExpression("^[0-9]{1,10}$").match(n.key).hasMatch())
      throw std::runtime_error("无法定位例程，请从导航树刷新并重新选择");
    return "SELECT pg_get_functiondef(p.oid) AS definition FROM pg_proc p "
           "JOIN pg_namespace n ON n.oid=p.pronamespace WHERE p.oid=" + n.key +
           "::oid AND n.nspname=" + literal(n.schema) + " AND p.proname=" + literal(n.object) +
           (n.kind == "procedure" ? " AND p.prokind='p'" : " AND p.prokind IN ('f','w')");
  }
  if (isSourceRelation(n.kind))
    return "SELECT pg_get_viewdef(" + literal(qualified(p, n)) +
           "::regclass, true) AS definition;";
  return "SELECT column_name, data_type, is_nullable, column_default FROM "
         "information_schema.columns WHERE table_schema = " +
         literal(n.schema) + " AND table_name = " + literal(n.object) +
         " ORDER BY ordinal_position;";
}
CatalogResult loadCatalog(Database &db, const ConnectionProfile &p,
                          const CatalogNode &n, const Cancellation &cancel) {
  if (p.driver == DatabaseKind::Redis) {
    CatalogResult out;
    if (n.kind == "connection") out.nodes << child(n, "redisKeys", "键");
    else if (n.kind == "redisKeys") {
      QueryLimits limits;
      limits.maxCellBytes = 1024;
      const auto keys = db.redisKeys(limits, cancel);
      out.error = keys.error;
      out.truncated = keys.truncated;
      out.cancelled = keys.cancelled;
      for (const auto& row : keys.rows) {
        const auto bytes = row.first().toByteArray();
        const auto decoded = QString::fromUtf8(bytes);
        const bool printable = decoded.toUtf8() == bytes && !decoded.trimmed().isEmpty() &&
          std::none_of(decoded.cbegin(), decoded.cend(), [](QChar ch) { return ch.category() == QChar::Other_Control; });
        auto node = child(n, "redisKey", printable ? decoded : quoteRedisArgument(bytes), false);
        node.key = QString::fromLatin1(bytes.toBase64());
        out.nodes << node;
      }
    }
    return out;
  }
  if (usesExtendedCatalog(p.driver) || p.driver == DatabaseKind::MongoDb ||
      (p.driver == DatabaseKind::OpenGauss && (n.kind == "indexes" || n.kind == "constraints")))
    return loadExtendedCatalog(db,p,n,cancel);
  if (usesJdbcCatalog(p.driver))
    return loadJdbcCatalog(db, p, n, cancel);
  CatalogResult out;
  auto literal = [&p](QString s) { return sqlLiteral(s, p); };
  auto folder = [&](QString kind, QString name) {
    out.nodes.push_back(child(n, kind, name));
  };
  auto query = [&](QString sql, QString kind, bool expandable = true) {
    QueryLimits limits;
    limits.maxRows = 1000;
    auto r = db.execute(sql, limits, cancel);
    out.truncated |= r.truncated;
    out.cancelled |= r.cancelled;
    if (!r.error.isEmpty())
      throw std::runtime_error(r.error.toStdString());
    for (const auto &row : r.rows) {
      auto node = child(n, kind, row[0].toString(), expandable,
                        row.size() > 1 ? row[1].toString() : QString{});
      if (row.size() > 2)
        node.key = row[2].toString();
      out.nodes.push_back(node);
    }
  };
  auto sch = literal(n.schema), obj = literal(n.object),
       database = literal(n.database);
  if (n.kind == "connection") {
    if (p.driver == navidog::DatabaseKind::SQLite) {
      auto node = child(n, "database", "main");
      node.schema = "main";
      out.nodes << node;
    } else if (mysql(p))
      query("SELECT schema_name FROM information_schema.schemata ORDER BY "
            "schema_name",
            "database");
    else if (usesNativePostgres(p.driver))
      query("SELECT datname FROM pg_database WHERE datallowconn AND "
            "has_database_privilege(datname, 'CONNECT') ORDER BY datname",
            "database");
    else if (p.driver == navidog::DatabaseKind::SQLServer)
      query("SELECT name FROM sys.databases WHERE HAS_DBACCESS(name)=1 ORDER "
            "BY name",
            "database");
    else if (navidog::usesSchemaAtConnection(p.driver))
      query("SELECT username FROM all_users ORDER BY username", "schema");
  } else if (n.kind == "database") {
    if (mysql(p) || p.driver == navidog::DatabaseKind::SQLite) {
      folder("tables", "表");
      folder("views", "视图");
      folder("functions", "函数");
      if (mysql(p))
        folder("procedures", "存储过程");
    } else if (usesNativePostgres(p.driver))
      query("SELECT schema_name FROM information_schema.schemata ORDER BY "
            "schema_name",
            "schema");
    else if (p.driver == navidog::DatabaseKind::SQLServer)
      query("SELECT name FROM sys.schemas ORDER BY name", "schema");
  } else if (n.kind == "schema") {
    folder("tables", "表");
    folder("views", "视图");
    folder("functions", "函数");
    folder("procedures", "存储过程");
    if (supportsSourceDesign(p.driver, "materializedView")) folder("materializedViews", "物化视图");
  } else if (n.kind == "table" || isSourceRelation(n.kind)) {
    folder("columns", "字段");
    if (n.kind == "table" && p.driver != DatabaseKind::Dameng) {
      folder("indexes", "索引");
      folder("foreignKeys", "外键");
      folder("triggers", "触发器");
      folder("checks", "检查");
    }
  } else if (QStringList{"indexes", "foreignKeys", "checks", "triggers"}
                 .contains(n.kind)) {
    auto definition = readTableDefinition(db, p, n, cancel);
    auto type = n.kind == "indexes"       ? TableObjectKind::Index
                : n.kind == "foreignKeys" ? TableObjectKind::ForeignKey
                : n.kind == "checks"      ? TableObjectKind::Check
                                          : TableObjectKind::Trigger;
    if (type == TableObjectKind::Index && !definition.primaryKeyName.isEmpty())
      out.nodes << child(n, "primaryKey", definition.primaryKeyName, false,
                         "PRIMARY KEY");
    for (const auto &o : definition.objects)
      if (o.kind == type) {
        auto detail = type == TableObjectKind::ForeignKey
                          ? formatIdentifierList(o.columns, p.driver) + " → " +
                                o.referenceSchema + "." + o.referenceTable
                      : !o.expression.isEmpty() ? o.expression
                                                : o.sql.left(200);
        out.nodes << child(n, tableObjectKindKey(type), o.name, false, detail);
      }
  } else if (n.kind == "function" || n.kind == "procedure") {
    if (p.driver != DatabaseKind::Dameng)
      folder("parameters", "参数");
    auto note = child(n, "note", "局部变量需查看例程定义", false);
    note.detail = "数据库目录通常不记录例程内部局部变量。右键查看定义。";
    out.nodes << note;
  } else if (p.driver == navidog::DatabaseKind::SQLite) {
    if (n.kind == "tables" || n.kind == "views")
      query("SELECT name FROM sqlite_schema WHERE type = " +
                literal(n.kind == "tables" ? "table" : "view") +
                " AND name NOT LIKE 'sqlite_%' AND name <> 'harbor_meta' ORDER "
                "BY name",
            n.kind == "tables" ? "table" : "view");
    else if (n.kind == "columns")
      query("SELECT name, type || CASE WHEN pk THEN ' · PRIMARY KEY' ELSE '' "
            "END FROM pragma_table_xinfo(" +
                obj + ") ORDER BY cid",
            "column", false);
    else if (n.kind == "indexes")
      query("SELECT name, CASE WHEN [unique] THEN 'UNIQUE' ELSE '' END FROM "
            "pragma_index_list(" +
                obj + ")",
            "index", false);
    else if (n.kind == "functions")
      query("SELECT name, 'args=' || narg FROM pragma_function_list ORDER BY "
            "name, narg",
            "builtin", false);
  } else if (mysql(p)) {
    auto filter = " WHERE table_schema = " + database;
    if (n.kind == "tables" || n.kind == "views")
      query("SELECT table_name, table_type FROM information_schema.tables" +
                filter + " AND table_type = " +
                literal(n.kind == "tables" ? "BASE TABLE" : "VIEW") +
                " ORDER BY table_name",
            n.kind == "tables" ? "table" : "view");
    else if (n.kind == "columns")
      query("SELECT column_name, CONCAT(column_type, ' ', is_nullable, ' ', "
            "column_key) FROM information_schema.columns" +
                filter + " AND table_name = " + obj +
                " ORDER BY ordinal_position",
            "column", false);
    else if (n.kind == "indexes")
      query("SELECT DISTINCT index_name, CASE WHEN non_unique=0 THEN 'UNIQUE' "
            "ELSE '' END FROM information_schema.statistics" +
                filter + " AND table_name = " + obj + " ORDER BY index_name",
            "index", false);
    else if (n.kind == "functions" || n.kind == "procedures")
      query("SELECT routine_name, data_type FROM information_schema.routines "
            "WHERE routine_schema = " +
                database + " AND routine_type = " +
                literal(n.kind == "functions" ? "FUNCTION" : "PROCEDURE") +
                " ORDER BY routine_name",
            n.kind == "functions" ? "function" : "procedure");
    else if (n.kind == "parameters")
      query("SELECT COALESCE(parameter_name,'RETURN'), "
            "CONCAT(COALESCE(parameter_mode,'RETURN'), ' ', dtd_identifier) "
            "FROM information_schema.parameters WHERE specific_schema = " +
                database + " AND specific_name = " + obj +
                " ORDER BY ordinal_position",
            "parameter", false);
  } else if (usesNativePostgres(p.driver)) {
    if (n.kind == "materializedViews")
      query("SELECT matviewname, CASE WHEN ispopulated THEN '已填充' ELSE '未填充' END FROM pg_matviews "
            "WHERE schemaname=" + sch + " ORDER BY matviewname", "materializedView");
    else if (n.kind == "tables" || n.kind == "views")
      query("SELECT table_name, table_type FROM information_schema.tables "
            "WHERE table_schema = " +
                sch + " AND table_type = " +
                literal(n.kind == "tables" ? "BASE TABLE" : "VIEW") +
                " ORDER BY table_name",
            n.kind == "tables" ? "table" : "view");
    else if (n.kind == "columns")
      query("SELECT a.attname, format_type(a.atttypid,a.atttypmod) || "
            "CASE WHEN a.attnotnull THEN ' · NOT NULL' ELSE '' END "
            "FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid "
            "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname=" +
                sch + " AND c.relname=" + obj +
                " AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum", "column", false);
    else if (n.kind == "indexes")
      query("SELECT indexname, indexdef FROM pg_indexes WHERE schemaname = " +
                sch + " AND tablename = " + obj + " ORDER BY indexname",
            "index", false);
    else if (n.kind == "functions" || n.kind == "procedures")
      query("SELECT p.proname, pg_get_function_identity_arguments(p.oid), "
            "p.oid::text FROM pg_proc p JOIN pg_namespace s ON "
            "s.oid=p.pronamespace WHERE s.nspname=" +
                sch + QString(n.kind == "functions" ? " AND p.prokind IN ('f','w')" : " AND p.prokind='p'") +
                " ORDER BY p.proname,p.oid",
            n.kind == "functions" ? "function" : "procedure");
    else if (n.kind == "parameters") {
      if (!QRegularExpression("^[0-9]+$").match(n.key).hasMatch())
        throw std::runtime_error("例程 OID 无效");
      query(
          "SELECT COALESCE(p.proargnames[a.ordinality],'arg_'||a.ordinality), "
          "CASE COALESCE(p.proargmodes[a.ordinality],'i') WHEN 'o' THEN 'OUT' "
          "WHEN 'b' THEN 'INOUT' WHEN 'v' THEN 'VARIADIC' WHEN 't' THEN 'OUT' "
          "ELSE 'IN' END || ' ' || format_type(a.type,NULL) FROM pg_proc p "
          "CROSS JOIN LATERAL "
          "unnest(COALESCE(p.proallargtypes,p.proargtypes::oid[])) WITH "
          "ORDINALITY AS a(type,ordinality) WHERE p.oid=" +
              n.key + " ORDER BY a.ordinality",
          "parameter", false);
    }
  } else if (p.driver == navidog::DatabaseKind::SQLServer) {
    if (n.kind == "tables" || n.kind == "views")
      query(
          "SELECT name, type_desc FROM sys.objects WHERE schema_id=SCHEMA_ID(" +
              sch + ") AND type=" + literal(n.kind == "tables" ? "U" : "V") +
              " ORDER BY name",
          n.kind == "tables" ? "table" : "view");
    else if (n.kind == "columns")
      query("SELECT c.name, TYPE_NAME(c.user_type_id) FROM sys.columns c WHERE "
            "c.object_id=OBJECT_ID(" +
                literal(qualified(p, n)) + ") ORDER BY c.column_id",
            "column", false);
    else if (n.kind == "indexes")
      query("SELECT name, type_desc FROM sys.indexes WHERE name IS NOT NULL "
            "AND object_id=OBJECT_ID(" +
                literal(qualified(p, n)) + ") ORDER BY name",
            "index", false);
    else if (n.kind == "functions" || n.kind == "procedures")
      query(
          "SELECT name, type_desc FROM sys.objects WHERE schema_id=SCHEMA_ID(" +
              sch + ") AND type IN " +
              QString(n.kind == "functions" ? "('FN','IF','TF','FS','FT')"
                                            : "('P','PC')") +
              " ORDER BY name",
          n.kind == "functions" ? "function" : "procedure");
    else if (n.kind == "parameters")
      query("SELECT name, TYPE_NAME(user_type_id) FROM sys.parameters WHERE "
            "object_id=OBJECT_ID(" +
                literal(qualified(p, n)) +
                ") AND parameter_id>0 ORDER BY parameter_id",
            "parameter", false);
  } else if (navidog::usesSchemaAtConnection(p.driver)) {
    if (n.kind == "materializedViews")
      query("SELECT mview_name, staleness FROM all_mviews WHERE owner=" + sch +
            " ORDER BY mview_name", "materializedView");
    else if (n.kind == "tables" || n.kind == "views")
      query("SELECT object_name, object_type FROM all_objects WHERE owner=" +
                sch + " AND object_type=" +
                literal(n.kind == "tables" ? "TABLE" : "VIEW") +
                (n.kind == "tables" && supportsSourceDesign(p.driver,"materializedView") ?
                    " AND NOT EXISTS (SELECT 1 FROM all_mviews m WHERE m.owner=all_objects.owner AND m.mview_name=all_objects.object_name)" : "") +
                " ORDER BY object_name",
            n.kind == "tables" ? "table" : "view");
    else if (n.kind == "columns")
      query("SELECT column_name, data_type FROM all_tab_columns WHERE owner=" +
                sch + " AND table_name=" + obj + " ORDER BY column_id",
            "column", false);
    else if (n.kind == "indexes")
      query(
          "SELECT index_name, uniqueness FROM all_indexes WHERE table_owner=" +
              sch + " AND table_name=" + obj + " ORDER BY index_name",
          "index", false);
    else if (n.kind == "functions" || n.kind == "procedures")
      query("SELECT object_name, object_type FROM all_objects WHERE owner=" +
                sch + " AND object_type=" +
                literal(n.kind == "functions" ? "FUNCTION" : "PROCEDURE") +
                " ORDER BY object_name",
            n.kind == "functions" ? "function" : "procedure");
    else if (n.kind == "parameters")
      query("SELECT NVL(argument_name,'RETURN'), in_out || ' ' || CASE WHEN "
            "type_owner IS NULL THEN data_type ELSE '\"' || "
            "REPLACE(type_owner,'\"','\"\"') || '\".\"' || "
            "REPLACE(type_name,'\"','\"\"') || '\"' || CASE WHEN type_subname "
            "IS NULL THEN '' ELSE '.\"' || REPLACE(type_subname,'\"','\"\"') "
            "|| '\"' END END FROM all_arguments WHERE owner=" +
                sch + " AND object_name=" + obj +
                " AND package_name IS NULL AND data_level=0 ORDER BY sequence",
            "parameter", false);
  }
  return out;
}
} // namespace navidog
