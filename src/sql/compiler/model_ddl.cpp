#include "parser.hpp"
namespace navidog::sql::compiler {
std::size_t typeSyntaxBytes(const BasicTypeSpec<std::vector<std::string>> &syntax) {
  std::size_t bytes=sizeof(syntax)+syntax.nameQuoted.size();
  for(const auto &name:syntax.name)bytes+=sizeof(name)+name.size();
  for(const auto &argument:syntax.arguments)bytes+=sizeof(argument)+argument.value.size();
  for(const auto &modifier:syntax.modifiers)bytes+=sizeof(modifier)+modifier.size();
  if(syntax.lengthSemantics)bytes+=syntax.lengthSemantics->size();
  if(syntax.intervalType)bytes+=sizeof(IntervalSpec);
  return bytes;
}
BasicTypeSpec<std::vector<std::string>> lowerTypeSpec(const Compilation &c,const BasicTypeSpec<QualifiedName> &t) {
  BasicTypeSpec<std::vector<std::string>> out;
  for(const auto &part:t.name) {
    out.name.push_back(canonicalIdentifier(c.dialect,c.ast.identifier(part.identifier),part.quoted));
    out.nameQuoted.push_back(part.quoted);
  }
  out.arguments=t.arguments;out.modifiers=t.modifiers;out.lengthSemantics=t.lengthSemantics;
  out.intervalType=t.intervalType;out.arrayDimensions=t.arrayDimensions;out.origin=t.origin;return out;
}
void collectDdlExpressions(const DdlSyntax &d, std::vector<ExprId> &work) {
  auto constraint = [&](const auto &k) { work.push_back(k.check); };
  auto column = [&](const auto &col) {
    work.push_back(col.defaultValue);
    work.push_back(col.generated);
    work.push_back(col.identityStart);
    work.push_back(col.identityIncrement);
    for (const auto &k : col.constraints)
      constraint(k);
  };
  for (const auto &col : d.definitions)
    column(col);
  for (const auto &k : d.constraints)
    constraint(k);
  for (const auto &a : d.actions) {
    work.push_back(a.value);
    if (a.column)
      column(*a.column);
    if (a.constraint)
      constraint(*a.constraint);
  }
  for (const auto &key : d.keys) {
    work.push_back(key.expression);
    work.push_back(key.prefixLength);
  }
  for (const auto &index:d.tableIndexes)
    for (const auto &key:index.keys) {
      work.push_back(key.expression);
      work.push_back(key.prefixLength);
    }
  for (const auto &option : d.sequenceOptions)
    work.push_back(option.second);
  work.push_back(d.predicate);
  work.push_back(d.commentValue);
}
ModelDdl lowerDdl(const Compilation &c, const DdlSyntax &in,
                  const std::unordered_map<ExprId, ExprId> &ids) {
  auto id = [&](ExprId e) { return ids.at(e); };
  auto name = [&](const NamePart &p) {
    return canonicalIdentifier(c.dialect, c.ast.identifier(p.identifier),
                               p.quoted);
  };
  auto qualified = [&](const QualifiedName &parts) {
    std::vector<std::string> result;
    for (const auto &p : parts)
      result.push_back(name(p));
    return result;
  };
  auto type = [&](const BasicTypeSpec<QualifiedName> &t) { return lowerTypeSpec(c,t); };
  auto constraint = [&](const BasicConstraint<NamePart, QualifiedName> &k) {
    BasicConstraint<std::string, std::vector<std::string>> out;
    out.kind = k.kind;
    if (k.name)
      out.name = name(*k.name);
    for (const auto &col : k.columns)
      out.columns.push_back(name(col));
    for (const auto &col : k.referencedColumns)
      out.referencedColumns.push_back(name(col));
    out.referencedTable = qualified(k.referencedTable);
    out.check = id(k.check);
    out.onDelete = k.onDelete;
    out.onUpdate = k.onUpdate;
    out.deferrable = k.deferrable;
    out.initiallyDeferred = k.initiallyDeferred;
    out.clustering = k.clustering;
    out.origin = k.origin;
    return out;
  };
  auto column = [&](const BasicColumnSpec<NamePart, QualifiedName> &col) {
    BasicColumnSpec<std::string, std::vector<std::string>> out;
    out.name = name(col.name);
    out.type = type(col.type);
    out.nullable = col.nullable;
    out.nullabilityOrigin=col.nullabilityOrigin;
    out.defaultValue = id(col.defaultValue);
    out.defaultParenthesized = col.defaultParenthesized;
    out.generated = id(col.generated);
    out.identityStart = id(col.identityStart);
    out.identityIncrement = id(col.identityIncrement);
    out.identity = col.identity;
    out.autoRandom = col.autoRandom;
    out.stored = col.stored;
    out.collation = qualified(col.collation);
    out.characterSet = qualified(col.characterSet);
    out.comment = col.comment;
    for (const auto &k : col.constraints)
      out.constraints.push_back(constraint(k));
    out.origin = col.origin;
    return out;
  };
  ModelDdl out;
  out.object = in.object;
  out.name = qualified(in.name);
  out.indexedTable = qualified(in.indexedTable);
  out.likeTable = qualified(in.likeTable);
  out.indexMethod = qualified(in.indexMethod);
  if (in.virtualTable) {
    auto module = std::make_shared<BasicVirtualTable<std::string>>();
    module->module = name(in.virtualTable->module);
    module->argumentList = in.virtualTable->argumentList;
    module->arguments = in.virtualTable->arguments;
    out.virtualTable = std::move(module);
  }
  for (const auto &n : in.names)
    out.names.push_back(qualified(n));
  for (const auto &pair : in.renames)
    out.renames.push_back({qualified(pair.first), qualified(pair.second)});
  for (const auto &n : in.columns)
    out.columns.push_back(name(n));
  for (const auto &n : in.includeColumns)
    out.includeColumns.push_back(name(n));
  for (const auto &col : in.definitions)
    out.definitions.push_back(column(col));
  for (const auto &k : in.constraints)
    out.constraints.push_back(constraint(k));
  for (const auto &a : in.actions) {
    BasicAlterAction<std::string, std::vector<std::string>> action;
    action.kind = a.kind;
    if (a.name)
      action.name = name(*a.name);
    if (a.newName)
      action.newName = name(*a.newName);
    if (a.column)
      action.column = column(*a.column);
    if (a.constraint)
      action.constraint = constraint(*a.constraint);
    if (a.type)
      action.type = type(*a.type);
    action.value = id(a.value);
    action.defaultParenthesized = a.defaultParenthesized;
    action.nullable = a.nullable;
    action.dropBehavior = a.dropBehavior;
    action.ifExists = a.ifExists;
    action.ifNotExists = a.ifNotExists;
    action.origin = a.origin;
    out.actions.push_back(std::move(action));
  }
  for (auto key : in.keys) {
    key.expression = id(key.expression);
    key.prefixLength = id(key.prefixLength);
    out.keys.push_back(key);
  }
  for (const auto &index:in.tableIndexes) {
    BasicTableIndex<std::string> result;
    if (index.name) result.name=name(*index.name);
    result.unique=index.unique;
    result.method=index.method;
    result.origin=index.origin;
    for (auto key:index.keys) {
      key.expression=id(key.expression);
      key.prefixLength=id(key.prefixLength);
      result.keys.push_back(std::move(key));
    }
    out.tableIndexes.push_back(std::move(result));
  }
  for (const auto &opt : in.sequenceOptions)
    out.sequenceOptions.push_back({opt.first, id(opt.second)});
  out.predicate = id(in.predicate);
  out.commentValue = id(in.commentValue);
  for (const auto &target : in.truncateTargets)
    out.truncateTargets.push_back(
        {qualified(target.name), target.only, target.descendants});
  out.ifExists = in.ifExists;
  out.ifNotExists = in.ifNotExists;
  out.orReplace = in.orReplace;
  out.temporary = in.temporary;
  out.globalTemporary = in.globalTemporary;
  out.unlogged = in.unlogged;
  out.unique = in.unique;
  out.withNoData = in.withNoData;
  out.purge = in.purge;
  out.truncateOnly = in.truncateOnly;
  out.truncateDescendants = in.truncateDescendants;
  out.restartIdentity = in.restartIdentity;
  out.reuseStorage = in.reuseStorage;
  out.dropBehavior = in.dropBehavior;
  out.engine = in.engine;
  out.characterSet = in.characterSet;
  out.collation = in.collation;
  out.tablespace = in.tablespace;
  out.onCommit = in.onCommit;
  out.viewCheck = in.viewCheck;
  out.mysqlViewOptions = in.mysqlViewOptions;
  out.mysqlLoadableFunction = in.mysqlLoadableFunction;
  out.tableComment = in.tableComment;
  out.distribution = in.distribution;
  out.indexScope = in.indexScope;
  if (in.partition) {
    const auto &source = *in.partition;
    auto &partition = out.partition.emplace();
    partition.origin = source.origin;
    partition.method = source.method;
    partition.linear = source.linear;
    partition.count = source.count;
    partition.algorithm = source.algorithm;
    for (const auto &column : source.columns) partition.columns.push_back(name(column));
  }
  return out;
}
} // namespace navidog::sql::compiler
