#pragma once
#include <algorithm>
#include <array>
#include <string_view>

namespace navidog::sql::compiler {
// Keyword spellings/classification independently reviewed against ScriptDOM TSql160.
// The documented SQL Server/Synapse list has five ordinary-name native-profile exceptions;
// see tests/sql/corpus/sqlserver-keyword-probes.json and sql-grammar-corpus.md.
inline constexpr std::array<std::string_view,179> sqlServerReservedWords{{
    "ADD","ALL","ALTER","AND","ANY","AS",
    "ASC","AUTHORIZATION","BACKUP","BEGIN","BETWEEN","BREAK",
    "BROWSE","BULK","BY","CASCADE","CASE","CHECK",
    "CHECKPOINT","CLOSE","CLUSTERED","COALESCE","COLLATE","COLUMN",
    "COMMIT","COMPUTE","CONSTRAINT","CONTAINS","CONTAINSTABLE","CONTINUE",
    "CONVERT","CREATE","CROSS","CURRENT","CURRENT_DATE","CURRENT_TIME",
    "CURRENT_TIMESTAMP","CURRENT_USER","CURSOR","DATABASE","DBCC","DEALLOCATE",
    "DECLARE","DEFAULT","DELETE","DENY","DESC","DISTINCT",
    "DISTRIBUTED","DOUBLE","DROP","ELSE","END","ERRLVL",
    "ESCAPE","EXCEPT","EXEC","EXECUTE","EXISTS","EXIT",
    "EXTERNAL","FETCH","FILE","FILLFACTOR","FOR","FOREIGN",
    "FREETEXT","FREETEXTTABLE","FROM","FULL","FUNCTION","GOTO",
    "GRANT","GROUP","HAVING","HOLDLOCK","IDENTITY","IDENTITYCOL",
    "IDENTITY_INSERT","IF","IN","INDEX","INNER","INSERT","INTERSECT",
    "INTO","IS","JOIN","KEY","KILL","LEFT",
    "LIKE","LINENO","MERGE","NATIONAL","NOCHECK","NONCLUSTERED",
    "NOT","NULL","NULLIF","OF","OFF","OFFSETS",
    "ON","OPEN","OPENDATASOURCE","OPENQUERY","OPENROWSET","OPENXML",
    "OPTION","OR","ORDER","OUTER","OVER","PERCENT",
    "PIVOT","PLAN","PRIMARY","PRINT","PROC","PROCEDURE",
    "PUBLIC","RAISERROR","READ","READTEXT","RECONFIGURE","REFERENCES",
    "REPLICATION","RESTORE","RESTRICT","RETURN","REVERT","REVOKE",
    "RIGHT","ROLLBACK","ROWCOUNT","ROWGUIDCOL","RULE","SAVE",
    "SCHEMA","SELECT","SEMANTICKEYPHRASETABLE","SEMANTICSIMILARITYDETAILSTABLE","SEMANTICSIMILARITYTABLE","SESSION_USER",
    "SET","SETUSER","SHUTDOWN","SOME","STATISTICS","SYSTEM_USER",
    "TABLE","TABLESAMPLE","TEXTSIZE","THEN","TO","TOP",
    "TRAN","TRANSACTION","TRIGGER","TRUNCATE","TRY_CONVERT","TSEQUAL",
    "UNION","UNIQUE","UNPIVOT","UPDATE","UPDATETEXT","USE",
    "USER","VALUES","VARYING","VIEW","WAITFOR","WHEN",
    "WHERE","WHILE","WITH","WRITETEXT"
}};
constexpr bool sqlServerKeywordsSorted() {
  for (std::size_t i=1;i<sqlServerReservedWords.size();++i)
    if (!(sqlServerReservedWords[i-1]<sqlServerReservedWords[i])) return false;
  return true;
}
static_assert(sqlServerKeywordsSorted(),"SQL Server keyword data must be sorted and unique");
inline bool sqlServerReservedWord(std::string_view word) {
  return std::binary_search(sqlServerReservedWords.begin(),sqlServerReservedWords.end(),word);
}
} // namespace navidog::sql::compiler
