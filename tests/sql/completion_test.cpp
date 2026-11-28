#include "navidog/sql/compiler/completion.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace navidog::sql::compiler;
void check(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
}
MemoryCatalog fixture() {
  MemoryCatalog c;
  c.tables.push_back({"app",
                      "public",
                      "users",
                      {{"id", ValueType::Integer},
                       {"name", ValueType::String},
                       {"DisplayName", ValueType::String}},
                      true,
                      true});
  c.tables.push_back({"app",
                      "public",
                      "orders",
                      {{"id", ValueType::Integer},
                       {"user_id", ValueType::Integer},
                       {"amount", ValueType::Decimal}},
                      true,
                      true});
  c.tables.push_back({"other",
                      "public",
                      "secret",
                      {{"password", ValueType::String}},
                      true,
                      true});
  c.routines.push_back({"app", "public", "calculate", 1, 2, false, false});
  c.routines.push_back({"app", "public", "process_order", 1, 1, true, false});
  return c;
}
CompletionResult complete(Dialect d, std::string s,
                          const CatalogSnapshot *c = nullptr) {
  auto at = s.find('|');
  check(at != std::string::npos, "missing test cursor");
  s.erase(at, 1);
  return completeSql(d, s, at, c, {"app", "public", d});
}
const CompletionItem *find(const CompletionResult &r, const std::string &label,
                           CompletionKind kind) {
  for (const auto &i : r.items)
    if (i.label == label && i.kind == kind)
      return &i;
  return nullptr;
}
void expect(const CompletionResult &r, const std::string &label,
            CompletionKind kind) {
  check(find(r, label, kind),
        "missing " + label + "; " +
            (r.diagnostics.empty() ? "" : r.diagnostics.front().message));
}
int main() {
  try {
    {
      constexpr auto pg=Dialect::PostgreSQL;
      MemoryCatalog large;
      for (int i=0;i<128;++i) {
        large.tables.push_back({"app","public","noise_"+std::to_string(i),{}});
        large.routines.push_back({"app","public","noise_fn_"+std::to_string(i),0,1,false,false});
        large.routines.push_back({"app","public","noise_proc_"+std::to_string(i),0,1,true,false});
      }
      large.tables.push_back({"app","archive","audit_snapshot_rollup",{{"id",ValueType::Integer}}});
      large.tables.push_back({"other","archive","audit_snapshot_secret",{}});
      large.routines.push_back({"app","archive","refresh_audit",1,1,false,false});
      large.routines.push_back({"app","archive","rebuild_rollups",1,1,true,false});
      large.tables.push_back({"app","public","noise",{}});
      large.routines.push_back({"app","public","noise_fn",1,1,false,false});
      large.routines.push_back({"app","public","noise_proc",1,1,true,false});
      CompletionOptions options;
      options.maxCatalogEntries=8;
      const auto request=[&](std::string sql) {
        return completeSql(pg,sql,sql.size(),&large,{"app","public",pg},options);
      };
      const auto table=request("SELECT * FROM archive.auditsnap");
      expect(table,"audit_snapshot_rollup",CompletionKind::Table);
      check(!find(table,"audit_snapshot_secret",CompletionKind::Table),
            "typed catalog search crossed the current database");
      expect(request("SELECT * FROM archive."),"audit_snapshot_rollup",CompletionKind::Table);
      const auto qualified=request("WITH audit_snapshot_cte AS (SELECT 1 AS id) SELECT * FROM archive.auditsnap");
      expect(qualified,"audit_snapshot_rollup",CompletionKind::Table);
      check(!find(qualified,"audit_snapshot_cte",CompletionKind::Table),"CTE was suggested under a schema qualifier");
      expect(request("SELECT archive.refresh_a"),"refresh_audit",CompletionKind::Function);
      expect(request("CALL archive.rebuild_"),"rebuild_rollups",CompletionKind::Procedure);
      const auto exactTable=request("SELECT * FROM noise");
      expect(exactTable,"noise",CompletionKind::Table);
      check(exactTable.items.front().label=="noise","exact table match lost priority after reaching the catalog cap");
      expect(request("SELECT noise_fn"),"noise_fn",CompletionKind::Function);
      expect(request("CALL noise_proc"),"noise_proc",CompletionKind::Procedure);
      const auto later=request("CREATE TABLE archive.audit_snapshot_local(id int); SELECT * FROM archive.auditsnap");
      expect(later,"audit_snapshot_local",CompletionKind::Table);
      expect(later,"audit_snapshot_rollup",CompletionKind::Table);
      const auto dropped=request("DROP TABLE archive.audit_snapshot_rollup; SELECT * FROM archive.auditsnap");
      check(!find(dropped,"audit_snapshot_rollup",CompletionKind::Table),
            "completion filtered before applying the script's DROP TABLE");
    }
    {
      for (auto dialect:{Dialect::PostgreSQL,Dialect::MySQL,Dialect::MariaDB,Dialect::TiDB,
                         Dialect::OceanBase,Dialect::Oracle,Dialect::SQLite,Dialect::SQLServer}) {
        const auto then=complete(dialect,"SELECT CASE WHEN 2>1 TH|");
        expect(then,"THEN",CompletionKind::Keyword);
        check(then.items.front().label=="THEN","CASE THEN was not ranked first");
        const auto elseArm=complete(dialect,"SELECT CASE 1 WHEN 1 THEN 2 EL|");
        expect(elseArm,"ELSE",CompletionKind::Keyword);
        expect(complete(dialect,"SELECT CASE WHEN 2>1 THEN 4 ELSE 2 E|"),"END",CompletionKind::Keyword);
        const auto partial=complete(dialect,"SELECT CASE WHEN 3 BETWEEN 1 TH|");
        check(!find(partial,"THEN",CompletionKind::Keyword),"CASE suggested THEN before BETWEEN upper bound");
      }
      const auto pg=Dialect::PostgreSQL;
      auto catalog=fixture();
      const auto scope=complete(pg,"SELECT CASE WHEN id>0 THEN dis| FROM users",&catalog);
      expect(scope,"DisplayName",CompletionKind::Column);
      check(scope.parsedScope,"incomplete CASE discarded its SELECT scope");
      expect(complete(pg,"SELECT CASE WHEN id>0 THEN CASE WHEN id<3 THEN dis| FROM users",&catalog),
             "DisplayName",CompletionKind::Column);
      expect(complete(pg,"SELECT CASE WHEN id BETWEEN 1 AND 3 TH| FROM users",&catalog),"THEN",CompletionKind::Keyword);
      const auto alreadyThen=complete(pg,"SELECT CASE WHEN id>1 | THEN 2 END FROM users",&catalog);
      check(!find(alreadyThen,"THEN",CompletionKind::Keyword),"CASE suggested duplicating the following THEN");
      expect(complete(Dialect::MySQL,"DELIMITER $$\nCREATE FUNCTION f(_id INT) RETURNS INT "
                      "BEGIN RETURN CASE WHEN _id>0 THEN _i| ELSE 0 END; END$$"),
             "_id",CompletionKind::Variable);
    }
    {
      const auto ora=Dialect::Oracle, ms=Dialect::SQLServer;
      const auto aggregates=complete(ora,"SELECT * FROM t PIVOT (co|");
      expect(aggregates,"COUNT",CompletionKind::Function);
      const auto *count=find(aggregates,"COUNT",CompletionKind::Function);
      check(count->insertText=="COUNT(*)" && count->selection==1,"Oracle PIVOT count did not select its argument");
      expect(complete(ms,"SELECT * FROM t PIVOT (count_b|"),"COUNT_BIG",CompletionKind::Function);
      const std::string oraclePivot="SELECT * FROM (SELECT 1 amount,2 sales_year FROM SYS.DUAL) PIVOT ";
      expect(complete(ora,oraclePivot+"(SUM(am|"),"AMOUNT",CompletionKind::Column);
      expect(complete(ora,oraclePivot+"(SUM(amount) AS total FO|"),"FOR",CompletionKind::Keyword);
      expect(complete(ora,oraclePivot+"(SUM(amount) FOR sal|"),"SALES_YEAR",CompletionKind::Column);
      expect(complete(ora,oraclePivot+"(SUM(amount) FOR sales_year I|"),"IN",CompletionKind::Keyword);
      const std::string tsqlPivot="SELECT * FROM (VALUES (1,2,2025)) s(user_id,amount,sales_year) PIVOT ";
      expect(complete(ms,tsqlPivot+"(SUM(a|) FOR sales_year IN ([2025])) p"),"amount",CompletionKind::Column);
      expect(complete(ms,tsqlPivot+"(SUM(amount) FOR s|;"),"sales_year",CompletionKind::Column);
      check(!find(complete(Dialect::PostgreSQL,"SELECT * FROM t PIVOT (count_b|"),
                  "COUNT_BIG",CompletionKind::Function),"SQL Server PIVOT completion leaked into PostgreSQL");
      expect(complete(ora,"SELECT * FROM sales UNPIVOT INC|"),"INCLUDE NULLS",CompletionKind::Keyword);
      expect(complete(ora,"SELECT * FROM sales UNPIVOT EXCLUDE NU|"),"NULLS",CompletionKind::Keyword);
      expect(complete(ora,"SELECT * FROM sales UNPIVOT INCLUDE NULLS (total FO|"),"FOR",CompletionKind::Keyword);
      expect(complete(ms,"SELECT * FROM sales s UNPIVOT (total FOR quarter I|"),"IN",CompletionKind::Keyword);
      const auto oracle=complete(ora,"SELECT u.qu| FROM (SELECT 1 id,2 q1,3 q2 FROM SYS.DUAL) "
                                    "UNPIVOT (total FOR quarter IN (q1,q2)) u");
      expect(oracle,"QUARTER",CompletionKind::Column);
      const auto server=complete(ms,"SELECT u.| FROM (VALUES (1,2,3)) s(id,q1,q2) "
                                    "UNPIVOT (total FOR quarter IN (q1,q2)) AS u");
      expect(server,"total",CompletionKind::Column);
      expect(server,"id",CompletionKind::Column);
      check(!find(server,"q1",CompletionKind::Column),"UNPIVOT output suggested a consumed input column");
      const auto input=complete(ora,"SELECT * FROM (SELECT 1 id,2 q1,3 q2 FROM SYS.DUAL) "
                                   "UNPIVOT INCLUDE NULLS (total FOR quarter IN (q1 AS 'first', q|");
      expect(input,"Q2",CompletionKind::Column);
      check(!find(input,"Q1",CompletionKind::Column) && !find(input,"QUARTER",CompletionKind::Column),
            "Oracle UNPIVOT input suggested a used column or an output descriptor");
      const auto middle=complete(ms,"SELECT * FROM (VALUES (1,2,3)) s([Item ID],q1,q2) "
                                    "UNPIVOT (total FOR quarter IN ([It|],q2)) u");
      expect(middle,"Item ID",CompletionKind::Column);
      const auto incomplete=complete(ms,"SELECT * FROM (VALUES (1,2,3)) s(id,q1,q2) "
                                        "UNPIVOT (total FOR quarter IN (q|;");
      expect(incomplete,"q2",CompletionKind::Column);
      const auto all=complete(ms,"SELECT * FROM (VALUES (1,2,3)) s(id,q1,q2) "
                                 "UNPIVOT (total FOR quarter IN (|,q2)) u");
      expect(all,"q1",CompletionKind::Column);
      check(!find(all,"q2",CompletionKind::Column) && !find(all,"total",CompletionKind::Column),
            "UNPIVOT input suggested a later column or an output measure");
      expect(complete(ora,"SELECT * FROM sales UNPIVOT ((total,units) FO|"),"FOR",CompletionKind::Keyword);
      expect(complete(ora,"SELECT * FROM sales UNPIVOT ((total,units) FOR (quarter,year_no) I|"),"IN",CompletionKind::Keyword);
      const std::string multi="SELECT * FROM (SELECT 1 id,2 q1,3 q2,4 v1,5 v2 FROM SYS.DUAL) "
                              "UNPIVOT ((total,units) FOR (quarter,year_no) IN ";
      expect(complete(ora,multi+"((q1,v1) AS ('first',2026),(q2,v|"),"V2",CompletionKind::Column);
      const auto reuse=complete(ora,multi+"((q1,v1) AS ('first',2026),(q2,|) AS ('second',2027))) u");
      expect(reuse,"V1",CompletionKind::Column);
      check(!find(reuse,"Q2",CompletionKind::Column),"UNPIVOT tuple suggested an already listed field");
      expect(complete(ora,"SELECT u.ye| FROM (SELECT 1 q1,2 v1 FROM SYS.DUAL) "
                          "UNPIVOT ((total,units) FOR (quarter,year_no) IN ((q1,v1) AS ('first',2026))) u"),
             "YEAR_NO",CompletionKind::Column);
      check(!find(complete(Dialect::PostgreSQL,"SELECT * FROM sales UNPIVOT INC|"),
                  "INCLUDE NULLS",CompletionKind::Keyword),"Oracle UNPIVOT completion leaked into PostgreSQL");
      CompletionOptions calcite;
      calcite.language.calciteExtensions=true;
      const std::string inputList="SELECT * FROM (VALUES (1,2,3)) s(id,q1,q2) "
                                  "UNPIVOT INCLUDE NULLS (total FOR label IN (q1 AS 'used', q";
      expect(completeSql(Dialect::PostgreSQL,inputList,inputList.size(),nullptr,{},calcite),"q2",CompletionKind::Column);
      const std::string mode="SELECT * FROM sales UNPIVOT EXC";
      expect(completeSql(Dialect::PostgreSQL,mode,mode.size(),nullptr,{},calcite),"EXCLUDE NULLS",CompletionKind::Keyword);
      const std::string tuple="SELECT * FROM (VALUES (1,2,3,4,5)) s(id,q1,v1,q2,v2) "
          "UNPIVOT ((total,units) FOR (label,period) IN ((q1,v1),(q2,q";
      const auto tupleFields=completeSql(Dialect::PostgreSQL,tuple,tuple.size(),nullptr,{},calcite);
      expect(tupleFields,"q1",CompletionKind::Column);
      check(!find(tupleFields,"q2",CompletionKind::Column),"Calcite UNPIVOT suggested a duplicate within one tuple");
      const std::string pivot="SELECT * FROM (VALUES (1,2,3)) s(id,q1,q2) PIVOT ";
      const auto completePivot=[&](std::string text) {
        return completeSql(Dialect::PostgreSQL,text,text.size(),nullptr,{},calcite);
      };
      expect(completePivot(pivot+"(co"),"COUNT",CompletionKind::Function);
      expect(completePivot(pivot+"(MIN(q"),"q1",CompletionKind::Column);
      expect(completePivot(pivot+"(COUNT(*) AS n FO"),"FOR",CompletionKind::Keyword);
      expect(completePivot(pivot+"(COUNT(*) FOR q"),"q2",CompletionKind::Column);
      expect(completePivot(pivot+"(MIN(q1) FOR (q"),"q2",CompletionKind::Column);
      expect(completePivot(pivot+"(MIN(q1) FOR (q2) I"),"IN",CompletionKind::Keyword);
      const auto composite=completePivot(pivot+"(MIN(q1) FOR (q1,q");
      expect(composite,"q2",CompletionKind::Column);
      check(!find(composite,"q1",CompletionKind::Column),"Calcite PIVOT suggested a repeated axis");
      expect(completePivot(pivot+"(MIN(q1) FOR (id,q2) I"),"IN",CompletionKind::Keyword);
    }
    {
      const auto catalog=fixture();
      const auto pg=Dialect::PostgreSQL;
      const auto suggested=complete(pg,"SELECT mo| FROM users",&catalog);
      expect(suggested,"MODE",CompletionKind::Function);
      const auto *mode=find(suggested,"MODE",CompletionKind::Function);
      check(mode->insertText=="mode() WITHIN GROUP (ORDER BY )" && mode->cursor+1==mode->insertText.size(),
            "MODE completion did not place the cursor in the ordering clause");
      expect(complete(pg,"SELECT pg_catalog.mo| FROM users",&catalog),"MODE",CompletionKind::Function);
      expect(complete(pg,"SELECT MODE() WI|"),"WITHIN GROUP",CompletionKind::Keyword);
      expect(complete(pg,"SELECT MODE() WITHIN G|"),"GROUP",CompletionKind::Keyword);
      expect(complete(pg,"SELECT MODE() WITHIN GROUP (ORD|"),"ORDER BY",CompletionKind::Keyword);
      expect(complete(pg,"SELECT MODE() WITHIN GROUP (ORDER B|"),"BY",CompletionKind::Keyword);
      const auto incomplete=complete(pg,"SELECT MODE() WITHIN GROUP (ORDER BY u.na| FROM public.users u",&catalog);
      expect(incomplete,"name",CompletionKind::Column);
      check(incomplete.parsedScope,"unterminated WITHIN GROUP lost its parsed table scope");
      const auto fraction=complete(pg,"SELECT percentile_di| FROM users",&catalog);
      expect(fraction,"PERCENTILE_DISC",CompletionKind::Function);
      const auto *percentile=find(fraction,"PERCENTILE_DISC",CompletionKind::Function);
      check(percentile->selection==3 && percentile->insertText.substr(percentile->cursor,3)=="0.5" &&
            percentile->insertText.find("WITHIN GROUP (ORDER BY )")!=std::string::npos,
            "percentile suggestion did not select the editable fraction");
      expect(complete(pg,"SELECT percentile_cont((0.5)) WI|"),"WITHIN GROUP",CompletionKind::Keyword);
    }
    {
      MemoryCatalog mysqlCatalog;
      mysqlCatalog.tables.push_back({"app","app","source_row",
                                    {{"account_id",ValueType::Integer},{"display_name",ValueType::String}}});
      mysqlCatalog.tables.push_back({"app","app","audit_row",{{"audit_text",ValueType::String}}});
      for (const auto dialect:{Dialect::MySQL,Dialect::MariaDB}) {
        const std::string header="CREATE TRIGGER audit_row_trg ";
        const auto prefix=header+"BEFORE UPDATE ON app.source_row FOR EACH ROW ";
        expect(complete(dialect,prefix+"SET NEW.dis|",&mysqlCatalog),"display_name",CompletionKind::Column);
        const auto read=complete(dialect,prefix+"SET NEW.display_name = OLD.di|",&mysqlCatalog);
        expect(read,"display_name",CompletionKind::Column);
        check(!find(read,"audit_text",CompletionKind::Column),"trigger candidate leaked a column from a different table");
        check(!find(complete(dialect,header+"BEFORE INSERT ON source_row FOR EACH ROW SET @v = OLD.di|",&mysqlCatalog),
                    "display_name",CompletionKind::Column),"INSERT trigger exposed OLD");
        check(!find(complete(dialect,header+"AFTER DELETE ON source_row FOR EACH ROW SET @v = NEW.di|",&mysqlCatalog),
                    "display_name",CompletionKind::Column),"DELETE trigger exposed NEW");
        check(!find(complete(dialect,header+"AFTER UPDATE ON source_row FOR EACH ROW SET NEW.di|",&mysqlCatalog),
                    "display_name",CompletionKind::Column),"AFTER trigger offered an UPDATE to NEW");
        expect(complete(dialect,header+"AFTER UPDATE ON source_row FOR EACH ROW SET @v = NEW.di|",&mysqlCatalog),
               "display_name",CompletionKind::Column);
        expect(complete(dialect,"DELIMITER $$\n"+prefix+
                        "BEGIN SET @v = CASE WHEN OLD.account_id=0 THEN 1 ELSE 2 END; SET NEW.display_|",
                        &mysqlCatalog),"display_name",CompletionKind::Column);
        check(!find(complete(dialect,prefix+"BEGIN SET @v = OLD.display_name; END; SELECT NEW.di|",
                            &mysqlCatalog),"display_name",CompletionKind::Column),"trigger scope escaped END");
      }
      expect(complete(Dialect::MySQL,"CREATE TABLE source_row (id INT, text_name VARCHAR(20)); "
                      "CREATE DEFINER=CURRENT_USER TRIGGER tr BEFORE UPDATE ON source_row FOR EACH ROW SET NEW.te|"),
             "text_name",CompletionKind::Column);
      MemoryCatalog sqliteCatalog;
      sqliteCatalog.tables.push_back({"main","main","source_row",{{"display_name",ValueType::String}}});
      const auto sqliteCompletion=[&](std::string sql) {
        const auto cursor=sql.find('|');
        sql.erase(cursor,1);
        return completeSql(Dialect::SQLite,std::move(sql),cursor,&sqliteCatalog,{"main","main",Dialect::SQLite});
      };
      expect(sqliteCompletion("CREATE TRIGGER tr AFTER UPDATE ON source_row WHEN NEW.di|"),
             "display_name",CompletionKind::Column);
      expect(sqliteCompletion("CREATE TRIGGER tr AFTER UPDATE ON main.source_row "
                              "BEGIN SELECT 'NOT END'; SELECT OLD.di|; END;"),
             "display_name",CompletionKind::Column);
      check(!find(sqliteCompletion("CREATE TRIGGER tr AFTER INSERT ON source_row BEGIN SELECT OLD.di|"),
                  "display_name",CompletionKind::Column),"SQLite INSERT trigger exposed OLD");
    }
    {
      const auto catalog = fixture();
      const auto pg = Dialect::PostgreSQL;
      const auto alterations=complete(pg,"ALTER TABLE public.users |",&catalog);
      expect(alterations,"ALTER COLUMN",CompletionKind::Keyword);
      expect(alterations,"RENAME COLUMN",CompletionKind::Keyword);
      check(!find(alterations,"orders",CompletionKind::Table),"ALTER action suggested another table");
      const auto dropColumn=complete(pg,"ALTER TABLE IF EXISTS public.users DROP COLUMN IF EXISTS |",&catalog);
      expect(dropColumn,"name",CompletionKind::Column);
      check(!find(dropColumn,"amount",CompletionKind::Column),"ALTER column escaped the target table");
      expect(complete(pg,"ALTER TABLE public.users DROP COLUMN IF EX|",&catalog),"EXISTS",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE I|",&catalog),"IF EXISTS",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users ADD COLUMN I|",&catalog),"IF NOT EXISTS",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users ADD IF N|",&catalog),"NOT EXISTS",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users ADD IF NOT E|",&catalog),"EXISTS",CompletionKind::Keyword);
      check(!find(complete(pg,"ALTER TABLE public.users ADD IF NOT EXISTS |",&catalog),
                  "id",CompletionKind::Column),"guarded declaration suggested an existing column");
      expect(complete(pg,"ALTER TABLE public.users ADD IF NOT EXISTS note TE|",&catalog),"TEXT",CompletionKind::Type);
      expect(complete(pg,"ALTER TABLE public.users ALTER name SET D|",&catalog),"DATA TYPE",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users ALTER name SET DATA T|",&catalog),"TYPE",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users ALTER name SET DATA TYPE TE|",&catalog),"TEXT",CompletionKind::Type);
      expect(complete(pg,"ALTER TABLE public.users RENAME na|",&catalog),"name",CompletionKind::Column);
      expect(complete(pg,"ALTER TABLE public.users RENAME name T|",&catalog),"TO",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users ALTER COLUMN name SET N|",&catalog),"NOT NULL",CompletionKind::Keyword);
      expect(complete(pg,"ALTER TABLE public.users RENAME COLUMN name T|",&catalog),"TO",CompletionKind::Keyword);
      check(!find(complete(pg,"ALTER TABLE public.users RENAME COLUMN name TO |",&catalog),
                  "id",CompletionKind::Column),"rename destination suggested an existing column");
      check(!find(complete(pg,"ALTER TABLE public.users ADD COLUMN |",&catalog),
                  "id",CompletionKind::Column),"new column suggested an existing column");
      expect(complete(pg,"ALTER TABLE public.users ADD COLUMN note TE|",&catalog),"TEXT",CompletionKind::Type);
      const auto subsequent=complete(pg,"ALTER TABLE public.users ADD COLUMN note TEXT, DROP COLUMN name, "
                                        "ALTER COLUMN |",&catalog);
      expect(subsequent,"note",CompletionKind::Column);
      check(!find(subsequent,"name",CompletionKind::Column),"a column dropped earlier in ALTER remained visible");
      const auto guardedChanges=complete(pg,"ALTER TABLE public.users ADD IF NOT EXISTS note TEXT, "
                                           "DROP IF EXISTS name, ALTER COLUMN |",&catalog);
      expect(guardedChanges,"note",CompletionKind::Column);
      check(!find(guardedChanges,"name",CompletionKind::Column),"a guarded drop did not update the statement scope");
      const auto my=Dialect::MySQL;
      for (const auto dialect:{Dialect::MariaDB,Dialect::TiDB}) {
        expect(complete(dialect,"ALTER TABLE app.users ADD COLUMN I|",&catalog),"IF NOT EXISTS",CompletionKind::Keyword);
        expect(complete(dialect,"ALTER TABLE app.users DROP IF EX|",&catalog),"EXISTS",CompletionKind::Keyword);
      }
      expect(complete(Dialect::MariaDB,"ALTER TABLE IF E|",&catalog),"EXISTS",CompletionKind::Keyword);
      check(!find(complete(my,"ALTER TABLE app.users ADD COLUMN I|",&catalog),"IF NOT EXISTS",CompletionKind::Keyword),
            "MySQL offered a MariaDB column guard");
      expect(complete(my,"ALTER TABLE app.users CHANGE COLUMN na|",&catalog),"name",CompletionKind::Column);
      expect(complete(my,"ALTER TABLE app.users MODIFY na| VARCHAR(20)",&catalog),"name",CompletionKind::Column);
      const auto ms=Dialect::SQLServer;
      const auto reservedSchema=complete(ms,"SELECT * FROM pu|",&catalog);
      expect(reservedSchema,"public",CompletionKind::Schema);
      check(find(reservedSchema,"public",CompletionKind::Schema)->insertText=="[public].",
            "SQL Server reserved schema completion was not delimited");
      auto reservedCatalog=catalog;
      reservedCatalog.tables.front().columns.push_back({"fillfactor",ValueType::Integer});
      const auto reservedColumn=complete(ms,"SELECT fi| FROM [public].users",&reservedCatalog);
      expect(reservedColumn,"fillfactor",CompletionKind::Column);
      check(find(reservedColumn,"fillfactor",CompletionKind::Column)->insertText=="[fillfactor]",
            "SQL Server reserved column completion was not delimited");
      const auto sessionValue=complete(ms,"SELECT CURRENT_U|");
      expect(sessionValue,"CURRENT_USER",CompletionKind::Function);
      check(find(sessionValue,"CURRENT_USER",CompletionKind::Function)->insertText=="CURRENT_USER",
            "SQL Server value keyword completion added invalid parentheses");
      const auto keywordCall=complete(ms,"SELECT COAL|");
      expect(keywordCall,"COALESCE",CompletionKind::Function);
      check(find(keywordCall,"COALESCE",CompletionKind::Function)->insertText=="COALESCE()",
            "SQL Server builtin function completion was quoted as an identifier");
      expect(complete(ms,"ALTER TABLE app.[public].users ALTER COLUMN na|",&catalog),"name",CompletionKind::Column);
      expect(complete(ms,"ALTER TABLE app.[public].users ALTER COLUMN name NV|",&catalog),"NVARCHAR",CompletionKind::Type);
      const auto output=complete(ms,"INSERT INTO [public].users(id) OUTPUT ins|",&catalog);
      expect(output,"inserted",CompletionKind::Alias);
      check(find(output,"inserted",CompletionKind::Alias)->insertText=="inserted.",
            "OUTPUT row alias did not enter the member slot");
      expect(complete(ms,"INSERT INTO [public].users(id) OUTPUT INSERTED.na| VALUES(1)",&catalog),
             "name",CompletionKind::Column);
      expect(complete(ms,"INSERT INTO [public].users(id) OUTPUT inserted.[Dis|]",&catalog),
             "DisplayName",CompletionKind::Column);
      check(!find(complete(ms,"INSERT INTO [public].users(id) OUTPUT deleted.na| VALUES(1)",&catalog),
                  "name",CompletionKind::Column),"INSERT OUTPUT exposed DELETED");
      const auto qualifiedOutput=complete(ms,"DELETE FROM [public].users OUTPUT na|",&catalog);
      expect(qualifiedOutput,"deleted.name",CompletionKind::Column);
      check(!find(qualifiedOutput,"name",CompletionKind::Column),"DELETE OUTPUT suggested a bare target column");
      expect(complete(ms,"DELETE FROM [public].users OUTPUT deleted.na| WHERE id=1",&catalog),
             "name",CompletionKind::Column);
      check(!find(complete(ms,"DELETE FROM [public].users OUTPUT inserted.na| WHERE id=1",&catalog),
                  "name",CompletionKind::Column),"DELETE OUTPUT exposed INSERTED");
      const std::string outputTail=" FROM [public].users u JOIN [public].orders o ON o.user_id=u.id";
      expect(complete(ms,"UPDATE u SET u.name='edited' OUTPUT inserted.na|"+outputTail,&catalog),
             "name",CompletionKind::Column);
      expect(complete(ms,"UPDATE u SET u.name='edited' OUTPUT o.am|"+outputTail,&catalog),
             "amount",CompletionKind::Column);
      check(!find(complete(ms,"UPDATE u SET u.name='edited' OUTPUT u.na|"+outputTail,&catalog),
                  "name",CompletionKind::Column),"UPDATE OUTPUT exposed target alias directly");
      check(!find(complete(ms,"UPDATE [public].users SET name='edited' OUTPUT inserted.name "
                             "WHERE inserted.na|='edited'",&catalog),
                  "name",CompletionKind::Column),"OUTPUT pseudo-row escaped into WHERE");
      expect(complete(ms,"CREATE TABLE scratch (own_value INT); INSERT INTO scratch OUTPUT inserted.own|"),
             "own_value",CompletionKind::Column);
      auto auditCatalog=catalog;
      auditCatalog.tables.push_back({"app","public","audit_log",{{"item_id",ValueType::Integer},
          {"old_name",ValueType::String},{"new_name",ValueType::String}},true,true});
      const auto stored=std::string("UPDATE [public].users SET name='edited' OUTPUT deleted.name,inserted.name INTO ");
      expect(complete(ms,stored+"[public].aud|",&auditCatalog),"audit_log",CompletionKind::Table);
      expect(complete(ms,stored+"[public].audit_log (old_|)",&auditCatalog),"old_name",CompletionKind::Column);
      const auto sinkColumns=complete(ms,stored+"[public].audit_log (item_id, |, new_name)",&auditCatalog);
      expect(sinkColumns,"old_name",CompletionKind::Column);
      check(!find(sinkColumns,"item_id",CompletionKind::Column) && !find(sinkColumns,"new_name",CompletionKind::Column),
            "OUTPUT INTO repeated a target column already listed");
      expect(complete(ms,"INSERT INTO [public].users OUTPUT inserted.na| INTO [public].audit_log(old_name) VALUES(1)",&auditCatalog),
             "name",CompletionKind::Column);
      expect(complete(ms,stored+"[public].audit_log (old_name,new_name) OUTPUT inserted.na|",&auditCatalog),
             "name",CompletionKind::Column);
      expect(complete(ms,"CREATE TABLE #audit (old_name VARCHAR(20)); "
                         "DELETE FROM [public].users OUTPUT deleted.name INTO #audit (old_|",&auditCatalog),
             "old_name",CompletionKind::Column);
      const std::string merge="MERGE INTO [public].users u USING [public].orders o ON u.id=o.user_id "
                              "WHEN MATCHED THEN UPDATE SET u.name='updated' "
                              "WHEN NOT MATCHED THEN INSERT (id,name) VALUES (o.id,'new') OUTPUT ";
      const auto action=complete(ms,merge+"$ac|",&auditCatalog);
      expect(action,"$action",CompletionKind::Column);
      check(find(action,"$action",CompletionKind::Column)->replacement.begin==merge.size(),
            "MERGE completion did not replace the partial dollar-prefixed value");
      expect(complete(ms,merge+"inserted.na|",&auditCatalog),"name",CompletionKind::Column);
      expect(complete(ms,merge+"deleted.na|;",&auditCatalog),"name",CompletionKind::Column);
      expect(complete(ms,merge+"o.am|;",&auditCatalog),"amount",CompletionKind::Column);
      expect(complete(ms,merge+"deleted.name INTO [public].audit_log(old_|",&auditCatalog),
             "old_name",CompletionKind::Column);
      check(!find(complete(ms,"INSERT INTO [public].users OUTPUT $ac|",&auditCatalog),
                  "$action",CompletionKind::Column),"MERGE pseudo-column was suggested in INSERT");
      const auto ora=Dialect::Oracle;
      MemoryCatalog oracleCatalog;
      oracleCatalog.tables.push_back({"app","PUBLIC","USERS",{{"ID",ValueType::Integer},{"NAME",ValueType::String}}});
      expect(complete(ora,"ALTER TABLE public.users MODIFY (name VARCHAR2(20), id|",&oracleCatalog),"ID",CompletionKind::Column);
      expect(complete(pg,"CREATE TABLE scratch (nickname TEXT); ALTER TABLE scratch DROP COLUMN ni|"),
             "nickname",CompletionKind::Column);
      const auto primary=complete(pg,"CREATE TABLE scratch (ticket_id int, site_id int, "
                                     "PRIMARY KEY (ticket_id, |",&catalog);
      expect(primary,"site_id",CompletionKind::Column);
      check(!find(primary,"ticket_id",CompletionKind::Column) && !find(primary,"amount",CompletionKind::Column),
            "PRIMARY KEY suggestions repeated a field or used an unrelated table");
      const auto foreign=complete(pg,"CREATE TABLE scratch (child_id int, owner_id int, "
                                     "CONSTRAINT fk FOREIGN KEY (o|) REFERENCES public.users(id))",&catalog);
      expect(foreign,"owner_id",CompletionKind::Column);
      const auto unique=complete(pg,"CREATE TABLE scratch (alpha int, beta int, gamma int, "
                                    "UNIQUE (alpha, |, gamma))");
      expect(unique,"beta",CompletionKind::Column);
      check(!find(unique,"gamma",CompletionKind::Column),"UNIQUE completion repeated a later field");
      expect(complete(my,"CREATE TEMPORARY TABLE app.scratch (item_id INT, unit_cost DECIMAL(10,2), "
                         "PRIMARY KEY (itemid|"),"item_id",CompletionKind::Column);
      expect(complete(ms,"CREATE TABLE dbo.scratch ([Item ID] INT, CONSTRAINT pk PRIMARY KEY ([It|"),
             "Item ID",CompletionKind::Column);
      expect(complete(ora,"CREATE TABLE scratch (local_code NUMBER(10), CONSTRAINT uq UNIQUE (LOCAL_|"),
             "LOCAL_CODE",CompletionKind::Column);
      expect(complete(Dialect::SQLite,"CREATE TABLE scratch (local_code INTEGER, UNIQUE (loc|"),
             "local_code",CompletionKind::Column);
      for (const auto dialect:{pg,my,ms,ora,Dialect::SQLite}) {
        const auto constraint=complete(dialect,"CREATE TABLE scratch (minimum_count INTEGER, max_count INTEGER, "
                                               "CONSTRAINT bounds CHECK ((minimum_count <= maxc|",&catalog);
        expect(constraint,dialect==ora?"MAX_COUNT":"max_count",CompletionKind::Column);
        if (dialect!=ora)
          check(!find(constraint,"amount",CompletionKind::Column),"CHECK completion leaked an unrelated table field");
      }
      expect(complete(pg,"ALTER TABLE public.orders ADD CONSTRAINT nonnegative CHECK (am|",&catalog),
             "amount",CompletionKind::Column);
      expect(complete(pg,"CREATE TABLE scratch (pay NUMERIC, fee NUMERIC CHECK (pa|"),
             "pay",CompletionKind::Column);
      auto insert = complete(pg, "INSERT INTO public.users (id, |) VALUES (1, 'name');", &catalog);
      expect(insert, "name", CompletionKind::Column);
      check(!find(insert, "id", CompletionKind::Column), "INSERT repeated a target column");
      check(std::all_of(insert.items.begin(), insert.items.end(), [](const auto &item) {
        return item.kind == CompletionKind::Column;
      }), "INSERT target list contains expression candidates");
      auto conflict=complete(pg,"INSERT INTO public.users AS u(id) SELECT user_id FROM orders "
                                "ON CONFLICT (id, na|",&catalog);
      expect(conflict,"name",CompletionKind::Column);
      check(!find(conflict,"user_id",CompletionKind::Column),"conflict inference offered source query columns");
      const auto middle=complete(pg,"INSERT INTO public.users(id) VALUES (1) "
                                    "ON CONFLICT (id, |, \"DisplayName\") DO NOTHING",&catalog);
      expect(middle,"name",CompletionKind::Column);
      check(middle.items.size()==1,"conflict inference repeated a column left or right of the cursor");
      expect(complete(pg,"CREATE TABLE scratch (account_id INT, nickname TEXT); "
                         "INSERT INTO scratch DEFAULT VALUES ON CONFLICT (ni|"),"nickname",CompletionKind::Column);
      MemoryCatalog sqliteCatalog;
      auto sqliteTable=catalog.tables.front();
      sqliteTable.database=sqliteTable.schema="main";
      sqliteCatalog.tables.push_back(std::move(sqliteTable));
      const std::string sqliteInsert="INSERT INTO main.users(id) VALUES(1) ON CONFLICT (na";
      expect(completeSql(Dialect::SQLite,sqliteInsert,sqliteInsert.size(),&sqliteCatalog,
                         {"main","main",Dialect::SQLite}),"name",CompletionKind::Column);
      const std::string upsert="INSERT INTO public.users AS u(id) SELECT user_id FROM orders "
                               "ON CONFLICT (id) DO UPDATE SET ";
      const auto assignments=complete(pg,upsert+"id=excluded.id, |",&catalog);
      expect(assignments,"name",CompletionKind::Column);
      check(!find(assignments,"id",CompletionKind::Column) && !find(assignments,"user_id",CompletionKind::Column),
            "upsert assignment candidates include an assigned or source column");
      const auto assignmentMiddle=complete(pg,upsert+"|=excluded.name, \"DisplayName\"='x'",&catalog);
      expect(assignmentMiddle,"name",CompletionKind::Column);
      check(!find(assignmentMiddle,"DisplayName",CompletionKind::Column),"upsert repeated a later assignment");
      expect(complete(pg,upsert+"name=excluded.na|",&catalog),"name",CompletionKind::Column);
      const auto mysql=Dialect::MySQL;
      expect(complete(mysql,"INSERT INTO app.users (id) VALUES (1) ON DUPLICATE KEY UPDATE users.na|",
                      &catalog),"name",CompletionKind::Column);
      const std::string sqliteAssignment=sqliteInsert.substr(0,sqliteInsert.size()-2)+
                                         "id) DO UPDATE SET na";
      expect(completeSql(Dialect::SQLite,sqliteAssignment,sqliteAssignment.size(),&sqliteCatalog,
                         {"main","main",Dialect::SQLite}),"name",CompletionKind::Column);
      auto update = complete(pg, "UPDATE public.users u SET id=COALESCE(1,2), n|= 'name' FROM orders;", &catalog);
      expect(update, "name", CompletionKind::Column);
      check(!find(update,"amount",CompletionKind::Column), "UPDATE SET column came from FROM");
      auto updateContext = complete(pg, "WITH src AS (SELECT 1 AS n) UPDATE public.users SET |", &catalog);
      expect(updateContext,"name",CompletionKind::Column);
      auto tsqlTarget = complete(Dialect::SQLServer,
          "UPDATE u SET n| = '' FROM [public].users AS u JOIN [public].orders AS o ON u.id=o.id;", &catalog);
      expect(tsqlTarget, "name", CompletionKind::Column);
      check(!find(tsqlTarget, "amount", CompletionKind::Column), "UPDATE alias resolved to the wrong side");
      auto insertMiddle = complete(pg, R"(INSERT INTO public.users (id, |, "DisplayName") VALUES (1, 'n', 'd');)", &catalog);
      expect(insertMiddle, "name", CompletionKind::Column);
      check(insertMiddle.items.size() == 1, "INSERT repeated a target column after the cursor");
      expect(complete(pg, "SELECT public.users.n| FROM public.users;", &catalog), "name", CompletionKind::Column);
      check(!find(complete(pg, "SELECT other.u.n| FROM public.users AS u;", &catalog), "name", CompletionKind::Column),
            "a partially matching qualifier leaked columns");
      check(!find(complete(pg, "SELECT public.users.n| FROM public.users AS u;", &catalog), "name", CompletionKind::Column),
            "an alias did not hide the physical relation name");
      auto literal = complete(pg, "UPDATE public.users SET id=COAL|;", &catalog);
      expect(literal, "COALESCE", CompletionKind::Function);
      auto quote = complete(pg, R"(INSERT INTO public.users ("Disp|") VALUES ('name');)", &catalog);
      const auto *column = find(quote,"DisplayName",CompletionKind::Column);
      check(column && column->insertText=="\"DisplayName\"", "quoted target column lost original spelling");
      check(!find(complete(pg,"INSERT INTO missing (|)",&catalog),"name",CompletionKind::Column),
            "unknown target borrowed unrelated column names");
    }
    {
      auto catalog = fixture();
      for (auto &table : catalog.tables)
        if (table.name == "orders") table.columns.push_back({"name", ValueType::String});
      const auto pg = Dialect::PostgreSQL;
      auto joined = complete(pg, "SELECT * FROM users u JOIN orders o USING (|", &catalog);
      expect(joined, "id", CompletionKind::Column);
      expect(joined, "name", CompletionKind::Column);
      check(joined.items.size() == 2, "USING suggested a non-common column or another kind");
      auto used = complete(pg, "SELECT * FROM users u JOIN orders o USING (id, |);", &catalog);
      expect(used, "name", CompletionKind::Column);
      check(used.items.size() == 1, "USING suggested an already used column");
      auto cte = complete(pg, "WITH x(pk) AS (SELECT id FROM users) "
                             "SELECT * FROM x JOIN (SELECT id AS pk FROM orders) y USING (p|);", &catalog);
      expect(cte, "pk", CompletionKind::Column);
      auto merged = complete(pg, "SELECT * FROM users u JOIN orders o USING(id) "
                              "JOIN users x USING (i|);", &catalog);
      expect(merged, "id", CompletionKind::Column);
      auto ambiguous = complete(pg, "SELECT * FROM users u JOIN orders o ON u.id=o.id "
                                 "JOIN users x USING (i|);", &catalog);
      check(!find(ambiguous, "id", CompletionKind::Column), "USING suggested an ambiguous name");
      auto missing = complete(pg, "SELECT * FROM users u JOIN unknown x USING (|);", &catalog);
      check(missing.items.empty(), "USING guessed a column from a missing table");
      auto mysql = complete(Dialect::MySQL, "SELECT * FROM users JOIN orders USING (|)", &catalog);
      expect(mysql, "id", CompletionKind::Column);
    }
    {
      const auto catalog = fixture();
      const auto pg = Dialect::PostgreSQL;
      auto order = complete(pg, "SELECT COUNT(*) AS total, id FROM users GROUP BY id ORDER BY to| DESC;", &catalog);
      expect(order, "total", CompletionKind::Column);
      auto group = complete(pg, "SELECT id AS person_id, COUNT(*) AS rows_n FROM users GROUP BY per|;", &catalog);
      expect(group, "person_id", CompletionKind::Column);
      check(!find(complete(pg, "SELECT COUNT(*) AS rows_n FROM users GROUP BY rows|;", &catalog),
                  "rows_n", CompletionKind::Column), "GROUP BY suggested an aggregate output");
      check(!find(complete(pg, "SELECT id AS person_id FROM users WHERE per|;", &catalog),
                  "person_id", CompletionKind::Column), "output alias leaked into WHERE");
      check(!find(complete(pg, "SELECT id AS n FROM users ORDER BY n| + 1;", &catalog),
                  "n", CompletionKind::Column), "bare PostgreSQL order alias leaked into a scalar expression");
      check(!find(complete(pg, "SELECT id AS shared, name AS shared FROM users ORDER BY sh|;", &catalog),
                  "shared", CompletionKind::Column), "an ambiguous output alias was suggested");
      auto set = complete(pg, "SELECT id AS person_id FROM users UNION ALL "
                             "SELECT user_id AS wrong_name FROM orders ORDER BY |;", &catalog);
      check(set.items.size() == 1, "compound ORDER BY suggested non-output names");
      expect(set, "person_id", CompletionKind::Column);
      expect(complete(Dialect::SQLServer, "SELECT id AS person_id FROM users ORDER BY per|;", &catalog),
             "person_id", CompletionKind::Column);
      check(!find(complete(Dialect::SQLServer, "SELECT id AS person_id FROM users GROUP BY per|;", &catalog),
                  "person_id", CompletionKind::Column), "SQL Server GROUP BY suggested a SELECT alias");
    }
    {
      const auto catalog = fixture();
      const auto pg = Dialect::PostgreSQL;
      auto inJoin = complete(pg, "SELECT * FROM users u JOIN orders o ON o.us|=u.id "
                               "JOIN (SELECT 1 AS future_id) x ON TRUE;", &catalog);
      expect(inJoin, "user_id", CompletionKind::Column);
      auto future = complete(pg, "SELECT * FROM users u JOIN orders o ON | "
                               "JOIN (SELECT 1 AS future_id) x ON TRUE;", &catalog);
      check(!find(future,"future_id",CompletionKind::Column) && !find(future,"x",CompletionKind::Alias),
            "JOIN ON suggested a relation from a later join");
      auto comma = complete(pg, "SELECT * FROM users before_join, "
                              "orders o JOIN users u ON before_join.n|=u.name;", &catalog);
      check(!find(comma,"name",CompletionKind::Column),
            "JOIN ON suggested the left side of a separate comma join");
      auto later = complete(pg, "SELECT * FROM users u JOIN orders o ON o.user_id=u.id "
                              "JOIN (SELECT 1 AS future_id) x ON x.fut|=o.id;", &catalog);
      expect(later,"future_id",CompletionKind::Column);
    }
    {
      const auto my = Dialect::MySQL;
      expect(complete(my, "CREATE ALGO|"),"ALGORITHM",CompletionKind::Keyword);
      expect(complete(my, "CREATE ALGORITHM = TEM|"),"TEMPTABLE",CompletionKind::Keyword);
      expect(complete(my, "CREATE OR REPLACE ALGORITHM=MERGE DEF|"),"DEFINER",CompletionKind::Keyword);
      expect(complete(my, "CREATE ALGORITHM=MERGE DEFINER = CUR|"),"CURRENT_USER",CompletionKind::Keyword);
      expect(complete(my, "CREATE ALGORITHM=UNDEFINED DEFINER='reader'@'localhost' SQL SEC|"),
             "SECURITY",CompletionKind::Keyword);
      auto security = complete(my, "CREATE SQL SECURITY |");
      expect(security,"INVOKER",CompletionKind::Keyword);
      expect(security,"DEFINER",CompletionKind::Keyword);
      check(security.items.size() == 2, "view security mode suggested unrelated expressions");
      expect(complete(my, "CREATE DEFINER=CURRENT_USER() FUN|"),"FUNCTION",CompletionKind::Keyword);
      expect(complete(Dialect::MariaDB, "CREATE SQL SECURITY INVOKER VI|"),"VIEW",CompletionKind::Keyword);
      check(!find(complete(Dialect::PostgreSQL,"CREATE ALGO|"),"ALGORITHM",CompletionKind::Keyword),
            "MySQL view header keyword leaked to PostgreSQL");
    }
    {
      auto catalog = fixture();
      catalog.tables.front().primaryKey = {"id"};
      catalog.tables.front().primaryKeyKnown = true;
      const auto pg = Dialect::PostgreSQL;
      auto reference = complete(pg, "CREATE TABLE people (owner_id int REFERENCES public.users(|", &catalog);
      expect(reference, "id", CompletionKind::Column);
      expect(reference, "name", CompletionKind::Column);
      check(reference.items.front().label == "id", "foreign-key completion did not prefer the primary key");
      check(!find(reference, "amount", CompletionKind::Column), "foreign-key completion used another table");
      auto more = complete(pg, "ALTER TABLE orders ADD FOREIGN KEY (user_id, amount) "
                             "REFERENCES public.users(id, n|);", &catalog);
      expect(more, "name", CompletionKind::Column);
      check(!find(more,"id",CompletionKind::Column), "foreign-key completion repeated a column");
      check(!find(complete(pg, "ALTER TABLE orders ADD FOREIGN KEY (user_id) REFERENCES public.unknown(|", &catalog),
                  "id", CompletionKind::Column), "foreign-key completion pulled from the altered table");
      expect(complete(Dialect::MySQL, "CREATE TABLE people (id int, "
                      "FOREIGN KEY (id) REFERENCES users(i|));", &catalog),"id",CompletionKind::Column);
    }
    {
      const auto my = Dialect::MySQL;
      expect(complete(my,"CREATE PROCEDURE I|"),"IF NOT EXISTS",CompletionKind::Keyword);
      expect(complete(my,"CREATE FUNCTION IF N|"),"NOT EXISTS",CompletionKind::Keyword);
      expect(complete(Dialect::MariaDB,"CREATE PROCEDURE IF NOT E|"),"EXISTS",CompletionKind::Keyword);
      expect(complete(my,"CREATE PROCEDURE p() COM|"),"COMMENT",CompletionKind::Keyword);
      expect(complete(my,"CREATE PROCEDURE p() LANGUAGE S|"),"SQL",CompletionKind::Keyword);
      expect(complete(my,"CREATE FUNCTION f(x INT) RETURNS INT READS SQL D|"),"DATA",CompletionKind::Keyword);
      expect(complete(my,"CREATE FUNCTION f() RETURNS INT SQL SECURITY IN|"),"INVOKER",CompletionKind::Keyword);
      expect(complete(Dialect::MariaDB,"CREATE PROCEDURE p() COMMENT 'demo' BE|"),"BEGIN",CompletionKind::Keyword);
      check(complete(my,"CREATE PROCEDURE p() COMMENT 'de|mo' SELECT 1;").suppressed,
            "routine header completion interfered with a comment string");
    }
    {
      const auto tidb = Dialect::TiDB, oceanbase = Dialect::OceanBase;
      expect(complete(tidb, "CREATE TABLE t(id BIGINT A|)"), "AUTO_RANDOM", CompletionKind::Keyword);
      expect(complete(tidb, "CREATE TABLE t(id BIGINT,PRIMARY KEY(id) C|)"), "CLUSTERED", CompletionKind::Keyword);
      expect(complete(tidb, "CREATE TABLE t(id BIGINT) PRE|"), "PRE_SPLIT_REGIONS", CompletionKind::Keyword);
      expect(complete(tidb, "ALTER TABLE t PLACEMENT P|"), "POLICY", CompletionKind::Keyword);
      expect(complete(tidb, "ADMIN SHOW DDL J|"), "JOBS", CompletionKind::Keyword);
      expect(complete(tidb, "SELECT TIDB_VER|"), "TIDB_VERSION", CompletionKind::Function);
      const auto create = complete(tidb, "CREATE |");
      check(!find(create,"FUNCTION",CompletionKind::Snippet) &&
            !find(create,"TRIGGER",CompletionKind::Keyword), "TiDB offered unsupported stored objects");
      expect(complete(oceanbase, "CREATE TABLE t(id INT) TAB|"), "TABLEGROUP", CompletionKind::Keyword);
      expect(complete(oceanbase, "CREATE INDEX k ON t(id) LO|"), "LOCAL", CompletionKind::Keyword);
      expect(complete(oceanbase, "SHOW TABLEG|"), "TABLEGROUPS", CompletionKind::Keyword);
      check(!find(complete(Dialect::MySQL, "CREATE TABLE t(id INT) TAB|"),
                  "TABLEGROUP", CompletionKind::Keyword), "OceanBase tablegroup leaked to MySQL");
    }
    for(auto dialect:{Dialect::PostgreSQL,Dialect::SQLite}) {
      auto r=complete(dialect,"SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE |)");
      expect(r,"CURRENT ROW",CompletionKind::Keyword);expect(r,"GROUP",CompletionKind::Keyword);expect(r,"TIES",CompletionKind::Keyword);expect(r,"NO OTHERS",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE NO |)"),"OTHERS",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE CURRENT |)"),"ROW",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT COUNT(*) OVER(ROWS UNBOUNDED PRECEDING E|)"),"EXCLUDE",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT COUNT(*) OVER w FROM t WINDOW w AS(ROWS UNBOUNDED PRECEDING EXCLUDE T|)"),"TIES",CompletionKind::Keyword);
    }
    for (const auto dialect:{Dialect::PostgreSQL,Dialect::SQLite,Dialect::MySQL,Dialect::MariaDB,
                             Dialect::SQLServer,Dialect::Oracle,Dialect::TiDB,Dialect::OceanBase}) {
      expect(complete(dialect,"SELECT SUM(amount) OVER (PA|"),"PARTITION BY",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT SUM(amount) OVER (PARTITION B|"),"BY",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT SUM(amount) OVER (PARTITION BY id O|"),"ORDER BY",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT SUM(amount) OVER (ORDER BY id ROWS BET|"),"BETWEEN",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT SUM(amount) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED P|"),"PRECEDING",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT SUM(amount) OVER (ORDER BY id ROWS BETWEEN 3 PRECEDING A|"),"AND",CompletionKind::Keyword);
      expect(complete(dialect,"SELECT SUM(amount) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED F|"),
             "FOLLOWING",CompletionKind::Keyword);
      auto catalog=fixture();
      if (dialect==Dialect::Oracle)
        catalog.tables.push_back({"app","PUBLIC","USERS",{{"NAME",ValueType::String}}});
      const auto relation=isMySqlDialect(dialect) || dialect==Dialect::SQLite?"users":
          dialect==Dialect::SQLServer?"[public].users":"public.users";
      const auto scope=complete(dialect,std::string("SELECT COUNT(*) OVER (PARTITION BY u.na| FROM ")+relation+" u",&catalog);
      check(find(scope,dialect==Dialect::Oracle?"NAME":"name",CompletionKind::Column) && scope.parsedScope,
            std::string("incomplete window lost source bindings in ")+dialectName(dialect));
    }
    {
      const auto pg=Dialect::PostgreSQL;
      const auto catalog=fixture();
      const auto direct=complete(pg,"SELECT COUNT(*) OVER fr| FROM public.users "
                                     "WINDOW framed AS (ORDER BY id ROWS UNBOUNDED PRECEDING)",&catalog);
      expect(direct,"framed",CompletionKind::Alias);
      check(!find(complete(pg,"SELECT COUNT(*) OVER (fr|) FROM public.users "
                              "WINDOW framed AS (ORDER BY id ROWS UNBOUNDED PRECEDING)",&catalog),
                  "framed",CompletionKind::Alias),"window copy offered a declaration with its own frame");
      const auto copied=complete(pg,"SELECT COUNT(*) OVER (| ORDER BY id) FROM public.users "
                                     "WINDOW shared AS (PARTITION BY id), ordered AS (ORDER BY id)",&catalog);
      expect(copied,"shared",CompletionKind::Alias);
      check(!find(copied,"ordered",CompletionKind::Alias),"window copy offered a competing ORDER BY");
      check(!find(complete(pg,"SELECT COUNT(*) OVER (| PARTITION BY id) FROM public.users "
                              "WINDOW shared AS (PARTITION BY id)",&catalog),
                  "shared",CompletionKind::Alias),"window copy offered a parent beside PARTITION BY");
      expect(complete(pg,"SELECT COUNT(*) OVER \"Daily|\" FROM public.users "
                         "WINDOW \"Daily Reports\" AS (ORDER BY id)",&catalog),"Daily Reports",CompletionKind::Alias);
      const auto outer=complete(pg,"SELECT COUNT(*) OVER | FROM (SELECT id,COUNT(*) OVER inner_w FROM public.users "
                                  "WINDOW inner_w AS ()) q WINDOW outer_w AS (ORDER BY id)",&catalog);
      expect(outer,"outer_w",CompletionKind::Alias);
      check(!find(outer,"inner_w",CompletionKind::Alias),"named window candidate escaped a nested SELECT");
      expect(complete(Dialect::MySQL,"SELECT COUNT(*) OVER ow| FROM users WINDOW owned_w AS (ORDER BY id)",&catalog),
             "owned_w",CompletionKind::Alias);
      const auto inlineWindow=complete(Dialect::SQLServer,"SELECT COUNT(*) OVER |");
      expect(inlineWindow,"PARTITION BY",CompletionKind::Keyword);
      const auto *partition=find(inlineWindow,"PARTITION BY",CompletionKind::Keyword);
      check(partition->insertText=="(PARTITION BY )" && partition->cursor+1==partition->insertText.size(),
            "OVER snippet did not place the caret inside parentheses");
      check(find(complete(pg,"SELECT (SELECT COUNT(*) OVER (ORDER BY u.na| FROM public.users u);",&catalog),
                 "name",CompletionKind::Column),"nested window recovery closed its query before FROM");
      expect(complete(pg,"SELECT COUNT(*) OVER (ORDER BY id NULLS F|)"),"FIRST",CompletionKind::Keyword);
      expect(complete(pg,"SELECT COUNT(*) OVER (ORDER BY id GR|)"),"GROUPS",CompletionKind::Keyword);
      check(!find(complete(Dialect::SQLServer,"SELECT COUNT(*) OVER (ORDER BY id NULLS F|)"),
                  "FIRST",CompletionKind::Keyword),"SQL Server window offered NULLS FIRST");
      check(!find(complete(Dialect::SQLServer,"SELECT COUNT(*) OVER (ORDER BY id RANGE 1 P|)"),
                  "PRECEDING",CompletionKind::Keyword) &&
            !compile(Dialect::SQLServer,"SELECT COUNT(*) OVER (ORDER BY id RANGE 1 PRECEDING) FROM t")->syntaxValid,
            "SQL Server accepted a numeric RANGE frame offset");
      check(!find(complete(Dialect::MySQL,"SELECT COUNT(*) OVER (ORDER BY id GR|)"),
                  "GROUPS",CompletionKind::Keyword),"MySQL window offered unsupported GROUPS frame");
      expect(complete(pg,"SELECT COUNT(*) OVER w FROM users WINDOW first AS (ORDER BY id), w AS (PA|",&catalog),
             "PARTITION BY",CompletionKind::Keyword);
      CompletionOptions punctuation;
      punctuation.columnListOnly=true;
      const std::string opening="SELECT COUNT(*) OVER (";
      expect(completeSql(pg,opening,opening.size(),&catalog,{"app","public",pg},punctuation),
             "PARTITION BY",CompletionKind::Keyword);
    }
    {
      auto r=complete(Dialect::MySQL,"SELECT CAST(1 AS |)");
      expect(r,"SIGNED",CompletionKind::Type);expect(r,"UNSIGNED",CompletionKind::Type);
      check(!find(r,"INTEGER",CompletionKind::Type) && !find(r,"VARCHAR",CompletionKind::Type),"MySQL CAST completion suggests invalid declaration targets");
      expect(complete(Dialect::MySQL,"CREATE TABLE t(id I|)"),"INTEGER",CompletionKind::Type);
      expect(complete(Dialect::PostgreSQL,"SELECT CAST(1 AS I|)"),"INTEGER",CompletionKind::Type);
    }
    {
      auto catalog=fixture();
      auto field=complete(Dialect::PostgreSQL,"SELECT EXTRACT(Y| FROM created_at) FROM users;",&catalog);
      expect(field,"YEAR",CompletionKind::Keyword);
      for(const auto &item:field.items)check(item.kind==CompletionKind::Keyword,"EXTRACT field polluted by columns/functions");
      expect(complete(Dialect::PostgreSQL,"SELECT EXTRACT(YEAR F|)", &catalog),"FROM",CompletionKind::Keyword);
      expect(complete(Dialect::PostgreSQL,"SELECT EXTRACT(YEAR FROM u.|) FROM users u;",&catalog),"id",CompletionKind::Column);
      expect(complete(Dialect::PostgreSQL,"SELECT INTERVAL '1' DAY TO S|"),"SECOND",CompletionKind::Keyword);
      auto year=complete(Dialect::Oracle,"SELECT INTERVAL '1' YEAR TO | FROM SYS.DUAL;");
      expect(year,"MONTH",CompletionKind::Keyword);
      check(!find(year,"DAY",CompletionKind::Keyword),"year-month qualifier suggested a day-time field");
      expect(complete(Dialect::MySQL,"SELECT DATE '2024-01-01'+INTERVAL 1 D|"),"DAY",CompletionKind::Keyword);
      auto snippet=complete(Dialect::PostgreSQL,"SELECT EXT|");const auto *item=find(snippet,"EXTRACT template",CompletionKind::Snippet);
      check(item && item->insertText.substr(item->cursor,item->selection)=="2000-01-01","EXTRACT snippet selection misses value");
    }

    for(auto d:{Dialect::MySQL,Dialect::PostgreSQL,Dialect::Oracle}) {
      auto r=complete(d,"SELECT DA|");expect(r,"DATE literal",CompletionKind::Snippet);
      const auto *item=find(r,"DATE literal",CompletionKind::Snippet);
      check(item && item->insertText=="DATE '2000-01-01'","date snippet has no valid payload");
    }

    {
      auto catalog=fixture();
      const auto pg=Dialect::PostgreSQL, ms=Dialect::SQLServer;
      auto start=complete(pg,"SELECT |",&catalog);
      expect(start,"*",CompletionKind::Snippet);
      expect(start,"* FROM",CompletionKind::Snippet);
      auto wildcard=complete(pg,"SELECT * |",&catalog);
      check(!wildcard.items.empty() && wildcard.items.front().label=="FROM","SELECT star lacks FROM continuation");
      auto projection=complete(pg,"SELECT COUNT(*) |",&catalog);
      expect(projection,"FROM",CompletionKind::Keyword);
      expect(projection,"AS",CompletionKind::Keyword);
      check(!find(projection,"CREATE",CompletionKind::Keyword),"unrelated keyword in projection continuation");
      auto relation=complete(pg,"SELECT * FROM users u |",&catalog);
      check(!relation.items.empty() && relation.items.front().label=="WHERE","relation continuation priority");
      expect(relation,"LEFT JOIN",CompletionKind::Keyword);
      expect(complete(pg,"SELECT * FROM users u LEFT |",&catalog),"JOIN",CompletionKind::Keyword);
      expect(complete(pg,"SELECT * FROM users u JOIN orders o |",&catalog),"USING",CompletionKind::Keyword);
      check(!find(complete(ms,"SELECT * FROM users u JOIN orders o |",&catalog),"USING",CompletionKind::Keyword),
            "SQL Server join suggested USING");
      expect(complete(pg,"SELECT * FROM users u CROSS JOIN orders o |",&catalog),"WHERE",CompletionKind::Keyword);
      expect(complete(pg,"SELECT * FROM (SELECT id |) v;",&catalog),"FROM",CompletionKind::Keyword);
      expect(complete(pg,"SELECT id | FROM users;",&catalog),"AS",CompletionKind::Keyword);
      check(!find(complete(pg,"SELECT id | FROM users;",&catalog),"FROM",CompletionKind::Keyword),
            "duplicated following FROM");
      expect(complete(pg,"SELECT * FROM users WHERE |",&catalog),"id",CompletionKind::Column);
      auto comparison=complete(pg,"SELECT * FROM users WHERE id >= 2 |",&catalog);
      check(!comparison.items.empty() && comparison.items.front().label=="AND","comparison continuation priority");
      expect(comparison,"ORDER BY",CompletionKind::Keyword);
      auto between=complete(pg,"SELECT * FROM users WHERE id BETWEEN 2 |",&catalog);
      check(between.items.size()==1 && between.items.front().label=="AND","BETWEEN must continue with AND");
      expect(complete(pg,"SELECT id FROM users ORDER |",&catalog),"BY",CompletionKind::Keyword);
      expect(complete(pg,"SELECT id FROM users ORDER BY id |",&catalog),"NULLS LAST",CompletionKind::Keyword);
      check(!find(complete(ms,"SELECT id FROM users ORDER BY id |",&catalog),"NULLS LAST",CompletionKind::Keyword),
            "SQL Server ORDER BY suggested unsupported NULL placement");
    }

    auto c = fixture();
    c.objects={{CatalogObjectKind::Index,"app","public","idx_users_name","users",{}},
      {CatalogObjectKind::Sequence,"app","public","order_seq",{},{}},
      {CatalogObjectKind::Trigger,"app","public","users_audit","users",{}},
      {CatalogObjectKind::Type,"app","public","order_state",{},{}},
      {CatalogObjectKind::Schema,"app",{},"public",{},{}},
      {CatalogObjectKind::Database,"",{},"app",{},{}},
      {CatalogObjectKind::Index,"other","public","secret_index",{},{}}};
    auto materialized=c.tables.front();materialized.name="users_summary";materialized.kind=ObjectKind::MaterializedView;c.tables.push_back(materialized);
    expect(complete(Dialect::MySQL,"SHOW |",&c),"TABLES",CompletionKind::Keyword);
    expect(complete(Dialect::MySQL,"SHOW FULL |",&c),"COLUMNS",CompletionKind::Keyword);
    expect(complete(Dialect::MySQL,"SHOW GLOBAL |",&c),"VARIABLES",CompletionKind::Keyword);
    expect(complete(Dialect::MySQL,"SHOW TABLES FROM |",&c),"app",CompletionKind::Database);
    expect(complete(Dialect::MySQL,"SHOW COLUMNS FROM |",&c),"users",CompletionKind::Table);
    expect(complete(Dialect::MySQL,"SHOW COLUMNS FROM app.|",&c),"users",CompletionKind::Table);
    expect(complete(Dialect::MySQL,"SHOW COLUMNS FROM users FROM |",&c),"app",CompletionKind::Database);
    expect(complete(Dialect::MySQL,"SHOW COLUMNS FROM users WHERE F|",&c),"Field",CompletionKind::Column);
    check(!find(complete(Dialect::MySQL,"SHOW COLUMNS FROM users WHERE i|",&c),"id",CompletionKind::Column),"SHOW predicate leaked inspected table columns");
    expect(complete(Dialect::MySQL,"SHOW FULL TABLES FROM app WHERE T|",&c),"Table_type",CompletionKind::Column);
    expect(complete(Dialect::MySQL,"SHOW CREATE PROCEDURE |",&c),"process_order",CompletionKind::Procedure);
    expect(complete(Dialect::MySQL,"DESCRIBE |",&c),"users",CompletionKind::Table);
    auto pg = Dialect::PostgreSQL, my = Dialect::MySQL, ora = Dialect::Oracle,
         ms = Dialect::SQLServer;
    {
      MemoryCatalog catalog;
      catalog.tables = {
          {"app", "public", "order_items", {{"user_id", ValueType::Integer}}},
          {"app", "public", "orderitems_audit", {{"visible_at", ValueType::String}}},
          {"other", "public", "private_order_items", {{"hidden_id", ValueType::Integer}}}};
      const auto tables = complete(my, "SELECT * FROM orderitems|", &catalog);
      expect(tables, "order_items", CompletionKind::Table);
      check(tables.items.front().label == "orderitems_audit", "fuzzy match outranked a literal prefix");
      expect(complete(my, "SELECT * FROM ITMS|", &catalog), "order_items", CompletionKind::Table);
      const auto suffix = complete(my, "SELECT * FROM items|", &catalog);
      expect(suffix, "order_items", CompletionKind::Table);
      check(!find(suffix, "private_order_items", CompletionKind::Table),
            "fuzzy table name bypassed database visibility");
      const auto column = complete(my, "SELECT i.usrid| FROM order_items i "
                                       "CROSS JOIN orderitems_audit audit", &catalog);
      check(find(column, "user_id", CompletionKind::Column), "missing fuzzy qualified user_id");
      check(find(column, "user_id", CompletionKind::Column)->insertText == "user_id",
            "fuzzy column completion did not preserve the original identifier");
      check(!find(complete(my, "SELECT audit.usrid| FROM order_items i "
                               "CROSS JOIN orderitems_audit audit", &catalog),
                  "user_id", CompletionKind::Column),
            "fuzzy column name bypassed its qualifier");
      const auto quoted = complete(my, "SELECT * FROM order_items i WHERE i.`userid|", &catalog);
      const auto *quotedColumn = find(quoted, "user_id", CompletionKind::Column);
      check(quotedColumn && quotedColumn->insertText == "`user_id`", "missing fuzzy quoted user_id");
      for (int i = 0; i != 150; ++i)
        catalog.tables.push_back({"app", "public", "a_" + std::to_string(i) + "_order_items", {}});
      catalog.tables.push_back({"app", "public", "orderitems", {}});
      CompletionOptions limited;
      limited.maxItems = 1;
      const std::string sql = "SELECT * FROM orderitems";
      const auto top = completeSql(my, sql, sql.size(), &catalog, {"app", "public", my}, limited);
      check(top.items.size() == 1 && top.items.front().label == "orderitems" && top.truncated,
            "many fuzzy candidates displaced a late exact match");
    }
    expect(complete(my, "INSERT INTO t(a,b) VALUES(1,2) AS incoming(new_a,new_b) "
                        "ON DUPLICATE KEY UPDATE c=incoming.new_|;"),
           "new_a",CompletionKind::Column);
    check(!find(complete(my, "INSERT INTO t(a,b) VALUES(incoming.new_|,2) AS incoming(new_a,new_b) "
                             "ON DUPLICATE KEY UPDATE c=incoming.new_b;"),
                "new_a",CompletionKind::Column), "incoming row leaked into inserted expressions");
    expect(complete(pg, "BEGIN; CREATE TABLE saved_demo(id INT); SAVEPOINT mark; "
                        "DROP TABLE saved_demo; ROLLBACK TO mark; SELECT * FROM saved_|;"),
           "saved_demo",CompletionKind::Table);
    check(!find(complete(pg, "BEGIN; SAVEPOINT mark; CREATE TABLE discarded_demo(id INT); "
                             "ROLLBACK TO mark; SELECT * FROM discarded_|;"),
                "discarded_demo",CompletionKind::Table), "rolled-back declaration leaked into completion");
    expect(complete(my, "SELECT j.val FROM (SELECT '[]' AS document) src "
                        "JOIN JSON_TABLE(src.docu|,'$[*]' COLUMNS(val INT PATH '$')) j ON TRUE;"),
           "document",CompletionKind::Column);
    expect(complete(my, "SELECT * FROM (SELECT '[]' AS document) src CROSS JOIN JSON_TABLE(src.docu|"),
           "document",CompletionKind::Column);
    check(!find(complete(my, "SELECT j.val FROM (SELECT '[]' AS doc) src "
                             "JOIN JSON_TABLE(future.docu|,'$[*]' COLUMNS(val INT PATH '$')) j ON TRUE "
                             "JOIN (SELECT '[]' AS document) future ON TRUE;"),
                "document",CompletionKind::Column), "JSON_TABLE document saw a future join sibling");
    expect(complete(pg, "SELECT d.re| FROM (VALUES (1,2)) AS d(renamed,other);"),
           "renamed", CompletionKind::Column);
    expect(complete(pg, "WITH d(renamed) AS (SELECT 1 AS old_name, 2 AS retained) SELECT d.ret| FROM d;"),
           "retained", CompletionKind::Column);
    expect(complete(pg, "WITH RECURSIVE a AS (SELECT b.ret| FROM b), b AS (SELECT 2 AS retained) SELECT * FROM a;"),
           "retained", CompletionKind::Column);
    expect(complete(pg, "SELECT id FROM t ORDER BY id FETCH FIRST 3 ROWS W|"),
           "WITH TIES", CompletionKind::Keyword);
    expect(complete(ora, "SELECT id FROM t ORDER BY id FETCH FIRST ROW WITH T|"),
           "TIES", CompletionKind::Keyword);
    check(!find(complete(ms, "SELECT id FROM t ORDER BY id OFFSET 1 ROW FETCH NEXT 3 ROWS |"),
                "WITH TIES", CompletionKind::Keyword), "SQL Server FETCH offered unsupported WITH TIES");
    expect(complete(ms, "SELECT d.re| FROM (VALUES (1,2)) AS d(renamed,other);"),
           "renamed", CompletionKind::Column);
    expect(complete(my, "DELIMITER $$\nCREATE FUNCTION loop_f(p INT) RETURNS INT BEGIN "
                        "DECLARE local_value INT DEFAULT 0; REPEAT SET local_value=local_value+1; "
                        "UNTIL loc| >= p END REPEAT; RETURN local_value; END$$\nDELIMITER ;"),
           "local_value", CompletionKind::Variable);
    expect(complete(pg, "CREATE FUNCTION loop_f(p INT) RETURNS INT LANGUAGE plpgsql AS $$ "
                        "DECLARE local_value INT:=0; BEGIN LOOP local_value:=local_value+1; "
                        "EXIT WHEN loc| >= p; END LOOP; RETURN local_value; END; $$;"),
           "local_value", CompletionKind::Variable);
    expect(complete(pg, "CREATE FUNCTION read_f(p INT) RETURNS INT LANGUAGE plpgsql AS $$ "
                        "DECLARE local_value INT; BEGIN SELECT 1 INTO STRICT loc|; "
                        "RETURN local_value; END; $$;"), "local_value", CompletionKind::Variable);
    expect(complete(pg, "CREATE FUNCTION read_f() RETURNS BOOL LANGUAGE plpgsql AS $$ "
                        "BEGIN RETURN read_f.fou|; END; $$;"), "found", CompletionKind::Variable);
    expect(complete(pg, "CREATE FUNCTION read_f() RETURNS TEXT LANGUAGE plpgsql AS $$ "
                        "BEGIN RETURN 'ok'; EXCEPTION WHEN OTHERS THEN RETURN sqlerr|; END; $$;"),
           "sqlerrm", CompletionKind::Variable);
    expect(complete(pg, "CREATE FUNCTION read_f() RETURNS TEXT LANGUAGE plpgsql AS $$ "
                        "BEGIN <<failed_step>> BEGIN RETURN 'ok'; EXCEPTION WHEN OTHERS THEN "
                        "RETURN failed_step.sqlsta|; END; END; $$;"), "sqlstate", CompletionKind::Variable);
    check(!find(complete(pg, "CREATE FUNCTION read_f() RETURNS TEXT LANGUAGE plpgsql AS $$ "
                             "BEGIN BEGIN NULL; EXCEPTION WHEN OTHERS THEN NULL; END; RETURN sqlerr|; END; $$;"),
                "sqlerrm", CompletionKind::Variable), "exception status leaked out of its handler");
    expect(complete(pg, "CREATE FUNCTION f() RETURNS TEXT LANGUAGE plpgsql AS $$ "
                        "DECLARE err_msg TEXT; BEGIN RETURN 'ok'; EXCEPTION WHEN OTHERS THEN "
                        "GET STACKED DIAGNOSTICS err_| = MESSAGE_TEXT; RETURN err_msg; END; $$;"),
           "err_msg", CompletionKind::Variable);
    const auto stacked=complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                                    "DECLARE v TEXT; BEGIN NULL; EXCEPTION WHEN OTHERS THEN "
                                    "GET STACKED DIAGNOSTICS v := |; END; $$;");
    expect(stacked,"MESSAGE_TEXT",CompletionKind::Keyword);
    expect(stacked,"PG_CONTEXT",CompletionKind::Keyword);
    check(!find(stacked,"ROW_COUNT",CompletionKind::Keyword), "ROW_COUNT appeared in stacked diagnostics");
    const auto current=complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                                     "DECLARE v BIGINT; BEGIN GET CURRENT DIAGNOSTICS v = row_|; END; $$;");
    expect(current,"ROW_COUNT",CompletionKind::Keyword);
    check(!find(current,"MESSAGE_TEXT",CompletionKind::Keyword), "MESSAGE_TEXT appeared in current diagnostics");
    const auto catchCompletion=complete(ms,"CREATE PROCEDURE p AS BEGIN TRY DECLARE @prior INT = 1; "
                                           "END TRY BEGIN CATCH SELECT ERROR_M|; END CATCH;");
    expect(catchCompletion,"ERROR_MESSAGE",CompletionKind::Function);
    expect(complete(ms,"CREATE PROCEDURE p AS BEGIN TRY DECLARE @prior INT = 1; END TRY "
                       "BEGIN CATCH SELECT @pri|; END CATCH;"),"prior",CompletionKind::Variable);
    expect(complete(pg, "CREATE FUNCTION loop_f() RETURNS INT LANGUAGE plpgsql AS $$ "
                        "BEGIN FOR loop_index IN 1..3 LOOP RETURN loop_in|; END LOOP; RETURN 0; END; $$;"),
           "loop_index", CompletionKind::Variable);
    check(!find(complete(pg, "CREATE FUNCTION loop_f() RETURNS INT LANGUAGE plpgsql AS $$ "
                             "BEGIN FOR loop_index IN 1..3 LOOP NULL; END LOOP; RETURN loop_in|; END; $$;"),
                "loop_index", CompletionKind::Variable), "numeric FOR index leaked outside its scope");
    expect(complete(my, "DELIMITER $$\nCREATE FUNCTION read_f(p INT) RETURNS INT BEGIN "
                        "DECLARE local_value INT; SELECT n FROM t INTO loc|; RETURN local_value; END$$\nDELIMITER ;"),
           "local_value", CompletionKind::Variable);
    for(auto dialect:{pg,my,ora,ms}) {
      for(const auto& text:{std::string("s|"),std::string("-- next statement\ns|"),std::string("SELECT 1;s|")}) {
        auto start=complete(dialect,text,&c);
        check(!start.items.empty() && start.items.front().label=="SELECT" && start.items.front().kind==CompletionKind::Keyword,"statement start does not prefer SELECT");
        check(!find(start,"SUBSTRING",CompletionKind::Function),"expression function leaked into statement start");
      }
      auto nested=complete(dialect,"WITH q AS(s|) SELECT * FROM q;",&c);
      check(!nested.items.empty() && nested.items.front().label=="SELECT","CTE query start does not prefer SELECT");
    }
    expect(complete(pg,"CREATE SEQ|",&c),"SEQUENCE",CompletionKind::Keyword);
    expect(complete(pg,"CREATE MAT|",&c),"MATERIALIZED VIEW",CompletionKind::Snippet);
    expect(complete(pg,"CREATE IND|",&c),"INDEX",CompletionKind::Snippet);
    check(!find(complete(Dialect::SQLite,"CREATE FUN|",&c),"FUNCTION",CompletionKind::Keyword),"unsupported SQLite CREATE FUNCTION keyword offered");
    expect(complete(pg,"DROP INDEX IF EXISTS idx_|",&c),"idx_users_name",CompletionKind::Index);
    expect(complete(pg,"DROP SEQUENCE public.order_|",&c),"order_seq",CompletionKind::Sequence);
    expect(complete(pg,"DROP TRIGGER users_| ON users",&c),"users_audit",CompletionKind::Trigger);
    expect(complete(pg,"ALTER TYPE order_|",&c),"order_state",CompletionKind::Type);
    expect(complete(pg,"SET search_path TO pub|",&c),"public",CompletionKind::Schema);
    expect(complete(my,"USE ap|",&c),"app",CompletionKind::Database);
    expect(complete(pg,"REFRESH MATERIALIZED VIEW users_|",&c),"users_summary",CompletionKind::MaterializedView);
    expect(complete(pg,"CREATE TABLE t(state order_|)",&c),"order_state",CompletionKind::Type);
    expect(complete(pg,"SELECT 1::public.order_|",&c),"order_state",CompletionKind::Type);
    expect(complete(pg,"SELECT CAST(1 AS public.order_|)",&c),"order_state",CompletionKind::Type);
    expect(complete(pg,"CREATE TABLE t(state public.order_|)",&c),"order_state",CompletionKind::Type);
    expect(complete(pg,"ALTER TABLE users ALTER COLUMN id TYPE order_|",&c),"order_state",CompletionKind::Type);

    auto dropped=complete(pg,"DROP FUNCTION cal|",&c);expect(dropped,"calculate",CompletionKind::Function);
    check(find(dropped,"calculate",CompletionKind::Function)->insertText=="calculate","DDL routine completion inserted call parentheses");
    dropped=complete(ms,"EXEC pro|",&c);expect(dropped,"process_order",CompletionKind::Procedure);
    check(find(dropped,"process_order",CompletionKind::Procedure)->insertText=="process_order","EXEC completion inserted invalid parentheses");
    check(!find(complete(pg,"DROP INDEX sec|",&c),"secret_index",CompletionKind::Index),"cross-database object leaked");
    check(!find(complete(pg,"CREATE INDEX idx_|",&c),"idx_users_name",CompletionKind::Index),"CREATE declaration suggested existing index");
    expect(complete(pg,"CREATE INDEX fresh_idx ON us|",&c),"users",CompletionKind::Table);
    expect(complete(pg,"CREATE INDEX scripted_idx ON users(name);DROP INDEX script|",&c),"scripted_idx",CompletionKind::Index);
    check(!find(complete(pg,"CREATE INDEX scripted_idx ON users(name);DROP INDEX scripted_idx;DROP INDEX script|",&c),"scripted_idx",CompletionKind::Index),"dropped script index remained visible");
    expect(complete(pg,"CREATE SEQUENCE scripted_seq;DROP SEQUENCE script|",&c),"scripted_seq",CompletionKind::Sequence);
    auto expression=complete(my,"SELECT s|",&c);expect(expression,"SUBSTRING",CompletionKind::Function);
    auto r = complete(pg,"CREATE FUNCTION script_f(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$; SELECT script_|;",&c);
    expect(r,"script_f",CompletionKind::Function);
    r=complete(my,"DELIMITER $$\nCREATE FUNCTION script_f(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p;$$\nDELIMITER ;\nSELECT script_|;",&c);
    expect(r,"script_f",CompletionKind::Function);
    r=complete(pg,"CREATE TABLE script_owned(id INTEGER,n INTEGER); CREATE FUNCTION script_f(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $$ BEGIN RETURN p; END; $$; SELECT s.| FROM script_owned s;",&c);
    expect(r,"id",CompletionKind::Column);expect(r,"n",CompletionKind::Column);
    r=complete(pg,"SELECT 1; CREATE FUNCTION script_f(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $$ DECLARE visible_local INTEGER; BEGIN RETURN visi|; END; $$; SELECT 2;",&c);
    expect(r,"visible_local",CompletionKind::Variable);
    r=complete(ms,"CREATE PROC script_p @p INTEGER AS BEGIN SELECT @p; END;\nGO\nEXEC script_|;",&c);
    expect(r,"script_p",CompletionKind::Procedure);
    r = complete(pg, "SELECT * FROM us|", &c);
    expect(r, "users", CompletionKind::Table);
    check(!find(r, "secret", CompletionKind::Table),
          "cross-database completion leaked");
    r = complete(pg, "SELECT u.na| FROM users u;", &c);
    expect(r, "name", CompletionKind::Column);
    check(r.parsedScope, "query AST not used");
    check(r.replacement.begin == 9 && r.replacement.end == 11,
          "prefix range wrong");
    r = complete(pg, "SELECT u.| FROM users u;", &c);
    expect(r, "DisplayName", CompletionKind::Column);
    check(find(r, "DisplayName", CompletionKind::Column)->insertText ==
              "\"DisplayName\"",
          "quoted case lost");
    check(!find(r, "SELECT", CompletionKind::Keyword),
          "keywords after qualifier");
    r = complete(pg, "SELECT | FROM users u JOIN orders o ON o.user_id=u.id;",
                 &c);
    expect(r, "u.id", CompletionKind::Column);
    expect(r, "o.id", CompletionKind::Column);
    check(!find(r, "id", CompletionKind::Column),
          "ambiguous bare column offered");
    expect(r, "u", CompletionKind::Alias);
    r = complete(
        pg,
        "SELECT 1 FROM users u WHERE EXISTS(SELECT 1 FROM orders o WHERE u.|);",
        &c);
    expect(r, "name", CompletionKind::Column);
    r = complete(
        pg,
        "SELECT 1 FROM users u WHERE EXISTS(SELECT 1 FROM orders u WHERE u.|);",
        &c);
    expect(r, "amount", CompletionKind::Column);
    check(!find(r, "name", CompletionKind::Column),
          "outer alias shadowing broken");
    r = complete(pg, "SELECT d.| FROM (SELECT id AS code,name FROM users) d;",
                 &c);
    expect(r, "code", CompletionKind::Column);
    expect(r, "name", CompletionKind::Column);
    r = complete(pg, "WITH chosen AS(SELECT id FROM users) SELECT * FROM ch|;",
                 &c);
    expect(r, "chosen", CompletionKind::Table);
    r = complete(pg, "CALL public.pro|", &c);
    expect(r, "process_order", CompletionKind::Procedure);
    r = complete(pg, "SELECT cal|", &c);
    expect(r, "calculate", CompletionKind::Function);
    r = complete(pg, "CALL pro|", &c);
    expect(r, "process_order", CompletionKind::Procedure);
    r = complete(pg, "CREATE TA|");
    expect(r, "TABLE", CompletionKind::Snippet);
    auto *snippet = find(r, "TABLE", CompletionKind::Snippet);
    check(snippet->replacement.begin == 0 &&
              snippet->insertText.rfind("CREATE TABLE", 0) == 0,
          "snippet leaves duplicate CREATE");
    check(snippet->insertText.substr(snippet->cursor, snippet->selection) ==
              "object_name",
          "snippet placeholder wrong");
    for (auto d : {pg, my, ora, ms}) {
      r = complete(d, "CREATE |");
      expect(r, "FUNCTION", CompletionKind::Snippet);
      expect(r, "PROCEDURE", CompletionKind::Snippet);
      expect(r, "TRIGGER", CompletionKind::Snippet);
    }
    r=complete(pg,"CREATE TABLE local_owned(id INTEGER,title TEXT);SELECT t.| FROM local_owned AS t;");
    expect(r,"id",CompletionKind::Column);expect(r,"title",CompletionKind::Column);
    r=complete(pg,"CREATE TABLE local_owned(id INTEGER);SELECT * FROM local_o|;");
    expect(r,"local_owned",CompletionKind::Table);
    r = complete(pg, "CREATE TABLE t(id |)");
    expect(r, "INTEGER", CompletionKind::Type);
    r = complete(ora, "CREATE TABLE t(id |)");
    expect(r, "NUMBER", CompletionKind::Type);
    r = complete(my, "SELECT IFN|");
    expect(r, "IFNULL", CompletionKind::Function);
    r = complete(ora, "SELECT NV|");
    expect(r, "NVL", CompletionKind::Function);
    r = complete(ms, "SELECT GET|");
    expect(r, "GETDATE", CompletionKind::Function);
    for (auto s : {"SELECT 'us|'", "SELECT 1 -- us|", "SELECT /*us|*/ 1",
                   "SELECT $s$us|$s$"}) {
      r = complete(pg, s, &c);
      check(r.suppressed && r.items.empty(), "completion inside opaque text");
    }

    for (auto pair : std::vector<std::pair<Dialect, std::string>>{
             {my, "SELECT \"abc|"},
             {ms, "SELECT N'abc|"},
             {pg, "SELECT E'abc|"},
             {ora, "SELECT q'[abc|"}}) {
      r = complete(pair.first, pair.second, &c);
      check(r.suppressed && r.items.empty(),
            "unfinished dialect string was not opaque");
    }
    r = complete(pg, "SELECT 'a'|", &c);
    check(!r.suppressed, "closed string boundary remains opaque");
    r = complete(pg, "SELECT /*a*/|", &c);
    check(!r.suppressed, "closed comment boundary remains opaque");
    r = complete(pg, "SELECT * FROM \"us|", &c);
    expect(r, "users", CompletionKind::Table);
    check(find(r, "users", CompletionKind::Table)->insertText == "\"users\"",
          "unfinished quoted identifier mishandled");
    expect(complete(my, "TABLE us|", &c), "users", CompletionKind::Table);
    expect(complete(ms, "SELECT r.n FROM users u CROSS APPLY (SELECT u.na| AS n) r;", &c),
           "name", CompletionKind::Column);
    r = complete(pg, "-- 中文😀\nSELECT u.na| FROM users u;", &c);
    expect(r, "name", CompletionKind::Column);
    check(r.replacement.begin == 23, "UTF8 source replacement changed");
    r = complete(
        pg,
        "CREATE FUNCTION f(p INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $$ "
        "DECLARE local_value INTEGER:=p; BEGIN RETURN loc|; END; $$;",
        &c);
    expect(r, "local_value", CompletionKind::Variable);
    r = complete(pg, "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL "
                     "SELECT n+1 FROM seq WHERE seq.|<6) SELECT n FROM seq;");
    expect(r, "n", CompletionKind::Column);
    r = complete(ms,
                 "CREATE FUNCTION f(@p INTEGER) RETURNS INTEGER AS BEGIN "
                 "DECLARE @local_value INTEGER=@p; RETURN @loc|; END;",
                 &c);
    expect(r, "local_value", CompletionKind::Variable);
    check(find(r, "local_value", CompletionKind::Variable)->insertText ==
              "@local_value",
          "T-SQL variable prefix lost");
    r = complete(pg,
                 "CREATE FUNCTION f(p_value INTEGER) RETURNS INTEGER LANGUAGE plpgsql AS $body$ "
                 "DECLARE local_value INTEGER := p_value; BEGIN RETURN loc|");
    expect(r, "local_value", CompletionKind::Variable);
    check(!r.suppressed, "an unfinished PL/pgSQL dollar body is not a string context");
    r = complete(pg, "CREATE PROCEDURE p(p_value INT) LANGUAGE plpgsql AS $$\n"
                     "DECLARE local_value INT := p_value;\nBEGIN\n  local_value := loc|", &c);
    expect(r, "local_value", CompletionKind::Variable);
    check(r.items.front().label == "local_value", "local variable should precede builtin names");
    r = complete(pg, "CREATE FUNCTION f(p_value INT) RETURNS INT LANGUAGE plpgsql AS $$ "
                     "DECLARE local_value INT := p_value; BEGIN CASE WHEN loc| > 0 "
                     "THEN RETURN local_value; ELSE RETURN 0; END CASE; END; $$;");
    expect(r, "local_value", CompletionKind::Variable);
    r = complete(pg,
                 "CREATE PROCEDURE p(p_value INTEGER) LANGUAGE plpgsql AS $$ "
                 "BEGIN RAISE NOTICE 'p_va|");
    check(r.suppressed && r.items.empty(), "strings inside a partial routine stay opaque");
    r = complete(pg, "SELECT $$AS BEGIN sel|");
    check(r.suppressed && r.items.empty(), "a standalone unfinished dollar string stays opaque");
    r = complete(ms, "CREATE OR ALTER PROC dbo.p @p_value INT AS BEGIN "
                    "SET NOCOUNT ON; DECLARE @local_value INT = @p_value; SELECT @loc|");
    expect(r, "local_value", CompletionKind::Variable);
    check(find(r, "local_value", CompletionKind::Variable)->insertText == "@local_value",
          "CREATE OR ALTER procedure lost the local variable spelling");
    r = complete(ms, "ALTER PROCEDURE dbo.p AS BEGIN "
                    "SET NOCOUNT OFF; DECLARE @local_value INT = 1; SELECT @loc|");
    expect(r, "local_value", CompletionKind::Variable);
    r = complete(my, "CREATE PROCEDURE p(IN p_value INT) BEGIN "
                    "DECLARE local_value INT DEFAULT p_value; SELECT loc|");
    expect(r, "local_value", CompletionKind::Variable);
    {
      const std::string prefix =
          "DELIMITER //\nCREATE PROCEDURE p() BEGIN DECLARE item_id INT; "
          "DECLARE main_cursor CURSOR FOR SELECT 1; ";
      for (const auto *use : {"OPEN main|; END//", "FETCH NEXT FROM main| INTO item_id; END//",
                              "CLOSE main|; END//"})
        expect(complete(my, prefix + use), "main_cursor", CompletionKind::Cursor);
      expect(complete(my, prefix + "OPEN main_cursor; FETCH main_cursor INTO item|; END//"),
             "item_id", CompletionKind::Variable);
      const auto inside = complete(my, prefix +
          "BEGIN DECLARE local_cursor CURSOR FOR SELECT 2; OPEN loc|; END; END//");
      expect(inside, "local_cursor", CompletionKind::Cursor);
      const auto outside = complete(my, prefix +
          "BEGIN DECLARE local_cursor CURSOR FOR SELECT 2; END; OPEN loc|; END//");
      check(!find(outside, "local_cursor", CompletionKind::Cursor), "cursor completion leaked a closed scope");
    }
    for (auto dialect : {my, Dialect::MariaDB, Dialect::OceanBase}) {
      const std::string prefix =
          "-- 中文\nDELIMITER\t$$\nCREATE FUNCTION f_get_quantity(_id INT) "
          "RETURNS INT READS SQL DATA\nBEGIN\nIF _id > 0 THEN\n";
      const std::string suffix =
          "RETURN (SELECT SUM(id) FROM t_activity WHERE id = _id); "
          "ELSE RETURN (SELECT SUM(id) FROM t_activity); END IF; END$$";
      const auto start = complete(dialect, prefix + "  _i|\n" + suffix);
      expect(start, "_id", CompletionKind::Variable);
      const auto *assignment = find(start, "_id", CompletionKind::Variable);
      check(assignment->insertText == "SET _id = " &&
            assignment->replacement.begin == prefix.size() + 2 &&
            assignment->replacement.end == prefix.size() + 4,
            "statement-start completion did not offer a SET assignment at the original location");
      const auto expression = complete(dialect, prefix +
          "RETURN (SELECT SUM(id) FROM t_activity WHERE id = _i|); "
          "ELSE RETURN 0; END IF; END$$\nDELIMITER ;");
      expect(expression, "_id", CompletionKind::Variable);
      check(find(expression, "_id", CompletionKind::Variable)->insertText == "_id",
            "expression completion replaced a parameter with a statement");
      expect(complete(dialect, prefix + "SET _i| = 1; " + suffix),
             "_id", CompletionKind::Variable);
      const auto close = complete(dialect, prefix + "RETURN 1;\n  en|\nEND$$");
      expect(close, "END IF;", CompletionKind::Keyword);
      check(close.items.front().insertText == "END IF;", "END IF did not rank first in its block");
      const auto completedEnd = complete(dialect, prefix + "RETURN 1;\n  END I|;\nEND$$");
      expect(completedEnd, "END IF;", CompletionKind::Keyword);
      const auto *ending = find(completedEnd, "END IF;", CompletionKind::Keyword);
      check(ending->replacement.end - ending->replacement.begin == 6 &&
                completedEnd.items.front().insertText == "END IF;",
            "END IF completion did not replace the complete clause and existing semicolon");
      const auto blank = complete(dialect, prefix + "RETURN 1;\n  |\nEND$$");
      check(!blank.items.empty() && blank.items.front().label == "END IF;",
            "IF body did not prioritize its full closing clause on a new line");
      const auto editEnd = complete(dialect, prefix + "RETURN 1;\n  END| IF;\nEND$$");
      expect(editEnd, "END IF;", CompletionKind::Keyword);
      const auto *wholeEnd = find(editEnd, "END IF;", CompletionKind::Keyword);
      check(wholeEnd->replacement.end - wholeEnd->replacement.begin == 7,
            "editing a full END IF duplicated the suffix");
      const auto delimiter = complete(dialect, prefix + "RETURN 1;\nEND|$$");
      expect(delimiter, "END IF;", CompletionKind::Keyword);
      check(find(delimiter, "END IF;", CompletionKind::Keyword)->replacement.end ==
                prefix.size() + std::string_view("RETURN 1;\nEND").size(),
            "completing END IF consumed part of the client delimiter");
      auto semiPrefix = prefix;
      semiPrefix.replace(semiPrefix.find("$$"), 2, ";;");
      const auto semiDelimiter = complete(dialect, semiPrefix + "RETURN 1;\nEND|;;");
      expect(semiDelimiter, "END IF;", CompletionKind::Keyword);
      check(find(semiDelimiter, "END IF;", CompletionKind::Keyword)->replacement.end ==
                semiPrefix.size() + std::string_view("RETURN 1;\nEND").size(),
            "a custom client delimiter starting with semicolon became part of the edit");
      check(!find(complete(dialect, prefix + "BEGIN RETURN 1;\nEND|;\nEND IF; END$$"),
                  "END IF;", CompletionKind::Keyword),
            "END within a nested BEGIN picked the enclosing IF");
      const auto nestedIf = complete(dialect, prefix + "RETURN 1;\n  ELSE\n    if|\nEND IF; END$$");
      expect(nestedIf, "IF", CompletionKind::Snippet);
      const auto *block = find(nestedIf, "IF", CompletionKind::Snippet);
      check(block->insertText == "IF condition THEN\n      \n    END IF;" &&
                block->cursor == 3 && block->selection == 9 &&
                !find(nestedIf, "IF", CompletionKind::Function),
            "statement completion inserted a scalar IF function or lost its condition selection");
      const auto scalarIf = complete(dialect, prefix + "RETURN if|; END IF; END$$");
      expect(scalarIf, "IF", CompletionKind::Function);
      check(find(scalarIf, "IF", CompletionKind::Function)->insertText == "IF()" &&
                !find(scalarIf, "IF", CompletionKind::Snippet),
            "scalar IF completion became a control statement");
    }
    r = complete(ora, "CREATE OR REPLACE PROCEDURE p(p_value IN NUMBER) AS "
                     "local_value NUMBER := p_value; BEGIN local_value := loc|");
    expect(r, "LOCAL_VALUE", CompletionKind::Variable);
    r = complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                    "BEGIN SELECT na|; SELECT name FROM users; END; $$;", &c);
    check(!find(r, "name", CompletionKind::Column),
          "partial embedded SQL suggested a column from a later statement");
    r = complete(pg, "CREATE FUNCTION p(p_count BIGINT) RETURNS BIGINT LANGUAGE plpgsql AS $$ "
                     "BEGIN RETURN $|; END; $$;");
    expect(r, "$1", CompletionKind::Variable);
    const auto *positional = find(r, "$1", CompletionKind::Variable);
    check(positional->insertText == "$1" && positional->detail.find("BIGINT") != std::string::npos,
          "positional argument completion lost its name or type");
    r = complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                     "DECLARE v INTEGER; BEGIN RAISE NOTICE 'routine says ; then END'; "
                     "WITH staged AS (SELECT 1 AS staged_id) SELECT s.st| INTO v FROM staged s; END; $$;");
    expect(r, "staged_id", CompletionKind::Column);
    check(r.parsedScope, "incomplete routine SQL lost the parsed CTE");
    r = complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                     "BEGIN PERFORM u.na| FROM users u; END; $$;", &c);
    expect(r, "name", CompletionKind::Column);
    r = complete(pg, "CREATE PROCEDURE p(name TEXT) LANGUAGE plpgsql AS $$ "
                     "BEGIN PERFORM na| FROM users u; END; $$;", &c);
    expect(r, "u.name", CompletionKind::Column);
    expect(r, "name", CompletionKind::Variable);
    check(find(r,"u.name",CompletionKind::Column)->insertText=="u.name" &&
          find(r,"name",CompletionKind::Variable)->insertText=="p.name",
          "column/parameter collision suggested an ambiguous reference");
    expect(complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                        "BEGIN PERFORM cal|; END; $$;", &c), "calculate", CompletionKind::Function);
    expect(complete(pg, "CREATE PROCEDURE p() LANGUAGE plpgsql AS $$ "
                        "BEGIN CALL proc|; END; $$;", &c), "process_order", CompletionKind::Procedure);
    r = complete(pg, "CREATE FUNCTION p() RETURNS INT LANGUAGE plpgsql AS $$ "
                     "<<outer_block>> DECLARE local_value BIGINT := 1; BEGIN "
                     "DECLARE local_value INTEGER := loc|; BEGIN NULL; END; RETURN 1; END outer_block; $$;");
    expect(r, "local_value", CompletionKind::Variable);
    check(find(r, "local_value", CompletionKind::Variable)->detail.find("BIGINT") != std::string::npos,
          "initializer looked up its own unfinished declaration");
    r = complete(pg, "CREATE FUNCTION prior() RETURNS INT LANGUAGE plpgsql AS $$ "
                     "BEGIN RAISE NOTICE 'skipped'; RETURN 1; END; $$; "
                     "CREATE FUNCTION current_f(p_value INT) RETURNS INT LANGUAGE plpgsql AS $$ "
                     "BEGIN RETURN p_va|; END; $$;");
    expect(r, "p_value", CompletionKind::Variable);
    r = complete(ora, "CREATE PROCEDURE p(input_value IN NUMBER, out_value OUT NUMBER) "
                      "AS BEGIN SELECT 1 INTO | FROM SYS.DUAL; END;");
    expect(r, "OUT_VALUE", CompletionKind::Variable);
    check(!find(r, "INPUT_VALUE", CompletionKind::Variable), "read-only Oracle IN parameter suggested as a write target");
    r = complete(ora, "CREATE PROCEDURE p AS i NUMBER; BEGIN FOR i IN 1..3 LOOP "
                     "SELECT 1 INTO i| FROM SYS.DUAL; END LOOP; END;");
    check(!find(r, "I", CompletionKind::Variable),
          "read-only innermost symbol did not hide the writable outer declaration");
    expect(complete(ora, "CREATE PROCEDURE p(input_value IN NUMBER, out_value OUT NUMBER) "
                         "AS BEGIN out_value := in|; END;"), "INPUT_VALUE", CompletionKind::Variable);
    r = complete(ms, "CREATE PROCEDURE p @p_value INT AS BEGIN DECLARE @local_value INT; SET @| = @p_value; END;");
    expect(r, "local_value", CompletionKind::Variable);
    const auto *tsqlVariable = find(r,"local_value",CompletionKind::Variable);
    check(tsqlVariable->insertText=="@local_value" && tsqlVariable->replacement.end-tsqlVariable->replacement.begin==1,
          "completing a bare T-SQL @ would duplicate the prefix");
    for (const auto dialect:{my,Dialect::MariaDB}) {
      expect(complete(dialect,"CREATE PROCEDURE p() BEGIN SIG|; END;"),"SIGNAL",CompletionKind::Keyword);
      const auto condition=complete(dialect,"CREATE PROCEDURE p() BEGIN "
          "DECLARE denied_access CONDITION FOR SQLSTATE '45000'; "
          "DECLARE errno_only CONDITION FOR 1213; SIGNAL |; END;");
      expect(condition,"SQLSTATE",CompletionKind::Keyword);
      expect(condition,"denied_access",CompletionKind::Variable);
      check(!find(condition,"errno_only",CompletionKind::Variable),"error-code condition suggested for SIGNAL");
      const auto item=complete(dialect,"CREATE PROCEDURE p() BEGIN SIGNAL SQLSTATE '45000' "
          "SET MESSAGE_TEXT='x', |; END;");
      expect(item,"MYSQL_ERRNO",CompletionKind::Keyword);
      check(!find(item,"MESSAGE_TEXT",CompletionKind::Keyword),"duplicate SIGNAL information field suggested");
      const auto value=complete(dialect,"CREATE PROCEDURE p(IN p_message VARCHAR(50)) BEGIN "
          "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT=p_me|; END;");
      expect(value,"p_message",CompletionKind::Variable);
      check(!find(value,"p_message",CompletionKind::Column),"scope variable presented as a table column");
      expect(complete(dialect,"CREATE PROCEDURE p() BEGIN DECLARE EXIT HANDLER FOR SQLEXCEPTION "
          "RESIGNAL SE|; DO 0; END;"),"SET",CompletionKind::Keyword);
    }
    for (const auto dialect:{my,Dialect::MariaDB,Dialect::OceanBase}) {
      const auto area=complete(dialect,"CREATE PROCEDURE p() BEGIN GET |; END;");
      expect(area,"DIAGNOSTICS",CompletionKind::Keyword);
      check(bool(find(area,"STACKED",CompletionKind::Keyword))==(dialect!=Dialect::MariaDB),
            "diagnostic stack completion leaked across dialects");
      const auto target=complete(dialect,"CREATE PROCEDURE p() BEGIN DECLARE v_count INT; "
          "GET DIAGNOSTICS |; END;");
      expect(target,"CONDITION",CompletionKind::Keyword);
      expect(target,"v_count",CompletionKind::Variable);
      const auto statement=complete(dialect,"CREATE PROCEDURE p() BEGIN DECLARE v_count INT; "
          "GET DIAGNOSTICS v_count = |; END;");
      expect(statement,"ROW_COUNT",CompletionKind::Keyword);
      expect(statement,"NUMBER",CompletionKind::Keyword);
      check(!find(statement,"MESSAGE_TEXT",CompletionKind::Keyword),"condition field offered for statement diagnostic");
      const auto field=complete(dialect,"CREATE PROCEDURE p() BEGIN DECLARE v_code INT; "
          "GET DIAGNOSTICS CONDITION 1 v_code = |; END;");
      expect(field,"MYSQL_ERRNO",CompletionKind::Keyword);
      check(bool(find(field,"ROW_NUMBER",CompletionKind::Keyword))==(dialect==Dialect::MariaDB),
            "ROW_NUMBER diagnostic suggested in wrong dialect");
      check(bool(find(field,"TABLE_NAME",CompletionKind::Keyword))==(dialect!=Dialect::OceanBase),
            "OceanBase condition diagnostic fields exceeded engine support");
      check(!find(field,"ROW_COUNT",CompletionKind::Keyword),"statement field offered for condition diagnostic");
    }
    for (const auto dialect:{my,Dialect::MariaDB,Dialect::OceanBase}) {
      const std::string surroundings="CREATE PROCEDURE p() outer_block: BEGIN "
          "again_loop: LOOP inner_block: BEGIN ";
      const auto leave=complete(dialect,surroundings+"LEAVE |; END inner_block; "
          "END LOOP again_loop; END outer_block;");
      expect(leave,"inner_block",CompletionKind::Label);
      expect(leave,"outer_block",CompletionKind::Label);
      expect(leave,"again_loop",CompletionKind::Label);
      const auto iterate=complete(dialect,surroundings+"ITERATE |; END inner_block; "
          "END LOOP again_loop; END outer_block;");
      expect(iterate,"again_loop",CompletionKind::Label);
      check(!find(iterate,"inner_block",CompletionKind::Label),"ITERATE advertised a block label");
      const auto handler=complete(dialect,"CREATE PROCEDURE p() outer_block: BEGIN "
          "DECLARE EXIT HANDLER FOR SQLEXCEPTION handler_block: BEGIN "
          "LEAVE |; END handler_block; DO 0; END outer_block;");
      expect(handler,"handler_block",CompletionKind::Label);
      check(!find(handler,"outer_block",CompletionKind::Label),"hidden outer label advertised inside handler");
      const auto closed=complete(dialect,"CREATE PROCEDURE p() outer_block: BEGIN "
          "done_block: BEGIN DO 0; END done_block; LEAVE |; END outer_block;");
      expect(closed,"outer_block",CompletionKind::Label);
      check(!find(closed,"done_block",CompletionKind::Label),"closed block's label remained visible");
    }
    for (const auto dialect:{my,Dialect::MariaDB,Dialect::OceanBase,Dialect::TiDB}) {
      const auto start=complete(dialect,"CREATE TABLE draft (user_id INT,nickname VARCHAR(30), KEY idx (|");
      expect(start,"user_id",CompletionKind::Column);
      expect(start,"nickname",CompletionKind::Column);
      const auto columns=complete(dialect,"CREATE TEMPORARY TABLE draft (user_id INT,nickname VARCHAR(30), "
          "UNIQUE INDEX idx USING BTREE (nickname(4), ui|");
      expect(columns,"user_id",CompletionKind::Column);
      const auto remaining=complete(dialect,"CREATE TABLE draft (user_id INT,nickname VARCHAR(30), "
          "KEY idx (nickname(4), |");
      expect(remaining,"user_id",CompletionKind::Column);
      check(!find(remaining,"nickname",CompletionKind::Column),"duplicate inline index key offered");
      const auto latter=complete(dialect,"CREATE TABLE draft (INDEX idx (ni|),nickname VARCHAR(30))");
      expect(latter,"nickname",CompletionKind::Column);
      const auto unrelated=complete(dialect,"CREATE TABLE draft (nickname VARCHAR(30), note VARCHAR(|");
      check(!find(unrelated,"nickname",CompletionKind::Column),"index fields suggested in a length slot");
    }
    for (const auto dialect:{my,Dialect::MariaDB}) {
      const std::string body="CREATE PROCEDURE p() BEGIN PREPARE recent_handle FROM 'SELECT 1'; ";
      expect(complete(dialect,body+"EXECUTE rec|; END;"),"recent_handle",CompletionKind::PreparedStatement);
      expect(complete(dialect,body+"DEALLOCATE PREPARE |; END;"),"recent_handle",CompletionKind::PreparedStatement);
      expect(complete(dialect,body+"EXECUTE recent_handle US|; END;"),"USING",CompletionKind::Keyword);
      expect(complete(dialect,"CREATE PROCEDURE p() BEGIN PREPARE test_handle FR|; END;"),"FROM",CompletionKind::Keyword);
      check(!find(complete(dialect,body+"DROP PREPARE recent_handle; EXECUTE |; END;"),
                  "recent_handle",CompletionKind::PreparedStatement),"released prepared handle suggested");
      check(!find(complete(dialect,"CREATE FUNCTION f() RETURNS INT BEGIN PRE|; RETURN 1; END;"),
                  "PREPARE",CompletionKind::Keyword),"stored function offered dynamic PREPARE");
      expect(complete(dialect,"CREATE AG|"),"AGGREGATE FUNCTION",CompletionKind::Keyword);
      expect(complete(dialect,"CREATE AGGREGATE FU|"),"FUNCTION",CompletionKind::Keyword);
      expect(complete(dialect,"CREATE FUNCTION loader RET|"),"RETURNS",CompletionKind::Keyword);
      expect(complete(dialect,"CREATE FUNCTION loader RETURNS IN|"),"INTEGER",CompletionKind::Keyword);
      expect(complete(dialect,"CREATE AGGREGATE FUNCTION IF NOT EXISTS load_it RETURNS DECIMAL SON|"),
             "SONAME",CompletionKind::Keyword);
      check(!find(complete(dialect,"CREATE FUNCTION f() RETURNS INT SO|"),"SONAME",CompletionKind::Keyword),
            "SONAME offered for a stored function");
    }
    expect(complete(Dialect::MariaDB,"CREATE OR REPLACE AGGREGATE FUNCTION loader RETURNS REAL S|"),
           "SONAME",CompletionKind::Keyword);
    check(!find(complete(Dialect::TiDB,"CREATE FUNCTION loader RETURNS REAL S|"),"SONAME",CompletionKind::Keyword),
          "TiDB offered loadable-function keywords");
    {
      MemoryCatalog indexCatalog;
      TableDefinition indexed;
      indexed.database="app";
      indexed.schema="public";
      indexed.name="index_source";
      indexed.columns={{"account_id",ValueType::Integer},{"display_name",ValueType::String},
                       {"other_name",ValueType::String}};
      indexCatalog.tables.push_back(indexed);
      indexed.name="INDEX_SOURCE";
      indexed.columns={{"ACCOUNT_ID",ValueType::Integer},{"DISPLAY_NAME",ValueType::String}};
      MemoryCatalog oracleCatalog;
      oracleCatalog.tables.push_back(indexed);
      for (const auto &[dialect,prefix]:std::vector<std::pair<Dialect,std::string>>{
          {my,"CREATE INDEX idx ON index_source ("},
          {pg,"CREATE UNIQUE INDEX idx ON index_source USING btree ("},
          {Dialect::Oracle,"CREATE INDEX idx ON index_source ("},
          {Dialect::SQLServer,"CREATE INDEX idx ON index_source ("}}) {
        const auto *catalog=dialect==Dialect::Oracle?&oracleCatalog:&indexCatalog;
        const auto fields=complete(dialect,prefix+"|",catalog);
        expect(fields,dialect==Dialect::Oracle?"ACCOUNT_ID":"account_id",CompletionKind::Column);
        const auto remaining=complete(dialect,prefix+"account_id DESC, |",catalog);
        expect(remaining,dialect==Dialect::Oracle?"DISPLAY_NAME":"display_name",CompletionKind::Column);
        check(!find(remaining,dialect==Dialect::Oracle?"ACCOUNT_ID":"account_id",CompletionKind::Column),
              "used column suggested again for a standalone index");
      }
      expect(complete(my,"CREATE TABLE scratch (short_id INT,nickname VARCHAR(40)); "
                         "CREATE INDEX idx ON scratch (nick|"),
             "nickname",CompletionKind::Column);
      check(!find(complete(my,"CREATE INDEX idx ON missing_table (|",&indexCatalog),
                  "account_id",CompletionKind::Column),"standalone index leaked another table's columns");
      for (const auto dialect:{pg,Dialect::SQLServer}) {
        const std::string include=dialect==Dialect::SQLServer?
            "CREATE INDEX idx ON [public].index_source (account_id DESC) INCLUDE (":
            "CREATE INDEX idx ON public.index_source (account_id DESC) INCLUDE (";
        expect(complete(dialect,"CREATE INDEX idx ON index_source (account_id) INC|",&indexCatalog),
               "INCLUDE",CompletionKind::Keyword);
        const auto fields=complete(dialect,include+"|",&indexCatalog);
        expect(fields,"display_name",CompletionKind::Column);
        check(!find(fields,"account_id",CompletionKind::Column),"index key repeated in INCLUDE");
        const auto remaining=complete(dialect,include+"display_name, |",&indexCatalog);
        expect(remaining,"other_name",CompletionKind::Column);
        check(!find(remaining,"display_name",CompletionKind::Column),"already included column suggested");
        const auto predicate=complete(dialect,include+"display_name) |",&indexCatalog);
        expect(predicate,"WHERE",CompletionKind::Keyword);
        check(!find(predicate,"INCLUDE",CompletionKind::Keyword),"duplicate INCLUDE clause suggested");
      }
      const auto expression=complete(pg,"CREATE INDEX idx ON index_source (LOWER(display_name)) INCLUDE (di|",
                                     &indexCatalog);
      expect(expression,"display_name",CompletionKind::Column);
    }
    expect(complete(pg,"SELECT * FROM pg_catalog.unnest(CAST('{1}' AS integer[])) W|"),
           "WITH ORDINALITY",CompletionKind::Keyword);
    expect(complete(pg,"SELECT * FROM gen|"),"generate_series",CompletionKind::Function);
    expect(complete(pg,"SELECT * FROM pg_catalog.gen|"),"generate_series",CompletionKind::Function);
    expect(complete(pg,"SELECT * FROM pg_catalog.generate_series(1,3) W|"),
           "WITH ORDINALITY",CompletionKind::Keyword);
    expect(complete(pg,"SELECT * FROM (VALUES (1)) a, pg_catalog.generate_series(1,3) WITH OR|"),
           "ORDINALITY",CompletionKind::Keyword);
    expect(complete(pg,"SELECT g.ord| FROM generate_series(1,3) WITH ORDINALITY AS g(v,ord)"),
           "ord",CompletionKind::Column);
    check(!find(complete(pg,"SELECT 1,generate_series(1,3) W|"),"WITH ORDINALITY",CompletionKind::Keyword),
          "ordinality completion leaked into a scalar projection");
    expect(complete(pg,"SELECT ARR|"),"ARRAY",CompletionKind::Keyword);
    {
      const auto catalog=fixture();
      expect(complete(pg,"SELECT ARRAY[id,na|] FROM public.users",&catalog),"name",CompletionKind::Column);
      expect(complete(pg,"SELECT ARRAY[[id,na| FROM public.users",&catalog),"name",CompletionKind::Column);
      expect(complete(pg,"SELECT ARRAY (SE|",&catalog),"SELECT",CompletionKind::Keyword);
      expect(complete(pg,"SELECT ARRAY(SELECT na| FROM public.users)",&catalog),"name",CompletionKind::Column);
      expect(complete(pg,"SELECT (ARRAY[1,2])[i| FROM public.users",&catalog),"id",CompletionKind::Column);
      expect(complete(pg,"SELECT (ARRAY[1,2])[:i|] FROM public.users",&catalog),"id",CompletionKind::Column);
      expect(complete(pg,"SELECT id = ANY(ARR| FROM public.users",&catalog),"ARRAY",CompletionKind::Keyword);
      expect(complete(pg,"SELECT id = ANY(ARRAY[i| FROM public.users",&catalog),"id",CompletionKind::Column);
      expect(complete(pg,"SELECT id <> ALL(ARRAY[i|]) FROM public.users",&catalog),"id",CompletionKind::Column);
    }
    {
      MemoryCatalog catalog;
      SqlType integer;
      integer.family=TypeFamily::Integer;
      integer.bits=32;
      integer.unsignedInteger=false;
      auto array=integer;
      array.arrayDimensions=1;
      catalog.tables.push_back({"app","public","samples",{
          {"sample_index",ValueType::Integer,std::make_shared<const SqlType>(integer)},
          {"values_array",ValueType::Unknown,std::make_shared<const SqlType>(array)}
      }});
      expect(complete(pg,"UPDATE public.samples SET values_array[samplei|",&catalog),
             "sample_index",CompletionKind::Column);
      expect(complete(pg,"UPDATE public.samples SET values_array[d.po| FROM (VALUES (1,2)) d(position,amount)",
                      &catalog),"position",CompletionKind::Column);
      expect(complete(pg,"UPDATE public.samples SET values_array[d.po|] = d.amount "
                         "FROM (VALUES (1,2)) d(position,amount)",&catalog),"position",CompletionKind::Column);
      expect(complete(pg,"UPDATE public.samples SET values_array[1]=9, values_a|",&catalog),
             "values_array",CompletionKind::Column);
    }
    expect(complete(pg,"SELECT * FROM users CROSS JOIN LATERAL unnest(users.items) WITH OR|"),
           "ORDINALITY",CompletionKind::Keyword);
    check(!find(complete(pg,"SELECT unnest(CAST('{1}' AS integer[])) W|"),
                "WITH ORDINALITY",CompletionKind::Keyword),
          "ordinality suggested for a scalar projection");
    check(!find(complete(pg,"SELECT * FROM utils.unnest(CAST('{1}' AS integer[])) WITH OR|"),
                "ORDINALITY",CompletionKind::Keyword),
          "ordinality suggested for a routine outside the supported namespace");
    expect(complete(pg,"SELECT u.po| FROM pg_catalog.unnest(CAST('{1}' AS integer[])) "
                       "WITH ORDINALITY AS u(element, position);"),"position",CompletionKind::Column);
    expect(complete(pg,"SELECT u.ord| FROM pg_catalog.unnest(CAST('{1}' AS integer[])) "
                       "WITH ORDINALITY AS u(element);"),"ordinality",CompletionKind::Column);
    check(!find(complete(pg,"SELECT u.ord| FROM pg_catalog.unnest(CAST('{1}' AS integer[])) "
                           "WITH ORDINALITY AS u(element, pos);"),"ordinality",CompletionKind::Column),
          "renamed ordinal column suggested under its default name");
    CompletionOptions o;
    o.language.shouldStop = [] { return true; };
    r = completeSql(pg, "SELECT x", 8, &c, {}, o);
    check(r.items.empty() && !r.diagnostics.empty(), "cancellation ignored");
    std::cout << "PASS contextual completion: catalog, aliases, scopes, CTEs, "
                 "routines, templates, Unicode, opaque text\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL completion: " << e.what() << '\n';
    return 1;
  }
}
