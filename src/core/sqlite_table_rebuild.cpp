#include "table_design_internal.hpp"
#include <QCryptographicHash>
#include <algorithm>
namespace navidog {
using namespace design;
bool requiresSqliteRebuild(const TableDefinition& before,const TableDefinition& after){
    if(existingColumnOrder(before,after)!=columnOrder(after))return true;
    if(before.withoutRowid!=after.withoutRowid || before.strict!=after.strict)return true;
    for(const auto& old:before.objects)if(old.kind==TableObjectKind::ForeignKey || old.kind==TableObjectKind::Check || old.constraint){auto it=std::find_if(after.objects.begin(),after.objects.end(),[&](const auto& o){return o.kind==old.kind && o.originalName==old.name;});if(it==after.objects.end() || *it!=old)return true;}
    for(const auto& o:after.objects)if((o.kind==TableObjectKind::ForeignKey || o.kind==TableObjectKind::Check || o.constraint) && o.originalName.isEmpty())return true;
    for(const auto& old:before.columns){auto it=std::find_if(after.columns.begin(),after.columns.end(),[&](const auto& c){return c.originalName==old.name;});if(it==after.columns.end()){if(old.primaryKey || old.autoIncrement)return true;continue;}
        if(std::tie(old.type,old.nullable,old.defaultExpression,old.primaryKey,old.autoIncrement,old.generatedExpression,old.generation,old.collation)!=std::tie(it->type,it->nullable,it->defaultExpression,it->primaryKey,it->autoIncrement,it->generatedExpression,it->generation,it->collation))return true;}
    for(const auto& c:after.columns)if(c.originalName.isEmpty() && (c.primaryKey || c.autoIncrement || !c.generatedExpression.isEmpty()))return true;
    return false;
}
QStringList sqliteRebuildSql(const ConnectionProfile& p,const TableDefinition& before,const TableDefinition& after){
    if(!before.detailsLoaded || before.rawSql.isEmpty())fail("请重新读取完整 SQLite 表结构后再修改约束");
    if(before.rawSql.contains(QRegularExpression("\\b(?:PRIMARY\\s+KEY|NOT\\s+NULL)\\s+(?:(?:ASC|DESC)\\s+)?ON\\s+CONFLICT|PRIMARY\\s+KEY\\s+DESC",QRegularExpression::CaseInsensitiveOption)))fail("此 SQLite 表有自定义主键 / NOT NULL 冲突策略，重建前需要保留原始 SQL 策略");
    for(const auto& c:after.columns)if(!c.originalName.isEmpty() && c.name!=c.originalName)fail("SQLite 请先保存字段重命名，再修改字段类型或约束，以正确更新依赖对象");
    auto digest=QString::fromLatin1(QCryptographicHash::hash((before.node.object+before.rawSql).toUtf8(),QCryptographicHash::Sha256).toHex().left(12));auto temporary="__navidog_rebuild_"+digest;
    TableDefinition temp=after;temp.node.object=temporary;QStringList sql{createTableSql(p,temp)};QStringList target,source;
    bool integerKey=false;for(const auto& c:after.columns)if(c.primaryKey && c.type.compare("INTEGER",Qt::CaseInsensitive)==0)integerKey=true;
    if(!before.withoutRowid && !after.withoutRowid && !integerKey){for(auto alias:{"rowid","_rowid_","oid"}){bool used=false;for(const auto& c:before.columns)used|=c.name.compare(alias,Qt::CaseInsensitive)==0;for(const auto& c:after.columns)used|=c.name.compare(alias,Qt::CaseInsensitive)==0;if(!used){target<<quoteIdentifier(alias,p.driver);source<<quoteIdentifier(alias,p.driver);break;}}}
    if(before.rawSql.contains(QRegularExpression("\\b(?:PRIMARY\\s+KEY|NOT\\s+NULL)\\s+(?:(?:ASC|DESC)\\s+)?ON\\s+CONFLICT|PRIMARY\\s+KEY\\s+DESC",QRegularExpression::CaseInsensitiveOption)))fail("此 SQLite 表有自定义主键 / NOT NULL 冲突策略，重建前需要保留原始 SQL 策略");
    for(const auto& c:after.columns)if(!c.originalName.isEmpty() && c.generatedExpression.isEmpty()){target<<quoteIdentifier(c.name,p.driver);source<<quoteIdentifier(c.originalName,p.driver);}
    if(target.isEmpty())fail("重建表至少需要保留一个已有字段");
    auto tempTable=tableQualifiedName(p,temp.node);auto table=tableQualifiedName(p,before.node);const bool sequence=std::any_of(before.columns.begin(),before.columns.end(),[](const auto& c){return c.autoIncrement;});
    auto seq=quoteIdentifier("__navidog_sequence_"+digest,p.driver);if(sequence)sql.prepend("CREATE TEMP TABLE "+seq+" AS SELECT seq FROM sqlite_sequence WHERE name="+lit(before.node.object,p.driver));
    sql<<"INSERT INTO "+tempTable+" ("+target.join(", ")+") SELECT "+source.join(", ")+" FROM "+table;
    sql<<"DROP TABLE "+table<<"ALTER TABLE "+tempTable+" RENAME TO "+quoteIdentifier(after.node.object,p.driver);
    TableDefinition empty;empty.node=after.node;sql<<tableObjectCreates(p,empty,after);
    if(sequence){sql<<"UPDATE sqlite_sequence SET seq=max(seq,COALESCE((SELECT seq FROM temp."+seq+"),0)) WHERE name="+lit(after.node.object,p.driver);sql<<"DROP TABLE temp."+seq;}
    return sql;
}
}
