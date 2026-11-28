#include "navidog/sql/compiler/outline.hpp"
#include "conversion_internal.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <unordered_set>

namespace navidog::sql::compiler {
namespace {
using Node = SqlOutlineNode;
using Kind = ProceduralKind;

SourceRange shifted(SourceRange range, std::size_t offset) {
  return {range.begin + offset, range.end + offset};
}

std::string joinedName(const std::vector<std::string> &parts) {
  std::string name;
  for (const auto &part : parts) {
    if (!name.empty()) name += '.';
    name += part;
  }
  return name;
}

std::string nameOf(const ASTContext &ast, const QualifiedName &parts) {
  std::vector<std::string> names;
  for (const auto &part : parts) names.push_back(ast.identifier(part.identifier));
  return joinedName(names);
}

std::string withName(std::string label, const std::string &name) {
  if (!name.empty()) label += " " + name;
  return label;
}

class OutlineBuilder {
public:
  Dialect dialect;
  const std::string &source;
  const LanguageOptions &options;
  SqlOutline result;
  std::size_t used = 0;

  bool stopped() const { return options.shouldStop && options.shouldStop(); }
  bool room(std::size_t depth = 0) {
    if (used < options.limits.nodes && depth < options.limits.nesting && !stopped())
      return true;
    result.truncated = true;
    return false;
  }

  Node *add(std::vector<Node> &nodes, std::string label, SourceRange range,
            std::size_t depth = 0, bool recovered = false) {
    if (range.begin >= range.end || range.end > source.size() || !room(depth))
      return nullptr;
    ++used;
    if (label.size() > 320) {
      std::size_t end = 320;
      while (end > 0 && (static_cast<unsigned char>(label[end]) & 0xc0) == 0x80) --end;
      label.resize(end);
      label += "...";
    }
    result.partial |= recovered;
    nodes.push_back({std::move(label), range, {}, recovered});
    return &nodes.back();
  }

  std::string excerpt(SourceRange range, std::size_t limit = 90) const {
    if (range.begin >= range.end || range.end > source.size()) return {};
    std::string label;
    bool space = false;
    for (auto i = range.begin; i < range.end; ++i) {
      const auto ch = static_cast<unsigned char>(source[i]);
      if (std::isspace(ch)) {
        space = !label.empty();
      } else {
        if (label.size() >= limit && (ch & 0xc0) != 0x80) {
          label += "...";
          break;
        }
        if (space) label += ' ';
        label += static_cast<char>(ch);
        space = false;
      }
    }
    return label;
  }

  SourceRange throughSemicolon(SourceRange range) const {
    auto end = range.end;
    while (end < source.size() && (source[end] == ' ' || source[end] == '\t' || source[end] == '\r'))
      ++end;
    if (end < source.size() && source[end] == ';') range.end = end + 1;
    return range;
  }

