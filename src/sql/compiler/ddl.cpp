#include "parser.hpp"
#include "sqlserver_syntax.hpp"
#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace navidog::sql::compiler {
ObjectKind Parser::objectKind() {
  if (at("TABLEGROUP") || (at("TABLE") && at("GROUP", 1))) {
    if (!ddlGrammar().oceanbase)
      fail("TABLEGROUP is specific to OceanBase MySQL mode", DiagnosticCode::UnsupportedSyntax);
    if (!eat("TABLEGROUP")) {
      expect("TABLE");
      expect("GROUP");
    }
    return ObjectKind::TableGroup;
  }
  if (eat("TABLE"))
    return ObjectKind::Table;
  if (eat("VIEW"))
    return ObjectKind::View;
  if (eat("MATERIALIZED")) {
    expect("VIEW");
    if(!ddlGrammar().postgres && !ddlGrammar().oracle)
      fail("materialized views are unavailable in this dialect",DiagnosticCode::UnsupportedSyntax);
    return ObjectKind::MaterializedView;
  }
  if (eat("INDEX"))
    return ObjectKind::Index;
  if (eat("SCHEMA"))
    return ObjectKind::Schema;
  if (eat("DATABASE"))
    return ObjectKind::Database;
  if (eat("SEQUENCE"))
    return ObjectKind::Sequence;
  fail("unsupported DDL object kind", DiagnosticCode::UnsupportedSyntax);
}
void Parser::ifNotExists(DdlSyntax &d) {
  if (!eat("IF"))
    return;
  if (ddlGrammar().oracle || ddlGrammar().sqlserver)
    fail("IF NOT EXISTS is unavailable in this DDL grammar baseline",
         DiagnosticCode::UnsupportedSyntax);
  expect("NOT");
  expect("EXISTS");
  d.ifNotExists = true;
}
void Parser::ifExists(DdlSyntax &d) {
  if (!eat("IF"))
    return;
  if (ddlGrammar().oracle)
    fail("IF EXISTS is unavailable in the Oracle 19c baseline",
         DiagnosticCode::UnsupportedSyntax);
  expect("EXISTS");
  d.ifExists = true;
}
void Parser::columnList(std::vector<NamePart> &columns) {
  expect("(");
  do {
    syntaxNode();
    columns.push_back(namePart());
  } while (eat(","));
  expect(")");
}
BasicTypeSpec<QualifiedName> Parser::typeSpec(bool literalName) {
  syntaxNode();
  BasicTypeSpec<QualifiedName> type;
  type.origin.begin = current().range.begin;
  if (ddlGrammar().sqlserver && at("NATIONAL"))
    fail("SQL Server NATIONAL character type prefixes require a dedicated model",DiagnosticCode::UnsupportedSyntax);
  if (ddlGrammar().postgres) {
    if (at("SETOF") || at("NATIONAL"))
      fail("this PostgreSQL type prefix requires a specialized type production",DiagnosticCode::UnsupportedSyntax);
    const bool literalQualifier=literalName && nameToken() && at(".",1);
    if (!typeNameToken() && !literalQualifier) fail("expected a PostgreSQL type name");
    if (literalName && at(".",1) && !nameToken())
      fail("a qualified PostgreSQL type literal requires a column-identifier prefix");
    const bool qualifiedType=functionNameToken() || literalQualifier;
    type.name.push_back(namePart(true));
    while (eat(".")) {
      if (!qualifiedType) fail("this PostgreSQL system type keyword cannot qualify a type name");
      if (type.name.size()>=4) fail("at most four type name components are supported",DiagnosticCode::UnsupportedSyntax);
      type.name.push_back(namePart(true));
    }
  } else if (at("TIME") || (ddlGrammar().mysql && at("SET")) || (ddlGrammar().sqlserver && at("DOUBLE"))) {
    auto token = take();
    type.name.push_back({context_.ast.intern(token.value), false, token.range});
  } else
    type.name = qualifiedName();
  auto base = asciiUpper(context_.ast.identifier(type.name.back().identifier));
  if (ddlGrammar().sqlserver && base=="DOUBLE") {
    expect("PRECISION");type.modifiers.push_back("PRECISION");
  } else if ((base == "DOUBLE" && eat("PRECISION")) ||
      ((base == "CHARACTER" || base == "CHAR" || base == "BIT") &&
       eat("VARYING")))
    type.modifiers.push_back(asciiUpper(previous().value));
  if(base=="INTERVAL" && type.name.size()==1 && !type.name[0].quoted)intervalTypeTail(type);
  else if (eat("(")) {
    do {
      std::string sign;
      if(eat("-")) sign="-"; else if(eat("+")) sign="+";
      if(!sign.empty() && current().kind!=TokenKind::Number)fail("type argument sign requires a number");
      if (current().kind != TokenKind::Number &&
          current().kind != TokenKind::String && !at("MAX") && !at("*"))
        fail("expected type length, precision, MAX or enum literal");
      syntaxNode();
      auto token = take();
      type.arguments.push_back({token.kind, sign+token.value});
      if (ddlGrammar().oracle && (at("CHAR") || at("BYTE")))
        type.lengthSemantics = asciiUpper(take().value);
    } while (eat(","));
    expect(")");
  }
  if ((base == "TIME" || base == "TIMESTAMP") &&
      (at("WITH") || at("WITHOUT"))) {
    std::string modifier = asciiUpper(take().value);
    if (eat("LOCAL"))
      modifier += " LOCAL";
    expect("TIME");
    expect("ZONE");
    if((ddlGrammar().postgres && modifier.find("LOCAL")!=std::string::npos) ||
       (ddlGrammar().oracle && (base=="TIME" || modifier=="WITHOUT")))
      fail("time zone type modifier is invalid in this dialect");
    type.modifiers.push_back(modifier + " TIME ZONE");
    if (!ddlGrammar().postgres && !ddlGrammar().oracle)
      fail("time zone type modifier is unavailable in this dialect",
           DiagnosticCode::UnsupportedSyntax);
  }
  for (auto word : {"UNSIGNED", "ZEROFILL"})
    if (eat(word)) {
      if (!ddlGrammar().mysql)
        fail("MySQL numeric type modifier in another dialect");
      type.modifiers.push_back(word);
    }
  while (eat("[")) {
    if (!ddlGrammar().postgres)
      fail("array types are PostgreSQL-specific in this frontend");
    expect("]");
    ++type.arrayDimensions;
  }
  type.origin.end = previous().range.end;
  return type;
}
BasicTableIndex<NamePart> Parser::tableIndex() {
  syntaxNode();
  BasicTableIndex<NamePart> index;
  index.origin.begin=current().range.begin;
  index.unique=eat("UNIQUE");
  if (!eat("KEY")) expect("INDEX");
  if (!at("USING") && nameToken()) index.name=namePart();
  const auto readMethod=[&] {
    if (index.method!=TableIndexMethod::Default)
      fail("duplicate index method",DiagnosticCode::InvalidQueryShape);
    if (eat("BTREE")) index.method=TableIndexMethod::Btree;
    else {
      expect("HASH");
      index.method=TableIndexMethod::Hash;
    }
  };
  if (eat("USING")) readMethod();
  expect("(");
  do {
    if (index.keys.size()>=context_.options.limits.columnsPerQuery)
      fail("table index key count exceeds the column limit",DiagnosticCode::ResourceLimit);
    if (at("("))
      fail("inline functional indexes require expression-key support",DiagnosticCode::UnsupportedSyntax);
    IndexElement key;
    const auto column=namePart();
    key.expression=node({ExprKind::Name,column.range,"",{column},{}});
    if (eat("(")) {
      const auto location=current().range;
      const auto length=ddlUnsignedInteger();
      if (!length || length>UINT32_MAX) {
        context_.report(DiagnosticCode::InvalidQueryShape,location,"index prefix length requires a positive integer");
        throw ParseFailure{};
      }
      key.prefixLength=node({ExprKind::Number,location,std::to_string(length),{},{}});
      expect(")");
    }
    key.descending=eat("DESC");
    if (!key.descending) eat("ASC");
    index.keys.push_back(key);
  } while (eat(","));
  expect(")");
  if (eat("USING")) readMethod();
  index.origin.end=previous().range.end;
  return index;
}
bool Parser::constraintStart() const {
  return at("CONSTRAINT") || at("PRIMARY") || at("UNIQUE") || at("FOREIGN") ||
         at("CHECK") || at("REFERENCES");
}
void Parser::referenceTail(
    BasicConstraint<NamePart, QualifiedName> &constraint) {
  auto action = [&] {
    if (eat("CASCADE"))
      return ReferenceAction::Cascade;
    if (eat("RESTRICT"))
      return ReferenceAction::Restrict;
    if (eat("NO")) {
      expect("ACTION");
      return ReferenceAction::NoAction;
    }
    expect("SET");
    if (eat("NULL"))
      return ReferenceAction::SetNull;
    expect("DEFAULT");
    return ReferenceAction::SetDefault;
  };
  while (eat("ON")) {
    if (eat("DELETE")) {
      if (constraint.onDelete != ReferenceAction::None)
        fail("duplicate ON DELETE");
      constraint.onDelete = action();
    } else {
      expect("UPDATE");
      if (ddlGrammar().oracle)
        fail("Oracle foreign keys do not support ON UPDATE");
      if (constraint.onUpdate != ReferenceAction::None)
        fail("duplicate ON UPDATE");
      constraint.onUpdate = action();
    }
  }
}
BasicConstraint<NamePart, QualifiedName>
Parser::constraintSpec(std::optional<NamePart> column) {
  syntaxNode();
  BasicConstraint<NamePart, QualifiedName> constraint;
  constraint.origin.begin = current().range.begin;
  if (eat("CONSTRAINT")) {
    if (!(ddlGrammar().tidb && (at("PRIMARY") || at("UNIQUE") || at("FOREIGN") || at("CHECK"))))
      constraint.name = namePart();
    if (column && ddlGrammar().tidb && !at("CHECK"))
      fail("TiDB inline CONSTRAINT names apply only to CHECK constraints");
  }
  if (eat("PRIMARY")) {
    expect("KEY");
    constraint.kind = ConstraintKind::PrimaryKey;
  } else if (eat("UNIQUE")) {
    constraint.kind = ConstraintKind::Unique;
    if (ddlGrammar().mysql)
      eat("KEY");
  } else if (eat("CHECK")) {
    constraint.kind = ConstraintKind::Check;
    expect("(");
    constraint.check = expression();
    requireScalar(constraint.check);
    expect(")");
  } else if (eat("FOREIGN")) {
    expect("KEY");
    constraint.kind = ConstraintKind::ForeignKey;
  } else if (at("REFERENCES") && column)
    constraint.kind = ConstraintKind::ForeignKey;
  else
    fail("expected PRIMARY KEY, UNIQUE, FOREIGN KEY or CHECK constraint");
  if (constraint.kind != ConstraintKind::Check) {
    if (column)
      constraint.columns.push_back(*column);
    else
      columnList(constraint.columns);
  }
  if (at("CLUSTERED") || at("NONCLUSTERED")) {
    if (!ddlGrammar().tidb || constraint.kind != ConstraintKind::PrimaryKey)
      fail("CLUSTERED/NONCLUSTERED here requires a TiDB PRIMARY KEY",
           DiagnosticCode::UnsupportedSyntax);
    constraint.clustering = eat("CLUSTERED") ? PrimaryKeyClustering::Clustered
                                            : PrimaryKeyClustering::Nonclustered;
    if (constraint.clustering == PrimaryKeyClustering::Nonclustered) expect("NONCLUSTERED");
  }
  if (constraint.kind == ConstraintKind::ForeignKey) {
    expect("REFERENCES");
    constraint.referencedTable = qualifiedName();
    if (at("("))
      columnList(constraint.referencedColumns);
    if (!constraint.referencedColumns.empty() &&
        constraint.columns.size() != constraint.referencedColumns.size())
      fail("foreign key local/referenced column counts differ");
    referenceTail(constraint);
  }
  if (at("DEFERRABLE") || (at("NOT") && at("DEFERRABLE", 1))) {
    if (!ddlGrammar().postgres && !ddlGrammar().oracle && !ddlGrammar().sqlite)
      fail("deferrable constraints are unavailable in this dialect");
    const bool negated = eat("NOT");
    expect("DEFERRABLE");
    constraint.deferrable = !negated;
  }
  if (eat("INITIALLY")) {
    if (!ddlGrammar().postgres && !ddlGrammar().oracle && !ddlGrammar().sqlite)
      fail("INITIALLY is unavailable in this dialect");
    if (eat("DEFERRED"))
      constraint.initiallyDeferred = true;
    else {
      expect("IMMEDIATE");
      constraint.initiallyDeferred = false;
    }
    if (constraint.deferrable == false && *constraint.initiallyDeferred)
      fail("NOT DEFERRABLE cannot be INITIALLY DEFERRED");
  }
  constraint.origin.end = previous().range.end;
  return constraint;
}
BasicColumnSpec<NamePart, QualifiedName>
Parser::columnSpec(bool allowMissingType) {
  syntaxNode();
  if (ddlGrammar().sqlserver && at("INDEX"))
    fail("SQL Server inline INDEX definitions require a dedicated index model",DiagnosticCode::UnsupportedSyntax);
  BasicColumnSpec<NamePart, QualifiedName> col;
  col.origin.begin = current().range.begin;
  col.name = namePart();
  if (ddlGrammar().sqlserver && eat("AS")) {
    col.generated = expression();
    col.stored = eat("PERSISTED");
    col.origin.end = previous().range.end;
    return col;
  }
  if ((!ddlGrammar().sqlite && !allowMissingType) || nameToken())
    col.type = typeSpec();
  validateColumnType(col.type);
  if (context_.dialect==Dialect::MySQL && col.type.name.size()==1 &&
      !col.type.name.front().quoted && col.type.arguments.empty() &&
      asciiUpper(context_.ast.identifier(col.type.name.front().identifier))=="VARCHAR") {
    context_.report(DiagnosticCode::ExpectedToken,col.type.origin,"MySQL VARCHAR columns require a length");
    throw ParseFailure{};
  }
  auto identityOptions = [&] {
    if (!eat("("))
      return;
    if (ddlGrammar().sqlserver) {
      col.identityStart = expression();
      expect(",");
      col.identityIncrement = expression();
    } else {
      if (eat("START")) {
        eat("WITH");
        col.identityStart = expression();
      }
      if (eat("INCREMENT")) {
        eat("BY");
        col.identityIncrement = expression();
      }
    }
    expect(")");
  };
  while (true) {
    if (eat("NOT")) {
      const auto start=previous().range.begin;
      expect("NULL");
      if (col.nullable)
        fail("duplicate nullability");
      col.nullable = false;
      col.nullabilityOrigin={start,previous().range.end};
    } else if (eat("NULL")) {
      if (col.nullable)
        fail("duplicate nullability");
      col.nullable = true;
      col.nullabilityOrigin=previous().range;
    } else if (eat("DEFAULT")) {
      if (col.defaultValue)
        fail("duplicate DEFAULT");
      col.defaultParenthesized = context_.dialect == Dialect::MySQL && eat("(");
      col.defaultValue = expression();
      if (col.defaultParenthesized) expect(")");
      requireScalar(col.defaultValue);
    } else if (at("AUTO_RANDOM")) {
      if (!ddlGrammar().tidb)
        fail("AUTO_RANDOM is TiDB-specific", DiagnosticCode::UnsupportedSyntax);
      if (col.autoRandom)
        fail("duplicate AUTO_RANDOM", DiagnosticCode::InvalidQueryShape);
      auto &random = col.autoRandom.emplace();
      random.origin.begin = take().range.begin;
      if (eat("(")) {
        random.shardBits = ddlUnsignedInteger();
        if (eat(",")) random.rangeBits = ddlUnsignedInteger();
        expect(")");
      }
      random.origin.end = previous().range.end;
    } else if (eat("AUTO_INCREMENT")) {
      if (!ddlGrammar().mysql)
        fail("AUTO_INCREMENT is MySQL-specific");
      col.identity = IdentityKind::AutoIncrement;
    } else if (eat("IDENTITY")) {
      if (!ddlGrammar().sqlserver)
        fail("IDENTITY keyword requires SQL Server syntax");
      if (col.identity!=IdentityKind::None)
        fail("duplicate IDENTITY clause");
      col.identity = IdentityKind::Identity;
      identityOptions();
    } else if (at("GENERATED") || (ddlGrammar().mysql && at("AS"))) {
      bool generated = eat("GENERATED");
      IdentityKind mode = IdentityKind::GeneratedAlways;
      if (generated) {
        if (eat("BY")) {
          expect("DEFAULT");
          mode = IdentityKind::GeneratedByDefault;
        } else
          expect("ALWAYS");
      }
      expect("AS");
      if (eat("IDENTITY")) {
        if (!ddlGrammar().postgres && !ddlGrammar().oracle)
          fail("generated identity is unavailable in this dialect");
        col.identity = mode;
        identityOptions();
      } else {
        expect("(");
        col.generated = expression();
        requireScalar(col.generated);
        expect(")");
        if (eat("STORED"))
          col.stored = true;
        else if (eat("VIRTUAL"))
          col.stored = false;
        else if (ddlGrammar().postgres)
          fail("PostgreSQL generated columns require STORED in the baseline");
      }
    } else if (eat("COLLATE"))
      col.collation = qualifiedName();
    else if (ddlGrammar().mysql && eat("CHARACTER")) {
      expect("SET");
      col.characterSet = qualifiedName();
    } else if (ddlGrammar().mysql && eat("COMMENT")) {
      if (current().kind != TokenKind::String)
        fail("column COMMENT requires a string literal");
      col.comment = take().value;
    } else if (constraintStart())
      col.constraints.push_back(constraintSpec(col.name));
    else
      break;
  }
  if (col.defaultValue && col.generated)
    fail("generated column cannot also have DEFAULT");
  if (context_.dialect == Dialect::MySQL && col.defaultValue && col.type.name.size() == 1) {
    const auto &value = context_.ast.expression(col.defaultValue);
    const auto type = asciiUpper(context_.ast.identifier(col.type.name.front().identifier));
    static const std::unordered_set<std::string> expressionTypes{
        "TINYBLOB", "BLOB", "MEDIUMBLOB", "LONGBLOB", "TINYTEXT", "TEXT",
        "MEDIUMTEXT", "LONGTEXT", "JSON", "GEOMETRY", "POINT", "LINESTRING",
        "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON", "GEOMETRYCOLLECTION"};
    if (!col.defaultParenthesized && value.kind != ExprKind::Null && expressionTypes.count(type)) {
      context_.report(DiagnosticCode::ExpectedToken, value.range,
                      type + " DEFAULT requires a parenthesized expression or NULL in MySQL");
      throw ParseFailure{};
    }
    if (!col.defaultParenthesized && value.kind == ExprKind::Null && col.nullable == false) {
      context_.report(DiagnosticCode::ExpectedToken, value.range,
                      "NOT NULL MySQL column cannot have a literal NULL default");
      throw ParseFailure{};
    }
  }
  col.origin.end = previous().range.end;
  return col;
}
void Parser::mysqlPartition(DdlSyntax &ddl) {
  auto &partition = ddl.partition.emplace();
  partition.origin.begin = current().range.begin;
  expect("PARTITION");
  expect("BY");
  if (ddl.temporary)
    fail("MySQL temporary tables cannot be partitioned", DiagnosticCode::InvalidQueryShape);
  partition.linear = eat("LINEAR");
  if (eat("HASH")) partition.method = PartitionMethod::Hash;
  else if (eat("KEY")) partition.method = PartitionMethod::Key;
  else fail("MySQL partition method is outside the supported direct HASH / KEY subset",
            DiagnosticCode::UnsupportedSyntax);
  const auto positive = [&](std::uint32_t maximum, const char *description) {
    const auto &token = current();
    std::uint32_t value = 0;
    const auto *end = token.value.data() + token.value.size();
    const auto parsed = std::from_chars(token.value.data(), end, value);
    if (token.kind != TokenKind::Number || parsed.ec != std::errc{} || parsed.ptr != end ||
        value == 0 || value > maximum)
      fail(description, DiagnosticCode::InvalidQueryShape);
    take();
    return value;
  };
  if (partition.method == PartitionMethod::Key && eat("ALGORITHM")) {
    expect("=");
    partition.algorithm = positive(2, "KEY partition ALGORITHM requires 1 or 2");
  }
  expect("(");
  if (partition.method == PartitionMethod::Hash) {
    partition.columns.push_back(namePart());
    if (!at(")")) fail("HASH partitioning currently requires a direct integer column",
                       DiagnosticCode::UnsupportedSyntax);
  } else if (!at(")")) {
    do {
      if (partition.columns.size() >= context_.options.limits.columnsPerQuery)
        fail("partition column limit exceeded", DiagnosticCode::ResourceLimit);
      partition.columns.push_back(namePart());
    } while (eat(","));
  }
  expect(")");
  if (eat("PARTITIONS"))
    partition.count = positive(8192, "PARTITIONS requires an integer between 1 and 8192");
  if (at("SUBPARTITION") || at("("))
    fail("explicit partition definitions and subpartitions are not covered", DiagnosticCode::UnsupportedSyntax);
  partition.origin.end = previous().range.end;
}
std::uint64_t Parser::ddlUnsignedInteger() {
  const auto &token = current();
  std::uint64_t value = 0;
  const auto *end = token.value.data() + token.value.size();
  const auto parsed = std::from_chars(token.value.data(), end, value);
  if (token.kind != TokenKind::Number || parsed.ec != std::errc{} || parsed.ptr != end)
    fail("expected an unsigned integer");
  take();
  return value;
}

