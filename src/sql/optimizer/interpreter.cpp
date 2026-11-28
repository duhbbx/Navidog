#include "navidog/sql/optimizer/interpreter.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "optimizer_internal.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <numeric>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace navidog::sql::optimizer {
class PreparedLocalQuery {
public:
  const LogicalPlan plan;
  const std::vector<LocalParameter> parameters;
  const std::size_t accountedBytes;

  PreparedLocalQuery(LogicalPlan logical, std::vector<LocalParameter> inputs, std::size_t bytes)
      : plan(std::move(logical)), parameters(std::move(inputs)), accountedBytes(bytes) {}
};

namespace {
using namespace compiler;
using Rows = std::vector<LocalRow>;
using Clock = std::chrono::steady_clock;
struct RuntimeFailure { Diagnostic diagnostic; };
enum class Op {
  Literal, Load, Duplicate, Pop, Jump, JumpUnlessTrue, Positive, Negative, ExactNumeric,
  TypedNumeric, NumericToInteger, NumericNegative, Not, IsNull, IsNotNull,
  Add, Subtract, NumericAdd, NumericSubtract, NumericMultiply, NumericDivide, NumericModulo,
  Multiply, Divide, Modulo, And, Or,
  Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, Distinct, NotDistinct,
  SqliteTruthEqual, SqliteTruthDifferent, TruthTest, Between, In,
  ScalarSubquery, ExistsSubquery, InSubquery, ArrayIndex, ArrayCompare
};
struct Instruction {
  Op op;
  SourceRange origin;
  LocalValue literal = std::monostate{};
  std::size_t argument = 0;
  const RelNode *capturedInput = nullptr;
};
using Scalar = std::vector<Instruction>;

bool isNull(const LocalValue &value) {
  return std::holds_alternative<std::monostate>(value);
}

constexpr std::size_t maxLocalParameters = 1024;
std::optional<std::size_t> parameterPosition(std::string_view spelling) {
  if (spelling.size()<2 || spelling.size()>5 || spelling.front()!='$' || spelling[1]=='0') return {};
  std::size_t position=0;
  const auto end=spelling.data()+spelling.size();
  const auto parsed=std::from_chars(spelling.data()+1,end,position);
  if (parsed.ec!=std::errc{} || parsed.ptr!=end || position>maxLocalParameters) return {};
  return position;
}

std::optional<std::int64_t> exactIntegerText(std::string_view value) {
  if (value.empty()) return {};
  std::int64_t number=0;
  const auto end=value.data()+value.size();
  const auto parsed=std::from_chars(value.data(),end,number);
  if (parsed.ec!=std::errc{} || parsed.ptr!=end) return {};
  return number;
}

bool plainBoolean(const SqlType &type) {
  return type.family==TypeFamily::Boolean && type.flavor.empty() &&
      !type.arrayDimensions && !type.sqliteAffinity && type.opaqueName.empty();
}

bool numericType(const SqlType &type) {
  return type.family==TypeFamily::Decimal &&
      !type.arrayDimensions && !type.sqliteAffinity && type.opaqueName.empty() && type.flavor.empty();
}

bool fitsSignedInteger(std::int64_t value, int bits) {
  if (bits==64) return true;
  const auto bound=std::int64_t(1) << (bits-1);
  return value>=-bound && value<bound;
}

class Interpreter {
  const LogicalPlan &plan;
  const InterpreterLimits &limits;
  Clock::time_point deadline;
  bool sqlite;
  RelationProvider *relations;
  const LocalRow *parameters;
  std::map<std::pair<QueryId,std::size_t>,std::vector<bool>> scanColumns;
  std::size_t steps = 0, bytes = 0;
  std::size_t relationalDepth = 0, evaluationDepth = 0;
  std::deque<LocalRow> evaluationStacks;
  std::vector<IntegerArrayLiteral> arrayLiterals;
  struct SubqueryPlan { RelId root; bool correlated; };
  struct SubqueryRows {
    Rows rows;
    std::optional<std::set<LocalValue>> membership;
  };
  std::map<std::pair<QueryId, QueryId>, SubqueryPlan> subqueries;
  std::map<RelId, SubqueryRows> subqueryCache;
  struct CapturedRow {
    const RelNode &schema;
    const LocalRow &row;
    std::map<RelId, SubqueryRows> subqueries;
  };
  std::deque<CapturedRow> capturedRows;
  struct WorkingRows {
    const Rows &rows;
    RelId seed;
    std::size_t width;
  };
  std::map<QueryId, WorkingRows> working;
  struct ComputedBindings {
    QueryId query;
    std::map<ExprId, std::size_t> terms;
    std::optional<std::set<std::size_t>> groupedColumns;
  };
  struct GroupedRows {
    const RelNode &schema;
    ComputedBindings bindings;
    Rows rows;
  };