  void syntax(Node &, const Compilation &, const Statement &, std::size_t offset,
              std::size_t depth);
};

const char *constraintLabel(ConstraintKind kind) {
  switch (kind) {
  case ConstraintKind::PrimaryKey: return "PRIMARY KEY";
  case ConstraintKind::Unique: return "UNIQUE";
  case ConstraintKind::ForeignKey: return "FOREIGN KEY";
  case ConstraintKind::Check: return "CHECK";
  }
  return "CONSTRAINT";
}

void OutlineBuilder::syntax(Node &root, const Compilation &parsed, const Statement &sql,
                            std::size_t shift, std::size_t depth) {
  if (!room(depth)) return;
  const auto &ast = parsed.ast;
  auto child = [&](std::string label, SourceRange range) {
    return add(root.children, std::move(label), shifted(range, shift), depth);
  };
  auto query = [&](Node &parent, QueryId id) {
    if (id && id <= ast.queryCount()) syntax(parent, parsed, ast.query(id), shift, depth + 1);
  };
  if (sql.ddl) {
    const auto &ddl = *sql.ddl;
    for (const auto &field : ddl.definitions)
      if (auto *column = child(withName("COLUMN", ast.identifier(field.name.identifier)), field.origin))
        for (const auto &key : field.constraints)
          add(column->children, constraintLabel(key.kind), shifted(key.origin, shift), depth + 1);
    for (const auto &key : ddl.constraints)
      child(withName(constraintLabel(key.kind), key.name ? ast.identifier(key.name->identifier) : ""),
            key.origin);
    for (const auto &index : ddl.tableIndexes)
      child(withName("INDEX", index.name ? ast.identifier(index.name->identifier) : ""), index.origin);
    for (const auto &action : ddl.actions)
      child(excerpt(shifted(action.origin, shift)), action.origin);
    if (ddl.names.size() > 1)
      for (const auto &name : ddl.names)
        if (!name.empty()) child(nameOf(ast, name), {name.front().range.begin, name.back().range.end});
  }
  if (sql.package) {
    const auto &pkg = *sql.package;
    if (!pkg.drop && !pkg.name.empty()) {
      if (!pkg.declarations.empty())
        child("HEADER", {sql.range.begin, pkg.declarations.front().origin.begin});
      // The package AST stores each declaration including its terminator.
      for (const auto &decl : pkg.declarations)
        if (auto *entry = child(excerpt(shifted(decl.origin, shift)), decl.origin))
          for (const auto &parameter : decl.parameters)
            add(entry->children, withName("PARAM", ast.identifier(parameter.name.identifier)),
                shifted(parameter.origin, shift), depth + 1);
    }
  }
  if (sql.trigger && !sql.trigger->drop) {
    const auto &trigger = *sql.trigger;
    auto bodyBegin = trigger.body.empty() ? sql.range.end : ast.query(trigger.body.front()).range.begin;
    if (trigger.compound)
      for (const auto &token : parsed.tokens) {
        if (token.range.begin >= bodyBegin) break;
        if (token.kind == TokenKind::Word && asciiUpper(token.value) == "BEGIN") {
          bodyBegin = token.range.begin;
          break;
        }
      }
    if (auto *header = child("HEADER", {sql.range.begin, bodyBegin})) {
      if (!trigger.target.empty())
        add(header->children, withName("ON", nameOf(ast, trigger.target)),
            shifted(trigger.targetOrigin, shift), depth + 1);
      if (trigger.when)
        add(header->children, "WHEN", shifted(ast.expression(trigger.when).range, shift), depth + 1);
    }
    if (auto *body = child("BODY", {bodyBegin, sql.range.end}))
      for (auto id : trigger.body) {
        const auto &step = ast.query(id);
        const auto range = shifted(step.range, shift);
        if (auto *entry = add(body->children, excerpt(range), throughSemicolon(range), depth + 1))
          query(*entry, id);
      }
  }
  std::unordered_set<QueryId> included;
  for (const auto &cte : sql.ctes) {
    included.insert(cte.query);
    if (auto *entry = child(withName("CTE", ast.identifier(cte.name.identifier)), cte.range))
      query(*entry, cte.query);
  }
  for (const auto &table : sql.tables) {
    auto label = table.join == JoinKind::First ? "FROM" : "JOIN";
    if (auto *entry = child(withName(label, table.query
            ? table.alias ? ast.identifier(table.alias->identifier) : "(SELECT)"
            : nameOf(ast, table.name)), table.range)) {
      included.insert(table.query);
      query(*entry, table.query);
    }
  }
  auto addQuery = [&](QueryId id, std::string label) {
    if (!id || !included.insert(id).second) return;
    const auto &sub = ast.query(id);
    if (auto *entry = child(std::move(label), sub.range)) query(*entry, id);
  };
  addQuery(sql.source, "SELECT");
  if (sql.setOperation) {
    addQuery(sql.setOperation->left, "SELECT");
    addQuery(sql.setOperation->right, "SELECT");
  }

  // Follow expression references without publishing a node for every literal.
  std::vector<ExprId> expressions{sql.where, sql.having};
  for (const auto &item : sql.projections) expressions.push_back(item.expression);
  for (const auto &item : sql.returning) expressions.push_back(item.expression);
  for (const auto &table : sql.tables) expressions.push_back(table.condition);
  for (const auto &item : sql.assignments) expressions.push_back(item.value);
  std::unordered_set<ExprId> seen;
  while (!expressions.empty() && room(depth)) {
    const auto id = expressions.back();
    expressions.pop_back();
    if (!id || !seen.insert(id).second) continue;
    const auto &expression = ast.expression(id);
    addQuery(expression.query, "SUBQUERY");
    expressions.insert(expressions.end(), expression.operands.begin(), expression.operands.end());
  }
  std::stable_sort(root.children.begin(), root.children.end(),
                   [](const auto &a, const auto &b) { return a.range.begin < b.range.begin; });
}

std::unique_ptr<Compilation> syntaxOnly(Dialect dialect, std::string input,
                                        const LanguageOptions &options) {
  auto parsed = std::make_unique<Compilation>(dialect, std::move(input), options, "<outline>");
  lex(*parsed);
  if (parsed->diagnostics.empty()) createParser(*parsed)->parse();
  return parsed;
}

std::string statementName(const Compilation &parsed, const Statement &stmt,
                          const std::string &verb) {
  if (stmt.ddl)
    return withName(verb + " " + asciiUpper(objectKindName(stmt.ddl->object)),
                    nameOf(parsed.ast, stmt.ddl->name));
  if (stmt.trigger) return withName(verb + " TRIGGER", nameOf(parsed.ast, stmt.trigger->name));
  if (stmt.package) return withName(verb + " PACKAGE", nameOf(parsed.ast, stmt.package->name));
  if (stmt.target) return withName(statementKindName(stmt.kind), nameOf(parsed.ast, stmt.target->name));
  if (!stmt.routine.empty()) return withName(verb, nameOf(parsed.ast, stmt.routine));
  return stmt.kind == StatementKind::Select ? verb : statementKindName(stmt.kind);
}

SourceRange spanning(const RoutineAst &ast, const std::vector<std::uint32_t> &ids) {
  if (ids.empty()) return {};
  return {ast.nodes.at(ids.front()).origin.begin, ast.nodes.at(ids.back()).origin.end};
}

void routineNodes(OutlineBuilder &out, std::vector<Node> &nodes, const RoutineAst &ast,
                  std::uint32_t id, std::size_t offset, std::size_t depth,
                  bool routineBody = false) {
  if (!id || id >= ast.nodes.size() || !out.room(depth)) return;
  const auto &entry = ast.nodes[id];
  const auto range = shifted(entry.origin, offset);
  std::string label;
  switch (entry.kind) {
  case Kind::Block: label = "BEGIN … END"; break;
  case Kind::TsqlTryCatch: label = "TRY … CATCH"; break;
  case Kind::If: case Kind::While: case Kind::NumericFor:
  case Kind::Case: case Kind::CaseWhen: case Kind::ExceptionHandler:
    if (entry.expression.end > entry.expression.begin)
      label = out.excerpt({range.begin, offset + entry.expression.end});
    else {
      const auto beforeBody = spanning(ast, entry.children).begin;
      // CASE operands and handler conditions precede their executable children.
      label = out.excerpt({range.begin, beforeBody > entry.origin.begin
            ? offset + beforeBody : range.end});
    }
    break;
  case Kind::Loop: label = "LOOP"; break;
  case Kind::Repeat: label = "REPEAT"; break;
  default:
    label = out.excerpt(range);
    break;
  }
  if (routineBody) label.insert(0, "BODY · ");
  auto *node = out.add(nodes, std::move(label), out.throughSemicolon(range), depth);
  if (!node) return;
  auto append = [&](std::vector<Node> &parent, const std::vector<std::uint32_t> &children,
                    std::size_t childDepth) {
    for (auto child : children) routineNodes(out, parent, ast, child, offset, childDepth);
  };
  if (entry.kind == Kind::If || entry.kind == Kind::TsqlTryCatch || entry.kind == Kind::Case) {
    auto branches = [&](const std::vector<std::uint32_t> &children, const char *label) {
      if (children.empty()) return;
      if (auto *branch = out.add(node->children, label, shifted(spanning(ast, children), offset), depth + 1))
        append(branch->children, children, depth + 2);
    };
    if (entry.kind == Kind::Case) append(node->children, entry.children, depth + 1);
    else branches(entry.children, entry.kind == Kind::If ? "THEN" : "TRY");
    const bool elsif = !entry.otherwise.empty() &&
        ast.nodes[entry.otherwise.front()].kind == Kind::If &&
        (out.source.compare(offset + ast.nodes[entry.otherwise.front()].origin.begin, 5, "ELSIF") == 0 ||
         out.source.compare(offset + ast.nodes[entry.otherwise.front()].origin.begin, 6, "ELSEIF") == 0 ||
         asciiUpper(out.source.substr(offset + ast.nodes[entry.otherwise.front()].origin.begin, 5)) == "ELSIF" ||
         asciiUpper(out.source.substr(offset + ast.nodes[entry.otherwise.front()].origin.begin, 6)) == "ELSEIF");
    if (elsif) append(node->children, entry.otherwise, depth + 1);
    else branches(entry.otherwise, entry.kind == Kind::TsqlTryCatch ? "CATCH" : "ELSE");
  } else {
    append(node->children, entry.children, depth + 1);
  }
  if (auto *handlers = out.add(node->children, "EXCEPTION",
                               shifted(spanning(ast, entry.handlers), offset), depth + 1))
    append(handlers->children, entry.handlers, depth + 2);
}

bool addRoutine(OutlineBuilder &out, const RoutineAst &ast, std::size_t offset,
                const std::string &normalized, const std::vector<Token> &tokens,
                std::size_t tokenIndex) {
  if (!ast.body || ast.name.empty() || !ast.origin.end) return false;
  const auto verb = ast.action == RoutineDefinitionAction::Alter ? "ALTER" : "CREATE";
  auto range = shifted(ast.origin, offset);
  if (range.end > normalized.size() || range.end <= range.begin) return false;
  if (normalized[range.end - 1] == ';' && out.source[range.end - 1] != ';') --range.end;
  auto *root = out.add(out.result.roots, withName(std::string(verb) +
      (ast.function ? " FUNCTION" : " PROCEDURE"), joinedName(ast.name)), range);
  if (!root) return true;
  const auto &body = ast.nodes.at(ast.body);
  auto headerEnd = offset + body.origin.begin;
  if (ast.dialect == Dialect::PostgreSQL) {
    for (auto i = tokenIndex; i < tokens.size() && tokens[i].range.begin < headerEnd; ++i)
      if (tokens[i].kind == TokenKind::String && tokens[i].range.end > headerEnd) {
        headerEnd = tokens[i].range.begin;
        break;
      }
  }
  if (auto *header = out.add(root->children, "HEADER", {offset, headerEnd}, 1))
    for (auto id : ast.parameters) {
      const auto &param = ast.symbols.at(id);
      out.add(header->children, withName("PARAM", param.name), shifted(param.origin, offset), 2);
    }
  routineNodes(out, root->children, ast, ast.body, offset, 1, true);
  return true;
}

// A bounded structural parser for procedural dialects the compiler does not
// yet lower, and for documents being edited. It recovers navigation ranges
// without treating recovered SQL as valid input for execution or conversion.
class ProceduralOutline {
  OutlineBuilder &out_;
  const std::vector<Token> &tokens_;
  std::size_t at_;
  std::size_t limit_;

