#include "navidog/er_model.hpp"
#include "navidog/sql.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "table_design_internal.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace navidog::er {
namespace {
using K = DatabaseKind;
[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

QString owner(const CatalogNode& node, DatabaseKind kind) {
    return isMySqlFamily(kind) ? node.database : node.schema;
}
QString fold(QString value, DatabaseKind kind) {
    return kind == K::SQLite || kind == K::SQLServer ? value.toCaseFolded() : value;
}

CatalogNode target(const CatalogNode& origin, const TableObject& object, DatabaseKind kind) {
    auto n = origin;
    n.object = object.referenceTable;
    if (!object.referenceSchema.isEmpty()) {
        if (isMySqlFamily(kind)) n.database = object.referenceSchema;
        else n.schema = object.referenceSchema;
    }
    return n;
}

bool isForeign(const TableObject& object) { return object.kind == TableObjectKind::ForeignKey; }
bool isView(const TableDefinition& table) { return table.node.kind == "view"; }

TableDefinition objectsOf(TableDefinition definition, bool foreign) {
    auto& objects = definition.objects;
    objects.erase(std::remove_if(objects.begin(), objects.end(), [foreign](const auto& item) {
        return isForeign(item) != foreign;
    }), objects.end());
    return definition;
}

sql::Dialect lexerDialect(K kind) {
    if (isMySqlFamily(kind)) return sql::Dialect::MySQL;
    switch (kind) {
    case K::PostgreSQL: return sql::Dialect::PostgreSQL;
    case K::Oracle: return sql::Dialect::Oracle;
    case K::SQLServer: return sql::Dialect::SQLServer;
    default: return sql::Dialect::SQLite;
    }
}

QString viewSql(const ConnectionProfile& profile, const TableDefinition& table, bool replace) {
    const auto query = viewBody(table.rawSql, profile.driver);
    const auto statement = "VIEW " + tableQualifiedName(profile, table.node) + " AS\n" + query;
    if (!replace) return "CREATE " + statement;
    if (profile.driver == K::SQLServer) return "ALTER " + statement;
    return "CREATE OR REPLACE " + statement;
}

QJsonObject tableJson(const TableDefinition& table) {
    auto object = tableDefinitionJson(table);
    object["node"] = QJsonObject{{"kind",table.node.kind},{"name",table.node.object},
                                {"database",table.node.database},{"schema",table.node.schema}};
    return object;
}

TableDefinition readDefinition(const QJsonObject& object, bool exists) {
    auto table = tableDefinitionFromJson(object);
    const auto node = object["node"].toObject();
    table.node.name = table.node.object = node["name"].toString();
    table.node.schema = node["schema"].toString();
    table.node.database = node["database"].toString();
    table.node.kind = node["kind"].toString("table");
    table.exists = exists;
    if (table.node.object.isEmpty() || (table.node.kind != "table" && table.node.kind != "view"))
        fail("模型中的数据库对象无效");
    if (table.columns.size() > 8192 || table.objects.size() > 8192)
        fail("单个模型对象的字段或约束超过上限");
    for (const auto& o : table.objects)
        if (int(o.kind) < 0 || int(o.kind) > int(TableObjectKind::Trigger))
            fail("模型中存在未知的表约束类型");
    return table;
}

bool same(const TableDefinition& a, const TableDefinition& b, K kind) {
    return nameKey(a.node,kind) == nameKey(b.node,kind) &&
           a.node.object == b.node.object && sameTableStructure(a,b);
}

QString renameTable(const ConnectionProfile& profile, const TableDefinition& before,
                    const TableDefinition& after) {
    const auto oldTable = tableQualifiedName(profile,before.node);
    const auto newName = quoteIdentifier(after.node.object,profile.driver);
    if (profile.driver == K::SQLServer)
        return "EXEC sys.sp_rename @objname=" + design::ddlLiteral(oldTable,profile.driver) +
               ", @newname=" + design::ddlLiteral(after.node.object,profile.driver);
    return "ALTER TABLE " + oldTable + " RENAME TO " + newName;
}

QString typeFamily(QString type, K kind) {
    type = type.simplified().toLower();
    if (kind == K::SQLite) {
        if (type.contains("int")) return "integer";
        if (type.contains("char") || type.contains("clob") || type.contains("text")) return "text";
        if (type.isEmpty() || type.contains("blob")) return "blob";
        if (type.contains("real") || type.contains("floa") || type.contains("doub")) return "real";
        return "numeric";
    }
    const auto modifier = type.indexOf('(');
    if (modifier >= 0) type = type.left(modifier).trimmed() + (type.contains(" unsigned") ? " unsigned" : "");
    if (type == "int" || type == "int4" || type == "serial") return "integer";
    if (type == "int8" || type == "bigserial") return "bigint";
    if (type == "int2" || type == "smallserial") return "smallint";
    if (type == "decimal") return "numeric";
    return type;
}

const TableColumn* column(const TableDefinition& table, const QString& name, K kind) {
    const auto it = std::find_if(table.columns.cbegin(), table.columns.cend(), [&](const auto& value) {
        return fold(value.name,kind) == fold(name,kind);
    });
    return it == table.columns.cend() ? nullptr : &*it;
}

QStringList primaryKey(const TableDefinition& table) {
    QVector<std::pair<int, QString>> ordered;
    for (const auto& col : table.columns)
        if (col.primaryKey > 0) ordered.push_back({col.primaryKey,col.name});
    std::sort(ordered.begin(),ordered.end());
    QStringList names;
    for (const auto& entry : ordered) names << entry.second;
    return names;
}

bool hasUniqueKey(const TableDefinition& table, const QStringList& names, K kind) {
    const auto matches = [&](const QStringList& columns) {
        if (columns.size() != names.size()) return false;
        for (int i = 0; i < names.size(); ++i)
            if (fold(columns[i],kind) != fold(names[i],kind)) return false;
        return true;
    };
    if (matches(primaryKey(table))) return true;
    return std::any_of(table.objects.cbegin(),table.objects.cend(),[&](const auto& object) {
        return object.kind == TableObjectKind::Index && object.unique && object.enabled &&
               object.predicate.isEmpty() && matches(object.columns);
    });
}

struct ViewStatement { int entity; QString sql, body; };
QStringList orderViews(const Project& project,const Graph& graph,const QVector<ViewStatement>& views,
                       const Cancellation& cancel) {
    namespace sc = navidog::sql::compiler;
    const auto dialect = sc::parseDialect(databaseKindKey(project.driver).toStdString()).value_or(sc::Dialect::SQLite);
    QHash<int,int> positions;
    for (int i = 0; i < views.size(); ++i) positions.insert(views[i].entity,i);
    QVector<int> pending(views.size(),0), queue;
    QVector<QVector<int>> dependsOn(views.size());
    for (int i = 0; i < views.size(); ++i) {
        if (cancel && cancel->load()) fail("生成 SQL 已停止");
        const auto& node = project.entities[views[i].entity].table.node;
        const auto analysis = sc::compile(dialect,views[i].body.toStdString());
        if (!analysis->syntaxValid || analysis->model.empty()) {
            queue.push_back(i);
            continue;
        }
        QSet<int> sources;
        for (const auto& query : analysis->model.queries) for (const auto& table : query.tables) {
            if (table.query || table.pseudo || table.name.empty() || table.name.size()>3) continue;
            auto name = node;
            name.object = QString::fromStdString(table.name.back());
            if (table.name.size()>1) {
                const auto ns = QString::fromStdString(table.name[table.name.size()-2]);
                if (isMySqlFamily(project.driver)) name.database = ns;
                else name.schema = ns;
            }
            const int found = positions.value(graph.byName.value(nameKey(name,project.driver),-1),-1);
            if (found >= 0) sources.insert(found);
        }
        pending[i] = sources.size();
        if (sources.isEmpty()) queue.push_back(i);
        for (int source : sources) dependsOn[source].push_back(i);
    }
    QStringList statements;
    for (int i = 0; i < queue.size(); ++i) {
        statements << views[queue[i]].sql;
        for (int dependent : dependsOn[queue[i]])
            if (--pending[dependent] == 0) queue.push_back(dependent);
    }
    if (statements.size() != views.size()) fail("视图定义之间存在循环依赖");
    return statements;
}
}

EntityId Project::add(TableDefinition definition, bool imported) {
    if (entities.size() >= maxEntities || nextId == 0) fail("模型表数量超过上限");
    Entity entity;
    entity.id = nextId++;
    entity.table = std::move(definition);
    entity.table.exists = imported;
    if (imported) entity.baseline = entity.table;
    entities.push_back(std::move(entity));
    return entities.back().id;
}

QString nameKey(const CatalogNode& node, K kind) {
    const auto ns = fold(owner(node,kind),kind);
    return QString::number(ns.size()) + ':' + ns + fold(node.object,kind);
}

QString displayName(const CatalogNode& node, K kind) {
    const auto ns = owner(node,kind);
    return ns.isEmpty() ? node.object : ns + '.' + node.object;
}

QString groupName(const Project& project, const Entity& entity) {
    const auto& node = entity.table.node;
    if (project.grouping == GroupKind::Custom)
        return entity.group.isEmpty() ? QString("未分组") : entity.group;
    const auto schema = owner(node,project.driver);
    if (project.grouping == GroupKind::Schema)
        return schema.isEmpty() ? QString("默认 Schema") : schema;
    const int sep = node.object.indexOf('_');
    return sep > 0 ? node.object.left(sep).toCaseFolded()
                   : schema.isEmpty() ? QString("其他") : schema;
}

Graph makeGraph(const Project& project) {
    Graph graph;
    graph.active.reserve(project.entities.size());
    graph.byId.reserve(project.entities.size());
    graph.byName.reserve(project.entities.size());
    graph.incidents.resize(project.entities.size());
    for (int i = 0; i < project.entities.size(); ++i) {
        const auto& e = project.entities[i];
        graph.byId.insert(e.id,i);
        if (e.removed) continue;
        graph.active.push_back(i);
        graph.byName.insert(nameKey(e.table.node,project.driver),i);
    }
    for (int i : graph.active) {
        const auto& table = project.entities[i].table;
        for (int j = 0; j < table.objects.size(); ++j) {
            const auto& object = table.objects[j];
            if (!isForeign(object)) continue;
            const int other = graph.byName.value(nameKey(target(table.node,object,project.driver),project.driver),-1);
            if (other < 0) continue;
            const int edge = graph.edges.size();
            graph.edges.push_back({i,other,j,hasUniqueKey(table,object.columns,project.driver)});
            graph.incidents[i].push_back(edge);
            if (i != other) graph.incidents[other].push_back(edge);
        }
    }
    return graph;
}

void replaceEntity(Project& project, int index, TableDefinition next, QHash<QString,QString> renamed) {
    const auto& prior = std::as_const(project.entities)[index].table;
    const auto key = nameKey(prior.node,project.driver);
    for (const auto& c : next.columns) {
        if (c.originalName.isEmpty()) continue;
        for (const auto& old : prior.columns)
            if (old.originalName == c.originalName && old.name != c.name && !renamed.contains(old.name))
                renamed.insert(old.name,c.name);
    }
    const bool changedName = prior.node.object != next.node.object ||
                             owner(prior.node,project.driver) != owner(next.node,project.driver);
    if (changedName || !renamed.isEmpty()) {
        QMap<QString,QString> ordered;
        for(auto it=renamed.cbegin();it!=renamed.cend();++it)ordered.insert(it.key(),it.value());
        for (auto& o : next.objects) {
            bool changed = false;
            for (auto& n : o.columns) if (renamed.contains(n)) { n = renamed.value(n); changed = true; }
            if (o.kind == TableObjectKind::Index || o.kind == TableObjectKind::Check) {
                if (!o.expression.isEmpty()) {
                    const auto expression = design::renameIdentifiers(o.expression,ordered,project.driver);
                    changed |= expression != o.expression;
                    o.expression = expression;
                }
            }
            if (changed && o.kind != TableObjectKind::Trigger) o.sql.clear();
        }
        const auto clean = [&](TableDefinition& t) {
            for (auto& object : t.objects) {
                if (!isForeign(object) || nameKey(target(t.node,object,project.driver),project.driver) != key)
                    continue;
                object.referenceTable = next.node.object;
                object.referenceSchema = owner(next.node,project.driver);
                for (auto& name : object.referenceColumns) name = renamed.value(name,name);
                object.sql.clear();
            }
        };
        for (int i = 0; i < project.entities.size(); ++i)
            if (i != index && !project.entities[i].removed) clean(project.entities[i].table);
        clean(next);
    }
    project.entities[index].table = std::move(next);
}

QStringList validate(const Project& project, const Graph& graph, int limit) {
    QStringList issues;
    QSet<QString> names;
    const auto kind = project.driver;
    const auto issue = [&](const QString& name, const QString& detail) {
        if (issues.size() < limit) issues << name + " · " + detail;
    };
    for (int i : graph.active) {
        const auto& entity = project.entities[i];
        const auto& t = entity.table;
        const auto name = displayName(t.node,kind);
        const auto key = nameKey(t.node,kind);
        if (t.node.object.trimmed().isEmpty() || t.node.object.contains(QChar::Null))
            issue(name,"对象名为空或包含空字符");
        if (names.contains(key)) issue(name,"对象名重复");
        names.insert(key);
        if (isView(t)) {
            if (!entity.baseline || t.rawSql != entity.baseline->rawSql)
                try { viewBody(t.rawSql,kind); } catch (const std::exception& e) { issue(name,QString::fromUtf8(e.what())); }
            continue;
        }
        QSet<QString> columns, objects;
        QSet<int> keyPositions;
        if (t.columns.isEmpty()) issue(name,"至少需要一个字段");
        for (const auto& c : t.columns) {
            const auto cn = fold(c.name,kind);
            if (cn.trimmed().isEmpty() || c.type.trimmed().isEmpty()) issue(name,"字段名和类型必须填写");
            if (columns.contains(cn)) issue(name,"字段名重复 " + c.name);
            columns.insert(cn);
            if (c.primaryKey) {
                if (keyPositions.contains(c.primaryKey)) issue(name,"主键字段顺序重复");
                keyPositions.insert(c.primaryKey);
            }
        }
        for (const auto& o : t.objects) {
            const auto on = fold(o.name,kind);
            const auto category = o.kind == TableObjectKind::Index && !o.constraint ? "index:" :
                o.kind == TableObjectKind::Trigger ? "trigger:" : "constraint:";
            if (on.isEmpty() || objects.contains(category+on)) issue(name,"约束或索引名称无效 " + o.name);
            objects.insert(category+on);
            if (!isForeign(o) || !o.enabled) continue;
            bool unchanged = false;
            if (entity.baseline)
                unchanged = std::any_of(entity.baseline->objects.cbegin(),entity.baseline->objects.cend(),
                                        [&](const auto& previous){ return o == previous; });
            if (o.columns.isEmpty() || o.columns.size() != o.referenceColumns.size()) {
                if (!unchanged) issue(name,"外键字段数量不对应 " + o.name);
                continue;
            }
            const int referred = graph.byName.value(nameKey(target(t.node,o,kind),kind),-1);
            if (referred < 0) {
                if (!unchanged) issue(name,"外键目标未在模型中 " + o.referenceTable);
                continue;
            }
            const auto& other = project.entities[referred].table;
            if (isView(other)) issue(name,"外键目标必须是表 " + o.referenceTable);
            if (!unchanged && other.detailsLoaded && !hasUniqueKey(other,o.referenceColumns,kind))
                issue(name,"引用列需要对应主键或唯一键 " + o.name);
            for (int k = 0; k < o.columns.size(); ++k) {
                auto* local = column(t,o.columns[k],kind);
                auto* remote = column(other,o.referenceColumns[k],kind);
                if (!local || (!remote && !unchanged)) issue(name,"外键引用了不存在的字段 " + o.name);
                if (local && remote && !unchanged && typeFamily(local->type,kind) != typeFamily(remote->type,kind) &&
                    kind != K::SQLite)
                    issue(name,"外键字段类型不一致 " + o.name);
                if (local && !unchanged && !local->nullable &&
                    (o.onDelete == "SET NULL" || o.onUpdate == "SET NULL"))
                    issue(name,"SET NULL 需要可空字段 " + o.name);
            }
        }
        if (issues.size() >= limit) break;
    }
    return issues;
}

QString viewBody(const QString& source, K kind) {
    const auto text = source.trimmed();
    const auto dialect = lexerDialect(kind);
    auto bytes = text.toUtf8();
    auto parsed = sql::lex(std::string_view(bytes.constData(),bytes.size()),dialect);
    if (!parsed.diagnostics.empty()) fail("视图查询包含未闭合的引号或注释");
    int depth = 0, begin = -1, end = bytes.size();
    bool skipAs = false;
    for (const auto& t : parsed.tokens) {
        if (t.kind == sql::TokenKind::Comment) continue;
        const auto token = bytes.mid(t.begin,t.end-t.begin);
        if (begin < 0 && !skipAs && (token.toUpper() == "CREATE" || token.toUpper() == "ALTER")) skipAs = true;
        if (skipAs) {
            if (token == "(") ++depth;
            if (token == ")") --depth;
            if (!depth && token.toUpper() == "AS") skipAs = false;
            continue;
        }
        if (begin < 0) {
            const auto upper = token.toUpper();
            if (upper != "SELECT" && upper != "WITH" && upper != "VALUES" && upper != "(")
                fail("视图需要一条查询");
            begin = t.begin;
        }
        if (token == ";") {
            if (end != bytes.size()) fail("视图只能包含一条查询");
            end = t.begin;
        } else if (end != bytes.size()) fail("视图只能包含一条查询");
    }
    if (begin < 0 || begin >= end) fail("请填写视图查询");
    return QString::fromUtf8(bytes.mid(begin,end-begin)).trimmed();
}

QStringList ddl(const Project& project, bool changesOnly,const Cancellation& cancel) {
    const auto graph = makeGraph(project);
    const auto issues = validate(project,graph);
    if (!issues.isEmpty()) fail(issues.join('\n'));
    const auto kind = project.driver;
    ConnectionProfile profile;
    profile.driver = kind;
    profile.version = project.version;
    profile.database = project.database;
    const bool sqlite = kind == K::SQLite;
    QStringList detach, renames, removals, tables, references;
    QVector<ViewStatement> views;
    bool rebuild = false;
    for (int index = 0; index < project.entities.size(); ++index) {
        const auto& entity = project.entities[index];
        if (cancel && cancel->load()) fail("生成 SQL 已停止");
        const auto& current = entity.table;
        auto original = changesOnly && entity.baseline ? *entity.baseline : TableDefinition{};
        const bool exists = original.exists;
        if (entity.removed) {
            if (!exists) continue;
            if (!sqlite && !isView(original)) detach << tableObjectDrops(profile,objectsOf(original,true),{});
            (isView(original) ? detach : removals) <<
                QString(isView(original) ? "DROP VIEW " : "DROP TABLE ") + tableQualifiedName(profile,original.node);
            continue;
        }
        if (exists && same(original,current,kind)) continue;
        if (isView(current)) {
            const bool recreate = exists && (sqlite || nameKey(original.node,kind) != nameKey(current.node,kind));
            if (recreate)
                detach << "DROP VIEW " + tableQualifiedName(profile,original.node);
            views << ViewStatement{index,viewSql(profile,current,exists && !recreate),viewBody(current.rawSql,kind)};
            continue;
        }
        auto after = current;
        if (project.version == DatabaseVersion::MySQL5_7) {
            for (const auto& o : after.objects) if (o.kind == TableObjectKind::Check &&
                std::none_of(original.objects.cbegin(),original.objects.cend(),[&](const auto& prior){return prior==o;}))
                fail("MySQL 5.7 无法执行 CHECK 约束，请选择可强制执行该约束的目标版本");
        }
        if (sqlite) {
            for (auto* definition : {&original,&after})
                for (auto& o : definition->objects)
                    if (isForeign(o)) {
                        if (!o.referenceSchema.isEmpty() && o.referenceSchema != after.node.schema)
                            fail("SQLite 外键不能跨 Schema");
                        o.referenceSchema.clear();
                    }
        }
        if (!exists) {
            // Preserve the draft's identities but treat all components as creations for this script.
            for (auto& col : after.columns) col.originalName.clear();
            for (auto& obj : after.objects) obj.originalName.clear();
        }
        if (exists && owner(original.node,kind) != owner(after.node,kind))
            fail("跨 Schema 移动既有表需要单独的目标方言迁移策略");
        if (!sqlite) {
            auto oldRefs = objectsOf(original,true);
            auto newRefs = objectsOf(after,true);
            detach << tableObjectDrops(profile,oldRefs,newRefs);
            oldRefs.node = after.node;
            oldRefs.exists = true;
            references << tableObjectCreates(profile,oldRefs,newRefs);
            original = objectsOf(std::move(original),false);
            after = objectsOf(std::move(after),false);
        }
        if (sqlite && exists && requiresSqliteRebuild(original,after)) {
            if (!original.detailsLoaded) fail("SQLite 重建依赖完整表定义，请先读取该表完整结构");
            rebuild = true;
        }
        if (exists && original.node.object != after.node.object) {
            renames << renameTable(profile,original,after);
            original.node = after.node;
        }
        tables << tableChangeSql(profile,original,after);
    }
    auto statements = detach + renames + removals + tables + references + orderViews(project,graph,views,cancel);
    if (rebuild) {
        statements.prepend("BEGIN IMMEDIATE");
        statements.prepend("PRAGMA legacy_alter_table=ON");
        statements.prepend("PRAGMA foreign_keys=OFF");
        statements << "PRAGMA foreign_key_check" << "COMMIT" << "PRAGMA legacy_alter_table=OFF" << "PRAGMA foreign_keys=ON";
    }
    return statements;
}

void save(const Project& project, const QString& path) {
    QJsonArray entities;
    for (const auto& entity : project.entities) {
        if (entity.removed && !entity.baseline) continue;
        QJsonObject row{{"id",int(entity.id)},{"table",tableJson(entity.table)},
                        {"group",entity.group},{"removed",entity.removed}};
        if (entity.baseline)
            row["baseline"] = same(entity.table,*entity.baseline,project.driver)
                ? QJsonValue(true) : QJsonValue(tableJson(*entity.baseline));
        if (entity.position)
            row["position"] = QJsonArray{entity.position->x(),entity.position->y()};
        entities << row;
    }
    auto bytes = QJsonDocument(QJsonObject{
        {"format","navidog-er"},{"version",1},{"title",project.title},
        {"dialect",databaseKindKey(project.driver)},{"databaseVersion",versionKey(project.version)},
        {"profile",project.profileId},{"database",project.database},{"schema",project.schema},
        {"layout",int(project.layout)},{"grouping",int(project.grouping)},{"objects",entities}
    }).toJson(QJsonDocument::Compact);
    if (bytes.size() > maxProjectBytes) fail("ER 模型文件超过大小上限");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(file.errorString());
}

Project open(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
    if (file.size() > maxProjectBytes) fail("ER 模型文件超过大小上限");
    QJsonParseError error;
    const auto value = QJsonDocument::fromJson(file.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError || value["format"] != "navidog-er" ||
        value["version"].toInt() != 1 || !value["objects"].isArray())
        fail("无法读取此 ER 模型文件");
    Project project;
    project.title = value["title"].toString("ER 模型");
    project.driver = databaseKindFromKey(value["dialect"].toString());
    if (project.driver == K::Unknown) fail("ER 模型使用了未知数据库类型");
    project.version = versionFromKey(value["databaseVersion"].toString());
    project.profileId = value["profile"].toString();
    project.database = value["database"].toString();
    project.schema = value["schema"].toString();
    project.layout = LayoutKind(std::clamp(value["layout"].toInt(),0,2));
    project.grouping = GroupKind(std::clamp(value["grouping"].toInt(),0,2));
    const auto rows = value["objects"].toArray();
    if (rows.size() > maxEntities) fail("模型表数量超过上限");
    QSet<int> ids;
    project.entities.reserve(rows.size());
    for (const auto& item : rows) {
        const auto row = item.toObject();
        const int id = row["id"].toInt();
        if (id <= 0 || id >= std::numeric_limits<int>::max() || ids.contains(id)) fail("模型对象标识无效");
        ids.insert(id);
        Entity e;
        e.id = id;
        project.nextId = std::max(project.nextId,e.id+1);
        const auto baseline = row["baseline"];
        e.table = readDefinition(row["table"].toObject(),baseline.isObject() || baseline.toBool());
        if (baseline.isObject()) e.baseline = readDefinition(baseline.toObject(),true);
        else if (baseline.toBool()) e.baseline = e.table;
        e.removed = row["removed"].toBool();
        e.group = row["group"].toString();
        const auto pos = row["position"].toArray();
        if (pos.size() == 2 && pos[0].isDouble() && pos[1].isDouble() &&
            std::abs(pos[0].toDouble()) < 1e8 && std::abs(pos[1].toDouble()) < 1e8)
            e.position = QPointF(pos[0].toDouble(),pos[1].toDouble());
        project.entities.push_back(std::move(e));
    }
    return project;
}
}