  [[noreturn]] static void fail(SourceRange at, std::string message,
                                DiagnosticCode code = DiagnosticCode::UnsupportedSyntax) {
    throw RuntimeFailure{{code, at, std::move(message)}};
  }
  static void requireNonText(const LocalValue &value, SourceRange at) {
    if (std::holds_alternative<LocalText>(value))
      fail(at, "local text comparison requires a verified collation");
  }
  void step(SourceRange at, std::size_t count = 1) {
    if (count > limits.maxSteps - steps)
      fail(at, "local interpreter exceeded its work budget", DiagnosticCode::ResourceLimit);
    steps += count;
    if (limits.shouldStop && limits.shouldStop())
      fail(at, "local interpretation cancelled", DiagnosticCode::Stopped);
    if (Clock::now() >= deadline)
      fail(at, "local interpreter exceeded its time budget", DiagnosticCode::ResourceLimit);
  }
  void charge(SourceRange at, std::size_t count) {
    if (count > (limits.maxBytes - bytes) / 2)
      fail(at, "local interpreter exceeded its allocation budget", DiagnosticCode::ResourceLimit);
    bytes += count * 2;
  }
  void rowBudget(SourceRange at, std::size_t width, std::size_t count) {
    if (count > limits.maxRows)
      fail(at, "local interpreter exceeded its row budget", DiagnosticCode::ResourceLimit);
    charge(at, count * (sizeof(LocalRow) + width * sizeof(LocalValue)));
  }
  LocalValue boolean(bool value) const {
    if (sqlite) return std::int64_t(value);
    return value;
  }
  enum class Truth { Null, False, True };
  Truth truth(const LocalValue &value, SourceRange at) const {
    if (isNull(value)) return Truth::Null;
    if (auto boolean = std::get_if<bool>(&value)) return *boolean ? Truth::True : Truth::False;
    if (sqlite) {
      if (auto integer = std::get_if<std::int64_t>(&value)) return *integer ? Truth::True : Truth::False;
    }
    fail(at, "local predicate requires a Boolean value", DiagnosticCode::InvalidQueryShape);
  }
  LocalValue truthValue(Truth value) const {
    return value == Truth::Null ? LocalValue{std::monostate{}} : boolean(value == Truth::True);
  }
  static std::int64_t integer(const std::string &text, SourceRange at) {
    const auto value=exactIntegerText(text);
    if (!value) fail(at,"local evaluator supports only exact signed integer literals");
    return *value;
  }
  LocalValue integerLiteral(const ModelExpression &expression,const std::string &text) {
    if (!sqlite && numericType(plan.program.types.get(expression.sqlType))) {
      const auto decimal=LocalNumeric::parse(text);
      if (!decimal) fail(expression.origin,"local numeric literals require a plain decimal within 27 integer and 27 fractional digits");
      charge(expression.origin,LocalNumeric::storageBytes);
      return *decimal;
    }
    return integer(text,expression.origin);
  }
  static LocalNumeric numericValue(const LocalValue &value,SourceRange origin) {
    if (const auto *numeric=std::get_if<LocalNumeric>(&value)) return *numeric;
    if (const auto *integer=std::get_if<std::int64_t>(&value)) return LocalNumeric(*integer);
    fail(origin,"local numeric operation requires an exact integer value");
  }
  static bool int32(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
  }
  void instruction(Scalar &out, Instruction entry) {
    charge(entry.origin, sizeof(entry));
    out.push_back(std::move(entry));
  }
  std::size_t registerIntegerArray(ExpressionRef ref,SourceRange at) {
    constexpr auto cellSize=sizeof(std::optional<std::int64_t>);
    const auto elementBudget=std::min<std::size_t>(65536,(limits.maxBytes-bytes)/(2*cellSize));
    auto array=postgresIntegerArrayLiteral(plan,ref,elementBudget,[&](auto work) { step(at,work); });
    if (array.status==IntegerArrayStatus::ResourceLimit)
      fail(at,"local integer array exceeds the literal budget",DiagnosticCode::ResourceLimit);
    if (array.status!=IntegerArrayStatus::Ready)
      fail(at,"local array operation requires a signed integer array literal");
    charge(at,sizeof(array)+array.elements.size()*cellSize+
              array.dimensions.size()*sizeof(IntegerArrayLiteral::Dimension));
    const auto slot=arrayLiterals.size();
    arrayLiterals.push_back(std::move(array));
    return slot;
  }
  std::size_t binding(const ModelExpression &expression, const RelNode *input) const {
    if (!input || !expression.binding || !expression.coalescedBindings.empty())
      fail(expression.origin, "local evaluator requires a directly bound input column");
    const auto &column = *expression.binding;
    const auto &owner = modelQuery(plan, column.scope);
    if (column.table >= owner.tables.size() || !owner.tables[column.table].definition ||
        column.column >= owner.tables[column.table].definition->columns.size())
      fail(expression.origin, "invalid local input column", DiagnosticCode::InvalidQueryShape);
    const auto &name = owner.tables[column.table].definition->columns[column.column].name;
    std::optional<std::size_t> slot;
    for (std::size_t i = 0; i < input->outputs.size(); ++i) {
      const auto &output = input->outputs[i];
      if (column.scope == output.query && column.table == output.table && name == output.name) {
        if (slot) fail(expression.origin, "local input column identity is ambiguous");
        slot = i;
      }
    }
    if (!slot) fail(expression.origin, "local sort or filter requires a column outside the input projection");
    return *slot;
  }
  const LocalValue *parameterBinding(const ModelExpression &e) const {
    if (sqlite || !parameters || e.kind!=ExprKind::Parameter) return nullptr;
    const auto position=parameterPosition(e.value);
    return position && *position<=parameters->size() ? &parameters->at(*position-1) : nullptr;
  }
  const LocalValue *capturedColumn(ExpressionRef ref) {
    const auto &e=modelExpression(plan,ref);
    if (!e.binding || e.binding->scope==ref.query) return nullptr;
    for (auto frame=capturedRows.rbegin(); frame!=capturedRows.rend(); ++frame) {
      step(e.origin);
      if (frame->schema.query!=e.binding->scope) continue;
      step(e.origin,frame->schema.outputs.size());
      return &frame->row.at(binding(e,&frame->schema));
    }
    return nullptr;
  }
  void compileScalar(Scalar &out, ExpressionRef ref, const RelNode *input,
                     bool projected, std::size_t depth = 0, const ComputedBindings *aggregate = nullptr) {
    const auto &e = modelExpression(plan, ref);
    step(e.origin);
    if (depth >= limits.maxDepth)
      fail(e.origin, "local expression nesting limit exceeded", DiagnosticCode::ResourceLimit);
    if (aggregate && ref.query == aggregate->query) {
      const auto term = aggregate->terms.find(ref.expression);
      if (term != aggregate->terms.end()) {
        instruction(out, {Op::Load, e.origin, std::monostate{}, term->second});
        return;
      }
    }
    if (e.localSymbol || e.window)
      fail(e.origin, "expression has a procedure, subquery or unsupported window dependency");
    if (e.query) {
      const auto dependency = subqueries.find({ref.query, e.query});
      if (dependency == subqueries.end())
        fail(e.origin, "local expression requires a compiled subquery");
      const auto &query = modelQuery(plan, e.query);
      const RelNode *capture=nullptr;
      if (dependency->second.correlated) {
        if (sqlite || !input || input->query!=ref.query || query.scope.parent!=ref.query || aggregate)
          fail(e.origin,"local correlated expression needs a PostgreSQL input in its declared scope");
        capture=input;
      }
      const bool exists = e.kind == ExprKind::Exists && e.operands.empty();
      const bool scalar = e.kind == ExprKind::Subquery && e.operands.empty();
      const bool any=!sqlite && e.kind==ExprKind::Quantified && (e.value=="= ANY" || e.value=="= SOME");
      const bool all=!sqlite && e.kind==ExprKind::Quantified && (e.value=="<> ALL" || e.value=="!= ALL");
      const bool in=e.operands.size()==1 &&
          (any || all || (e.kind==ExprKind::In && (e.value=="IN" || e.value=="NOT IN")));
      if (!exists && !scalar && !in)
        fail(e.origin, "unsupported local subquery expression");
      if (exists && capture &&
          std::any_of(query.projections.begin(),query.projections.end(),[&](const auto &output) {
            const auto &value=modelExpression(plan,{query.id,output.expression});
            return value.kind!=ExprKind::Name && value.kind!=ExprKind::Number &&
                   value.kind!=ExprKind::Null && value.kind!=ExprKind::Boolean;
          }))
        fail(e.origin,"local correlated EXISTS requires a directly bound or literal projection");
      if (!exists && plan.nodes.at(dependency->second.root).outputs.size() != 1)
        fail(e.origin, "scalar subquery requires one output column", DiagnosticCode::InvalidQueryShape);
      if (!sqlite && !exists) {
        if (query.projections.size() != 1)
          fail(e.origin, "local PostgreSQL subquery needs a resolved scalar output type");
        const auto &output = modelExpression(plan, {query.id, query.projections.front().expression});
        if (output.type != ValueType::Integer && output.type != ValueType::Boolean &&
            !(scalar && (output.type==ValueType::String || output.type==ValueType::Decimal)))
          fail(output.origin, "local PostgreSQL subquery needs a supported scalar output");
        if (in) {
          const auto &value = modelExpression(plan, {ref.query, e.operands.front()});
          if (value.type != output.type && value.type != ValueType::Null)
            fail(value.origin, "local PostgreSQL IN subquery needs compatible exact operand types");
        }
      }
      if (in) compileScalar(out, {ref.query, e.operands.front()}, input, projected, depth + 1, aggregate);
      const auto operation = exists ? Op::ExistsSubquery : scalar ? Op::ScalarSubquery : Op::InSubquery;
      instruction(out, {operation, e.origin, std::monostate{}, dependency->second.root,capture});
      if (in && (e.value=="NOT IN" || all)) instruction(out, {Op::Not, e.origin});
      return;
    }
    if (e.outputColumn) {
      if (!projected || !input || *e.outputColumn >= input->outputs.size() ||
          input->outputs.size() != modelQuery(plan, ref.query).projections.size())
        fail(e.origin, "local output reference lacks a supported projection slot");
      instruction(out, {Op::Load, e.origin, std::monostate{}, *e.outputColumn});
      return;
    }
    switch (e.kind) {
    case ExprKind::Null:
      instruction(out, {Op::Literal, e.origin});
      return;
    case ExprKind::Boolean:
      if (e.unresolvedBooleanName)
        fail(e.origin, "SQLite Boolean word needs known column names");
      if (sqlite && input) {
        step(e.origin, input->outputs.size());
        if (std::any_of(input->outputs.begin(), input->outputs.end(), [&](const auto &column) {
              return column.name.size() == e.value.size() && uppercase(column.name) == e.value;
            }))
          fail(e.origin, "SQLite Boolean keyword is shadowed by a visible column");
      }
      instruction(out, {Op::Literal, e.origin, boolean(e.value == "TRUE")});
      return;
    case ExprKind::Number:
      instruction(out, {Op::Literal,e.origin,integerLiteral(e,e.value)});
      return;
    case ExprKind::Parameter: {
      const auto *binding=parameterBinding(e);
      if (!binding) fail(e.origin,"local query parameters require PostgreSQL prepare and execute");
      instruction(out,{Op::Literal,e.origin,*binding});
      if (numericType(plan.program.types.get(e.sqlType)) && std::holds_alternative<std::int64_t>(*binding))
        instruction(out,{Op::ExactNumeric,e.origin});
      return;
    }
    case ExprKind::Cast: {
      if (sqlite || e.operands.size()!=1)
        fail(e.origin,"local CAST requires a supported PostgreSQL scalar domain");
      const auto &from=modelExpression(plan,{ref.query,e.operands.front()});
      const auto &sourceType=plan.program.types.get(from.sqlType);
      const auto &destination=plan.program.types.get(e.sqlType);
      const bool null=from.kind==ExprKind::Null;
      const bool boolean=plainBoolean(destination) && (null || plainBoolean(sourceType));
      const auto bits=signedIntegerBits(destination);
      const bool integer=bits && (null || signedIntegerBits(sourceType) || numericType(sourceType));
      const bool numeric=numericType(destination) &&
          (null || signedIntegerBits(sourceType) || numericType(sourceType));
      if (!boolean && !integer && !numeric)
        fail(e.origin,"local CAST requires an exact integer, numeric or Boolean conversion");
      compileScalar(out,{ref.query,e.operands.front()},input,projected,depth+1,aggregate);
      if (integer)
        instruction(out,{numericType(sourceType)?Op::NumericToInteger:Op::Positive,e.origin,
                         std::monostate{},static_cast<std::size_t>(*bits)});
      if (numeric) {
        if (!destination.precision) instruction(out,{Op::ExactNumeric,e.origin});
        else {
          const auto scale=destination.scale.value_or(0);
          if (scale < -LocalNumeric::maxScale || scale>LocalNumeric::maxScale)
            fail(e.origin,"local numeric cast scale must be within -27 to 27",DiagnosticCode::ResourceLimit);
          instruction(out,{Op::TypedNumeric,e.origin,std::int64_t(scale),
                           static_cast<std::size_t>(*destination.precision)});
        }
      }
      return;
    }
    case ExprKind::Subscript: {
      const auto axes=e.value.size();
      if (sqlite || !axes || e.operands.size()!=axes+1 ||
          !std::all_of(e.value.begin(),e.value.end(),[](auto c) {
            return c==static_cast<char>(SubscriptKind::Index);
          }))
        fail(e.origin,"local array access requires element subscripts");
      const auto slot=registerIntegerArray({ref.query,e.operands.front()},e.origin);
      for (std::size_t i=1;i<e.operands.size();++i) {
        const ExpressionRef index{ref.query,e.operands[i]};
        const auto &value=modelExpression(plan,index);
        const auto &type=plan.program.types.get(value.sqlType);
        if (!signedIntegerBits(type) && !numericType(type) && value.kind!=ExprKind::Null)
          fail(value.origin,"local array subscript requires signed integer or exact numeric");
        compileScalar(out,index,input,projected,depth+1,aggregate);
        instruction(out,{numericType(type)?Op::NumericToInteger:Op::Positive,value.origin,std::monostate{},32});
      }
      instruction(out,{Op::ArrayIndex,e.origin,static_cast<std::int64_t>(axes),slot});
      return;
    }
    case ExprKind::Quantified: {
      const auto name=std::string_view(e.value);
      const auto space=name.find(' ');
      if (sqlite || e.operands.size()!=2 || space==std::string_view::npos)
        fail(e.origin,"invalid local PostgreSQL array comparison",DiagnosticCode::InvalidQueryShape);
      const auto operation=comparisonOperation(name.substr(0,space));
      const auto quantifier=name.substr(space+1);
      if (!operation || (quantifier!="ANY" && quantifier!="SOME" && quantifier!="ALL"))
        fail(e.origin,"unsupported local PostgreSQL array comparison");
      const ExpressionRef left{ref.query,e.operands.front()};
      const auto &operand=modelExpression(plan,left);
      const auto &type=plan.program.types.get(operand.sqlType);
      if (operand.kind!=ExprKind::Null && !signedIntegerBits(type) && !numericType(type))
        fail(operand.origin,"local PostgreSQL array comparison needs an exact numeric left operand");
      const auto slot=registerIntegerArray({ref.query,e.operands.back()},e.origin);
      compileScalar(out,left,input,projected,depth+1,aggregate);
      const auto packed=2*static_cast<std::int64_t>(*operation)+(quantifier=="ALL");
      instruction(out,{Op::ArrayCompare,e.origin,packed,slot});
      return;
    }
    case ExprKind::Name: {
      if (const auto *captured=capturedColumn(ref)) {
        // Bytecode for each dependent invocation is built with that
        // invocation's row. Captures are constant while it executes.
        instruction(out,{Op::Literal,e.origin,*captured});
        return;
      }
      step(e.origin, input ? input->outputs.size() : 0);
      const auto slot = binding(e, input);
      if (aggregate && aggregate->groupedColumns && !aggregate->groupedColumns->count(slot))
        fail(e.origin, "local grouped projection requires a grouping column or aggregate result");
      instruction(out, {Op::Load, e.origin, std::monostate{}, slot});
      return;
    }
    case ExprKind::Case:
      compileCase(out,ref.query,e,input,projected,depth+1,aggregate);
      if (!sqlite && numericType(plan.program.types.get(e.sqlType)))
        instruction(out,{Op::ExactNumeric,e.origin});
      return;
    case ExprKind::TruthTest: {
      const bool negate = e.value.compare(0, 7, "IS NOT ") == 0;
      if (sqlite || e.operands.size() != 1 || e.value.compare(0, 3, "IS ") != 0)
        fail(e.origin, "invalid local truth test", DiagnosticCode::InvalidQueryShape);
      const auto expected = e.value.substr(negate ? 7 : 3);
      if (expected != "TRUE" && expected != "FALSE" && expected != "UNKNOWN")
        fail(e.origin, "invalid local truth test", DiagnosticCode::InvalidQueryShape);
      compileScalar(out, {ref.query, e.operands[0]}, input, projected, depth + 1, aggregate);
      instruction(out, {Op::TruthTest, e.origin,
          expected == "UNKNOWN" ? LocalValue{} : boolean(expected == "TRUE"), negate});
      return;
    }
    case ExprKind::Between:
    case ExprKind::In: {
      const bool between = e.kind == ExprKind::Between;
      const bool symmetric = e.value == "BETWEEN SYMMETRIC" || e.value == "NOT BETWEEN SYMMETRIC";
      const bool negate = e.value.compare(0, 4, "NOT ") == 0;
      const bool valid = between ?
          e.operands.size() == 3 && (symmetric || e.value == "BETWEEN" || e.value == "NOT BETWEEN") :
          (e.operands.size() >= 2 || (sqlite && e.operands.size() == 1)) &&
              (e.value == "IN" || e.value == "NOT IN");
      if (e.query || !valid)
        fail(e.origin, "local predicate needs a scalar BETWEEN or a literal/expression IN list");
      for (auto operand : e.operands)
        compileScalar(out, {ref.query, operand}, input, projected, depth + 1, aggregate);
      instruction(out, {between ? Op::Between : Op::In, e.origin, std::monostate{},
                        between ? std::size_t(symmetric) : e.operands.size() - 1});
      if (negate) instruction(out, {Op::Not, e.origin});
      return;
    }
    default:
      break;
    }
    auto operation = scalarOperation(e);
    if (!operation)
      fail(e.origin, "expression is outside the supported local scalar operations");
    if (*operation == Op::Negative) {
      auto operand = e.operands.front();
      bool negative = true;
      auto operandDepth = depth;
      while (true) {
        if (++operandDepth >= limits.maxDepth)
          fail(e.origin, "local expression nesting limit exceeded", DiagnosticCode::ResourceLimit);
        const auto &inner = modelExpression(plan, {ref.query, operand});
        step(inner.origin);
        if (inner.kind == ExprKind::Number) {
          instruction(out, {Op::Literal, e.origin,
              integerLiteral(e,negative?"-"+inner.value:inner.value)});
          return;
        }
        if (sqlite || inner.kind != ExprKind::Unary || inner.value != "-" ||
            inner.operands.size() != 1)
          break;
        negative = !negative;
        operand = inner.operands.front();
      }
    }
    for (auto operand : e.operands)
      compileScalar(out, {ref.query, operand}, input, projected, depth + 1, aggregate);
    if (sqlite && (*operation == Op::Distinct || *operation == Op::NotDistinct) &&
        modelExpression(plan, {ref.query, e.operands[1]}).kind == ExprKind::Boolean)
      operation = *operation == Op::Distinct ? Op::SqliteTruthDifferent : Op::SqliteTruthEqual;
    const auto &type=plan.program.types.get(e.sqlType);
    if (!sqlite && numericType(type)) {
      std::optional<Op> numeric;
      switch (*operation) {
      case Op::Positive:numeric=Op::ExactNumeric;break;
      case Op::Negative:numeric=Op::NumericNegative;break;
      case Op::Add:numeric=Op::NumericAdd;break;
      case Op::Subtract:numeric=Op::NumericSubtract;break;
      case Op::Multiply:numeric=Op::NumericMultiply;break;
      case Op::Divide:numeric=Op::NumericDivide;break;
      case Op::Modulo:numeric=Op::NumericModulo;break;
      default:break;
      }
      if (numeric) {
        instruction(out,{*numeric,e.origin});
        return;
      }
    }
    std::size_t width = 0;
    if (arithmetic(*operation)) {
      if (sqlite)
        width = 64;
      else
        width = signedIntegerBits(type).value_or(0);
    }
    instruction(out, {*operation, e.origin, std::monostate{}, width});
  }
  void compileCase(Scalar &out, QueryId query, const ModelExpression &e,
                   const RelNode *input, bool projected, std::size_t depth,
                   const ComputedBindings *aggregate) {
    const bool simple = e.value == "simple";
    const std::size_t first = simple ? 1 : 0;
    if ((!simple && e.value != "searched") || e.operands.size() < first + 3 ||
        (e.operands.size() - first) % 2 != 1)
      fail(e.origin, "invalid CASE expression shape", DiagnosticCode::InvalidQueryShape);
    const auto compileOperand = [&](ExprId id) {
      compileScalar(out, {query, id}, input, projected, depth, aggregate);
    };
    const auto inferred = [&](ExprId id) { return modelExpression(plan, {query, id}).type; };
    const auto validatePredicate = [&](ExprId condition) {
      if (sqlite) return;
      const auto type = inferred(condition);
      if (simple) {
        const auto selector = inferred(e.operands.front());
        if ((selector == ValueType::Integer && type == ValueType::Boolean) ||
            (selector == ValueType::Boolean && type == ValueType::Integer))
          fail(modelExpression(plan, {query, condition}).origin,
               "CASE selector and WHEN value use incompatible types", DiagnosticCode::InvalidQueryShape);
        return;
      }
      if (type != ValueType::Boolean && type != ValueType::Null && type != ValueType::Unknown)
        fail(modelExpression(plan, {query, condition}).origin,
             "CASE WHEN requires a Boolean predicate", DiagnosticCode::InvalidQueryShape);
    };
    bool sawInteger = false, sawBoolean = false;
    const auto branchType = [&](ExprId value) {
      sawInteger |= inferred(value) == ValueType::Integer;
      sawBoolean |= inferred(value) == ValueType::Boolean;
    };
    if (simple) compileOperand(e.operands.front());
    std::vector<std::size_t> finish;
    charge(e.origin, e.operands.size() * sizeof(std::size_t));
    for (std::size_t i = first; i + 1 < e.operands.size() - 1; i += 2) {
      validatePredicate(e.operands[i]);
      if (simple) instruction(out, {Op::Duplicate, e.origin});
      compileOperand(e.operands[i]);
      if (simple) instruction(out, {Op::Equal, e.origin});
      const auto next = out.size();
      instruction(out, {Op::JumpUnlessTrue, modelExpression(plan, {query, e.operands[i]}).origin});
      if (simple) instruction(out, {Op::Pop, e.origin});
      branchType(e.operands[i + 1]);
      compileOperand(e.operands[i + 1]);
      finish.push_back(out.size());
      instruction(out, {Op::Jump, e.origin});
      out[next].argument = out.size();
    }
    if (simple) instruction(out, {Op::Pop, e.origin});
    branchType(e.operands.back());
    compileOperand(e.operands.back());
    if (!sqlite && sawInteger && sawBoolean)
      fail(e.origin, "CASE results have incompatible integer and Boolean types", DiagnosticCode::InvalidQueryShape);
    for (auto jump : finish) out[jump].argument = out.size();
  }
  static std::optional<Op> comparisonOperation(std::string_view name) {
    if (name == "=") return Op::Equal;
    if (name == "<>" || name == "!=") return Op::NotEqual;
    if (name == "<") return Op::Less;
    if (name == "<=") return Op::LessEqual;
    if (name == ">") return Op::Greater;
    if (name == ">=") return Op::GreaterEqual;
    return {};
  }
  static std::optional<Op> scalarOperation(const ModelExpression &e) {
    if (e.operands.size() == 1) {
      if (e.kind == ExprKind::IsNull) {
        if (e.value == "IS NULL") return Op::IsNull;
        if (e.value == "IS NOT NULL") return Op::IsNotNull;
      }
      if (e.kind == ExprKind::Unary) {
        if (e.value == "NOT") return Op::Not;
        if (e.value == "+") return Op::Positive;
        if (e.value == "-") return Op::Negative;
      }
      return {};
    }
    if (e.operands.size() != 2 || e.kind != ExprKind::Binary) return {};
    const auto &name = e.value;
    if (name == "+") return Op::Add;
    if (name == "-") return Op::Subtract;
    if (name == "*") return Op::Multiply;
    if (name == "/") return Op::Divide;
    if (name == "%") return Op::Modulo;
    if (name == "AND") return Op::And;
    if (name == "OR") return Op::Or;
    if (name == "IS DISTINCT FROM") return Op::Distinct;
    if (name == "IS NOT DISTINCT FROM") return Op::NotDistinct;
    return comparisonOperation(name);
  }
  LocalValue unary(const Instruction &instruction,const LocalValue &a) {
    const auto at = instruction.origin;
    switch (instruction.op) {
    case Op::IsNull: return boolean(isNull(a));
    case Op::IsNotNull: return boolean(!isNull(a));
    case Op::ExactNumeric:
      if (isNull(a) || std::holds_alternative<LocalNumeric>(a)) return a;
      charge(at,LocalNumeric::storageBytes);
      return numericValue(a,at);
    case Op::TypedNumeric: {
      if (isNull(a)) return a;
      charge(at,LocalNumeric::storageBytes);
      const auto scale=static_cast<int>(std::get<std::int64_t>(instruction.literal));
      auto result=numericValue(a,at).withTypeModifier(static_cast<int>(instruction.argument),scale);
      if (!result) fail(at,"local numeric value exceeds the cast precision",DiagnosticCode::InvalidQueryShape);
      return std::move(*result);
    }
    case Op::NumericToInteger: {
      if (isNull(a)) return a;
      charge(at,LocalNumeric::storageBytes+LocalNumeric::maxFormattedBytes);
      const auto rounded=numericValue(a,at).withTypeModifier(27,0);
      const auto value=rounded?exactIntegerText(rounded->decimal()):std::nullopt;
      if (!value || !fitsSignedInteger(*value,static_cast<int>(instruction.argument)))
        fail(at,"local numeric value exceeds the integer cast range",DiagnosticCode::InvalidQueryShape);
      return *value;
    }
    case Op::NumericNegative: {
      if (isNull(a)) return a;
      charge(at,LocalNumeric::storageBytes);
      LocalNumeric value;
      value.subtract(numericValue(a,at));
      return value;
    }
    case Op::TruthTest:
      return boolean((truth(a, at) == truth(instruction.literal, at)) != bool(instruction.argument));
    case Op::Not: {
      const auto t = truth(a, at);
      return t == Truth::Null ? a : boolean(t == Truth::False);
    }
    default:
      break;
    }
    if (isNull(a)) return a;
    return evalInteger(instruction, LocalValue{std::int64_t(0)}, a);
  }
  static bool arithmetic(Op op) {
    return op == Op::Positive || op == Op::Negative || op == Op::Add ||
           op == Op::Subtract || op == Op::Multiply || op == Op::Divide || op == Op::Modulo;
  }
  LocalValue evalInteger(const Instruction &instruction, const LocalValue &a,
                         const LocalValue &b) const {
    const auto at = instruction.origin;
    const auto x = std::get_if<std::int64_t>(&a), y = std::get_if<std::int64_t>(&b);
    if (!x || !y || !instruction.argument)
      fail(at, "local arithmetic requires a resolved signed integer result type");
    const auto maximum = instruction.argument == 64 ? std::numeric_limits<std::int64_t>::max()
        : (std::int64_t(1) << (instruction.argument - 1)) - 1;
    const auto minimum = -maximum - 1;
    const auto overflow = [&]() -> LocalValue {
      fail(at, sqlite ? "SQLite arithmetic would promote to an unsupported REAL result"
                     : "local arithmetic exceeds the PostgreSQL integer result range",
           DiagnosticCode::InvalidQueryShape);
    };
    if (*x < minimum || *x > maximum || *y < minimum || *y > maximum) return overflow();
    switch (instruction.op) {
    case Op::Positive: return *y;
    case Op::Add:
      if ((*y > 0 && *x > maximum - *y) || (*y < 0 && *x < minimum - *y)) return overflow();
      return *x + *y;
    case Op::Negative:
    case Op::Subtract:
      if ((*y < 0 && *x > maximum + *y) || (*y > 0 && *x < minimum + *y)) return overflow();
      return *x - *y;
    case Op::Multiply: {
      const auto magnitude = [](std::int64_t value) {
        return value < 0 ? std::uint64_t(0) - std::uint64_t(value) : std::uint64_t(value);
      };
      const auto bound = std::uint64_t(maximum) + ((*x < 0) != (*y < 0));
      if (*y && magnitude(*x) > bound / magnitude(*y)) return overflow();
      return *x * *y;
    }
    case Op::Divide:
    case Op::Modulo:
      if (!*y) {
        if (sqlite) return std::monostate{};
        fail(at, "local integer division by zero", DiagnosticCode::InvalidQueryShape);
      }
      if (*x == minimum && *y == -1)
        return instruction.op == Op::Modulo ? LocalValue{std::int64_t(0)} : overflow();
      return instruction.op == Op::Divide ? *x / *y : *x % *y;
    default:
      fail(at, "invalid local integer operator", DiagnosticCode::InvalidQueryShape);
    }
  }
  LocalValue binary(const Instruction &instruction,const LocalValue &a,const LocalValue &b) {
    const auto op = instruction.op;
    const auto at = instruction.origin;
    if (op == Op::And || op == Op::Or) {
      const auto l = truth(a, at), r = truth(b, at);
      const auto decisive = op == Op::And ? Truth::False : Truth::True;
      if (l == decisive || r == decisive) return truthValue(decisive);
      if (l == Truth::Null || r == Truth::Null) return std::monostate{};
      return truthValue(l);
    }
    if (op == Op::SqliteTruthEqual || op == Op::SqliteTruthDifferent) {
      const auto equal = truth(a, at) == truth(b, at);
      return boolean(op == Op::SqliteTruthEqual ? equal : !equal);
    }
    if (isNull(a) || isNull(b)) {
      if (op == Op::Distinct) return boolean(isNull(a) != isNull(b));
      if (op == Op::NotDistinct) return boolean(isNull(a) == isNull(b));
      return std::monostate{};
    }
    if (op==Op::NumericAdd || op==Op::NumericSubtract) {
      charge(at,LocalNumeric::storageBytes);
      auto result=numericValue(a,at);
      const auto rhs=numericValue(b,at);
      const bool fits=op==Op::NumericAdd?result.add(rhs):result.subtract(rhs);
      if (!fits) fail(at,"local numeric result exceeds 27 digits",DiagnosticCode::ResourceLimit);
      return result;
    }
    if (op==Op::NumericMultiply) {
      charge(at,LocalNumeric::storageBytes);
      auto result=numericValue(a,at).multipliedBy(numericValue(b,at));
      if (!result) fail(at,"local numeric product exceeds its decimal range",DiagnosticCode::ResourceLimit);
      return std::move(*result);
    }
    if (op==Op::NumericDivide || op==Op::NumericModulo) {
      step(at,64);
      charge(at,LocalNumeric::storageBytes);
      const auto left=numericValue(a,at), right=numericValue(b,at);
      if (right==LocalNumeric{})
        fail(at,"local numeric division by zero",DiagnosticCode::InvalidQueryShape);
      auto result=op==Op::NumericDivide?left.dividedBy(right):left.remainder(right);
      if (!result) fail(at,"local numeric quotient exceeds its decimal range",DiagnosticCode::ResourceLimit);
      return std::move(*result);
    }
    if (arithmetic(op)) return evalInteger(instruction, a, b);
    requireNonText(a, at);
    requireNonText(b, at);
    const auto comparison=[&]() {
      if (a.index()==b.index()) return a==b?0:a<b?-1:1;
      const auto *left=std::get_if<LocalNumeric>(&a);
      const auto *right=std::get_if<LocalNumeric>(&b);
      const auto *leftInteger=std::get_if<std::int64_t>(&a);
      const auto *rightInteger=std::get_if<std::int64_t>(&b);
      if (left && rightInteger) return left->compare(*rightInteger);
      if (right && leftInteger) return -right->compare(*leftInteger);
      fail(at,"local comparison requires compatible value types",DiagnosticCode::InvalidQueryShape);
    }();
    switch (op) {
    case Op::Equal: case Op::NotDistinct: return boolean(comparison==0);
    case Op::Distinct: case Op::NotEqual: return boolean(comparison!=0);
    case Op::Less: return boolean(comparison<0);
    case Op::LessEqual: return boolean(comparison<=0);
    case Op::Greater: return boolean(comparison>0);
    case Op::GreaterEqual: return boolean(comparison>=0);
    default: break;
    }
    fail(at, "invalid local scalar operator", DiagnosticCode::InvalidQueryShape);
  }
  SubqueryRows &subqueryRows(RelId root, SourceRange origin, std::optional<SubqueryRows> &uncached) {
    if (!working.empty()) {
      uncached.emplace(SubqueryRows{execute(root, relationalDepth + 1), std::nullopt});
      return *uncached;
    }
    auto &cache=capturedRows.empty()?subqueryCache:capturedRows.back().subqueries;
    if (const auto known = cache.find(root); known != cache.end()) return known->second;
    auto rows = execute(root, relationalDepth + 1);
    charge(origin, sizeof(SubqueryRows) + 3 * sizeof(void *));
    return cache.emplace(root, SubqueryRows{std::move(rows), std::nullopt}).first->second;
  }
  LocalValue subqueryValue(const Instruction &op,const LocalValue &value,const LocalRow &input) {
    std::optional<SubqueryRows> uncached;
    const auto root=static_cast<RelId>(op.argument);
    if (op.capturedInput)
      uncached.emplace(SubqueryRows{executeDependent(root,relationalDepth+1,*op.capturedInput,input),
                                   std::nullopt});
    auto &result=op.capturedInput?*uncached:subqueryRows(root,op.origin,uncached);
    if (op.op == Op::ExistsSubquery) return boolean(!result.rows.empty());
    if (result.rows.empty()) return op.op == Op::InSubquery ? boolean(false) : LocalValue{};
    if (op.op == Op::ScalarSubquery) {
      if (!sqlite && result.rows.size() > 1)
        fail(op.origin, "scalar subquery returned more than one row", DiagnosticCode::InvalidQueryShape);
      return result.rows.front().at(0);
    }
    if (isNull(value)) return std::monostate{};
    requireNonText(value, op.origin);
    if (!result.membership) {
      auto &members = result.membership.emplace();
      for (const auto &row : result.rows) {
        step(op.origin);
        const auto &cell = row.at(0);
        requireNonText(cell, op.origin);
        if (!isNull(cell) && cell.index() != value.index())
          fail(op.origin, "local IN subquery rows have incompatible scalar types", DiagnosticCode::InvalidQueryShape);
        if (members.count(cell)) continue;
        charge(op.origin, sizeof(LocalValue) + 3 * sizeof(void *));
        members.insert(cell);
      }
    }
    if (result.membership->count(value)) return boolean(true);
    return result.membership->count(LocalValue{}) ? LocalValue{} : boolean(false);
  }
  LocalValue eval(const Scalar &scalar, const LocalRow &row) {
    const auto level = evaluationDepth++;
    struct Leave { std::size_t &depth; ~Leave() { --depth; } } leave{evaluationDepth};
    if (level >= limits.maxDepth)
      fail(scalar.front().origin, "local scalar subquery nesting limit exceeded", DiagnosticCode::ResourceLimit);
    if (evaluationStacks.size() == level) {
      charge(scalar.front().origin, sizeof(LocalRow));
      evaluationStacks.emplace_back();
    }
    auto &stack = evaluationStacks.at(level);
    stack.clear();
    if (stack.capacity() < scalar.size()) {
      charge(scalar.front().origin, scalar.size() * sizeof(LocalValue));
      stack.reserve(scalar.size());
    }
    for (std::size_t index = 0; index < scalar.size();) {
      const auto &op = scalar[index++];
      step(op.origin);
      switch (op.op) {
      case Op::Literal: stack.push_back(op.literal); break;
      case Op::Load: stack.push_back(row.at(op.argument)); break;
      case Op::ScalarSubquery: case Op::ExistsSubquery:
        stack.push_back(subqueryValue(op,LocalValue{},row));
        break;
      case Op::InSubquery:
        stack.back() = subqueryValue(op,stack.back(),row);
        break;
      case Op::ArrayIndex: {
        const auto &array=arrayLiterals.at(op.argument);
        const auto count=static_cast<std::size_t>(std::get<std::int64_t>(op.literal));
        const auto begin=stack.size()-count;
        std::optional<std::size_t> position;
        if (count==array.dimensions.size()) {
          position=0;
          for (std::size_t axis=0;axis<count;++axis) {
            const auto &dimension=array.dimensions[axis];
            const auto *index=std::get_if<std::int64_t>(&stack[begin+axis]);
            if (!index || *index<dimension.lower ||
                static_cast<std::uint64_t>(*index-dimension.lower)>=dimension.size) {
              position.reset();
              break;
            }
            *position=*position*dimension.size+static_cast<std::size_t>(*index-dimension.lower);
          }
        }
        const auto value=position?array.elements.at(*position):std::nullopt;
        stack.resize(begin);
        stack.push_back(value?LocalValue{*value}:LocalValue{});
        break;
      }
      case Op::ArrayCompare: {
        const auto &array=arrayLiterals.at(op.argument);
        if (array.nullValue) {
          stack.back()=std::monostate{};
          break;
        }
        const auto packed=std::get<std::int64_t>(op.literal);
        const auto operation=static_cast<Op>(packed/2);
        const auto identity=packed%2?Truth::True:Truth::False;
        const auto decisive=packed%2?Truth::False:Truth::True;
        auto result=identity;
        for (const auto &item:array.elements) {
          step(op.origin);
          const auto element=item?LocalValue{*item}:LocalValue{};
          const auto compared=truth(binary({operation,op.origin},stack.back(),element),op.origin);
          if (compared==decisive) {
            result=decisive;
            break;
          }
          if (compared==Truth::Null) result=Truth::Null;
        }
        stack.back()=truthValue(result);
        break;
      }
      case Op::Duplicate: stack.push_back(stack.back()); break;
      case Op::Pop: stack.pop_back(); break;
      case Op::Jump: index = op.argument; break;
      case Op::JumpUnlessTrue:
        if (truth(stack.back(), op.origin) != Truth::True) index = op.argument;
        stack.pop_back();
        break;
      case Op::Positive: case Op::Negative: case Op::ExactNumeric: case Op::TypedNumeric:
      case Op::NumericToInteger: case Op::NumericNegative: case Op::Not:
      case Op::IsNull: case Op::IsNotNull: case Op::TruthTest:
        stack.back() = unary(op, stack.back());
        break;
      case Op::Between: {
        const auto high = stack.back();
        stack.pop_back();
        const auto low = stack.back();
        stack.pop_back();
        const auto value = stack.back();
        auto within = [&](const LocalValue &from, const LocalValue &to) {
          return binary({Op::And, op.origin},
                        binary({Op::GreaterEqual, op.origin}, value, from),
                        binary({Op::LessEqual, op.origin}, value, to));
        };
        auto result = within(low, high);
        if (op.argument) result = binary({Op::Or, op.origin}, result, within(high, low));
        stack.back() = result;
        break;
      }
      case Op::In: {
        const auto slot = stack.size() - op.argument - 1;
        auto result = Truth::False;
        const auto comparisonDomain=[](const LocalValue &value) {
          return std::holds_alternative<LocalNumeric>(value)?LocalValue{std::int64_t{0}}.index():value.index();
        };
        auto comparisonType = comparisonDomain(stack[slot]);
        for (auto i = slot + 1; i < stack.size(); ++i) {
          step(op.origin);
          if (!isNull(stack[i])) {
            if (comparisonType && comparisonType != comparisonDomain(stack[i]))
              fail(op.origin, "local IN list needs compatible scalar types", DiagnosticCode::InvalidQueryShape);
            comparisonType = comparisonDomain(stack[i]);
          }
          const auto match = truth(binary({Op::Equal, op.origin}, stack[slot], stack[i]), op.origin);
          if (match == Truth::True || result == Truth::False) result = match;
        }
        stack.resize(slot + 1);
        stack.back() = truthValue(result);
        break;
      }
      default: {
        auto right = stack.back();
        stack.pop_back();
        stack.back() = binary(op, stack.back(), right);
        break;
      }
      }
    }
    return stack.back();
  }
  std::vector<Scalar> compileAll(const std::vector<ExpressionRef> &keys, const RelNode *input,
                                 bool projected, SourceRange at, const ComputedBindings *aggregate = nullptr) {
    charge(at, keys.size() * sizeof(Scalar));
    std::vector<Scalar> expressions(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
      auto &expression = expressions[i];
      compileScalar(expression, keys[i], input, projected, 0, aggregate);
      if (expression.size() > 1 && std::none_of(expression.begin(), expression.end(),
              [](const auto &op) { return op.op == Op::Load; })) {
        const auto origin = expression.back().origin;
        auto value = eval(expression, {});
        expression.resize(1);
        expression.front() = {Op::Literal, origin, std::move(value)};
      }
    }
    return expressions;
  }
  void compatible(const Rows &rows, std::vector<std::size_t> &types, SourceRange at) {
    for (const auto &row : rows) {
      if (row.size() != types.size())
        fail(at, "local operator output shape is inconsistent", DiagnosticCode::InvalidQueryShape);
      step(at, row.size() + 1);
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (isNull(row[i])) continue;
        requireNonText(row[i], at);
        if (types[i] && types[i] != row[i].index())
          fail(at, "local output columns require consistent value types", DiagnosticCode::InvalidQueryShape);
        types[i] = row[i].index();
      }
    }
  }
  std::vector<std::size_t> numericColumns(const RelNode &n) {
    const auto &query=modelQuery(plan,n.query);
    std::vector<std::size_t> positions;
    if (sqlite || query.projections.size()!=n.outputs.size()) return positions;
    for (std::size_t i=0;i<query.projections.size();++i) {
      const auto &e=modelExpression(plan,{query.id,query.projections[i].expression});
      step(e.origin);
      if (numericType(plan.program.types.get(e.sqlType))) {
        charge(e.origin,sizeof(std::size_t));
        positions.push_back(i);
      }
    }
    return positions;
  }
  void normalizeNumericRows(Rows &rows,const std::vector<std::size_t> &positions,SourceRange origin) {
    if (positions.empty()) return;
    for (auto &row:rows) {
      step(origin,positions.size()+1);
      for (auto i:positions) {
        auto &value=row.at(i);
        if (!isNull(value) && !std::holds_alternative<LocalNumeric>(value)) {
          charge(origin,LocalNumeric::storageBytes);
          value=numericValue(value,origin);
        }
      }
    }
  }
  Rows values(const RelNode &n) {
    const auto width = n.outputs.size();
    rowBudget(n.origin, width, n.valueCount);
    if (n.keys.empty() && !n.valueCount) return {};
    if (n.keys.empty() && !width && n.valueCount == 1) return {LocalRow{}};
    if (!width || n.keys.size() != width * n.valueCount)
      fail(n.origin, "local VALUES shape is inconsistent", DiagnosticCode::InvalidQueryShape);
    Rows out;
    out.reserve(n.valueCount);
    std::size_t index = 0;
    for (std::size_t i = 0; i < n.valueCount; ++i) {
      LocalRow row;
      row.reserve(width);
      for (std::size_t j = 0; j < width; ++j) {
        Scalar value;
        compileScalar(value, n.keys.at(index++), nullptr, false);
        row.push_back(eval(value, {}));
      }
      out.push_back(std::move(row));
    }
    normalizeNumericRows(out,numericColumns(n),n.origin);
    std::vector<std::size_t> types(width);
    compatible(out,types,n.origin);
    return out;
  }
  void emitExpansionRow(Rows &rows,const RelNode &n,const LocalRow &input,
                        LocalValue value,std::size_t ordinal) {
    step(n.origin,n.outputs.size()+1);
    LocalRow row;
    row.reserve(n.outputs.size());
    row.insert(row.end(),input.begin(),input.end());
    row.push_back(std::move(value));
    if (n.withOrdinality) row.push_back(static_cast<std::int64_t>(ordinal));
    rows.push_back(std::move(row));
  }
  Rows integerSeries(const RelNode &n,std::size_t depth) {
    if (sqlite || n.inputs.empty() || n.keys.size()<2 || n.keys.size()>3)
      fail(n.origin,"invalid local PostgreSQL integer series",DiagnosticCode::InvalidQueryShape);
    const auto &input=plan.nodes.at(n.inputs.front());
    if (n.outputs.size()!=input.outputs.size()+(n.withOrdinality?2:1))
      fail(n.origin,"invalid local integer series output width",DiagnosticCode::InvalidQueryShape);
    const auto expressions=compileAll(n.keys,&input,false,n.origin);
    const auto source=execute(n.inputs.front(),depth+1);
    Rows rows;
    for (const auto &row:source) {
      std::int64_t values[]{0,0,1};
      bool empty=false;
      for (std::size_t i=0;i<expressions.size();++i) {
        const auto value=eval(expressions[i],row);
        if (const auto *integer=std::get_if<std::int64_t>(&value)) values[i]=*integer;
        else if (isNull(value)) empty=true;
        else fail(n.origin,"local series requires signed integer inputs",DiagnosticCode::InvalidQueryShape);
      }
      if (empty) continue;
      if (!values[2]) fail(n.origin,"PostgreSQL generate_series step size cannot equal zero",DiagnosticCode::InvalidQueryShape);
      const auto count=integerSeriesCount(values[0],values[1],values[2]);
      if (!count || *count>limits.maxRows-rows.size())
        fail(n.origin,"local integer series exceeded its output row budget",DiagnosticCode::ResourceLimit);
      rowBudget(n.origin,n.outputs.size(),static_cast<std::size_t>(*count));
      auto value=values[0];
      for (std::size_t i=0;i<*count;++i) {
        emitExpansionRow(rows,n,row,value,i+1);
        if (i+1<*count) value+=values[2]; // The next value remains within the verified start/stop bounds.
      }
    }
    return rows;
  }
  Rows unnest(const RelNode &n,std::size_t depth) {
    const auto query=postgresIntegerArrayQuery(plan,n);
    if (sqlite || n.keys.size()!=1 || (n.inputs.size()!=1 && !query))
      fail(n.origin,"invalid local PostgreSQL array expansion",DiagnosticCode::InvalidQueryShape);
    const auto &inputNode=plan.nodes.at(n.inputs.front());
    const auto &input=inputNode.outputs;
    if (n.outputs.size()!=input.size()+(n.withOrdinality?2:1) ||
        !std::equal(input.begin(),input.end(),n.outputs.begin(),[](const auto &a,const auto &b) {
          return a.query==b.query && a.table==b.table && a.name==b.name;
        }))
      fail(n.origin,"local array expansion changes input field identity",DiagnosticCode::InvalidQueryShape);
    if (query) {
      const auto source=execute(n.inputs.front(),depth+1);
      Rows rows;
      for (const auto &inputRow:source) {
        std::optional<SubqueryRows> temporary;
        if (query->correlated)
          temporary.emplace(SubqueryRows{executeDependent(query->root,depth+1,inputNode,inputRow),std::nullopt});
        const auto &items=query->correlated?temporary->rows:subqueryRows(query->root,n.origin,temporary).rows;
        if (items.size()>limits.maxRows-rows.size())
          fail(n.origin,"local UNNEST exceeded its output row budget",DiagnosticCode::ResourceLimit);
        rowBudget(n.origin,n.outputs.size(),items.size());
        std::size_t ordinal=0;
        for (const auto &item:items) {
          if (item.size()!=1 || (!isNull(item.front()) && !std::holds_alternative<std::int64_t>(item.front())))
            fail(n.origin,"local ARRAY query returned a value outside its signed integer type",DiagnosticCode::InvalidQueryShape);
          emitExpansionRow(rows,n,inputRow,item.front(),++ordinal);
        }
      }
      return rows;
    }
    const auto elementBudget=std::min(limits.maxRows,(limits.maxBytes-bytes)/(2*sizeof(std::optional<std::int64_t>)));
    const auto literal=postgresIntegerArrayLiteral(plan,n.keys.front(),elementBudget,
        [&](std::size_t work) { step(n.origin,work); });
    if (literal.status==IntegerArrayStatus::ResourceLimit)
      fail(n.origin,"local array literal exceeds the expansion budget",DiagnosticCode::ResourceLimit);
    if (literal.status!=IntegerArrayStatus::Ready)
      fail(n.origin,"local UNNEST requires a rectangular signed integer array literal or a single-column signed integer ARRAY query");
    charge(n.origin,literal.elements.size()*sizeof(literal.elements.front()));
    const auto source=execute(n.inputs.front(),depth+1);
    const auto count=literal.elements.size();
    if (count && source.size()>limits.maxRows/count)
      fail(n.origin,"local UNNEST exceeded its output row budget",DiagnosticCode::ResourceLimit);
    rowBudget(n.origin,n.outputs.size(),source.size()*count);
    Rows rows;
    rows.reserve(source.size()*count);
    for (const auto &inputRow:source)
      for (std::size_t i=0;i<count;++i) {
        const auto &element=literal.elements[i];
        emitExpansionRow(rows,n,inputRow,element?LocalValue{*element}:LocalValue{std::monostate{}},i+1);
      }
    return rows;
  }
  Rows projection(const RelNode &n, const RelNode &input, Rows rows,
                  const ComputedBindings *computed = nullptr) {
    if (!computed && n.keys.size() == 1) {
      const auto &key = modelExpression(plan, n.keys.front());
      const auto qualifiedStar=[&] {
        if (key.name.size()!=2) return false;
        const auto &query=modelQuery(plan,n.query);
        const auto &first=input.outputs.front();
        if (first.query!=n.query || first.table>=query.tables.size()) return false;
        const auto &table=query.tables[first.table];
        return (table.unpivot || table.pivot) && table.alias==key.name.front() &&
            std::all_of(input.outputs.begin(),input.outputs.end(),[&](const auto &field) {
              return field.query==first.query && field.table==first.table;
            });
      };
      if (key.kind == ExprKind::Star && !input.outputs.empty() &&
          (key.name.empty() || qualifiedStar()) &&
          n.outputs.size() == input.outputs.size() &&
          std::equal(n.outputs.begin(), n.outputs.end(), input.outputs.begin(), [](const auto &a, const auto &b) {
            return a.query == b.query && a.table == b.table && a.name == b.name;
          })) return rows;
    }
    if (n.keys.size() != n.outputs.size())
      fail(n.origin, "local projection requires explicit output columns");
    const auto expressions = compileAll(n.keys, &input, false, n.origin, computed);
    rowBudget(n.origin, expressions.size(), rows.size());
    for (auto &row : rows) {
      step(n.origin);
      LocalRow result;
      result.reserve(expressions.size());
      for (const auto &value : expressions)
        result.push_back(eval(value, row));
      row = std::move(result);
    }
    return rows;
  }
  std::vector<Scalar> compilePredicates(const RelNode &n, const RelNode &input,
                                        const ComputedBindings *aggregate = nullptr) {
    auto expressions = compileAll(n.predicates, &input, false, n.origin, aggregate);
    if (!sqlite) {
      for (auto ref : n.predicates) {
        const auto &e = modelExpression(plan, ref);
        if (e.type != ValueType::Boolean && e.type != ValueType::Null && e.type != ValueType::Unknown)
          fail(e.origin, "local predicate requires a Boolean value", DiagnosticCode::InvalidQueryShape);
      }
    }
    return expressions;
  }
  bool matches(const std::vector<Scalar> &expressions, const LocalRow &row) {
    bool kept = true;
    for (const auto &expression : expressions)
      kept &= truth(eval(expression, row), expression.back().origin) == Truth::True;
    return kept;
  }
  Rows filter(const RelNode &n, const RelNode &input, Rows rows) {
    const auto expressions = compilePredicates(n, input);
    const auto firstDropped = std::remove_if(rows.begin(), rows.end(), [&](const auto &row) {
      step(n.origin);
      return !matches(expressions, row);
    });
    rows.erase(firstDropped, rows.end());
    return rows;
  }
  Rows scalarCalc(const RelNode &n,const RelNode &input,Rows rows) {
    const auto conditions=compilePredicates(n,input);
    const auto projected=compileAll(n.keys,&input,false,n.origin);
    std::size_t kept=0;
    for (auto &row:rows) {
      step(n.origin);
      if (!matches(conditions,row)) continue;
      auto &target=rows[kept++];
      if (projected.empty()) {
        if (&target!=&row) target=std::move(row);
      } else {
        rowBudget(n.origin,projected.size(),kept);
        LocalRow values;
        values.reserve(projected.size());
        for (const auto &value:projected) values.push_back(eval(value,row));
        target=std::move(values);
      }
    }
    rows.resize(kept);
    return rows;
  }
  enum class AggregateKind { Count, Sum, SumNumeric, Average, Min, Max, Mode, PercentileDisc };
  struct AggregateTerm {
    AggregateKind kind;
    SourceRange origin;
    Scalar argument, filter;
    bool distinct;
    bool descending = false;
    std::optional<double> fraction;
  };
  struct OrderedCounts {
    std::map<std::int64_t,std::size_t> frequencies;
    std::size_t rows=0, modeFrequency=0;
  };
  struct Accumulator {
    LocalValue value;
    std::set<LocalValue> seen;
    std::uint32_t averageCount=0;
    std::unique_ptr<OrderedCounts> ordered;
  };
  std::optional<OperatorInfo> nativeOperator(const ModelExpression &call, RoutineKind kind) const {
    if (call.kind != ExprKind::Call || call.window ||
        (call.routineResolved && (sqlite || call.routineSchema != "pg_catalog" ||
         call.routineKind != kind || call.routineVolatility != FunctionVolatility::Immutable)))
      return std::nullopt;
    const auto builtin = builtinOperator(plan.program.sourceDialect, call.name, call.nameQuoted, call.operands.size());
    return builtin && builtin->kind == kind ? builtin : std::nullopt;
  }
  std::optional<double> literalFraction(ExpressionRef ref) {
    const auto *value=&modelExpression(plan,ref);
    const auto &type=plan.program.types.get(value->sqlType);
    if (value->kind==ExprKind::Cast && value->operands.size()==1 &&
        type.family==TypeFamily::Floating && type.bits==64 && !type.arrayDimensions && type.opaqueName.empty())
      value=&modelExpression(plan,{ref.query,value->operands.front()});
    bool negative=false;
    if (value->kind==ExprKind::Unary && value->operands.size()==1 &&
        (value->value=="+" || value->value=="-")) {
      negative=value->value=="-";
      value=&modelExpression(plan,{ref.query,value->operands.front()});
    }
    if (value->kind==ExprKind::Null) return {};
    if (value->kind!=ExprKind::Number || value->value.size()>128)
      fail(value->origin,"local discrete percentile needs a bounded numeric literal or NULL fraction");
    std::istringstream input(value->value);
    input.imbue(std::locale::classic());
    double fraction=0;
    if (!(input>>fraction) || !input.eof())
      fail(value->origin,"discrete percentile fraction is outside the supported double precision range");
    return negative?-fraction:fraction;
  }
  AggregateTerm aggregateTerm(ExpressionRef ref, const ModelExpression &expression,
                              const ModelExpression &call, AggregateKind kind,
                              const RelNode &input, const ComputedBindings *bindings = nullptr) {
    const bool percentile=kind==AggregateKind::PercentileDisc;
    const bool emptyCount=sqlite && kind==AggregateKind::Count && call.operands.empty() && !call.distinct;
    if (!emptyCount && call.operands.size() != (percentile?2u:1u))
      fail(expression.origin, "local aggregate requires one exact scalar");
    AggregateTerm term{kind, expression.origin, {}, {}, call.distinct, false, {}};
    if (!capturedRows.empty() && !bindings) {
      // PostgreSQL can attach an aggregate that uses only outer variables
      // to an enclosing query block. The local operator owns only this block.
      std::vector<ExprId> pending{ref.expression};
      while (!pending.empty()) {
        const auto &part=modelExpression(plan,{ref.query,pending.back()});
        pending.pop_back();
        step(part.origin);
        if (part.query || (part.binding && part.binding->scope!=ref.query))
          fail(part.origin,"local aggregate requires arguments owned by its query block");
        pending.insert(pending.end(),part.operands.begin(),part.operands.end());
      }
    }
    if (!emptyCount) {
      const ExpressionRef argument{ref.query, call.operands.back()};
      const auto &arg = modelExpression(plan, argument);
      const auto &argumentType=plan.program.types.get(arg.sqlType);
      if (!sqlite && kind==AggregateKind::Sum &&
          (signedIntegerBits(argumentType)==64 || numericType(argumentType)))
        term.kind=AggregateKind::SumNumeric;
      if (kind==AggregateKind::Average &&
          (sqlite || (!signedIntegerBits(argumentType) && !numericType(argumentType))))
        fail(arg.origin,"local AVG requires an exact PostgreSQL integer or numeric input");
      if (kind==AggregateKind::Mode || percentile) {
        if (!call.withinGroup || !signedIntegerBits(argumentType))
          fail(arg.origin,"local ordered-set aggregate requires a signed PostgreSQL integer ordering value");
        term.descending=call.withinGroup->descending;
        if (percentile) term.fraction=literalFraction({ref.query,call.operands.front()});
      }
      if (arg.kind != ExprKind::Star) {
        if (!sqlite && kind != AggregateKind::Count && arg.type == ValueType::Boolean)
          fail(arg.origin, "local PostgreSQL SUM/MIN/MAX requires an integer input");
        compileScalar(term.argument, argument, &input, false, 0, bindings);
      } else if (kind != AggregateKind::Count || call.distinct || arg.name.size() > 1) {
        fail(arg.origin, "local wildcard aggregate requires COUNT(*)");
      }
    }
    if (expression.kind == ExprKind::AggregateFilter) {
      if (expression.operands.size() != 2)
        fail(expression.origin, "invalid aggregate FILTER shape", DiagnosticCode::InvalidQueryShape);
      const ExpressionRef refFilter{ref.query, expression.operands.back()};
      const auto &predicate = modelExpression(plan, refFilter);
      if (!sqlite && predicate.type != ValueType::Boolean && predicate.type != ValueType::Null &&
          predicate.type != ValueType::Unknown)
        fail(predicate.origin, "aggregate FILTER requires a Boolean predicate", DiagnosticCode::InvalidQueryShape);
      compileScalar(term.filter, refFilter, &input, false, 0, bindings);
    }
    return term;
  }
  std::vector<AggregateTerm> aggregateTerms(const RelNode &project, const RelNode *having,
      const RelNode &input, ComputedBindings &bindings, std::vector<ExpressionRef> &grouping) {
    const auto &query = modelQuery(plan, project.query);
    std::vector<ExpressionRef> work = project.keys;
    if (having) work.insert(work.end(), having->predicates.begin(), having->predicates.end());
    std::set<ExprId> visited;
    std::vector<AggregateTerm> terms;
    while (!work.empty()) {
      const auto ref = work.back();
      work.pop_back();
      if (ref.query != query.id) fail(project.origin, "aggregate term is outside the query block");
      const auto &expression = modelExpression(plan, ref);
      step(expression.origin);
      if (!visited.insert(ref.expression).second) continue;
      if (expression.kind == ExprKind::Window && !expression.operands.empty()) {
        const auto &base = modelExpression(plan, {query.id, expression.operands.front()});
        const bool filtered = base.kind == ExprKind::AggregateFilter && base.operands.size() == 2;
        const auto &call = filtered ? modelExpression(plan, {query.id, base.operands.front()}) : base;
        for (auto child : call.operands) work.push_back({query.id, child});
        if (filtered) work.push_back({query.id, base.operands.back()});
        for (auto child = expression.operands.begin() + 1; child != expression.operands.end(); ++child)
          work.push_back({query.id, *child});
        continue;
      }
      const bool filtered = expression.kind == ExprKind::AggregateFilter;
      if (filtered && expression.operands.size() != 2)
        fail(expression.origin, "invalid aggregate FILTER shape", DiagnosticCode::InvalidQueryShape);
      const auto &call = filtered ? modelExpression(plan, {query.id, expression.operands.front()}) : expression;
      if (!sqlite && !filtered) {
        const auto scalar = nativeOperator(call, RoutineKind::Scalar);
        if (scalar && scalar->name == "GROUPING") {
          charge(expression.origin, sizeof(ExpressionRef));
          grouping.push_back(ref);
          continue;
        }
      }
      const auto builtin = nativeOperator(call, RoutineKind::Aggregate);
      if (!builtin) {
        for (auto child : expression.operands) work.push_back({query.id, child});
        continue;
      }
      const auto &name = builtin->name;
      const bool mode=name=="MODE" && call.withinGroup;
      const bool percentile=name=="PERCENTILE_DISC" && call.withinGroup;
      const bool emptyCount=sqlite && name=="COUNT" && call.operands.empty() && !call.distinct;
      if ((!emptyCount && call.operands.size()!=(percentile?2u:1u)) ||
          (!mode && !percentile && name!="COUNT" && name!="SUM" && name!="AVG" && name!="MIN" && name!="MAX"))
        fail(expression.origin,"local aggregate requires a supported function of one exact scalar");
      const auto kind=percentile?AggregateKind::PercentileDisc:mode?AggregateKind::Mode:
          name=="COUNT"?AggregateKind::Count:name=="SUM"?AggregateKind::Sum:
          name=="AVG"?AggregateKind::Average:name=="MIN"?AggregateKind::Min:AggregateKind::Max;
      charge(expression.origin, sizeof(AggregateTerm) + 2 * sizeof(std::size_t));
      bindings.terms.emplace(ref.expression, input.outputs.size() + terms.size());
      terms.push_back(aggregateTerm(ref, expression, call, kind, input));
    }
    return terms;
  }
  void accumulate(const AggregateTerm &term, Accumulator &result, const LocalRow &row) {
    step(term.origin);
    if (!term.filter.empty() && truth(eval(term.filter, row), term.filter.back().origin) != Truth::True) return;
    auto value = term.argument.empty() ? LocalValue{std::int64_t{1}} : eval(term.argument, row);
    if (isNull(value)) return;
    if (term.distinct) {
      requireNonText(value, term.origin);
      if (result.seen.count(value)) return;
      charge(term.origin, sizeof(LocalValue) + 3 * sizeof(void *));
      result.seen.insert(value);
    }
    if (term.kind == AggregateKind::Count) {
      result.value = isNull(result.value) ? std::int64_t{1} : std::get<std::int64_t>(result.value) + 1;
      return;
    }
    if (term.kind==AggregateKind::SumNumeric || term.kind==AggregateKind::Average) {
      if (isNull(result.value)) {
        charge(term.origin,LocalNumeric::storageBytes);
        result.value=LocalNumeric{};
      }
      auto &sum=std::get<LocalNumeric>(result.value);
      const auto *integer=std::get_if<std::int64_t>(&value);
      if (!(integer?sum.add(*integer):sum.add(numericValue(value,term.origin))))
        fail(term.origin,"local aggregate exceeded its decimal accumulator capacity",DiagnosticCode::ResourceLimit);
      if (term.kind==AggregateKind::Average) ++result.averageCount;
      return;
    }
    if (const auto *numeric=std::get_if<LocalNumeric>(&value);
        numeric && (term.kind==AggregateKind::Min || term.kind==AggregateKind::Max)) {
      if (isNull(result.value) ||
          (term.kind==AggregateKind::Min?*numeric<numericValue(result.value,term.origin)
                                       :*numeric>numericValue(result.value,term.origin)))
        result.value=*numeric;
      return;
    }
    const auto number = std::get_if<std::int64_t>(&value);
    if (!number || (!sqlite && term.kind == AggregateKind::Sum && !int32(*number)))
      fail(term.origin, "local numeric aggregate requires an exact supported integer input");
    if (term.kind==AggregateKind::Mode || term.kind==AggregateKind::PercentileDisc) {
      if (!result.ordered) {
        charge(term.origin,sizeof(OrderedCounts));
        result.ordered=std::make_unique<OrderedCounts>();
      }
      auto &counts=*result.ordered;
      auto [entry,inserted]=counts.frequencies.try_emplace(*number,0);
      if (inserted) charge(term.origin,sizeof(*entry)+4*sizeof(void *));
      const auto frequency=++entry->second;
      ++counts.rows;
      if (term.kind==AggregateKind::Mode &&
          (frequency > counts.modeFrequency ||
          (frequency == counts.modeFrequency && (term.descending ? *number > std::get<std::int64_t>(result.value)
                                                                : *number < std::get<std::int64_t>(result.value))))) {
        counts.modeFrequency=frequency;
        result.value=*number;
      }
      return;
    }
    if (isNull(result.value)) {
      result.value = *number;
      return;
    }
    auto &current = std::get<std::int64_t>(result.value);
    if (term.kind == AggregateKind::Min) current = std::min(current, *number);
    else if (term.kind == AggregateKind::Max) current = std::max(current, *number);
    else {
      if ((*number > 0 && current > std::numeric_limits<std::int64_t>::max() - *number) ||
          (*number < 0 && current < std::numeric_limits<std::int64_t>::min() - *number))
        fail(term.origin, "local SUM exceeds the exact integer range", DiagnosticCode::InvalidQueryShape);
      current += *number;
    }
  }
  LocalValue finishAggregate(const AggregateTerm &term,const Accumulator &state) {
    if (term.kind==AggregateKind::Count && isNull(state.value)) return std::int64_t{0};
    if (term.kind==AggregateKind::PercentileDisc) {
      if (!term.fraction) return {};
      const auto fraction=*term.fraction;
      if (!std::isfinite(fraction) || fraction<0 || fraction>1)
        fail(term.origin,"percentile fraction must be between 0 and 1",DiagnosticCode::InvalidQueryShape);
      if (!state.ordered || !state.ordered->rows) return {};
      const auto &frequencies=state.ordered->frequencies;
      auto rank=std::max(std::size_t{1},static_cast<std::size_t>(std::ceil(fraction*state.ordered->rows)));
      const auto visit=[&](auto begin,auto end) -> LocalValue {
        for (auto it=begin;it!=end;++it) {
          step(term.origin);
          if (rank<=it->second) return it->first;
          rank-=it->second;
        }
        fail(term.origin,"percentile count does not match its retained values",DiagnosticCode::InvalidQueryShape);
      };
      return term.descending?visit(frequencies.rbegin(),frequencies.rend()):visit(frequencies.begin(),frequencies.end());
    }
    if (term.kind!=AggregateKind::Average || !state.averageCount) return state.value;
    charge(term.origin,LocalNumeric::storageBytes);
    const auto average=std::get<LocalNumeric>(state.value).dividedByCount(state.averageCount);
    if (!average)
      fail(term.origin,"local AVG result exceeds its decimal scale budget",DiagnosticCode::ResourceLimit);
    return *average;
  }
  std::pair<const RelNode *, const RelNode *> groupedInput(const RelNode &input) const {
    if (input.kind == RelKind::Aggregate && !input.distinct) return {&input, nullptr};
    if (input.kind == RelKind::Filter && !input.inputs.empty()) {
      const auto &group = plan.nodes.at(input.inputs.front());
      if (group.kind == RelKind::Aggregate && !group.distinct) return {&group, &input};
    }
    return {nullptr, nullptr};
  }
  std::size_t groupingSlot(ExpressionRef ref, const RelNode &input) {
    const auto &query = modelQuery(plan, ref.query);
    const auto *key = &modelExpression(plan, ref);
    if (key->outputColumn && *key->outputColumn < query.projections.size())
      key = &modelExpression(plan, {query.id, query.projections[*key->outputColumn].expression});
    step(key->origin, input.outputs.size() + 1);
    if (key->kind != ExprKind::Name)
      fail(key->origin, "local GROUP BY requires directly bound columns");
    return binding(*key, &input);
  }
  GroupedRows aggregateRows(const RelNode &project, const RelNode &aggregate,
                            const RelNode *having, std::size_t depth) {
    if (aggregate.query != project.query || aggregate.inputs.size() != 1 ||
        (sqlite && !aggregate.groupingSets.empty()) || (having && having->query != project.query) ||
        project.keys.size() != project.outputs.size())
      fail(project.origin, "local aggregation requires explicit projection and simple grouping columns");
    if (having) checkScalarDependencies(*having, 1);
    const auto &input = plan.nodes.at(aggregate.inputs.front());
    ComputedBindings bindings{project.query, {}, std::set<std::size_t>{}};
    for (auto ref : aggregate.keys) {
      const auto slot = groupingSlot(ref, input);
      if (bindings.groupedColumns->insert(slot).second)
        charge(aggregate.origin, sizeof(std::size_t) + 3 * sizeof(void *));
    }
    std::vector<ExpressionRef> grouping;
    const auto terms = aggregateTerms(project, having, input, bindings, grouping);
    std::vector<std::vector<std::size_t>> maskColumns;
    const auto maskBase = input.outputs.size() + terms.size();
    for (auto ref : grouping) {
      const auto &call = modelExpression(plan, ref);
      if (call.operands.empty() || call.operands.size() > 31)
        fail(call.origin, "invalid GROUPING column count", DiagnosticCode::InvalidQueryShape);
      charge(call.origin, sizeof(std::vector<std::size_t>) + (2 + call.operands.size()) * sizeof(std::size_t));
      std::vector<std::size_t> columns;
      for (auto id : call.operands) {
        const auto slot = groupingSlot({ref.query, id}, input);
        if (!bindings.groupedColumns->count(slot))
          fail(call.origin, "GROUPING argument is outside the grouping columns", DiagnosticCode::InvalidQueryShape);
        columns.push_back(slot);
      }
      bindings.terms.emplace(ref.expression, maskBase + maskColumns.size());
      maskColumns.push_back(std::move(columns));
    }
    const auto restrictions = having ? compilePredicates(*having, input, &bindings) : std::vector<Scalar>{};
    const auto source = execute(aggregate.inputs.front(), depth + 1);
    Rows output;
    const auto width = maskBase + maskColumns.size();
    const auto appendGroups = [&](const std::vector<std::size_t> &slots) {
      std::map<LocalRow, std::vector<Accumulator>> groups;
      const auto createGroup = [&](const LocalRow &key) -> std::vector<Accumulator> & {
        if (groups.size() >= limits.maxRows)
          fail(aggregate.origin, "local grouping exceeds its row budget", DiagnosticCode::ResourceLimit);
        charge(project.origin, sizeof(LocalRow) + key.size() * sizeof(LocalValue) +
                               terms.size() * sizeof(Accumulator) + 4 * sizeof(void *));
        return groups.try_emplace(key, terms.size()).first->second;
      };
      if (slots.empty()) createGroup({});
      charge(project.origin, slots.size() * sizeof(LocalValue) + maskColumns.size() * sizeof(std::int64_t));
      LocalRow key(slots.size());
      for (const auto &row : source) {
        step(project.origin, slots.size() + 1);
        for (std::size_t i = 0; i < slots.size(); ++i) {
          key[i] = row.at(slots[i]);
          requireNonText(key[i], aggregate.origin);
        }
        const auto group = groups.find(key);
        auto &values = group == groups.end() ? createGroup(key) : group->second;
        for (std::size_t i = 0; i < terms.size(); ++i) accumulate(terms[i], values[i], row);
      }
      std::vector<std::int64_t> masks;
      for (const auto &columns : maskColumns) {
        std::int64_t mask = 0;
        for (auto slot : columns) {
          step(project.origin);
          mask = mask * 2 + !std::binary_search(slots.begin(), slots.end(), slot);
        }
        masks.push_back(mask);
      }
      rowBudget(project.origin, width, groups.size());
      for (const auto &[group, values] : groups) {
        step(project.origin, slots.size() + terms.size() + masks.size() + 1);
        LocalRow groupRow(width);
        for (std::size_t i = 0; i < slots.size(); ++i) groupRow[slots[i]] = group[i];
        for (std::size_t i=0;i<terms.size();++i)
          groupRow[input.outputs.size()+i]=finishAggregate(terms[i],values[i]);
        for (std::size_t i = 0; i < masks.size(); ++i) groupRow[maskBase + i] = masks[i];
        if (!matches(restrictions, groupRow)) continue;
        if (output.size() >= limits.maxRows)
          fail(aggregate.origin, "local grouping sets exceed their combined row budget", DiagnosticCode::ResourceLimit);
        output.push_back(std::move(groupRow));
      }
    };
    if (aggregate.groupingSets.empty()) {
      charge(aggregate.origin, bindings.groupedColumns->size() * sizeof(std::size_t));
      const std::vector<std::size_t> slots(bindings.groupedColumns->begin(), bindings.groupedColumns->end());
      appendGroups(slots);
    } else for (const auto &set : aggregate.groupingSets) {
      charge(aggregate.origin, set.size() * sizeof(std::size_t));
      std::vector<std::size_t> slots;
      for (auto ref : set) slots.push_back(groupingSlot(ref, input));
      std::sort(slots.begin(), slots.end());
      slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
      appendGroups(slots);
    }
    return {input, std::move(bindings), std::move(output)};
  }
  Rows join(const RelNode &n, const RelNode &lhs, const RelNode &rhs,
            const Rows &left, const Rows &right) {
    const auto width = n.outputs.size(), leftWidth = lhs.outputs.size(), rightWidth = rhs.outputs.size();
    const bool preserveLeft = n.join == JoinKind::Left || n.join == JoinKind::Full;
    const bool preserveRight = n.join == JoinKind::Right || n.join == JoinKind::Full;
    if (n.distinct || n.fixedOrder || n.correlated || n.join == JoinKind::First ||
        width != leftWidth + rightWidth)
      fail(n.origin, "local join requires independent inputs without column coalescing");
    const auto expressions = compilePredicates(n, n);
    using JoinKey = std::array<std::int64_t, 4>;
    struct JoinKeyHash {
      std::size_t operator()(const JoinKey &key) const {
        std::size_t seed = 0;
        for (auto value : key)
          seed ^= std::hash<std::int64_t>{}(value) + 0x9e3779b9U + (seed << 6) + (seed >> 2);
        return seed;
      }
    };
    std::array<std::size_t, JoinKey{}.size()> leftSlots{}, rightSlots{};
    std::size_t keyCount = 0;
    constexpr auto entryBytes = sizeof(std::pair<const JoinKey,std::vector<std::size_t>>) +
        4 * sizeof(void *) + sizeof(std::size_t);
    const bool preferLeft = n.join == JoinKind::Inner && left.size() < right.size();
    const auto hashRows = preferLeft ? left.size() : right.size();
    if (!sqlite && !n.predicates.empty() && n.predicates.size() <= leftSlots.size() &&
        left.size() >= 16 && right.size() >= 16 &&
        hashRows <= (limits.maxBytes - bytes) / (2 * entryBytes)) {
      const auto slot = [](const RelNode &input, const ColumnRef &key) -> std::optional<std::size_t> {
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < input.outputs.size(); ++i) {
          const auto &output = input.outputs[i];
          if (output.query != key.query || output.table != key.table || output.name != key.name) continue;
          if (found) return {};
          found = i;
        }
        return found;
      };
      for (auto predicate : n.predicates) {
        const auto equality = integerEqualityColumns(plan, predicate, IntegerEquality::ExactSigned);
        if (!equality) break;
        auto l = slot(lhs, equality->first), r = slot(rhs, equality->second);
        if (!l || !r) {
          l = slot(lhs, equality->second);
          r = slot(rhs, equality->first);
        }
        if (!l || !r) break;
        leftSlots[keyCount] = *l;
        rightSlots[keyCount] = *r;
        ++keyCount;
      }
      if (keyCount != n.predicates.size()) keyCount = 0;
    }
    const auto integerKey = [&](const LocalRow &row, const auto &slots) -> std::optional<JoinKey> {
      JoinKey key{};
      for (std::size_t i = 0; i < keyCount; ++i) {
        const auto &value = row.at(slots[i]);
        if (isNull(value)) return std::nullopt;
        const auto component = std::get_if<std::int64_t>(&value);
        if (!component)
          fail(n.origin, "local integer join key does not match its bound type", DiagnosticCode::InvalidQueryShape);
        key[i] = *component;
      }
      return key;
    };
    // Inner joins may build either side. Outer joins retain the original
    // probe/build orientation, including their unmatched-row handling.
    const bool buildLeft = keyCount && preferLeft;
    const auto &probe = buildLeft ? right : left;
    const auto &build = buildLeft ? left : right;
    const auto &probeSlots = buildLeft ? rightSlots : leftSlots;
    const auto &buildSlots = buildLeft ? leftSlots : rightSlots;
    const auto probeOffset = buildLeft ? leftWidth : 0;
    const auto buildOffset = buildLeft ? 0 : leftWidth;
    const auto probeWidth = buildLeft ? rightWidth : leftWidth;
    const auto buildWidth = buildLeft ? leftWidth : rightWidth;
    std::unordered_map<JoinKey,std::vector<std::size_t>,JoinKeyHash> index;
    if (keyCount) {
      charge(n.origin, build.size() * entryBytes);
      index.reserve(build.size());
      for (std::size_t i = 0; i < build.size(); ++i) {
        step(n.origin, keyCount);
        if (const auto key = integerKey(build[i], buildSlots)) index[*key].push_back(i);
      }
    }
    charge(n.origin, (preserveRight ? right.size() : 0) + width * sizeof(LocalValue));
    std::vector<bool> matchedRight(preserveRight ? right.size() : 0);
    LocalRow pair(width);
    Rows output;
    auto emit = [&]() {
      if (output.size() >= limits.maxRows)
        fail(n.origin, "local interpreter exceeded its row budget", DiagnosticCode::ResourceLimit);
      charge(n.origin, sizeof(LocalRow) + width * sizeof(LocalValue));
      output.push_back(pair);
    };
    for (const auto &row : probe) {
      step(n.origin, probeWidth + 1);
      std::copy(row.begin(), row.end(), pair.begin() + probeOffset);
      bool matched = false;
      const auto visit = [&](std::size_t j) {
        step(n.origin, buildWidth + 1);
        std::copy(build[j].begin(), build[j].end(), pair.begin() + buildOffset);
        if (!keyCount && !matches(expressions, pair)) return;
        matched = true;
        if (preserveRight) matchedRight[j] = true;
        emit();
      };
      if (keyCount) {
        step(n.origin, keyCount);
        if (const auto key = integerKey(row, probeSlots)) {
          const auto found = index.find(*key);
          if (found != index.end())
            for (auto j : found->second) visit(j);
        }
      } else {
        for (std::size_t j = 0; j < build.size(); ++j) visit(j);
      }
      if (preserveLeft && !matched) {
        std::fill(pair.begin() + leftWidth, pair.end(), LocalValue{std::monostate{}});
        emit();
      }
    }
    if (preserveRight) {
      std::fill(pair.begin(), pair.begin() + leftWidth, LocalValue{std::monostate{}});
      for (std::size_t j = 0; j < right.size(); ++j) {
        step(n.origin, rightWidth + 1);
        if (matchedRight[j]) continue;
        std::copy(right[j].begin(), right[j].end(), pair.begin() + leftWidth);
        emit();
      }
    }
    return output;
  }
  Rows executeDependent(RelId id,std::size_t depth,const RelNode &schema,const LocalRow &row) {
    charge(schema.origin,sizeof(CapturedRow));
    capturedRows.push_back({schema,row,{}});
    auto rows=execute(id,depth);
    capturedRows.pop_back();
    return rows;
  }
  Rows correlate(const RelNode &join,std::size_t depth) {
    if (sqlite || join.inputs.size()!=2 || join.distinct || !join.correlated ||
        !join.predicates.empty() || (join.join!=JoinKind::Cross && join.join!=JoinKind::Left))
      fail(join.origin,"local LATERAL requires a PostgreSQL CROSS or LEFT dependent join");
    const auto &left=plan.nodes.at(join.inputs[0]);
    const auto &right=plan.nodes.at(join.inputs[1]);
    const auto &owner=modelQuery(plan,join.query);
    if (right.kind!=RelKind::Subquery || right.scalar || right.materialized ||
        right.query!=join.query || !right.table || *right.table>=owner.tables.size() ||
        left.query!=join.query || join.outputs.size()!=left.outputs.size()+right.outputs.size())
      fail(join.origin,"local LATERAL requires a bound derived right input");
    const auto &derived=owner.tables[*right.table];
    const auto &inner=modelQuery(plan,derived.query);
    if (!derived.name.empty() || inner.scope.parent!=join.query || !inner.scope.parentTables)
      fail(join.origin,"local LATERAL requires verified outer scope bindings");
    auto input=execute(join.inputs[0],depth+1);
    Rows result;
    for (const auto &row:input) {
      step(join.origin);
      auto matches=executeDependent(join.inputs[1],depth+1,left,row);
      const auto append=[&](LocalRow values) {
        if (result.size()>=limits.maxRows)
          fail(join.origin,"local LATERAL exceeded its output row budget",DiagnosticCode::ResourceLimit);
        step(join.origin,join.outputs.size());
        charge(join.origin,sizeof(LocalRow)+join.outputs.size()*sizeof(LocalValue));
        values.insert(values.begin(),row.begin(),row.end());
        result.push_back(std::move(values));
      };
      if (matches.empty() && join.join==JoinKind::Left) append(LocalRow(right.outputs.size()));
      for (auto &match:matches) append(std::move(match));
    }
    return result;
  }
  Rows membership(const RelNode &join,std::size_t depth) {
    const auto fields=membershipColumns(plan,join);
    if (sqlite || !fields)
      fail(join.origin,"local membership requires verified PostgreSQL integer keys");
    const auto &keys=*join.membershipKeys;
    const auto &right=modelExpression(plan,keys.right);
    const auto slot=[&](RelId source,const ColumnRef &field) {
      const auto &columns=plan.nodes.at(source).outputs;
      const auto found=std::find_if(columns.begin(),columns.end(),[&](const auto &column) {
        return column.query==field.query && column.table==field.table && column.name==field.name;
      });
      return static_cast<std::size_t>(found-columns.begin());
    };
    const auto leftOrdinal=slot(join.inputs.front(),fields->first);
    const auto rightOrdinal=slot(join.inputs.back(),fields->second);
    const bool anti=join.kind==RelKind::AntiJoin;
    const auto &predicate=modelExpression(plan,join.predicates.front());
    // A planned NOT IN can encounter NULLs if the source metadata changed.
    const bool nullAware=anti && (predicate.kind==ExprKind::In ||
                                  predicate.kind==ExprKind::Quantified);
    auto outer=execute(join.inputs[0],depth+1);
    if (outer.empty()) return outer;
    const RelationKeySet *lookup=nullptr;
    RelationKeySet keysAtSource;
    if (relations && !nullAware && plan.nodes.at(join.inputs.back()).kind==RelKind::Scan &&
        outer.size()<=RelationKeySet::maxKeys) {
      charge(join.origin,sizeof(RelationKeySet::Column)+outer.size()*sizeof(RelationKeySet::Key));
      const auto width=signedIntegerBits(plan.program.types.get(right.sqlType));
      keysAtSource.columns.push_back({right.binding->column,*width});
      keysAtSource.keys.reserve(outer.size());
      for (const auto &row:outer) {
        step(join.origin);
        if (const auto *value=std::get_if<std::int64_t>(&row.at(leftOrdinal)))
          keysAtSource.keys.push_back({*value,0,0,0});
      }
      std::sort(keysAtSource.keys.begin(),keysAtSource.keys.end());
      keysAtSource.keys.erase(std::unique(keysAtSource.keys.begin(),keysAtSource.keys.end()),keysAtSource.keys.end());
      lookup=&keysAtSource;
    }
    auto inner=execute(join.inputs[1],depth+1,{},nullptr,nullptr,lookup);
    std::unordered_set<std::int64_t> matches;
    charge(join.origin,inner.size()*(sizeof(std::int64_t)+4*sizeof(void *)));
    matches.reserve(inner.size());
    bool hasNull=false;
    for (const auto &row:inner) {
      step(join.origin);
      if (const auto *key=std::get_if<std::int64_t>(&row.at(rightOrdinal))) matches.insert(*key);
      else hasNull=true;
    }
    const auto removed=std::remove_if(outer.begin(),outer.end(),[&](const auto &row) {
      step(join.origin);
      const auto *key=std::get_if<std::int64_t>(&row.at(leftOrdinal));
      const bool exists=key && matches.find(*key)!=matches.end();
      if (nullAware && !inner.empty() && (!key || hasNull)) return true;
      return exists==anti;
    });
    outer.erase(removed,outer.end());
    return outer;
  }
  Rows deduplicate(const RelNode &n, Rows rows) {
    rowBudget(n.origin, n.outputs.size(), rows.size());
    std::set<LocalRow> seen;
    const auto firstDropped = std::remove_if(rows.begin(), rows.end(), [&](const auto &row) {
      step(n.origin, row.size() + 1);
      for (const auto &value : row) requireNonText(value, n.origin);
      return !seen.insert(row).second;
    });
    rows.erase(firstDropped, rows.end());
    return rows;
  }
  struct RowHash {
    std::size_t operator()(const LocalRow &row) const {
      auto hash = row.size();
      for (const auto &value : row) {
        const auto cell = std::visit([](const auto &scalar) -> std::size_t {
          using T = std::decay_t<decltype(scalar)>;
          if constexpr (std::is_same_v<T, LocalText>) return std::hash<std::string_view>{}(scalar.utf8());
          else if constexpr (std::is_same_v<T, LocalNumeric>) return scalar.hash();
          else return std::hash<T>{}(scalar);
        }, value);
        hash ^= cell + std::size_t{0x9e3779b9u} + (hash << 6) + (hash >> 2);
      }
      return hash;
    }
  };
  Rows set(const RelNode &n, Rows left, Rows right) {
    const auto numeric=numericColumns(n);
    normalizeNumericRows(left,numeric,n.origin);
    normalizeNumericRows(right,numeric,n.origin);
    std::vector<std::size_t> types(n.outputs.size());
    compatible(left, types, n.origin);
    compatible(right, types, n.origin);
    const bool intersect = n.setKind == SetKind::Intersect;
    // Equal numeric cells can have different display scales. Preserve the
    // left arm's representative when returning matched numeric rows.
    const bool buildLeft=intersect && numeric.empty() && left.size()<=right.size();
    auto &build=buildLeft?left:right;
    auto &probe=buildLeft?right:left;
    charge(n.origin, build.size() * (sizeof(std::pair<const LocalRow, std::size_t>) + 4 * sizeof(void *)));
    std::unordered_map<LocalRow, std::size_t, RowHash> counts;
    counts.reserve(build.size());
    for (auto &row : build) {
      step(n.origin, row.size() + 1);
      ++counts[std::move(row)];
    }
    const auto firstDropped = std::remove_if(probe.begin(), probe.end(), [&](const auto &row) {
      step(n.origin, row.size() + 1);
      const auto found = counts.find(row);
      const auto matches = found != counts.end() && found->second != 0;
      if (matches && !n.distinct) --found->second;
      return matches != intersect;
    });
    probe.erase(firstDropped, probe.end());
    if (n.distinct) return deduplicate(n, std::move(probe));
    return std::move(probe);
  }
  Rows unionRows(const RelNode &n, std::size_t depth) {
    Rows output;
    const auto numeric=numericColumns(n);
    std::vector<std::size_t> types(n.outputs.size());
    for (auto input : n.inputs) {
      auto rows = execute(input, depth + 1);
      normalizeNumericRows(rows,numeric,n.origin);
      compatible(rows,types,n.origin);
      if (rows.size() > limits.maxRows - output.size())
        fail(n.origin, "local interpreter exceeded its row budget", DiagnosticCode::ResourceLimit);
      charge(n.origin, rows.size() * sizeof(LocalRow));
      output.insert(output.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    }
    return n.distinct ? deduplicate(n, std::move(output)) : std::move(output);
  }
  Rows repeatUnion(const RelNode &n, std::size_t depth) {
    if (sqlite || n.setKind != SetKind::Union || !n.workingTableOf || n.inputs.size() != 2)
      fail(n.origin, "local recursive execution requires a PostgreSQL working-table UNION");
    const auto seed = n.inputs[0];
    auto delta = execute(seed, depth + 1);
    const auto width = n.outputs.size();
    std::vector<std::size_t> types(width);
    compatible(delta, types, n.origin);
    if (!delta.empty() && std::any_of(types.begin(), types.end(), [](auto type) { return type == 0; }))
      fail(n.origin, "local recursion requires a known non-NULL seed value for each output type");
    std::set<LocalRow> seen;
    const auto removeSeen = [&](Rows &rows) {
      if (!n.distinct) return;
      rowBudget(n.origin, width, rows.size());
      const auto duplicate = std::remove_if(rows.begin(), rows.end(), [&](const auto &row) {
        step(n.origin, width + 1);
        return !seen.insert(row).second;
      });
      rows.erase(duplicate, rows.end());
    };
    removeSeen(delta);
    Rows out;
    for (std::size_t iteration = 0; !delta.empty(); ++iteration) {
      step(n.origin);
      if (iteration >= limits.maxIterations || delta.size() > limits.maxRows - out.size())
        fail(n.origin, "local recursive query exceeded its iteration or row budget", DiagnosticCode::ResourceLimit);
      const auto inserted = working.emplace(n.workingTableOf, WorkingRows{delta, seed, width});
      if (!inserted.second)
        fail(n.origin, "recursive work table re-entered its owner", DiagnosticCode::InvalidQueryShape);
      auto next = execute(n.inputs[1], depth + 1);
      working.erase(inserted.first);
      compatible(next, types, n.origin);
      removeSeen(next);
      rowBudget(n.origin, width, delta.size());
      out.insert(out.end(), std::make_move_iterator(delta.begin()), std::make_move_iterator(delta.end()));
      delta = std::move(next);
    }
    return out;
  }
  Rows workingRows(const RelNode &n) {
    const auto source = working.find(n.workingTableOf);
    if (source == working.end() || source->second.seed != n.workingSeed ||
        n.outputs.size() != source->second.width || !n.inputs.empty())
      fail(n.origin, "recursive work table has no compatible active iteration", DiagnosticCode::InvalidQueryShape);
    const auto &rows = source->second.rows;
    rowBudget(n.origin, n.outputs.size(), rows.size());
    step(n.origin, rows.size());
    return rows;
  }
  Rows evaluateKeys(const std::vector<Scalar> &expressions, const Rows &rows, SourceRange origin) {
    rowBudget(origin, expressions.size(), rows.size());
    Rows keys;
    keys.reserve(rows.size());
    for (const auto &row : rows) {
      LocalRow key;
      key.reserve(expressions.size());
      for (const auto &expression : expressions) key.push_back(eval(expression, row));
      keys.push_back(std::move(key));
    }
    std::vector<std::size_t> types(expressions.size());
    compatible(keys, types, origin);
    return keys;
  }
  bool orderedBefore(const LocalRow &a, const LocalRow &b, const std::vector<bool> &descending,
                     const std::vector<std::string> &nullOrdering, SourceRange origin) {
    step(origin, a.size() + 1);
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i] == b[i]) continue;
      if (isNull(a[i]) || isNull(b[i])) {
        const bool first = nullOrdering[i] == "FIRST" ||
            (nullOrdering[i].empty() && descending[i] != sqlite);
        return isNull(a[i]) == first;
      }
      return descending[i] ? b[i] < a[i] : a[i] < b[i];
    }
    return false;
  }
  Rows sort(const RelNode &n, const RelNode &input, Rows rows) {
    if (n.keys.size() != n.descending.size() || n.keys.size() != n.nullOrdering.size())
      fail(n.origin, "invalid local sort key shape", DiagnosticCode::InvalidQueryShape);
    const auto expressions = compileAll(n.keys, &input, true, n.origin);
    const auto keys = evaluateKeys(expressions, rows, n.origin);
    charge(n.origin, rows.size() * sizeof(std::size_t) + sizeof(LocalRow) * rows.size());
    std::vector<std::size_t> order(rows.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](auto l, auto r) {
      return orderedBefore(keys[l], keys[r], n.descending, n.nullOrdering, n.origin);
    });
    Rows sorted;
    sorted.reserve(rows.size());
    for (auto i : order) sorted.push_back(std::move(rows[i]));
    return sorted;
  }
  enum class Ranking { RowNumber, Rank, DenseRank };
  enum class FrameUnit { Rows, Peers, NumericRange };
  struct FrameBound {
    WindowBound kind;
    std::uint64_t offset = 0;
  };
  struct WindowFrame {
    FrameUnit unit = FrameUnit::Peers;
    FrameBound start{WindowBound::UnboundedPreceding};
    FrameBound end{WindowBound::CurrentRow};
    WindowExclusion exclusion = WindowExclusion::None;
  };
  struct TileBuckets {
    std::size_t count=0;
    Scalar countExpression;
  };
  struct FrameValue {
    Scalar value, positionExpression;
    std::size_t position = 1;
    bool fromEnd = false;
  };
  struct OffsetValue {
    Scalar value, fallback, offsetExpression;
    std::size_t offset = 1;
    bool following = false;
  };
  struct WindowTerm {
    std::variant<Ranking, TileBuckets, FrameValue, OffsetValue, AggregateTerm> function;
    SourceRange origin;
    std::vector<Scalar> partition, order;
    std::vector<bool> descending;
    std::vector<std::string> nullOrdering;
    WindowFrame frame;
    bool sumZero=false;
  };
  std::size_t windowIntegerArgument(ExpressionRef ref, bool allowZero = false) {
    const auto &arg = modelExpression(plan, ref);
    const auto *message = allowZero ? "local window offset requires a nonnegative 32-bit integer literal"
                                    : "local window position requires a positive 32-bit integer literal";
    if (arg.kind != ExprKind::Number)
      fail(arg.origin, message);
    const auto number = integer(arg.value, arg.origin);
    if (!int32(number) || number < (allowZero ? 0 : 1))
      fail(arg.origin, message);
    return static_cast<std::size_t>(number);
  }
  void compileWindowInteger(Scalar &output,ExpressionRef ref,const RelNode &input,
                            const ComputedBindings &bindings,std::string_view operation) {
    const auto &argument=modelExpression(plan,ref);
    const auto bits=signedIntegerBits(plan.program.types.get(argument.sqlType));
    if (argument.kind!=ExprKind::Null && (!bits || *bits>32))
      fail(argument.origin,"local PostgreSQL "+std::string(operation)+" requires a signed integer argument");
    compileScalar(output,ref,&input,false,0,&bindings);
  }
  std::optional<std::int64_t> windowIntegerValue(const Scalar &expression,const LocalRow &row,
                                                std::string_view operation) {
    const auto value=eval(expression,row);
    if (isNull(value)) return {};
    const auto number=std::get_if<std::int64_t>(&value);
    if (!number || !int32(*number))
      fail(expression.back().origin,"local "+std::string(operation)+" exceeded the signed integer range",
           DiagnosticCode::InvalidQueryShape);
    return *number;
  }
  Scalar directWindowValue(ExpressionRef ref, const RelNode &input, const ComputedBindings &bindings) {
    Scalar scalar;
    compileScalar(scalar, ref, &input, false, 0, &bindings);
    if (scalar.size() != 1 || (scalar.front().op != Op::Load && scalar.front().op != Op::Literal))
      fail(modelExpression(plan, ref).origin, "local window value requires a literal or an already computed input");
    return scalar;
  }
  WindowFrame windowFrame(ExpressionRef ref, const ModelExpression &e, std::size_t offset) {
    const auto &spec = *e.window;
    if (spec.frameUnit.empty() && offset == e.operands.size()) return {};
    if (spec.frameUnit != "ROWS" && spec.frameUnit != "GROUPS" && spec.frameUnit != "RANGE")
      fail(e.origin, "invalid local window frame unit", DiagnosticCode::InvalidQueryShape);
    auto bound = [&](WindowBound kind) -> FrameBound {
      if (kind != WindowBound::Preceding && kind != WindowBound::Following)
        return {kind == WindowBound::None ? WindowBound::CurrentRow : kind};
      if (offset >= e.operands.size())
        fail(e.origin, "missing local window frame offset", DiagnosticCode::InvalidQueryShape);
      Scalar scalar;
      compileScalar(scalar, {ref.query, e.operands[offset++]}, nullptr, false);
      const auto value = eval(scalar, {});
      const auto integer = std::get_if<std::int64_t>(&value);
      if (!integer || *integer < 0)
        fail(scalar.back().origin, "local window offset requires a nonnegative exact integer");
      if (spec.frameUnit == "RANGE" && !int32(*integer))
        fail(scalar.back().origin, "local RANGE offsets require a nonnegative 32-bit integer");
      return {kind, static_cast<std::uint64_t>(*integer)};
    };
    WindowFrame frame{spec.frameUnit == "ROWS" ? FrameUnit::Rows : FrameUnit::Peers,
                      bound(spec.start), bound(spec.end), spec.exclusion};
    if (offset != e.operands.size())
      fail(e.origin, "invalid local window frame offset count", DiagnosticCode::InvalidQueryShape);
    const auto boundedOffset = [](FrameBound b) {
      return b.kind == WindowBound::Preceding || b.kind == WindowBound::Following;
    };
    if (spec.frameUnit == "RANGE" && (boundedOffset(frame.start) || boundedOffset(frame.end))) {
      if (spec.ordering.size() != 1)
        fail(e.origin, "local RANGE offsets require one ORDER BY key", DiagnosticCode::InvalidQueryShape);
      frame.unit = FrameUnit::NumericRange;
    }
    return frame;
  }
  WindowTerm windowTerm(ExpressionRef ref, const ModelExpression &e, const RelNode &input,
                        const ComputedBindings &bindings) {
    if (!e.window || e.operands.size() < 1 + e.window->partitions + e.window->ordering.size())
      fail(e.origin, "invalid local window shape", DiagnosticCode::InvalidQueryShape);
    const auto &base = modelExpression(plan, {ref.query, e.operands.front()});
    const bool filtered = base.kind == ExprKind::AggregateFilter && base.operands.size() == 2;
    const auto &call = filtered ? modelExpression(plan, {ref.query, base.operands.front()}) : base;
    WindowTerm term{Ranking::RowNumber, e.origin, {}, {}, {}, {}, {}};
    const auto frameOffset = 1 + e.window->partitions + e.window->ordering.size();
    const bool defaultFrame = e.window->frameUnit.empty() && e.window->exclusion == WindowExclusion::None &&
                              frameOffset == e.operands.size();
    const auto window = !filtered && !call.distinct ? nativeOperator(call, RoutineKind::Window) : std::nullopt;
    if (window && (window->name == "ROW_NUMBER" || window->name == "RANK" ||
                   window->name == "DENSE_RANK" || window->name == "NTILE")) {
      if (!defaultFrame)
        fail(e.origin, "local ranking currently requires an implicit frame");
      if (window->name == "NTILE") {
        TileBuckets buckets;
        const ExpressionRef count{ref.query,call.operands.front()};
        if (sqlite) buckets.count=windowIntegerArgument(count);
        else compileWindowInteger(buckets.countExpression,count,input,bindings,"NTILE");
        term.function=std::move(buckets);
      } else
        term.function = window->name == "ROW_NUMBER" ? Ranking::RowNumber :
                        window->name == "RANK" ? Ranking::Rank : Ranking::DenseRank;
    } else if (window && (window->name == "FIRST_VALUE" || window->name == "LAST_VALUE" ||
                          window->name == "NTH_VALUE")) {
      FrameValue value;
      value.value = directWindowValue({ref.query, call.operands.front()}, input, bindings);
      if (window->name == "NTH_VALUE") {
        const ExpressionRef position{ref.query,call.operands[1]};
        if (sqlite) value.position=windowIntegerArgument(position);
        else compileWindowInteger(value.positionExpression,position,input,bindings,"NTH_VALUE");
      }
      value.fromEnd = window->name == "LAST_VALUE";
      term.function = std::move(value);
      term.frame = windowFrame(ref, e, frameOffset);
    } else if (window && (window->name == "LAG" || window->name == "LEAD")) {
      if (!defaultFrame)
        fail(e.origin, "local LAG and LEAD currently require an implicit frame");
      OffsetValue value;
      const ExpressionRef refValue{ref.query, call.operands.front()};
      value.value = directWindowValue(refValue, input, bindings);
      value.following = window->name == "LEAD";
      if (call.operands.size() >= 2) {
        const ExpressionRef offset{ref.query,call.operands[1]};
        if (sqlite) value.offset=windowIntegerArgument(offset,true);
        else compileWindowInteger(value.offsetExpression,offset,input,bindings,"LAG/LEAD offset");
      }
      if (call.operands.size() == 3) {
        const ExpressionRef refDefault{ref.query, call.operands[2]};
        const auto &argument = modelExpression(plan, refValue);
        const auto &fallback = modelExpression(plan, refDefault);
        const auto supported = [](ValueType type) {
          return type == ValueType::Null || type == ValueType::Integer || type == ValueType::Boolean;
        };
        if (!sqlite && (!supported(argument.type) || !supported(fallback.type) ||
            (argument.type != fallback.type && argument.type != ValueType::Null && fallback.type != ValueType::Null)))
          fail(fallback.origin, "local PostgreSQL LAG and LEAD need compatible exact value and default types");
        value.fallback = directWindowValue(refDefault, input, bindings);
      }
      term.function = std::move(value);
    } else {
      const auto aggregate = nativeOperator(call, RoutineKind::Aggregate);
      if (!aggregate || call.distinct ||
          (aggregate->name!="COUNT" && aggregate->name!="SUM" && aggregate->name!="AVG" &&
           aggregate->name!="MIN" && aggregate->name!="MAX"))
        fail(e.origin,"this window function is unavailable in local execution");
      const auto kind=aggregate->name=="COUNT"?AggregateKind::Count:
          aggregate->name=="AVG"?AggregateKind::Average:aggregate->name=="MIN"?AggregateKind::Min:
          aggregate->name=="MAX"?AggregateKind::Max:AggregateKind::Sum;
      if (!sqlite && (kind==AggregateKind::Min || kind==AggregateKind::Max) && call.operands.size()==1) {
        const auto &argument=modelExpression(plan,{ref.query,call.operands.front()});
        if (argument.kind!=ExprKind::Null && !signedIntegerBits(plan.program.types.get(argument.sqlType)))
          fail(argument.origin,"local window MIN/MAX requires a signed integer input");
      }
      term.function=aggregateTerm(ref,base,call,kind,input,&bindings);
      term.frame = windowFrame(ref, e, frameOffset);
    }
    std::vector<ExpressionRef> partitions, ordering;
    for (std::size_t i = 0; i < e.window->partitions; ++i)
      partitions.push_back({ref.query, e.operands[1 + i]});
    for (std::size_t i = 0; i < e.window->ordering.size(); ++i) {
      ordering.push_back({ref.query, e.operands[1 + e.window->partitions + i]});
      term.descending.push_back(e.window->ordering[i].descending);
      term.nullOrdering.push_back(e.window->ordering[i].nulls);
    }
    term.partition = compileAll(partitions, &input, false, e.origin, &bindings);
    term.order = compileAll(ordering, &input, false, e.origin, &bindings);
    return term;
  }
  void rankPartition(const WindowTerm &term, Ranking ranking, std::size_t first, std::size_t last,
                     const std::vector<std::size_t> &order, const Rows &keys, Rows &rows, std::size_t slot) {
    std::int64_t rank = 1, dense = 1;
    for (auto i = first; i < last; ++i) {
      step(term.origin, term.order.size() + 1);
      const auto position = static_cast<std::int64_t>(i - first + 1);
      if (i > first && keys[order[i]] != keys[order[i - 1]]) {
        rank = position;
        ++dense;
      }
      rows[order[i]][slot] = ranking == Ranking::RowNumber ? position : ranking == Ranking::Rank ? rank : dense;
    }
  }
  void tilePartition(const WindowTerm &term, const TileBuckets &buckets, std::size_t first, std::size_t last,
                     const std::vector<std::size_t> &order, Rows &rows, std::size_t slot) {
    const auto size = last - first;
    auto count=buckets.count;
    auto start=first;
    // PostgreSQL initializes NTILE with the first non-NULL argument, using
    // the full partition size. Before initialization the outputs remain NULL.
    if (!buckets.countExpression.empty())
      for (;start<last;++start) {
        step(term.origin);
        const auto value=windowIntegerValue(buckets.countExpression,rows[order[start]],"NTILE count");
        if (!value) continue;
        if (*value<=0)
          fail(buckets.countExpression.back().origin,"NTILE count must be greater than zero",
               DiagnosticCode::InvalidQueryShape);
        count=static_cast<std::size_t>(*value);
        break;
      }
    if (start==last) return;
    const auto small = size / count;
    const auto largeBuckets = size % count;
    const auto largeSpan = (small + 1) * largeBuckets;
    for (std::size_t i = 0; i < last-start; ++i) {
      step(term.origin);
      const auto bucket = i < largeSpan ? i / (small + 1)
                                       : largeBuckets + (i - largeSpan) / small;
      rows[order[start + i]][slot] = static_cast<std::int64_t>(bucket + 1);
    }
  }
  void offsetPartition(const WindowTerm &term, const OffsetValue &value, std::size_t first,
                       std::size_t last, const std::vector<std::size_t> &order, Rows &rows,
                       std::size_t slot) {
    for (auto i = first; i < last; ++i) {
      step(term.origin);
      auto &row = rows[order[i]];
      auto distance=value.offset;
      auto following=value.following;
      if (!value.offsetExpression.empty()) {
        const auto integer=windowIntegerValue(value.offsetExpression,row,"LAG/LEAD offset");
        if (!integer) continue;
        following=following!=(*integer<0);
        distance=std::size_t(*integer<0?-*integer:*integer);
      }
      const bool exists = following ? distance < last - i : distance <= i - first;
      if (exists) {
        const auto neighbor = following ? i + distance : i - distance;
        row[slot] = eval(value.value, rows[order[neighbor]]);
      } else if (!value.fallback.empty()) {
        row[slot] = eval(value.fallback, row);
      }
    }
  }
  static std::size_t frameEdge(FrameBound bound, std::size_t position, std::size_t count, bool end) {
    switch (bound.kind) {
    case WindowBound::UnboundedPreceding: return 0;
    case WindowBound::UnboundedFollowing: return count;
    case WindowBound::Preceding: return bound.offset <= position ? position - bound.offset + end : 0;
    case WindowBound::Following: return bound.offset < count - position ? position + bound.offset + end : count;
    default: return position + end;
    }
  }
  struct RangePartition {
    // Numeric keys are contiguous. Normalizing descending order makes every
    // numeric boundary monotone, so each cursor traverses the slice once.
    std::vector<std::int64_t> keys;
    std::size_t first = 0, lower = 0, upper = 0;
  };
  RangePartition rangePartition(const WindowTerm &term, std::size_t first, std::size_t last,
                                const std::vector<std::size_t> &order, const Rows &keys) {
    RangePartition range;
    charge(term.origin, (last - first) * sizeof(std::int64_t));
    range.keys.reserve(last - first);
    for (auto i = first; i < last; ++i) {
      step(term.origin);
      const auto &key = keys[order[i]].front();
      if (isNull(key)) {
        if (range.keys.empty()) ++range.first;
        continue;
      }
      const auto integer = std::get_if<std::int64_t>(&key);
      if (!integer || !int32(*integer))
        fail(term.origin, "local offset RANGE requires a 32-bit integer ORDER BY value");
      range.keys.push_back(term.descending.front() ? -*integer : *integer);
    }
    return range;
  }
  std::size_t rangeEdge(RangePartition &range, FrameBound bound, std::size_t i,
                        std::size_t count, std::size_t peerEdge, bool end, SourceRange origin) {
    if (bound.kind == WindowBound::UnboundedPreceding) return 0;
    if (bound.kind == WindowBound::UnboundedFollowing) return count;
    if (bound.kind == WindowBound::CurrentRow || i < range.first || i - range.first >= range.keys.size())
      return peerEdge;
    const auto distance = static_cast<std::int64_t>(bound.offset);
    const auto threshold = range.keys[i - range.first] +
        (bound.kind == WindowBound::Preceding ? -distance : distance);
    auto &cursor = end ? range.upper : range.lower;
    while (cursor < range.keys.size() && (end ? range.keys[cursor] <= threshold : range.keys[cursor] < threshold)) {
      step(origin);
      ++cursor;
    }
    return range.first + cursor;
  }
  struct FrameSlices {
    struct Slice { std::size_t begin, end; };
    std::array<Slice, 3> slices{};
    std::size_t size = 0;
    void add(std::size_t begin, std::size_t end) {
      if (begin < end) slices[size++] = {begin, end};
    }
  };
  template <typename Consumer>
  void windowFrames(const WindowTerm &term, std::size_t first, std::size_t last,
                    const std::vector<std::size_t> &order, const Rows &keys, Consumer consume) {
    const auto count = last - first;
    std::vector<std::size_t> peers;
    const bool byPeers = term.frame.unit == FrameUnit::Peers;
    const bool byRange = term.frame.unit == FrameUnit::NumericRange;
    auto range = byRange ? rangePartition(term, first, last, order, keys) : RangePartition{};
    const auto exclusion = term.frame.exclusion;
    const bool excludePeers = exclusion == WindowExclusion::Group || exclusion == WindowExclusion::Ties;
    const bool needsPeers = byPeers || byRange || excludePeers;
    if (needsPeers) {
      charge(term.origin, (count + 1) * sizeof(std::size_t));
      peers.reserve(count + 1);
      peers.push_back(0);
      for (std::size_t i = 1; i < count; ++i) {
        step(term.origin, term.order.size() + 1);
        if (keys[order[first + i]] != keys[order[first + i - 1]]) peers.push_back(i);
      }
      peers.push_back(count);
    }
    std::size_t group = 0;
    for (std::size_t i = 0; i < count; ++i) {
      step(term.origin);
      if (needsPeers && i == peers[group + 1]) ++group;
      const auto edge = [&](FrameBound bound, bool end) {
        if (byRange) return rangeEdge(range, bound, i, count, peers[group + end], end, term.origin);
        if (byPeers) return peers[frameEdge(bound, group, peers.size() - 1, end)];
        return frameEdge(bound, i, count, end);
      };
      const auto begin = edge(term.frame.start, false);
      const auto end = edge(term.frame.end, true);
      FrameSlices frame;
      if (begin < end && (excludePeers || exclusion == WindowExclusion::CurrentRow)) {
        const auto cutBegin = std::max(begin, excludePeers ? peers[group] : i);
        const auto cutEnd = std::min(end, excludePeers ? peers[group + 1] : i + 1);
        if (cutBegin < cutEnd) {
          frame.add(begin, cutBegin);
          if (exclusion == WindowExclusion::Ties && cutBegin <= i && i < cutEnd) frame.add(i, i + 1);
          frame.add(cutEnd, end);
        } else {
          frame.add(begin, end);
        }
      } else frame.add(begin, end);
      consume(i, frame);
    }
  }
  void frameValuePartition(const WindowTerm &term, const FrameValue &value,
                           std::size_t first, std::size_t last, const std::vector<std::size_t> &order,
                           const Rows &keys, Rows &rows, std::size_t slot) {
    windowFrames(term, first, last, order, keys, [&](std::size_t i, const FrameSlices &frame) {
      auto position = value.position - 1;
      auto &current=rows[order[first+i]];
      if (!value.positionExpression.empty()) {
        const auto ordinal=windowIntegerValue(value.positionExpression,current,"NTH_VALUE position");
        if (!ordinal) return;
        if (*ordinal<=0)
          fail(value.positionExpression.back().origin,"NTH_VALUE position must be greater than zero",
               DiagnosticCode::InvalidQueryShape);
        position=static_cast<std::size_t>(*ordinal-1);
      }
      for (std::size_t index = 0; index < frame.size; ++index) {
        const auto &segment = frame.slices[value.fromEnd ? frame.size - 1 - index : index];
        const auto size = segment.end - segment.begin;
        if (position < size) {
          const auto selected = value.fromEnd ? segment.end - 1 - position : segment.begin + position;
          current[slot] = eval(value.value, rows[order[first + selected]]);
          return;
        }
        position -= size;
      }
    });
  }
  template <class Sum>
  void aggregateWindow(const WindowTerm &term,const AggregateTerm &aggregate,
                       std::size_t first,std::size_t last,const std::vector<std::size_t> &order,
                       const Rows &keys,Rows &rows,std::size_t slot) {
    struct Prefix { std::int64_t count=0; Sum sum{}; };
    const auto count = last - first;
    charge(term.origin,(count+1)*sizeof(Prefix));
    if constexpr (std::is_same_v<Sum,LocalNumeric>)
      charge(term.origin,(count+1)*LocalNumeric::storageBytes);
    std::vector<Prefix> prefix(count+1);
    // A numeric sum preserves the greatest display scale of the actual frame.
    // Prefix subtraction alone can retain an excluded row's fractional scale.
    std::vector<std::uint8_t> scales;
    std::int64_t positive = 0, negative = 0;
    bool hasNull=false;
    for (std::size_t i = 0; i < count; ++i) {
      step(term.origin);
      const auto &row = rows[order[first + i]];
      auto &next = prefix[i + 1];
      next = prefix[i];
      if (!aggregate.filter.empty() && truth(eval(aggregate.filter, row), aggregate.origin) != Truth::True) continue;
      const auto value = aggregate.argument.empty() ? LocalValue{std::int64_t{1}} : eval(aggregate.argument, row);
      if (isNull(value)) {
        hasNull=true;
        continue;
      }
      ++next.count;
      if (aggregate.kind == AggregateKind::Count) continue;
      if constexpr (std::is_same_v<Sum,LocalNumeric>) {
        const auto numeric=numericValue(value,aggregate.origin);
        if (!next.sum.add(numeric))
          fail(aggregate.origin,"local window aggregate exceeded its decimal capacity",DiagnosticCode::ResourceLimit);
        if (numeric.scale() && scales.empty()) {
          charge(term.origin,2*count*sizeof(std::uint8_t));
          scales.resize(2*count);
        }
        if (!scales.empty()) scales[count+i]=static_cast<std::uint8_t>(numeric.scale());
      } else {
        const auto *integer=std::get_if<std::int64_t>(&value);
        if (!integer || (!sqlite && !int32(*integer)))
          fail(aggregate.origin,"local window SUM requires an exact supported integer input");
        // Bounding each sign makes every selected subset safe to sum.
        if ((*integer > 0 && positive > std::numeric_limits<std::int64_t>::max() - *integer) ||
            (*integer < 0 && negative < std::numeric_limits<std::int64_t>::min() - *integer))
          fail(aggregate.origin,"local window SUM cannot prove exact integers throughout the partition");
        (*integer > 0 ? positive : negative) += *integer;
        next.sum += *integer;
      }
    }
    for (auto i=scales.empty()?0:count-1;i>0;--i) {
      step(term.origin);
      scales[i]=std::max(scales[2*i],scales[2*i+1]);
    }
    const auto scaleOf=[&](std::size_t begin,std::size_t end) {
      std::uint8_t maximum=0;
      if (scales.empty()) return maximum;
      for (begin+=count,end+=count;begin<end;begin/=2,end/=2) {
        step(term.origin);
        if (begin%2) maximum=std::max(maximum,scales[begin++]);
        if (end%2) maximum=std::max(maximum,scales[--end]);
      }
      return maximum;
    };
    const bool sumZero=term.sumZero && !hasNull &&
        (aggregate.kind==AggregateKind::Sum || aggregate.kind==AggregateKind::SumNumeric);
    windowFrames(term,first,last,order,keys,[&](std::size_t i,const FrameSlices &frame) {
      Prefix total;
      std::uint8_t scale=0;
      for (std::size_t index=0;index<frame.size;++index) {
        const auto &segment = frame.slices[index];
        if (!sumZero) total.count += prefix[segment.end].count - prefix[segment.begin].count;
        if constexpr (std::is_same_v<Sum,LocalNumeric>) {
          auto slice=prefix[segment.end].sum;
          if (!slice.subtract(prefix[segment.begin].sum) || !total.sum.add(slice))
            fail(aggregate.origin,"local window frame exceeded its decimal capacity",DiagnosticCode::ResourceLimit);
          scale=std::max(scale,scaleOf(segment.begin,segment.end));
        } else {
          total.sum += prefix[segment.end].sum - prefix[segment.begin].sum;
        }
      }
      auto &result = rows[order[first + i]][slot];
      if (aggregate.kind==AggregateKind::Count) {
        result=total.count;
      } else if (sumZero || total.count) {
        if constexpr (std::is_same_v<Sum,LocalNumeric>) {
          charge(term.origin,LocalNumeric::storageBytes);
          if (!total.sum.setDisplayScale(scale))
            fail(aggregate.origin,"window numeric scale does not match its frame",DiagnosticCode::InvalidQueryShape);
          if (aggregate.kind==AggregateKind::Average) {
            const auto average=total.sum.dividedByCount(static_cast<std::uint32_t>(total.count));
            if (!average)
              fail(aggregate.origin,"local window AVG result exceeds its decimal scale budget",DiagnosticCode::ResourceLimit);
            result=*average;
            return;
          }
        }
        result=total.sum;
      }
    });
  }
  void extremaWindow(const WindowTerm &term,const AggregateTerm &aggregate,
                     std::size_t first,std::size_t last,const std::vector<std::size_t> &order,
                     const Rows &keys,Rows &rows,std::size_t slot) {
    using Value=std::optional<std::int64_t>;
    const auto count=last-first;
    charge(term.origin,2*count*sizeof(Value));
    std::vector<Value> tree(2*count);
    const bool minimum=aggregate.kind==AggregateKind::Min;
    const auto combine=[minimum](Value left,Value right)->Value {
      if (!left) return right;
      if (!right) return left;
      return minimum?std::min(*left,*right):std::max(*left,*right);
    };
    for (std::size_t i=0;i<count;++i) {
      step(term.origin);
      const auto &row=rows[order[first+i]];
      if (!aggregate.filter.empty() && truth(eval(aggregate.filter,row),aggregate.origin)!=Truth::True) continue;
      const auto value=eval(aggregate.argument,row);
      if (isNull(value)) continue;
      const auto integer=std::get_if<std::int64_t>(&value);
      if (!integer) fail(aggregate.origin,"local window MIN/MAX requires an integer storage value");
      tree[count+i]=*integer;
    }
    for (auto i=count-1;i>0;--i) {
      step(term.origin);
      tree[i]=combine(tree[2*i],tree[2*i+1]);
    }
    windowFrames(term,first,last,order,keys,[&](std::size_t row,const FrameSlices &frame) {
      Value result;
      for (std::size_t slice=0;slice<frame.size;++slice) {
        auto begin=frame.slices[slice].begin+count;
        auto end=frame.slices[slice].end+count;
        while (begin<end) {
          step(term.origin);
          if (begin%2) result=combine(result,tree[begin++]);
          if (end%2) result=combine(result,tree[--end]);
          begin/=2;
          end/=2;
        }
      }
      if (result) rows[order[first+row]][slot]=*result;
    });
  }
  void aggregatePartition(const WindowTerm &term,const AggregateTerm &aggregate,
                          std::size_t first,std::size_t last,const std::vector<std::size_t> &order,
                          const Rows &keys,Rows &rows,std::size_t slot) {
    if (aggregate.kind==AggregateKind::Min || aggregate.kind==AggregateKind::Max)
      extremaWindow(term,aggregate,first,last,order,keys,rows,slot);
    else if (aggregate.kind==AggregateKind::SumNumeric || aggregate.kind==AggregateKind::Average)
      aggregateWindow<LocalNumeric>(term,aggregate,first,last,order,keys,rows,slot);
    else
      aggregateWindow<std::int64_t>(term,aggregate,first,last,order,keys,rows,slot);
  }
  struct WindowOrder {
    Rows partitions, keys;
    std::vector<std::size_t> order;
  };
  WindowOrder orderWindow(const WindowTerm &term,const Rows &rows) {
    WindowOrder result{evaluateKeys(term.partition,rows,term.origin),
                       evaluateKeys(term.order,rows,term.origin),{}};
    charge(term.origin, rows.size() * sizeof(std::size_t));
    result.order.resize(rows.size());
    std::iota(result.order.begin(),result.order.end(),std::size_t{0});
    if (!term.partition.empty() || !term.order.empty()) {
      std::stable_sort(result.order.begin(),result.order.end(),[&](auto left,auto right) {
        step(term.origin,term.partition.size()+1);
        if (result.partitions[left]!=result.partitions[right])
          return result.partitions[left]<result.partitions[right];
        return orderedBefore(result.keys[left],result.keys[right],term.descending,term.nullOrdering,term.origin);
      });
    }
    return result;
  }
  void windowRows(const WindowTerm &term, const WindowOrder &layout, Rows &rows,std::size_t slot) {
    const auto &order=layout.order;
    const auto &keys=layout.keys;
    const auto &partition=layout.partitions;
    for (std::size_t first = 0; first < order.size();) {
      auto last = first + 1;
      while (last < order.size() && partition[order[first]] == partition[order[last]]) {
        step(term.origin, term.partition.size() + 1);
        ++last;
      }
      if (const auto ranking = std::get_if<Ranking>(&term.function))
        rankPartition(term, *ranking, first, last, order, keys, rows, slot);
      else if (const auto buckets = std::get_if<TileBuckets>(&term.function))
        tilePartition(term, *buckets, first, last, order, rows, slot);
      else if (const auto value = std::get_if<FrameValue>(&term.function))
        frameValuePartition(term, *value, first, last, order, keys, rows, slot);
      else if (const auto offset = std::get_if<OffsetValue>(&term.function))
        offsetPartition(term, *offset, first, last, order, rows, slot);
      else
        aggregatePartition(term, std::get<AggregateTerm>(term.function), first, last, order, keys, rows, slot);
      first = last;
    }
  }
  Rows windowProjection(const RelNode &project, const RelNode &window, std::size_t depth) {
    if (window.query != project.query || window.inputs.size() != 1)
      fail(window.origin, "local window requires one independent input");
    const auto &input = plan.nodes.at(window.inputs.front());
    const auto [group, having] = groupedInput(input);
    auto data = group ? aggregateRows(project, *group, having, depth + 1 + bool(having)) :
        GroupedRows{input, {project.query, {}, std::nullopt}, execute(window.inputs.front(), depth + 1)};
    auto &bindings = data.bindings;
    auto &rows = data.rows;
    const auto width = data.schema.outputs.size() + bindings.terms.size();
    std::vector<WindowTerm> terms;
    std::vector<ExpressionRef> work = project.keys;
    std::set<ExprId> visited;
    while (!work.empty()) {
      const auto ref = work.back();
      work.pop_back();
      const auto &e = modelExpression(plan, ref);
      step(e.origin);
      if (ref.query != project.query || !visited.insert(ref.expression).second) continue;
      if (e.kind == ExprKind::Window) {
        charge(e.origin, sizeof(WindowTerm) + 2 * sizeof(std::size_t));
        terms.push_back(windowTerm(ref, e, data.schema, bindings));
        bindings.terms.emplace(ref.expression, width + terms.size() - 1);
      } else {
        for (auto arg : e.operands) work.push_back({ref.query, arg});
      }
    }
    rowBudget(window.origin, width + terms.size(), rows.size());
    for (auto &row : rows) row.resize(width + terms.size());
    std::vector<bool> completed(terms.size());
    charge(window.origin,completed.size());
    for (const auto &group:window.windowGroups) {
      std::optional<WindowOrder> order;
      for (auto ref:group.functions) {
        step(window.origin);
        if (ref.query!=project.query) continue;
        const auto slot=bindings.terms.find(ref.expression);
        if (slot==bindings.terms.end() || slot->second<width || slot->second-width>=terms.size()) continue;
        const auto term=slot->second-width;
        if (completed[term]) continue;
        terms[term].sumZero=std::any_of(window.nonNullWindowSums.begin(),window.nonNullWindowSums.end(),
            [&](const auto &proof){return proof.query==ref.query && proof.expression==ref.expression;});
        if (!order) order=orderWindow(terms[term],rows);
        windowRows(terms[term],*order,rows,slot->second);
        completed[term]=true;
      }
    }
    for (std::size_t i=0;i<terms.size();++i)
      if (!completed[i]) windowRows(terms[i],orderWindow(terms[i],rows),rows,width+i);
    return projection(project, data.schema, std::move(rows), &bindings);
  }
  struct Pagination {
    std::optional<std::uint64_t> limit;
    std::uint64_t offset = 0;
  };

  Pagination pagination(const RelNode &n) {
    const auto &query=modelQuery(plan,n.query);
    const auto binding=[&](ExprId id) -> std::optional<std::uint64_t> {
      if (sqlite || !parameters || !id)
        fail(n.origin,"local pagination requires nonnegative integer literals or prepared parameters");
      const auto &expression=modelExpression(plan,{query.id,id});
      const auto *value=parameterBinding(expression);
      if (!value) fail(expression.origin,"local pagination requires a direct prepared parameter");
      if (isNull(*value)) return {};
      const auto *number=std::get_if<std::int64_t>(value);
      if (!number || *number<0)
        fail(expression.origin,"local pagination requires a nonnegative bound integer",DiagnosticCode::InvalidQueryShape);
      return static_cast<std::uint64_t>(*number);
    };
    Pagination page;
    page.offset=n.rowOffset ? static_cast<std::uint64_t>(*n.rowOffset) : binding(query.offset).value_or(0);
    if (n.rowLimit) page.limit=static_cast<std::uint64_t>(*n.rowLimit);
    else if (!n.keys.empty()) page.limit=binding(query.limit);
    return page;
  }

  Rows limit(const RelNode &n, const RelNode &input, Rows rows, const Pagination &page) {
    const auto skip=static_cast<std::size_t>(std::min<std::uint64_t>(page.offset,rows.size()));
    rows.erase(rows.begin(),rows.begin()+skip);
    if (!page.limit || *page.limit>=rows.size()) return rows;
    auto end=static_cast<std::size_t>(*page.limit);
    if (n.withTies && end > 0) {
      if (input.kind != RelKind::Sort || input.inputs.size() != 1 || input.keys.empty())
        fail(n.origin, "local WITH TIES needs an ordered input", DiagnosticCode::InvalidQueryShape);
      const auto &schema = plan.nodes.at(input.inputs.front());
      const auto keys = compileAll(input.keys, &schema, true, input.origin);
      charge(n.origin, keys.size() * sizeof(LocalValue));
      LocalRow last;
      last.reserve(keys.size());
      for (const auto &key : keys) last.push_back(eval(key, rows[end - 1]));
      auto peer = [&](const LocalRow &row) {
        for (std::size_t i = 0; i < keys.size(); ++i)
          if (last[i] != eval(keys[i], row)) return false;
        return true;
      };
      while (end < rows.size() && peer(rows[end])) ++end;
    }
    rows.resize(end);
    return rows;
  }
  void checkScalarDependencies(const RelNode &n, std::size_t primary) {
    if (n.inputs.size() < primary)
      fail(n.origin, "local operator is missing a relational input", DiagnosticCode::InvalidQueryShape);
    for (std::size_t i = primary; i < n.inputs.size(); ++i) {
      step(n.origin);
      const auto &dependency = plan.nodes.at(n.inputs[i]);
      if (dependency.kind != RelKind::Subquery || !dependency.scalar)
        fail(dependency.origin, "local operator has an unsupported dependent input");
    }
  }
  void collectScanColumns() {
    for (const auto &node : plan.nodes) {
      step(node.origin);
      if (node.kind != RelKind::Scan || !node.table) continue;
      charge(node.origin, node.outputs.size() + 96);
      const auto &bound=modelQuery(plan,node.query).tables.at(*node.table);
      scanColumns.try_emplace({node.query,*node.table},node.outputs.size(),
                              bool(bound.pivot || bound.unpivot));
    }
    const auto mark = [&](const ColumnBinding &column, SourceRange at) {
      const auto found = scanColumns.find({column.scope,column.table});
      if (found == scanColumns.end()) return;
      const auto &binding=modelQuery(plan,column.scope).tables.at(column.table);
      // Transform outputs have their own ordinal space. The scan carries
      // the full source shape for grouping, matching and NULL tests.
      if (binding.pivot || binding.unpivot) return;
      if (column.column >= found->second.size())
        fail(at, "invalid federated scan column binding", DiagnosticCode::InvalidQueryShape);
      found->second[column.column] = true;
    };
    for (const auto &query : plan.program.queries) {
      for (const auto &e : query.expressions) {
        step(e.origin);
        if (e.binding) mark(*e.binding, e.origin);
        for (const auto &column : e.coalescedBindings) {
          step(e.origin);
          mark(column, e.origin);
        }
      }
      const bool star = query.tableQuery != TableQuerySyntax::None ||
          std::any_of(query.projections.begin(), query.projections.end(), [&](const auto &slot) {
            return modelExpression(plan, {query.id,slot.expression}).kind == ExprKind::Star;
          });
      if (!star) continue;
      for (auto input = scanColumns.lower_bound({query.id,0});
           input != scanColumns.end() && input->first.first == query.id; ++input)
        std::fill(input->second.begin(), input->second.end(), true);
    }
  }
  std::optional<std::size_t> scanColumn(ExpressionRef ref,const RelNode &scan) {
    if (sqlite || scan.kind!=RelKind::Scan || !scan.table || scan.query!=ref.query) return {};
    const auto &q=modelQuery(plan,scan.query);
    if (!q.catalogChecked || *scan.table>=q.tables.size()) return {};
    const auto &binding=q.tables[*scan.table];
    if (binding.pivot || binding.unpivot || !binding.definition) return {};
    const auto &table=*binding.definition;
    const auto &e=modelExpression(plan,ref);
    if (!table.complete || table.kind!=ObjectKind::Table || e.kind!=ExprKind::Name ||
        !e.binding || e.localSymbol || e.outputColumn || !e.coalescedBindings.empty() ||
        e.binding->scope!=scan.query || e.binding->table!=*scan.table ||
        e.binding->column>=table.columns.size()) return {};
    return e.binding->column;
  }
  std::optional<std::size_t> scanExactColumn(ExpressionRef ref,const RelNode &scan,bool allowNumeric=true) {
    const auto ordinal=scanColumn(ref,scan);
    if (!ordinal) return {};
    const auto &table=*modelQuery(plan,scan.query).tables[*scan.table].definition;
    const auto &type=table.columns[*ordinal].sqlType;
    return type && (signedIntegerBits(*type) || (allowNumeric && numericType(*type))) ? ordinal : std::nullopt;
  }
  std::optional<RelationKeySet> scanMembershipKeys(ExpressionRef ref,const RelNode &scan) {
    const auto &predicate=modelExpression(plan,ref);
    const bool list=predicate.kind==ExprKind::In && predicate.value=="IN" &&
                    predicate.operands.size()>1 && predicate.operands.size()<=257;
    const bool quantified=predicate.kind==ExprKind::Quantified && predicate.operands.size()==2 &&
                          (predicate.value=="= ANY" || predicate.value=="= SOME");
    if (predicate.query || (!list && !quantified)) return {};
    const auto ordinal=scanExactColumn({ref.query,predicate.operands.front()},scan,false);
    if (!ordinal) return {};
    const auto &type=modelQuery(plan,scan.query).tables[*scan.table].definition->columns[*ordinal].sqlType;
    const auto width=*signedIntegerBits(*type);
    RelationKeySet keys;
    keys.columns.push_back({*ordinal,width});
    const auto add=[&](std::int64_t value) {
      if (!fitsSignedInteger(value,width) ||
          std::any_of(keys.keys.begin(),keys.keys.end(),[&](const auto &key) {return key[0]==value;}))
        return true;
      if (keys.keys.size()==RelationKeySet::maxKeys) return false;
      keys.keys.push_back({value});
      return true;
    };
    if (list) {
      for (std::size_t i=1;i<predicate.operands.size();++i) {
        step(predicate.origin);
        const ExpressionRef item{ref.query,predicate.operands[i]};
        const auto &expression=modelExpression(plan,item);
        if (expression.kind==ExprKind::Null) continue;
        if (!signedIntegerBits(plan.program.types.get(expression.sqlType))) return {};
        auto value=exactIntegerLiteral(plan,item);
        if (const auto *bound=parameterBinding(expression)) {
          if (isNull(*bound)) continue;
          const auto *integer=std::get_if<std::int64_t>(bound);
          if (!integer) return {};
          value=*integer;
        }
        if (!value || !add(*value)) return {};
      }
    } else {
      const ExpressionRef source{ref.query,predicate.operands.back()};
      const auto &array=modelExpression(plan,source);
      const auto &body=array.kind==ExprKind::Cast && array.operands.size()==1?
          modelExpression(plan,{ref.query,array.operands.front()}):array;
      // Bracket arrays and typed NULL do not depend on PostgreSQL's
      // session-specific parsing of textual array values.
      if (body.kind!=ExprKind::ArrayConstructor && body.kind!=ExprKind::Null) return {};
      const auto literal=postgresIntegerArrayLiteral(plan,source,256,[&](auto work) {step(predicate.origin,work);});
      if (literal.status!=IntegerArrayStatus::Ready) return {};
      for (const auto &value:literal.elements)
        if (value && !add(*value)) return {};
    }
    charge(predicate.origin,sizeof(RelationKeySet)+keys.keys.size()*sizeof(RelationKeySet::Key));
    return keys;
  }
  std::optional<std::int64_t> scanIntegerLiteral(ExpressionRef ref) {
    const auto &value=modelExpression(plan,ref);
    if (!signedIntegerBits(plan.program.types.get(value.sqlType))) return {};
    if (value.kind==ExprKind::Number) return exactIntegerText(value.value);
    if (value.kind==ExprKind::Unary && value.operands.size()==1 &&
        (value.value=="+" || value.value=="-")) {
      const auto &literal=modelExpression(plan,{ref.query,value.operands.front()});
      if (literal.kind==ExprKind::Number && literal.value.size()<=19)
        return exactIntegerText(value.value=="-" ? "-"+literal.value : literal.value);
    }
    return {};
  }
  std::optional<LocalNumeric> scanNumericLiteral(ExpressionRef ref) {
    const auto &value=modelExpression(plan,ref);
    if (!numericType(plan.program.types.get(value.sqlType))) return {};
    if (value.kind==ExprKind::Number && value.value.size()<=128)
      return LocalNumeric::parse(value.value);
    if (value.kind==ExprKind::Unary && value.operands.size()==1 && (value.value=="+" || value.value=="-")) {
      const auto &literal=modelExpression(plan,{ref.query,value.operands.front()});
      if (literal.kind==ExprKind::Number && literal.value.size()<=128)
        return LocalNumeric::parse(value.value=="-"?"-"+literal.value:literal.value);
    }
    return {};
  }
  std::optional<RelationPredicate> scanPredicate(ExpressionRef ref, const RelNode &scan) {
    const auto column=[&](ExprId id) { return scanExactColumn({ref.query,id},scan); };
    const auto &e=modelExpression(plan,ref);
    if (e.kind==ExprKind::IsNull && e.operands.size()==1 &&
        (e.value=="IS NULL" || e.value=="IS NOT NULL")) {
      const auto index=scanColumn({ref.query,e.operands.front()},scan);
      if (!index) return {};
      return RelationPredicate{*index,e.value=="IS NULL"?RelationPredicateKind::IsNull:RelationPredicateKind::IsNotNull};
    }
    if (e.kind!=ExprKind::Binary || e.operands.size()!=2) return {};
    auto index=column(e.operands.front());
    const bool reversed=!index;
    if (reversed) index=column(e.operands[1]);
    if (!index) return {};
    const ExpressionRef constant{ref.query,e.operands[reversed?0:1]};
    const auto &value=modelExpression(plan,constant);
    const auto &type=plan.program.types.get(value.sqlType);
    const auto &table=*modelQuery(plan,scan.query).tables[*scan.table].definition;
    const bool numericColumn=numericType(*table.columns[*index].sqlType);
    const auto *bound=parameterBinding(value);
    if (!bound && (signedIntegerBits(type) || (numericColumn && numericType(type))))
      bound=capturedColumn(constant);
    const auto *integer=bound?std::get_if<std::int64_t>(bound):nullptr;
    const auto number=integer?std::optional<std::int64_t>(*integer):scanIntegerLiteral(constant);
    const bool null=value.kind==ExprKind::Null || (bound && isNull(*bound));
    std::optional<LocalNumeric> numeric;
    if (numericColumn && !number && !null) {
      if (const auto *decimal=bound?std::get_if<LocalNumeric>(bound):nullptr) numeric=*decimal;
      else numeric=scanNumericLiteral(constant);
    }
    if (!number && !null && !numeric) return {};
    if (numeric) charge(e.origin,LocalNumeric::storageBytes);
    const auto &op = e.value;
    RelationPredicateKind kind;
    if (op == "=") kind = RelationPredicateKind::Equal;
    else if (op == "<>" || op == "!=") kind = RelationPredicateKind::NotEqual;
    else if (op == "<") kind = reversed ? RelationPredicateKind::Greater : RelationPredicateKind::Less;
    else if (op == "<=") kind = reversed ? RelationPredicateKind::GreaterEqual : RelationPredicateKind::LessEqual;
    else if (op == ">") kind = reversed ? RelationPredicateKind::Less : RelationPredicateKind::Greater;
    else if (op == ">=") kind = reversed ? RelationPredicateKind::LessEqual : RelationPredicateKind::GreaterEqual;
    else return {};
    return RelationPredicate{*index,kind,number.value_or(0),null,std::move(numeric)};
  }
  std::optional<RelationKeySet> scanDisjunctionKeys(ExpressionRef root,const RelNode &scan) {
    const auto &expression=modelExpression(plan,root);
    if (expression.kind!=ExprKind::Binary || expression.value!="OR") return {};
    std::optional<RelationKeySet> combined;
    std::vector<ExpressionRef> pending{root};
    charge(expression.origin,sizeof(ExpressionRef));
    std::size_t visited=0;
    while (!pending.empty()) {
      if (++visited>2*RelationKeySet::maxKeys-1) return {};
      const auto ref=pending.back();
      pending.pop_back();
      const auto &arm=modelExpression(plan,ref);
      step(arm.origin);
      if (arm.kind==ExprKind::Binary && arm.value=="OR" && arm.operands.size()==2) {
        charge(arm.origin,2*sizeof(ExpressionRef));
        pending.push_back({ref.query,arm.operands.back()});
        pending.push_back({ref.query,arm.operands.front()});
        continue;
      }
      auto keys=scanMembershipKeys(ref,scan);
      if (!keys) {
        const auto equal=scanPredicate(ref,scan);
        if (!equal || equal->kind!=RelationPredicateKind::Equal || equal->numericConstant) return {};
        const auto &table=*modelQuery(plan,scan.query).tables[*scan.table].definition;
        const auto &type=table.columns.at(equal->column).sqlType;
        const auto width=type?signedIntegerBits(*type):std::nullopt;
        if (!width) return {};
        keys.emplace();
        keys->columns.push_back({equal->column,*width});
        if (!equal->constantIsNull && fitsSignedInteger(equal->constant,*width))
          keys->keys.push_back({equal->constant});
        charge(arm.origin,sizeof(RelationKeySet)+sizeof(RelationKeySet::Column)+
                          keys->keys.size()*sizeof(RelationKeySet::Key));
      }
      if (!combined) {
        combined=std::move(keys);
        continue;
      }
      if (combined->columns.front().ordinal!=keys->columns.front().ordinal) return {};
      for (const auto &key:keys->keys) {
        step(arm.origin,combined->keys.size()+1);
        if (std::find(combined->keys.begin(),combined->keys.end(),key)!=combined->keys.end()) continue;
        if (combined->keys.size()==RelationKeySet::maxKeys) return {};
        charge(arm.origin,sizeof(RelationKeySet::Key));
        combined->keys.push_back(key);
      }
    }
    return combined;
  }
  struct ScanRestrictions {
    std::vector<RelationPredicate> predicates;
    std::optional<RelationKeySet> keys;
    bool coversAllPredicates = true;
  };
  std::optional<ScanRestrictions> scanRestrictions(const RelNode &filter, const RelNode &scan,
                                                   bool allowMembership = true) {
    if (filter.query != scan.query || filter.inputs.size() != 1 || filter.predicates.size() > 64) return {};
    ScanRestrictions out;
    std::map<std::size_t, RelationKeySet> byColumn;
    charge(filter.origin, filter.predicates.size() * sizeof(RelationPredicate));
    for (auto ref : filter.predicates) {
      step(filter.origin);
      if (auto predicate = scanPredicate(ref, scan)) {
        out.predicates.push_back(std::move(*predicate));
        continue;
      }
      auto keys = allowMembership ? scanMembershipKeys(ref, scan) : std::nullopt;
      if (!keys && allowMembership) keys=scanDisjunctionKeys(ref,scan);
      // Only prefilter when every conjunct has a non-throwing supported form.
      if (!keys) return {};
      const auto ordinal = keys->columns.front().ordinal;
      auto [entry, inserted] = byColumn.try_emplace(ordinal, std::move(*keys));
      if (inserted) {
        charge(filter.origin, sizeof(decltype(byColumn)::value_type));
        continue;
      }
      auto &intersection = entry->second.keys;
      intersection.erase(std::remove_if(intersection.begin(), intersection.end(), [&](const auto &key) {
        step(filter.origin, keys->keys.size() + 1);
        return std::find(keys->keys.begin(), keys->keys.end(), key) == keys->keys.end();
      }), intersection.end());
    }
    if (!byColumn.empty()) {
      out.coversAllPredicates = byColumn.size() == 1;
      // One independent key set is enough for a scan hint. Every original
      // conjunct is still evaluated by Filter or Calc after the source read.
      const auto smallest = std::min_element(byColumn.begin(), byColumn.end(), [](const auto &left, const auto &right) {
        return left.second.keys.size() < right.second.keys.size();
      });
      out.keys = std::move(smallest->second);
    }
    return out;
  }
  using JoinPredicates = std::array<std::vector<RelationPredicate>, 2>;
  JoinPredicates joinScanPredicates(const RelNode &n, const RelNode *where) {
    if (!relations || sqlite || n.inputs.size() != 2 || n.fixedOrder || n.distinct || n.correlated ||
        n.predicates.size() > 64 || (n.join != JoinKind::Inner && n.join != JoinKind::Left &&
                                    n.join != JoinKind::Right && n.join != JoinKind::Cross)) return {};
    const auto &left = plan.nodes.at(n.inputs[0]);
    const auto &right = plan.nodes.at(n.inputs[1]);
    if (left.kind != RelKind::Scan || right.kind != RelKind::Scan || !left.table || !right.table ||
        left.table == right.table || left.query != n.query || right.query != n.query) return {};
    const auto &query = modelQuery(plan, n.query);
    if (!query.catalogChecked) return {};
    const auto int32Column = [&](const ColumnBinding &binding) {
      if (binding.table >= query.tables.size()) return false;
      const auto &table = query.tables[binding.table].definition;
      if (!table || binding.column >= table->columns.size()) return false;
      const auto &type = table->columns[binding.column].sqlType;
      return type && signedIntegerBits(*type) == 32;
    };
    const auto side = [&](ExpressionRef ref) -> std::optional<std::pair<std::size_t, RelationPredicate>> {
      if (auto predicate = scanPredicate(ref, left)) return std::make_pair(0u, *predicate);
      if (auto predicate = scanPredicate(ref, right)) return std::make_pair(1u, *predicate);
      return {};
    };
    JoinPredicates on;
    using ColumnPair = std::array<std::size_t, 2>;
    std::vector<ColumnPair> equalityKeys;
    charge(n.origin, n.predicates.size() * (sizeof(RelationPredicate) + sizeof(ColumnPair)));
    for (auto ref : n.predicates) {
      step(n.origin);
      if (const auto predicate = side(ref)) {
        const auto i = predicate->first;
        // Preserved-side conditions affect whether a row matches, not
        // whether the outer join emits the preserved row.
        if ((i == 0 && n.join != JoinKind::Left) || (i == 1 && n.join != JoinKind::Right))
          on[i].push_back(predicate->second);
        continue;
      }
      const auto equal = integerEqualityColumns(plan, ref, IntegerEquality::ExactSigned);
      if (ref.query != n.query || !equal ||
          !((equal->first.table == *left.table && equal->second.table == *right.table) ||
            (equal->second.table == *left.table && equal->first.table == *right.table))) return {};
      const auto &test = modelExpression(plan, ref);
      const auto &a = *modelExpression(plan, {ref.query, test.operands[0]}).binding;
      const auto &b = *modelExpression(plan, {ref.query, test.operands[1]}).binding;
      if (int32Column(a) && int32Column(b))
        equalityKeys.push_back(a.table == *left.table ? ColumnPair{a.column, b.column}
                                                      : ColumnPair{b.column, a.column});
    }
    JoinPredicates restriction;
    if (where && where->query == n.query && where->predicates.size() <= 64) {
      charge(where->origin, where->predicates.size() * sizeof(RelationPredicate));
      for (auto ref : where->predicates) {
        step(where->origin);
        const auto predicate = side(ref);
        if (!predicate) {
          restriction = {};
          break;
        }
        const auto i = predicate->first;
        // Applying WHERE on the null-extended side may manufacture a row
        // that now satisfies the original filter.
        if ((i == 0 && n.join != JoinKind::Right) || (i == 1 && n.join != JoinKind::Left))
          restriction[i].push_back(predicate->second);
      }
    }
    for (std::size_t i = 0; i < on.size(); ++i)
      on[i].insert(on[i].end(), restriction[i].begin(), restriction[i].end());
    const auto add = [&](std::size_t side, RelationPredicate predicate) {
      if (on[side].size() >= 64) return;
      const auto existing = std::any_of(on[side].begin(), on[side].end(), [&](const auto &p) {
        return p.column == predicate.column && p.kind == predicate.kind &&
               p.constant == predicate.constant && p.constantIsNull == predicate.constantIsNull &&
               p.numericConstant == predicate.numericConstant;
      });
      if (!existing) {
        charge(n.origin, sizeof(RelationPredicate));
        on[side].push_back(predicate);
      }
    };
    const std::array originalSizes{on[0].size(), on[1].size()};
    for (std::size_t from = 0; from < on.size(); ++from) {
      const auto to = 1 - from;
      if ((to == 0 && n.join == JoinKind::Left) || (to == 1 && n.join == JoinKind::Right))
        continue;
      for (const auto &key : equalityKeys) {
        for (std::size_t i = 0; i < originalSizes[from] && on[to].size() < 64; ++i) {
          step(n.origin);
          const auto source = on[from][i];
          if (source.column != key[from] || source.kind == RelationPredicateKind::IsNull ||
              source.numericConstant) continue;
          add(to, {key[to], source.kind, source.constant, source.constantIsNull});
        }
      }
    }
    for (const auto &key : equalityKeys) {
      if (n.join != JoinKind::Left) add(0, {key[0], RelationPredicateKind::IsNotNull});
      if (n.join != JoinKind::Right) add(1, {key[1], RelationPredicateKind::IsNotNull});
    }
    return on;
  }
  std::optional<RelationKeySet> lookupJoinKeys(const RelNode &join, const Rows &leftRows) {
    if (!relations || join.fixedOrder || join.correlated || join.distinct || join.predicates.empty() ||
        join.predicates.size()>RelationKeySet::maxWidth ||
        join.inputs.size() != 2 || (join.join != JoinKind::Inner && join.join != JoinKind::Left) ||
        leftRows.size()>RelationKeySet::maxKeys) return {};
    const auto &left = plan.nodes.at(join.inputs[0]), &right = plan.nodes.at(join.inputs[1]);
    if(left.kind!=RelKind::Scan || right.kind!=RelKind::Scan)return {};
    const auto &query=modelQuery(plan,right.query);
    std::array<std::size_t,RelationKeySet::maxWidth> sourceOrdinals{};
    RelationKeySet result;
    charge(join.origin, join.predicates.size()*sizeof(RelationKeySet::Column)+
                       leftRows.size()*sizeof(RelationKeySet::Key));
    for(auto ref:join.predicates) {
      step(join.origin);
      const auto &predicate=modelExpression(plan,ref);
      if(predicate.kind!=ExprKind::Binary || predicate.value!="=" || predicate.operands.size()!=2)
        return {};
      const ExpressionRef a{ref.query,predicate.operands[0]}, b{ref.query,predicate.operands[1]};
      auto source=scanExactColumn(a,left,false);
      auto target=scanExactColumn(b,right,false);
      if(!source || !target) {
        source=scanExactColumn(b,left,false);
        target=scanExactColumn(a,right,false);
      }
      if(!source || !target)return {};
      const auto &type=query.tables.at(*right.table).definition->columns.at(*target).sqlType;
      const auto width=signedIntegerBits(*type);
      sourceOrdinals[result.columns.size()]=*source;
      result.columns.push_back({*target,*width});
    }
    result.keys.reserve(leftRows.size());
    for (const auto &row : leftRows) {
      RelationKeySet::Key key{};
      bool matches=true;
      for(std::size_t i=0;i<result.columns.size();++i) {
        step(join.origin);
        const auto value=std::get_if<std::int64_t>(&row.at(sourceOrdinals[i]));
        const auto bits=result.columns[i].width;
        if(!value) {
          matches=false;
          break;
        }
        if(bits<64) {
          const auto bound=std::int64_t{1}<<(bits-1);
          if(*value< -bound || *value>=bound) {
            matches=false;
            break;
          }
        }
        key[i]=*value;
      }
      if(matches)result.keys.push_back(key);
    }
    std::sort(result.keys.begin(), result.keys.end());
    result.keys.erase(std::unique(result.keys.begin(), result.keys.end()), result.keys.end());
    return result;
  }
  struct PageAtScan {
    RelId scan;
    RelationPage page;
  };
  std::optional<PageAtScan> orderedPage(const RelNode &limit,const Pagination &counts) {
    if (!relations || sqlite || limit.withTies || limit.inputs.size() != 1 ||
        !counts.limit || counts.offset>limits.maxRows || *counts.limit>limits.maxRows-counts.offset)
      return {};
    const auto &sort = plan.nodes.at(limit.inputs.front());
    if (sort.kind != RelKind::Sort || sort.query != limit.query || sort.inputs.size() != 1 ||
        sort.keys.empty() || sort.keys.size() != sort.descending.size() ||
        sort.keys.size() != sort.nullOrdering.size())
      return {};
    const auto &project = plan.nodes.at(sort.inputs.front());
    if (project.kind != RelKind::Project || project.query != sort.query || project.inputs.size() != 1 ||
        project.keys.empty() || project.keys.size() != project.outputs.size())
      return {};
    auto source = project.inputs.front();
    const auto &below = plan.nodes.at(source);
    if (below.kind == RelKind::Filter) {
      if (below.inputs.size() != 1) return {};
      source = below.inputs.front();
    }
    const auto &scan = plan.nodes.at(source);
    const auto &q = modelQuery(plan, project.query);
    if (scan.kind != RelKind::Scan || scan.query != q.id || !scan.table || *scan.table >= q.tables.size())
      return {};
    const auto &bound = q.tables.at(*scan.table);
    if (!q.catalogChecked || !bound.definition || !bound.definition->complete ||
        bound.definition->kind != ObjectKind::Table || !relations->supportsOrderedReads(*bound.definition))
      return {};
    if (below.kind == RelKind::Filter) {
      const auto filters=scanRestrictions(below,scan,relations->supportsOrderedKeyReads(*bound.definition));
      if (!filters || !filters->coversAllPredicates ||
          !relations->supportsOrderedFilters(*bound.definition,filters->predicates)) return {};
    }

    charge(limit.origin, project.keys.size() * (sizeof(std::size_t) + sizeof(char)) +
                         sort.keys.size() * sizeof(RelationOrderKey));
    std::vector<std::size_t> ordinals;
    for (auto ref : project.keys) {
      const auto &column = modelExpression(plan, ref);
      step(column.origin);
      if (!boundScalarColumn(plan, ref) || column.binding->table != *scan.table ||
          !signedIntegerBits(plan.program.types.get(column.sqlType)))
        return {};
      ordinals.push_back(column.binding->column);
    }
    std::vector<char> ordered(ordinals.size());
    PageAtScan out{source, {static_cast<std::size_t>(*counts.limit + counts.offset), {}}};
    for (std::size_t i = 0; i < sort.keys.size(); ++i) {
      const auto ref=sort.keys[i];
      const auto &key = modelExpression(plan, ref);
      step(key.origin);
      auto projected=key.outputColumn;
      if (!projected && boundScalarColumn(plan,ref) && key.binding->table==*scan.table) {
        const auto column=key.binding->column;
        step(key.origin,ordinals.size());
        const auto found=std::find(ordinals.begin(),ordinals.end(),column);
        if (found!=ordinals.end() && std::find(std::next(found),ordinals.end(),column)==ordinals.end())
          projected=std::size_t(std::distance(ordinals.begin(),found));
      }
      if (!projected || *projected>=ordinals.size()) return {};
      const auto output=*projected;
      ordered[output] = true;
      out.page.order.push_back({ordinals[output], sort.descending[i],
          sort.nullOrdering[i] == "FIRST" || (sort.nullOrdering[i].empty() && sort.descending[i])});
    }
    // If every visible value participates in the order, truncation at a
    // tie cannot select a different visible row from that tie group.
    if (std::find(ordered.begin(), ordered.end(), false) != ordered.end()) return {};
    return out;
  }
  Rows scan(const RelNode &n, const ModelStatement &query, const std::vector<RelationPredicate> &predicates,
            const RelationPage *page, const RelationKeySet *keySet = nullptr) {
    if (!relations)
      fail(n.origin, "table scan requires an explicit local relation provider");
    if (!query.catalogChecked || !n.table || *n.table >= query.tables.size())
      fail(n.origin, "local table scan requires complete bound catalog metadata");
    const auto &bound = query.tables.at(*n.table);
    const auto &definition=sourceDefinition(bound);
    if (!definition || !definition->complete || !definition->arityKnown ||
        definition->database.empty() || definition->name.empty() || n.outputs.empty() ||
        definition->columns.size() != n.outputs.size() || bound.pseudo || bound.query ||
        !bound.columnAliases.empty())
      fail(n.origin, "local table scan requires a verified relation and output shape");
    const auto &table = *definition;
    charge(n.origin, table.columns.size() * sizeof(std::size_t));
    std::vector<std::size_t> columns;
    columns.reserve(table.columns.size());
    const auto &needed = scanColumns.at({n.query,*n.table});
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
      step(n.origin);
      const auto &column = table.columns[i];
      if (n.outputs[i].name != column.name)
        fail(n.origin, "local scan column identities do not match the catalog", DiagnosticCode::InvalidQueryShape);
      if (!needed[i]) continue;
      const auto &type = column.sqlType;
      if (!type || type->arrayDimensions || !type->opaqueName.empty())
        fail(n.origin, "local table scan requires verified scalar column types");
      columns.push_back(i);
      if (type->family == TypeFamily::Boolean || type->family == TypeFamily::Character ||
          type->family == TypeFamily::VarCharacter || type->family == TypeFamily::String ||
          (!sqlite && numericType(*type))) continue;
      if (type->family != TypeFamily::Integer ||
          !(type->unsignedInteger == false || (sqlite && type->sqliteAffinity)) ||
          (type->bits && (*type->bits < 2 || *type->bits > 64)))
        fail(n.origin, "local table scan supports signed integer, Boolean, numeric and text columns");
      if (!type->bits && !(sqlite && type->sqliteAffinity))
        fail(n.origin, "local table scan needs a known integer width");
    }
    const auto shouldStop = [end = deadline, cancelled = limits.shouldStop] {
      return Clock::now() >= end || (cancelled && cancelled());
    };
    auto cursor = relations->open({table, bound.inheritance, limits.maxRows,
        limits.maxBytes - bytes, deadline, shouldStop, columns, predicates, page, keySet});
    step(n.origin);
    if (!cursor)
      fail(n.origin, "local relation source was not found");
    Rows rows;
    for (;;) {
      step(n.origin);
      auto row = cursor->next();
      step(n.origin);
      if (!row) break;
      if (page && rows.size() == page->prefixRows)
        fail(n.origin, "ordered relation provider exceeded its requested prefix", DiagnosticCode::InvalidQueryShape);
      if (rows.size() == limits.maxRows)
        fail(n.origin, "local table scan exceeded its row budget", DiagnosticCode::ResourceLimit);
      if (row->size() != columns.size())
        fail(n.origin, "local relation provider returned an unexpected row width", DiagnosticCode::InvalidQueryShape);
      charge(n.origin, sizeof(LocalRow) + table.columns.size() * sizeof(LocalValue));
      for (std::size_t i = 0; i < columns.size(); ++i) {
        step(n.origin);
        auto &value = row->at(i);
        if (isNull(value)) continue;
        const auto &type = *table.columns[columns[i]].sqlType;
        if (type.family == TypeFamily::Boolean) {
          const auto flag = std::get_if<bool>(&value);
          if (!flag)
            fail(n.origin, "local relation provider returned a non-Boolean value", DiagnosticCode::InvalidQueryShape);
          value = boolean(*flag);
        } else if (type.family == TypeFamily::Integer) {
          const auto number = std::get_if<std::int64_t>(&value);
          if (!number)
            fail(n.origin, "local relation provider returned a non-integer value", DiagnosticCode::InvalidQueryShape);
          const auto bits = type.bits.value_or(64);
          if (bits < 64) {
            const auto range = std::int64_t{1} << (bits - 1);
            if (*number < -range || *number >= range)
              fail(n.origin, "local relation provider returned an out-of-range integer",
                   DiagnosticCode::InvalidQueryShape);
          }
        } else if (numericType(type)) {
          const auto numeric=std::get_if<LocalNumeric>(&value);
          if (!numeric || (type.scale && numeric->scale()!=std::size_t(std::max(*type.scale,0))))
            fail(n.origin,"local relation provider returned a numeric value of the wrong type or scale",
                 DiagnosticCode::InvalidQueryShape);
          charge(n.origin,LocalNumeric::storageBytes);
        } else {
          const auto text = std::get_if<LocalText>(&value);
          if (!text)
            fail(n.origin, "local relation provider returned a non-text value", DiagnosticCode::InvalidQueryShape);
          charge(n.origin, sizeof(std::string) + text->utf8().size());
        }
      }
      if (row->size() == table.columns.size()) {
        rows.push_back(std::move(*row));
      } else {
        LocalRow expanded(table.columns.size());
        for (std::size_t i = 0; i < row->size(); ++i)
          expanded[columns[i]] = std::move(row->at(i));
        rows.push_back(std::move(expanded));
      }
    }
    return rows;
  }
  std::optional<Rows> readExtremum(const RelNode &project) {
    if (!relations) return {};
    const auto lookup = matchExtremumLookup(plan, project);
    if (!lookup) return {};
    const auto &source = plan.nodes.at(lookup->scan);
    const auto &scope = modelQuery(plan, source.query);
    if (!relations->supportsOrderedReads(*scope.tables[*source.table].definition)) return {};
    const std::vector<RelationPredicate> predicates{{lookup->column, RelationPredicateKind::IsNotNull}};
    if (!relations->supportsOrderedFilters(*scope.tables[*source.table].definition,predicates)) return {};
    const RelationPage page{1, {{lookup->column, lookup->descending, false}}};
    auto input = scan(source, scope, predicates, &page);
    auto value = input.empty() ? LocalValue{} : std::move(input.front().at(lookup->column));
    if (!input.empty() && isNull(value))
      fail(project.origin, "ordered relation provider did not apply the extremum predicate", DiagnosticCode::InvalidQueryShape);
    rowBudget(project.origin, 1, 1);
    return Rows{{std::move(value)}};
  }
  Rows unpivot(const RelNode &node,const ModelStatement &query,std::size_t depth) {
    if (sqlite || !node.table || *node.table>=query.tables.size() || node.inputs.size()!=1)
      fail(node.origin,"invalid local UNPIVOT relation");
    const auto &table=query.tables[*node.table];
    const auto &shape=table.unpivot;
    if (!shape || !table.unpivotInput || !table.definition || !table.definition->complete ||
        shape->measures.empty() || shape->descriptors.empty() || shape->inputs.empty() ||
        shape->inputOrdinals.size()!=shape->inputs.size() ||
        node.outputs.size()!=shape->retainedOrdinals.size()+shape->descriptors.size()+shape->measures.size())
      fail(node.origin,"local UNPIVOT requires a resolved row input");
    const auto firstMeasure=shape->retainedOrdinals.size()+shape->descriptors.size();
    for (std::size_t i=firstMeasure;i<node.outputs.size();++i) {
      const auto &type=table.definition->columns.at(i).sqlType;
      if (!type || !signedIntegerBits(*type))
        fail(node.origin,"local UNPIVOT requires verified signed integer measures");
    }
    charge(node.origin,shape->inputs.size()*(sizeof(LocalRow)+shape->descriptors.size()*sizeof(LocalValue)));
    std::vector<LocalRow> labels;
    for (std::size_t i=0;i<shape->inputs.size();++i) {
      const auto &input=shape->inputs[i];
      if (shape->inputOrdinals[i].size()!=shape->measures.size() || input.columns.size()!=shape->measures.size() ||
          (!input.labels.empty() && input.labels.size()!=shape->descriptors.size()))
        fail(input.origin,"invalid local UNPIVOT branch",DiagnosticCode::InvalidQueryShape);
      LocalRow row;
      if (input.labels.empty()) {
        auto label=unpivotDefaultLabel(input.columns);
        charge(input.origin,sizeof(std::string)+label.size());
        row.assign(shape->descriptors.size(),LocalText(std::move(label)));
      } else {
        for (auto id:input.labels) {
          step(input.origin);
          const auto &value=modelExpression(plan,{query.id,id});
          if (value.kind==ExprKind::String) {
            charge(value.origin,sizeof(std::string)+value.value.size());
            row.emplace_back(LocalText(value.value));
          } else if (value.kind==ExprKind::Null) {
            row.emplace_back();
          } else {
            fail(value.origin,"local UNPIVOT requires text or NULL labels");
          }
        }
      }
      labels.push_back(std::move(row));
    }
    auto source=execute(node.inputs.front(),depth+1);
    Rows output;
    const bool include=shape->includeNulls.value_or(false);
    for (const auto &row:source)
      for (std::size_t i=0;i<shape->inputOrdinals.size();++i) {
        const auto &measures=shape->inputOrdinals[i];
        bool anyValue=false;
        for (auto ordinal:measures) {
          step(node.origin);
          const auto &value=row.at(ordinal);
          if (isNull(value)) continue;
          if (!std::holds_alternative<std::int64_t>(value))
            fail(node.origin,"local UNPIVOT received a noninteger measure",DiagnosticCode::InvalidQueryShape);
          anyValue=true;
        }
        if (!include && !anyValue) continue;
        if (output.size()==limits.maxRows)
          fail(node.origin,"local UNPIVOT expansion exceeded the row budget",DiagnosticCode::ResourceLimit);
        rowBudget(node.origin,node.outputs.size(),1);
        LocalRow result;
        result.reserve(node.outputs.size());
        for (auto column:shape->retainedOrdinals) result.push_back(row.at(column));
        result.insert(result.end(),labels[i].begin(),labels[i].end());
        for (auto ordinal:measures) result.push_back(row.at(ordinal));
        output.push_back(std::move(result));
      }
    return output;
  }
  Rows pivot(const RelNode &node,const ModelStatement &query,std::size_t depth) {
    if (sqlite || !node.table || *node.table>=query.tables.size() || node.inputs.size()!=1)
      fail(node.origin,"invalid local PIVOT relation");
    const auto &table=query.tables[*node.table];
    const auto &shape=table.pivot;
    if (!shape || !table.definition || !table.definition->complete ||
        !table.pivotInput || shape->axisOrdinals.empty() || shape->buckets.empty() ||
        node.outputs.size()!=shape->groupingOrdinals.size()+shape->buckets.size())
      fail(node.origin,"local PIVOT needs a resolved input relation");
    const auto &fields=table.pivotInput->columns;
    for (auto axis:shape->axisOrdinals) {
      step(node.origin);
      if (axis>=fields.size() || !fields[axis].sqlType || !signedIntegerBits(*fields[axis].sqlType))
        fail(node.origin,"local PIVOT requires verified signed integer axes");
    }
    auto kind=AggregateKind::Count;
    if (shape->aggregate=="SUM") kind=AggregateKind::Sum;
    else if (shape->aggregate=="AVG") kind=AggregateKind::Average;
    else if (shape->aggregate=="MIN") kind=AggregateKind::Min;
    else if (shape->aggregate=="MAX") kind=AggregateKind::Max;
    else if (shape->aggregate!="COUNT") fail(shape->origin,"unsupported local PIVOT aggregate");
    AggregateTerm term{kind,shape->argumentOrigin,{},{},false,false,{}};
    if (shape->argumentOrdinal) {
      const auto column=*shape->argumentOrdinal;
      if (column>=fields.size())
        fail(shape->argumentOrigin,"invalid local PIVOT aggregate input",DiagnosticCode::InvalidQueryShape);
      if (kind!=AggregateKind::Count) {
        const auto &type=fields[column].sqlType;
        if (!type || (!signedIntegerBits(*type) && !numericType(*type)))
          fail(shape->argumentOrigin,"local PIVOT numeric aggregates require exact input types");
        if (kind==AggregateKind::Sum && (signedIntegerBits(*type)==64 || numericType(*type)))
          term.kind=AggregateKind::SumNumeric;
      }
      instruction(term.argument,{Op::Load,shape->argumentOrigin,{},column});
    } else if (kind!=AggregateKind::Count) {
      fail(shape->argumentOrigin,"local PIVOT aggregate input is missing",DiagnosticCode::InvalidQueryShape);
    }
    using AxisKey=std::vector<std::int64_t>;
    std::map<AxisKey,std::vector<std::size_t>> buckets;
    AxisKey axisKey(shape->axisOrdinals.size());
    charge(node.origin,axisKey.size()*sizeof(std::int64_t));
    const auto lookupKey=[&](auto valueAt) {
      bool nonNull=true;
      for (std::size_t i=0;i<axisKey.size();++i) {
        step(node.origin);
        const auto value=valueAt(i);
        if (value) axisKey[i]=*value;
        else nonNull=false;
      }
      return nonNull;
    };
    for (std::size_t i=0;i<shape->buckets.size();++i) {
      const auto &bucket=shape->buckets[i];
      if (bucket.values.size()!=axisKey.size())
        fail(bucket.origin,"invalid local PIVOT bucket width",DiagnosticCode::InvalidQueryShape);
      if (!lookupKey([&](std::size_t j)->std::optional<std::int64_t> {
            const ExpressionRef ref{query.id,bucket.values[j]};
            if (modelExpression(plan,ref).kind==ExprKind::Null) return {};
            const auto value=exactIntegerLiteral(plan,ref);
            if (!value) fail(bucket.origin,"local PIVOT bucket requires an exact signed integer or NULL");
            return value;
          })) continue;
      auto [found,inserted]=buckets.try_emplace(axisKey);
      charge(bucket.origin,sizeof(std::size_t)+(inserted?sizeof(*found)+4*sizeof(void *)+
            axisKey.size()*sizeof(std::int64_t):0));
      found->second.push_back(i);
    }
    const auto source=execute(node.inputs.front(),depth+1);
    const auto width=shape->buckets.size();
    const auto &ordinals=shape->groupingOrdinals;
    std::map<LocalRow,std::vector<Accumulator>> groups;
    const auto groupValues=[&](const LocalRow &key)->std::vector<Accumulator>& {
      const auto search=groups.lower_bound(key);
      if (search!=groups.end() && !(key<search->first)) return search->second;
      if (groups.size()>=limits.maxRows)
        fail(node.origin,"local PIVOT groups exceed the row budget",DiagnosticCode::ResourceLimit);
      charge(node.origin,sizeof(LocalRow)+key.size()*sizeof(LocalValue)+width*sizeof(Accumulator)+4*sizeof(void *));
      return groups.try_emplace(search,key,width)->second;
    };
    if (ordinals.empty()) groupValues({});
    charge(node.origin,ordinals.size()*sizeof(LocalValue));
    LocalRow key(ordinals.size());
    for (const auto &row:source) {
      step(node.origin,ordinals.size()+1);
      for (std::size_t i=0;i<ordinals.size();++i) {
        key[i]=row.at(ordinals[i]);
        requireNonText(key[i],node.origin);
      }
      auto &values=groupValues(key);
      if (!lookupKey([&](std::size_t i)->std::optional<std::int64_t> {
            const auto &axisValue=row.at(shape->axisOrdinals[i]);
            if (isNull(axisValue)) return {};
            const auto number=std::get_if<std::int64_t>(&axisValue);
            if (!number)
              fail(node.origin,"local PIVOT received a noninteger axis",DiagnosticCode::InvalidQueryShape);
            return *number;
          })) continue;
      const auto found=buckets.find(axisKey);
      if (found==buckets.end()) continue;
      for (auto bucket:found->second) accumulate(term,values[bucket],row);
    }
    rowBudget(node.origin,node.outputs.size(),groups.size());
    Rows result;
    result.reserve(groups.size());
    for (const auto &[key,values]:groups) {
      step(node.origin,ordinals.size()+width+1);
      result.push_back(key);
      auto &row=result.back();
      row.reserve(node.outputs.size());
      for (const auto &value:values) row.push_back(finishAggregate(term,value));
    }
    return result;
  }
  Rows execute(RelId id, std::size_t depth = 0, const std::vector<RelationPredicate> &predicates = {},
               const PageAtScan *page = nullptr, const RelNode *whereAboveJoin = nullptr,
               const RelationKeySet *keySet = nullptr) {
    const auto &n = plan.nodes.at(id);
    step(n.origin);
    if (depth >= limits.maxDepth)
      fail(n.origin, "local relational nesting limit exceeded", DiagnosticCode::ResourceLimit);
    struct Restore { std::size_t &current, previous; ~Restore() { current = previous; } }
        restore{relationalDepth, relationalDepth};
    relationalDepth = depth;
    const auto &q = modelQuery(plan, n.query);
    if (q.kind != StatementKind::Select)
      fail(q.origin, "local interpretation requires a SELECT or VALUES query");
    if (n.kind == RelKind::Values) return values(n);
    if (n.kind == RelKind::Unnest) return unnest(n,depth);
    if (n.kind == RelKind::Unpivot) return unpivot(n,q,depth);
    if (n.kind == RelKind::Pivot) return pivot(n,q,depth);
    if (n.kind == RelKind::Series) return integerSeries(n,depth);
    if (n.kind == RelKind::Sample) {
      if (!n.sampling || n.inputs.size()!=1 ||
          (n.sampling->milliPercent!=0 && n.sampling->milliPercent!=TableSampling::full))
        fail(n.origin,"nontrivial TABLESAMPLE requires the native PostgreSQL scan");
      if (n.sampling->milliPercent==0)
        fail(n.origin,"local sampling requires provider authorization even for an empty result");
      return execute(n.inputs.front(),depth+1);
    }
    if (n.kind == RelKind::Scan)
      return scan(n, q, predicates, page && page->scan == id ? &page->page : nullptr, keySet);
    if (n.kind == RelKind::RepeatUnion) return repeatUnion(n, depth);
    if (n.kind == RelKind::WorkingTableScan) return workingRows(n);
    if (n.kind == RelKind::Correlate) return correlate(n,depth);
    if (n.kind==RelKind::SemiJoin || n.kind==RelKind::AntiJoin) return membership(n,depth);
    if (n.kind == RelKind::SetOperation) {
      if (n.inputs.size() < 2 || n.inputs.size() > 64)
        fail(n.origin, "invalid local set input arity", DiagnosticCode::InvalidQueryShape);
      if (n.setKind == SetKind::Union) return unionRows(n, depth);
      auto rows=execute(n.inputs.front(),depth+1);
      for (std::size_t i=1; i<n.inputs.size(); ++i)
        rows=set(n,std::move(rows),execute(n.inputs[i],depth+1));
      return rows;
    }
    if (n.kind == RelKind::Join) {
      checkScalarDependencies(n, 2);
      const auto push = joinScanPredicates(n, whereAboveJoin);
      auto left = execute(n.inputs.at(0), depth + 1, push[0]);
      const auto keys = lookupJoinKeys(n, left);
      auto right = execute(n.inputs.at(1), depth + 1, push[1], nullptr, nullptr, keys ? &*keys : nullptr);
      return join(n, plan.nodes.at(n.inputs[0]), plan.nodes.at(n.inputs[1]), left, right);
    }
    switch (n.kind) {
    case RelKind::Filter: case RelKind::Project: case RelKind::Calc: case RelKind::Sort:
    case RelKind::Limit: case RelKind::Rename: case RelKind::Coerce:
      break;
    case RelKind::Aggregate:
      if (!n.distinct || !q.distinct) fail(n.origin, "local interpretation currently supports DISTINCT without aggregates");
      break;
    case RelKind::Subquery:
      if (!n.table || n.scalar || (n.correlated && capturedRows.empty()))
        fail(n.origin, "local subquery execution requires a derived input in its declared scope");
      break;
    default:
      fail(n.origin, "relational operator requires unsupported runtime semantics or a database");
    }
    checkScalarDependencies(n, 1);
    const auto &input = plan.nodes.at(n.inputs.front());
    if (n.kind == RelKind::Project) {
      if (auto extremum = readExtremum(n)) return std::move(*extremum);
      if (input.kind == RelKind::Window)
        return windowProjection(n, input, depth + 1);
      const auto [group, having] = groupedInput(input);
      if (group) {
        auto data = aggregateRows(n, *group, having, depth + 1 + bool(having));
        return projection(n, data.schema, std::move(data.rows), &data.bindings);
      }
    }
    if (n.kind == RelKind::Subquery && n.materialized) {
      if (input.outputs.size() != n.outputs.size())
        fail(n.origin, "materialized CTE column shape is incomplete");
      std::optional<SubqueryRows> uncached;
      const auto &retained = subqueryRows(n.inputs.front(), n.origin, uncached).rows;
      rowBudget(n.origin, n.outputs.size(), retained.size());
      return retained;
    }
    const bool filterScan=relations && (n.kind==RelKind::Filter || n.kind==RelKind::Calc) && input.kind==RelKind::Scan;
    const auto restrictions = filterScan ? scanRestrictions(n, input).value_or(ScanRestrictions{}) : ScanRestrictions{};
    const auto counts = n.kind==RelKind::Limit ? pagination(n) : Pagination{};
    const auto candidate = page || n.kind!=RelKind::Limit ? std::nullopt : orderedPage(n,counts);
    const auto *where = n.kind == RelKind::Filter && n.inputs.size() == 1 && input.kind == RelKind::Join ? &n : nullptr;
    auto rows=execute(n.inputs.front(),depth+1,restrictions.predicates,candidate?&*candidate:page,where,
                      restrictions.keys?&*restrictions.keys:nullptr);
    switch (n.kind) {
    case RelKind::Calc: return scalarCalc(n,input,std::move(rows));
    case RelKind::Project: return projection(n, input, std::move(rows));
    case RelKind::Filter: return filter(n, input, std::move(rows));
    case RelKind::Sort: return sort(n, input, std::move(rows));
    case RelKind::Limit: return limit(n, input, std::move(rows), counts);
    case RelKind::Aggregate: return deduplicate(n, std::move(rows));
    case RelKind::Rename: case RelKind::Coerce: case RelKind::Subquery:
      if (input.outputs.size() != n.outputs.size())
        fail(n.origin, "derived relation column shape is incomplete");
      return rows;
    default:
      fail(n.origin, "invalid local relational operator", DiagnosticCode::InvalidQueryShape);
    }
  }

