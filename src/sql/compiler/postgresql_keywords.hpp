#pragma once
// Keyword classification data from PostgreSQL kwlist.h at the reviewed commit
// in tests/sql/corpus/reference-sources.json. The recognizer below is Navidog's.
// Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
// Portions Copyright (c) 1994, The Regents of the University of California
// PostgreSQL permission/warranty notice is reproduced in the repository NOTICE.
#include <algorithm>
#include <iterator>
#include <string_view>

namespace navidog::sql::compiler {
enum class PostgreSqlKeywordClass { Ordinary, Reserved, ColumnName, TypeFunction };
namespace pg_keyword_detail {
inline constexpr std::string_view reserved[]={
    "ALL","ANALYSE","ANALYZE","AND","ANY","ARRAY",
    "AS","ASC","ASYMMETRIC","BOTH","CASE","CAST",
    "CHECK","COLLATE","COLUMN","CONSTRAINT","CREATE","CURRENT_CATALOG",
    "CURRENT_DATE","CURRENT_ROLE","CURRENT_TIME","CURRENT_TIMESTAMP","CURRENT_USER","DEFAULT",
    "DEFERRABLE","DESC","DISTINCT","DO","ELSE","END",
    "EXCEPT","FALSE","FETCH","FOR","FOREIGN","FROM",
    "GRANT","GROUP","HAVING","IN","INITIALLY","INTERSECT",
    "INTO","LATERAL","LEADING","LIMIT","LOCALTIME","LOCALTIMESTAMP",
    "NOT","NULL","OFFSET","ON","ONLY","OR",
    "ORDER","PLACING","PRIMARY","REFERENCES","RETURNING","SELECT",
    "SESSION_USER","SOME","SYMMETRIC","SYSTEM_USER","TABLE","THEN",
    "TO","TRAILING","TRUE","UNION","UNIQUE","USER",
    "USING","VARIADIC","WHEN","WHERE","WINDOW","WITH",
};
inline constexpr std::string_view column[]={
    "BETWEEN","BIGINT","BIT","BOOLEAN","CHAR","CHARACTER",
    "COALESCE","DEC","DECIMAL","EXISTS","EXTRACT","FLOAT",
    "GREATEST","GROUPING","INOUT","INT","INTEGER","INTERVAL",
    "JSON","JSON_ARRAY","JSON_ARRAYAGG","JSON_EXISTS","JSON_OBJECT","JSON_OBJECTAGG",
    "JSON_QUERY","JSON_SCALAR","JSON_SERIALIZE","JSON_TABLE","JSON_VALUE","LEAST",
    "MERGE_ACTION","NATIONAL","NCHAR","NONE","NORMALIZE","NULLIF",
    "NUMERIC","OUT","OVERLAY","POSITION","PRECISION","REAL",
    "ROW","SETOF","SMALLINT","SUBSTRING","TIME","TIMESTAMP",
    "TREAT","TRIM","VALUES","VARCHAR","XMLATTRIBUTES","XMLCONCAT",
    "XMLELEMENT","XMLEXISTS","XMLFOREST","XMLNAMESPACES","XMLPARSE","XMLPI",
    "XMLROOT","XMLSERIALIZE","XMLTABLE",
};
inline constexpr std::string_view typeFunction[]={
    "AUTHORIZATION","BINARY","COLLATION","CONCURRENTLY","CROSS","CURRENT_SCHEMA",
    "FREEZE","FULL","ILIKE","INNER","IS","ISNULL",
    "JOIN","LEFT","LIKE","NATURAL","NOTNULL","OUTER",
    "OVERLAPS","RIGHT","SIMILAR","TABLESAMPLE","VERBOSE",
};
inline constexpr std::string_view needsAs[]={
    "ARRAY","AS","CHAR","CHARACTER","CREATE","DAY",
    "EXCEPT","FETCH","FILTER","FOR","FROM","GRANT",
    "GROUP","HAVING","HOUR","INTERSECT","INTO","ISNULL",
    "LIMIT","MINUTE","MONTH","NOTNULL","OFFSET","ON",
    "ORDER","OVER","OVERLAPS","PRECISION","RETURNING","SECOND",
    "TO","UNION","VARYING","WHERE","WINDOW","WITH",
    "WITHIN","WITHOUT","YEAR",
};
template<std::size_t N>
bool contains(const std::string_view (&words)[N],std::string_view upper) {
  return std::binary_search(std::begin(words),std::end(words),upper);
}
template<std::size_t N>
constexpr bool sortedUnique(const std::string_view (&words)[N]) {
  for (std::size_t i=1;i<N;++i) if (!(words[i-1]<words[i])) return false;
  return true;
}
static_assert(sortedUnique(reserved) && sortedUnique(column) &&
              sortedUnique(typeFunction) && sortedUnique(needsAs),
              "PostgreSQL keyword classification requires sorted unique data");
} // namespace pg_keyword_detail
inline PostgreSqlKeywordClass postgresKeywordClass(std::string_view upper) {
  using namespace pg_keyword_detail;
  if (contains(reserved,upper)) return PostgreSqlKeywordClass::Reserved;
  if (contains(column,upper)) return PostgreSqlKeywordClass::ColumnName;
  if (contains(typeFunction,upper)) return PostgreSqlKeywordClass::TypeFunction;
  return PostgreSqlKeywordClass::Ordinary;
}
inline bool postgresAliasNeedsAs(std::string_view upper) {
  return pg_keyword_detail::contains(pg_keyword_detail::needsAs,upper);
}
} // namespace navidog::sql::compiler
