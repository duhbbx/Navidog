#include "bounded_output.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <streambuf>

namespace navidog::sql::compiler {
void dumpDdl(std::ostream &, const Compilation &, const DdlSyntax &);
void dumpDdl(std::ostream &, const Compilation &, const ModelDdl &);
void dumpTypeSpec(std::ostream &,const Compilation &,const BasicTypeSpec<QualifiedName> &);
void dumpTypeSpec(std::ostream &,const Compilation &,const BasicTypeSpec<std::vector<std::string>> &);
namespace {
std::string json(std::string_view text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\')
      out << '\\' << char(c);
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c)
          << std::dec;
    else
      out << char(c);
  }
  out << '"';
  return out.str();
}
void setJson(std::ostream &out, const std::optional<SetOperation> &set) {
  if (!set)
    return;
  out << ",\"setOperation\":{\"operator\":"
      << json(set->kind == SetKind::Union       ? "UNION"
              : set->kind == SetKind::Intersect ? "INTERSECT"
                                                : "EXCEPT")
      << ",\"all\":" << (set->all ? "true" : "false")
      << ",\"recursive\":" << (set->recursive ? "true" : "false")
      << ",\"left\":" << set->left << ",\"right\":" << set->right << '}';
}
void windowJson(std::ostream &out, const std::optional<WindowSpec> &spec) {
  if (!spec)
    return;
  out << ",\"window\":{\"partitions\":" << spec->partitions
      << ",\"ordering\":[";
  for (std::size_t i = 0; i < spec->ordering.size(); ++i) {
    if (i)
      out << ',';
    const auto &item = spec->ordering[i];
    out << "{\"descending\":" << (item.descending ? "true" : "false")
        << ",\"explicitAscending\":" << (item.explicitAscending?"true":"false")
        << ",\"nulls\":" << json(item.nulls) << '}';
  }
  out << "],\"frameUnit\":" << json(spec->frameUnit)
      << ",\"between\":" << (spec->between ? "true" : "false")
      << ",\"start\":" << int(spec->start) << ",\"end\":" << int(spec->end)
      << ",\"exclusion\":" << json(windowExclusionName(spec->exclusion))
      << '}';
}
void windowSourceJson(std::ostream &out,const std::optional<WindowSource> &source) {
  if (!source) return;
  out << ",\"windowSource\":{\"reference\":" << json(source->reference)
      << ",\"bare\":" << (source->bare?"true":"false") << ",\"keys\":" << source->keys;
  windowJson(out,source->specification);
  out << '}';
}
void withinGroupJson(std::ostream &out,const std::shared_ptr<const WindowOrdering> &order) {
  if (!order) return;
  out << ",\"withinGroup\":{\"descending\":" << (order->descending?"true":"false")
      << ",\"nulls\":" << json(order->nulls) << '}';
}
void constructorJson(std::ostream &out,const std::optional<JsonConstructorSpec> &spec) {
  if (!spec) return;
  const auto policy=spec->nullPolicy==JsonNullPolicy::NullOnNull?"NULL ON NULL":
                    spec->nullPolicy==JsonNullPolicy::AbsentOnNull?"ABSENT ON NULL":"DEFAULT";
  out << ",\"jsonConstructor\":{\"nullPolicy\":" << json(policy)
      << ",\"returningJson\":" << (spec->returningJson?"true":"false") << '}';
}
void intervalJson(std::ostream &out,const std::shared_ptr<const IntervalSpec> &spec) {
  if(!spec)return;
  out<<",\"interval\":{\"leading\":"<<json(intervalFieldName(spec->leading))
     <<",\"trailing\":"<<json(intervalFieldName(spec->trailing))
     <<",\"mysqlUnit\":"<<json(intervalUnitName(spec->mysqlUnit))
     <<",\"prefixPrecision\":"<<(spec->prefixPrecision?"true":"false");
  if(spec->leadingPrecision)out<<",\"leadingPrecision\":"<<*spec->leadingPrecision;
  if(spec->fractionalPrecision)out<<",\"fractionalPrecision\":"<<*spec->fractionalPrecision;
  out<<'}';
}
const char *kind(ExprKind k) {
  switch (k) {
  case ExprKind::Name:
    return "name";
  case ExprKind::Number:
    return "number";
  case ExprKind::String:
    return "string";
  case ExprKind::TypedLiteral:
    return "typedLiteral";
  case ExprKind::Interval:return "interval";
  case ExprKind::Extract:return "extract";
  case ExprKind::Null:
    return "null";
  case ExprKind::Boolean:
    return "boolean";
  case ExprKind::Variable:
    return "variable";
  case ExprKind::Parameter:
    return "parameter";
  case ExprKind::Star:
    return "star";
  case ExprKind::Unary:
    return "unary";
  case ExprKind::Binary:
    return "binary";
  case ExprKind::LikeEscape:
    return "likeEscape";
  case ExprKind::Call:
    return "call";
  case ExprKind::Window:
    return "window";
  case ExprKind::AggregateFilter:
    return "aggregateFilter";
  case ExprKind::Cast:
    return "cast";
  case ExprKind::Convert:
    return "convert";
  case ExprKind::Between:
    return "between";
  case ExprKind::In:
    return "in";
  case ExprKind::IsNull:
    return "isNull";
  case ExprKind::TruthTest:
    return "truthTest";
  case ExprKind::Case:
    return "case";
  case ExprKind::Subquery:
    return "subquery";
  case ExprKind::Exists:
    return "exists";
  case ExprKind::Default:
    return "default";
  case ExprKind::Row:
    return "row";
  case ExprKind::ArrayConstructor:
    return "arrayConstructor";
  case ExprKind::Subscript:
    return "subscript";
  case ExprKind::MergeAction:
    return "mergeAction";
  case ExprKind::Quantified:
    return "quantified";
  }
  return "unknown";
}
void range(std::ostream &out, SourceRange r) {
  out << '[' << r.begin << ',' << r.end << ']';
}
template <typename T>
void numbers(std::ostream &out, const std::vector<T> &values) {
  out << '[';
  bool comma = false;
  for (auto value : values) {
    if (comma)
      out << ',';
    comma = true;
    out << value;
  }
  out << ']';
}
void strings(std::ostream &out, const std::vector<std::string> &values) {
  out << '[';
  bool comma = false;
  for (const auto &value : values) {
    if (comma)
      out << ',';
    comma = true;
    out << json(value);
  }
  out << ']';
}
void sqlTypeJson(std::ostream &out,const SqlType &type) {
  out << "{\"family\":" << json(typeFamilyName(type.family))
      << ",\"nullability\":" << json(nullabilityName(type.nullability))
      << ",\"timeZone\":" << json(timeZoneName(type.timeZone))
      << ",\"lengthUnit\":" << json(lengthUnitName(type.lengthUnit))
      << ",\"arrayDimensions\":" << type.arrayDimensions
      << ",\"sqliteAffinity\":" << (type.sqliteAffinity?"true":"false");
  auto number=[&](const char *key,const auto &value) {out<<",\""<<key<<"\":";if(value)out<<*value;else out<<"null";};
  if(type.family==TypeFamily::Interval) {
    out<<",\"intervalLeading\":"<<json(intervalFieldName(type.intervalLeading))
       <<",\"intervalTrailing\":"<<json(intervalFieldName(type.intervalTrailing))
       <<",\"intervalUnit\":"<<json(intervalUnitName(type.intervalUnit));
    number("intervalLeadingPrecision",type.intervalLeadingPrecision);
  }
  number("precision",type.precision);number("scale",type.scale);number("bits",type.bits);number("length",type.length);
  auto boolean=[&](const char *key,const auto &value) {out<<",\""<<key<<"\":";if(value)out<<(*value?"true":"false");else out<<"null";};
  boolean("unsigned",type.unsignedInteger);boolean("nationalCharacter",type.nationalCharacter);
  out<<",\"characterSet\":";strings(out,type.characterSet);out<<",\"collation\":";strings(out,type.collation);
  out<<",\"opaqueName\":";strings(out,type.opaqueName);out<<",\"opaqueArguments\":";strings(out,type.opaqueArguments);
  out<<",\"opaqueModifiers\":";strings(out,type.opaqueModifiers);out<<",\"flavor\":"<<json(type.flavor)<<'}';
}
void names(std::ostream &out, const Compilation &c, const QualifiedName &name) {
  out << '[';
  bool comma = false;
  for (const auto &p : name) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"text\":" << json(c.ast.identifier(p.identifier))
        << ",\"quoted\":" << (p.quoted ? "true" : "false") << '}';
  }
  out << ']';
}
void alias(std::ostream &out, const Compilation &c,
           const std::optional<NamePart> &p) {
  if (!p)
    out << "null";
  else
    names(out, c, {*p});
}
void samplingJson(std::ostream &out,const std::optional<TableSampling> &sampling) {
  if (!sampling) return;
  out << ",\"sampling\":{\"method\":\"" << tableSamplingMethodName(sampling->method)
      << "\",\"percent\":" << tableSamplePercent(sampling->milliPercent);
  if (sampling->repeatable) out << ",\"repeatable\":" << *sampling->repeatable;
  out << '}';
}
void clickHouseSamplingJson(std::ostream &out, const std::optional<ClickHouseSampling> &sample) {
  if (!sample) return;
  const auto ratio = [&](const ClickHouseSampleRatio &value) {
    out << "{\"numerator\":" << json(value.numerator);
    if (!value.denominator.empty())
      out << ",\"denominator\":" << json(value.denominator);
    out << '}';
  };
  out << ",\"clickHouseSampling\":{\"size\":";
  ratio(sample->size);
  if (sample->offset) {
    out << ",\"offset\":";
    ratio(*sample->offset);
  }
  out << '}';
}
void limitByJson(std::ostream &out, const std::optional<GroupedRowLimit> &limit) {
  if (!limit) return;
  out << ",\"limitBy\":{\"count\":" << limit->count << ",\"offset\":" << limit->offset
      << ",\"all\":" << (limit->all ? "true" : "false") << ",\"keys\":[";
  for (std::size_t i = 0; i < limit->keys.size(); ++i) {
    if (i) out << ',';
    out << limit->keys[i];
  }
  out << "]}";
}
template<class Name,class WriteName>
void rowMatchJson(std::ostream &out,const std::optional<BasicRowMatch<Name>> &match,WriteName write) {
  if (!match) return;
  out << ",\"rowMatch\":{\"partition\":";
  if (match->partition) write(*match->partition);
  else out << "null";
  out << ",\"order\":";
  write(match->order);
  out << ",\"descending\":" << (match->descending?"true":"false")
      << ",\"nullOrdering\":" << json(match->nullOrdering) << ",\"measure\":";
  write(match->measure);
  out << ",\"variable\":";
  write(match->variable);
  out << ",\"testColumn\":";
  write(match->testColumn);
  out << ",\"comparison\":" << json(match->comparison) << ",\"threshold\":" << match->threshold << '}';
}
template<class Column, class WriteName>
void jsonTableColumnsJson(std::ostream &out, const Compilation &c,
                         const std::vector<Column> &columns, WriteName writeName,
                         std::size_t depth) {
  if (depth > c.options.limits.nesting)
    throw std::length_error("JSON_TABLE report nesting limit exceeded");
  out << '[';
  bool comma = false;
  for (const auto &column : columns) {
    if (comma) out << ',';
    comma = true;
    out << "{\"kind\":" << int(column.kind) << ",\"name\":";
    if (column.name) writeName(*column.name);
    else out << "null";
    out << ",\"path\":" << json(column.path) << ",\"type\":";
    dumpTypeSpec(out, c, column.type);
    out << ",\"onEmpty\":{\"kind\":" << int(column.onEmpty.kind) << ",\"json\":" << json(column.onEmpty.json)
        << "},\"onError\":{\"kind\":" << int(column.onError.kind) << ",\"json\":" << json(column.onError.json)
        << "},\"columns\":";
    jsonTableColumnsJson(out, c, column.children, writeName, depth+1);
    out << '}';
  }
  out << ']';
}
template<class Table, class WriteName>
void jsonTableJson(std::ostream &out, const Compilation &c,
                   const std::shared_ptr<const Table> &table, WriteName writeName) {
  if (!table) return;
  out << ",\"jsonTable\":{\"document\":" << table->document << ",\"path\":" << json(table->path)
      << ",\"documentTables\":";
  numbers(out, table->documentTables);
  out << ",\"columns\":";
  jsonTableColumnsJson(out, c, table->columns, writeName, 0);
  out << '}';
}
template<class Spec, class WriteName>
void preparedJson(std::ostream &out, const std::optional<Spec> &prepared, WriteName writeName) {
  out << ",\"prepared\":";
  if (!prepared) {
    out << "null";
    return;
  }
  out << "{\"action\":" << int(prepared->action) << ",\"name\":";
  writeName(prepared->name);
  out << ",\"source\":" << prepared->source << ",\"arguments\":";
  numbers(out, prepared->arguments);
  out << '}';
}
template<class Spec, class WriteName, class WriteQName>
void pivotJson(std::ostream &out, const std::shared_ptr<const Spec> &pivot,
               WriteName writeName, WriteQName writeQName) {
  if (!pivot) return;
  const auto optionalName = [&](const auto &name) {
    if (name) writeName(*name);
    else out << "null";
  };
  out << ",\"pivot\":{\"aggregate\":" << json(pivot->aggregate) << ",\"argument\":";
  writeQName(pivot->argument);
  out << ",\"axis\":";
  writeQName(pivot->axes);
  out << ",\"aggregateAlias\":";
  optionalName(pivot->aggregateAlias);
  out << ",\"inputAlias\":";
  optionalName(pivot->inputAlias);
  out << ",\"relationAlias\":";
  optionalName(pivot->relationAlias);
  out << ",\"buckets\":[";
  for (std::size_t i = 0; i < pivot->buckets.size(); ++i) {
    if (i) out << ',';
    const auto &values=pivot->buckets[i].values;
    if (values.size()<=1) out << "{\"value\":" << (values.empty()?0:values.front());
    else {
      out << "{\"values\":";
      numbers(out,values);
    }
    out << ",\"name\":";
    optionalName(pivot->buckets[i].name);
    out << '}';
  }
  out << "]}";
}
template<class Spec,class WriteName>
void unpivotJson(std::ostream &out,const std::shared_ptr<const Spec> &unpivot,WriteName write) {
  if (!unpivot) return;
  const auto columns=[&](const auto &list) {
    out << '[';
    for (std::size_t i=0;i<list.size();++i) {
      if (i) out << ',';
      write(list[i]);
    }
    out << ']';
  };
  out << ",\"unpivot\":{\"measures\":";
  columns(unpivot->measures);
  out << ",\"descriptors\":";
  columns(unpivot->descriptors);
  out << ",\"inputAlias\":";
  if (unpivot->inputAlias) write(*unpivot->inputAlias);
  else out << "null";
  out << ",\"relationAlias\":";
  if (unpivot->relationAlias) write(*unpivot->relationAlias);
  else out << "null";
  out << ",\"includeNulls\":";
  if (unpivot->includeNulls) out << (*unpivot->includeNulls?"true":"false");
  else out << "null";
  out << ",\"inputs\":[";
  for (std::size_t i=0;i<unpivot->inputs.size();++i) {
    if (i) out << ',';
    const auto &input=unpivot->inputs[i];
    out << "{\"columns\":";
    columns(input.columns);
    out << ",\"labels\":";
    numbers(out,input.labels);
    out << '}';
  }
  out << "]}";
}
template<class Spec, class WriteName>
void pragmaJson(std::ostream &out, const std::optional<Spec> &pragma, WriteName writeName) {
  if (!pragma) return;
  out << ",\"pragma\":{\"name\":";
  writeName(pragma->name);
  const auto style = pragma->style == PragmaArgumentStyle::Assignment ? "assignment" :
      pragma->style == PragmaArgumentStyle::Parenthesized ? "parenthesized" : "none";
  out << ",\"argumentStyle\":" << json(style) << ",\"argument\":" << pragma->argument << '}';
}
template<class Spec, class WriteNames>
void vacuumJson(std::ostream &out, const std::shared_ptr<const Spec> &vacuum, WriteNames writer) {
  if (!vacuum) return;
  out << ",\"vacuum\":{\"options\":[";
  for (std::size_t i = 0; i < vacuum->options.size(); ++i) {
    if (i) out << ',';
    const auto &option = vacuum->options[i];
    out << "{\"name\":" << json(vacuumOptionName(option.kind)) << ",\"value\":";
    if (const auto boolean = std::get_if<bool>(&option.argument)) out << (*boolean ? "true" : "false");
    else if (const auto workers = std::get_if<int>(&option.argument)) out << *workers;
    else if (const auto mode = std::get_if<VacuumIndexCleanup>(&option.argument)) out << json(vacuumIndexCleanupName(*mode));
    else out << "null";
    out << '}';
  }
  out << "],\"targets\":[";
  for (std::size_t i = 0; i < vacuum->targets.size(); ++i) {
    if (i) out << ',';
    const auto &target = vacuum->targets[i];
    out << "{\"name\":";
    writer(target.name);
    out << ",\"columns\":";
    writer(target.columns);
    out << '}';
  }
  out << "]}";
}
template<class Spec, class WriteNames>
void packageJson(std::ostream &out,const Compilation &c,
                 const std::shared_ptr<const Spec> &package,WriteNames writer) {
  if(!package)return;
  using Kind=PackageDeclarationKind;
  out<<",\"package\":{\"name\":";
  writer(package->name);
  out<<",\"drop\":"<<(package->drop?"true":"false")
     <<",\"replace\":"<<(package->replace?"true":"false")
     <<",\"edition\":"<<json(package->edition==PackageEdition::Editionable?"EDITIONABLE":
          package->edition==PackageEdition::NonEditionable?"NONEDITIONABLE":"default")
     <<",\"authid\":"<<json(package->auth==PackageAuth::Definer?"DEFINER":
          package->auth==PackageAuth::CurrentUser?"CURRENT_USER":"default");
  if(package->endName) {
    out<<",\"endName\":";
    writer(decltype(package->name){*package->endName});
  }
  out<<",\"declarations\":[";
  bool first=true;
  for(const auto &declaration:package->declarations) {
    if(!first)out<<',';
    first=false;
    const auto kind=declaration.kind;
    out<<"{\"kind\":"<<json(kind==Kind::Procedure?"procedure":kind==Kind::Function?"function":
        kind==Kind::Variable?"variable":kind==Kind::Constant?"constant":"exception")
       <<",\"name\":";
    writer(decltype(package->name){declaration.name});
    out<<",\"type\":";
    if(kind==Kind::Procedure || kind==Kind::Exception)out<<"null";
    else dumpTypeSpec(out,c,declaration.type);
    out<<",\"initializer\":"<<declaration.initializer<<",\"deterministic\":"
       <<(declaration.deterministic?"true":"false")<<",\"parameters\":[";
    bool firstParameter=true;
    for(const auto &parameter:declaration.parameters) {
      if(!firstParameter)out<<',';
      firstParameter=false;
      out<<"{\"name\":";
      writer(decltype(package->name){parameter.name});
      out<<",\"mode\":"<<json(parameter.mode==ParameterMode::In?"IN":
          parameter.mode==ParameterMode::InOut?"IN OUT":"OUT")
         <<",\"noCopy\":"<<(parameter.noCopy?"true":"false")
         <<",\"default\":"<<parameter.defaultValue<<",\"type\":";
      dumpTypeSpec(out,c,parameter.type);
      out<<'}';
    }
    out<<"]}";
  }
  out<<"]}";
}
template<class Spec, class WriteNames>
void triggerJson(std::ostream &out, const std::shared_ptr<const Spec> &trigger, WriteNames writer) {
  if (!trigger) return;
  out << ",\"trigger\":{\"name\":";
  writer(trigger->name);
  out << ",\"drop\":" << (trigger->drop ? "true" : "false")
      << ",\"temporary\":" << (trigger->temporary ? "true" : "false")
      << ",\"ifGuard\":" << (trigger->ifGuard ? "true" : "false");
  if (!trigger->drop) {
    out << ",\"timing\":" << json(triggerTimingName(trigger->timing))
        << ",\"event\":" << json(triggerEventName(trigger->event))
        << ",\"explicitRow\":" << (trigger->explicitRow ? "true" : "false")
        << ",\"compound\":" << (trigger->compound ? "true" : "false") << ",\"target\":";
    writer(trigger->target);
    if(trigger->definer) {
      out << ",\"definer\":{\"currentUser\":" << (trigger->definer->currentUser?"true":"false")
          << ",\"user\":" << json(trigger->definer->user)
          << ",\"host\":" << json(trigger->definer->host) << '}';
    }
    if(trigger->relativeTrigger) {
      out << ",\"order\":" << json(trigger->order==TriggerOrder::Follows?"FOLLOWS":"PRECEDES")
          << ",\"relativeTrigger\":";
      const decltype(trigger->updateColumns) relative{*trigger->relativeTrigger};
      writer(relative);
    }
    out << ",\"updateColumns\":";
    writer(trigger->updateColumns);
    out << ",\"when\":" << trigger->when << ",\"body\":[";
    for (std::size_t i = 0; i < trigger->body.size(); ++i) {
      if (i) out << ',';
      out << trigger->body[i];
    }
    out << ']';
  }
  out << '}';
}
template<class Spec, class WriteNames>
void copyJson(std::ostream &out, const std::shared_ptr<const Spec> &copy, WriteNames writer) {
  if (!copy) return;
  out << ",\"copy\":{\"direction\":" << json(copy->direction == CopyDirection::From ? "from" : "to")
      << ",\"stream\":" << json(copy->direction == CopyDirection::From ? "stdin" : "stdout")
      << ",\"columns\":";
  writer(copy->columns);
  if (copy->format) out << ",\"format\":" << json(copyFormatName(*copy->format));
  if (copy->header) out << ",\"header\":" << json(copyHeaderName(*copy->header));
  if (copy->freeze) out << ",\"freeze\":" << (*copy->freeze ? "true" : "false");
  const auto option = [&](std::string_view key, const std::optional<std::string> &value) {
    if (value) out << ',' << json(key) << ':' << json(*value);
  };
  option("delimiter", copy->delimiter);
  option("nullMarker", copy->nullMarker);
  option("quote", copy->quote);
  option("escape", copy->escape);
  option("encoding", copy->encoding);
  if (copy->forceQuoteAll) out << ",\"forceQuoteAll\":true";
  out << ",\"forceQuote\":"; writer(copy->forceQuote);
  out << ",\"forceNotNull\":"; writer(copy->forceNotNull);
  out << ",\"forceNull\":"; writer(copy->forceNull);
  out << '}';
}
void roleJson(std::ostream &out, const std::optional<RoleControl> &role) {
  out << ",\"role\":";
  if (!role) {
    out << "null";
    return;
  }
  auto accounts = [&](const auto &list) {
    out << '[';
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (i) out << ',';
      const auto &account = list[i];
      out << "{\"name\":" << json(account.name) << ",\"host\":";
      if (account.host) out << json(*account.host);
      else out << "null";
      out << ",\"range\":[" << account.range.begin << ',' << account.range.end << "]}";
    }
    out << ']';
  };
  out << "{\"scope\":" << int(role->scope) << ",\"mode\":" << int(role->mode) << ",\"roles\":";
  accounts(role->roles);
  out << ",\"recipients\":";
  accounts(role->recipients);
  out << '}';
}
template<class Spec, class WriteName>
void transactionJson(std::ostream &out, const std::optional<Spec> &transaction,
                     WriteName writeName) {
  out << ",\"transaction\":";
  if (!transaction) {
    out << "null";
    return;
  }
  const auto &control = *transaction;
  out << "{\"action\":" << int(control.action)
      << ",\"mode\":" << int(control.mode)
      << ",\"isolation\":" << int(control.isolation)
      << ",\"access\":" << int(control.access)
      << ",\"chain\":" << int(control.chain)
      << ",\"release\":" << int(control.release)
      << ",\"consistentSnapshot\":" << (control.consistentSnapshot ? "true" : "false")
      << ",\"deferrable\":";
  if (control.deferrable) out << (*control.deferrable ? "true" : "false");
  else out << "null";
  out << ",\"name\":";
  if (control.name) writeName(*control.name);
  else out << "null";
  out << '}';
}
template<class Spec, class WriteName, class WriteTarget>
void inspectionJson(std::ostream &out, const std::optional<Spec> &inspection,
                    WriteName writeName, WriteTarget writeTarget) {
  out << ",\"inspection\":";
  if (!inspection) { out << "null"; return; }
  const auto &value = *inspection;
  out << "{\"kind\":" << json(inspectionKindName(value.kind))
      << ",\"object\":" << json(inspectionObjectName(value.object))
      << ",\"scope\":" << json(value.scope == InspectionScope::Global ? "global" :
                                  value.scope == InspectionScope::Session ? "session" : "default")
      << ",\"full\":" << (value.full ? "true" : "false")
      << ",\"extended\":" << (value.extended ? "true" : "false")
      << ",\"ifNotExists\":" << (value.ifNotExists ? "true" : "false")
      << ",\"pattern\":" << value.pattern << ",\"target\":";
  writeTarget(value.target);
  out << ",\"database\":";
  if (value.database) writeName(*value.database); else out << "null";
  out << ",\"column\":";
  if (value.column) writeName(*value.column); else out << "null";
  if (value.jobCount) out << ",\"jobCount\":" << *value.jobCount;
  out << '}';
}
void header(std::ostream &out, const Compilation &c) {
  out << "{\"dialect\":" << json(dialectName(c.dialect))
      << ",\"syntaxValid\":" << (c.syntaxValid ? "true" : "false")
      << ",\"ok\":" << (c.ok() ? "true" : "false") << ",\"diagnostics\":[";
  bool comma = false;
  for (const auto &d : c.diagnostics) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"code\":" << int(d.code) << ",\"range\":";
    range(out, d.range);
    out << ",\"message\":" << json(d.message) << '}';
  }
  out << ']';
}
void order(std::ostream &out, const std::vector<OrderItem> &items) {
  out << '[';
  bool comma = false;
  for (const auto &i : items) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"expression\":" << i.expression
        << ",\"descending\":" << (i.descending ? "true" : "false")
        << ",\"explicitAscending\":" << (i.explicitAscending?"true":"false")
        << ",\"nulls\":" << json(i.nulls) << '}';
  }
  out << ']';
}
void assignments(std::ostream &out, const std::vector<Assignment> &values) {
  out << '[';
  bool comma = false;
  for (const auto &a : values) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"targets\":";
    numbers(out, a.targets);
    out << ",\"value\":" << a.value << '}';
  }
  out << ']';
}
void rows(std::ostream &out, const std::vector<std::vector<ExprId>> &values) {
  out << '[';
  bool comma = false;
  for (const auto &row : values) {
    if (comma)
      out << ',';
    comma = true;
    numbers(out, row);
  }
  out << ']';
}
template <class Branch, class Columns>
void mergeJson(std::ostream &out, ExprId on, RelationId source,
               const std::vector<Branch> &clauses, Columns columns) {
  out << ",\"merge\":{\"on\":" << on << ",\"source\":" << source
      << ",\"clauses\":[";
  for (std::size_t i = 0; i < clauses.size(); ++i) {
    if (i)
      out << ',';
    const auto &clause = clauses[i];
    out << "{\"match\":"
        << json(clause.match == MergeMatch::Matched ? "matched"
                : clause.match == MergeMatch::NotMatchedTarget
                    ? "notMatchedTarget"
                    : "notMatchedSource")
        << ",\"action\":"
        << json(clause.action == MergeAction::Update   ? "update"
                : clause.action == MergeAction::Delete ? "delete"
                : clause.action == MergeAction::Insert ? "insert"
                                                       : "nothing")
        << ",\"condition\":" << clause.condition
        << ",\"deleteAfterUpdateWhere\":" << clause.deleteWhere
        << ",\"defaultValues\":" << (clause.defaultValues ? "true" : "false")
        << ",\"origin\":";
    range(out, clause.origin);
    out << ",\"columns\":";
    columns(clause.columns);
    out << ",\"values\":";
    numbers(out, clause.values);
    out << ",\"assignments\":";
    assignments(out, clause.assignments);
    out << '}';
  }
  out << "]}";
}
void syntaxDml(std::ostream &out, const Compilation &c, const Statement &s) {
  if (s.kind == StatementKind::Merge)
    mergeJson(out, s.mergeOn, s.from, s.mergeClauses,
              [&](const auto &columns) { names(out, c, columns); });
  out << ",\"routine\":";
  names(out, c, s.routine);
  out << ",\"arguments\":";
  numbers(out, s.arguments);
  out << ",\"target\":";
  if (s.target) {
    out << "{\"name\":";
    names(out, c, s.target->name);
    out << ",\"alias\":";
    alias(out, c, s.target->alias);
    out << ",\"inheritance\":" << int(s.target->inheritance) << '}';
  } else
    out << "null";
  out << ",\"columns\":";
  names(out, c, s.columns);
  out << ",\"insertSource\":" << int(s.insertSource)
      << ",\"replaceExisting\":" << (s.replaceExisting ? "true" : "false")
      << ",\"source\":" << s.source;
  out << ",\"rows\":";
  rows(out, s.rows);
  out << ",\"assignments\":";
  assignments(out, s.assignments);
  out << ",\"returning\":[";
  bool comma = false;
  for (const auto &p : s.returning) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"expression\":" << p.expression << ",\"alias\":";
    alias(out, c, p.alias);
    out << '}';
  }
  out << "],\"returningInto\":";
  numbers(out, s.returningInto);
  if (s.outputInto) {
    const auto &sink=*s.outputInto;
    out << ",\"outputInto\":{\"table\":";
    names(out,c,sink.table);
    out << ",\"tableVariable\":" << json(sink.tableVariable) << ",\"columns\":";
    names(out,c,sink.columns);
    out << ",\"projections\":[";
    for (std::size_t i=0;i<sink.projections.size();++i) {
      if (i) out << ',';
      out << "{\"expression\":" << sink.projections[i].expression << ",\"alias\":";
      alias(out,c,sink.projections[i].alias);
      out << '}';
    }
    out << "]}";
  }
  out << ",\"modifiers\":";
  strings(out, s.modifiers);
  out << ",\"deleteTargets\":[";
  comma = false;
  for (const auto &target : s.deleteTargets) {
    if (comma)
      out << ',';
    comma = true;
    names(out, c, target);
  }
  out << ']';
  out << ",\"conflict\":{\"action\":" << int(s.conflict.action)
      << ",\"targets\":";
  numbers(out, s.conflict.targets);
  out << ",\"constraint\":";
  alias(out, c, s.conflict.constraint);
  out << ",\"rowAlias\":";
  alias(out, c, s.conflict.rowAlias);
  out << ",\"columnAliases\":";
  names(out, c, s.conflict.columnAliases);
  out << ",\"targetWhere\":" << s.conflict.targetWhere
      << ",\"where\":" << s.conflict.where << ",\"assignments\":";
  assignments(out, s.conflict.assignments);
  out << '}';
  out << ",\"from\":" << s.from << ",\"relations\":[";
  comma = false;
  for (const auto &r : s.relations) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"left\":" << r.left << ",\"right\":" << r.right
        << ",\"table\":" << r.table << ",\"join\":" << int(r.join)
        << ",\"on\":" << r.condition
        << ",\"natural\":" << (r.natural ? "true" : "false")
        << ",\"lateral\":" << (r.lateral ? "true" : "false")
        << ",\"implicitLateral\":" << (r.implicitLateral ? "true" : "false")
        << ",\"comma\":" << (r.comma?"true":"false")
        << ",\"fixedOrder\":" << (r.fixedOrder ? "true" : "false") << ",\"using\":";
    names(out, c, r.usingColumns);
    out << '}';
  }
  out << ']';
}
void modelDml(std::ostream &out, const ModelStatement &m) {
  if (m.kind == StatementKind::Merge)
    mergeJson(out, m.mergeOn, m.sourceFrom, m.mergeClauses,
              [&](const auto &columns) { strings(out, columns); });

  out << ",\"routine\":";
  strings(out, m.routine);
  out << ",\"arguments\":";
  numbers(out, m.arguments);
  out << ",\"target\":";
  if (m.target) {
    out << "{\"name\":";
    strings(out, m.target->name);
    out << ",\"alias\":" << json(m.target->alias)
        << ",\"inheritance\":" << int(m.target->inheritance) << '}';
  } else
    out << "null";
  out << ",\"targetReference\":";
  if (m.targetReference) {
    out << "{\"name\":";
    strings(out, m.targetReference->name);
    out << ",\"alias\":" << json(m.targetReference->alias)
        << ",\"inheritance\":" << int(m.targetReference->inheritance) << '}';
  } else
    out << "null";
  out << ",\"targetTable\":";
  if (m.targetTable)
    out << *m.targetTable;
  else
    out << "null";
  out << ",\"columns\":";
  strings(out, m.columns);
  out << ",\"insertSource\":" << int(m.insertSource)
      << ",\"replaceExisting\":" << (m.replaceExisting ? "true" : "false")
      << ",\"source\":" << m.source;
  out << ",\"rows\":";
  rows(out, m.rows);
  out << ",\"assignments\":";
  assignments(out, m.assignments);
  out << ",\"returning\":[";
  bool comma = false;
  for (const auto &p : m.returning) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"expression\":" << p.expression << ",\"alias\":" << json(p.alias)
        << '}';
  }
  out << "],\"returningInto\":";
  numbers(out, m.returningInto);
  if (m.outputInto) {
    const auto &sink=*m.outputInto;
    out << ",\"outputInto\":{\"table\":";
    strings(out,sink.table);
    out << ",\"tableVariable\":" << json(sink.tableVariable) << ",\"columns\":";
    strings(out,sink.columns);
    out << ",\"projections\":[";
    for (std::size_t i=0;i<sink.projections.size();++i) {
      if (i) out << ',';
      out << "{\"expression\":" << sink.projections[i].expression
          << ",\"alias\":" << json(sink.projections[i].alias) << '}';
    }
    out << "]}";
  }
  out << ",\"modifiers\":";
  strings(out, m.modifiers);
  out << ",\"deleteTargets\":[";
  comma = false;
  for (const auto &target : m.deleteTargets) {
    if (comma)
      out << ',';
    comma = true;
    strings(out, target);
  }
  out << ']';
  out << ",\"conflict\":{\"action\":" << int(m.conflict.action)
      << ",\"targets\":";
  numbers(out, m.conflict.targets);
  out << ",\"constraint\":" << json(m.conflict.constraint)
      << ",\"rowAlias\":" << json(m.conflict.rowAlias)
      << ",\"columnAliases\":";
  strings(out, m.conflict.columnAliases);
  out << ",\"incomingTable\":";
  if (m.conflict.incomingTable)
    out << *m.conflict.incomingTable;
  else
    out << "null";
  out << ",\"targetWhere\":" << m.conflict.targetWhere
      << ",\"where\":" << m.conflict.where << ",\"assignments\":";
  assignments(out, m.conflict.assignments);
  out << '}';
  out << ",\"sourceFrom\":" << m.sourceFrom << ",\"from\":" << m.from
      << ",\"relations\":[";
  comma = false;
  for (const auto &r : m.relations) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"left\":" << r.left << ",\"right\":" << r.right
        << ",\"table\":" << r.table << ",\"join\":" << int(r.join)
        << ",\"on\":" << r.condition
        << ",\"natural\":" << (r.natural ? "true" : "false")
        << ",\"lateral\":" << (r.lateral ? "true" : "false")
        << ",\"implicitLateral\":" << (r.implicitLateral ? "true" : "false")
        << ",\"comma\":" << (r.comma?"true":"false")
        << ",\"fixedOrder\":" << (r.fixedOrder ? "true" : "false") << ",\"using\":";
    strings(out, r.usingColumns);
    out << '}';
  }
  out << ']';
}
void syntaxQuery(std::ostream &out, const Compilation &c,
                 const SelectStatement &s) {
  out << "{\"kind\":" << json(statementKindName(s.kind)) << ",\"range\":";
  range(out, s.range);
  syntaxDml(out, c, s);
  inspectionJson(out, s.inspection, [&](const auto &n) { names(out, c, {n}); },
                  [&](const auto &target) { names(out, c, target); });
  transactionJson(out, s.transaction, [&](const auto &n) { names(out, c, {n}); });
  preparedJson(out, s.prepared, [&](const auto &n) { names(out, c, {n}); });
  roleJson(out, s.role);
  pragmaJson(out, s.pragma, [&](const auto &n) { names(out, c, n); });
  vacuumJson(out, s.vacuum, [&](const auto &n) { names(out, c, n); });
  triggerJson(out, s.trigger, [&](const auto &n) { names(out, c, n); });
  packageJson(out,c,s.package,[&](const auto &n){names(out,c,n);});
  copyJson(out, s.copy, [&](const auto &n) { names(out, c, n); });
  out << ",\"ddl\":";
  if (s.ddl)
    dumpDdl(out, c, *s.ddl);
  else
    out << "null";
  out << ",\"distinct\":" << (s.distinct ? "true" : "false")
      << ",\"fixedJoinOrder\":" << (s.fixedJoinOrder ? "true" : "false")
      << ",\"tableQuery\":" << int(s.tableQuery)
      << ",\"valuesQuery\":" << (s.valuesQuery ? "true" : "false")
      << ",\"sqlServerValueConstructor\":" << (s.sqlServerValueConstructor ? "true" : "false")
      << ",\"recursiveWith\":" << (s.recursiveWith ? "true" : "false") << ",\"ctes\":[";
  bool comma = false;
  for (const auto &cte : s.ctes) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"name\":";
    names(out, c, {cte.name});
    out << ",\"columns\":";
    names(out, c, cte.columns);
    out << ",\"query\":" << cte.query
        << ",\"materialization\":" << int(cte.materialization) << '}';
  }
  out << "],\"projections\":[";
  comma = false;
  for (const auto &p : s.projections) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"expression\":" << p.expression << ",\"alias\":";
    alias(out, c, p.alias);
    out << '}';
  }
  out << "],\"tables\":[";
  comma = false;
  for (const auto &t : s.tables) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"name\":";
    names(out, c, t.name);
    out << ",\"alias\":";
    alias(out, c, t.alias);
    out << ",\"columnAliases\":";
    names(out, c, t.columnAliases);
    out << ",\"join\":" << int(t.join) << ",\"on\":" << t.condition
        << ",\"query\":" << t.query << ",\"lateralLeft\":" << t.lateralLeft
        << ",\"inheritance\":" << int(t.inheritance);
    samplingJson(out,t.sampling);
    clickHouseSamplingJson(out, t.clickHouseSampling);
    if (t.finalRead) out << ",\"finalRead\":true";
    rowMatchJson(out,t.rowMatch,[&](const auto &field) { names(out,c,{field}); });
    if (t.postgresFunction==PostgresRowFunction::Unnest) out << ",\"unnestFunction\":true";
    if (t.postgresFunction==PostgresRowFunction::GenerateSeries) out << ",\"postgresFunction\":\"generate_series\"";
    if (t.ordinalityColumn) {
      out << (t.postgresFunction==PostgresRowFunction::Unnest?",\"unnestOrdinality\":":",\"ordinalityColumn\":");
      alias(out,c,t.ordinalityColumn);
    }
    jsonTableJson(out, c, t.jsonTable, [&](const auto &n) { names(out, c, {n}); });
    pivotJson(out, t.pivot, [&](const auto &n) { names(out, c, {n}); },
             [&](const auto &n) { names(out, c, n); });
    unpivotJson(out,t.unpivot,[&](const auto &n) { names(out,c,{n}); });
    out << '}';
  }
  out << "],\"prewhere\":" << s.prewhere << ",\"where\":" << s.where
      << ",\"qualify\":" << s.qualify;
  if (s.systemTime) out << ",\"systemTime\":" << s.systemTime;
  if (s.groupByAll) out << ",\"groupByAll\":true";
  limitByJson(out, s.limitBy);
  out << ",\"groupBy\":";
  numbers(out, s.groupBy);
  out<<",\"groupingSets\":[";for(std::size_t i=0;i<s.groupingSets.size();++i){if(i)out<<',';numbers(out,s.groupingSets[i]);}out<<']';
  out << ",\"having\":" << s.having << ",\"orderBy\":";
  order(out, s.orderBy);
  out << ",\"limit\":{\"syntax\":" << int(s.limit.syntax)
      << ",\"count\":" << s.limit.count << ",\"offset\":" << s.limit.offset
      << ",\"withTies\":" << (s.limit.withTies ? "true" : "false")
      << "},\"namedWindows\":[";
  bool windowComma = false;
  for (const auto &w : s.namedWindows) {
    if (windowComma)
      out << ',';
    windowComma = true;
    out << "{\"name\":";
    names(out, c, QualifiedName{w.name});
    out << ",\"keys\":";
    numbers(out, w.keys);
    windowJson(out, w.specification);windowSourceJson(out,w.source);
    out << '}';
  }
  out << "]";
  setJson(out, s.setOperation);
  out << '}';
}
} // namespace
std::string dumpAST(const Compilation &c) {
  std::ostringstream out;
  header(out, c);
  out << ",\"expressions\":[";
  for (ExprId id = 1; id <= c.ast.size(); ++id) {
    if (id > 1)
      out << ',';
    const auto &e = c.ast.expression(id);
    out << "{\"id\":" << id << ",\"kind\":" << json(kind(e.kind))
        << ",\"range\":";
    range(out, e.range);
    out << ",\"value\":" << json(e.value) << ",\"name\":";
    names(out, c, e.name);
    out << ",\"operands\":";
    numbers(out, e.operands);
    if(e.castType){out<<(e.kind==ExprKind::TypedLiteral?",\"literalType\":":",\"castType\":");dumpTypeSpec(out,c,*e.castType);}
    windowJson(out, e.window);windowSourceJson(out,e.windowSource);intervalJson(out,e.interval);withinGroupJson(out,e.withinGroup);constructorJson(out,e.jsonConstructor);
    out << ",\"query\":" << e.query
        << ",\"distinct\":" << (e.distinct ? "true" : "false")
        << ",\"explicitElse\":" << (e.explicitElse ? "true" : "false") << '}';
  }
  out << "],\"statements\":[";
  bool comma = false;
  for (const auto &s : c.statements) {
    if (comma)
      out << ',';
    comma = true;
    syntaxQuery(out, c, s);
  }
  out << "],\"nestedQueries\":[";
  for (QueryId id = 1; id <= c.ast.queryCount(); ++id) {
    if (id > 1)
      out << ',';
    out << "{\"id\":" << id << ",\"select\":";
    syntaxQuery(out, c, c.ast.query(id));
    out << '}';
  }
  out << "]}";
  return out.str();
}
std::string dumpModel(const Compilation &c, std::size_t maxBytes) {
  BoundedOutputBuffer buffer(maxBytes, c.options.shouldStop,
                             "AST JSON size limit exceeded");
  std::ostream out(&buffer);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  header(out, c);
  out << ",\"roots\":";
  numbers(out, c.model.roots);
  out << ",\"irVersion\":" << ModelProgram::formatVersion
      << ",\"sourceDialect\":" << json(dialectName(c.model.sourceDialect));
  out<<",\"types\":[";
  bool typeComma=false;
  for(const auto &type:c.model.types.all()) {
    if(typeComma)out<<',';typeComma=true;sqlTypeJson(out,type);
  }
  out<<']';
  out << ",\"queries\":[";
  bool comma = false;
  for (const auto &m : c.model) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"id\":" << m.id << ",\"kind\":" << json(statementKindName(m.kind))
        << ",\"origin\":";
    range(out, m.origin);
    modelDml(out, m);
    inspectionJson(out, m.inspection, [&](const auto &n) { out << json(n); },
                    [&](const auto &target) { strings(out, target); });
    transactionJson(out, m.transaction, [&](const auto &n) { out << json(n); });
    preparedJson(out, m.prepared, [&](const auto &n) { out << json(n); });
    roleJson(out, m.role);
    pragmaJson(out, m.pragma, [&](const auto &n) { strings(out, n); });
    vacuumJson(out, m.vacuum, [&](const auto &n) { strings(out, n); });
    triggerJson(out, m.trigger, [&](const auto &n) { strings(out, n); });
    packageJson(out,c,m.package,[&](const auto &n){strings(out,n);});
    copyJson(out, m.copy, [&](const auto &n) { strings(out, n); });
    if (m.prepared && m.prepared->parameterCount)
      out << ",\"preparedParameterCount\":" << *m.prepared->parameterCount;
    setJson(out, m.setOperation);
    out << ",\"sourceFile\":" << json(m.sourceFile) << ",\"transactionEffect\":"
        << json(transactionEffectName(m.transactionEffect)) << ",\"ddl\":";
    if (m.ddl)
      dumpDdl(out, c, *m.ddl);
    else
      out << "null";
    out << ",\"context\":{\"database\":" << json(m.context.database)
        << ",\"schema\":" << json(m.context.schema)
        << ",\"semanticEnvironment\":"
        << json(m.context.environment == SemanticEnvironment::Sql ? "sql"
                : m.context.environment ==
                        SemanticEnvironment::ProceduralCondition
                    ? "proceduralCondition"
                    : "proceduralScalar")
        << "},\"catalogChecked\":" << (m.catalogChecked ? "true" : "false")
        << ",\"distinct\":" << (m.distinct ? "true" : "false")
        << ",\"fixedJoinOrder\":" << (m.fixedJoinOrder ? "true" : "false")
        << ",\"expressions\":[";
    for (ExprId id = 1; id < m.expressions.size(); ++id) {
      if (id > 1)
        out << ',';
      const auto &e = m.expressions[id];
      out << "{\"id\":" << id << ",\"kind\":" << json(kind(e.kind))
          << ",\"value\":" << json(e.value) << ",\"name\":";
      strings(out, e.name);
      out << ",\"nameQuoted\":[";
      bool quoteComma = false;
      for (bool quoted : e.nameQuoted) {
        if (quoteComma)
          out << ',';
        quoteComma = true;
        out << (quoted ? "true" : "false");
      }
      out << "],\"unicodeString\":" << (e.unicodeString ? "true" : "false");
      if (e.unresolvedBooleanName) out << ",\"unresolvedBooleanName\":true";
      out << ",\"localSymbol\":" << e.localSymbol;
      if (e.outputColumn)
        out << ",\"outputColumn\":" << *e.outputColumn;
      out << ",\"operands\":";
      numbers(out, e.operands);
      if(e.castType){out<<(e.kind==ExprKind::TypedLiteral?",\"literalType\":":",\"castType\":");dumpTypeSpec(out,c,*e.castType);}
      windowJson(out, e.window);windowSourceJson(out,e.windowSource);intervalJson(out,e.interval);withinGroupJson(out,e.withinGroup);constructorJson(out,e.jsonConstructor);
      if(e.kind==ExprKind::Case) out << ",\"implicitCaseElse\":" << (e.implicitCaseElse?"true":"false");
      if(e.kind==ExprKind::Call) out<<",\"routineKind\":"<<json(routineKindName(callKind(e,c.dialect)))
          <<",\"volatility\":"<<json(volatilityName(callVolatility(e,c.dialect)))
          <<",\"routineResolved\":"<<(e.routineResolved?"true":"false")
          <<",\"routineSchema\":"<<json(e.routineSchema);
      out << ",\"type\":" << int(e.type) << ",\"sqlType\":" << e.sqlType << ",\"origin\":";
      range(out, e.origin);
      out << ",\"query\":" << e.query
          << ",\"distinct\":" << (e.distinct ? "true" : "false");
      if (!e.coalescedBindings.empty()) {
        out << ",\"coalescedBindings\":[";
        bool first = true;
        for (const auto &b : e.coalescedBindings) {
          if (!first)
            out << ',';
          first = false;
          out << "{\"scope\":" << b.scope << ",\"table\":" << b.table
              << ",\"column\":" << b.column << '}';
        }
        out << ']';
      }
      if (e.binding)
        out << ",\"binding\":{\"table\":" << e.binding->table
            << ",\"column\":" << e.binding->column
            << ",\"scope\":" << e.binding->scope << '}';
      out << '}';
    }
    out << "],\"tableQuery\":" << int(m.tableQuery)
        << ",\"valuesQuery\":" << (m.valuesQuery ? "true" : "false")
        << ",\"sqlServerValueConstructor\":" << (m.sqlServerValueConstructor ? "true" : "false")
        << ",\"recursiveWith\":" << (m.recursiveWith ? "true" : "false") << ",\"ctes\":[";
    bool pc = false;
    for (const auto &cte : m.ctes) {
      if (pc)
        out << ',';
      pc = true;
      out << "{\"name\":" << json(cte.name) << ",\"columns\":";
      strings(out, cte.columns);
      out << ",\"query\":" << cte.query
          << ",\"materialization\":" << int(cte.materialization);
      if (cte.definition) {
        out << ",\"arityKnown\":" << (cte.definition->arityKnown ? "true" : "false")
            << ",\"outputColumns\":[";
        for (std::size_t i = 0; i < cte.definition->columns.size(); ++i) {
          if (i) out << ',';
          out << json(cte.definition->columns[i].name);
        }
        out << ']';
      }
      out << '}';
    }
    out << "],\"projections\":[";
    pc = false;
    for (const auto &p : m.projections) {
      if (pc)
        out << ',';
      pc = true;
      out << "{\"expression\":" << p.expression
          << ",\"alias\":" << json(p.alias) << ",\"implicitAlias\":" << (p.implicitAlias?"true":"false") << '}';
    }
    out << "],\"tables\":[";
    pc = false;
    for (const auto &t : m.tables) {
      if (pc)
        out << ',';
      pc = true;
      out << "{\"name\":";
      strings(out, t.name);
      out << ",\"columnAliases\":";
      strings(out, t.columnAliases);
      out << ",\"alias\":" << json(t.alias) << ",\"join\":" << int(t.join)
          << ",\"on\":" << t.condition << ",\"query\":" << t.query
          << ",\"workingTableOf\":" << t.workingTableOf
          << ",\"inheritance\":" << int(t.inheritance)
          << ",\"pseudo\":" << (t.pseudo ? "true" : "false");
      samplingJson(out,t.sampling);
      clickHouseSamplingJson(out, t.clickHouseSampling);
      if (t.finalRead) out << ",\"finalRead\":true";
      rowMatchJson(out,t.rowMatch,[&](const auto &field) { out << json(field); });
      if (t.postgresFunction==PostgresRowFunction::Unnest) out << ",\"unnestFunction\":true";
      if (t.postgresFunction==PostgresRowFunction::GenerateSeries) out << ",\"postgresFunction\":\"generate_series\"";
      if (t.ordinalityColumn)
        out << (t.postgresFunction==PostgresRowFunction::Unnest?",\"unnestOrdinality\":":",\"ordinalityColumn\":")
            << json(*t.ordinalityColumn);
      jsonTableJson(out, c, t.jsonTable, [&](const auto &n) { out << json(n); });
      pivotJson(out, t.pivot, [&](const auto &n) { out << json(n); },
                [&](const auto &n) { strings(out, n); });
      unpivotJson(out,t.unpivot,[&](const auto &n) { out << json(n); });
      out << '}';
    }
    out << "],\"scope\":{\"parent\":";
    if (m.scope.parent)
      out << *m.scope.parent;
    else
      out << "null";
    if (m.scope.parentTables) {
      out << ",\"parentTables\":";
      numbers(out, *m.scope.parentTables);
    }
    out << ",\"tables\":[";
    pc = false;
    for (const auto &symbol : m.scope.tables) {
      if (pc)
        out << ',';
      pc = true;
      out << "{\"name\":" << json(symbol.name) << ",\"table\":" << symbol.table
          << '}';
    }
    out << "]},\"prewhere\":" << m.prewhere << ",\"where\":" << m.where
        << ",\"qualify\":" << m.qualify;
    if (m.systemTime) out << ",\"systemTime\":" << m.systemTime;
    if (m.groupByAll) out << ",\"groupByAll\":true";
    limitByJson(out, m.limitBy);
    out << ",\"groupBy\":";
    numbers(out, m.groupBy);
    out<<",\"groupingSets\":[";for(std::size_t i=0;i<m.groupingSets.size();++i){if(i)out<<',';numbers(out,m.groupingSets[i]);}out<<']';
    out << ",\"having\":" << m.having << ",\"orderBy\":";
    order(out, m.orderBy);
    out << ",\"namedWindows\":[";
    bool namedComma = false;
    for (const auto &w : m.namedWindows) {
      if (namedComma)
        out << ',';
      namedComma = true;
      out << "{\"name\":" << json(w.name) << ",\"keys\":";
      numbers(out, w.keys);
      windowJson(out, w.specification);windowSourceJson(out,w.source);
      out << '}';
    }
    out << "],\"limit\":" << m.limit << ",\"offset\":" << m.offset
        << ",\"limitWithTies\":" << (m.limitWithTies ? "true" : "false");
    if (m.selectInto) {
      out << ",\"selectInto\":{\"mode\":" << int(m.selectInto->mode) << ",\"targets\":";
      numbers(out, m.selectInto->targets);
      out << '}';
    }
    out << '}';
  }
  out << "]}";
  return std::move(buffer.value);
}
} // namespace navidog::sql::compiler
