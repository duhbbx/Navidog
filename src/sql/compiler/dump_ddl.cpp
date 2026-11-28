#include "navidog/sql/compiler/compiler.hpp"
#include <iomanip>
#include <ostream>
namespace navidog::sql::compiler {
namespace {
class DdlJson {
  std::ostream &out;
  const Compilation &c;

public:
  DdlJson(std::ostream &stream, const Compilation &compilation)
      : out(stream), c(compilation) {}
  void value(const std::string &text) {
    out << '"';
    for (unsigned char ch : text) {
      if (ch == '"' || ch == '\\')
        out << '\\' << char(ch);
      else if (ch < 32)
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(ch)
            << std::dec;
      else
        out << char(ch);
    }
    out << '"';
  }
  void value(const NamePart &p) {
    out << "{\"text\":";
    value(c.ast.identifier(p.identifier));
    out << ",\"quoted\":" << (p.quoted ? "true" : "false") << '}';
  }
  void value(bool b) { out << (b ? "true" : "false"); }
  template <class T> void value(const std::optional<T> &opt) {
    if (opt)
      value(*opt);
    else
      out << "null";
  }
  template <class T> void list(const std::vector<T> &values) {
    out << '[';
    bool comma = false;
    for (const auto &v : values) {
      if (comma)
        out << ',';
      comma = true;
      value(v);
    }
    out << ']';
  }
  void key(const char *name) {
    out << ',';
    value(std::string(name));
    out << ':';
  }
  void range(SourceRange origin) {
    out << '[' << origin.begin << ',' << origin.end << ']';
  }
  template <class Q> void type(const BasicTypeSpec<Q> &t) {
    out << "{\"name\":";
    list(t.name);
    key("nameQuoted");list(t.nameQuoted);
    key("arguments");
    out << '[';
    bool comma = false;
    for (const auto &a : t.arguments) {
      if (comma)
        out << ',';
      comma = true;
      out << "{\"kind\":" << int(a.kind) << ",\"value\":";
      value(a.value);
      out << '}';
    }
    out << ']';
    key("modifiers");
    list(t.modifiers);
    key("lengthSemantics");
    value(t.lengthSemantics);
    key("arrayDimensions");
    out << t.arrayDimensions;
    if(t.intervalType) {
      const auto &s=*t.intervalType;key("intervalType");out<<"{\"leading\":";value(std::string(intervalFieldName(s.leading)));
      key("trailing");value(std::string(intervalFieldName(s.trailing)));
      key("leadingPrecision");if(s.leadingPrecision)out<<*s.leadingPrecision;else out<<"null";
      key("fractionalPrecision");if(s.fractionalPrecision)out<<*s.fractionalPrecision;else out<<"null";
      key("prefixPrecision");value(s.prefixPrecision);out<<'}';
    }
    key("origin");
    range(t.origin);
    out << '}';
  }
  template <class N, class Q> void constraint(const BasicConstraint<N, Q> &k) {
    out << "{\"kind\":" << int(k.kind);
    key("name");
    value(k.name);
    key("columns");
    list(k.columns);
    key("referencedTable");
    list(k.referencedTable);
    key("referencedColumns");
    list(k.referencedColumns);
    key("check");
    out << k.check;
    key("onDelete");
    out << int(k.onDelete);
    key("onUpdate");
    out << int(k.onUpdate);
    key("deferrable");
    value(k.deferrable);
    key("initiallyDeferred");
    value(k.initiallyDeferred);
    if (k.clustering != PrimaryKeyClustering::Unspecified) {
      key("clustering");
      value(std::string(k.clustering == PrimaryKeyClustering::Clustered ? "clustered" : "nonclustered"));
    }
    key("origin");
    range(k.origin);
    out << '}';
  }
  template <class N, class Q> void column(const BasicColumnSpec<N, Q> &col) {
    out << "{\"name\":";
    value(col.name);
    key("type");
    type(col.type);
    key("nullable");
    value(col.nullable);
    key("default");
    out << col.defaultValue;
    key("defaultParenthesized");
    value(col.defaultParenthesized);
    key("generated");
    out << col.generated;
    key("identity");
    out << int(col.identity);
    if (col.autoRandom) {
      key("autoRandom");
      out << "{\"shardBits\":";
      if (col.autoRandom->shardBits) out << *col.autoRandom->shardBits; else out << "null";
      key("rangeBits");
      if (col.autoRandom->rangeBits) out << *col.autoRandom->rangeBits; else out << "null";
      key("origin");
      range(col.autoRandom->origin);
      out << '}';
    }
    key("identityStart");
    out << col.identityStart;
    key("identityIncrement");
    out << col.identityIncrement;
    key("stored");
    value(col.stored);
    key("collation");
    list(col.collation);
    key("characterSet");
    list(col.characterSet);
    key("comment");
    value(col.comment);
    key("constraints");
    out << '[';
    bool comma = false;
    for (const auto &k : col.constraints) {
      if (comma)
        out << ',';
      comma = true;
      constraint(k);
    }
    out << ']';
    key("origin");
    range(col.origin);
    out << '}';
  }
  template <class N, class Q> void ddl(const BasicDdl<N, Q> &d) {
    out << "{\"object\":";
    value(std::string(objectKindName(d.object)));
    key("name");
    list(d.name);
    key("indexedTable");
    list(d.indexedTable);
    key("indexMethod");
    list(d.indexMethod);
    key("likeTable");
    list(d.likeTable);
    if (d.virtualTable) {
      key("virtualTable");
      out << "{\"module\":";
      value(d.virtualTable->module);
      key("argumentList");
      value(d.virtualTable->argumentList);
      key("arguments");
      out << '[';
      for (std::size_t i = 0; i < d.virtualTable->arguments.size(); ++i) {
        if (i) out << ',';
        const auto &arg = d.virtualTable->arguments[i];
        out << "{\"payload\":";
        value(arg.payload);
        key("origin");
        range(arg.origin);
        out << '}';
      }
      out << "]}";
    }
    key("names");
    out << '[';
    bool comma = false;
    for (const auto &n : d.names) {
      if (comma)
        out << ',';
      comma = true;
      list(n);
    }
    out << ']';
    key("renames");
    out << '[';
    comma = false;
    for (const auto &pair : d.renames) {
      if (comma)
        out << ',';
      comma = true;
      out << "{\"from\":";
      list(pair.first);
      key("to");
      list(pair.second);
      out << '}';
    }
    out << ']';
    key("columns");
    list(d.columns);
    key("includeColumns");
    list(d.includeColumns);
    key("definitions");
    out << '[';
    comma = false;
    for (const auto &col : d.definitions) {
      if (comma)
        out << ',';
      comma = true;
      column(col);
    }
    out << ']';
    key("constraints");
    out << '[';
    comma = false;
    for (const auto &k : d.constraints) {
      if (comma)
        out << ',';
      comma = true;
      constraint(k);
    }
    out << ']';
    key("actions");
    out << '[';
    comma = false;
    for (const auto &a : d.actions) {
      if (comma)
        out << ',';
      comma = true;
      out << "{\"kind\":" << int(a.kind);
      key("name");
      value(a.name);
      key("newName");
      value(a.newName);
      key("column");
      if (a.column)
        column(*a.column);
      else
        out << "null";
      key("constraint");
      if (a.constraint)
        constraint(*a.constraint);
      else
        out << "null";
      key("type");
      if (a.type)
        type(*a.type);
      else
        out << "null";
      key("value");
      out << a.value;
      key("defaultParenthesized");
      value(a.defaultParenthesized);
      key("nullable");
      value(a.nullable);
      key("dropBehavior");
      out << int(a.dropBehavior);
      key("ifExists");
      value(a.ifExists);
      key("ifNotExists");
      value(a.ifNotExists);
      key("origin");
      range(a.origin);
      out << '}';
    }
    out << ']';
    key("keys");
    out << '[';
    comma = false;
    for (const auto &k : d.keys) {
      if (comma)
        out << ',';
      comma = true;
      out << "{\"expression\":" << k.expression
          << ",\"prefixLength\":" << k.prefixLength << ",\"nulls\":";
      value(k.nulls);
      out << ",\"descending\":";
      value(k.descending);
      out << '}';
    }
    out << ']';
    key("tableIndexes");
    out << '[';
    comma=false;
    for (const auto &index:d.tableIndexes) {
      if (comma) out << ',';
      comma=true;
      out << "{\"name\":";
      if (index.name) value(*index.name);
      else out << "null";
      key("unique");
      value(index.unique);
      key("method");
      out << int(index.method);
      key("origin");
      range(index.origin);
      key("keys");
      out << '[';
      for (std::size_t i=0;i<index.keys.size();++i) {
        if (i) out << ',';
        const auto &part=index.keys[i];
        out << "{\"expression\":" << part.expression << ",\"prefixLength\":" << part.prefixLength;
        key("descending");
        value(part.descending);
        out << '}';
      }
      out << "]}";
    }
    out << ']';
    key("truncateTargets");
    out << '[';
    comma = false;
    for (const auto &target : d.truncateTargets) {
      if (comma)
        out << ',';
      comma = true;
      out << "{\"name\":";
      list(target.name);
      key("only");
      value(target.only);
      key("descendants");
      value(target.descendants);
      out << '}';
    }
    out << ']';
    key("predicate");
    out << d.predicate;
    key("commentValue");
    out << d.commentValue;
    key("ifExists");
    value(d.ifExists);
    key("ifNotExists");
    value(d.ifNotExists);
    key("orReplace");
    value(d.orReplace);
    key("temporary");
    value(d.temporary);
    key("globalTemporary");
    value(d.globalTemporary);
    key("unlogged");
    value(d.unlogged);
    key("unique");
    value(d.unique);
    key("withNoData");
    value(d.withNoData);
    key("purge");
    value(d.purge);
    key("truncateOnly");
    value(d.truncateOnly);
    key("truncateDescendants");
    value(d.truncateDescendants);
    key("restartIdentity");
    value(d.restartIdentity);
    key("reuseStorage");
    value(d.reuseStorage);
    key("dropBehavior");
    out << int(d.dropBehavior);
    key("engine");
    value(d.engine);
    key("characterSet");
    value(d.characterSet);
    key("collation");
    value(d.collation);
    key("tablespace");
    value(d.tablespace);
    key("onCommit");
    value(d.onCommit);
    key("viewCheck");
    value(d.viewCheck);
    if (d.mysqlViewOptions) {
      const auto &header = *d.mysqlViewOptions;
      key("mysqlViewOptions");
      out << "{\"algorithm\":";
      value(std::string(mysqlViewAlgorithmName(header.algorithm)));
      out << ",\"security\":";
      value(std::string(mysqlViewSecurityName(header.security)));
      if (header.definer) {
        out << ",\"definer\":{\"currentUser\":";
        value(header.definer->currentUser);
        out << ",\"user\":";
        value(header.definer->user);
        out << ",\"host\":";
        value(header.definer->host);
        out << '}';
      }
      out << '}';
    }
    if (d.mysqlLoadableFunction) {
      const auto &function=*d.mysqlLoadableFunction;
      key("mysqlLoadableFunction");
      out << "{\"result\":";
      value(std::string(mysqlLoadableResultName(function.result)));
      out << ",\"aggregate\":";
      value(function.aggregate);
      out << ",\"library\":";
      value(function.library);
      out << '}';
    }
    key("tableComment");
    value(d.tableComment);
    if (d.indexScope != IndexScope::Unspecified) {
      key("indexScope");
      value(std::string(d.indexScope == IndexScope::Local ? "local" : "global"));
    }
    if (!d.distribution.empty()) {
      key("distribution");
      out << '[';
      comma = false;
      for (const auto &option : d.distribution) {
        if (comma) out << ',';
        comma = true;
        out << "{\"kind\":";
        value(std::string(distributionOptionName(option.kind)));
        key("value");
        if (const auto *number = std::get_if<std::uint64_t>(&option.value)) out << *number;
        else if (const auto *name = std::get_if<std::string>(&option.value)) value(*name);
        else out << "null";
        key("origin");
        range(option.origin);
        out << '}';
      }
      out << ']';
    }
    if (d.partition) {
      const auto &partition = *d.partition;
      key("partition");
      out << "{\"method\":";
      value(std::string(partition.method == PartitionMethod::Hash ? "hash" : "key"));
      key("linear");
      value(partition.linear);
      key("columns");
      list(partition.columns);
      key("count");
      if (partition.count) out << *partition.count; else out << "null";
      key("algorithm");
      if (partition.algorithm) out << *partition.algorithm; else out << "null";
      out << '}';
    }
    key("sequenceOptions");
    out << '[';
    comma = false;
    for (const auto &option : d.sequenceOptions) {
      if (comma)
        out << ',';
      comma = true;
      out << "{\"kind\":" << int(option.first) << ",\"value\":" << option.second
          << '}';
    }
    out << "]}";
  }
};
} // namespace
void dumpDdl(std::ostream &out, const Compilation &c, const DdlSyntax &ddl) {
  DdlJson writer(out, c);
  writer.ddl(ddl);
}
void dumpDdl(std::ostream &out, const Compilation &c, const ModelDdl &ddl) {
  DdlJson writer(out, c);
  writer.ddl(ddl);
}
} // namespace navidog::sql::compiler

namespace navidog::sql::compiler {
void dumpTypeSpec(std::ostream &out,const Compilation &c,const BasicTypeSpec<QualifiedName> &type) {DdlJson(out,c).type(type);}
void dumpTypeSpec(std::ostream &out,const Compilation &c,const BasicTypeSpec<std::vector<std::string>> &type) {DdlJson(out,c).type(type);}
}