  std::string word(std::size_t offset = 0) const {
    if (at_ + offset >= limit_) return {};
    const auto &t = tokens_[at_ + offset];
    if (t.kind == TokenKind::Word) return asciiUpper(t.value);
    return t.kind == TokenKind::Symbol ? t.value : std::string{};
  }

  bool nameAt(std::size_t offset = 0) const {
    if (at_ + offset >= limit_) return false;
    const auto kind = tokens_[at_ + offset].kind;
    return kind == TokenKind::Identifier || kind == TokenKind::Word;
  }

  std::string qualified() {
    if (!nameAt()) return {};
    auto name = tokens_[at_++].value;
    while (word() == "." && nameAt(1)) {
      ++at_;
      name += "." + tokens_[at_++].value;
    }
    return name;
  }

  bool oneOf(std::initializer_list<std::string_view> choices) const {
    const auto token = word();
    return std::find(choices.begin(), choices.end(), token) != choices.end();
  }

  bool eat(std::string_view value) {
    if (word() != value) return false;
    ++at_;
    return true;
  }

  SourceRange span(std::size_t begin) const {
    if (begin >= at_) return {};
    return {tokens_[begin].range.begin, tokens_[at_ - 1].range.end};
  }

  bool separatorAtEnd() const {
    const auto &token = tokens_[at_ - 1];
    return token.kind == TokenKind::Symbol && token.value == ";";
  }

