#pragma once

#include "table_design.hpp"
#include <QPointF>
#include <QRectF>
#include <QHash>
#include <optional>

namespace navidog::er {
using EntityId = quint32;
constexpr int maxEntities = 100000;
constexpr qint64 maxProjectBytes = 512LL * 1024 * 1024;

enum class LayoutKind { GravityGrid, Grid, DependencyLayers };
enum class GroupKind { Prefix, Schema, Custom };

struct Entity {
    EntityId id = 0;
    TableDefinition table;
    std::optional<TableDefinition> baseline;
    QString group;
    std::optional<QPointF> position;
    bool removed = false;
};

struct Project {
    QString title = "ER 模型";
    QString profileId, database, schema;
    DatabaseKind driver = DatabaseKind::SQLite;
    DatabaseVersion version = DatabaseVersion::Auto;
    LayoutKind layout = LayoutKind::GravityGrid;
    GroupKind grouping = GroupKind::Prefix;
    QVector<Entity> entities;
    EntityId nextId = 1;
    EntityId add(TableDefinition definition, bool imported = false);
};

struct Edge {
    int from, to, constraint;
    bool uniqueSource = false;
};

struct Graph {
    QVector<int> active;
    QVector<Edge> edges;
    QVector<QVector<int>> incidents;
    QHash<EntityId, int> byId;
    QHash<QString, int> byName;
};

QString nameKey(const CatalogNode&, DatabaseKind);
QString displayName(const CatalogNode&, DatabaseKind);
QString groupName(const Project&, const Entity&);
Graph makeGraph(const Project&);
void replaceEntity(Project&, int index, TableDefinition, QHash<QString,QString> renames = {});
QStringList validate(const Project&, const Graph&, int limit = 50);
QStringList ddl(const Project&, bool changesOnly, const Cancellation& cancel = {});
QString viewBody(const QString& definition, DatabaseKind);
void save(const Project&, const QString& path);
Project open(const QString& path);

struct Group {
    QString name;
    QVector<int> members;
    QRectF bounds;
};
struct Arrangement {
    QVector<QRectF> boxes;
    QVector<Group> groups;
    QRectF bounds;
};

constexpr qreal cardWidth = 250;
constexpr qreal cardHeight = 128;
Arrangement arrange(const Project&, const Graph&);

class SpatialIndex {
public:
    void reset(const Arrangement&);
    void move(int index, const QRectF& before, const QRectF& after);
    QVector<int> intersecting(const QRectF&, const Arrangement&) const;
private:
    QHash<quint64, QVector<int>> buckets_;
};

struct ImportOptions {
    QString schema;
    QString nameContains;
    QStringList exactNames;
    bool includeViews = true;
};

struct ImportResult {
    Project project;
    QStringList notices;
};
bool supportsReverse(DatabaseKind);
ImportResult reverse(Database&, const ConnectionProfile&, const ImportOptions&,
                     const Cancellation&, const std::function<void(int)>& progress = {});
}
