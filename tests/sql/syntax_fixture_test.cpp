#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace sql = navidog::sql::compiler;

namespace {
constexpr std::pair<sql::DiagnosticCode, std::string_view> diagnosticNames[] = {
    {sql::DiagnosticCode::InvalidEncoding, "InvalidEncoding"},
    {sql::DiagnosticCode::LexicalError, "LexicalError"},
    {sql::DiagnosticCode::ExpectedToken, "ExpectedToken"},
    {sql::DiagnosticCode::UnsupportedSyntax, "UnsupportedSyntax"},
    {sql::DiagnosticCode::ResourceLimit, "ResourceLimit"},
    {sql::DiagnosticCode::DuplicateAlias, "DuplicateAlias"},
    {sql::DiagnosticCode::UnknownTable, "UnknownTable"},
    {sql::DiagnosticCode::UnknownColumn, "UnknownColumn"},
    {sql::DiagnosticCode::AmbiguousColumn, "AmbiguousColumn"},
    {sql::DiagnosticCode::InvalidQueryShape, "InvalidQueryShape"},
    {sql::DiagnosticCode::Stopped, "Stopped"},
    {sql::DiagnosticCode::UnknownRoutine, "UnknownRoutine"},
    {sql::DiagnosticCode::RoutineArgumentCount, "RoutineArgumentCount"},
    {sql::DiagnosticCode::CursorRuntime, "CursorRuntime"},
};

enum class Outcome {
  Valid, SyntaxError, SemanticError, Unsupported, RoutineValid, RoutineError, RoutineRuntime
};

struct Expectation {
  Outcome outcome;
  std::optional<sql::DiagnosticCode> diagnostic;
};

Expectation readExpectation(const std::string &outcome, std::istringstream &entry) {
  if (outcome == "routine-runtime")
    return {Outcome::RoutineRuntime, sql::DiagnosticCode::CursorRuntime};
  if (outcome == "valid" || outcome == "routine-valid" || outcome == "unsupported") {
    if (outcome == "unsupported")
      return {Outcome::Unsupported, sql::DiagnosticCode::UnsupportedSyntax};
    return {outcome == "valid" ? Outcome::Valid : Outcome::RoutineValid, {}};
  }
  if (outcome != "syntax-error" && outcome != "semantic-error" && outcome != "routine-error")
    throw std::runtime_error("unknown outcome: " + outcome);
  const auto failure = outcome == "syntax-error" ? Outcome::SyntaxError :
                       outcome == "semantic-error" ? Outcome::SemanticError : Outcome::RoutineError;
  const auto optionStart = entry.tellg();
  std::string diagnostic;
  if ((entry >> diagnostic) && (diagnostic.compare(0, 2, "<<") == 0 ||
                               diagnostic.compare(0, 8, "catalog=") == 0)) {
    entry.seekg(optionStart);
    return {failure, {}};
  }
  for (const auto &[code, name] : diagnosticNames) {
    if (diagnostic != name)
      continue;
    if (outcome == "syntax-error" && code == sql::DiagnosticCode::UnsupportedSyntax)
      throw std::runtime_error("use 'unsupported' for a known grammar gap");
    return {failure, code};
  }
  throw std::runtime_error("expected a diagnostic code after " + outcome +
                           (diagnostic.empty() ? "" : ": " + diagnostic));
}

bool matches(const Expectation &expected, bool ok, bool syntaxValid,
             const std::vector<sql::Diagnostic> &diagnostics) {
  switch (expected.outcome) {
  case Outcome::Valid:
  case Outcome::RoutineValid:
    return ok;
  case Outcome::SyntaxError:
  case Outcome::Unsupported:
    if (syntaxValid) return false;
    break;
  case Outcome::SemanticError:
    if (!syntaxValid) return false;
    break;
  case Outcome::RoutineError:
  case Outcome::RoutineRuntime:
    break;
  }
  return !ok && std::any_of(diagnostics.begin(), diagnostics.end(),
      [&](const auto &entry) {
        if (expected.diagnostic) return entry.code == *expected.diagnostic;
        return entry.code != sql::DiagnosticCode::UnsupportedSyntax &&
               entry.code != sql::DiagnosticCode::ResourceLimit &&
               entry.code != sql::DiagnosticCode::Stopped;
      });
}

std::string_view diagnosticName(sql::DiagnosticCode code) {
  for (const auto &[value, label] : diagnosticNames)
    if (value == code) return label;
  return "unknown diagnostic";
}

void printDiagnostics(std::ostream &output, const sql::SourceBuffer &source,
                      const std::vector<sql::Diagnostic> &diagnostics, std::size_t firstLine) {
  for (const auto &diagnostic : diagnostics) {
    const auto position = source.position(diagnostic.range.begin);
    output << '\n' << "  " << firstLine + position.line - 1 << ':' << position.byteColumn
           << ' ' << diagnosticName(diagnostic.code) << " - " << diagnostic.message;
  }
}

void requireMatch(const Expectation &expected, const sql::SourceBuffer &source,
                  bool ok, bool syntaxValid, const std::vector<sql::Diagnostic> &diagnostics,
                  const std::string &outcome, std::size_t firstLine) {
  if (matches(expected, ok, syntaxValid, diagnostics))
    return;
  std::ostringstream error;
  error << "expected " << outcome;
  if (expected.diagnostic) error << ' ' << diagnosticName(*expected.diagnostic);
  error << ", got " << (ok ? "accepted" : "rejected");
  if (expected.outcome != Outcome::RoutineValid && expected.outcome != Outcome::RoutineError)
    error << " (syntax " << (syntaxValid ? "valid" : "invalid") << ')';
  printDiagnostics(error, source, diagnostics, firstLine);
  throw std::runtime_error(error.str());
}

struct SqlBlock {
  std::string text;
  std::size_t firstLine;
};

void loadCatalog(sql::Dialect dialect, const SqlBlock &block, sql::ScriptContext &context) {
  auto schema = sql::compile(dialect, block.text);
  if (!schema->ok()) {
    std::ostringstream error;
    error << "invalid catalog fixture";
    printDiagnostics(error, schema->source, schema->diagnostics, block.firstLine);
    throw std::runtime_error(error.str());
  }
  if (schema->model.roots.empty()) throw std::runtime_error("empty catalog fixture");
  for (auto root : schema->model.roots) {
    const auto &statement = schema->model.at(root - 1);
    if (statement.kind != sql::StatementKind::Create || !statement.ddl ||
        statement.ddl->object != sql::ObjectKind::Table || statement.ddl->definitions.empty())
      throw std::runtime_error("catalog fixture expects CREATE TABLE with declared columns");
    if (const auto error = context.observe(schema->model, root))
      throw std::runtime_error("invalid catalog fixture: " + error->message);
  }
}

void checkFixture(sql::Dialect dialect, SqlBlock block, const Expectation &expected,
                  const std::string &outcome, const SqlBlock *catalogFixture) {
  sql::MemoryCatalog emptyCatalog;
  sql::ScriptContext context(dialect, &emptyCatalog);
  const sql::CatalogSnapshot *catalog = nullptr;
  if (catalogFixture) {
    loadCatalog(dialect, *catalogFixture, context);
    catalog = &context;
  }
  if (expected.outcome == Outcome::RoutineValid || expected.outcome == Outcome::RoutineError ||
      expected.outcome == Outcome::RoutineRuntime) {
    const auto routine = sql::compileRoutine(dialect, block.text, {}, catalog);
    const sql::SourceBuffer original(std::move(block.text));
    if (expected.outcome == Outcome::RoutineRuntime) {
      requireMatch({Outcome::RoutineValid, {}}, original, routine.ok(), false,
                   routine.diagnostics, "a valid routine", block.firstLine);
      if (routine.runtimeDiagnostics.empty())
        throw std::runtime_error("expected a cursor runtime diagnostic");
    } else
      requireMatch(expected, original, routine.ok(), false, routine.diagnostics, outcome, block.firstLine);
  } else {
    const auto statement = sql::compile(dialect, std::move(block.text), {}, catalog);
    if (expected.outcome==Outcome::Valid && !statement->ok()) {
      std::vector<sql::Token> significant;
      for (const auto &token:statement->tokens)
        if (token.kind!=sql::TokenKind::Comment) significant.push_back(token);
      constexpr std::string_view delimiter = "DELIMITER";
      const bool clientRoutine = !significant.empty() &&
          significant.front().kind == sql::TokenKind::Word &&
          sql::isMySqlDialect(dialect) &&
          std::equal(significant.front().value.begin(), significant.front().value.end(),
                     delimiter.begin(), delimiter.end(),
                     [](unsigned char input, char expected) { return std::toupper(input) == expected; });
      if (clientRoutine || sql::isRoutineStart(dialect,significant)) {
        const auto routine=sql::compileRoutine(dialect,std::string(statement->source.text()),{},catalog);
        requireMatch(expected,statement->source,routine.ok(),false,routine.diagnostics,outcome,block.firstLine);
        return;
      }
    }
    requireMatch(expected, statement->source, statement->ok(), statement->syntaxValid,
                 statement->diagnostics, outcome, block.firstLine);
  }
}

struct Counts { std::size_t passed = 0, failed = 0; };

void runSuite(const fs::path &path, Counts &counts) {
  const auto directory = path.parent_path();
  const auto dialect = sql::parseDialect(directory.filename().string());
  if (!dialect) throw std::runtime_error("unknown dialect directory: " + directory.string());
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open " + path.string());
  std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  std::size_t offset = source.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  std::size_t line = 0;
  const auto nextLine = [&]() {
    const auto start = offset;
    const auto newline = source.find('\n', start);
    const auto end = newline == std::string::npos ? source.size() : newline;
    offset = newline == std::string::npos ? end : end + 1;
    ++line;
    auto text = std::string_view(source).substr(start, end - start);
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
    return text;
  };

  std::map<std::string, SqlBlock> catalogs;
  const auto label = (directory.filename() / path.filename()).generic_string();
  while (offset < source.size()) {
    const std::string header(nextLine());
    std::istringstream fields(header);
    std::string directive, name, outcome, end;
    if (!(fields >> directive) || directive.front() == '#') continue;
    const auto headerLine = line;
    std::optional<Expectation> expected;
    std::string catalog;
    SqlBlock block;
    bool readingBody = false;
    try {
      if (!(fields >> name) || (directive != "case" && directive != "catalog"))
        throw std::runtime_error("expected 'case <name> <outcome> <<SQL' or 'catalog <name> <<SQL'");
      if (directive == "case") {
        if (!(fields >> outcome)) throw std::runtime_error("missing expected outcome");
        expected = readExpectation(outcome, fields);
        fields >> end;
        if (end.compare(0, 8, "catalog=") == 0) {
          catalog = end.substr(8);
          if (catalog.empty()) throw std::runtime_error("empty catalog name");
          end.clear();
          fields >> end;
        }
      } else {
        fields >> end;
      }
      std::string extra;
      if (end.size() < 3 || end.compare(0, 2, "<<") != 0 || (fields >> extra))
        throw std::runtime_error("expected a final block delimiter like <<SQL");
      end.erase(0, 2);
      readingBody = true;
      const auto begin = offset;
      block.firstLine = line + 1;
      bool closed = false;
      while (offset < source.size()) {
        const auto endOffset = offset;
        if (nextLine() != end) continue;
        block.text = source.substr(begin, endOffset - begin);
        closed = true;
        break;
      }
      if (!closed) throw std::runtime_error("missing closing delimiter " + end);
    } catch (const std::exception &error) {
      ++counts.failed;
      std::cerr << "FAIL " << label << ':' << headerLine << ": " << error.what() << '\n';
      if (!readingBody && (directive == "case" || directive == "catalog")) {
        std::istringstream recovery(header);
        std::string word, last;
        while (recovery >> word) last = word;
        if (last.size() > 2 && last.compare(0, 2, "<<") == 0) {
          last.erase(0, 2);
          while (offset < source.size())
            if (nextLine() == last) break;
          continue;
        }
      }
      return;
    }

    try {
      if (!expected) {
        if (!catalogs.emplace(name, std::move(block)).second)
          throw std::runtime_error("duplicate catalog name: " + name);
        continue;
      }
      const auto fixture = catalogs.find(catalog);
      if (!catalog.empty() && fixture == catalogs.end()) throw std::runtime_error("unknown catalog: " + catalog);
      checkFixture(*dialect, std::move(block), *expected, outcome,
                   fixture != catalogs.end() ? &fixture->second : nullptr);
      ++counts.passed;
      std::cout << "PASS " << label << ':' << headerLine << " [" << name << "]\n";
    } catch (const std::exception &error) {
      ++counts.failed;
      std::cerr << "FAIL " << label << ':' << headerLine << " [" << name << "]: " << error.what() << '\n';
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: navidog-sql-syntax-tests <fixture-directory-or-sqltest-file>\n";
    return 2;
  }
  try {
    const fs::path input = argv[1];
    Counts counts;
    if (fs::is_regular_file(input)) {
      runSuite(input, counts);
    } else {
      std::vector<fs::path> suites;
      for (const auto &dialect : fs::directory_iterator(input)) {
        if (!dialect.is_directory()) continue;
        for (const auto &entry : fs::directory_iterator(dialect.path()))
          if (entry.is_regular_file() && entry.path().extension() == ".sqltest")
            suites.push_back(entry.path());
      }
      std::sort(suites.begin(), suites.end());
      for (const auto &suite : suites) runSuite(suite, counts);
    }
    const auto total = counts.passed + counts.failed;
    if (!total) throw std::runtime_error("no SQL fixtures found in .sqltest files");
    std::cout << counts.passed << '/' << total << " SQL fixtures passed\n";
    return counts.failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