public:
  Interpreter(const LogicalPlan &plan, const InterpreterLimits &limits, Clock::time_point deadline,
              RelationProvider *relations = nullptr, const LocalRow *parameters = nullptr)
      : plan(plan), limits(limits), deadline(deadline), sqlite(plan.program.sourceDialect == Dialect::SQLite),
        relations(relations), parameters(parameters) {}

  int constantTruth(ExpressionRef ref) {
    Scalar scalar;
    compileScalar(scalar, ref, nullptr, false);
    const auto value = truth(eval(scalar, {}), scalar.back().origin);
    return value == Truth::Null ? -1 : value == Truth::True ? 1 : 0;
  }
  std::vector<std::size_t> literalFilter(const RelNode &filter, const RelNode &input,
                                         const RelNode &literal) {
    const auto predicates = compilePredicates(filter, input);
    const auto rows = values(literal);
    std::vector<std::size_t> kept;
    rowBudget(filter.origin, 1, rows.size());
    kept.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
      step(filter.origin);
      if (matches(predicates, rows[i])) kept.push_back(i);
    }
    return kept;
  }

  LocalExchangeResult exchangePlan(const LocalExchangeSpec &spec) {
    const auto &root=plan.nodes.at(plan.root);
    const auto &query=modelQuery(plan,root.query);
    if (spec.partitions==0 || spec.partitions>128 || spec.keys.empty() || spec.keys.size()>32 ||
        query.setOperation || !query.groupingSets.empty() || query.projections.size()!=root.outputs.size())
      fail(root.origin,"local hash exchange needs 1–128 partitions and explicit output positions",
           DiagnosticCode::InvalidQueryShape);
    const auto literal=[&](const ModelExpression &expression) {
      if (expression.kind==ExprKind::Number) return exactIntegerText(expression.value).has_value();
      if (expression.kind!=ExprKind::Cast || expression.operands.size()!=1) return false;
      const auto &operand=modelExpression(plan,{query.id,expression.operands.front()});
      return operand.kind==ExprKind::Null || (operand.kind==ExprKind::Number &&
          exactIntegerText(operand.value).has_value());
    };
    std::set<std::size_t> positions;
    LocalExchangeResult result;
    for (auto position:spec.keys) {
      step(root.origin);
      if (position>=query.projections.size() || !positions.insert(position).second)
        fail(root.origin,"local exchange keys must be distinct valid output ordinals",
             DiagnosticCode::InvalidQueryShape);
      const ExpressionRef projected{query.id,query.projections[position].expression};
      const auto &value=modelExpression(plan,projected);
      if (!signedIntegerBits(plan.program.types.get(value.sqlType)))
        fail(value.origin,"local hash exchange requires verified signed integer keys");
      bool constant=literal(value);
      if (const auto column=boundScalarColumn(plan,projected);column && query.where) {
        std::vector<ExprId> pending{query.where};
        while (!pending.empty() && !constant) {
          step(value.origin);
          const auto &predicate=modelExpression(plan,{query.id,pending.back()});
          pending.pop_back();
          if (predicate.kind==ExprKind::Binary && predicate.value=="AND" && predicate.operands.size()==2) {
            pending.insert(pending.end(),predicate.operands.begin(),predicate.operands.end());
            continue;
          }
          const auto sameBinding=[&](ExprId candidate) {
            const auto field=boundScalarColumn(plan,{query.id,candidate});
            return field && field->query==column->query && field->table==column->table &&
                field->name==column->name;
          };
          if (predicate.kind==ExprKind::IsNull && predicate.value=="IS NULL" && predicate.operands.size()==1)
            constant=sameBinding(predicate.operands.front());
          if (predicate.kind==ExprKind::Binary && predicate.value=="=" && predicate.operands.size()==2) {
            for (std::size_t i=0;i<2;++i) {
              const ExpressionRef bound{query.id,predicate.operands[i]};
              const ExpressionRef other{query.id,predicate.operands[1-i]};
              constant|=sameBinding(bound.expression) && sameIntegerType(plan,bound,other) &&
                  literal(modelExpression(plan,other));
            }
          }
        }
      }
      if (constant) result.removedConstantKeys.push_back(position);
      else result.keys.push_back(position);
    }
    const auto partitions=result.keys.empty()?std::size_t{1}:spec.partitions;
    charge(root.origin,(partitions+1)*sizeof(std::size_t));
    result.offsets.resize(partitions+1);
    return result;
  }
  void exchangeRows(Interpretation &out,LocalExchangeResult distribution,SourceRange origin) {
    const auto partitions=distribution.offsets.size()-1;
    if (partitions==1) {
      distribution.offsets.back()=out.rows.size();
      out.exchange=std::move(distribution);
      return;
    }
    charge(origin,out.rows.size()*(sizeof(std::size_t)+sizeof(LocalRow))+
                  partitions*sizeof(std::size_t));
    std::vector<std::size_t> destinations;
    destinations.reserve(out.rows.size());
    for (const auto &row:out.rows) {
      step(origin,distribution.keys.size()+1);
      std::uint64_t hash=0xcbf29ce484222325ULL;
      for (auto position:distribution.keys) {
        const auto &value=row.at(position);
        if (const auto integer=std::get_if<std::int64_t>(&value))
          hash^=(static_cast<std::uint64_t>(*integer)*0x9e3779b97f4a7c15ULL);
        else if (isNull(value))
          hash^=0x517cc1b727220a95ULL;
        else
          fail(origin,"exchange key did not produce an integer or NULL",DiagnosticCode::InvalidQueryShape);
        hash*=0x100000001b3ULL;
        hash^=hash>>32;
      }
      const auto bucket=static_cast<std::size_t>(hash%partitions);
      destinations.push_back(bucket);
      ++distribution.offsets[bucket+1];
    }
    for (std::size_t i=1;i<distribution.offsets.size();++i)
      distribution.offsets[i]+=distribution.offsets[i-1];
    auto cursor=distribution.offsets;
    Rows arranged(out.rows.size());
    for (std::size_t i=0;i<out.rows.size();++i) {
      step(origin);
      arranged[cursor[destinations[i]]++]=std::move(out.rows[i]);
    }
    out.rows=std::move(arranged);
    out.exchange=std::move(distribution);
  }
  Interpretation run(const LocalExchangeSpec *exchange=nullptr) {
    if (relations) collectScanColumns();
    for (const auto &node : plan.nodes) {
      step(node.origin);
      if (node.kind != RelKind::Subquery || !node.scalar || node.inputs.size() != 1) continue;
      const auto child = node.inputs.front();
      const auto key = std::make_pair(node.query, plan.nodes.at(child).query);
      if (subqueries.count(key)) continue;
      charge(node.origin, sizeof(key) + sizeof(SubqueryPlan) + 3 * sizeof(void *));
      subqueries.emplace(key, SubqueryPlan{child, node.correlated});
    }
    Interpretation out;
    auto distribution=exchange?std::optional<LocalExchangeResult>(exchangePlan(*exchange)):std::nullopt;
    out.rows = execute(plan.root);
    const auto origin = plan.nodes.at(plan.root).origin;
    for (const auto &row : out.rows) {
      step(origin, row.size() + 1);
      for (const auto &value : row) {
        if (auto text = std::get_if<LocalText>(&value)) {
          // An output can repeat a shared cell many times. Budget the JSON
          // spelling separately from the backing text charged at its scan.
          const auto size = text->utf8().size();
          if (size > limits.maxBytes / 6)
            fail(origin, "local text output exceeded its byte budget", DiagnosticCode::ResourceLimit);
          charge(origin, size * 6);
        } else if (std::holds_alternative<LocalNumeric>(value)) {
          charge(origin,LocalNumeric::maxFormattedBytes+2);
        }
      }
    }
    for (const auto &column : plan.nodes.at(plan.root).outputs) {
      charge(plan.nodes.at(plan.root).origin, sizeof(column.name) + column.name.size());
      out.columns.push_back(column.name);
    }
    if (distribution) exchangeRows(out,std::move(*distribution),origin);
    return out;
  }
};
} // namespace

