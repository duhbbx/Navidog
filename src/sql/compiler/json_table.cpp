#include "parser.hpp"

namespace navidog::sql::compiler {
std::vector<BasicJsonTableColumn<NamePart, QualifiedName>>
Parser::jsonTableColumns(std::size_t &count) {
  if (depth_ >= context_.options.limits.nesting)
    fail("JSON_TABLE column nesting limit exceeded", DiagnosticCode::ResourceLimit);
  struct Depth {
    std::size_t &value;
    explicit Depth(std::size_t &depth) : value(depth) { ++value; }
    ~Depth() { --value; }
  } depth(depth_);
  expect("COLUMNS");
  expect("(");
  std::vector<BasicJsonTableColumn<NamePart, QualifiedName>> columns;
  do {
    if (++count > context_.options.limits.columnsPerQuery)
      fail("JSON_TABLE column limit exceeded", DiagnosticCode::ResourceLimit);
    syntaxNode();
    BasicJsonTableColumn<NamePart, QualifiedName> column;
    column.origin.begin = current().range.begin;
    if (eat("NESTED")) {
      column.kind = JsonTableColumnKind::Nested;
      eat("PATH");
      if (current().kind != TokenKind::String)
        fail("JSON_TABLE NESTED PATH requires a string literal");
      column.path = take().value;
      column.children = jsonTableColumns(count);
    } else {
      column.name = namePart();
      if (eat("FOR")) {
        expect("ORDINALITY");
        column.kind = JsonTableColumnKind::Ordinality;
      } else {
        column.type = typeSpec();
        if (eat("EXISTS")) column.kind = JsonTableColumnKind::Exists;
        expect("PATH");
        if (current().kind != TokenKind::String)
          fail("JSON_TABLE PATH requires a string literal");
        column.path = take().value;
        while (column.kind == JsonTableColumnKind::Path &&
               (at("NULL") || at("ERROR") || at("DEFAULT"))) {
          JsonTableResponse response;
          if (eat("NULL")) response.kind = JsonTableResponseKind::Null;
          else if (eat("ERROR")) response.kind = JsonTableResponseKind::Error;
          else {
            expect("DEFAULT");
            response.kind = JsonTableResponseKind::Default;
            if (current().kind != TokenKind::String)
              fail("JSON_TABLE DEFAULT requires a JSON string literal");
            response.json = take().value;
          }
          expect("ON");
          const bool empty = eat("EMPTY");
          if (!empty) expect("ERROR");
          auto &slot = empty ? column.onEmpty : column.onError;
          if (slot.kind != JsonTableResponseKind::Unspecified)
            fail("JSON_TABLE response specified more than once for the same event");
          slot = std::move(response);
        }
      }
    }
    column.origin.end = previous().range.end;
    columns.push_back(std::move(column));
  } while (eat(","));
  expect(")");
  return columns;
}
TableSource Parser::jsonTableSource() {
  TableSource source;
  source.range.begin = current().range.begin;
  expect("JSON_TABLE");
  expect("(");
  auto json = std::make_shared<JsonTableSyntax>();
  json->document = expression();
  requireScalar(json->document);
  expect(",");
  if (current().kind != TokenKind::String)
    fail("JSON_TABLE row path requires a string literal");
  json->path = take().value;
  std::size_t count = 0;
  json->columns = jsonTableColumns(count);
  expect(")");
  source.alias = alias(true);
  if (!source.alias) fail("JSON_TABLE requires a table alias");
  source.jsonTable = std::move(json);
  source.range.end = previous().range.end;
  return source;
}
} // namespace navidog::sql::compiler
