#include "navidog/estimated_plan.hpp"
#include "navidog/types.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace navidog {
namespace {
std::optional<double> number(const QJsonValue &v) {
  bool ok = v.isDouble();
  double n = ok ? v.toDouble() : v.toString().toDouble(&ok);
  if (!ok || !std::isfinite(n) || n < 0)
    return {};
  return n;
}
std::optional<double> number(const QString &v) { return number(QJsonValue(v)); }
QString shortValue(const QJsonValue &v) {
  if (v.isString())
    return v.toString().left(2048);
  if (v.isDouble())
    return QString::number(v.toDouble(), 'g', 15);
  if (v.isBool())
    return v.toBool() ? "true" : "false";
  if (v.isNull())
    return "null";
  return QString::fromUtf8(
             v.isArray()
                 ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)
                 : QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact))
      .left(2048);
}
struct Decoder {
  EstimatedPlan &plan;
  PlanParseLimits limits;
  int add(EstimatedPlanNode node, int parent, int depth) {
    if (plan.nodes.size() >= limits.maxNodes || depth > limits.maxDepth)
      throw std::runtime_error("计划节点或层级超过显示解析限额");
    const auto id = int(plan.nodes.size());
    plan.nodes.append(std::move(node));
    if (parent >= 0)
      plan.nodes[parent].children.append(id);
    else
      plan.roots.append(id);
    return id;
  }
  void dameng(const QueryResult &result) {
    const auto columns = result.columns;
    const int level = columns.indexOf("LEVEL_ID");
    const int operation = columns.indexOf("OPERATION");
    if (level < 0 || operation < 0)
      throw std::runtime_error("达梦计划缺少 LEVEL_ID 或 OPERATION");
    const int table = columns.indexOf("TAB_NAME");
    const int index = columns.indexOf("IDX_NAME");
    const int rows = columns.indexOf("ROW_NUMS");
    const int bytes = columns.indexOf("BYTES");
    const int cost = columns.indexOf("COST");
    QVector<int> stack;
    int rootLevel = -1;
    for (const auto &row : result.rows) {
      if (row.size() != columns.size())
        throw std::runtime_error("达梦计划列数不一致");
      bool valid = false;
      const int nativeLevel = row[level].toInt(&valid);
      if (!valid || nativeLevel < 0 || row[operation].toString().isEmpty())
        throw std::runtime_error("达梦计划行缺少层级或算子");
      if (rootLevel < 0)
        rootLevel = nativeLevel;
      const int depth = nativeLevel - rootLevel;
      if (depth < 0 || depth > stack.size())
        throw std::runtime_error("达梦计划缺少父节点");
      stack.resize(depth);
      EstimatedPlanNode node;
      node.operation = row[operation].toString();
      node.object = row.value(table).toString();
      node.index = row.value(index).toString();
      node.rows = number(QJsonValue(row.value(rows).toString()));
      node.rowBytes = number(QJsonValue(row.value(bytes).toString()));
      node.totalCost = number(QJsonValue(row.value(cost).toString()));
      for (int col = 0; col < columns.size(); ++col)
        if (!row[col].isNull())
          node.properties.insert(columns[col], row[col].toString().left(2048));
      stack << add(std::move(node), stack.isEmpty() ? -1 : stack.back(), depth);
    }
  }
  void postgres(const QJsonObject &object, int parent, int depth) {
    EstimatedPlanNode node;
    node.operation = object["Node Type"].toString();
    if (node.operation.isEmpty())
      throw std::runtime_error("PostgreSQL 计划缺少 Node Type");
    const auto schema = object["Schema"].toString();
    node.object = object["Relation Name"].toString();
    if(!schema.isEmpty() && !node.object.isEmpty())node.object=schema+"."+node.object;
    node.index = object["Index Name"].toString();
    node.alias = object["Alias"].toString();
    node.rows = number(object["Plan Rows"]);
    node.startupCost = number(object["Startup Cost"]);
    node.totalCost = number(object["Total Cost"]);
    node.rowBytes = number(object["Plan Width"]);
    for (auto it = object.begin(); it != object.end(); ++it)
      if (it.key() != "Plans")
        node.properties.insert(it.key(), shortValue(it.value()));
    auto id = add(std::move(node), parent, depth);
    for (const auto &child : object["Plans"].toArray()) {
      if (!child.isObject())
        throw std::runtime_error("PostgreSQL Plans 格式不正确");
      postgres(child.toObject(), id, depth + 1);
    }
  }
  void mysql(const QJsonValue &value, int parent, int depth,
             const QString &label) {
    if (depth > limits.maxDepth)
      throw std::runtime_error("MySQL 计划层级超过限额");
    if (value.isArray()) {
      int id = parent;
      if (label == "nested_loop") {
        EstimatedPlanNode n;
        n.operation = "Nested Loop";
        id = add(n, parent, depth);
      }
      for (const auto &v : value.toArray())
        mysql(v, id, depth + 1, {});
      return;
    }
    if (!value.isObject())
      return;
    const auto object = value.toObject();
    int id = parent;
    const bool operation = !label.isEmpty() || object.contains("operation");
    if (operation) {
      EstimatedPlanNode node;
      node.operation = object["operation"].toString();
      if (node.operation.isEmpty())
        node.operation = label;
      if (label == "table")
        node.operation = "Table Access · " + object["access_type"].toString();
      node.object = object["table_name"].toString();
      node.index = object["key"].toString();
      if(node.index.isEmpty())node.index=object["index_name"].toString();
      node.alias=object["alias"].toString();
      node.rows = number(object["rows_produced_per_join"]);
      if (!node.rows)
        node.rows = number(object["rows_examined_per_scan"]);
      if (!node.rows)
        node.rows = number(object["rows"]);
      const auto costs = object["cost_info"].toObject();
      node.totalCost = number(costs["query_cost"]);
      if (!node.totalCost)
        node.totalCost = number(costs["prefix_cost"]);
      if (!node.totalCost)
        node.totalCost = number(object["estimated_total_cost"]);
      if(!node.totalCost)node.totalCost=number(object["cost"]);
      if (!node.rows)
        node.rows = number(object["estimated_rows"]);
      for (auto it = object.begin(); it != object.end(); ++it)
        if (!it.value().isObject() && !it.value().isArray())
          node.properties.insert(it.key(), shortValue(it.value()));
      for (auto it = costs.begin(); it != costs.end(); ++it)
        node.properties.insert("cost_info." + it.key(), shortValue(it.value()));
      id = add(std::move(node), parent, depth);
    }
    static const QSet<QString> childKeys = {"query_plan",
                                            "query_block",
                                            "nested_loop",
                                            "table",
                                            "ordering_operation",
                                            "grouping_operation",
                                            "duplicates_removal",
                                            "buffer_result",
                                            "materialized_from_subquery",
                                            "attached_subqueries",
                                            "optimized_away_subqueries",
                                            "union_result",
                                            "query_specifications",
                                            "windowing",
                                            "windows",
                                            "inputs",
                                            "subqueries", "input", "filesort", "temporary_table", "read_sorted_file",
                                            "expression_cache", "expression-cache", "block-nl-join", "window_functions_computation", "recursive_union"};
    for (auto it = object.begin(); it != object.end(); ++it)
      if (childKeys.contains(it.key()))
        mysql(it.value(), id, depth + 1, it.key());
  }
  QSet<qint64> oceanbaseIds;
  void oceanbase(const QJsonObject &object,int parent,int depth) {
    EstimatedPlanNode node;node.operation=object["OPERATOR"].toString().trimmed();
    if(node.operation.isEmpty())throw std::runtime_error("OceanBase 计划缺少 OPERATOR");
    if(object.contains("ID")) {
      bool valid=false;auto id=object["ID"].toVariant().toString().toLongLong(&valid);
      if(!valid || id<0 || oceanbaseIds.contains(id))throw std::runtime_error("OceanBase 计划 ID 无效或重复");
      oceanbaseIds.insert(id);
    }
    node.object=object["NAME"].toString();node.rows=number(object["EST.ROWS"]);node.totalCost=number(object["COST"]);
    node.estimatedTimeUs=number(object["EST.TIME(us)"]);
    QMap<unsigned,QJsonObject> children;
    for(auto it=object.begin();it!=object.end();++it) {
      if(it.key().startsWith("CHILD_")) {
        bool valid=false;auto order=it.key().mid(6).toUInt(&valid);
        if(!valid || !order || !it.value().isObject())throw std::runtime_error("OceanBase CHILD 节点格式无效");
        children.insert(order,it.value().toObject());
      } else node.properties.insert(it.key(),shortValue(it.value()));
    }
    auto id=add(std::move(node),parent,depth);
    for(const auto &child:children)oceanbase(child,id,depth+1);
  }
  void sqlserver(const QByteArray &raw) {
    QXmlStreamReader xml(raw);
    QVector<int> stack;
    QVector<QSet<qulonglong>> queryNodeIds;
    int depth = 0;
    std::size_t tokens = 0;
    bool root = false;
    while (!xml.atEnd()) {
      const auto token = xml.readNext();
      if (++tokens > 1000000)
        throw std::runtime_error("XML 计划标记超过限额");
      if (token == QXmlStreamReader::DTD ||
          token == QXmlStreamReader::EntityReference)
        throw std::runtime_error("执行计划 XML 不允许 DTD 或外部实体");
      if (token == QXmlStreamReader::StartElement) {
        if (++depth > limits.maxDepth * 4)
          throw std::runtime_error("XML 计划层级超过限额");
        const auto tag = xml.name().toString();
        const auto attributes = xml.attributes();
        if((tag=="RelOp" || tag=="QueryPlan" || tag=="Object") && xml.namespaceUri().toString()!="http://schemas.microsoft.com/sqlserver/2004/07/showplan")
          throw std::runtime_error("SQL Server 计划结构节点命名空间不正确");
        if (!root) {
          if (tag != "ShowPlanXML" ||
              xml.namespaceUri().toString() !=
                  "http://schemas.microsoft.com/sqlserver/2004/07/showplan")
            throw std::runtime_error(
                "SQL Server ShowPlanXML 根节点或命名空间不正确");
          root = true;
        }
        if (tag == "QueryPlan")
          queryNodeIds.append(QSet<qulonglong>{});
        if (tag == "RelOp") {
          if (attributes.hasAttribute("NodeId")) {
            bool valid = false;
            auto id = attributes.value("NodeId").toString().toULongLong(&valid);
            if (!valid)
              throw std::runtime_error("SQL Server 计划 NodeId 无效");
            if (queryNodeIds.isEmpty())
              queryNodeIds.append(QSet<qulonglong>{});
            if (queryNodeIds.back().contains(id))
              throw std::runtime_error(
                  "SQL Server 同一 QueryPlan 中的 NodeId 重复");
            queryNodeIds.back().insert(id);
          }
          EstimatedPlanNode node;
          node.operation = attributes.value("PhysicalOp").toString();
          if (node.operation.isEmpty())
            node.operation = attributes.value("LogicalOp").toString();
          if (node.operation.isEmpty())
            throw std::runtime_error("SQL Server 计划算子名称缺失");
          node.rows = number(attributes.value("EstimateRows").toString());
          node.totalCost =
              number(attributes.value("EstimatedTotalSubtreeCost").toString());
          node.rowBytes = number(attributes.value("AvgRowSize").toString());
          for (const auto &a : attributes)
            node.properties.insert(a.name().toString(),
                                   a.value().toString().left(2048));
          stack.append(add(std::move(node), stack.isEmpty() ? -1 : stack.back(),
                           int(stack.size())));
        } else if (!stack.isEmpty()) {
          auto &node = plan.nodes[stack.back()];
          if (tag == "Object") {
            node.object = attributes.value("Schema").toString() + "." +
                          attributes.value("Table").toString();
            node.index = attributes.value("Index").toString();
            node.alias = attributes.value("Alias").toString();
          }
          if (tag == "ScalarOperator" &&
              attributes.hasAttribute("ScalarString") &&
              node.properties.size() < 128)
            node.properties.insert(
                "ScalarString " + QString::number(node.properties.size()),
                attributes.value("ScalarString").toString().left(2048));
          if (tag == "Warnings" || tag == "MissingIndex" ||
              tag == "PlanAffectingConvert")
            node.properties.insert(tag, "服务端计划包含此项，请检查原始 XML");
        }
      } else if (token == QXmlStreamReader::EndElement) {
        if (xml.name() == QLatin1String("RelOp") && !stack.isEmpty())
          stack.removeLast();
        if (xml.name() == QLatin1String("QueryPlan") && !queryNodeIds.isEmpty())
          queryNodeIds.removeLast();
        --depth;
      }
    }
    if (xml.hasError() || !root || plan.nodes.isEmpty())
      throw std::runtime_error("无法解析完整的 SQL Server 执行计划 XML");
  }
  void tidb(const QueryResult &result) {
    const auto idColumn = result.columns.indexOf("id");
    const auto rowsColumn = result.columns.indexOf("estRows");
    const auto objectColumn = result.columns.indexOf("access object");
    if (idColumn < 0 || rowsColumn < 0 || objectColumn < 0)
      throw std::runtime_error("TiDB 执行计划缺少 id、estRows 或 access object");
    const QChar branch{0x251c}, lastBranch{0x2514}, vertical{0x2502}, horizontal{0x2500};
    QVector<int> path;
    for (const auto &row : result.rows) {
      if (row.size() != result.columns.size())
        throw std::runtime_error("TiDB 计划行字段数量与表头不一致");
      const auto label = row[idColumn].toString();
      int offset = 0, depth = 0;
      while (offset + 1 < label.size() &&
             (label[offset] == QLatin1Char(' ') || label[offset] == vertical) &&
             label[offset + 1] == QLatin1Char(' ')) {
        offset += 2;
        ++depth;
      }
      if (offset + 1 < label.size() && (label[offset] == branch || label[offset] == lastBranch) &&
          label[offset + 1] == horizontal) {
        offset += 2;
        ++depth;
      } else if (offset != 0) {
        throw std::runtime_error("TiDB 执行计划缩进缺少分支符号");
      }
      if (depth > path.size() || label.mid(offset).trimmed().isEmpty())
        throw std::runtime_error("TiDB 执行计划缺少算子或父节点");
      path.resize(depth);
      EstimatedPlanNode node;
      node.operation = label.mid(offset).trimmed();
      node.object = row[objectColumn].toString();
      node.rows = number(row[rowsColumn].toString());
      for (int i = 0; i < result.columns.size(); ++i)
        node.properties.insert(result.columns[i], row[i].toString().left(2048));
      path.append(add(std::move(node), path.isEmpty() ? -1 : path.back(), depth));
    }
  }
  void rows(const QueryResult &result, bool oracle) {
    auto column = [&](const QString &name) {
      for (int i = 0; i < result.columns.size(); ++i)
        if (result.columns[i].compare(name, Qt::CaseInsensitive) == 0)
          return i;
      return -1;
    };
    const auto idColumn = column("id"),
               parentColumn = column(oracle ? "parent_id" : "parent"),
               opColumn = column(oracle ? "operation" : "detail");
    if (idColumn < 0 || parentColumn < 0 || opColumn < 0)
      throw std::runtime_error("计划行缺少 ID / Parent / Operation 字段");
    QMap<qint64, int> index;
    QVector<std::optional<qint64>> parents;
    for (const auto &row : result.rows) {
      auto cell = [&](int col) -> QVariant {
        return col >= 0 && col < row.size() ? row[col] : QVariant{};
      };
      bool valid = false;
      const auto serverId = cell(idColumn).toLongLong(&valid);
      if (!valid || index.contains(serverId))
        throw std::runtime_error("执行计划节点 ID 无效或重复");
      EstimatedPlanNode node;
      node.operation = cell(opColumn).toString();
      if (oracle) {
        const auto options = cell(column("options")).toString();
        if (!options.isEmpty())
          node.operation += " · " + options;
        const auto owner = cell(column("object_owner")).toString();
        node.object = (owner.isEmpty() ? "" : owner + ".") +
                      cell(column("object_name")).toString();
        node.rows = number(cell(column("cardinality")).toString());
        node.totalCost = number(cell(column("cost")).toString());
        node.rowBytes = number(cell(column("bytes")).toString());
        if (node.rows && *node.rows > 0 && node.rowBytes)
          node.rowBytes = *node.rowBytes / *node.rows;
      }
      for (int c = 0; c < result.columns.size(); ++c)
        node.properties.insert(result.columns[c],
                               cell(c).toString().left(2048));
      auto id = add(std::move(node), -1, 0);
      index.insert(serverId, id);
      auto parent = cell(parentColumn);
      if (parent.isNull())
        parents.append(std::optional<qint64>{});
      else {
        auto p = parent.toLongLong(&valid);
        if (!valid)
          throw std::runtime_error("执行计划父节点 ID 无效");
        if (p == serverId && (oracle || serverId != 0))
          throw std::runtime_error("执行计划父节点形成自身循环");
        parents.append(p == serverId ? std::optional<qint64>{}
                                     : std::optional<qint64>{p});
      }
    }
    plan.roots.clear();
    for (int i = 0; i < parents.size(); ++i) {
      if (parents[i] && index.contains(*parents[i]))
        plan.nodes[index[*parents[i]]].children.append(i);
      else {
        if (parents[i] && (oracle || *parents[i] > 0))
          throw std::runtime_error(
              "执行计划父节点缺失，未将不完整的子树显示成完整计划");
        plan.roots.append(i);
      }
    }
    QVector<int> state(plan.nodes.size(), 0);
    struct Work {
      int id, depth;
      bool exit;
    };
    for (int start = 0; start < plan.nodes.size(); ++start) {
      if (state[start])
        continue;
      QVector<Work> work{{start, 0, false}};
      while (!work.isEmpty()) {
        auto w = work.takeLast();
        if (w.exit) {
          state[w.id] = 2;
          continue;
        }
        if (state[w.id] == 1)
          throw std::runtime_error("执行计划存在父子循环");
        if (state[w.id] == 2)
          continue;
        if (w.depth > limits.maxDepth)
          throw std::runtime_error("执行计划层级超过限额");
        state[w.id] = 1;
        work.append({w.id, w.depth, true});
        for (auto child : plan.nodes[w.id].children)
          work.append({child, w.depth + 1, false});
      }
    }
  }
};
} // namespace
EstimatedPlan decodeEstimatedPlan(DatabaseKind kind, const QueryResult &result,
                                  PlanParseLimits limits) {
  EstimatedPlan plan;
  plan.driver = kind;
  plan.format=isOceanBase(kind)?"oceanbase-json":kind==DatabaseKind::Oracle?"oracle-plan-table":kind==DatabaseKind::SQLite?"sqlite-plan-rows":
    kind==DatabaseKind::SQLServer?"sqlserver-showplan-xml":usesNativePostgres(kind)?"postgresql-json":
    kind==DatabaseKind::TiDB?"tidb-plan-rows":kind==DatabaseKind::Dameng?"dameng-plan-rows":isMySqlFamily(kind)?"mysql-json":QString{};
  limits.maxBytes = std::clamp(limits.maxBytes, 1024, 16 * 1024 * 1024);
  limits.maxNodes = std::clamp(limits.maxNodes, 1, 50000);
  limits.maxDepth = std::clamp(limits.maxDepth, 1, 128);
  try {
    if (!result.error.isEmpty() || result.cancelled)
      throw std::runtime_error("未取得执行计划");
    // Preserve a bounded original response even when decoding later fails.
    // A truncated response is inspectable, but never marked as a complete plan.
    if(kind==DatabaseKind::SQLite || kind==DatabaseKind::Oracle || kind==DatabaseKind::TiDB || kind==DatabaseKind::Dameng) {
      QJsonArray rows;
      qint64 used=0;
      for(const auto &row:result.rows) {
        QJsonArray values;
        for(const auto &cell:row) {
          const auto text=cell.toString();used+=text.size()*2ll;
          if(used>limits.maxBytes)throw std::runtime_error("执行计划原始行数据超过限额");
          values.append(cell.isNull()?QJsonValue(QJsonValue::Null):QJsonValue(text));
        }
        rows.append(values);
        if(rows.size()>limits.maxNodes)throw std::runtime_error("执行计划原始行数超过限额");
      }
      plan.raw=QJsonDocument(QJsonObject{{"columns",QJsonArray::fromStringList(result.columns)},{"rows",rows}}).toJson(QJsonDocument::Compact);
      if(plan.raw.size()>limits.maxBytes)throw std::runtime_error("执行计划原始行文档超过限额");
    } else if(result.rows.size()==1 && !result.rows[0].isEmpty()) {
      if(result.rows[0][0].toString().size()<=limits.maxBytes)plan.raw=result.rows[0][0].toString().toUtf8();
      if(plan.raw.size()>limits.maxBytes)plan.raw.clear();
    }
    if (result.truncated || result.cellTruncated)
      throw std::runtime_error(
          "执行计划原始结果被截断，未将不完整内容显示成完整计划");
    if (result.rows.size() > limits.maxNodes)
      throw std::runtime_error("执行计划结果行超过限额");
    qint64 bytes = 0;
    for (const auto &row : result.rows)
      for (const auto &cell : row) {
        bytes += cell.toString().size() * 2ll;
        if (bytes > limits.maxBytes * 2ll)
          throw std::runtime_error("执行计划原始数据超过字节限额");
      }
    Decoder decoder{plan, limits, {}};
    if (kind == DatabaseKind::Dameng) {
      decoder.dameng(result);
    } else if (kind == DatabaseKind::TiDB) {
      decoder.tidb(result);
    } else if (usesNativePostgres(kind) || isMySqlFamily(kind) || isOceanBase(kind) ||
               kind == DatabaseKind::SQLServer) {
      if (result.rows.size() != 1 || result.rows[0].isEmpty())
        throw std::runtime_error("执行计划不符合单个 JSON / XML 文档格式");
      plan.raw = result.rows[0][0].toString().toUtf8();
      if (plan.raw.size() > limits.maxBytes)
        throw std::runtime_error("执行计划文档超过字节限额");
      if (kind == DatabaseKind::SQLServer) {
        plan.format = "sqlserver-showplan-xml";
        decoder.sqlserver(plan.raw);
      } else {
        QJsonParseError error;
        auto document = QJsonDocument::fromJson(plan.raw, &error);
        if (error.error != QJsonParseError::NoError)
          throw std::runtime_error("执行计划 JSON 格式无效；请检查原始结果");
        if (usesNativePostgres(kind)) {
          plan.format = "postgresql-json";
          if (!document.isArray() || document.array().size() != 1 ||
              !document.array()[0].toObject()["Plan"].isObject())
            throw std::runtime_error("PostgreSQL 计划缺少根 Plan");
          decoder.postgres(document.array()[0].toObject()["Plan"].toObject(),
                           -1, 0);
        } else {
          plan.format = "mysql-json";
          if (!document.isObject())
            throw std::runtime_error("MySQL 计划不是 JSON 对象");
          if(isOceanBase(kind)) {
            plan.format="oceanbase-json";decoder.oceanbase(document.object(),-1,0);
          } else {
            plan.format=kind==DatabaseKind::MariaDB?"mariadb-json":"mysql-json";
            decoder.mysql(document.object(), -1, 0, {});
          }
        }
      }
    } else if (kind == DatabaseKind::SQLite || kind==DatabaseKind::Oracle) {
      plan.format = kind == DatabaseKind::SQLite ? "sqlite-plan-rows"
                                                 : "oracle-plan-table";
      decoder.rows(result, kind == DatabaseKind::Oracle);
    } else
      throw std::runtime_error("此数据库暂不支持结构化计划显示");
    if (plan.nodes.isEmpty())
      throw std::runtime_error("原始计划没有可识别的算子；请查看原始结果");
    plan.complete = true;
  } catch (const std::exception &e) {
    plan.complete = false;
    plan.nodes.clear();
    plan.roots.clear();
    plan.warning = QString::fromUtf8(e.what());
    if (plan.raw.size() > limits.maxBytes)
      plan.raw.clear();
  }
  return plan;
}
QByteArray estimatedPlanJson(const EstimatedPlan &plan, int maxBytes) {
  QJsonObject root{
      {"version", 1},
      {"driver", databaseKindKey(plan.driver)},
      {"format", plan.format},
      {"complete", plan.complete},
      {"warning", plan.warning},
      {"measurement",
       "server estimates; not actual rows/timing; COST is database-specific"}};
  const auto &request=plan.request;
  root.insert("request",QJsonObject{{"issued",request.issued},{"received",request.received},{"isolatedSession",request.isolated},
    {"connection",request.connectionName},{"database",request.database},{"schema",request.schema},{"serverVersion",request.serverVersion},
    {"requestedAtUtc",request.requestedAtUtc},{"elapsedMs",double(request.elapsedMs)},{"commands",QJsonArray::fromStringList(request.commands)}});
  QJsonArray nodes, roots;
  for (auto id : plan.roots)
    roots.append(id);
  for (int i = 0; i < plan.nodes.size(); ++i) {
    const auto &n = plan.nodes[i];
    QJsonObject object{{"id", i},
                       {"operation", n.operation},
                       {"object", n.object},
                       {"index", n.index},
                       {"alias", n.alias}};
    if (n.rows)
      object.insert("estimatedRows", *n.rows);
    if (n.totalCost)
      object.insert("totalCost", *n.totalCost);
    if (n.startupCost)
      object.insert("startupCost", *n.startupCost);
    if (n.rowBytes)
      object.insert("averageRowBytes", *n.rowBytes);
    if(n.estimatedTimeUs)object.insert("estimatedTimeUs",*n.estimatedTimeUs);
    QJsonObject properties;
    for (auto it = n.properties.begin(); it != n.properties.end(); ++it)
      properties.insert(it.key(), it.value());
    object.insert("properties", properties);
    QJsonArray children;
    for (auto id : n.children)
      children.append(id);
    object.insert("children", children);
    nodes.append(object);
  }
  root.insert("roots", roots);
  root.insert("nodes", nodes);
  auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (bytes.size() > maxBytes)
    throw std::runtime_error("执行计划报告超过导出字节限额");
  return bytes;
}
} // namespace navidog