std::optional<int> evaluateConstantTruth(PlannerContext &context, ExpressionRef ref) {
  const auto &plan = context.result.logical;
  if (plan.program.sourceDialect != Dialect::PostgreSQL && plan.program.sourceDialect != Dialect::SQLite)
    return std::nullopt;
  InterpreterLimits limits;
  limits.maxSteps = 512;
  limits.maxBytes = std::min<std::size_t>(32 * 1024, context.options.maxBytes - context.result.accountedBytes);
  limits.maxDepth = std::min<std::size_t>(32, context.options.maxDepth);
  limits.shouldStop = [&] { context.stop(); return false; };
  try {
    return Interpreter(plan, limits, context.deadline).constantTruth(ref);
  } catch (const RuntimeFailure &) {
    return std::nullopt;
  }
}

std::optional<std::vector<std::size_t>> evaluateLiteralFilter(
    PlannerContext &context, const RelNode &filter, const RelNode &input, const RelNode &values) {
  InterpreterLimits limits;
  limits.maxRows = 256;
  limits.maxSteps = 16384;
  limits.maxBytes = std::min<std::size_t>(128 * 1024, context.options.maxBytes - context.result.accountedBytes);
  limits.maxDepth = std::min<std::size_t>(32, context.options.maxDepth);
  limits.shouldStop = [&] { context.stop(); return false; };
  try {
    return Interpreter(context.result.logical, limits, context.deadline).literalFilter(filter, input, values);
  } catch (const RuntimeFailure &) {
    return std::nullopt;
  }
}

