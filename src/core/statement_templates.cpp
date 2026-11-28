#include "navidog/statement_templates.hpp"
#include "navidog/data_edit.hpp"
#include <stdexcept>
namespace navidog {
QString statementTemplate(const ConnectionProfile& p,const TableDefinition& definition,StatementTemplate kind){
    if(definition.columns.isEmpty())throw std::runtime_error("未读取到字段，无法生成 SQL");
    const auto table=tableQualifiedName(p,definition.node);QStringList all,writable,editable;
    for(const auto& column:definition.columns){auto name=quoteIdentifier(column.name,p.driver);all<<name;if(column.protectedReason.isEmpty() && column.generatedExpression.isEmpty() && !column.autoIncrement){writable<<name;if(!column.primaryKey)editable<<name;}}
    if(kind==StatementTemplate::Select)return "SELECT\n    "+all.join(",\n    ")+"\nFROM "+table+";\n";
    const QString prefix="-- 按需填写值并核对条件后，再手动执行。\n";
    if(kind==StatementTemplate::Insert){
        if(writable.isEmpty()){
            if(usesOracleSql(p.driver))throw std::runtime_error("Oracle 表没有普通可写字段，请按实际默认值编写插入语句");
            return prefix+"INSERT INTO "+table+(navidog::isMySqlFamily(p.driver)?" () VALUES ();\n":" DEFAULT VALUES;\n");
        }
        QStringList values;for(int i=0;i<writable.size();++i)values<<"NULL";
        return prefix+"INSERT INTO "+table+" (\n    "+writable.join(",\n    ")+"\n) VALUES (\n    "+values.join(",\n    ")+"\n);\n";
    }
    QStringList predicates;for(auto index:primaryKeyColumns(definition))predicates<<quoteIdentifier(definition.columns[index].name,p.driver)+" = NULL";
    auto where=predicates.isEmpty()?QString("1 = 0"):predicates.join("\n  AND ");
    if(kind==StatementTemplate::Delete)return prefix+"DELETE FROM "+table+"\nWHERE "+where+";\n";
    if(editable.isEmpty())throw std::runtime_error("没有可生成 UPDATE 的普通非主键字段");
    QStringList assignments;for(auto name:editable)assignments<<name+" = NULL";
    return prefix+"UPDATE "+table+"\nSET "+assignments.join(",\n    ")+"\nWHERE "+where+";\n";
}
}
