#include "frontends/frontends.hpp"

namespace navidog::sql::compiler {
const char *inspectionKindName(InspectionKind kind) {
  switch (kind) {
  case InspectionKind::TableGroups: return "TABLEGROUPS";
  case InspectionKind::DdlStatus: return "ADMIN SHOW DDL";
  case InspectionKind::DdlJobs: return "ADMIN SHOW DDL JOBS";
  case InspectionKind::Databases: return "DATABASES";
  case InspectionKind::Tables: return "TABLES";
  case InspectionKind::Columns: return "COLUMNS";
  case InspectionKind::Indexes: return "INDEX";
  case InspectionKind::TableStatus: return "TABLE STATUS";
  case InspectionKind::Variables: return "VARIABLES";
  case InspectionKind::Status: return "STATUS";
  case InspectionKind::ProcessList: return "PROCESSLIST";
  case InspectionKind::CreateDefinition: return "CREATE";
  case InspectionKind::Describe: return "DESCRIBE";
  }
  return "UNKNOWN";
}
const char *inspectionObjectName(InspectionObject object) {
  switch (object) {
  case InspectionObject::Table: return "TABLE";
  case InspectionObject::View: return "VIEW";
  case InspectionObject::Database: return "DATABASE";
  case InspectionObject::Function: return "FUNCTION";
  case InspectionObject::Procedure: return "PROCEDURE";
  case InspectionObject::Trigger: return "TRIGGER";
  case InspectionObject::Event: return "EVENT";
  case InspectionObject::TableGroup: return "TABLEGROUP";
  }
  return "UNKNOWN";
}
Statement Parser::inspectionStatement() {
  fail("inspection statement is not implemented for this dialect",
       DiagnosticCode::UnsupportedSyntax);
}
Statement MySQLParser::inspectionStatement() {
  Statement s;
  s.kind = StatementKind::Inspect;
  s.inspection.emplace();
  auto &out = *s.inspection;
  auto pattern = [&] {
    if (current().kind != TokenKind::String)
      fail("inspection LIKE requires a string pattern");
    auto t = take();
    out.pattern = node({ExprKind::String, t.range, std::move(t.value), {}, {}});
  };
  auto target = [&] {
    out.target = qualifiedName();
    if (out.target.size() > 2)
      fail("MySQL inspection names accept at most database.object");
  };
  if (eat("DESCRIBE") || eat("DESC")) {
    if (at("SELECT") || at("INSERT") || at("UPDATE") || at("DELETE") || at("FORMAT"))
      fail("DESCRIBE query-plan form is not implemented in the local parser",
           DiagnosticCode::UnsupportedSyntax);
    out.kind = InspectionKind::Describe;
    target();
    if (current().kind == TokenKind::String)
      pattern();
    else if (nameToken())
      out.column = namePart();
    return s;
  }
  expect("SHOW");
  out.extended = eat("EXTENDED");
  out.full = eat("FULL");
  if (eat("GLOBAL")) out.scope = InspectionScope::Global;
  else if (eat("SESSION")) out.scope = InspectionScope::Session;
  bool filter = false, like = true, database = false;
  if (eat("DATABASES") || eat("SCHEMAS")) {
    out.kind = InspectionKind::Databases; filter = true;
  } else if (eat("TABLES")) {
    out.kind = InspectionKind::Tables; filter = database = true;
  } else if (eat("COLUMNS") || eat("FIELDS")) {
    out.kind = InspectionKind::Columns; filter = database = true;
    if (!eat("FROM") && !eat("IN")) fail("expected FROM or IN before table name");
    target();
  } else if (eat("INDEX") || eat("INDEXES") || eat("KEYS")) {
    out.kind = InspectionKind::Indexes; filter = database = true; like = false;
    if (!eat("FROM") && !eat("IN")) fail("expected FROM or IN before table name");
    target();
  } else if (eat("TABLE")) {
    expect("STATUS"); out.kind = InspectionKind::TableStatus; filter = database = true;
  } else if (eat("VARIABLES")) {
    out.kind = InspectionKind::Variables; filter = true;
  } else if (eat("STATUS")) {
    out.kind = InspectionKind::Status; filter = true;
  } else if (eat("PROCESSLIST")) {
    out.kind = InspectionKind::ProcessList;
  } else if (eat("CREATE")) {
    out.kind = InspectionKind::CreateDefinition;
    if (eat("TABLE")) out.object = InspectionObject::Table;
    else if (eat("VIEW")) out.object = InspectionObject::View;
    else if (eat("DATABASE") || eat("SCHEMA")) out.object = InspectionObject::Database;
    else if (eat("FUNCTION")) out.object = InspectionObject::Function;
    else if (eat("PROCEDURE")) out.object = InspectionObject::Procedure;
    else if (eat("TRIGGER")) out.object = InspectionObject::Trigger;
    else if (eat("EVENT")) out.object = InspectionObject::Event;
    else fail("this SHOW CREATE object is not implemented", DiagnosticCode::UnsupportedSyntax);
    if (out.object == InspectionObject::Database && eat("IF")) {
      expect("NOT"); expect("EXISTS"); out.ifNotExists = true;
    }
    if (context_.dialect == Dialect::TiDB &&
        (out.object == InspectionObject::Function || out.object == InspectionObject::Procedure ||
         out.object == InspectionObject::Trigger || out.object == InspectionObject::Event))
      fail("TiDB has no such stored object type", DiagnosticCode::UnsupportedSyntax);
    target();
    if (out.object == InspectionObject::Database && out.target.size() != 1)
      fail("SHOW CREATE DATABASE requires one database name");
  } else {
    fail("this SHOW form is not implemented in the local parser",
         DiagnosticCode::UnsupportedSyntax);
  }
  if (out.full && out.kind != InspectionKind::Tables && out.kind != InspectionKind::Columns &&
      out.kind != InspectionKind::ProcessList)
    fail("FULL is only accepted for SHOW TABLES, COLUMNS and PROCESSLIST");
  if (out.extended && out.kind != InspectionKind::Tables && out.kind != InspectionKind::Columns &&
      out.kind != InspectionKind::Indexes)
    fail("EXTENDED is only accepted for SHOW TABLES, COLUMNS and INDEX");
  if (out.scope != InspectionScope::Default && out.kind != InspectionKind::Variables &&
      out.kind != InspectionKind::Status)
    fail("GLOBAL / SESSION is only accepted for SHOW VARIABLES and STATUS");
  if (database && (eat("FROM") || eat("IN")))
    out.database = namePart();
  // These are alternative spellings for the same database qualifier. Reject
  // contradictory qualifiers rather than silently choosing one.
  if (out.database && out.target.size() == 2 &&
      context_.ast.identifier(out.database->identifier) != context_.ast.identifier(out.target[0].identifier))
    fail("conflicting database qualifiers in SHOW statement");
  if (filter && like && eat("LIKE")) pattern();
  else if (filter && eat("WHERE")) s.where = expression();
  return s;
}

TableDefinition inspectionOutput(const ModelInspection &inspection,
                                 const NameResolutionContext &context) {
  TableDefinition output;
  output.name = "__inspection_output";
  auto add = [&](const char *name, ValueType type = ValueType::String) {
    output.columns.push_back({name, type});
  };
  switch (inspection.kind) {
  case InspectionKind::Databases: add("Database"); break;
  case InspectionKind::Tables: {
    const auto database = inspection.database.value_or(context.database);
    if (!database.empty()) output.columns.push_back({"Tables_in_" + database, ValueType::String});
    else output.complete = output.arityKnown = false; // Dynamic result label is unknown.
    if (inspection.full) add("Table_type");
    break;
  }
  case InspectionKind::Columns:
  case InspectionKind::Describe:
    add("Field"); add("Type");
    if (inspection.full) add("Collation");
    add("Null"); add("Key"); add("Default"); add("Extra");
    if (inspection.full) { add("Privileges"); add("Comment"); }
    break;
  case InspectionKind::Indexes:
    add("Table"); add("Non_unique", ValueType::Integer); add("Key_name");
    add("Seq_in_index", ValueType::Integer); add("Column_name"); add("Collation");
    add("Cardinality", ValueType::Integer); add("Sub_part", ValueType::Integer);
    add("Packed"); add("Null"); add("Index_type"); add("Comment");
    add("Index_comment"); add("Visible"); add("Expression");
    break;
  case InspectionKind::TableStatus:
    for (auto name : {"Name", "Engine", "Version", "Row_format", "Rows", "Avg_row_length",
                      "Data_length", "Max_data_length", "Index_length", "Data_free", "Auto_increment",
                      "Create_time", "Update_time", "Check_time", "Collation", "Checksum", "Create_options", "Comment"})
      add(name, ValueType::Unknown); // Version/product-dependent scalar types.
    break;
  case InspectionKind::Variables:
  case InspectionKind::Status: add("Variable_name"); add("Value"); break;
  case InspectionKind::ProcessList:
    for (auto name : {"Id", "User", "Host", "db", "Command", "Time", "State", "Info"}) add(name, ValueType::Unknown);
    break;
  case InspectionKind::TableGroups:
    add("Tablegroup_name"); add("Table_name"); add("Database_name"); add("Sharding");
    break;
  case InspectionKind::DdlStatus:
    add("SCHEMA_VER", ValueType::Integer); add("OWNER_ID"); add("OWNER_ADDRESS");
    add("RUNNING_JOBS"); add("SELF_ID"); add("QUERY");
    break;
  case InspectionKind::DdlJobs:
    for (auto name : {"JOB_ID", "SCHEMA_ID", "TABLE_ID", "ROW_COUNT"}) add(name, ValueType::Integer);
    for (auto name : {"DB_NAME", "TABLE_NAME", "JOB_TYPE", "SCHEMA_STATE", "STATE", "COMMENTS"}) add(name);
    for (auto name : {"CREATE_TIME", "START_TIME", "END_TIME"}) add(name, ValueType::Unknown);
    output.complete = false; // Job output gains columns across versions.
    break;
  case InspectionKind::CreateDefinition:
    break; // Not filterable; vendor version controls the returned definition fields.
  }
  return output;
}
} // namespace navidog::sql::compiler