namespace {
void normalizeLimits(InterpreterLimits &limits) {
  limits.maxRows=std::clamp<std::size_t>(limits.maxRows,1,100000);
  limits.maxSteps=std::clamp<std::size_t>(limits.maxSteps,1,10000000);
  limits.maxBytes=std::clamp<std::size_t>(limits.maxBytes,65536,128*1024*1024);
  limits.maxDepth=std::clamp<std::size_t>(limits.maxDepth,1,128);
  limits.maxTimeMs=std::clamp<std::size_t>(limits.maxTimeMs,1,10000);
  limits.maxIterations=std::clamp<std::size_t>(limits.maxIterations,1,4096);
}

template <class Run>
auto guardedLocal(Run run) {
  decltype(run()) result;
  try {
    return run();
  } catch (const RuntimeFailure &error) {
    result.diagnostics.push_back(error.diagnostic);
  } catch (const RelationScanError &error) {
    result.diagnostics.push_back({error.code, {}, error.message});
  } catch (const PlannerFailure &error) {
    result.diagnostics.push_back(error.diagnostic);
  } catch (const PlannerBudget &) {
    result.diagnostics.push_back({DiagnosticCode::ResourceLimit, {}, "local interpreter lowering budget exceeded"});
  } catch (const PlannerStopped &) {
    result.diagnostics.push_back({DiagnosticCode::Stopped, {}, "local interpretation cancelled"});
  } catch (const std::bad_alloc &) {
    result.diagnostics.push_back({DiagnosticCode::ResourceLimit, {}, "local interpreter allocation failed"});
  } catch (const std::exception &error) {
    result.diagnostics.push_back({DiagnosticCode::InvalidQueryShape, {}, error.what()});
  }
  return result;
}

PlannerOptions localPlannerOptions(const InterpreterLimits &limits) {
  PlannerOptions options;
  options.maxDepth=limits.maxDepth;
  options.maxBytes=limits.maxBytes;
  options.shouldStop=limits.shouldStop;
  options.heuristic.rules={HepRule::DecorrelateExists,HepRule::RewriteInSubquery,
                           HepRule::MarkToSemiOrAntiJoin,
                           HepRule::FilterCorrelate,HepRule::FilterWindowTranspose,HepRule::CalcMerge,
                           HepRule::ProjectOverSumToSum0};
  options.heuristic.maxPasses=4;
  options.heuristic.maxMatches=512;
  return options;
}
void rewriteLocalInputs(PlannerContext &context) {
  auto &plan=context.result.logical;
  const auto &root=plan.nodes.at(plan.root);
  const bool scalarPipeline=root.kind==RelKind::Project && root.inputs.size()==1 &&
      plan.nodes.at(root.inputs.front()).kind==RelKind::Filter;
  if (plan.program.sourceDialect!=Dialect::PostgreSQL ||
      (!scalarPipeline && std::none_of(plan.nodes.begin(),plan.nodes.end(),[](const auto &node) {
        return node.kind==RelKind::Correlate || node.kind==RelKind::Window ||
               (node.kind==RelKind::Subquery && node.scalar);
      }))) return;
  plan.root=runHep(context);
}

std::vector<LocalParameter> collectLocalParameters(PlannerContext &context) {
  const auto &model=context.result.logical.program;
  std::map<std::size_t,LocalParameter> positional;
  for (const auto &query:model.queries) {
    context.stop();
    for (std::size_t i=1;i<query.expressions.size();++i) {
      if ((i & 255u)==0) context.stop();
      const auto &expression=query.expressions[i];
      if (expression.kind!=ExprKind::Parameter) continue;
      const auto position=parameterPosition(expression.value);
      if (!position || expression.localSymbol)
        context.fail(expression.origin,"local prepare requires positional parameters from $1 through $1024");
      const auto &type=model.types.get(expression.sqlType);
      if (!plainBoolean(type) && !signedIntegerBits(type) && !numericType(type))
        context.fail(expression.origin,"local parameter needs a resolved signed integer, numeric or Boolean type");
      const auto existing=positional.find(*position);
      if (existing==positional.end()) {
        context.charge(2*sizeof(LocalParameter)+sizeof(std::size_t)+3*sizeof(void *));
        positional.emplace(*position,LocalParameter{*position,type,expression.origin});
      } else if (existing->second.type!=type) {
        context.fail(expression.origin,"local parameter has conflicting types in one query");
      }
    }
  }
  std::vector<LocalParameter> parameters;
  parameters.reserve(positional.size());
  for (auto &[position,parameter]:positional) {
    if (position!=parameters.size()+1)
      context.fail(parameter.firstUse,"local prepare needs contiguous parameter positions starting at $1");
    parameters.push_back(std::move(parameter));
  }
  return parameters;
}

Interpretation runInterpretation(const ModelProgram &model, InterpreterLimits limits,
                                 RelationProvider *relations,const LocalExchangeSpec *exchange=nullptr) {
  return guardedLocal([&] {
    if (model.sourceDialect!=Dialect::PostgreSQL && model.sourceDialect!=Dialect::SQLite)
      throw RuntimeFailure{{DiagnosticCode::UnsupportedSyntax, {},
                            "local interpreter currently supports PostgreSQL and SQLite semantics"}};
    normalizeLimits(limits);
    const auto deadline=Clock::now()+std::chrono::milliseconds(limits.maxTimeMs);
    const auto options=localPlannerOptions(limits);
    OptimizationResult lowering;
    PlannerContext context{lowering,nullptr,options,deadline};
    lowerLogical(context,model);
    if (relations) rewriteLocalInputs(context);
    limits.maxBytes-=lowering.accountedBytes;
    return Interpreter(lowering.logical,limits,deadline,relations).run(exchange);
  });
}

Interpretation runPrepared(const PreparedLocalQuery &query, const LocalRow &bindings,
                           InterpreterLimits limits, RelationProvider *relations) {
  return guardedLocal([&] {
    normalizeLimits(limits);
    const auto deadline=Clock::now()+std::chrono::milliseconds(limits.maxTimeMs);
    if (limits.shouldStop && limits.shouldStop())
      throw RuntimeFailure{{DiagnosticCode::Stopped,{},"local interpretation cancelled"}};
    if (bindings.size()!=query.parameters.size()) {
      const auto origin=bindings.size()<query.parameters.size()
          ?query.parameters[bindings.size()].firstUse:SourceRange{};
      throw RuntimeFailure{{DiagnosticCode::InvalidQueryShape,origin,
                            "local query requires one binding per prepared parameter"}};
    }
    auto inputBytes=bindings.size()*sizeof(LocalValue);
    for (std::size_t i=0;i<bindings.size();++i) {
      const auto &value=bindings[i];
      if (isNull(value)) continue;
      const auto &parameter=query.parameters[i];
      const auto bits=signedIntegerBits(parameter.type);
      const auto *number=std::get_if<std::int64_t>(&value);
      const bool decimal=std::holds_alternative<LocalNumeric>(value);
      const bool compatible=bits ? number && fitsSignedInteger(*number,*bits) :
          numericType(parameter.type) ? number || decimal : std::holds_alternative<bool>(value);
      if (!compatible)
        throw RuntimeFailure{{DiagnosticCode::InvalidQueryShape,parameter.firstUse,
                              "binding for $"+std::to_string(parameter.position)+" does not fit its prepared type"}};
      if (decimal) inputBytes+=LocalNumeric::storageBytes;
    }
    if (query.accountedBytes>limits.maxBytes || inputBytes>limits.maxBytes-query.accountedBytes)
      throw RuntimeFailure{{DiagnosticCode::ResourceLimit,{},"prepared plan and bindings exceed the allocation budget"}};
    limits.maxBytes-=query.accountedBytes+inputBytes;
    return Interpreter(query.plan,limits,deadline,relations,&bindings).run();
  });
}
} // namespace