  // CASE expressions and parentheses can contain words that look like
  // procedural delimiters. Only consider stops at the outer SQL level.
  void seek(std::initializer_list<std::string_view> stops) {
    std::size_t parentheses = 0, cases = 0;
    while (at_ < limit_ && !out_.stopped()) {
      const auto t = word();
      if (!parentheses && !cases &&
          std::find(stops.begin(), stops.end(), t) != stops.end()) return;
      if (t == "(") ++parentheses;
      else if (t == ")" && parentheses) --parentheses;
      else if (t == "CASE") ++cases;
      else if (t == "END" && cases) --cases;
      ++at_;
    }
  }

  void item(std::vector<Node> &nodes, std::size_t begin, std::string label,
            std::size_t depth, std::vector<Node> children = {}) {
    auto range = span(begin);
    if (begin < at_ && separatorAtEnd() && out_.source[range.end - 1] != ';')
      range.end = at_ - begin > 1 ? tokens_[at_ - 2].range.end : range.begin;
    if (label.empty()) label = out_.excerpt(range);
    if (auto *entry = out_.add(nodes, std::move(label), range, depth, true))
      entry->children = std::move(children);
  }

  void list(std::vector<Node> &nodes, std::size_t depth,
            std::initializer_list<std::string_view> stops) {
    while (at_ < limit_ && !oneOf(stops) && out_.room(depth)) {
      const auto start = at_;
      if (!eat(";")) statement(nodes, depth);
      if (start == at_) return;
    }
  }

  void end(std::string_view tag = {}) {
    if (!eat("END")) return;
    if (!tag.empty()) eat(tag);
    else if ((out_.dialect == Dialect::Oracle || isMySqlDialect(out_.dialect)) &&
             nameAt() && word(1) == ";") ++at_;
    eat(";");
  }

