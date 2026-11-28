#include "navidog/database_sql.hpp"
#include "navidog/metadata_cache.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace navidog {
namespace {
std::optional<sql::compiler::Dialect> compilerDialect(DatabaseKind kind) {
  return compilerDialectFor(kind);
}
std::string indexKey(const std::string &kind, const std::string &schema,
                     const std::string &name) {
  return kind + std::to_string(schema.size()) + ":" + schema +
         std::to_string(name.size()) + ":" + name;
}
bool ascii(std::string_view name) {
  for (unsigned char c : name)
    if (c >= 128)
      return false;
  return true;
}
std::string folded(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return value;
}
sql::compiler::ValueType valueType(const QString &text, DatabaseKind kind) {
  using namespace sql::compiler;
  auto type = text.trimmed();
  // Do not match type fragments: point/interval are not integer types, and
  // an array of integers is not a scalar integer.
  if (type.contains('[') || type.contains(']'))
    return ValueType::Unknown;
  const auto parameters = type.indexOf('(');
  if (parameters >= 0)
    type = type.left(parameters).trimmed();
  for (const auto &suffix :
       {QStringLiteral(" unsigned"), QStringLiteral(" zerofill")})
    if (type.endsWith(suffix, Qt::CaseInsensitive))
      type.chop(suffix.size());
  return typeCategory({type.trimmed().toStdString()},
                      compilerDialect(kind).value_or(Dialect::PostgreSQL));
}
} // namespace
void prepareMetadataTypeFacts(MetadataSnapshot &snapshot,const Cancellation &cancel) {
  snapshot.lookup.reset();
  snapshot.typeFacts.clear();
  const auto dialect=compilerDialect(snapshot.driver);
  if(!dialect)return;
  std::set<std::string> attempted;
  std::size_t bytes = 0;
  const auto prepare = [&](const QString &nativeType) {
    if (cancel && cancel->load()) return false;
    const auto name = nativeType.toStdString();
    if (name.empty() || name.size() > 4096 || attempted.count(name)) return true;
    if (attempted.size() >= 512) return false;
    attempted.insert(name);
    try {
      if (auto type = sql::compiler::catalogTypeFromText(*dialect, name)) {
        sql::compiler::TypeContext measure;
        measure.intern(*type);
        bytes += 2 * name.size() + measure.accountedBytes();
        if (bytes > 2 * 1024 * 1024) return false;
        snapshot.typeFacts.emplace(name, std::move(*type));
      }
    } catch (const std::exception &) { /* Optional type enrichment never disables names/columns. */ }
    return true;
  };
  for (const auto &object : snapshot.objects)
    for (const auto &column : object.columns)
      if (!prepare(column.type)) return;
  // Table/parameter types have priority within the same bounded dictionary.
  for (const auto &object : snapshot.objects)
    if (object.kind == "function" && !prepare(object.type)) return;
}
struct MetadataLookupIndex {
  QVector<MetadataObject> objects;
  DatabaseKind driver;
  bool caseSensitive;
  std::map<std::string,std::shared_ptr<const sql::compiler::SqlType>> types;
  std::unordered_map<std::string,std::vector<std::size_t>> names;
};
MetadataCatalog::MetadataCatalog(MetadataSnapshotPtr snapshot)
    : snapshot_(std::move(snapshot)) {
  if (!snapshot_ || snapshot_->invalidated)
    return;
  const auto matches=[&](const auto &index) {
    return index && index->driver==snapshot_->driver && index->caseSensitive==snapshot_->caseSensitive &&
        index->objects.constData()==snapshot_->objects.constData() &&
        index->objects.size()==snapshot_->objects.size();
  };
  lookup_=std::atomic_load(&snapshot_->lookup.index_);
  if (matches(lookup_)) return;
  auto lookup=std::make_shared<MetadataLookupIndex>();
  // Sharing the Qt array keeps its identity alive. Editing the snapshot
  // before republication detaches it, so a new reader sees a new index.
  lookup->objects=snapshot_->objects;
  lookup->driver=snapshot_->driver;
  lookup->caseSensitive=snapshot_->caseSensitive;
  for (const auto &type:snapshot_->typeFacts)
    lookup->types.emplace(type.first,std::make_shared<const sql::compiler::SqlType>(type.second));
  lookup->names.reserve(std::size_t(snapshot_->objects.size()));
  for (int i = 0; i < snapshot_->objects.size(); ++i) {
    const auto &o = snapshot_->objects[i];
    const auto kind = o.kind == "view" || o.kind=="materializedView" ? "table" : o.kind.toStdString();
    lookup->names[indexKey(kind, normalized(o.schema.toStdString()),
                    (kind != "table" && isMySqlFamily(snapshot_->driver)
                         ? folded(o.name.toStdString())
                         : normalized(o.name.toStdString())))]
        .push_back(std::size_t(i));
  }
  // Publish the entire immutable index together. Concurrent workers use
  // the one that wins publication and never observe partially built names.
  std::shared_ptr<const MetadataLookupIndex> built=std::move(lookup);
  if (!std::atomic_compare_exchange_strong(&snapshot_->lookup.index_,&lookup_,built) && matches(lookup_))
    return;
  lookup_=std::move(built);
}
std::vector<sql::compiler::CatalogObjectDefinition>
MetadataCatalog::listObjects(const sql::compiler::NameResolutionContext &ctx,
    sql::compiler::CatalogObjectKind kind,std::size_t limit) const {
  using K=sql::compiler::CatalogObjectKind;
  static const std::map<K,QString> kinds={{K::Database,"database"},{K::Schema,"schema"},{K::Index,"index"},
    {K::Sequence,"sequence"},{K::Trigger,"trigger"},{K::Type,"type"},{K::Synonym,"synonym"},
    {K::Package,"package"},{K::Event,"event"}};
  std::vector<sql::compiler::CatalogObjectDefinition> out;
  if(!snapshot_ || snapshot_->invalidated || ctx.database!=snapshot_->database.toStdString())return out;
  auto found=kinds.find(kind);if(found==kinds.end())return out;
  for(const auto &o:snapshot_->objects) {
    if(out.size()>=limit)break;
    if(o.kind!=found->second)continue;
    out.push_back({kind,o.database.toStdString(),o.schema.toStdString(),o.name.toStdString(),o.type.toStdString(),{}});
  }
  return out;
}
std::vector<sql::compiler::TableDefinition>
MetadataCatalog::listRelations(const sql::compiler::NameResolutionContext &ctx,
                               std::size_t limit) const {
  return matchingRelations(ctx,limit,{});
}
std::vector<sql::compiler::TableDefinition>
MetadataCatalog::matchingRelations(const sql::compiler::NameResolutionContext &ctx,std::size_t limit,
                                   const sql::compiler::CatalogNameFilter &accept) const {
  std::vector<sql::compiler::TableDefinition> out;
  if (!snapshot_ || snapshot_->invalidated ||
      ctx.database != snapshot_->database.toStdString())
    return out;
  for (const auto &o : snapshot_->objects) {
    if (out.size() >= limit)
      break;
    if (o.kind != "table" && o.kind != "view" && o.kind != "materializedView")
      continue;
    sql::compiler::TableDefinition t;
    t.database = o.database.toStdString();
    t.schema = o.schema.toStdString();
    t.name = o.name.toStdString();
    if (accept && !accept(t.database,t.schema,t.name)) continue;
    t.complete = snapshot_->columnsComplete;
    t.arityKnown = t.complete;
    t.kind = o.kind == "table"  ? sql::compiler::ObjectKind::Table
             : o.kind == "view" ? sql::compiler::ObjectKind::View
                                : sql::compiler::ObjectKind::MaterializedView;
    out.push_back(std::move(t));
  }
  return out;
}
std::vector<sql::compiler::RoutineDefinition>
MetadataCatalog::listRoutines(const sql::compiler::NameResolutionContext &ctx,
                              bool procedure, std::size_t limit) const {
  return matchingRoutines(ctx,procedure,limit,{});
}
std::vector<sql::compiler::RoutineDefinition>
MetadataCatalog::matchingRoutines(const sql::compiler::NameResolutionContext &ctx,
                                  bool procedure,std::size_t limit,
                                  const sql::compiler::CatalogNameFilter &accept) const {
  std::vector<sql::compiler::RoutineDefinition> out;
  if (!snapshot_ || snapshot_->invalidated ||
      ctx.database != snapshot_->database.toStdString())
    return out;
  for (const auto &o : snapshot_->objects) {
    if (out.size() >= limit)
      break;
    if (o.kind != (procedure ? "procedure" : "function"))
      continue;
    auto database=o.database.toStdString(),schema=o.schema.toStdString(),name=o.name.toStdString();
    if (accept && !accept(database,schema,name)) continue;
    out.push_back({std::move(database), std::move(schema),
                   std::move(name), std::size_t(o.minArgs),
                   std::size_t(o.maxArgs), procedure, o.variadic,
                   valueType(o.type, snapshot_->driver),o.routineKind,o.volatility});
  }
  return out;
}
std::string MetadataCatalog::normalized(const std::string &value) const {
  return snapshot_ && !snapshot_->caseSensitive ? folded(value) : value;
}
bool MetadataCatalog::relationNameEquals(std::string_view a,
                                         std::string_view b) const {
  return normalized(std::string(a)) == normalized(std::string(b));
}
bool MetadataCatalog::columnNameEquals(std::string_view a,
                                       std::string_view b) const {
  const bool insensitive = snapshot_ && (isMySqlFamily(snapshot_->driver) ||
                                         !snapshot_->caseSensitive);
  return insensitive ? folded(std::string(a)) == folded(std::string(b))
                     : a == b;
}
std::vector<std::string> MetadataCatalog::resolvedSchemas(
    const sql::compiler::NameResolutionContext &ctx,
    const std::vector<std::string> &name) const {
  if (!snapshot_ || name.empty() || name.size() > 3 ||
      ctx.database != snapshot_->database.toStdString())
    return {};
  if (name.size() == 3 && name[0] != snapshot_->database.toStdString())
    return {};
  if (name.size() > 1) {
    const auto schema = name[name.size() - 2];
    if (isMySqlFamily(snapshot_->driver) &&
        schema != snapshot_->database.toStdString())
      return {};
    return {schema};
  }
  std::vector<std::string> result;
  for (const auto &p : snapshot_->searchPath)
    result.push_back(p.toStdString());
  return result;
}
std::optional<sql::compiler::TableDefinition>
MetadataCatalog::findTable(const sql::compiler::NameResolutionContext &ctx,
                           const std::vector<std::string> &name) const {
  if (!snapshot_ || !snapshot_->usable())
    return {};
  for (const auto &schema : resolvedSchemas(ctx, name)) {
    const auto found = lookup_->names.find(
        indexKey("table", normalized(schema), normalized(name.back())));
    if (found == lookup_->names.end())
      continue;
    const auto &object = snapshot_->objects[int(found->second.front())];
    sql::compiler::TableDefinition table;
    table.database = object.database.toStdString();
    table.schema = object.schema.toStdString();
    table.name = object.name.toStdString();
    table.kind = object.kind == "view" ? sql::compiler::ObjectKind::View
                 : object.kind == "materializedView"
                     ? sql::compiler::ObjectKind::MaterializedView
                     : sql::compiler::ObjectKind::Table;
    table.complete = snapshot_->columnsComplete;
    table.arityKnown = snapshot_->columnsComplete;
    table.noDescendantsKnown = object.noDescendantsKnown && object.kind == "table";
    table.rowSecurityDisabledKnown = object.rowSecurityDisabledKnown && object.kind == "table";
    for (const auto &column : object.columns) {
      sql::compiler::ColumnDefinition definition{column.name.toStdString(),valueType(column.type,snapshot_->driver)};
      definition.nonNullKnown=column.nonNullKnown && object.kind=="table";
      if(auto type=lookup_->types.find(column.type.toStdString());type!=lookup_->types.end())definition.sqlType=type->second;
      table.columns.push_back(std::move(definition));
      if ((isMySqlFamily(snapshot_->driver) || !snapshot_->caseSensitive) &&
          !ascii(column.name.toStdString()))
        table.complete = false;
    }
    if (table.complete && object.kind == "table") {
      for (const auto &name : object.primaryKey) {
        const auto spelling = name.toStdString();
        const auto field = std::find_if(table.columns.begin(), table.columns.end(), [&](const auto &column) {
          return column.nonNullKnown && column.name == spelling;
        });
        if (field == table.columns.end()) break;
        table.primaryKey.push_back(field->name);
      }
      table.primaryKeyKnown = !table.primaryKey.empty() && table.primaryKey.size() == std::size_t(object.primaryKey.size());
      if (!table.primaryKeyKnown) table.primaryKey.clear();
      if (snapshot_->driver==DatabaseKind::PostgreSQL) {
        for (const auto &key:object.strictUniqueKeys) {
          std::vector<std::string> columns;
          for (const auto &name:key) {
            const auto spelling=name.toStdString();
            const auto column=std::find_if(table.columns.begin(),table.columns.end(),[&](const auto &candidate) {
              return candidate.name==spelling && candidate.nonNullKnown;
            });
            if (column==table.columns.end()) break;
            columns.push_back(column->name);
          }
          if (!columns.empty() && columns.size()==std::size_t(key.size()))
            table.strictUniqueKeys.push_back(std::move(columns));
        }
      }
    }
    return table;
  }
  return {};
}
bool MetadataCatalog::tableLookupComplete(
    const sql::compiler::NameResolutionContext &ctx,
    const std::vector<std::string> &name) const {
  if (snapshot_ && usesOracleSql(snapshot_->driver))
    return false; // Synonyms / database links need their own resolution layer.
  if (!snapshot_ || !snapshot_->usable() || !snapshot_->tablesComplete)
    return false;
  if (!snapshot_->caseSensitive)
    for (const auto &part : name)
      if (!ascii(part))
        return false;
  const auto schemas = resolvedSchemas(ctx, name);
  if (schemas.empty())
    return false;
  for (const auto &schema : schemas) {
    bool known = snapshot_->searchPath.contains(QString::fromStdString(schema));
    if (!known)
      for (const auto &object : snapshot_->objects)
        if (object.schema.toStdString() == schema) {
          known = true;
          break;
        }
    if (!known)
      return false;
  }
  return true;
}
std::vector<sql::compiler::RoutineDefinition>
MetadataCatalog::findRoutines(const sql::compiler::NameResolutionContext &ctx,
                              const std::vector<std::string> &name,
                              bool procedure) const {
  std::vector<sql::compiler::RoutineDefinition> result;
  if (!snapshot_ || !snapshot_->usable() || !snapshot_->routinesComplete)
    return result;
  for (const auto &schema : resolvedSchemas(ctx, name)) {
    const auto found = lookup_->names.find(
        indexKey(procedure ? "procedure" : "function", normalized(schema),
                 (isMySqlFamily(snapshot_->driver) ? folded(name.back())
                                                   : normalized(name.back()))));
    if (found == lookup_->names.end())
      continue;
    for (auto id : found->second) {
      using namespace sql::compiler;
      const auto &o = snapshot_->objects[int(id)];
      RoutineDefinition routine{o.database.toStdString(), o.schema.toStdString(),
                                o.name.toStdString(),
                                std::size_t(std::max(0, o.minArgs)),
                                std::size_t(std::max(0, o.maxArgs)), procedure,
                                o.variadic, valueType(o.type, snapshot_->driver), o.routineKind, o.volatility};
      if (!procedure && o.routineKind != RoutineKind::TableFunction) {
        auto type = lookup_->types.find(o.type.toStdString());
        if (type != lookup_->types.end() && type->second->family != TypeFamily::Unknown &&
            type->second->opaqueName.empty())
          routine.sqlType = type->second;
      }
      result.push_back(std::move(routine));
    }
  }
  return result;
}
bool MetadataCatalog::routineLookupComplete(
    const sql::compiler::NameResolutionContext &ctx,
    const std::vector<std::string> &name, bool procedure) const {
  // An unqualified unknown function may be built in, an extension or a synonym.
  return snapshot_ && snapshot_->routinesComplete &&
         (procedure || name.size() > 1) && tableLookupComplete(ctx, name);
}
} // namespace navidog