LocalPreparation prepareLocalQuery(const ModelProgram &model, InterpreterLimits limits) {
  return guardedLocal([&] {
    if (model.sourceDialect!=Dialect::PostgreSQL)
      throw RuntimeFailure{{DiagnosticCode::UnsupportedSyntax,{},"local prepare currently requires PostgreSQL"}};
    normalizeLimits(limits);
    const auto deadline=Clock::now()+std::chrono::milliseconds(limits.maxTimeMs);
    const auto options=localPlannerOptions(limits);
    OptimizationResult lowering;
    PlannerContext context{lowering,nullptr,options,deadline};
    lowerLogical(context,model);
    for (const auto &query:lowering.logical.program.queries) {
      context.stop();
      if (query.kind!=StatementKind::Select)
        context.fail(query.origin,"local prepare requires a read-only SELECT");
    }
    auto parameters=collectLocalParameters(context);
    rewriteLocalInputs(context);
    context.charge(sizeof(PreparedLocalQuery)+2*sizeof(void *));
    return LocalPreparation{std::make_shared<const PreparedLocalQuery>(
        std::move(lowering.logical),std::move(parameters),lowering.accountedBytes),{}};
  });
}

const std::vector<LocalParameter> &localParameters(const PreparedLocalQuery &query) {
  return query.parameters;
}