bool Parser::distributionOption(DdlSyntax &d, bool alter) {
  const auto grammar = ddlGrammar();
  const auto begin = current().range.begin;
  const bool setGroup = alter && at("SET") && at("TABLEGROUP", 1);
  using Option = DistributionOptionKind;
  const auto kind = [&]() -> std::optional<Option> {
    if (at("TABLEGROUP") || setGroup) return Option::TableGroup;
    if (at("PLACEMENT")) return Option::PlacementPolicy;
    if (at("SHARD_ROW_ID_BITS")) return Option::ShardRowIdBits;
    if (at("PRE_SPLIT_REGIONS")) return Option::PreSplitRegions;
    if (at("AUTO_RANDOM_BASE")) return Option::AutoRandomBase;
    return {};
  }();
  if (!kind) return false;
  if (d.object != ObjectKind::Table ||
      (*kind == Option::TableGroup ? !grammar.oceanbase : !grammar.tidb))
    fail("distribution option is unavailable for this object or dialect",
         DiagnosticCode::UnsupportedSyntax);
  if (alter && *kind == Option::PreSplitRegions)
    fail("PRE_SPLIT_REGIONS applies at table creation", DiagnosticCode::UnsupportedSyntax);
  syntaxNode();
  if (setGroup) expect("SET");
  take();
  if (*kind == Option::PlacementPolicy) expect("POLICY");
  DistributionOption option{*kind, {}, {begin, 0}};
  if (*kind == Option::PlacementPolicy && eat("SET")) {
    expect("DEFAULT");
  } else {
    eat("=");
    if (*kind == Option::TableGroup && current().kind == TokenKind::String &&
        current().value.empty()) {
      take();
    } else if (*kind == Option::PlacementPolicy && eat("DEFAULT")) {
      // DEFAULT refers to the effective inherited placement policy.
    } else if (*kind == Option::TableGroup || *kind == Option::PlacementPolicy) {
      const auto name = namePart();
      option.value = canonicalIdentifier(context_.dialect,
          context_.ast.identifier(name.identifier), name.quoted);
    } else {
      option.value = ddlUnsignedInteger();
    }
  }
  option.origin.end = previous().range.end;
  d.distribution.push_back(std::move(option));
  return true;
}

