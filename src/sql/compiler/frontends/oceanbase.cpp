#include "frontends.hpp"

namespace navidog::sql::compiler {
OceanBaseParser::OceanBaseParser(Compilation &c) : MySQLParser(c) {}

Parser::DdlGrammar OceanBaseParser::ddlGrammar() const {
  auto grammar = MySQLParser::ddlGrammar();
  grammar.oceanbase = true;
  return grammar;
}

Statement OceanBaseParser::inspectionStatement() {
  const bool list = at("SHOW") && at("TABLEGROUPS", 1);
  const bool definition = at("SHOW") && at("CREATE", 1) && at("TABLEGROUP", 2);
  if (!list && !definition) return MySQLParser::inspectionStatement();
  Statement statement;
  statement.kind = StatementKind::Inspect;
  auto &inspection = statement.inspection.emplace();
  expect("SHOW");
  if (list) {
    expect("TABLEGROUPS");
    inspection.kind = InspectionKind::TableGroups;
  } else {
    expect("CREATE");
    expect("TABLEGROUP");
    inspection.kind = InspectionKind::CreateDefinition;
    inspection.object = InspectionObject::TableGroup;
    inspection.target = {namePart()};
  }
  return statement;
}
} // namespace navidog::sql::compiler