Interpretation executeLocalQuery(const PreparedLocalQuery &query, const LocalRow &bindings,
                                 InterpreterLimits limits) {
  return runPrepared(query,bindings,std::move(limits),nullptr);
}

Interpretation executeLocalQueryWithRelations(const PreparedLocalQuery &query, const LocalRow &bindings,
                                              RelationProvider &relations, InterpreterLimits limits) {
  return runPrepared(query,bindings,std::move(limits),&relations);
}

Interpretation interpret(const ModelProgram &model, InterpreterLimits limits) {
  return runInterpretation(model, std::move(limits), nullptr);
}
Interpretation interpretWithRelations(const ModelProgram &model, RelationProvider &relations,
                                       InterpreterLimits limits) {
  return runInterpretation(model, std::move(limits), &relations);
}
Interpretation interpretPartitioned(const ModelProgram &model,const LocalExchangeSpec &exchange,
                                    InterpreterLimits limits) {
  return runInterpretation(model,std::move(limits),nullptr,&exchange);
}
Interpretation interpretPartitionedWithRelations(const ModelProgram &model,const LocalExchangeSpec &exchange,
                                    RelationProvider &relations,InterpreterLimits limits) {
  return runInterpretation(model,std::move(limits),&relations,&exchange);
}
std::optional<LocalValue> parseLocalBinding(std::string_view text) {
  if (text=="null") return LocalValue{};
  if (text=="true") return LocalValue{true};
  if (text=="false") return LocalValue{false};
  const bool plus=text.size()>1 && text.front()=='+' && text[1]>='0' && text[1]<='9';
  const auto number=exactIntegerText(plus?text.substr(1):text);
  if (number) return LocalValue{*number};
  if (auto decimal=LocalNumeric::parse(text)) return LocalValue{std::move(*decimal)};
  return {};
}
} // namespace navidog::sql::optimizer