void Parser::tableGroupSharding(DdlSyntax &d) {
  if (!at("SHARDING")) return;
  const auto begin = take().range.begin;
  expect("=");
  if (current().kind != TokenKind::String)
    fail("SHARDING requires a quoted mode");
  const auto mode = asciiUpper(take().value);
  d.distribution.push_back({DistributionOptionKind::Sharding, mode,
                            {begin, previous().range.end}});
}

bool Parser::vendorTableOption(DdlSyntax &d) {
  if (d.object != ObjectKind::Table) return false;
  const auto extension = context_.options.extension;
  auto positiveCount = [&]() {
    const auto token = current();
    std::uint32_t count = 0;
    const auto end = token.value.data() + token.value.size();
    const auto parsed = std::from_chars(token.value.data(), end, count);
    if (token.kind != TokenKind::Number || parsed.ec != std::errc{} || parsed.ptr != end || !count)
      fail("expected a positive integer count");
    take();
    return count;
  };
  if (extension == SqlExtension::YugabyteDb && eat("SPLIT")) {
    if (d.tabletCount) fail("duplicate SPLIT INTO clause");
    expect("INTO");
    d.tabletCount = positiveCount();
    expect("TABLETS");
    return true;
  }
  if (!isOlapExtension(extension)) return false;
  auto layout = [&]() -> BasicOlapLayout<NamePart> & {
    if (!d.olap) d.olap.emplace();
    return *d.olap;
  };
  const auto begin = current().range.begin;
  OlapKeyKind key = OlapKeyKind::Unspecified;
  if (eat("DUPLICATE")) key = OlapKeyKind::Duplicate;
  else if (eat("AGGREGATE")) key = OlapKeyKind::Aggregate;
  else if (eat("UNIQUE")) key = OlapKeyKind::Unique;
  else if (extension == SqlExtension::StarRocks && eat("PRIMARY")) key = OlapKeyKind::Primary;
  if (key != OlapKeyKind::Unspecified) {
    auto &value = layout();
    if (value.key != OlapKeyKind::Unspecified) fail("duplicate OLAP key clause");
    value.key = key;
    expect("KEY");
    columnList(value.keyColumns);
    value.keyOrigin = {begin, previous().range.end};
    return true;
  }
  if (eat("DISTRIBUTED")) {
    auto &value = layout();
    if (value.distribution != OlapDistribution::Unspecified) fail("duplicate DISTRIBUTED BY clause");
    expect("BY");
    if (eat("HASH")) {
      value.distribution = OlapDistribution::Hash;
      columnList(value.distributionColumns);
    } else {
      expect("RANDOM");
      value.distribution = OlapDistribution::Random;
    }
    if (eat("BUCKETS")) {
      value.automaticBuckets = extension == SqlExtension::Doris && eat("AUTO");
      if (!value.automaticBuckets) value.buckets = positiveCount();
    }
    value.distributionOrigin = {begin, previous().range.end};
    return true;
  }
  if (eat("ORDER")) {
    auto &value = layout();
    if (!value.sortColumns.empty()) fail("duplicate table ORDER BY clause");
    expect("BY");
    columnList(value.sortColumns);
    value.sortOrigin = {begin, previous().range.end};
    return true;
  }
  if (eat("PROPERTIES")) {
    auto &properties = layout().properties;
    if (!properties.empty()) fail("duplicate PROPERTIES clause");
    std::unordered_set<std::string> names;
    expect("(");
    do {
      syntaxNode();
      if (current().kind != TokenKind::String) fail("property name must be quoted");
      auto name = take().value;
      if (name.empty() || !names.insert(asciiUpper(name)).second) fail("empty or duplicate table property");
      expect("=");
      if (current().kind != TokenKind::String) fail("property value must be quoted");
      properties.emplace_back(std::move(name), take().value);
    } while (eat(","));
    expect(")");
    return true;
  }
  return false;
}

