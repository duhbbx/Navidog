#include "navidog/catalog.hpp"
#include "navidog/metadata_cache.hpp"
#include "jdbc_catalog.hpp"
#include "extended_catalog.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "navidog/query_context.hpp"
#include "metadata_relation_proofs.hpp"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <algorithm>
#include <stdexcept>
namespace navidog {
namespace {
QString literal(QString value, const ConnectionProfile &p) {
  if (isMySqlFamily(p.driver))
    return "CONVERT(0x" + QString::fromLatin1(value.toUtf8().toHex()) +
           " USING utf8mb4)";
  value.replace("'", "''");
  if (usesNativePostgres(p.driver))
    value.replace("\\", "\\\\");
  return (usesNativePostgres(p.driver)  ? "E'"
          : p.driver == DatabaseKind::SQLServer ? "N'"
                                                : "'") +
         value + "'";
}
QString stableKey(const QString &kind, const QString &schema,
                  const QString &name) {
  return QString::fromLatin1(
      QCryptographicHash::hash(QJsonDocument(QJsonArray{kind, schema, name})
                                   .toJson(QJsonDocument::Compact),
                               QCryptographicHash::Sha256)
          .toHex());
}
} // namespace
MetadataSnapshot collectMetadata(Database &db, const ConnectionProfile &p,
                                 const QString &schema,
                                 const Cancellation &cancel, bool searchOnly) {
  if (usesExtendedCatalog(p.driver))
    return collectExtendedMetadata(db,p,schema,cancel);
  if (usesJdbcCatalog(p.driver))
    return collectJdbcMetadata(db, p, schema, cancel, searchOnly);
  MetadataSnapshot out;
  out.profileId = p.id;
  out.identity = metadataIdentity(p);
  out.database = p.database;
  out.schema = schema;
  out.key = metadataKey(p, p.database, schema);
  out.driver = p.driver;
  out.serverVersion = db.serverVersion();
  QElapsedTimer elapsed;
  elapsed.start();
  qint64 total = 0;
  auto query = [&](const QString &sql, int maxRows) {
    if (cancel->load() || elapsed.elapsed() > 15000)
      throw std::runtime_error(
          "metadata refresh cancelled or exceeded time budget");
    QueryLimits limit;
    limit.maxRows = maxRows;
    limit.maxBytes = 8 * 1024 * 1024;
    limit.maxCellBytes = 4096;
    auto r = db.execute(sql, limit, cancel);
    if (r.cancelled || !r.error.isEmpty())
      throw std::runtime_error(r.error.isEmpty() ? "metadata refresh cancelled"
                                                 : r.error.toStdString());
    total += r.cachedBytes;
    if (total > 24 * 1024 * 1024)
      throw std::runtime_error("metadata collection byte budget exceeded");
    return r;
  };
  QString tables, columns, routines, parameters;
  auto sch = literal(schema, p), database = literal(p.database, p);
  if (isMySqlFamily(p.driver)) {
    auto settings = query("SELECT @@lower_case_table_names", 1);
    out.caseSensitive =
        settings.rows.isEmpty() || settings.rows[0][0].toInt() == 0;
    out.searchPath = QStringList{p.database};
    tables = "SELECT table_schema,table_name,CASE WHEN table_type='VIEW' THEN "
             "'view' ELSE 'table' END,'' FROM information_schema.tables WHERE "
             "table_schema=" +
             database + " ORDER BY table_name";
    columns = "SELECT "
              "table_schema,table_name,ordinal_position,column_name,column_"
              "type,CASE WHEN is_nullable='NO' AND extra NOT LIKE '%GENERATED%' THEN 1 ELSE 0 END FROM information_schema.columns WHERE table_schema=" +
              database + " ORDER BY table_name,ordinal_position";
    routines =
        "SELECT "
        "r.routine_schema,r.routine_name,LOWER(r.routine_type),r.data_type,r."
        "specific_name,COUNT(CASE WHEN p.ordinal_position>0 THEN 1 "
        "END),COUNT(CASE WHEN p.ordinal_position>0 THEN 1 END),0 FROM "
        "information_schema.routines r LEFT JOIN information_schema.parameters "
        "p ON p.specific_schema=r.routine_schema AND "
        "p.specific_name=r.specific_name AND p.routine_type=r.routine_type "
        "WHERE r.routine_schema=" +
        database +
        " GROUP BY "
        "r.routine_schema,r.routine_name,r.routine_type,r.data_type,r.specific_"
        "name ORDER BY r.routine_name";
    parameters =
        "SELECT "
        "r.routine_schema,r.routine_name,LOWER(r.routine_type),r.specific_name,"
        "p.ordinal_position,p.parameter_name,p.dtd_identifier,COALESCE(p."
        "parameter_mode,'RETURN'),CASE WHEN p.parameter_mode IN('IN','INOUT') "
        "THEN 1 ELSE 0 END,0,0 FROM information_schema.routines r JOIN "
        "information_schema.parameters p ON p.specific_schema=r.routine_schema "
        "AND p.specific_name=r.specific_name AND p.routine_type=r.routine_type "
        "WHERE r.routine_schema=" +
        database +
        " AND p.ordinal_position>0 ORDER BY r.routine_name,p.ordinal_position";
  } else if (usesNativePostgres(p.driver)) {
    auto path = query("SELECT unnest(current_schemas(true))", 64);
    if (schema.isEmpty())
      for (const auto &row : path.rows)
        out.searchPath << row[0].toString();
    else {
      out.searchPath << "pg_catalog" << schema;
      out.searchPath.removeDuplicates();
    }
    const QString filter =
        "n.nspname NOT LIKE 'pg_toast%' AND n.nspname NOT LIKE 'pg_temp_%' AND "
        "has_schema_privilege(n.oid,'USAGE')";
    tables = "SELECT n.nspname,c.relname,CASE WHEN c.relkind='m' THEN 'materializedView' WHEN c.relkind='v' THEN "
             "'view' ELSE 'table' END,c.oid::text,"
             "CASE WHEN c.relkind='r' AND NOT c.relhassubclass THEN 1 ELSE 0 END,"
             "(SELECT pg_catalog.json_agg(a.attname ORDER BY k.ord)::text "
             "FROM pg_catalog.pg_constraint pk JOIN pg_catalog.pg_index i ON i.indexrelid=pk.conindid "
             "CROSS JOIN LATERAL pg_catalog.unnest(pk.conkey) WITH ORDINALITY AS k(attnum,ord) "
             "JOIN pg_catalog.pg_attribute a ON a.attrelid=c.oid AND a.attnum=k.attnum "
             "WHERE pk.conrelid=c.oid AND pk.contype='p' AND pk.convalidated AND NOT pk.condeferrable "
             "AND i.indisvalid AND i.indisready AND i.indimmediate),"
             "CASE WHEN c.relkind='r' AND NOT c.relrowsecurity THEN 1 ELSE 0 END "
             "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE c.relkind "
             "IN('r','p','v','m','f') AND " +
             filter +
             " AND "
             "has_table_privilege(c.oid,'SELECT,INSERT,UPDATE,DELETE,"
             "REFERENCES') ORDER BY n.nspname,c.relname";
    columns =
        "SELECT "
        "n.nspname,c.relname,a.attnum,a.attname,format_type(a.atttypid,a."
        "atttypmod),CASE WHEN a.attnotnull AND a.attgenerated='' THEN 1 ELSE 0 END FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE a.attnum>0 AND NOT "
        "a.attisdropped AND c.relkind IN('r','p','v','m','f') AND " +
        filter +
        " AND "
        "has_table_privilege(c.oid,'SELECT,INSERT,UPDATE,DELETE,REFERENCES') "
        "ORDER BY n.nspname,c.relname,a.attnum";
    const QString count = "CASE WHEN p.prokind='p' THEN "
                          "COALESCE(array_length(p.proallargtypes,1),p."
                          "pronargs) ELSE p.pronargs END";
    routines =
        "SELECT n.nspname,p.proname,CASE WHEN p.prokind='p' THEN 'procedure' "
        "ELSE 'function' END,format_type(p.prorettype,NULL),p.oid::text," +
        count +
        "-p.pronargdefaults-CASE WHEN p.provariadic<>0 THEN 1 ELSE 0 END," +
        count +
        ",CASE WHEN p.provariadic<>0 THEN 1 ELSE 0 END,p.prokind,p.provolatile,CASE WHEN p.proretset THEN 1 ELSE 0 END FROM pg_proc p JOIN "
        "pg_namespace n ON n.oid=p.pronamespace WHERE " +
        filter +
        " AND has_function_privilege(p.oid,'EXECUTE') ORDER BY "
        "n.nspname,p.proname,p.oid";
    parameters =
        "SELECT n.nspname,p.proname,CASE WHEN p.prokind='p' THEN 'procedure' "
        "ELSE 'function' "
        "END,p.oid::text,a.ordinality,COALESCE(p.proargnames[a.ordinality],'"
        "arg_'||a.ordinality),format_type(a.type,NULL),COALESCE(p.proargmodes["
        "a.ordinality],'i'),CASE WHEN "
        "COALESCE(p.proargmodes[a.ordinality],'i') IN('i','b','v') THEN 1 ELSE "
        "0 END,0,CASE WHEN COALESCE(p.proargmodes[a.ordinality],'i')='v' THEN "
        "1 ELSE 0 END FROM pg_proc p JOIN pg_namespace n ON "
        "n.oid=p.pronamespace CROSS JOIN LATERAL "
        "unnest(COALESCE(p.proallargtypes,p.proargtypes::oid[])) WITH "
        "ORDINALITY a(type,ordinality) WHERE " +
        filter +
        " AND has_function_privilege(p.oid,'EXECUTE') ORDER BY "
        "p.oid,a.ordinality";
  } else if (p.driver == DatabaseKind::SQLite) {
    out.caseSensitive = false;
    out.searchPath = QStringList{"main"};
    tables = "SELECT 'main',name,type,name FROM main.sqlite_schema WHERE type "
             "IN('table','view') AND name NOT LIKE 'sqlite_%' ORDER BY name";
    columns =
        "SELECT 'main',m.name,c.cid+1,c.name,c.type,CASE WHEN c.\"notnull\"=1 AND c.hidden=0 THEN 1 ELSE 0 END FROM main.sqlite_schema m "
        "JOIN pragma_table_xinfo(m.name) c WHERE m.type IN('table','view') AND "
        "m.name NOT LIKE 'sqlite_%' ORDER BY m.name,c.cid";
    routines = "SELECT 'main',name,'function','',name||':'||narg,CASE WHEN "
               "narg<0 THEN 0 ELSE narg END,CASE WHEN narg<0 THEN 2147483647 "
               "ELSE narg END,CASE WHEN narg<0 THEN 1 ELSE 0 END,type,flags FROM "
               "pragma_function_list ORDER BY name,narg";
  } else if (p.driver == DatabaseKind::Dameng) {
    const auto current = query("SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual", 1);
    out.searchPath = QStringList{schema.isEmpty() ? current.rows.value(0).value(0).toString() : schema};
    sch = literal(out.searchPath.front(), p);
    const auto scope = searchOnly ? QString{} : " AND owner=" + sch;
    tables = "SELECT owner,object_name,LOWER(object_type),TO_CHAR(object_id) FROM all_objects "
             "WHERE object_type IN('TABLE','VIEW')" + scope + " ORDER BY owner,object_name";
    columns = "SELECT owner,table_name,column_id,column_name,data_type FROM all_tab_columns "
              "WHERE 1=1" + scope + " ORDER BY owner,table_name,column_id";
    routines = "SELECT owner,object_name,LOWER(object_type),'',TO_CHAR(object_id),0,2147483647,1 "
               "FROM all_objects WHERE object_type IN('FUNCTION','PROCEDURE')" + scope +
               " ORDER BY owner,object_name";
  } else if (usesOracleSql(p.driver)) {
    auto current =
        query("SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual", 1);
    out.searchPath = QStringList{
        schema.isEmpty() ? current.rows.value(0).value(0).toString() : schema};
    sch = literal(out.searchPath[0], p);
    tables = "SELECT owner,object_name,LOWER(object_type),TO_CHAR(object_id) "
             "FROM all_objects WHERE owner=" +
             sch + " AND object_type IN('TABLE','VIEW') ORDER BY object_name";
    columns = "SELECT owner,table_name,column_id,column_name,data_type FROM "
              "all_tab_columns WHERE owner=" +
              sch + " ORDER BY table_name,column_id";
    routines =
        "SELECT p.owner,p.object_name,LOWER(o.object_type),MAX(CASE WHEN "
        "a.position=0 THEN a.data_type "
        "END),TO_CHAR(p.object_id)||':'||p.subprogram_id,SUM(CASE WHEN "
        "a.position>0 AND a.defaulted<>'Y' THEN 1 ELSE 0 END),COUNT(CASE WHEN "
        "a.position>0 THEN 1 END),0 FROM all_procedures p JOIN all_objects o "
        "ON o.object_id=p.object_id LEFT JOIN "
        "all_arguments a ON a.object_id=p.object_id AND "
        "a.subprogram_id=p.subprogram_id AND a.data_level=0 WHERE p.owner=" +
        sch +
        " AND p.procedure_name IS NULL AND o.object_type "
        "IN('FUNCTION','PROCEDURE') GROUP BY "
        "p.owner,p.object_name,o.object_type,p.object_id,p.subprogram_id ORDER "
        "BY p.object_name";
    parameters =
        "SELECT "
        "p.owner,p.object_name,LOWER(o.object_type),TO_CHAR(p.object_id)||':'||"
        "p.subprogram_id,a.position,a.argument_name,a.data_type,a.in_out,CASE "
        "WHEN a.in_out IN('IN','IN/OUT') THEN 1 ELSE 0 END,CASE WHEN "
        "a.defaulted='Y' THEN 1 ELSE 0 END,0 FROM all_procedures p JOIN "
        "all_objects o ON o.object_id=p.object_id JOIN "
        "all_arguments a ON a.object_id=p.object_id AND "
        "a.subprogram_id=p.subprogram_id WHERE p.owner=" +
        sch +
        " AND p.procedure_name IS NULL AND o.object_type "
        "IN('FUNCTION','PROCEDURE') AND a.data_level=0 AND a.position>0 ORDER "
        "BY p.object_id,p.subprogram_id,a.position";
  } else if (p.driver == DatabaseKind::SQLServer) {
    auto current = query("SELECT SCHEMA_NAME(),CASE WHEN 'a' COLLATE "
                         "DATABASE_DEFAULT='A' THEN 0 ELSE 1 END",
                         1);
    out.caseSensitive = current.rows.value(0).value(1).toBool();
    out.searchPath = QStringList{
        schema.isEmpty() ? current.rows.value(0).value(0).toString() : schema};
    if (!out.searchPath.contains("dbo"))
      out.searchPath << "dbo";
    tables = "SELECT s.name,o.name,CASE WHEN o.type='V' THEN 'view' ELSE "
             "'table' END,CONVERT(varchar(20),o.object_id) FROM sys.objects o "
             "JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE o.type "
             "IN('U','V') AND o.is_ms_shipped=0 ORDER BY s.name,o.name";
    columns =
        "SELECT s.name,o.name,c.column_id,c.name,TYPE_NAME(c.user_type_id),CASE WHEN c.is_nullable=0 AND c.is_computed=0 THEN 1 ELSE 0 END "
        "FROM sys.columns c JOIN sys.objects o ON o.object_id=c.object_id JOIN "
        "sys.schemas s ON s.schema_id=o.schema_id WHERE o.type IN('U','V') AND "
        "o.is_ms_shipped=0 ORDER BY s.name,o.name,c.column_id";
    routines =
        "SELECT s.name,o.name,CASE WHEN o.type IN('P','PC') THEN 'procedure' "
        "ELSE 'function' END,'',CONVERT(varchar(20),o.object_id),0,COUNT(CASE "
        "WHEN p.parameter_id>0 THEN 1 END),0,o.type FROM sys.objects o JOIN "
        "sys.schemas s ON s.schema_id=o.schema_id LEFT JOIN sys.parameters p "
        "ON p.object_id=o.object_id WHERE o.type "
        "IN('P','PC','FN','IF','TF','FS','FT') AND o.is_ms_shipped=0 GROUP BY "
        "s.name,o.name,o.type,o.object_id ORDER BY s.name,o.name";
    parameters =
        "SELECT s.name,o.name,CASE WHEN o.type IN('P','PC') THEN 'procedure' "
        "ELSE 'function' "
        "END,CONVERT(varchar(20),o.object_id),p.parameter_id,p.name,TYPE_NAME("
        "p.user_type_id),CASE WHEN p.is_output=1 THEN 'INOUT' ELSE 'IN' "
        "END,1,p.has_default_value,0 FROM sys.objects o JOIN sys.schemas s ON "
        "s.schema_id=o.schema_id JOIN sys.parameters p ON "
        "p.object_id=o.object_id WHERE o.type "
        "IN('P','PC','FN','IF','TF','FS','FT') AND o.is_ms_shipped=0 AND "
        "p.parameter_id>0 ORDER BY o.object_id,p.parameter_id";
  } else
    throw std::runtime_error("metadata collection unavailable for this driver");
  QMap<QPair<QString, QString>, int> relations;
  QHash<QString, int> routineIds;
  auto tableRows = query(tables, 20000);
  out.tablesComplete = !tableRows.truncated && !tableRows.cellTruncated;
  for (const auto &row : tableRows.rows) {
    MetadataObject o;
    o.schema = row[0].toString();
    o.name = row[1].toString();
    o.kind = row[2].toString();
    o.database = p.database;
    o.key = stableKey(o.kind, o.schema, o.name);
    if (usesNativePostgres(p.driver) && o.kind == "table" && row.size() >= 6) {
      o.noDescendantsKnown = row[4].toInt() == 1;
      o.primaryKey = metadata_detail::decodePrimaryKey(row[5].toString());
      o.rowSecurityDisabledKnown = row.size() >= 7 && row[6].toInt() == 1;
    }
    relations.insert({o.schema, o.name}, out.objects.size());
    out.objects << o;
  }
  auto columnRows = query(columns, 50000);
  out.columnsComplete = !columnRows.truncated && !columnRows.cellTruncated;
  for (const auto &row : columnRows.rows) {
    auto found = relations.constFind({row[0].toString(), row[1].toString()});
    if (found != relations.cend()) {
      MetadataColumn column{row[3].toString(),row[4].toString(),{},row[2].toInt()};
      column.nonNullKnown=row.size()>5 && row[5].toInt()==1 && out.objects[*found].kind=="table";
      out.objects[*found].columns << std::move(column);
    }
  }
  auto routineRows = query(routines, 20000);
  out.routinesComplete = !routineRows.truncated && !routineRows.cellTruncated;
  for (const auto &row : routineRows.rows) {
    MetadataObject o;
    o.schema = row[0].toString();
    o.name = row[1].toString();
    o.kind = row[2].toString();
    o.type = row[3].toString();
    o.database = p.database;
    o.key = stableKey(o.kind, o.schema, row[4].toString());
    o.minArgs = row[5].toInt();
    o.maxArgs = row[6].toInt();
    o.variadic = row[7].toBool();
    using RK=sql::compiler::RoutineKind;using FV=sql::compiler::FunctionVolatility;
    if(o.kind=="procedure")o.routineKind=RK::Procedure;
    else if(isMySqlFamily(p.driver))o.routineKind=RK::Scalar;
    if(usesNativePostgres(p.driver) && row.size()>=11) {
      const auto kind=row[8].toString(),volatility=row[9].toString();
      o.routineKind=kind=="a"?RK::Aggregate:kind=="w"?RK::Window:kind=="p"?RK::Procedure:
        row[10].toBool()?RK::TableFunction:kind=="f"?RK::Scalar:RK::Unknown;
      o.volatility=volatility=="i"?FV::Immutable:volatility=="s"?FV::Stable:volatility=="v"?FV::Volatile:FV::Unknown;
    }
    if(p.driver==DatabaseKind::SQLite && row.size()>=10) {
      const auto kind=row[8].toString();
      o.routineKind=kind=="s"?RK::Scalar:kind=="a"?RK::Aggregate:kind=="w"?RK::Aggregate:RK::Unknown;
      // SQLite's 'w' routines may be used both as aggregate and window calls.
      // Dedicated ranking/offset routines require OVER, from the shared catalog.
      if(auto op=sql::compiler::builtinOperator(sql::compiler::Dialect::SQLite,{o.name.toStdString()},{},o.minArgs))
        if(op->kind==RK::Window)o.routineKind=RK::Window;
      o.volatility=(row[9].toLongLong() & 0x800)?FV::Immutable:FV::Unknown;
    }
    if(p.driver==DatabaseKind::SQLServer && row.size()>=9 && o.kind!="procedure") {
      const auto kind=row[8].toString();
      o.routineKind=(kind=="IF" || kind=="TF" || kind=="FT")?RK::TableFunction:
        (kind=="FN" || kind=="FS")?RK::Scalar:RK::Unknown;
    }
    routineIds.insert(o.key, out.objects.size());
    out.objects << o;
  }
  if (!parameters.isEmpty()) {
    auto rows = query(parameters, 50000);
    out.routinesComplete &= !rows.truncated && !rows.cellTruncated;
    for (const auto &row : rows.rows) {
      auto id =
          stableKey(row[2].toString(), row[0].toString(), row[3].toString());
      auto found = routineIds.constFind(id);
      if (found == routineIds.cend())
        continue;
      MetadataColumn c;
      c.ordinal = row[4].toInt();
      c.name = row[5].toString();
      c.type = row[6].toString();
      c.mode = row[7].toString();
      c.input = row[8].toBool();
      c.hasDefault = row[9].toBool();
      c.variadic = row[10].toBool();
      out.objects[*found].columns << c;
    }
  }
  if (usesNativePostgres(p.driver))
    for (auto &routine : out.objects)
      if (routine.kind == "function" || routine.kind == "procedure") {
        int defaults = std::max(0, routine.maxArgs - routine.minArgs -
                                       (routine.variadic ? 1 : 0));
        for (int i = routine.columns.size() - 1; i >= 0 && defaults > 0; --i)
          if (routine.columns[i].input && !routine.columns[i].variadic) {
            routine.columns[i].hasDefault = true;
            --defaults;
          }
      }
  const auto readDefinitions = [&](const QString& sql, bool routine, bool lines = false) {
    if (cancel->load() || elapsed.elapsed() > 12000) { out.definitionsComplete = false; return; }
    QueryLimits limits;
    limits.maxRows = 20000;
    limits.maxBytes = 2 * 1024 * 1024;
    limits.maxCellBytes = 64 * 1024;
    try {
      const auto result = db.execute(sql, limits, cancel);
      if (!result.error.isEmpty() || result.cancelled) { out.definitionsComplete = false; return; }
      out.definitionsComplete &= !result.truncated && !result.cellTruncated;
      QHash<QString, int> byName;
      if (lines) for (auto it = routineIds.cbegin(); it != routineIds.cend(); ++it) {
        const auto& object = out.objects[it.value()];
        byName.insert(stableKey(object.kind, object.schema, object.name), it.value());
      }
      QSet<int> oversized;
      for (const auto& row : result.rows) {
        if (cancel->load()) return;
        const auto owner = row.value(0).toString(), name = row.value(1).toString();
        int index = -1;
        if (routine) {
          const auto kind = row.value(2).toString();
          index = lines ? byName.value(stableKey(kind, owner, name), -1)
                        : routineIds.value(stableKey(kind, owner, row.value(3).toString()), -1);
        } else {
          index = relations.value({owner, name}, -1);
        }
        if (index < 0 || oversized.contains(index)) continue;
        auto definition = row.value(4).toString();
        auto& output = out.objects[index].definition;
        if (lines) output += definition;
        else output = std::move(definition);
        if (output.toUtf8().size() >= limits.maxCellBytes) {
          output.clear();
          oversized.insert(index);
          out.definitionsComplete = false;
        }
      }
      if (lines && result.truncated && !result.rows.isEmpty()) {
        const auto& last = result.rows.back();
        const auto index = byName.value(stableKey(last.value(2).toString(), last.value(0).toString(),
                                                  last.value(1).toString()), -1);
        if (index >= 0) out.objects[index].definition.clear();
      }
    } catch (const std::exception&) { out.definitionsComplete = false; }
  };
  out.definitionsRead = out.definitionsComplete = true;
  if (isMySqlFamily(p.driver)) {
    readDefinitions("SELECT table_schema,table_name,'view','',view_definition FROM information_schema.views "
                    "WHERE table_schema=" + database + " ORDER BY table_name", false);
    readDefinitions("SELECT routine_schema,routine_name,LOWER(routine_type),specific_name,routine_definition "
                    "FROM information_schema.routines WHERE routine_schema=" + database + " ORDER BY routine_name", true);
  } else if (usesNativePostgres(p.driver)) {
    const QString userSchemas = " AND n.nspname NOT IN('pg_catalog','information_schema') AND n.nspname NOT LIKE 'pg_toast%' "
                                "AND n.nspname NOT LIKE 'pg_temp_%' AND has_schema_privilege(n.oid,'USAGE')";
    readDefinitions("SELECT n.nspname,c.relname,'view','',pg_get_viewdef(c.oid,false) FROM pg_class c "
                    "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE c.relkind IN('v','m')" + userSchemas +
                    " AND has_table_privilege(c.oid,'SELECT,INSERT,UPDATE,DELETE,REFERENCES') ORDER BY n.nspname,c.relname", false);
    readDefinitions("SELECT n.nspname,p.proname,CASE WHEN p.prokind='p' THEN 'procedure' ELSE 'function' END,"
                    "p.oid::text,pg_get_functiondef(p.oid) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace "
                    "WHERE p.prokind IN('f','p','w')" + userSchemas + " AND has_function_privilege(p.oid,'EXECUTE') "
                    "AND (pg_has_role(p.proowner,'USAGE') OR has_schema_privilege(n.oid,'CREATE')) "
                    "ORDER BY n.nspname,p.proname,p.oid", true);
  } else if (p.driver == DatabaseKind::SQLite) {
    if (std::any_of(out.objects.cbegin(), out.objects.cend(), [](const auto& object) { return object.kind == "view"; }))
      readDefinitions("SELECT 'main',name,'view','',sql FROM main.sqlite_schema WHERE type='view' ORDER BY name", false);
  } else if (p.driver == DatabaseKind::Dameng) {
    const auto scope = searchOnly ? QString{} : " AND owner=" + sch;
    readDefinitions("SELECT owner,view_name,'view',view_name,text FROM all_views WHERE 1=1" +
                    scope + " ORDER BY owner,view_name", false);
    readDefinitions("SELECT owner,name,LOWER(type),name,text FROM all_source "
                    "WHERE type IN('FUNCTION','PROCEDURE')" + scope + " ORDER BY owner,name,type,line", true, true);
  } else if (usesOracleSql(p.driver)) {
    readDefinitions("SELECT owner,view_name,'view',view_name,text FROM all_views WHERE owner=" + sch +
                    " ORDER BY view_name", false);
    readDefinitions("SELECT owner,name,LOWER(type),name,text FROM all_source WHERE owner=" + sch +
                    " AND type IN('FUNCTION','PROCEDURE') ORDER BY name,type,line", true, true);
  } else if (p.driver == DatabaseKind::SQLServer) {
    const QString modules = "CONVERT(varchar(20),o.object_id),m.definition FROM sys.sql_modules m "
                            "JOIN sys.objects o ON o.object_id=m.object_id JOIN sys.schemas s ON s.schema_id=o.schema_id "
                            "WHERE o.is_ms_shipped=0 AND ";
    readDefinitions("SELECT s.name,o.name,'view'," + modules + "o.type='V' ORDER BY s.name,o.name", false);
    readDefinitions("SELECT s.name,o.name,CASE WHEN o.type='P' THEN 'procedure' ELSE 'function' END," + modules +
                    "o.type IN('P','FN','IF','TF') ORDER BY s.name,o.name", true);
  }
  // Optional positive symbols: permission/version failures must not disable
  // independently complete table/column/routine validation. Never read data.
  QString extraObjects;
  if(isMySqlFamily(p.driver)) {
    extraObjects="SELECT '',schema_name,'database','',schema_name FROM information_schema.schemata UNION ALL "
      "SELECT table_schema,index_name,'index',table_name,CONCAT(table_name,':',index_name) FROM information_schema.statistics WHERE table_schema="+database+
      " AND index_name<>'PRIMARY' GROUP BY table_schema,table_name,index_name UNION ALL "
      "SELECT trigger_schema,trigger_name,'trigger',event_object_table,trigger_name FROM information_schema.triggers WHERE trigger_schema="+database+
      " UNION ALL SELECT event_schema,event_name,'event','',event_name FROM information_schema.events WHERE event_schema="+database;
  } else if(usesNativePostgres(p.driver)) {
    extraObjects="SELECT ''::text,datname::text,'database'::text,''::text,oid::text FROM pg_database WHERE datallowconn AND has_database_privilege(oid,'CONNECT') UNION ALL "
      "SELECT ''::text,nspname::text,'schema','',oid::text FROM pg_namespace WHERE nspname NOT LIKE 'pg_toast%' AND nspname NOT LIKE 'pg_temp_%' AND has_schema_privilege(oid,'USAGE') UNION ALL "
      "SELECT n.nspname,c.relname,CASE WHEN c.relkind='S' THEN 'sequence' ELSE 'index' END,'',c.oid::text FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE c.relkind IN('S','i','I') AND n.nspname NOT IN('pg_catalog','information_schema') AND n.nspname NOT LIKE 'pg_%' AND has_schema_privilege(n.oid,'USAGE') UNION ALL "
      "SELECT n.nspname,t.tgname,'trigger',c.relname,t.oid::text FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid JOIN pg_namespace n ON n.oid=c.relnamespace WHERE NOT t.tgisinternal AND has_schema_privilege(n.oid,'USAGE') AND has_table_privilege(c.oid,'TRIGGER') UNION ALL "
      "SELECT n.nspname,t.typname,'type','',t.oid::text FROM pg_type t JOIN pg_namespace n ON n.oid=t.typnamespace WHERE t.typtype IN('c','d','e','r','m') AND n.nspname NOT IN('pg_catalog','information_schema') AND n.nspname NOT LIKE 'pg_%' AND has_type_privilege(t.oid,'USAGE')";
  } else if(p.driver==DatabaseKind::SQLite) {
    extraObjects="SELECT 'main',name,type,tbl_name,name FROM main.sqlite_schema WHERE type IN('index','trigger') AND sql IS NOT NULL AND name NOT LIKE 'sqlite_%' UNION ALL SELECT '', 'main','schema','','main'";
  } else if(usesSchemaAtConnection(p.driver)) {
    extraObjects="SELECT owner,object_name,LOWER(object_type),'',TO_CHAR(object_id) FROM all_objects WHERE owner="+sch+
      " AND object_type IN('INDEX','SEQUENCE','TRIGGER','TYPE','SYNONYM','PACKAGE') UNION ALL SELECT '',username,'schema','',username FROM all_users";
  } else if(p.driver==DatabaseKind::SQLServer) {
    extraObjects="SELECT '' COLLATE DATABASE_DEFAULT,name COLLATE DATABASE_DEFAULT,'database','',CONVERT(nvarchar(20),database_id) FROM sys.databases WHERE HAS_DBACCESS(name)=1 UNION ALL "
      "SELECT '',name COLLATE DATABASE_DEFAULT,'schema','',CONVERT(nvarchar(20),schema_id) FROM sys.schemas UNION ALL "
      "SELECT s.name COLLATE DATABASE_DEFAULT,i.name COLLATE DATABASE_DEFAULT,'index',o.name COLLATE DATABASE_DEFAULT,CONCAT(o.object_id,':',i.index_id) FROM sys.indexes i JOIN sys.objects o ON o.object_id=i.object_id JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE i.name IS NOT NULL AND o.is_ms_shipped=0 AND i.is_primary_key=0 UNION ALL "
      "SELECT s.name COLLATE DATABASE_DEFAULT,o.name COLLATE DATABASE_DEFAULT,CASE o.type WHEN 'SO' THEN 'sequence' WHEN 'SN' THEN 'synonym' ELSE 'trigger' END,'',CONVERT(nvarchar(20),o.object_id) FROM sys.objects o JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE o.type IN('SO','SN','TR','TA') AND o.is_ms_shipped=0 UNION ALL "
      "SELECT s.name COLLATE DATABASE_DEFAULT,t.name COLLATE DATABASE_DEFAULT,'type','',CONVERT(nvarchar(20),t.user_type_id) FROM sys.types t JOIN sys.schemas s ON s.schema_id=t.schema_id WHERE t.is_user_defined=1";
  }
  if(!extraObjects.isEmpty() && elapsed.elapsed()<12000 && out.objects.size()<20000 && !cancel->load()) {
    try {
      const int limit=std::min(4096,20000-int(out.objects.size()));
      auto rows=query(extraObjects,limit);
      if(rows.truncated || rows.cellTruncated)out.objectWarning="其他对象候选已达到读取上限";
      for(const auto &row:rows.rows) {
        if(row.size()<5)throw std::runtime_error("invalid extra object metadata shape");
        MetadataObject o;o.schema=row[0].toString();o.name=row[1].toString();o.kind=row[2].toString();o.type=row[3].toString();o.database=p.database;
        o.key=stableKey(o.kind,o.schema,row[4].toString());out.objects.push_back(std::move(o));
      }
    } catch(const std::exception &) {out.objectWarning="其他对象目录未完成；已读取的表、字段和例程仍可使用";}
  } else if(!extraObjects.isEmpty())out.objectWarning="本轮其他对象候选读取已延后";
  if (out.objects.size() > 20000)
    throw std::runtime_error("metadata object limit exceeded");
  if (searchOnly) {
    if (cancel->load()) throw std::runtime_error("metadata refresh cancelled");
    out.refreshedMs = QDateTime::currentMSecsSinceEpoch();
    return out;
  }
  // Optional cost metadata must not disable name validation when statistics
  // permissions, server versions or analysis coverage differ.
  if (!cancel->load() && elapsed.elapsed() < 12000) {
    try {
      auto stats = collectPlannerStatistics(db, p, schema, cancel);
      out.statisticsWarning = stats.warning;
      out.statistics =
          std::make_shared<const PlannerStatisticsSnapshot>(std::move(stats));
    } catch (const std::exception &e) {
      out.statisticsWarning = QString::fromUtf8(e.what()).left(1024);
    }
  } else
    out.statisticsWarning = "名称元数据已读取；本轮未继续读取目录统计";
  if (usesNativePostgres(p.driver) && !cancel->load() && elapsed.elapsed()<12000) {
    // A hard proof is independent of ANALYZE and does not read table data.
    // Included columns do not participate in uniqueness.
    constexpr auto uniqueIndexes =
        "SELECT n.nspname,c.relname,pg_catalog.json_agg(a.attname ORDER BY k.ord)::text "
        "FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class c ON c.oid=i.indrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_catalog.pg_class ind ON ind.oid=i.indexrelid "
        "JOIN pg_catalog.pg_am am ON am.oid=ind.relam AND am.amname='btree' "
        "CROSS JOIN LATERAL pg_catalog.unnest(i.indkey) WITH ORDINALITY AS k(attnum,ord) "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid=c.oid AND a.attnum=k.attnum "
        "JOIN pg_catalog.pg_opclass op ON op.oid=i.indclass[(k.ord-1)::int] "
        "JOIN pg_catalog.pg_namespace ops ON ops.oid=op.opcnamespace AND ops.nspname='pg_catalog' "
        "WHERE c.relkind='r' AND i.indisunique AND NOT i.indisprimary AND i.indimmediate "
        "AND i.indisvalid AND i.indisready AND i.indislive AND NOT i.indcheckxmin "
        "AND i.indpred IS NULL AND i.indexprs IS NULL AND k.ord<=i.indnkeyatts "
        "AND a.attnotnull AND NOT a.attisdropped AND a.attgenerated='' "
        "AND op.opcdefault AND op.opcintype=a.atttypid AND "
        "((a.atttypid='pg_catalog.int2'::pg_catalog.regtype AND op.opcname='int2_ops') OR "
        "(a.atttypid='pg_catalog.int4'::pg_catalog.regtype AND op.opcname='int4_ops') OR "
        "(a.atttypid='pg_catalog.int8'::pg_catalog.regtype AND op.opcname='int8_ops')) "
        "AND n.nspname NOT IN ('pg_catalog','information_schema') AND n.nspname NOT LIKE 'pg_toast%' "
        "AND n.nspname NOT LIKE 'pg_temp_%' AND has_schema_privilege(n.oid,'USAGE') "
        "AND has_table_privilege(c.oid,'SELECT,INSERT,UPDATE,DELETE,REFERENCES') "
        "GROUP BY n.nspname,c.relname,i.indexrelid,i.indnkeyatts "
        "HAVING pg_catalog.count(*)=i.indnkeyatts ORDER BY n.nspname,c.relname,i.indexrelid";
    try {
      const auto proofs=query(QString::fromLatin1(uniqueIndexes),20000);
      for (const auto &row:proofs.rows) {
        if (row.size()!=3) continue;
        const auto owner=relations.constFind({row[0].toString(),row[1].toString()});
        if (owner==relations.cend()) continue;
        auto &known=out.objects[*owner].strictUniqueKeys;
        if (known.size()>=metadata_detail::maxSecondaryKeys) continue;
        auto key=metadata_detail::decodePrimaryKey(row[2].toString());
        if (!key.isEmpty() && key.size()<=metadata_detail::maxSecondaryKeyColumns && !known.contains(key))
          known.push_back(std::move(key));
      }
    } catch (const std::exception &) {
      if (!out.statisticsWarning.isEmpty()) out.statisticsWarning+=" · ";
      out.statisticsWarning+="唯一索引约束信息未读取";
    }
  }
  if(cancel->load())throw std::runtime_error("metadata refresh cancelled");
  prepareMetadataTypeFacts(out,cancel);
  if(cancel->load())throw std::runtime_error("metadata type enrichment cancelled");
  out.refreshedMs = QDateTime::currentMSecsSinceEpoch();
  out.verified = true;
  if (p.http.enabled || !p.advanced.initialSql.trimmed().isEmpty()) {
    out.tablesComplete = out.columnsComplete = out.routinesComplete = false;
  }
  return out;
}
} // namespace navidog
