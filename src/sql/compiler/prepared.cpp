#include "parser.hpp"

namespace navidog::sql::compiler {
Statement Parser::preparedStatement() {
  Statement statement;
  statement.kind = StatementKind::Prepared;
  auto &prepared = statement.prepared.emplace();
  const auto userVariable = [&] {
    const auto &token = current();
    return token.kind == TokenKind::Variable && token.value.size() > 1 &&
           token.value.front() == '@' && token.value[1] != '@';
  };
  if (eat("PREPARE")) {
    prepared.action = PreparedAction::Prepare;
    prepared.name = namePart();
    expect("FROM");
    if (current().kind != TokenKind::String && !userVariable())
      fail("PREPARE FROM requires a SQL string literal or a MySQL user variable");
    prepared.source = primary();
  } else if (eat("EXECUTE")) {
    prepared.action = PreparedAction::Execute;
    prepared.name = namePart();
    if (eat("USING")) {
      do {
        if (!userVariable())
          fail("EXECUTE USING in this MySQL baseline requires session user variables");
        prepared.arguments.push_back(primary());
      } while (eat(","));
    }
  } else {
    if (!eat("DROP")) expect("DEALLOCATE");
    expect("PREPARE");
    prepared.action = PreparedAction::Deallocate;
    prepared.name = namePart();
  }
  return statement;
}
} // namespace navidog::sql::compiler
