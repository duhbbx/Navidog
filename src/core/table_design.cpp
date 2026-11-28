#include "navidog/table_design.hpp"
#include "jdbc_catalog.hpp"
#include "navidog/sql.hpp"
#include "table_design_internal.hpp"
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <stdexcept>
namespace navidog {
namespace {
using K=DatabaseKind;
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }
bool mysql(K k) { return navidog::isMySqlFamily(k); }
QString literal(QString s,K kind) {
    if(mysql(kind)) return s.isEmpty() ? "''" : "CONVERT(0x"+QString::fromLatin1(s.toUtf8().toHex())+" USING utf8mb4)";
    if(kind==K::PostgreSQL) s.replace("\\","\\\\");
    s.replace("'","''"); return (kind==K::PostgreSQL ? "E'" : kind==K::SQLServer ? "N'" : "'")+s+"'";
}
sql::Dialect dialect(K k) {
    return mysql(k) ? sql::Dialect::MySQL : usesNativePostgres(k) ? sql::Dialect::PostgreSQL : k==K::Oracle ? sql::Dialect::Oracle : k==K::SQLServer ? sql::Dialect::SQLServer : sql::Dialect::SQLite;
}
void validate(const TableColumn& c,K k) {
    if(c.name.trimmed().isEmpty() || c.name.contains(QChar::Null))fail("字段名不能为空或包含空字符");
    const auto typeTokens=design::tokens(c.type,k);if(typeTokens.isEmpty())fail("请填写字段类型");int typeDepth=0;const auto first=typeTokens.first().text.toUpper();
    const QStringList forbidden{"DEFAULT","NOT","NULL","PRIMARY","UNIQUE","REFERENCES","CHECK","CONSTRAINT","COMMENT","COLLATE","AUTO_INCREMENT","GENERATED","IDENTITY","DROP","CREATE","ALTER","SELECT","INSERT","UPDATE","DELETE"};
    for(const auto& t:typeTokens){if(t.kind==sql::TokenKind::Comment)fail("类型不能包含注释");if(t.kind==sql::TokenKind::Word && forbidden.contains(t.text.toUpper()))fail("类型栏请仅填写类型，属性使用对应控件");if(t.kind==sql::TokenKind::String && first!="ENUM" && first!="SET")fail("类型字符串仅用于 ENUM / SET");if(t.kind==sql::TokenKind::Symbol){if(t.text=="(")++typeDepth;else if(t.text==")"){if(--typeDepth<0)fail("类型括号不匹配");}else if(t.text!="," && t.text!="[" && t.text!="]" && t.text!=".")fail("字段类型格式不支持："+c.type);else if(t.text=="," && typeDepth==0)fail("字段类型必须为单个类型");}}
    if(typeDepth)fail("类型括号不匹配");
    if(!c.generatedExpression.isEmpty()){design::expression(c.generatedExpression,k);if(!c.defaultExpression.isEmpty() || c.autoIncrement)fail("生成字段不能同时设置默认值或自增");}
    if(c.autoIncrement && !c.defaultExpression.isEmpty())fail("自增 / 标识字段不能同时设置默认值");
    if(c.defaultExpression.trimmed().isEmpty())return;
    auto bytes=c.defaultExpression.toUtf8();auto lexed=sql::lex(std::string_view(bytes.constData(),bytes.size()),dialect(k));int depth=0;
    if(!lexed.diagnostics.empty())fail("默认值表达式未闭合");
    for(auto t:lexed.tokens) {
        auto text=bytes.mid(t.begin,t.end-t.begin);
        if(t.kind==sql::TokenKind::Comment)fail("默认值不能包含注释");
        if(t.kind!=sql::TokenKind::Symbol)continue;
        if(text=="(")++depth;
        else if(text==")"){if(--depth<0)fail("默认值括号不匹配");}
        else if(text==";" || (text=="," && depth==0))fail("默认值必须是单个表达式");
    }
    if(depth)fail("默认值括号不匹配");
}
QString columnSql(const TableColumn& c,K k,bool attributes=true) {
    auto out=quoteIdentifier(c.name,k)+" "+c.type.trimmed();
    if(k==K::SQLite && c.autoIncrement){if(c.type.compare("INTEGER",Qt::CaseInsensitive)!=0 || !c.primaryKey)fail("SQLite 自增字段必须为 INTEGER 主键");out+=" PRIMARY KEY AUTOINCREMENT";}
    if(!c.collation.isEmpty())out+=" COLLATE "+quoteIdentifier(c.collation,k);
    if(!c.generatedExpression.isEmpty()) {
        design::expression(c.generatedExpression,k);
        if(k==K::SQLServer)out=quoteIdentifier(c.name,k)+" AS ("+c.generatedExpression+")"+(c.generation=="STORED"?" PERSISTED":"");
        else if(k==K::Oracle)out+=" GENERATED ALWAYS AS ("+c.generatedExpression+") VIRTUAL";
        else out+=" GENERATED ALWAYS AS ("+c.generatedExpression+") "+(c.generation=="VIRTUAL" && k!=K::PostgreSQL?"VIRTUAL":"STORED");
    }else {
        if(c.autoIncrement && k!=K::SQLite){if(mysql(k))out+=" AUTO_INCREMENT";else if(k==K::SQLServer)out+=" IDENTITY(1,1)";else out+=" GENERATED "+QString(c.generation=="ALWAYS"?"ALWAYS":"BY DEFAULT")+" AS IDENTITY";}
        if(!c.defaultExpression.trimmed().isEmpty())out+=" DEFAULT "+c.defaultExpression.trimmed();
    }
    if(!c.nullable || c.primaryKey)out+=" NOT NULL";else if(k!=K::SQLite && c.generatedExpression.isEmpty())out+=" NULL";
    if(attributes && !c.attributes.isEmpty())out+=" "+c.attributes;
    if(mysql(k) && !c.comment.isEmpty())out+=" COMMENT "+design::ddlLiteral(c.comment,k);
    return out;
}
QString mariaDefault(QString text,bool noBackslash,QString& protectedReason){
    if(text.size()<2 || !text.startsWith('\'') || !text.endsWith('\'')){
        if(text.contains('\\'))protectedReason="默认表达式含复杂转义，请使用完整 SQL 修改该字段。";return text;
    }
    QString value;bool valid=true;
    for(int i=1;i<text.size()-1;++i){auto c=text[i];
        if(c=='\''){if(i+1<text.size()-1 && text[i+1]=='\''){value+='\'';++i;}else{valid=false;break;}}
        else if(c=='\\' && !noBackslash){if(i+1<text.size()-1 && (text[i+1]=='\'' || text[i+1]=='\\'))value+=text[++i];else{valid=false;break;}}
        else value+=c;
    }
    if(!valid || value.contains('\\')){protectedReason="默认值含复杂转义，请使用完整 SQL 修改该字段。";return text;}
    value.replace("'","''");return "'"+value+"'";
}
QStringList primary(const TableDefinition& d,bool original=false) {
    QVector<TableColumn> cols;for(auto c:d.columns)if(c.primaryKey)cols<<c;
    std::stable_sort(cols.begin(),cols.end(),[](auto a,auto b){return a.primaryKey<b.primaryKey;});
    QStringList out;for(auto c:cols)out<<(original ? c.originalName : c.name);return out;
}
bool sameProperties(const TableColumn& a,const TableColumn& b) {
    return std::tie(a.type,a.nullable,a.defaultExpression,a.primaryKey,a.comment,a.autoIncrement,a.generatedExpression,a.generation,a.collation,a.attributes)==std::tie(b.type,b.nullable,b.defaultExpression,b.primaryKey,b.comment,b.autoIncrement,b.generatedExpression,b.generation,b.collation,b.attributes);
}
QueryResult checked(Database& db,const QString& sql,const Cancellation& cancel) {
    auto r=db.execute(sql,{},cancel);if(r.cancelled)fail("操作已停止");if(!r.error.isEmpty())fail(r.error);
    if(r.truncated || r.cellTruncated)fail("表结构超过读取上限，未加载不完整结构");return r;
}
}
QString tableQualifiedName(const ConnectionProfile& p,const CatalogNode& n) {
    QStringList parts;
    if(mysql(p.driver) && !n.database.isEmpty())parts<<n.database;
    else if(!n.schema.isEmpty())parts<<n.schema;
    parts<<n.object;for(auto& part:parts)part=quoteIdentifier(part,p.driver);return parts.join('.');
}
TableDefinition readTableDefinition(Database& db,const ConnectionProfile& p,const CatalogNode& node,const Cancellation& cancel,bool details) {
    if(usesJdbcCatalog(p.driver))return readJdbcTable(db,node,cancel);
    TableDefinition out;out.node=node;out.exists=true;const auto k=p.driver;
    auto lit=[&](QString x){return literal(x,k);};
    auto filter=" WHERE table_schema="+lit(node.schema)+" AND table_name="+lit(node.object);
    QueryResult r;
    if(k==K::SQLite) {
        r=checked(db,"SELECT name,type,[notnull],dflt_value,pk,hidden FROM pragma_table_xinfo("+lit(node.object)+","+lit(node.schema.isEmpty()?"main":node.schema)+") ORDER BY cid",cancel);
        for(auto row:r.rows) {TableColumn c;c.originalName=c.name=row[0].toString();c.type=row[1].toString();if(c.type.isEmpty())c.type="BLOB";c.nullable=!row[2].toBool();c.defaultExpression=row[3].toString();c.primaryKey=row[4].toInt();if(c.primaryKey)c.nullable=false;if(row[5].toInt())c.protectedReason="生成字段请使用 SQL 修改";out.columns<<c;}
    } else if(mysql(k)) {
        if(k==K::MariaDB){auto mode=checked(db,"SELECT @@SESSION.sql_mode",cancel);if(!mode.rows.isEmpty())out.sqlMode=mode.rows[0][0].toString();}
        filter=" WHERE table_schema="+lit(node.database)+" AND table_name="+lit(node.object);
        r=checked(db,"SELECT column_name,column_type,is_nullable,column_default,column_key,extra,collation_name,column_comment,generation_expression FROM information_schema.columns"+filter+" ORDER BY ordinal_position",cancel);
        int pk=0;
        for(auto row:r.rows) {
            TableColumn c;c.originalName=c.name=row[0].toString();c.type=row[1].toString();c.nullable=row[2].toString()=="YES";if(row[4].toString()=="PRI")c.primaryKey=++pk;
            auto extra=row[5].toString();
            if(row[3].isValid()) {
                auto d=row[3].toString();
                const bool temporal=QRegularExpression("^(CURRENT_TIMESTAMP|CURRENT_DATE|CURRENT_TIME)(\\([0-9]*\\))?$",QRegularExpression::CaseInsensitiveOption).match(d).hasMatch();
                const bool numericType=QRegularExpression("^(tinyint|smallint|mediumint|int|integer|bigint|decimal|numeric|float|double|real|bit|boolean|bool)\\b",QRegularExpression::CaseInsensitiveOption).match(c.type).hasMatch();
                if(k==K::MariaDB)c.defaultExpression=mariaDefault(d,out.sqlMode.split(',').contains("NO_BACKSLASH_ESCAPES"),c.protectedReason);
                else if(temporal)c.defaultExpression=d;
                else if(extra.contains("DEFAULT_GENERATED",Qt::CaseInsensitive))c.defaultExpression="("+design::mysqlMetadataExpression(d,c.protectedReason)+")";
                else if(numericType && QRegularExpression("^(?:-?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?|[bB]'[01]+')$").match(d).hasMatch())c.defaultExpression=d;
                else if(numericType){c.defaultExpression=d;c.protectedReason="复杂数值默认值请使用 SQL 修改";}
                else if(d.isEmpty())c.defaultExpression="''";
                else c.defaultExpression="0x"+QString::fromLatin1(d.toUtf8().toHex());
            }
            extra.remove(QRegularExpression("\\bDEFAULT_GENERATED\\b",QRegularExpression::CaseInsensitiveOption));
            c.collation=row[6].toString();
            c.attributes+=extra.trimmed();
            c.comment=row[7].toString();c.autoIncrement=extra.contains("auto_increment",Qt::CaseInsensitive);c.attributes.remove(QRegularExpression("\\bauto_increment\\b",QRegularExpression::CaseInsensitiveOption));
            if(extra.contains("GENERATED",Qt::CaseInsensitive)){if(row.size()>8 && !row[8].toString().isEmpty()){c.generatedExpression=row[8].toString();if(k==K::MySQL)c.generatedExpression=design::mysqlMetadataExpression(c.generatedExpression,c.protectedReason);else if(k==K::MariaDB && c.generatedExpression.contains('\\'))c.protectedReason="生成表达式含复杂转义，请使用完整 SQL 修改该字段。";c.generation=extra.contains("STORED",Qt::CaseInsensitive)?"STORED":"VIRTUAL";c.attributes.remove(QRegularExpression("\\b(?:VIRTUAL|STORED)\\s+GENERATED\\b",QRegularExpression::CaseInsensitiveOption));}else c.protectedReason="生成字段定义不可见";}
            out.columns<<c;
        }
    } else if(usesNativePostgres(k)) {
        r=checked(db,"SELECT a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull::int,pg_get_expr(d.adbin,d.adrelid),COALESCE(array_position(i.indkey::smallint[],a.attnum)+1,0),a.attidentity::text,a.attgenerated::text FROM pg_attribute a JOIN pg_class t ON t.oid=a.attrelid JOIN pg_namespace s ON s.oid=t.relnamespace LEFT JOIN pg_attrdef d ON d.adrelid=t.oid AND d.adnum=a.attnum LEFT JOIN pg_index i ON i.indrelid=t.oid AND i.indisprimary WHERE s.nspname="+lit(node.schema)+" AND t.relname="+lit(node.object)+" AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",cancel);
        for(auto row:r.rows) {TableColumn c;c.originalName=c.name=row[0].toString();c.type=row[1].toString();c.nullable=!row[2].toBool();c.defaultExpression=row[3].toString();c.primaryKey=row[4].toInt();if(!row[5].toString().isEmpty()){c.autoIncrement=true;c.generation=row[5].toString()=="a"?"ALWAYS":"BY DEFAULT";if(!details)c.protectedReason="标识字段使用数据库默认值";}if(!row[6].toString().isEmpty()){c.generatedExpression=c.defaultExpression;c.defaultExpression.clear();c.generation="STORED";c.protectedReason="已有生成表达式请使用引擎支持的 SQL 修改";}out.columns<<c;}
    } else if(k==K::Oracle) {
        r=checked(db,"SELECT c.column_name,c.data_type,c.nullable,c.data_default,c.data_precision,c.data_scale,c.char_length,c.char_used,COALESCE(pk.position,0),c.virtual_column FROM all_tab_cols c LEFT JOIN (SELECT cc.owner,cc.table_name,cc.column_name,cc.position FROM all_cons_columns cc JOIN all_constraints ac ON ac.owner=cc.owner AND ac.constraint_name=cc.constraint_name WHERE ac.constraint_type='P') pk ON pk.owner=c.owner AND pk.table_name=c.table_name AND pk.column_name=c.column_name WHERE c.owner="+lit(node.schema)+" AND c.table_name="+lit(node.object)+" AND c.hidden_column='NO' ORDER BY c.column_id",cancel);
        for(auto row:r.rows) {TableColumn c;c.originalName=c.name=row[0].toString();c.type=row[1].toString();c.nullable=row[2].toString()=="Y";c.defaultExpression=row[3].toString().trimmed();c.primaryKey=row[8].toInt();if(c.type=="NUMBER" && row[4].isValid())c.type+="("+row[4].toString()+","+QString::number(row[5].toInt())+")";if(c.type=="VARCHAR2" || c.type=="CHAR" || c.type=="NVARCHAR2" || c.type=="NCHAR")c.type+="("+row[6].toString()+QString((c.type=="VARCHAR2" || c.type=="CHAR") ? (row[7].toString()=="C" ? " CHAR" : " BYTE") : "")+")";if(row[9].toString()=="YES" || c.defaultExpression.contains("NEXTVAL",Qt::CaseInsensitive))c.protectedReason="标识 / 生成字段请使用 SQL 修改";out.columns<<c;}
    } else if(k==K::SQLServer) {
        r=checked(db,"SELECT c.name,TYPE_NAME(c.user_type_id),c.is_nullable,d.definition,c.max_length,c.precision,c.scale,c.is_identity,c.is_computed,COALESCE(ic.key_ordinal,0),c.collation_name FROM sys.columns c LEFT JOIN sys.default_constraints d ON d.object_id=c.default_object_id LEFT JOIN sys.indexes i ON i.object_id=c.object_id AND i.is_primary_key=1 LEFT JOIN sys.index_columns ic ON ic.object_id=c.object_id AND ic.index_id=i.index_id AND ic.column_id=c.column_id WHERE c.object_id=OBJECT_ID("+lit(tableQualifiedName(p,node))+") ORDER BY c.column_id",cancel);
        for(auto row:r.rows) {TableColumn c;c.originalName=c.name=row[0].toString();c.type=row[1].toString();c.nullable=row[2].toBool();c.defaultExpression=row[3].toString();c.primaryKey=row[9].toInt();auto t=c.type.toLower();if(QStringList{"varchar","nvarchar","char","nchar","binary","varbinary"}.contains(t)){int len=row[4].toInt();c.type+="("+QString(len==-1 ? "MAX" : QString::number(len/(t.startsWith('n')?2:1)))+")";}else if(t=="decimal" || t=="numeric")c.type+="("+row[5].toString()+","+row[6].toString()+")";if(row[7].toBool() || row[8].toBool())c.protectedReason="标识 / 计算字段请使用 SQL 修改";if(!row[10].toString().isEmpty())c.attributes="COLLATE "+quoteIdentifier(row[10].toString(),k);out.columns<<c;}
    } else fail("该数据库暂不支持表设计");
    if(out.columns.isEmpty())fail("未读取到表字段，请确认表存在且账号具有查看权限");
    if(details && node.kind!="view")readTableDetails(db,p,out,cancel);
    return out;
}
QString createTableSql(const ConnectionProfile& p,const TableDefinition& after) {
    int autoCount=0,keyCount=0;for(const auto& c:after.columns){autoCount+=c.autoIncrement;keyCount+=c.primaryKey>0;}if(autoCount>1 && (mysql(p.driver) || p.driver==K::SQLite || p.driver==K::SQLServer))fail("此引擎一张表最多一个自增 / 标识字段");if(p.driver==K::SQLite && autoCount && after.withoutRowid)fail("WITHOUT ROWID 表不能使用 AUTOINCREMENT");if(p.driver==K::SQLite && autoCount && keyCount!=1)fail("SQLite 自增字段必须是唯一的 INTEGER 主键");
    QStringList cols;bool inlineKey=false;for(const auto& c:after.columns){cols<<columnSql(c,p.driver);inlineKey|=p.driver==K::SQLite && c.autoIncrement;}
    auto keys=primary(after);for(auto& key:keys)key=quoteIdentifier(key,p.driver);
    if(!keys.isEmpty() && !inlineKey)cols<<(after.primaryKeyName.isEmpty()?QString{}:"CONSTRAINT "+quoteIdentifier(after.primaryKeyName,p.driver)+" ")+"PRIMARY KEY ("+keys.join(", ")+")";
    cols<<tableConstraints(p,after);return "CREATE TABLE "+tableQualifiedName(p,after.node)+" (\n    "+cols.join(",\n    ")+"\n)"+tableOptionsSql(p,after);
}
QStringList tableChangeSql(const ConnectionProfile& p,const TableDefinition& before,const TableDefinition& input) {
    auto after=input;
    if(mysql(p.driver) && before.exists){QMap<QString,QString> names;for(const auto& c:after.columns)if(!c.originalName.isEmpty() && c.name!=c.originalName)names[c.originalName]=c.name;
        for(auto& o:after.objects){auto old=std::find_if(before.objects.begin(),before.objects.end(),[&](const auto& a){return a.kind==o.kind && a.name==o.originalName;});if(old==before.objects.end())continue;
            if(o.kind==TableObjectKind::Check && o.expression==old->expression){o.expression=design::renameIdentifiers(o.expression,names,p.driver);if(o.expression!=old->expression)o.sql.clear();}
            if(o.kind==TableObjectKind::ForeignKey && o.columns==old->columns)for(auto& column:o.columns)if(names.contains(column))column=names[column];
        }
    }
    const auto k=p.driver;const auto table=tableQualifiedName(p,after.node);QStringList statements;QSet<QString> names,origins;
    if(after.node.object.trimmed().isEmpty() || after.node.object.contains(QChar::Null))fail("请填写表名");
    if(after.columns.isEmpty())fail("至少需要一个字段");
    if(after.columns.size()>512)fail("表设计最多支持 512 个字段");
    for(const auto& c:after.columns) {
        auto name=c.name.toCaseFolded();if(names.contains(name))fail("字段名重复："+c.name);names.insert(name);
        if(!c.originalName.isEmpty()){if(origins.contains(c.originalName))fail("字段来源重复");origins.insert(c.originalName);}
        auto old=std::find_if(before.columns.begin(),before.columns.end(),[&](auto a){return a.name==c.originalName;});
        if(!c.originalName.isEmpty() && old==before.columns.end())fail("字段来源不在原始表结构中，请重新读取");
        if(old==before.columns.end() || old->name!=c.name || !sameProperties(*old,c))validate(c,k);
    }
    if(usesJdbcCatalog(k) && (before.objects!=after.objects || before.comment!=after.comment))
        fail("此 JDBC 驱动的表设计支持字段和主键。其他表对象请用 SQL 修改。");
    if(!before.exists){TableDefinition empty;return QStringList{createTableSql(p,after)}+tableObjectCreates(p,empty,after)+tablePropertySql(p,empty,after);}
    if(k==K::SQLite && requiresSqliteRebuild(before,after))return sqliteRebuildSql(p,before,after);
    statements=tableObjectDrops(p,before,after);
    if(before.node.object!=after.node.object || before.node.schema!=after.node.schema || before.node.database!=after.node.database)fail("修改页的表与数据库不可更改");
    if(!mysql(k) && k!=K::SQLite && design::existingColumnOrder(before,after)!=design::columnOrder(after))fail("该引擎不支持原地修改字段顺序");
    // Renames must not collide with a still-existing column in the operation order.
    for(auto c:after.columns)if(!c.originalName.isEmpty() && c.originalName!=c.name)
        for(auto old:before.columns)if(old.name.compare(c.name,Qt::CaseInsensitive)==0)fail("重命名的目标字段名已存在，请分次修改");
    // Existing primary keys, identity, generated fields and dependencies need richer editors.
    const bool keyChanged=primary(before)!=primary(after,true);
    if(keyChanged && !primary(before).isEmpty()){
        if(mysql(k))statements<<"ALTER TABLE "+table+" DROP PRIMARY KEY";
        else {if(before.primaryKeyName.isEmpty())fail("请重新读取主键约束名称");statements<<"ALTER TABLE "+table+" DROP CONSTRAINT "+quoteIdentifier(before.primaryKeyName,k);}
    }
    for(auto old:before.columns) {
        auto now=std::find_if(after.columns.begin(),after.columns.end(),[&](auto a){return a.originalName==old.name;});
        if(now==after.columns.end()) {
            if(!old.protectedReason.isEmpty())fail(old.protectedReason);
            statements<<"ALTER TABLE "+table+" DROP COLUMN "+quoteIdentifier(old.name,k);continue;
        }
        bool renamed=now->name!=old.name,changed=!sameProperties(old,*now);
        if(!renamed && !changed)continue;
        auto withoutComment=*now;withoutComment.comment=old.comment;
        if(usesJdbcCatalog(k) && old.comment!=now->comment)fail("请通过 SQL 修改字段注释");
        if(!renamed && sameProperties(old,withoutComment) && !mysql(k))continue;
        if(!old.protectedReason.isEmpty())fail(old.protectedReason);
        auto column=quoteIdentifier(now->name,k);
        if(mysql(k)) {
            auto c=*now;c.attributes=old.attributes;
            statements<<"ALTER TABLE "+table+" CHANGE COLUMN "+quoteIdentifier(old.name,k)+" "+columnSql(c,k);continue;
        }
        if(renamed) {
            if(k==K::SQLServer)statements<<"EXEC sys.sp_rename @objname="+literal(table+"."+quoteIdentifier(old.name,k),k)+", @newname="+literal(now->name,k)+", @objtype=N'COLUMN'";
            else statements<<"ALTER TABLE "+table+" RENAME COLUMN "+quoteIdentifier(old.name,k)+" TO "+column;
        }
        if(!changed)continue;
        if(k==K::SQLite)fail("当前 SQLite 支持增删字段及重命名；修改字段类型、默认值或约束需通过 SQL 重建表");

        if(old.primaryKey && old.nullable!=now->nullable)fail("主键字段不能允许 NULL");
        if(usesNativePostgres(k)) {
            if(old.autoIncrement!=now->autoIncrement)statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+(now->autoIncrement?" ADD GENERATED "+QString(now->generation=="ALWAYS"?"ALWAYS":"BY DEFAULT")+" AS IDENTITY":" DROP IDENTITY");
            else if(old.autoIncrement && old.generation!=now->generation)statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+" SET GENERATED "+QString(now->generation=="ALWAYS"?"ALWAYS":"BY DEFAULT");
            if(old.type!=now->type)statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+" TYPE "+now->type;
            if(old.defaultExpression!=now->defaultExpression)statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+(now->defaultExpression.isEmpty()?" DROP DEFAULT":" SET DEFAULT "+now->defaultExpression);
            if(old.nullable!=now->nullable)statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+(now->nullable?" DROP NOT NULL":" SET NOT NULL");
        } else if(usesJdbcCatalog(k)) {
            if(old.autoIncrement!=now->autoIncrement || old.generation!=now->generation)
                fail("请通过 SQL 修改生成字段");
            if(old.type!=now->type)
                statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+
                    (usesPostgresCompatibleSql(k) ? " TYPE " : " SET DATA TYPE ")+now->type;
            if(old.defaultExpression!=now->defaultExpression)
                statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+
                    (now->defaultExpression.isEmpty() ? " DROP DEFAULT" : " SET DEFAULT "+now->defaultExpression);
            if(old.nullable!=now->nullable)
                statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+(now->nullable ? " DROP NOT NULL" : " SET NOT NULL");
        } else if(k==K::Oracle) {
            QStringList changes;
            if(old.type!=now->type)changes<<now->type;
            if(old.defaultExpression!=now->defaultExpression)changes<<"DEFAULT "+(now->defaultExpression.isEmpty()?QString("NULL"):now->defaultExpression);
            if(old.nullable!=now->nullable)changes<<(now->nullable?"NULL":"NOT NULL");
            statements<<"ALTER TABLE "+table+" MODIFY ("+column+" "+changes.join(' ')+")";
        } else if(k==K::SQLServer) {
            if(old.defaultExpression!=now->defaultExpression){
                if(!old.defaultConstraint.isEmpty())statements<<"ALTER TABLE "+table+" DROP CONSTRAINT "+quoteIdentifier(old.defaultConstraint,k);
                if(!now->defaultExpression.isEmpty())statements<<"ALTER TABLE "+table+" ADD DEFAULT "+now->defaultExpression+" FOR "+column;
            }
            statements<<"ALTER TABLE "+table+" ALTER COLUMN "+column+" "+now->type+" "+old.attributes+(now->nullable?" NULL":" NOT NULL");
        }
    }
    for(auto c:after.columns)if(c.originalName.isEmpty()) {

        statements<<"ALTER TABLE "+table+(k==K::Oracle?" ADD (":" ADD ")+columnSql(c,k,false)+(k==K::Oracle?")":"");
    }
    if(keyChanged && !primary(after).isEmpty()){
        auto keys=primary(after);for(auto& key:keys)key=quoteIdentifier(key,k);
        statements<<"ALTER TABLE "+table+" ADD "+(before.primaryKeyName.isEmpty()?QString{}:"CONSTRAINT "+quoteIdentifier(before.primaryKeyName,k)+" ")+"PRIMARY KEY ("+keys.join(", ")+")";
    }
    if(mysql(k)){
        auto current=design::existingColumnOrder(before,after);auto desired=design::columnOrder(after);
        for(int i=0;i<desired.size();++i){int at=current.indexOf(desired[i]);if(at==i)continue;const auto& c=after.columns[i];if(!c.protectedReason.isEmpty())fail(c.protectedReason);statements<<"ALTER TABLE "+table+" MODIFY COLUMN "+columnSql(c,k)+(i==0?" FIRST":" AFTER "+quoteIdentifier(desired[i-1],k));current.move(at,i);}
    }
    statements<<tableObjectCreates(p,before,after)<<tablePropertySql(p,before,after);
    if(mysql(k)){
        QStringList combined,remaining;int anchor=-1;auto prefix="ALTER TABLE "+table+" ";auto suffix=" ON "+table;
        for(const auto& statement:statements){QString clause;if(statement.startsWith(prefix) && !statement.mid(prefix.size()).startsWith("DROP FOREIGN KEY ") && !statement.mid(prefix.size()).startsWith("DROP CHECK ") && !statement.mid(prefix.size()).startsWith("ADD CONSTRAINT "))clause=statement.mid(prefix.size());else if(statement.startsWith("DROP INDEX ") && statement.endsWith(suffix))clause="DROP INDEX "+statement.mid(11,statement.size()-11-suffix.size());
            else if(QRegularExpression("^CREATE (?:UNIQUE |FULLTEXT |SPATIAL )?INDEX ").match(statement).hasMatch() && statement.contains(" ON "+table+" ")){auto pos=statement.indexOf(" ON "+table+" ");clause="ADD "+statement.mid(7,pos-7)+statement.mid(pos+4+table.size());}
            if(clause.isEmpty())remaining<<statement;else{if(anchor<0)anchor=remaining.size();combined<<clause;}
        }
        if(!combined.isEmpty())remaining.insert(qMax(0,anchor),prefix+combined.join(",\n    "));return remaining;
    }
    return statements;
}
TableDesignResult applyTableChanges(const ConnectionProfile& p,const TableDefinition& before,const TableDefinition& after,const Cancellation& cancel) {
    TableDesignResult result;result.definition=before;bool transaction=false,commitAttempted=false;std::unique_ptr<Database> db;
    try {
        if(p.http.enabled)fail("HTTP 隧道不支持表设计会话，请使用直连或 SSH 保存结构");
        if(p.readOnly)fail("只读连接无法保存表结构");
        auto statements=tableChangeSql(p,before,after);if(statements.isEmpty()){result.definition=after;return result;}
        if(cancel->load()) {result.cancelled=true;return result;}
        db=connectDatabase(p);
        const bool rebuild=p.driver==K::SQLite && before.exists && requiresSqliteRebuild(before,after);
        if(rebuild){checked(*db,"PRAGMA foreign_keys=OFF",cancel);checked(*db,"PRAGMA legacy_alter_table=ON",cancel);}
        if(p.driver==K::SQLite || usesNativePostgres(p.driver) || p.driver==K::SQLServer) {checked(*db,p.driver==K::SQLServer?"BEGIN TRANSACTION":p.driver==K::SQLite?"BEGIN IMMEDIATE":"BEGIN",cancel);transaction=true;}
        if(before.exists && usesNativePostgres(p.driver)){checked(*db,"SET LOCAL lock_timeout=5000",cancel);checked(*db,"LOCK TABLE "+tableQualifiedName(p,before.node)+" IN ACCESS EXCLUSIVE MODE",cancel);}
        if(before.exists && (!before.detailsLoaded || !sameTableStructure(readTableDefinition(*db,p,before.node,cancel),before)))fail("表结构已被其他操作修改，请重新读取后再保存");
        QString mysqlMode;
        if(mysql(p.driver)){auto mode=checked(*db,"SELECT @@SESSION.sql_mode",cancel);mysqlMode=mode.rows.isEmpty()?QString{}:mode.rows[0][0].toString();}
        for(auto sql:statements) {if(cancel->load())fail("保存已停止");
            if(mysql(p.driver)){auto mode=mysqlMode;bool originalTrigger=false;for(const auto& object:after.objects)if(object.kind==TableObjectKind::Trigger && object.sql==sql && !object.sqlMode.isEmpty()){mode=object.sqlMode;originalTrigger=true;break;}if(!originalTrigger && !mode.split(',').contains("NO_BACKSLASH_ESCAPES"))mode+=(mode.isEmpty()?QString{}:",")+"NO_BACKSLASH_ESCAPES";checked(*db,"SET SESSION sql_mode="+literal(mode,p.driver),cancel);}
            auto r=db->executeDefinition(sql,{},cancel);if(r.cancelled)fail("保存已停止");if(!r.error.isEmpty())fail(r.error);++result.appliedStatements;}
        if(mysql(p.driver))checked(*db,"SET SESSION sql_mode="+literal(mysqlMode,p.driver),cancel);
        if(rebuild && !checked(*db,"PRAGMA foreign_key_check",cancel).rows.isEmpty())fail("修改后存在外键不一致，已取消保存");
        if(transaction) {commitAttempted=true;checked(*db,"COMMIT",cancel);transaction=false;}
        result.definition=after;result.definition.exists=true;
        // Do not report committed DDL as failed merely because metadata refresh failed.
        try {result.definition=readTableDefinition(*db,p,after.node,cancel);}catch(const std::exception& e) {result.refreshError=QString::fromUtf8(e.what());result.definition.detailsLoaded=false;for(auto& c:result.definition.columns)c.originalName=c.name;for(auto& o:result.definition.objects)o.originalName=o.name;}
    } catch(const std::exception& e) {
        result.error=QString::fromUtf8(e.what());result.cancelled=cancel->load();
        if(transaction && db) {try {checked(*db,"ROLLBACK",cancellation());result.rolledBack=true;}catch(...) {db.reset();if(!commitAttempted)result.rolledBack=true;} }
        result.partial=result.appliedStatements>0 && !result.rolledBack;
    }
    return result;
}
}