void Parser::tableOptions(DdlSyntax &d) {
  const auto g = ddlGrammar();
  bool afterComma = false;
  while (true) {
    if (vendorTableOption(d) || distributionOption(d)) {
      // The option was recorded with its source range.
    } else if (g.mysql && eat("ENGINE")) {
      eat("=");
      auto n = namePart();
      d.engine = context_.ast.identifier(n.identifier);
    } else if (g.mysql && (at("DEFAULT") || at("CHARACTER") || at("CHARSET") ||
                           at("COLLATE"))) {
      eat("DEFAULT");
      if (eat("COLLATE")) {
        eat("=");
        auto n = namePart();
        d.collation = context_.ast.identifier(n.identifier);
      } else {
        if (!eat("CHARSET")) {
          expect("CHARACTER");
          expect("SET");
        }
        eat("=");
        auto n = namePart();
        d.characterSet = context_.ast.identifier(n.identifier);
      }
    } else if (g.mysql && eat("COMMENT")) {
      eat("=");
      if (current().kind != TokenKind::String)
        fail("table COMMENT requires a string literal");
      d.tableComment = take().value;
    } else if ((g.oracle || g.postgres) && eat("TABLESPACE")) {
      auto n = namePart();
      d.tablespace = context_.ast.identifier(n.identifier);
    } else if ((g.oracle || g.postgres) && eat("ON")) {
      expect("COMMIT");
      if (eat("PRESERVE")) {
        expect("ROWS");
        d.onCommit = "PRESERVE ROWS";
      } else if (eat("DELETE")) {
        expect("ROWS");
        d.onCommit = "DELETE ROWS";
      } else {
        expect("DROP");
        if (!g.postgres)
          fail("ON COMMIT DROP is PostgreSQL-specific");
        d.onCommit = "DROP";
      }
    } else {
      if (afterComma) fail("expected table option after ','");
      break;
    }
    afterComma = g.tidb && eat(",");
  }
}
void Parser::sequenceOptions(DdlSyntax &d, bool alter) {
  const auto g = ddlGrammar();
  if (g.sqlite || (g.mysql && context_.dialect != Dialect::MariaDB))
    fail("sequences are unavailable in this dialect",
         DiagnosticCode::UnsupportedSyntax);
  while (true) {
    SequenceOptionKind kind;
    bool value = true;
    if (eat("START")) {
      if (g.sqlserver && alter) fail("ALTER SEQUENCE uses RESTART, not START");
      if (g.sqlserver) expect("WITH"); else eat("WITH");
      kind = SequenceOptionKind::Start;
    } else if (eat("INCREMENT")) {
      if (g.sqlserver) expect("BY"); else eat("BY");
      kind = SequenceOptionKind::Increment;
    } else if (eat("MINVALUE"))
      kind = SequenceOptionKind::Minimum;
    else if (eat("MAXVALUE"))
      kind = SequenceOptionKind::Maximum;
    else if (eat("CACHE"))
      kind = SequenceOptionKind::Cache;
    else if (eat("CYCLE")) {
      kind = SequenceOptionKind::Cycle;
      value = false;
    } else if (eat("NO")) {
      value = false;
      if (eat("MINVALUE"))
        kind = SequenceOptionKind::NoMinimum;
      else if (eat("MAXVALUE"))
        kind = SequenceOptionKind::NoMaximum;
      else if (g.sqlserver && eat("CACHE"))
        kind = SequenceOptionKind::NoCache;
      else {
        expect("CYCLE");
        kind = SequenceOptionKind::NoCycle;
      }
    } else if (g.oracle && eat("NOCACHE")) {
      kind = SequenceOptionKind::NoCache;
      value = false;
    } else if (g.oracle && eat("NOCYCLE")) {
      kind = SequenceOptionKind::NoCycle;
      value = false;
    } else if (g.oracle && eat("NOMINVALUE")) {
      kind = SequenceOptionKind::NoMinimum;
      value = false;
    } else if (g.oracle && eat("NOMAXVALUE")) {
      kind = SequenceOptionKind::NoMaximum;
      value = false;
    } else
      break;
    const auto group=[](SequenceOptionKind option) {
      if (option==SequenceOptionKind::NoMinimum) return SequenceOptionKind::Minimum;
      if (option==SequenceOptionKind::NoMaximum) return SequenceOptionKind::Maximum;
      if (option==SequenceOptionKind::NoCache) return SequenceOptionKind::Cache;
      if (option==SequenceOptionKind::NoCycle) return SequenceOptionKind::Cycle;
      return option;
    };
    for (const auto &option : d.sequenceOptions)
      if (option.first == kind || (g.sqlserver && group(option.first)==group(kind)))
        fail("duplicate sequence option");
    if (g.sqlserver && kind==SequenceOptionKind::Cache &&
        current().kind!=TokenKind::Number && !at("+") && !at("-")) value=false;
    d.sequenceOptions.push_back({kind, value ? expression() : 0});
  }
}
const char *mysqlViewAlgorithmName(MySqlViewAlgorithm algorithm) {
  switch (algorithm) {
  case MySqlViewAlgorithm::Undefined: return "UNDEFINED";
  case MySqlViewAlgorithm::Merge: return "MERGE";
  case MySqlViewAlgorithm::TempTable: return "TEMPTABLE";
  case MySqlViewAlgorithm::Unspecified: return "";
  }
  return "";
}
const char *mysqlViewSecurityName(MySqlViewSecurity security) {
  switch (security) {
  case MySqlViewSecurity::Definer: return "DEFINER";
  case MySqlViewSecurity::Invoker: return "INVOKER";
  case MySqlViewSecurity::Unspecified: return "";
  }
  return "";
}
const char *mysqlLoadableResultName(MySqlLoadableResult result) {
  switch (result) {
  case MySqlLoadableResult::String: return "STRING";
  case MySqlLoadableResult::Integer: return "INTEGER";
  case MySqlLoadableResult::Real: return "REAL";
  case MySqlLoadableResult::Decimal: return "DECIMAL";
  }
  return "";
}
bool mysqlLoadableFunctionAhead(const std::vector<Token> &tokens,std::size_t at) {
  const auto keyword=[&](std::size_t position,std::string_view value) {
    return position<tokens.size() && tokens[position].kind==TokenKind::Word &&
           asciiUpper(tokens[position].value)==value;
  };
  if (keyword(at,"AGGREGATE")) return true;
  if (!keyword(at++,"FUNCTION")) return false;
  if (keyword(at,"IF") && keyword(at+1,"NOT") && keyword(at+2,"EXISTS")) at+=3;
  return at<tokens.size() && (tokens[at].kind==TokenKind::Word || tokens[at].kind==TokenKind::Identifier) &&
         keyword(at+1,"RETURNS");
}
TriggerDefiner Parser::storedObjectDefiner() {
  expect("=");
  TriggerDefiner definer;
  if (eat("CURRENT_USER")) {
    definer.currentUser = true;
    if (eat("(")) expect(")");
  } else {
    const auto accountPart = [&] {
      if (current().kind != TokenKind::Word && current().kind != TokenKind::Identifier &&
          current().kind != TokenKind::String)
        fail("expected a MySQL account name or host");
      auto value = take().value;
      if (value.size() > 512)
        fail("account component is too long", DiagnosticCode::ResourceLimit);
      return value;
    };
    definer.user = accountPart();
    definer.host = eat("@") ? accountPart() : "%";
  }
  return definer;
}
void Parser::createDdl(Statement &s, DdlSyntax &d) {
  const auto g = ddlGrammar();
  if (eat("VIRTUAL")) {
    createVirtualTable(d);
    return;
  }
  if (eat("OR")) {
    expect("REPLACE");
    d.orReplace = true;
  }
  if (g.mysql && mysqlLoadableFunctionAhead(tokens_,cursor_)) {
    const auto dialect=context_.dialect;
    if (dialect!=Dialect::MySQL && dialect!=Dialect::MariaDB)
      fail("loadable functions require MySQL or MariaDB",DiagnosticCode::UnsupportedSyntax);
    if (d.orReplace && dialect!=Dialect::MariaDB)
      fail("OR REPLACE for loadable functions is MariaDB-specific");
    d.object=ObjectKind::LoadableFunction;
    auto &function=d.mysqlLoadableFunction.emplace();
    function.aggregate=eat("AGGREGATE");
    expect("FUNCTION");
    ifNotExists(d);
    if (d.ifNotExists && d.orReplace) fail("OR REPLACE cannot be combined with IF NOT EXISTS");
    d.name.push_back(namePart());
    expect("RETURNS");
    if (eat("STRING")) function.result=MySqlLoadableResult::String;
    else if (eat("INTEGER") || eat("INT")) function.result=MySqlLoadableResult::Integer;
    else if (eat("REAL")) function.result=MySqlLoadableResult::Real;
    else if (eat("DECIMAL")) function.result=MySqlLoadableResult::Decimal;
    else fail("loadable functions return STRING, INTEGER, REAL, or DECIMAL");
    expect("SONAME");
    if (current().kind!=TokenKind::String) fail("SONAME requires a string literal");
    function.library=take().value;
    return;
  }
  if ((context_.dialect == Dialect::MySQL || context_.dialect == Dialect::MariaDB) &&
      (at("ALGORITHM") || at("DEFINER") || at("SQL"))) {
    auto &header = d.mysqlViewOptions.emplace();
    if (eat("ALGORITHM")) {
      expect("=");
      if (eat("UNDEFINED")) header.algorithm = MySqlViewAlgorithm::Undefined;
      else if (eat("MERGE")) header.algorithm = MySqlViewAlgorithm::Merge;
      else {
        expect("TEMPTABLE");
        header.algorithm = MySqlViewAlgorithm::TempTable;
      }
    }
    if (eat("DEFINER")) header.definer = storedObjectDefiner();
    if (eat("SQL")) {
      expect("SECURITY");
      if (eat("DEFINER")) header.security = MySqlViewSecurity::Definer;
      else {
        expect("INVOKER");
        header.security = MySqlViewSecurity::Invoker;
      }
    }
    if (!at("VIEW")) fail("MySQL view options must be followed by VIEW");
  }
  if (eat("GLOBAL")) {
    expect("TEMPORARY");
    d.temporary = d.globalTemporary = true;
    if (!g.oracle && !g.postgres)
      fail("GLOBAL TEMPORARY is unavailable in this dialect");
  } else if (eat("TEMPORARY") || eat("TEMP")) {
    d.temporary = true;
    if (g.sqlserver || g.oracle)
      fail("temporary object keyword is unavailable in this dialect baseline");
  } else if (eat("UNLOGGED")) {
    if (!g.postgres)
      fail("UNLOGGED is PostgreSQL-specific");
    d.unlogged = true;
  }
  d.unique = eat("UNIQUE");
  d.object = objectKind();
  if (g.postgres && d.object==ObjectKind::Index && at("CONCURRENTLY"))
    fail("concurrent index creation requires separate transaction/deployment semantics",DiagnosticCode::UnsupportedSyntax);
  ifNotExists(d);
  if (g.postgres && d.object==ObjectKind::Index) {
    // IF NOT EXISTS requires a name; ordinary PostgreSQL indexes may let the
    // server choose it. CREATE INDEX names themselves are never qualified.
    if (d.ifNotExists || !at("ON")) d.name.push_back(namePart());
  } else
    d.name = qualifiedName();
  if (g.sqlserver && (d.object==ObjectKind::Sequence || d.object==ObjectKind::View) && d.name.size()>2)
    fail("SQL Server sequence/view names permit at most a schema qualifier");
  if (g.sqlserver && d.object==ObjectKind::View &&
      context_.ast.identifier(d.name.back().identifier).find('#')==0)
    fail("SQL Server views cannot have temporary names");
  if (d.ifNotExists && ((d.object == ObjectKind::View && !g.sqlite) ||
                        (d.object == ObjectKind::Index && g.mysql &&
                         context_.dialect != Dialect::MariaDB) ||
                        (d.object == ObjectKind::Database && g.postgres)))
    fail("IF NOT EXISTS is unavailable for this object in this dialect");
  if (d.orReplace &&
      (d.object != ObjectKind::View || (!g.mysql && !g.oracle && !g.postgres)))
    fail("OR REPLACE is unsupported for this DDL object");
  if (d.unique && d.object != ObjectKind::Index)
    fail("UNIQUE prefix requires INDEX");
  if ((d.temporary || d.unlogged) && d.object != ObjectKind::Table &&
      !(g.sqlite && d.object == ObjectKind::View))
    fail("temporary/unlogged prefix is unsupported for this object");
  if (d.object == ObjectKind::TableGroup) {
    if (d.name.size() != 1) fail("TABLEGROUP names are unqualified");
    tableGroupSharding(d);
    return;
  }
  if (d.object == ObjectKind::Table) {
    bool definitionList=false;
    if (eat("LIKE")) {
      if (!g.mysql)
        fail("CREATE TABLE LIKE is MySQL-specific in this grammar");
      d.likeTable = qualifiedName();
      return;
    }
    if (eat("(")) {
      definitionList=true;
      if (at(")") && !g.postgres)
        fail("CREATE TABLE requires a column or constraint");
      if (!at(")")) do {
        if (g.mysql && (at("INDEX") || at("KEY") || (at("UNIQUE") && (at("INDEX",1) || at("KEY",1)))))
          d.tableIndexes.push_back(tableIndex());
        else if (constraintStart())
          d.constraints.push_back(constraintSpec());
        else
          d.definitions.push_back(columnSpec());
      } while (eat(","));
      expect(")");
    }
    tableOptions(d);
    if ((context_.dialect == Dialect::MySQL || g.tidb || g.oceanbase) && at("PARTITION")) {
      if (d.definitions.empty())
        fail("partitioned CREATE TABLE needs declared columns", DiagnosticCode::UnsupportedSyntax);
      mysqlPartition(d);
    }
    if (g.postgres && definitionList && (at("AS") || at("SELECT") || at("WITH")))
      fail("PostgreSQL CREATE TABLE AS uses an optional name list, not typed column definitions");
    if (eat("AS") || at("SELECT") || at("WITH")) {
      if (g.sqlserver)
        fail("CREATE TABLE AS SELECT is unavailable in this SQL Server grammar",
             DiagnosticCode::UnsupportedSyntax);
      s.source = storeQuery(select(false, true));
      if (g.postgres && eat("WITH")) {
        expect("NO");
        expect("DATA");
        d.withNoData = true;
      }
    } else if (d.definitions.empty() && d.constraints.empty() && !(g.postgres && definitionList))
      fail("CREATE TABLE requires a definition or SELECT");
    return;
  }
  if (d.object == ObjectKind::View || d.object == ObjectKind::MaterializedView) {
    if (at("("))
      columnList(d.columns);
    expect("AS");
    auto source=select(false,true);
    if (g.sqlserver && !source.orderBy.empty() && source.limit.syntax==LimitSyntax::None)
      fail("ORDER BY in a SQL Server view requires TOP or OFFSET");
    s.source = storeQuery(std::move(source));
    if(d.object==ObjectKind::MaterializedView) {
      if(eat("WITH")){if(!g.postgres)fail("materialized WITH DATA is PostgreSQL-specific");d.withNoData=eat("NO");expect("DATA");}
      return;
    }
    if (eat("WITH")) {
      if (g.sqlite)
        fail("SQLite views have no CHECK OPTION");
      if (eat("LOCAL")) {
        if (!g.postgres && !g.mysql)
          fail("LOCAL view check is unavailable in this dialect");
        d.viewCheck = "LOCAL";
      } else if (eat("CASCADED")) {
        if (!g.postgres && !g.mysql)
          fail("CASCADED view check is unavailable in this dialect");
        d.viewCheck = "CASCADED";
      } else
        d.viewCheck = "DEFAULT";
      expect("CHECK");
      expect("OPTION");
    }
    return;
  }
  if (d.object == ObjectKind::Index) {
    expect("ON");
    d.indexedTable = qualifiedName();
    if (g.postgres && eat("USING"))
      d.indexMethod.push_back(namePart());
    expect("(");
    do {
      IndexElement key;
      if (g.sqlserver) {
        const auto column=namePart();
        key.expression=node({ExprKind::Name,column.range,"",{column},{}});
      } else if (g.postgres) {
        // index_elem is ColId, func_expr_windowless, or '(' a_expr ')'.
        // Do not consume a full unparenthesized expression here: arithmetic,
        // indirection, casts and window/filter suffixes need their own parens.
        if (eat("(")) {
          key.expression=expression();
          expect(")");
        } else {
          key.expression=primary();
          const auto &value=context_.ast.expression(key.expression);
          if (!((value.kind==ExprKind::Name && value.name.size()==1) ||
                value.kind==ExprKind::Call || value.kind==ExprKind::Cast ||
                value.kind==ExprKind::Extract))
            fail("PostgreSQL index expressions require parentheses");
        }
      } else if (g.mysql && nameToken() && at("(", 1) &&
          current().kind != TokenKind::End &&
          tokens_[std::min(cursor_ + 2, tokens_.size() - 1)].kind ==
              TokenKind::Number &&
          at(")", 3)) {
        const auto begin = current().range.begin;
        auto col = qualifiedName();
        key.expression = node({ExprKind::Name,
                               {begin, previous().range.end},
                               "",
                               std::move(col),
                               {}});
        expect("(");
        key.prefixLength = limitValue();
        expect(")");
      } else
        key.expression = expression();
      requireScalar(key.expression);
      key.descending = eat("DESC");
      if (!key.descending)
        eat("ASC");
      if (eat("NULLS")) {
        if (!nullOrdering())
          fail("NULLS ordering is unavailable for this index dialect");
        if (eat("FIRST"))
          key.nulls = "FIRST";
        else {
          expect("LAST");
          key.nulls = "LAST";
        }
      }
      d.keys.push_back(key);
    } while (eat(","));
    expect(")");
    if (g.oceanbase && (at("LOCAL") || at("GLOBAL"))) {
      d.indexScope = eat("LOCAL") ? IndexScope::Local : IndexScope::Global;
      if (d.indexScope == IndexScope::Global) expect("GLOBAL");
    }
    if (g.sqlserver || g.postgres) {
      if (eat("INCLUDE"))
        columnList(d.includeColumns);
    }
    if (eat("WHERE")) {
      if (!g.postgres && !g.sqlserver && !g.sqlite)
        fail("partial indexes are unavailable in this dialect");
      d.predicate = expression();
      requireScalar(d.predicate);
      if (g.sqlserver && !validSqlServerIndexFilter(d.predicate,
          [&](ExprId id)->const Expr & {return context_.ast.expression(id);}))
        fail("SQL Server filtered indexes require column predicates joined by AND and literal comparison values with at most one CAST/CONVERT");
    }
    return;
  }
  if (d.object == ObjectKind::Sequence) {
    sequenceOptions(d);
    return;
  }
  if (g.oracle || g.sqlite)
    fail("administrative schema/database creation requires a separate grammar "
         "in this dialect",
         DiagnosticCode::UnsupportedSyntax);
  if (g.mysql)
    tableOptions(d);
}
void Parser::alterDdl(Statement &, DdlSyntax &d) {
  const auto g = ddlGrammar();
  const bool maria = context_.dialect == Dialect::MariaDB;
  if (eat("TEMPORARY")) {
    if (!g.mysql)
      fail("DROP TEMPORARY is MySQL-specific");
    d.temporary = true;
  }
  d.object = objectKind();
  ifExists(d);
  if ((g.sqlite &&
       (d.object == ObjectKind::Schema || d.object == ObjectKind::Database ||
        d.object == ObjectKind::Sequence)) ||
      (g.oracle && d.object == ObjectKind::Schema) ||
      (g.mysql && d.object == ObjectKind::Sequence &&
       context_.dialect != Dialect::MariaDB))
    fail("object kind is unavailable for DROP in this dialect");
  if (d.ifExists && !g.postgres && !(maria && d.object == ObjectKind::Table))
    fail("ALTER IF EXISTS is unavailable for this object in this dialect");
  d.name = qualifiedName();
  if (d.object == ObjectKind::Sequence) {
    if (g.sqlserver && d.name.size()>2)
      fail("SQL Server sequence names permit at most a schema qualifier");
    sequenceOptions(d,true);
    return;
  }
  if (d.object != ObjectKind::Table)
    fail("ALTER for this object is not implemented yet",
         DiagnosticCode::UnsupportedSyntax);
  if (distributionOption(d, true)) return;
  do {
    syntaxNode();
    BasicAlterAction<NamePart, QualifiedName> action;
    action.origin.begin = current().range.begin;
    if (eat("ADD")) {
      const bool columnKeyword = eat("COLUMN");
      if (eat("IF")) {
        if (!g.postgres && !maria && !(g.tidb && columnKeyword))
          fail("ADD COLUMN IF NOT EXISTS is unavailable here in this dialect");
        expect("NOT");
        expect("EXISTS");
        action.ifNotExists = true;
      }
      if (eat("(")) {
        if (action.ifNotExists)
          fail("guarded grouped ADD columns are not implemented",
               DiagnosticCode::UnsupportedSyntax);
        if (!g.oracle && !g.mysql)
          fail("grouped ADD columns are unavailable in this dialect");
        do {
          syntaxNode();
          BasicAlterAction<NamePart, QualifiedName> grouped;
          grouped.origin.begin = current().range.begin;
          if (constraintStart()) {
            grouped.kind = AlterKind::AddConstraint;
            grouped.constraint = constraintSpec();
          } else {
            grouped.kind = AlterKind::AddColumn;
            grouped.column = columnSpec();
          }
          grouped.origin.end = previous().range.end;
          d.actions.push_back(std::move(grouped));
        } while (eat(","));
        expect(")");
        continue;
      }
      if (constraintStart()) {
        if (action.ifNotExists)
          fail("ADD COLUMN IF NOT EXISTS requires a column declaration");
        action.kind = AlterKind::AddConstraint;
        action.constraint = constraintSpec();
      } else {
        action.kind = AlterKind::AddColumn;
        action.column = columnSpec();
      }
    } else if (eat("DROP")) {
      if (eat("CONSTRAINT")) {
        action.kind = AlterKind::DropConstraint;
        action.name = namePart();
      } else if (eat("PRIMARY")) {
        if (!g.mysql)
          fail("DROP PRIMARY KEY is MySQL-specific");
        expect("KEY");
        action.kind = AlterKind::DropConstraint;
        BasicConstraint<NamePart, QualifiedName> key;
        key.kind = ConstraintKind::PrimaryKey;
        action.constraint = key;
      } else {
        eat("COLUMN");
        action.kind = AlterKind::DropColumn;
        if (eat("IF")) {
          expect("EXISTS");
          if (!g.postgres && !g.sqlserver && !maria && !g.tidb)
            fail("DROP COLUMN IF EXISTS is unavailable in this dialect");
          action.ifExists = true;
        }
        action.name = namePart();
      }
    } else if (eat("RENAME")) {
      if (g.sqlserver)
        fail("SQL Server uses sp_rename rather than ALTER TABLE RENAME",
             DiagnosticCode::UnsupportedSyntax);
      if (eat("COLUMN") || (g.postgres && !at("TO"))) {
        action.kind = AlterKind::RenameColumn;
        action.name = namePart();
        expect("TO");
        action.newName = namePart();
      } else {
        action.kind = AlterKind::RenameTable;
        expect("TO");
        action.newName = namePart();
      }
    } else if (eat("MODIFY")) {
      if (!g.mysql && !g.oracle)
        fail("MODIFY COLUMN is unavailable in this dialect");
      eat("COLUMN");
      action.kind = AlterKind::ModifyColumn;
      if (eat("(")) {
        if (!g.oracle)
          fail("grouped MODIFY is Oracle-specific in this baseline");
        do {
          syntaxNode();
          BasicAlterAction<NamePart, QualifiedName> grouped;
          grouped.kind = AlterKind::ModifyColumn;
          grouped.origin.begin = current().range.begin;
          grouped.column = columnSpec(true);
          grouped.origin.end = previous().range.end;
          d.actions.push_back(std::move(grouped));
        } while (eat(","));
        expect(")");
        continue;
      }
      action.column = columnSpec(g.oracle);
    } else if (eat("CHANGE")) {
      if (!g.mysql)
        fail("CHANGE COLUMN is MySQL-specific");
      eat("COLUMN");
      action.kind = AlterKind::ChangeColumn;
      action.name = namePart();
      action.column = columnSpec();
    } else if (eat("ALTER")) {
      eat("COLUMN");
      action.name = namePart();
      if (g.sqlserver) {
        action.kind = AlterKind::AlterType;
        action.type = typeSpec();
        if (eat("NOT")) {
          expect("NULL");
          action.nullable = false;
        } else if (eat("NULL"))
          action.nullable = true;
      } else if (at("TYPE") || (g.postgres && at("SET") && at("DATA", 1))) {
        if (!g.postgres)
          fail("ALTER COLUMN TYPE is PostgreSQL-specific");
        if (eat("SET"))
          expect("DATA");
        expect("TYPE");
        action.kind = AlterKind::AlterType;
        action.type = typeSpec();
        if (eat("USING"))
          action.value = expression();
      } else if (eat("SET")) {
        if (eat("DEFAULT")) {
          action.kind = AlterKind::SetDefault;
          action.defaultParenthesized = (context_.dialect == Dialect::MySQL || g.tidb) && eat("(");
          action.value = expression();
          if (action.defaultParenthesized) expect(")");
          if (g.tidb && !action.defaultParenthesized) {
            const auto &value=context_.ast.expression(action.value);
            const bool literal=value.kind==ExprKind::Number || value.kind==ExprKind::String ||
              value.kind==ExprKind::Null || value.kind==ExprKind::Boolean;
            const bool signedNumber=value.kind==ExprKind::Unary && value.operands.size()==1 &&
              (value.value=="+" || value.value=="-") && context_.ast.expression(value.operands.front()).kind==ExprKind::Number;
            if (!literal && !signedNumber)
              fail("TiDB ALTER COLUMN DEFAULT expressions require parentheses");
          }
        } else {
          expect("NOT");
          expect("NULL");
          if (!g.postgres)
            fail("SET NOT NULL is PostgreSQL-specific");
          action.kind = AlterKind::SetNotNull;
        }
      } else if (eat("DROP")) {
        if (eat("DEFAULT"))
          action.kind = AlterKind::DropDefault;
        else {
          expect("NOT");
          expect("NULL");
          if (!g.postgres)
            fail("DROP NOT NULL is PostgreSQL-specific");
          action.kind = AlterKind::DropNotNull;
        }
      } else
        fail("expected ALTER COLUMN action");
    } else
      fail("unsupported ALTER TABLE action", DiagnosticCode::UnsupportedSyntax);
    if (eat("CASCADE")) {
      if (!g.postgres || (action.kind != AlterKind::DropColumn &&
                          action.kind != AlterKind::DropConstraint))
        fail("CASCADE is unavailable for this ALTER action");
      action.dropBehavior = DropBehavior::Cascade;
    } else if (eat("RESTRICT")) {
      if (!g.postgres || (action.kind != AlterKind::DropColumn &&
                          action.kind != AlterKind::DropConstraint))
        fail("RESTRICT is unavailable for this ALTER action");
      action.dropBehavior = DropBehavior::Restrict;
    }
    action.origin.end = previous().range.end;
    d.actions.push_back(std::move(action));
  } while (eat(","));
}
void Parser::dropDdl(DdlSyntax &d) {
  const auto g = ddlGrammar();
  if (eat("TEMPORARY")) {
    if (!g.mysql)
      fail("DROP TEMPORARY is MySQL-specific");
    d.temporary = true;
  }
  d.object = objectKind();
  ifExists(d);
  if ((g.sqlite &&
       (d.object == ObjectKind::Schema || d.object == ObjectKind::Database ||
        d.object == ObjectKind::Sequence)) ||
      (g.oracle && d.object == ObjectKind::Schema) ||
      (g.mysql && d.object == ObjectKind::Sequence &&
       context_.dialect != Dialect::MariaDB))
    fail("object kind is unavailable for DROP in this dialect");
  if (d.object == ObjectKind::TableGroup) {
    if (d.temporary) fail("DROP TEMPORARY requires TABLE");
    d.name = qualifiedName();
    if (d.name.size() != 1) fail("TABLEGROUP names are unqualified");
    d.names = {d.name};
    return;
  }
  do {
    syntaxNode();
    d.names.push_back(qualifiedName());
    if ((g.oracle || g.sqlite) && at(","))
      fail("multiple DROP targets are unavailable in this dialect");
  } while (eat(","));
  d.name = d.names.front();
  if (g.sqlserver && d.object==ObjectKind::Sequence)
    for (const auto &name:d.names) if (name.size()>2)
      fail("SQL Server sequence names permit at most a schema qualifier");
  if (d.temporary && d.object != ObjectKind::Table)
    fail("DROP TEMPORARY requires TABLE");
  if (d.object == ObjectKind::Index && (g.mysql || g.sqlserver)) {
    if (d.names.size() != 1)
      fail("multiple DROP INDEX targets require separate ON clauses",
           DiagnosticCode::UnsupportedSyntax);
    expect("ON");
    d.indexedTable = qualifiedName();
  }
  if (eat("CASCADE")) {
    if (g.oracle) {
      expect("CONSTRAINTS");
      d.dropBehavior = DropBehavior::CascadeConstraints;
    } else
      d.dropBehavior = DropBehavior::Cascade;
  } else if (eat("RESTRICT"))
    d.dropBehavior = DropBehavior::Restrict;
  if (eat("PURGE")) {
    if (!g.oracle || d.object != ObjectKind::Table)
      fail("PURGE is Oracle DROP TABLE-specific");
    d.purge = true;
  }
}
void Parser::truncateDdl(DdlSyntax &d) {
  const auto g = ddlGrammar();
  if (g.sqlite)
    fail("SQLite has no TRUNCATE statement", DiagnosticCode::UnsupportedSyntax);
  if (g.sqlserver)
    expect("TABLE");
  else
    eat("TABLE");
  d.object = ObjectKind::Table;
  do {
    syntaxNode();
    BasicTruncateTarget<QualifiedName> target;
    if (eat("ONLY")) {
      if (!g.postgres)
        fail("ONLY is PostgreSQL-specific");
      target.only = true;
    }
    target.name = qualifiedName();
    if (eat("*")) {
      if (!g.postgres || target.only)
        fail("inheritance * requires a non-ONLY PostgreSQL target");
      target.descendants = true;
    }
    d.names.push_back(target.name);
    d.truncateTargets.push_back(std::move(target));
    if (!g.postgres && at(","))
      fail("multiple TRUNCATE targets are PostgreSQL-specific");
  } while (eat(","));
  d.truncateOnly = d.truncateTargets.front().only;
  d.truncateDescendants = d.truncateTargets.front().descendants;
  d.name = d.names.front();
  if (g.postgres) {
    if (eat("RESTART")) {
      expect("IDENTITY");
      d.restartIdentity = true;
    } else if (eat("CONTINUE")) {
      expect("IDENTITY");
      d.restartIdentity = false;
    }
    if (eat("CASCADE"))
      d.dropBehavior = DropBehavior::Cascade;
    else if (eat("RESTRICT"))
      d.dropBehavior = DropBehavior::Restrict;
  }
  if (g.oracle) {
    if (eat("REUSE")) {
      expect("STORAGE");
      d.reuseStorage = true;
    } else if (eat("DROP")) {
      expect("STORAGE");
      d.reuseStorage = false;
    }
  }
}
void Parser::renameDdl(DdlSyntax &d) {
  const auto g = ddlGrammar();
  if (!g.mysql && !g.oracle)
    fail("this dialect uses ALTER ... RENAME or sp_rename",
         DiagnosticCode::UnsupportedSyntax);
  if (g.mysql)
    expect("TABLE");
  d.object = ObjectKind::Table;
  do {
    syntaxNode();
    auto oldName = qualifiedName();
    expect("TO");
    auto newName = qualifiedName();
    d.renames.push_back({std::move(oldName), std::move(newName)});
    if (!g.mysql && at(","))
      fail("Oracle RENAME only accepts one object");
  } while (eat(","));
  d.name = d.renames.front().first;
}
Statement Parser::ddl() {
  syntaxNode();
  const auto grammar=ddlGrammar();
  if(grammar.oracle) {
    if(at("DROP") && at("PACKAGE",1))return packageStatement();
    if(at("CREATE")) {
      std::size_t next=1;
      if(at("OR",next) && at("REPLACE",next+1))next+=2;
      if(at("EDITIONABLE",next) || at("NONEDITIONABLE",next))++next;
      if(at("PACKAGE",next))return packageStatement();
    }
  }
  if(grammar.mysql) {
    if((at("DROP") || at("CREATE")) && at("TRIGGER",1))return triggerStatement();
    if(at("CREATE") && at("DEFINER",1))for(std::size_t look=4;look<=9;++look)
      if(at("TRIGGER",look))return triggerStatement();
  }
  if (grammar.sqlite && ((at("DROP") && at("TRIGGER", 1)) ||
      (at("CREATE") && (at("TRIGGER", 1) ||
       ((at("TEMP", 1) || at("TEMPORARY", 1)) && at("TRIGGER", 2))))))
    return triggerStatement();
  Statement s;
  DdlSyntax d;
  if (eat("CREATE")) {
    s.kind = StatementKind::Create;
    createDdl(s, d);
  } else if (eat("ALTER")) {
    s.kind = StatementKind::Alter;
    alterDdl(s, d);
  } else if (eat("DROP")) {
    s.kind = StatementKind::Drop;
    dropDdl(d);
  } else if (eat("TRUNCATE")) {
    s.kind = StatementKind::Truncate;
    truncateDdl(d);
  } else if (eat("RENAME")) {
    s.kind = StatementKind::Rename;
    renameDdl(d);
  } else {
    expect("COMMENT");
    if (!ddlGrammar().oracle && !ddlGrammar().postgres)
      fail("COMMENT ON is unavailable in this dialect");
    s.kind = StatementKind::Comment;
    expect("ON");
    d.object = objectKind();
    d.name = qualifiedName();
    expect("IS");
    if (current().kind != TokenKind::String && !at("NULL"))
      fail("COMMENT requires a literal string or NULL");
    d.commentValue = primary();
  }
  s.ddl = std::move(d);
  return s;
}
} // namespace navidog::sql::compiler
