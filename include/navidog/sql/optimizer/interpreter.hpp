#pragma once
#include <array>
#include "navidog/sql/compiler/compiler.hpp"
#include <chrono>
#include <memory>
#include <string_view>
#include <variant>

namespace navidog::sql::optimizer {
// Value copies share the backing bytes. SQL comparison paths must establish
// collation separately from these C++ value comparisons.
class LocalText {
  std::shared_ptr<const std::string> utf8_;

public:
  explicit LocalText(std::string utf8) : utf8_(std::make_shared<const std::string>(std::move(utf8))) {}
  std::string_view utf8() const { return utf8_ ? std::string_view(*utf8_) : std::string_view{}; }
  bool operator==(const LocalText &other) const { return utf8() == other.utf8(); }
  bool operator!=(const LocalText &other) const { return !(*this == other); }
  bool operator<(const LocalText &other) const { return utf8() < other.utf8(); }
  bool operator>(const LocalText &other) const { return other < *this; }
  bool operator<=(const LocalText &other) const { return !(other < *this); }
  bool operator>=(const LocalText &other) const { return !(*this < other); }
};
// An exact decimal with bounded integral and fractional parts. Trailing
// fractional zeros preserve the SQL display scale but do not affect equality.
class LocalNumeric {
  static constexpr int fractionalLimbs=3, digitsPerLimb=9;
  struct Storage {
    std::array<std::uint32_t,6> units{};
    std::uint8_t scale=0;
    bool negative=false;
    int compareMagnitude(const Storage &) const;
    int digit(int position) const;
    std::pair<int,int> leadingDecimalGroup() const;
    bool roundAt(int discarded, bool increase);
    std::optional<Storage> divide(const Storage &,int scale,bool keepRemainder) const;
    bool zero() const;
  };
  std::shared_ptr<Storage> data_;
  const Storage &value() const;
  void replace(Storage);
  bool addStorage(const Storage &);
  static Storage integer(std::int64_t);
  explicit LocalNumeric(Storage);

public:
  static constexpr int maxScale=fractionalLimbs*digitsPerLimb;
  static constexpr std::size_t maxFormattedBytes=56;
  static constexpr std::size_t storageBytes=sizeof(Storage)+2*sizeof(void *);
  explicit LocalNumeric(std::int64_t value = 0);
  static std::optional<LocalNumeric> parse(std::string_view);
  bool add(std::int64_t value);
  bool add(const LocalNumeric &);
  bool subtract(LocalNumeric);
  std::optional<LocalNumeric> withTypeModifier(int precision,int scale) const;
  std::optional<LocalNumeric> multipliedBy(const LocalNumeric &) const;
  std::optional<LocalNumeric> dividedBy(const LocalNumeric &) const;
  std::optional<LocalNumeric> remainder(const LocalNumeric &) const;
  std::optional<LocalNumeric> dividedByCount(std::uint32_t) const;
  std::size_t scale() const { return value().scale; }
  bool setDisplayScale(std::size_t);
  std::string decimal() const;
  std::size_t hash() const;
  int compare(std::int64_t) const;
  bool operator==(const LocalNumeric &) const;
  bool operator!=(const LocalNumeric &other) const { return !(*this==other); }
  bool operator<(const LocalNumeric &) const;
  bool operator>(const LocalNumeric &other) const { return other<*this; }
  bool operator<=(const LocalNumeric &other) const { return !(other<*this); }
  bool operator>=(const LocalNumeric &other) const { return !(*this<other); }
};
using LocalValue = std::variant<std::monostate,std::int64_t,bool,LocalText,LocalNumeric>;
using LocalRow = std::vector<LocalValue>;
// CLI spelling for an integer, exact decimal, Boolean or NULL binding.
std::optional<LocalValue> parseLocalBinding(std::string_view);

struct InterpreterLimits {
  std::size_t maxRows = 10000, maxSteps = 1000000, maxBytes = 16 * 1024 * 1024;
  std::size_t maxDepth = 64, maxTimeMs = 2000, maxIterations = 1024;
  std::function<bool()> shouldStop;
};
struct LocalExchangeSpec {
  std::size_t partitions = 4;
  std::vector<std::size_t> keys; // Zero-based output ordinals.
};
struct LocalExchangeResult {
  std::vector<std::size_t> keys, removedConstantKeys;
  // Consecutive offsets delimit each partition in Interpretation::rows.
  std::vector<std::size_t> offsets;
};
struct Interpretation {
  std::vector<std::string> columns;
  std::vector<LocalRow> rows;
  std::vector<compiler::Diagnostic> diagnostics;
  std::optional<LocalExchangeResult> exchange = {};
  bool ok() const { return diagnostics.empty(); }
};

enum class RelationPredicateKind { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, IsNull, IsNotNull };
struct RelationPredicate {
  std::size_t column;
  RelationPredicateKind kind;
  std::int64_t constant = 0;
  bool constantIsNull = false;
  std::optional<LocalNumeric> numericConstant = std::nullopt;
};
struct RelationOrderKey {
  std::size_t column;
  bool descending, nullsFirst;
};
struct RelationPage {
  std::size_t prefixRows;
  std::vector<RelationOrderKey> order;
};
struct RelationKeySet {
  static constexpr std::size_t maxWidth=4, maxKeys=64;
  using Key=std::array<std::int64_t,maxWidth>;
  struct Column { std::size_t ordinal; int width; };
  std::vector<Column> columns;
  std::vector<Key> keys;
};
struct RelationScanRequest {
  const compiler::TableDefinition &table;
  compiler::TableInheritance inheritance;
  std::size_t maxRows, maxBytes;
  std::chrono::steady_clock::time_point deadline;
  std::function<bool()> shouldStop;
  // Catalog ordinals in result order. An empty list requests row cardinality
  // only. Each source row then yields an empty LocalRow.
  const std::vector<std::size_t> &columns;
  // A provider may apply exact predicates. Returning unfiltered rows also
  // works because the caller still evaluates the local Filter.
  const std::vector<RelationPredicate> &predicates;
  // With a page, the provider must apply every predicate and return the
  // sorted prefix. Only providers advertising ordered reads receive pages.
  const RelationPage *page = nullptr;
  // Optional exact keys from an already read join input or a literal
  // membership filter. Local execution still evaluates the original predicate.
  // An empty key list proves that the provider can return no matching rows.
  // A provider advertising ordered key reads must apply these keys before
  // sorting or truncating a request that also carries a page.
  const RelationKeySet *keySet = nullptr;
};
struct RelationScanError {
  compiler::DiagnosticCode code;
  std::string message;
};

class RelationCursor {
public:
  virtual ~RelationCursor() = default;
  // Cells match the requested ordinals. Text cells contain valid UTF-8.
  // nullopt ends the stream.
  virtual std::optional<LocalRow> next() = 0;
};

class RelationProvider {
public:
  virtual ~RelationProvider() = default;
  // One invocation uses a provider's operation-local snapshots. It may keep
  // transport state on the caller's worker and must honor the request budgets.
  // The catalog definition, ordinals and predicates live until the cursor closes.
  virtual bool supportsOrderedReads(const compiler::TableDefinition &) const { return false; }
  // An ordered provider can decline a predicate shape before a prefix is requested.
  virtual bool supportsOrderedFilters(const compiler::TableDefinition &,
                                      const std::vector<RelationPredicate> &) const { return true; }
  virtual bool supportsOrderedKeyReads(const compiler::TableDefinition &) const { return false; }
  virtual std::unique_ptr<RelationCursor> open(const RelationScanRequest &) = 0;
};

// Evaluate one supported, server-independent SELECT over constant relations.
// Errors, cancellation and resource exhaustion publish no partial rows.
Interpretation interpret(const compiler::ModelProgram &, InterpreterLimits = {});
// The compiled query binds complete table definitions with source aliases in
// database/schema/name. The provider resolves only the explicitly supplied sources.
Interpretation interpretWithRelations(const compiler::ModelProgram &, RelationProvider &,
                                       InterpreterLimits = {});
Interpretation interpretPartitioned(const compiler::ModelProgram &,const LocalExchangeSpec &,
                                    InterpreterLimits = {});
Interpretation interpretPartitionedWithRelations(const compiler::ModelProgram &,const LocalExchangeSpec &,
                                    RelationProvider &,InterpreterLimits = {});
// A prepared local query owns its checked logical plan. Bindings are supplied
// for each execution and are never retained in the query.
class PreparedLocalQuery;
struct LocalParameter {
  std::size_t position;
  compiler::SqlType type;
  compiler::SourceRange firstUse;
};
struct LocalPreparation {
  std::shared_ptr<const PreparedLocalQuery> query;
  std::vector<compiler::Diagnostic> diagnostics;
  bool ok() const { return query && diagnostics.empty(); }
};
// PostgreSQL positional parameters with a verified signed integer or Boolean
// type. Positions are contiguous and one-based; SQL NULL is a valid binding.
LocalPreparation prepareLocalQuery(const compiler::ModelProgram &, InterpreterLimits = {});
const std::vector<LocalParameter> &localParameters(const PreparedLocalQuery &);
Interpretation executeLocalQuery(const PreparedLocalQuery &, const LocalRow &bindings,
                                 InterpreterLimits = {});
Interpretation executeLocalQueryWithRelations(const PreparedLocalQuery &, const LocalRow &bindings,
                                              RelationProvider &, InterpreterLimits = {});
std::string dumpInterpretation(const Interpretation &);
} // namespace navidog::sql::optimizer
