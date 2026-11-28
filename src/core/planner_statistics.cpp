#include "navidog/planner_statistics.hpp"
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace navidog {
namespace {
namespace opt = sql::optimizer;
std::optional<double> numeric(const QVariant &v) {
  if (v.isNull())
    return {};
  bool ok = false;
  double n = v.toDouble(&ok);
  return ok && std::isfinite(n) && n >= 0 ? std::optional<double>{n}
                                          : std::optional<double>{};
}
QString literal(QString value, const ConnectionProfile &p) {
  if (isMySqlFamily(p.driver))
    return "CONVERT(0x" + QString::fromLatin1(value.toUtf8().toHex()) +
           " USING utf8mb4)";
  value.replace("'", "''");
  if (p.driver == DatabaseKind::PostgreSQL)
    value.replace("\\", "\\\\");
  return (p.driver == DatabaseKind::PostgreSQL  ? "E'"
          : p.driver == DatabaseKind::SQLServer ? "N'"
                                                : "'") +
         value + "'";
}
std::string text(const QJsonObject &o, const QString &k) {
  return o[k].toString().toStdString();
}
} // namespace
PlannerStatisticsSnapshot collectPlannerStatistics(Database &db,
                                                   const ConnectionProfile &p,
                                                   const QString &schema,
                                                   const Cancellation &cancel) {
  PlannerStatisticsSnapshot out;
  QElapsedTimer timer;
  timer.start();
  qint64 bytes = 0;
  auto query = [&](const QString &sql, int count) {
    if (cancel->load() || timer.elapsed() > 6000)
      throw std::runtime_error("目录统计读取已取消或达到时间预算");
    QueryLimits limits;
    limits.maxRows = count;
    limits.maxBytes = 2 * 1024 * 1024;
    limits.maxCellBytes = 4096;
    auto r = db.execute(sql, limits, cancel);
    if (r.cancelled || !r.error.isEmpty())
      throw std::runtime_error(r.error.isEmpty() ? "目录统计读取已取消"
                                                 : r.error.toStdString());
    bytes += r.cachedBytes;
    if (bytes > 8 * 1024 * 1024)
      throw std::runtime_error("目录统计读取达到字节预算");
    return r;
  };
  const auto database = literal(p.database, p), sch = literal(schema, p);
  QString tables, columns, indexes;
  bool guessedWidth = false;
  if (p.driver == DatabaseKind::PostgreSQL) {
    const QString filter =
        "n.nspname NOT IN('pg_catalog','information_schema') AND n.nspname NOT "
        "LIKE 'pg_toast%' AND n.nspname NOT LIKE 'pg_temp_%' AND "
        "has_table_privilege(c.oid,'SELECT,INSERT,UPDATE,DELETE,REFERENCES')";
    tables = "SELECT n.nspname,c.relname,c.reltuples,CASE WHEN c.reltuples>0 "
             "AND c.relpages>0 THEN "
             "c.relpages::numeric*current_setting('block_size')::numeric/"
             "c.reltuples ELSE NULL END FROM pg_class c JOIN pg_namespace n ON "
             "n.oid=c.relnamespace WHERE c.relkind IN('r','p','m') AND "
             "c.reltuples>=0 AND " +
             filter + " ORDER BY n.nspname,c.relname";
    columns =
        "SELECT s.schemaname,s.tablename,s.attname,CASE WHEN s.n_distinct<0 "
        "THEN -s.n_distinct*c.reltuples ELSE s.n_distinct "
        "END,s.null_frac,s.avg_width FROM pg_stats s JOIN pg_namespace n ON "
        "n.nspname=s.schemaname JOIN pg_class c ON c.relnamespace=n.oid AND "
        "c.relname=s.tablename WHERE c.reltuples>=0 AND " +
        filter + " ORDER BY s.schemaname,s.tablename,s.attname";
    indexes =
        "SELECT "
        "n.nspname,c.relname,ic.relname,a.attname,k.ordinality,i.indisunique,"
        "CASE WHEN am.amname='btree' AND i.indoption[(k.ordinality-1)::int]=0 "
        "THEN 1 ELSE 0 END,ic.relpages FROM pg_index i JOIN pg_class c ON "
        "c.oid=i.indrelid JOIN pg_namespace n ON n.oid=c.relnamespace JOIN "
        "pg_class ic ON ic.oid=i.indexrelid JOIN pg_am am ON am.oid=ic.relam "
        "CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY "
        "k(attnum,ordinality) JOIN pg_attribute a ON a.attrelid=c.oid AND "
        "a.attnum=k.attnum WHERE i.indisvalid AND i.indisready AND i.indpred "
        "IS NULL AND i.indexprs IS NULL AND k.ordinality<=i.indnkeyatts AND " +
        filter + " ORDER BY n.nspname,c.relname,ic.relname,k.ordinality";
  } else if (isMySqlFamily(p.driver)) {
    tables = "SELECT table_schema,table_name,table_rows,CASE WHEN "
             "avg_row_length>0 THEN avg_row_length ELSE NULL END FROM "
             "information_schema.tables WHERE table_schema=" +
             database +
             " AND table_type='BASE TABLE' AND table_rows IS NOT NULL ORDER BY "
             "table_name";
    // Cardinality of a composite prefix is not an individual column NDV.
    columns =
        "SELECT "
        "s.table_schema,s.table_name,s.column_name,s.cardinality,NULL,NULL "
        "FROM information_schema.statistics s WHERE s.table_schema=" +
        database +
        " AND s.seq_in_index=1 AND s.column_name IS NOT NULL ORDER BY "
        "s.table_name,s.index_name";
    indexes = "SELECT "
              "s.table_schema,s.table_name,s.index_name,s.column_name,s.seq_in_"
              "index,CASE WHEN s.non_unique=0 THEN 1 ELSE 0 END,CASE WHEN "
              "s.index_type='BTREE' AND s.collation='A' THEN 1 ELSE 0 END,NULL "
              "FROM information_schema.statistics s WHERE s.table_schema=" +
              database +
              " AND s.column_name IS NOT NULL AND NOT EXISTS(SELECT 1 FROM "
              "information_schema.statistics e WHERE "
              "e.table_schema=s.table_schema AND e.table_name=s.table_name AND "
              "e.index_name=s.index_name AND e.column_name IS NULL) ORDER BY "
              "s.table_name,s.index_name,s.seq_in_index";
  } else if (p.driver == DatabaseKind::Oracle) {
    const QString owner =
        schema.isEmpty() ? "SYS_CONTEXT('USERENV','CURRENT_SCHEMA')" : sch;
    tables = "SELECT owner,table_name,num_rows,CASE WHEN avg_row_len>0 THEN "
             "avg_row_len ELSE NULL END FROM all_tables WHERE owner=" +
             owner + " AND num_rows IS NOT NULL ORDER BY table_name";
    columns =
        "SELECT s.owner,s.table_name,s.column_name,s.num_distinct,CASE WHEN "
        "t.num_rows>0 THEN s.num_nulls/t.num_rows ELSE 0 END,s.avg_col_len "
        "FROM all_tab_col_statistics s JOIN all_tables t ON t.owner=s.owner "
        "AND t.table_name=s.table_name WHERE s.owner=" +
        owner +
        " AND t.num_rows IS NOT NULL ORDER BY s.table_name,s.column_name";
    indexes = "SELECT "
              "i.table_owner,i.table_name,i.index_name,c.column_name,c.column_"
              "position,CASE WHEN i.uniqueness='UNIQUE' THEN 1 ELSE 0 END,CASE "
              "WHEN c.descend='ASC' THEN 1 ELSE 0 END,i.leaf_blocks FROM "
              "all_indexes i JOIN all_ind_columns c ON c.index_owner=i.owner "
              "AND c.index_name=i.index_name WHERE i.table_owner=" +
              owner +
              " AND i.status='VALID' AND i.index_type='NORMAL' ORDER BY "
              "i.table_name,i.index_name,c.column_position";
  } else if (p.driver == DatabaseKind::SQLServer) {
    tables =
        "SELECT s.name,t.name,SUM(CONVERT(float,p.rows)),CASE WHEN "
        "SUM(CONVERT(float,p.rows))>0 THEN "
        "SUM(CONVERT(float,a.data_pages))*8192/SUM(CONVERT(float,p.rows)) ELSE "
        "64 END FROM sys.tables t JOIN sys.schemas s ON "
        "s.schema_id=t.schema_id JOIN sys.partitions p ON "
        "p.object_id=t.object_id AND p.index_id IN(0,1) LEFT JOIN "
        "sys.allocation_units a ON a.container_id=p.hobt_id AND a.type=1 WHERE "
        "t.is_ms_shipped=0 GROUP BY s.name,t.name ORDER BY s.name,t.name";
    indexes =
        "SELECT s.name,t.name,i.name,c.name,k.key_ordinal,i.is_unique,CASE "
        "WHEN i.type=2 AND k.is_descending_key=0 THEN 1 ELSE 0 END,NULL FROM "
        "sys.indexes i JOIN sys.tables t ON t.object_id=i.object_id JOIN "
        "sys.schemas s ON s.schema_id=t.schema_id JOIN sys.index_columns k ON "
        "k.object_id=i.object_id AND k.index_id=i.index_id JOIN sys.columns c "
        "ON c.object_id=k.object_id AND c.column_id=k.column_id WHERE "
        "t.is_ms_shipped=0 AND i.is_disabled=0 AND i.is_hypothetical=0 AND "
        "i.has_filter=0 AND i.type IN(1,2) AND k.key_ordinal>0 ORDER BY "
        "s.name,t.name,i.name,k.key_ordinal";
  } else if (p.driver == DatabaseKind::SQLite) {
    auto rows = query(
        "SELECT tbl,idx,stat FROM main.sqlite_stat1 ORDER BY tbl,idx", 20000);
    if (rows.truncated || rows.cellTruncated) {
      out.warning = "sqlite_stat1 超过读取上限，未使用不完整统计";
      return out;
    }
    QMap<QString, opt::TableStatistics> found;
    for (const auto &row : rows.rows) {
      const auto parts = row.value(2).toString().split(
          QRegularExpression("\\s+"), Qt::SkipEmptyParts);
      if (parts.isEmpty())
        continue;
      const auto count = numeric(parts.front());
      if (!count)
        continue;
      auto &t = found[row.value(0).toString()];
      t.database = p.database.toStdString();
      t.schema = "main";
      t.name = row.value(0).toString().toStdString();
      t.rows = *count;
      t.averageRowBytes = 64;
      t.rowBytesAssumed = true;
      t.source = opt::StatisticsSource::ServerCatalog;
      t.provenance = "sqlite_stat1；平均行宽暂用 64 字节假设";
    }
    for (const auto &t : found)
      out.tables.push_back(t);
    out.complete = true;
    out.refreshedMs = QDateTime::currentMSecsSinceEpoch();
    return out;
  } else {
    out.warning = "此数据库尚无目录统计读取器";
    return out;
  }
  QMap<QPair<QString, QString>, std::size_t> positions;
  auto r = query(tables, 4096);
  out.complete = !r.truncated && !r.cellTruncated;
  for (const auto &row : r.rows) {
    auto count = numeric(row.value(2));
    if (!count || *count > 1e15)
      continue;
    opt::TableStatistics t;
    t.database = p.database.toStdString();
    t.schema = row.value(0).toString().toStdString();
    t.name = row.value(1).toString().toStdString();
    t.rows = *count;
    auto width = numeric(row.value(3));
    t.averageRowBytes = width && *width > 0 ? std::min(1e9, *width) : 64;
    t.rowBytesAssumed = guessedWidth || !width || *width <= 0;
    t.source = opt::StatisticsSource::ServerCatalog;
    t.provenance =
        (databaseKindKey(p.driver) + " 系统目录估计；未扫描或采样业务数据")
            .toStdString();
    positions.insert({row.value(0).toString(), row.value(1).toString()},
                     out.tables.size());
    out.tables.push_back(std::move(t));
  }
  if (!columns.isEmpty()) {
    r = query(columns, 20000);
    out.complete &= !r.truncated && !r.cellTruncated;
    if (!r.truncated && !r.cellTruncated)
      for (const auto &row : r.rows) {
        auto it = positions.constFind(
            {row.value(0).toString(), row.value(1).toString()});
        if (it == positions.cend())
          continue;
        auto &t = out.tables[*it];
        opt::ColumnStatistics col;
        col.name = row.value(2).toString().toStdString();
        auto distinct = numeric(row.value(3));
        if (distinct)
          col.distinct = std::min(t.rows, *distinct);
        auto nulls = numeric(row.value(4));
        if (nulls && *nulls <= 1)
          col.nullFraction = *nulls;
        col.averageBytes = numeric(row.value(5));
        if (t.columns.size() < 4096 &&
            std::none_of(t.columns.begin(), t.columns.end(),
                         [&](const auto &c) { return c.name == col.name; }))
          t.columns.push_back(col);
      }
  }
  if (!indexes.isEmpty()) {
    r = query(indexes, 20000);
    out.complete &= !r.truncated && !r.cellTruncated;
    // A truncated index may omit trailing keys and cannot be considered unique.
    if (!r.truncated && !r.cellTruncated)
      for (const auto &row : r.rows) {
        auto it = positions.constFind(
            {row.value(0).toString(), row.value(1).toString()});
        if (it == positions.cend())
          continue;
        auto &t = out.tables[*it];
        const auto indexName = row.value(2).toString().toStdString();
        auto index =
            std::find_if(t.indexes.begin(), t.indexes.end(),
                         [&](const auto &v) { return v.name == indexName; });
        if (index == t.indexes.end()) {
          if (t.indexes.size() >= 4096)
            continue;
          t.indexes.push_back({indexName,
                               {},
                               row.value(5).toBool(),
                               true,
                               numeric(row.value(7))});
          index = std::prev(t.indexes.end());
        }
        index->ordered &= row.value(6).toBool();
        index->columns.push_back(row.value(3).toString().toStdString());
      }
  }
  if (!out.complete)
    out.warning = "部分目录统计达到读取上限；缺失的列或索引统计不会猜测补全";
  out.refreshedMs = QDateTime::currentMSecsSinceEpoch();
  return out;
}
QJsonObject plannerTableJson(const opt::TableStatistics &t) {
  QJsonObject o{{"database", QString::fromStdString(t.database)},
                {"schema", QString::fromStdString(t.schema)},
                {"name", QString::fromStdString(t.name)},
                {"rows", t.rows},
                {"averageRowBytes", t.averageRowBytes},
                {"rowBytesAssumed", t.rowBytesAssumed},
                {"provenance", QString::fromStdString(t.provenance)}};
  QJsonArray columns, indexes;
  for (const auto &c : t.columns) {
    QJsonObject v{{"name", QString::fromStdString(c.name)}};
    if (c.distinct)
      v.insert("distinct", *c.distinct);
    if (c.nullFraction)
      v.insert("nullFraction", *c.nullFraction);
    if (c.averageBytes)
      v.insert("averageBytes", *c.averageBytes);
    columns.append(v);
  }
  for (const auto &i : t.indexes) {
    QJsonArray names;
    for (const auto &n : i.columns)
      names.append(QString::fromStdString(n));
    QJsonObject v{{"name", QString::fromStdString(i.name)},
                  {"columns", names},
                  {"unique", i.unique},
                  {"ordered", i.ordered}};
    if (i.pages)
      v.insert("pages", *i.pages);
    indexes.append(v);
  }
  o.insert("columns", columns);
  o.insert("indexes", indexes);
  return o;
}
opt::TableStatistics plannerTableFromJson(const QJsonObject &o) {
  opt::TableStatistics t;
  t.database = text(o, "database");
  t.schema = text(o, "schema");
  t.name = text(o, "name");
  t.rows = o["rows"].toDouble(-1);
  t.averageRowBytes = o["averageRowBytes"].toDouble(-1);
  t.rowBytesAssumed = o["rowBytesAssumed"].toBool();
  t.provenance = text(o, "provenance");
  t.source = opt::StatisticsSource::ServerCatalog;
  if (t.name.empty() || !std::isfinite(t.rows) || t.rows < 0 || t.rows > 1e15 ||
      !std::isfinite(t.averageRowBytes) || t.averageRowBytes <= 0 ||
      t.averageRowBytes > 1e9)
    throw std::runtime_error("本地目录表统计数值无效");
  auto columns = o["columns"].toArray(), indexes = o["indexes"].toArray();
  if (columns.size() > 4096 || indexes.size() > 4096)
    throw std::runtime_error("本地目录列或索引统计超过限额");
  auto optional = [](const QJsonObject &object,
                     const QString &key) -> std::optional<double> {
    if (!object.contains(key))
      return {};
    const auto v = object[key].toDouble(-1);
    if (!std::isfinite(v) || v < 0)
      throw std::runtime_error("本地目录统计数值无效");
    return v;
  };
  for (auto value : columns) {
    auto v = value.toObject();
    opt::ColumnStatistics c;
    c.name = text(v, "name");
    c.distinct = optional(v, "distinct");
    c.nullFraction = optional(v, "nullFraction");
    c.averageBytes = optional(v, "averageBytes");
    if ((c.distinct && *c.distinct > t.rows + 1) ||
        (c.nullFraction && *c.nullFraction > 1))
      throw std::runtime_error("本地目录 NDV / NULL 统计无效");
    t.columns.push_back(c);
  }
  for (auto value : indexes) {
    auto v = value.toObject();
    opt::IndexStatistics i;
    i.name = text(v, "name");
    i.unique = v["unique"].toBool();
    i.ordered = v["ordered"].toBool();
    i.pages = optional(v, "pages");
    auto names = v["columns"].toArray();
    if (names.size() > 4096)
      throw std::runtime_error("本地索引列超过限额");
    for (auto n : names)
      i.columns.push_back(n.toString().toStdString());
    t.indexes.push_back(i);
  }
  return t;
}
} // namespace navidog