  void handler(std::vector<Node> &nodes, std::size_t depth) {
    const auto start = at_++;
    seek({"THEN", "WHEN", "END"});
    const auto label = out_.excerpt(span(start));
    std::vector<Node> children;
    if (eat("THEN")) list(children, depth + 1, {"WHEN", "ELSE", "END"});
    item(nodes, start, label, depth, std::move(children));
  }

  void block(std::vector<Node> &nodes, std::size_t depth) {
    const auto start = at_++;
    const bool attempt = out_.dialect == Dialect::SQLServer && eat("TRY");
    std::vector<Node> children;
    list(children, depth + 1, {"EXCEPTION", "END"});
    if (eat("EXCEPTION")) {
      const auto handlers = at_ - 1;
      std::vector<Node> branches;
      while (word() == "WHEN" && out_.room(depth + 2)) handler(branches, depth + 2);
      item(children, handlers, "EXCEPTION", depth + 1, std::move(branches));
    }
    end(attempt ? "TRY" : "");
    if (attempt && word() == "BEGIN" && word(1) == "CATCH") {
      const auto catchStart = at_;
      at_ += 2;
      std::vector<Node> branch;
      list(branch, depth + 2, {"END"});
      end("CATCH");
      item(children, catchStart, "CATCH", depth + 1, std::move(branch));
    }
    item(nodes, start, attempt ? "TRY … CATCH" : "BEGIN … END", depth, std::move(children));
  }

  void conditional(std::vector<Node> &nodes, std::size_t depth) {
    const auto start = at_++;
    const bool tsql = out_.dialect == Dialect::SQLServer;
    if (tsql)
      seek({"BEGIN", "SELECT", "INSERT", "UPDATE", "DELETE", "SET", "PRINT",
            "EXEC", "EXECUTE", "RETURN", "THROW", ";"});
    else
      seek({"THEN", ";"});
    const auto title = out_.excerpt(span(start));
    std::vector<Node> children;
    if (tsql && at_ < limit_ && !oneOf({";", "END", "ELSE"}))
      statement(children, depth + 1);
    else if (eat("THEN"))
      list(children, depth + 1, {"ELSIF", "ELSEIF", "ELSE", "END"});
    while (oneOf({"ELSIF", "ELSEIF", "ELSE"}) && out_.room(depth + 1)) {
      const auto branch = at_;
      const bool fallback = eat("ELSE");
      if (!fallback) {
        ++at_;
        seek({"THEN", ";", "END"});
      }
      const auto label = out_.excerpt(span(branch));
      std::vector<Node> body;
      if (tsql) statement(body, depth + 2);
      else if (fallback || eat("THEN"))
        list(body, depth + 2, {"ELSIF", "ELSEIF", "ELSE", "END"});
      item(children, branch, label, depth + 1, std::move(body));
    }
    if (!tsql) end("IF");
    eat(";");
    item(nodes, start, title, depth, std::move(children));
  }

  void loop(std::vector<Node> &nodes, std::size_t depth) {
    const auto start = at_;
    const auto type = word();
    ++at_;
    if (type != "LOOP" && type != "REPEAT") seek({"LOOP", "DO", "BEGIN", ";"});
    const auto title = out_.excerpt(span(start));
    std::vector<Node> children;
    if (word() == "BEGIN" && out_.room(depth + 1)) block(children, depth + 1);
    else if (eat("LOOP") || eat("DO") || type == "LOOP" || type == "REPEAT") {
      list(children, depth + 1, {"UNTIL", "END"});
      if (eat("UNTIL")) seek({"END"});
      end(type == "REPEAT" ? "REPEAT" : word(1) == "WHILE" ? "WHILE" : "LOOP");
    }
    eat(";");
    item(nodes, start, title, depth, std::move(children));
  }

  void declaration(std::vector<Node> &nodes, std::size_t depth) {
    const auto start = at_;
    if (word() == "DECLARE" && word(2) == "HANDLER") {
      at_ += 3;
      seek({"BEGIN", "SET", "SELECT", "INSERT", "UPDATE", "DELETE", "SIGNAL", "RESIGNAL", "CALL", ";"});
      const auto label = out_.excerpt(span(start));
      std::vector<Node> body;
      if (word() != ";" && out_.room(depth + 1)) statement(body, depth + 1);
      eat(";");
      item(nodes, start, label, depth, std::move(body));
      return;
    }
    seek({";", "BEGIN", "END"});
    eat(";");
    item(nodes, start, {}, depth);
  }

