#include "frontends.hpp"
#include <algorithm>
#include <unordered_set>

namespace navidog::sql::compiler {
bool TiDBParser::nameToken() const {
  if (current().kind==TokenKind::Word && (at("SSL") || at("REQUIRE"))) return false;
  return MySQLParser::nameToken();
}
void TiDBParser::validateColumnType(const BasicTypeSpec<QualifiedName> &type) {
  // TiDB Type is a closed union of NumericType, StringType and DateAndTimeType;
  // PostgreSQL-style user-defined/qualified/quoted type names are not allowed.
  static const std::unordered_set<std::string> names{
      "TINYINT","SMALLINT","MEDIUMINT","MIDDLEINT","INT","INT1","INT2","INT3","INT4","INT8","INTEGER","BIGINT",
      "BOOL","BOOLEAN","DECIMAL","NUMERIC","FIXED","FLOAT","REAL","DOUBLE","FLOAT4","FLOAT8","BIT",
      "CHARACTER","CHAR","NCHAR","NATIONAL","VARCHAR","VARCHARACTER","NVARCHAR","BINARY","VARBINARY",
      "TINYBLOB","BLOB","MEDIUMBLOB","LONGBLOB","TINYTEXT","TEXT","MEDIUMTEXT","LONGTEXT","ENUM","SET","JSON","LONG","VECTOR",
      "DATE","DATETIME","TIMESTAMP","TIME","YEAR","SQL_TSI_YEAR"};
  if (type.name.size()!=1 || type.name.front().quoted ||
      !names.count(asciiUpper(context_.ast.identifier(type.name.front().identifier))))
    fail("expected a built-in TiDB column type");
  const auto base=asciiUpper(context_.ast.identifier(type.name.front().identifier));
  if (base=="NATIONAL")
    fail("multiword TiDB NATIONAL character types require a dedicated prefix model",DiagnosticCode::UnsupportedSyntax);
  const bool varying=std::find(type.modifiers.begin(),type.modifiers.end(),"VARYING")!=type.modifiers.end();
  if (type.arguments.empty() && (base=="VARCHAR" || base=="VARCHARACTER" || base=="NVARCHAR" ||
      base=="VARBINARY" || ((base=="CHAR" || base=="CHARACTER") && varying)))
    fail("this TiDB character/binary column type requires a length");
  if ((base=="ENUM" || base=="SET") && (type.arguments.empty() ||
      std::any_of(type.arguments.begin(),type.arguments.end(),[](const auto &argument){return argument.kind!=TokenKind::String;})))
    fail("TiDB ENUM/SET require a nonempty list of string literals");
  if (base=="ENUM" || base=="SET") return;
  static const std::unordered_set<std::string> noArguments{
      "BOOL","BOOLEAN","TINYBLOB","MEDIUMBLOB","LONGBLOB","TINYTEXT","MEDIUMTEXT","LONGTEXT","JSON","LONG","DATE"};
  static const std::unordered_set<std::string> precisionScale{
      "DECIMAL","NUMERIC","FIXED","FLOAT","REAL","DOUBLE","FLOAT4","FLOAT8"};
  const auto maximum=noArguments.count(base)?0u:precisionScale.count(base)?2u:1u;
  if (type.arguments.size()>maximum)
    fail("too many TiDB column type arguments");
  for (const auto &argument:type.arguments)
    if (argument.kind!=TokenKind::Number || argument.value.empty() ||
        argument.value.find_first_not_of("0123456789")!=std::string::npos)
      fail("TiDB column lengths/precision require unsigned integer tokens");
  if ((base=="DOUBLE" || base=="REAL" || base=="FLOAT8") && type.arguments.size()==1)
    fail("TiDB double-precision column types require either zero or two arguments in the default parser profile");
}
Parser::DdlGrammar TiDBParser::ddlGrammar() const {
  auto grammar = MySQLParser::ddlGrammar();
  grammar.tidb = true;
  return grammar;
}

Statement TiDBParser::inspectionStatement() {
  if (!eat("ADMIN")) return MySQLParser::inspectionStatement();
  if (!eat("SHOW") || !eat("DDL"))
    fail("only ADMIN SHOW DDL and ADMIN SHOW DDL JOBS are covered",
         DiagnosticCode::UnsupportedSyntax);
  Statement statement;
  statement.kind = StatementKind::Inspect;
  auto &inspection = statement.inspection.emplace();
  inspection.kind = InspectionKind::DdlStatus;
  if (eat("JOBS")) {
    inspection.kind = InspectionKind::DdlJobs;
    if (current().kind == TokenKind::Number) {
      inspection.jobCount = ddlUnsignedInteger();
      if (*inspection.jobCount > static_cast<std::uint64_t>(INT64_MAX))
        fail("DDL JOBS count exceeds the signed 64-bit range",
             DiagnosticCode::InvalidQueryShape);
    }
    if (eat("WHERE")) {
      statement.where = expression();
      requireScalar(statement.where);
    }
  } else if (at("JOB")) {
    fail("ADMIN SHOW DDL JOB QUERIES is not covered", DiagnosticCode::UnsupportedSyntax);
  }
  return statement;
}

Statement TiDBParser::triggerStatement() {
  fail("TiDB does not support triggers", DiagnosticCode::UnsupportedSyntax);
}
} // namespace navidog::sql::compiler