  void member(std::vector<Node> &nodes, std::size_t depth) {
    const auto start = at_;
    const auto kind = word();
    ++at_;
    const auto name = qualified();
    seek({"AS", "IS", ";", "END"});
    if (!eat("AS") && !eat("IS")) {
      eat(";");
      item(nodes, start, withName(kind, name), depth);
      return;
    }
    std::vector<Node> children;
    item(children, start, "HEADER", depth + 1);
    declarations(children, depth + 1);
    if (word() == "BEGIN" && out_.room(depth + 1)) block(children, depth + 1);
    item(nodes, start, withName(kind, name), depth, std::move(children));
  }

  void declarations(std::vector<Node> &nodes, std::size_t depth) {
    while (at_ < limit_ && !oneOf({"BEGIN", "END"}) && out_.room(depth)) {
      const auto start = at_;
      if (oneOf({"PROCEDURE", "FUNCTION"})) member(nodes, depth);
      else declaration(nodes, depth);
      if (at_ == start) return;
    }
  }

public:
  ProceduralOutline(OutlineBuilder &out, const std::vector<Token> &tokens,
                    std::size_t begin, std::size_t end)
      : out_(out), tokens_(tokens), at_(begin), limit_(end) {}

  void statement(std::vector<Node> &nodes, std::size_t depth) {
    if (!out_.room(depth)) return;
    const auto start = at_;
    if (word() == "BEGIN") block(nodes, depth);
    else if (word() == "IF") conditional(nodes, depth);
    else if (oneOf({"FOR", "WHILE", "LOOP", "REPEAT"})) loop(nodes, depth);
    else if (word() == "DECLARE" && isMySqlDialect(out_.dialect)) declaration(nodes, depth);
    else if (eat("CASE")) {
      seek({"WHEN", ";", "END"});
      std::vector<Node> children;
      while (word() == "WHEN" && out_.room(depth + 1)) handler(children, depth + 1);
      if (eat("ELSE")) {
        const auto branch = at_ - 1;
        std::vector<Node> otherwise;
        list(otherwise, depth + 2, {"END"});
        item(children, branch, "ELSE", depth + 1, std::move(otherwise));
      }
      end("CASE");
      item(nodes, start, "CASE", depth, std::move(children));
    } else {
      seek({";", "END", "ELSE", "ELSIF", "ELSEIF", "EXCEPTION", "WHEN"});
      eat(";");
      item(nodes, start, {}, depth);
    }
  }

  std::size_t definition(std::size_t start, const std::string &verb, std::string kind) {
    ++at_; // The object type.
    if (kind == "PACKAGE" && eat("BODY")) kind += " BODY";
    if (eat("IF")) {
      eat("NOT");
      eat("EXISTS");
    }
    const auto name = qualified();
    std::vector<Node> children;
    if (verb == "DROP") {
      seek({";"});
      eat(";");
    } else if (kind == "PACKAGE" || kind == "PACKAGE BODY") {
      seek({"AS", "IS", ";"});
      if (eat("AS") || eat("IS")) {
        item(children, start, "HEADER", 1);
        declarations(children, 1);
        if (word() == "BEGIN") block(children, 1);
        else end();
      } else eat(";");
    } else if (kind == "TRIGGER") {
      if (isMySqlDialect(out_.dialect)) {
        seek({"FOR", ";"});
        if (eat("FOR") && eat("EACH") && eat("ROW")) {
          if (eat("FOLLOWS") || eat("PRECEDES")) qualified();
          item(children, start, "HEADER", 1);
          statement(children, 1);
        }
      } else if (out_.dialect == Dialect::PostgreSQL) {
        seek({"EXECUTE", ";"});
        item(children, start, "HEADER", 1);
        const auto action = at_;
        seek({";"});
        eat(";");
        item(children, action, {}, 1);
      } else {
        seek({"DECLARE", "BEGIN", "AS", ";"});
        if (eat("AS") || oneOf({"DECLARE", "BEGIN"})) {
          item(children, start, "HEADER", 1);
          eat("DECLARE");
          if (out_.dialect == Dialect::SQLServer) list(children, 1, {});
          else {
            if (out_.dialect == Dialect::Oracle && word() != "BEGIN") declarations(children, 1);
            statement(children, 1);
          }
        }
      }
      eat(";");
    } else {
      seek({"AS", "IS", "BEGIN", "RETURN", ";"});
      // Oracle RETURN belongs to the function signature.
      if (word() == "RETURN" && out_.dialect == Dialect::Oracle)
        seek({"AS", "IS", ";"});
      const bool marker = eat("AS") || eat("IS");
      item(children, start, "HEADER", 1);
      if (marker && out_.dialect == Dialect::Oracle) declarations(children, 1);
      if (out_.dialect == Dialect::SQLServer) list(children, 1, {});
      else if (at_ < limit_ && !oneOf({";", "END"})) statement(children, 1);
      eat(";");
    }
    item(out_.result.roots, start, withName(verb + " " + kind, name), 0, std::move(children));
    return at_;
  }

  std::size_t anonymousBlock() {
    const auto start = at_;
    std::vector<Node> children;
    if (eat("DECLARE")) declarations(children, 1);
    if (word() == "BEGIN" && out_.room(1)) block(children, 1);
    item(out_.result.roots, start, "PL/SQL BLOCK", 0, std::move(children));
    return at_;
  }

  std::size_t quotedBody(std::vector<Node> &nodes, std::size_t depth) {
    list(nodes, depth, {});
    return at_;
  }
};

bool keyword(const std::vector<Token> &tokens, std::size_t i, std::string_view name) {
  return i < tokens.size() && tokens[i].kind == TokenKind::Word &&
         asciiUpper(tokens[i].value) == name;
}

bool separator(const Token &t) { return t.kind == TokenKind::Symbol && t.value == ";"; }

std::size_t definitionType(const std::vector<Token> &tokens, std::size_t start) {
  if (!keyword(tokens, start, "CREATE") && !keyword(tokens, start, "ALTER") &&
      !keyword(tokens, start, "DROP")) return tokens.size();
  auto i = start + 1;
  if (keyword(tokens, i, "OR") &&
      (keyword(tokens, i + 1, "REPLACE") || keyword(tokens, i + 1, "ALTER"))) i += 2;
  if (keyword(tokens, i, "EDITIONABLE") || keyword(tokens, i, "NONEDITIONABLE") ||
      keyword(tokens, i, "TEMPORARY") || keyword(tokens, i, "TEMP")) ++i;
  if (keyword(tokens, i, "DEFINER")) {
    // DEFINER accounts can be quoted and can include a host component.
    for (auto end = std::min(tokens.size(), i + 8); i < end; ++i)
      if (keyword(tokens, i, "TRIGGER") || keyword(tokens, i, "FUNCTION") ||
          keyword(tokens, i, "PROCEDURE") || keyword(tokens, i, "VIEW") ||
          separator(tokens[i])) break;
  }
  return i;
}

std::size_t batchEnd(const std::vector<Token> &tokens, std::size_t start,
                     const std::string &source) {
  for (auto i = start; i < tokens.size(); ++i)
    if (separator(tokens[i]) && source[tokens[i].range.begin] != ';') return i;
  return tokens.size();
}

std::vector<Token> significant(Compilation &c, std::size_t offset = 0) {
  lex(c, true, true);
  std::vector<Token> result;
  for (auto &t : c.tokens)
    if (t.kind != TokenKind::Comment && t.kind != TokenKind::End) {
      t.range = shifted(t.range, offset);
      result.push_back(t);
    }
  return result;
}

bool addPgQuotedBody(OutlineBuilder &out, const std::vector<Token> &tokens,
                     std::size_t begin, std::size_t type, std::size_t end,
                     SourceRange range) {
  if (type + 1 >= end) return false;
  for (auto i = type + 2; i < end; ++i) {
    if (!keyword(tokens, i - 1, "AS") || tokens[i].kind != TokenKind::String) continue;
    const auto quoted = tokens[i].range;
    if (out.source[quoted.begin] != '$') continue;
    const auto delimiterEnd = out.source.find('$', quoted.begin + 1);
    if (delimiterEnd == std::string::npos || delimiterEnd >= quoted.end) continue;
    const auto delimiterLength = delimiterEnd + 1 - quoted.begin;
    if (quoted.end < delimiterEnd + 1 + delimiterLength) continue;
    const auto body = SourceRange{delimiterEnd + 1, quoted.end - delimiterLength};
    std::string name = tokens[type + 1].value;
    for (auto j = type + 2; j + 1 < i && tokens[j].value == "."; j += 2)
      name += "." + tokens[j + 1].value;
    auto *root = out.add(out.result.roots, withName("CREATE " + asciiUpper(tokens[type].value), name),
                         range, 0, true);
    if (!root) return true;
    out.add(root->children, "HEADER", {tokens[begin].range.begin, quoted.begin}, 1);
    if (auto *content = out.add(root->children, "BODY", body, 1, true)) {
      Compilation inner(out.dialect, out.source.substr(body.begin, body.end - body.begin),
                        out.options, "<outline body>");
      auto innerTokens = significant(inner, body.begin);
      ProceduralOutline parser(out, innerTokens, 0, innerTokens.size());
      parser.quotedBody(content->children, 2);
    }
    return true;
  }
  return false;
}

} // namespace

SqlOutline outlineSql(Dialect dialect, std::string source, LanguageOptions options) {
  OutlineBuilder builder{dialect, source, options, {}};
  if (source.size() > options.limits.sourceBytes) {
    builder.result.truncated = true;
    return std::move(builder.result);
  }
  try {
    std::vector<Diagnostic> normalizationErrors;
    auto normalized = normalizeConversionScript(dialect, source, options, normalizationErrors);
    if (!normalizationErrors.empty()) {
      builder.result.partial = true;
      normalized = source;
    }
    Compilation script(dialect, normalized, options, "<outline script>");
    const auto tokens = significant(script);
    builder.result.partial |= !script.diagnostics.empty();
    std::size_t count = 0;
    for (std::size_t i = 0; i < tokens.size() && builder.room();) {
      if (separator(tokens[i])) {
        ++i;
        continue;
      }
      if (++count > options.limits.statements) {
        builder.result.truncated = true;
        break;
      }
      const auto start = i;
      const auto begin = tokens[i].range.begin;
      const auto type = definitionType(tokens, i);
      const auto verb = asciiUpper(tokens[i].value);
      const bool routine = isRoutineStart(dialect, tokens, i);
      const bool routineDefinition = verb != "DROP" &&
          (routine || keyword(tokens, type, "FUNCTION") ||
           keyword(tokens, type, "PROCEDURE") || keyword(tokens, type, "PROC"));
      const bool package = keyword(tokens, type, "PACKAGE");
      const bool packageBody = package && keyword(tokens, type + 1, "BODY");
      const bool trigger = keyword(tokens, type, "TRIGGER");
      const bool anonymous = dialect == Dialect::Oracle && (verb == "DECLARE" || verb == "BEGIN");
      const auto batch = dialect == Dialect::SQLServer && (routineDefinition || trigger) && verb != "DROP"
          ? batchEnd(tokens, i, source) : tokens.size();
      auto advance = [&](std::size_t end) { while (i < tokens.size() && tokens[i].range.begin < end) ++i; };

      // Parsing one definition at a time avoids consuming the next root. A
      // MySQL client delimiter or an Oracle/SQL Server batch marker is already
      // normalized without shifting source byte offsets.
      if (routine && !builder.stopped()) {
        const auto end = batch < tokens.size() ? tokens[batch].range.begin : normalized.size();
        const auto parsed = parseRoutineForCompletion(dialect, normalized.substr(begin, end - begin), options);
        const auto remaining = std::lower_bound(tokens.begin() + i, tokens.begin() + batch,
            begin + parsed.ast.origin.end, [](const Token &token, std::size_t offset) {
              return token.range.begin < offset;
            });
        const bool wholeBatch = dialect != Dialect::SQLServer ||
            std::all_of(remaining, tokens.begin() + batch, separator);
        if (wholeBatch && parsed.diagnostics.empty() &&
            addRoutine(builder, parsed.ast, begin, normalized, tokens, i)) {
          advance(begin + parsed.ast.origin.end);
          continue;
        }
      }

      const auto next = statementEndIndex(dialect, tokens, i);
      const auto last = std::min(next, tokens.size() - 1);
      auto range = SourceRange{begin, tokens[last].range.end};
      // Client delimiters are not part of the SQL node copied to the clipboard.
      auto nodeRange = range;
      if (separator(tokens[last]) && source[tokens[last].range.begin] != ';')
        nodeRange.end = last > start ? tokens[last - 1].range.end : begin;

      if (routineDefinition && dialect == Dialect::PostgreSQL &&
          addPgQuotedBody(builder, tokens, start, type, last + 1, nodeRange)) {
        i = last + 1;
        continue;
      }

      if (!packageBody && !routine && !anonymous) {
        const auto parsed = syntaxOnly(dialect, normalized.substr(begin, range.end - begin), options);
        if (parsed->diagnostics.empty() && parsed->statements.size() == 1) {
          const auto &stmt = parsed->statements.front();
          if (auto *root = builder.add(builder.result.roots, statementName(*parsed, stmt, verb), nodeRange))
            builder.syntax(*root, *parsed, stmt, begin, 1);
          i = last + 1;
          continue;
        }
      }

      if (anonymous) {
        ProceduralOutline parser(builder, tokens, i, tokens.size());
        i = parser.anonymousBlock();
      } else if (type < tokens.size() && (routineDefinition || package || trigger)) {
        const auto kind = asciiUpper(tokens[type].value);
        ProceduralOutline parser(builder, tokens, type, batch);
        i = parser.definition(start, verb, kind);
      }
      if (i <= start) {
        builder.add(builder.result.roots, builder.excerpt(nodeRange), nodeRange, 0, true);
        i = last + 1;
      }
    }
  } catch (const CompilationAborted &) {
    builder.result.truncated = true;
  }
  return std::move(builder.result);
}

} // namespace navidog::sql::compiler
