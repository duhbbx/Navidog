#include "navidog/er_model.hpp"
#include "table_design_internal.hpp"
#include <QRegularExpression>
#include <QVersionNumber>
#include <algorithm>

namespace navidog::er {
namespace {
using K = DatabaseKind;
using Rows = QVector<QVector<QVariant>>;
constexpr int pageSize = 256;

struct Queries {
    QString objects, columns, foreignKeys, indexes, checks;
    QString name;
    bool fetchFirst = false, checksPartial = false;
};

struct BatchLimit : std::runtime_error {
    BatchLimit() : std::runtime_error("目录查询达到读取上限，请缩小 Schema 或表名范围后重新导入") {}
};

Rows read(Database& database,const QString& query,const Cancellation& cancel,
          int maxRows = 100000) {
    if (cancel->load()) design::fail("逆向导入已停止");
    QueryLimits limits;
    limits.maxRows = maxRows;
    limits.maxBytes = 48LL*1024*1024;
    limits.maxCellBytes = 1024*1024;
    auto result = database.execute(query,limits,cancel);
    if (result.cancelled || cancel->load()) design::fail("逆向导入已停止");
    if (!result.error.isEmpty()) design::fail(result.error);
    if (result.truncated || result.cellTruncated) throw BatchLimit{};
    return std::move(result.rows);
}

QVersionNumber numericVersion(const QString& text, K kind) {
    return QVersionNumber::fromString(kind == K::MariaDB && text.startsWith("5.5.5-") ? text.mid(6) : text);
}

bool truth(const QVariant& value) {
    const auto text = value.toString().toLower();
    return text == "1" || text == "t" || text == "true" || text == "yes";
}

// Every child query is scoped to one page of catalog names and ordered by
// table, object, and ordinal. This preserves compound keys without per-table requests.
Queries sqliteQueries(bool views) {
    Queries q;
    q.name = "m.name";
    const QString objects = " FROM main.sqlite_schema m WHERE m.name NOT GLOB 'sqlite_*'";
    q.objects = "SELECT m.name,CASE WHEN m.type='view' THEN 1 ELSE 0 END,m.sql,''" + objects +
                (views ? " AND m.type IN ('table','view')" : " AND m.type='table'");
    q.columns = "SELECT m.name,c.name,c.type,1-c.[notnull],c.dflt_value,c.pk,'','',c.hidden,''"
                " FROM main.sqlite_schema m JOIN pragma_table_xinfo(m.name,'main') c WHERE m.name IN ($names)"
                " ORDER BY m.name,c.cid";
    q.foreignKeys = "SELECT m.name,'fk_'||f.id,f.[from],'',f.[table],f.[to],f.on_delete,f.on_update,1,0,0"
                    " FROM main.sqlite_schema m JOIN pragma_foreign_key_list(m.name,'main') f"
                    " WHERE m.name IN ($names) AND m.type='table' ORDER BY m.name,f.id,f.seq";
    q.indexes = "SELECT m.name,i.name,i.[unique],i.origin='u',x.name,x.[desc],i.partial,s.sql"
                " FROM main.sqlite_schema m JOIN pragma_index_list(m.name,'main') i"
                " JOIN pragma_index_xinfo(i.name,'main') x LEFT JOIN main.sqlite_schema s ON s.name=i.name"
                " WHERE m.name IN ($names) AND i.origin<>'pk' AND x.key=1 ORDER BY m.name,i.name,x.seqno";
    return q;
}

Queries mysqlQueries(K kind,const QString& scope,bool views,const QVersionNumber& version) {
    Queries q;
    q.name = "BINARY t.table_name";
    q.objects = "SELECT t.table_name,t.table_type='VIEW',v.view_definition,t.table_comment"
                " FROM information_schema.tables t LEFT JOIN information_schema.views v"
                " ON v.table_schema=t.table_schema AND v.table_name=t.table_name WHERE t.table_schema="+scope+
                (views ? " AND t.table_type IN ('BASE TABLE','VIEW')" : " AND t.table_type='BASE TABLE'");
    q.columns = "SELECT c.table_name,c.column_name,c.column_type,c.is_nullable='YES',c.column_default,"
                "COALESCE(p.ordinal_position,0),c.extra,c.generation_expression,0,c.column_comment,c.collation_name"
                " FROM information_schema.columns c LEFT JOIN information_schema.key_column_usage p"
                " ON p.table_schema=c.table_schema AND p.table_name=c.table_name AND p.column_name=c.column_name"
                " AND p.constraint_name='PRIMARY' WHERE c.table_schema="+scope+
                " AND c.table_name IN ($names) ORDER BY BINARY c.table_name,c.ordinal_position";
    q.foreignKeys = "SELECT k.table_name,k.constraint_name,k.column_name,k.referenced_table_schema,"
                    "k.referenced_table_name,k.referenced_column_name,r.delete_rule,r.update_rule,1,0,0"
                    " FROM information_schema.key_column_usage k JOIN information_schema.referential_constraints r"
                    " ON r.constraint_schema=k.constraint_schema AND r.constraint_name=k.constraint_name AND r.table_name=k.table_name"
                    " WHERE k.table_schema="+scope+" AND k.table_name IN ($names)"
                    " ORDER BY BINARY k.table_name,k.constraint_name,k.ordinal_position";
    q.indexes = "SELECT t.table_name,t.index_name,1-t.non_unique,0,t.column_name,t.collation='D',0,'',t.sub_part,t.index_type"
                " FROM information_schema.statistics t WHERE t.table_schema="+scope+
                " AND t.table_name IN ($names) AND t.index_name<>'PRIMARY'"
                " ORDER BY BINARY t.table_name,t.index_name,t.seq_in_index";
    if (kind == K::MySQL && version >= QVersionNumber(8,0,16)) {
        q.checks = "SELECT t.table_name,t.constraint_name,c.check_clause,t.enforced='YES'"
                   " FROM information_schema.table_constraints t JOIN information_schema.check_constraints c"
                   " ON c.constraint_schema=t.constraint_schema AND c.constraint_name=t.constraint_name"
                   " WHERE t.table_schema="+scope+" AND t.table_name IN ($names) AND t.constraint_type='CHECK'";
    } else if (kind == K::MariaDB && version >= QVersionNumber(10,3)) {
        q.checks = "SELECT table_name,constraint_name,check_clause,1 FROM information_schema.check_constraints"
                   " WHERE constraint_schema="+scope+" AND table_name IN ($names)";
    } else {
        q.checksPartial = kind != K::MySQL || version.isNull();
    }
    return q;
}

Queries postgresQueries(const QString& scope,bool views,const QVersionNumber& version) {
    Queries q;
    q.name = "t.relname";
    const QString tables = " FROM pg_class t JOIN pg_namespace s ON s.oid=t.relnamespace";
    const auto schema = " WHERE s.nspname="+scope;
    const auto inSchema = schema+" AND t.relname IN ($names)";
    q.objects = "SELECT t.relname,t.relkind='v',CASE WHEN t.relkind='v' THEN pg_get_viewdef(t.oid,false) ELSE '' END,"
                "COALESCE(obj_description(t.oid,'pg_class'),'')" + tables + schema +
                (views ? " AND t.relkind IN ('r','p','v')" : " AND t.relkind IN ('r','p')");
    const QString identity = version.majorVersion() >= 10 ? "a.attidentity::text" : "''";
    const QString generated = version.majorVersion() >= 12 ? "a.attgenerated::text" : "''";
    q.columns = "SELECT t.relname,a.attname,format_type(a.atttypid,a.atttypmod),NOT a.attnotnull,"
                "pg_get_expr(d.adbin,d.adrelid),COALESCE(array_position(i.indkey::smallint[],a.attnum)+1,0),"
                + identity + "," + generated + ",0,COALESCE(col_description(t.oid,a.attnum),''),'',p.conname" + tables +
                " JOIN pg_attribute a ON a.attrelid=t.oid LEFT JOIN pg_attrdef d ON d.adrelid=t.oid AND d.adnum=a.attnum"
                " LEFT JOIN pg_index i ON i.indrelid=t.oid AND i.indisprimary"
                " LEFT JOIN pg_constraint p ON p.conrelid=t.oid AND p.contype='p'"+
                inSchema+" AND a.attnum>0 AND NOT a.attisdropped ORDER BY t.relname,a.attnum";
    const auto action = [](const QString& field) {
        return "CASE c."+field+" WHEN 'a' THEN 'NO ACTION' WHEN 'r' THEN 'RESTRICT' WHEN 'c' THEN 'CASCADE'"
               " WHEN 'n' THEN 'SET NULL' WHEN 'd' THEN 'SET DEFAULT' END";
    };
    q.foreignKeys = "SELECT t.relname,c.conname,a.attname,rs.nspname,rt.relname,b.attname,"
                    + action("confdeltype") + ","+action("confupdtype")+
                    ",c.convalidated,c.condeferrable,c.condeferred"+tables+
                    " JOIN pg_constraint c ON c.conrelid=t.oid AND c.contype='f'"
                    " JOIN pg_class rt ON rt.oid=c.confrelid JOIN pg_namespace rs ON rs.oid=rt.relnamespace"
                    " CROSS JOIN LATERAL unnest(c.conkey,c.confkey) WITH ORDINALITY k(source,target,position)"
                    " JOIN pg_attribute a ON a.attrelid=t.oid AND a.attnum=k.source"
                    " JOIN pg_attribute b ON b.attrelid=rt.oid AND b.attnum=k.target" + inSchema +
                    " ORDER BY t.relname,c.conname,k.position";
    const QString distinctKeys = version.majorVersion() >= 11 ? "i.indnkeyatts" : "i.indnatts";
    q.indexes = "SELECT t.relname,COALESCE(c.conname,n.relname),i.indisunique,c.contype='u',a.attname,0,"
                "i.indpred IS NOT NULL,pg_get_indexdef(n.oid),0,am.amname,pg_get_indexdef(n.oid,k.position::int,true)"
                + tables + " JOIN pg_index i ON i.indrelid=t.oid AND NOT i.indisprimary"
                " JOIN pg_class n ON n.oid=i.indexrelid JOIN pg_am am ON am.oid=n.relam"
                " LEFT JOIN pg_constraint c ON c.conindid=n.oid AND c.conrelid=t.oid AND c.contype='u'"
                " CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY k(attnum,position)"
                " LEFT JOIN pg_attribute a ON a.attrelid=t.oid AND a.attnum=k.attnum" + inSchema +
                " AND k.position<="+distinctKeys+" ORDER BY t.relname,n.relname,k.position";
    q.checks = "SELECT t.relname,c.conname,pg_get_expr(c.conbin,c.conrelid),c.convalidated" + tables +
               " JOIN pg_constraint c ON c.conrelid=t.oid AND c.contype='c'"+inSchema;
    return q;
}

Queries oracleQueries(const QString& scope,bool views) {
    Queries q;
    q.name = "t.object_name";
    q.fetchFirst = true;
    q.objects = "SELECT t.object_name,CASE WHEN t.object_type='VIEW' THEN 1 ELSE 0 END,NULL,c.comments"
                " FROM all_objects t LEFT JOIN all_tab_comments c ON c.owner=t.owner AND c.table_name=t.object_name"
                " WHERE t.owner="+scope+(views ? " AND t.object_type IN ('TABLE','VIEW')" : " AND t.object_type='TABLE'");
    q.columns = "SELECT a.table_name,a.column_name,"
                "CASE WHEN a.data_type='NUMBER' AND a.data_precision IS NOT NULL THEN a.data_type||'('||a.data_precision||','||a.data_scale||')'"
                " WHEN a.data_type IN ('CHAR','VARCHAR2','NCHAR','NVARCHAR2') THEN a.data_type||'('||a.char_length||')'"
                " ELSE a.data_type END,"
                "CASE WHEN a.nullable='Y' THEN 1 ELSE 0 END,a.data_default,p.position,'','',"
                "CASE WHEN a.virtual_column='YES' THEN 1 ELSE 0 END,b.comments,'',p.constraint_name"
                " FROM all_tab_cols a LEFT JOIN all_col_comments b ON b.owner=a.owner"
                " AND b.table_name=a.table_name AND b.column_name=a.column_name"
                " LEFT JOIN (SELECT k.* FROM all_constraints c JOIN all_cons_columns k ON c.owner=k.owner"
                " AND c.constraint_name=k.constraint_name WHERE c.constraint_type='P' AND c.owner="+scope+
                ") p ON p.owner=a.owner AND p.table_name=a.table_name AND p.column_name=a.column_name"
                " WHERE a.owner="+scope+" AND a.table_name IN ($names) AND a.hidden_column='NO'"
                " ORDER BY a.table_name,a.column_id";
    q.foreignKeys = "SELECT c.table_name,c.constraint_name,k.column_name,r.owner,r.table_name,b.column_name,"
                    "c.delete_rule,'NO ACTION',CASE WHEN c.status='ENABLED' THEN 1 ELSE 0 END,"
                    "CASE WHEN c.deferrable='DEFERRABLE' THEN 1 ELSE 0 END,CASE WHEN c.deferred='DEFERRED' THEN 1 ELSE 0 END"
                    " FROM all_constraints c JOIN all_cons_columns k ON k.owner=c.owner AND k.constraint_name=c.constraint_name"
                    " JOIN all_constraints r ON r.owner=c.r_owner AND r.constraint_name=c.r_constraint_name"
                    " JOIN all_cons_columns b ON b.owner=r.owner AND b.constraint_name=r.constraint_name AND b.position=k.position"
                    " WHERE c.owner="+scope+" AND c.table_name IN ($names) AND c.constraint_type='R'"
                    " ORDER BY c.table_name,c.constraint_name,k.position";
    q.indexes = "SELECT i.table_name,COALESCE(c.constraint_name,i.index_name),CASE WHEN i.uniqueness='UNIQUE' THEN 1 ELSE 0 END,"
                "CASE WHEN c.constraint_type='U' THEN 1 ELSE 0 END,k.column_name,CASE WHEN k.descend='DESC' THEN 1 ELSE 0 END,0,''"
                " FROM all_indexes i JOIN all_ind_columns k ON k.index_owner=i.owner AND k.index_name=i.index_name"
                " LEFT JOIN all_constraints c ON c.index_owner=i.owner AND c.index_name=i.index_name AND c.constraint_type IN ('P','U')"
                " WHERE i.table_owner="+scope+" AND i.table_name IN ($names) AND (c.constraint_type IS NULL OR c.constraint_type<>'P')"
                " ORDER BY i.table_name,i.index_name,k.column_position";
    q.checks = "SELECT table_name,constraint_name,search_condition_vc,CASE WHEN status='ENABLED' THEN 1 ELSE 0 END"
               " FROM all_constraints WHERE owner="+scope+" AND table_name IN ($names) AND constraint_type='C'";
    return q;
}

Queries sqlServerQueries(const QString& scope,bool views) {
    Queries q;
    q.name = "t.name";
    const QString tables = " FROM sys.objects t JOIN sys.schemas s ON s.schema_id=t.schema_id";
    const auto inSchema = " WHERE s.name="+scope+" AND t.name IN ($names)";
    q.objects = "SELECT TOP ($limit) t.name,CASE WHEN t.type='V' THEN 1 ELSE 0 END,"
                "CASE WHEN t.type='V' THEN OBJECT_DEFINITION(t.object_id) ELSE NULL END,CONVERT(nvarchar(max),ep.value)"+ tables +
                " LEFT JOIN sys.extended_properties ep ON ep.class=1 AND ep.major_id=t.object_id"
                " AND ep.minor_id=0 AND ep.name=N'MS_Description'"
                " WHERE s.name="+scope+(views ? " AND t.type IN ('U','V')" : " AND t.type='U'");
    q.columns = "SELECT t.name,a.name,ty.name+CASE WHEN ty.name IN ('varchar','nvarchar','char','nchar','varbinary','binary')"
                " THEN '('+CASE WHEN a.max_length=-1 THEN 'MAX' ELSE CONVERT(varchar(7),a.max_length/"
                " CASE WHEN ty.name IN ('nvarchar','nchar') THEN 2 ELSE 1 END) END+')'"
                " WHEN ty.name IN('decimal','numeric') THEN '('+CONVERT(varchar(7),a.precision)+','+CONVERT(varchar(7),a.scale)+')' ELSE '' END,"
                "a.is_nullable,d.definition,COALESCE(k.key_ordinal,0),'','',CASE WHEN a.is_computed=1 OR a.is_identity=1 THEN 1 ELSE 0 END,"
                "CONVERT(nvarchar(max),ep.value),'',i.name,d.name" + tables+
                " JOIN sys.columns a ON a.object_id=t.object_id JOIN sys.types ty ON ty.user_type_id=a.user_type_id"
                " LEFT JOIN sys.default_constraints d ON d.object_id=a.default_object_id"
                " LEFT JOIN sys.indexes i ON i.object_id=t.object_id AND i.is_primary_key=1"
                " LEFT JOIN sys.index_columns k ON k.object_id=t.object_id AND k.index_id=i.index_id AND k.column_id=a.column_id"
                " LEFT JOIN sys.extended_properties ep ON ep.class=1 AND ep.major_id=t.object_id"
                " AND ep.minor_id=a.column_id AND ep.name=N'MS_Description'" +
                inSchema+" ORDER BY t.name,a.column_id";
    q.foreignKeys = "SELECT t.name,c.name,a.name,r.name,b.name,d.name,REPLACE(c.delete_referential_action_desc,'_',' '),"
                    "REPLACE(c.update_referential_action_desc,'_',' '),1-CONVERT(int,c.is_disabled),0,0"+tables+
                    " JOIN sys.foreign_keys c ON c.parent_object_id=t.object_id"
                    " JOIN sys.foreign_key_columns k ON k.constraint_object_id=c.object_id"
                    " JOIN sys.columns a ON a.object_id=t.object_id AND a.column_id=k.parent_column_id"
                    " JOIN sys.objects b ON b.object_id=k.referenced_object_id JOIN sys.schemas r ON r.schema_id=b.schema_id"
                    " JOIN sys.columns d ON d.object_id=b.object_id AND d.column_id=k.referenced_column_id"+
                    inSchema+" ORDER BY t.name,c.name,k.constraint_column_id";
    q.indexes = "SELECT t.name,i.name,i.is_unique,i.is_unique_constraint,a.name,k.is_descending_key,i.has_filter,i.filter_definition"
                +tables+" JOIN sys.indexes i ON i.object_id=t.object_id AND i.is_primary_key=0 AND i.is_hypothetical=0"
                " JOIN sys.index_columns k ON k.object_id=t.object_id AND k.index_id=i.index_id AND k.key_ordinal>0"
                " JOIN sys.columns a ON a.object_id=t.object_id AND a.column_id=k.column_id"+inSchema+
                " ORDER BY t.name,i.name,k.key_ordinal";
    q.checks = "SELECT t.name,c.name,c.definition,1-CONVERT(int,c.is_disabled)"+tables+
               " JOIN sys.check_constraints c ON c.parent_object_id=t.object_id"+inSchema;
    return q;
}

QString mysqlDefault(const QVariant& value,const QString& type,const QString& extra,K kind) {
    if (value.isNull()) return {};
    auto text = value.toString();
    if (kind == K::MariaDB || extra.contains("DEFAULT_GENERATED",Qt::CaseInsensitive) ||
        text.startsWith("CURRENT_TIMESTAMP",Qt::CaseInsensitive)) return text;
    static const QRegularExpression numeric("^(tinyint|smallint|mediumint|int|bigint|decimal|numeric|float|double|real|bit)\\b",QRegularExpression::CaseInsensitiveOption);
    if (numeric.match(type).hasMatch()) return text;
    return text.isEmpty() ? QString("''") : "0x"+QString::fromLatin1(text.toUtf8().toHex());
}

void sqliteChecks(TableDefinition& table) {
    const auto tokens = design::tokens(table.rawSql,K::SQLite);
    for (int i = 0; i+1 < tokens.size(); ++i) {
        if (tokens[i].kind != sql::TokenKind::Word ||
            tokens[i].text.compare("CHECK",Qt::CaseInsensitive) || tokens[i+1].text != "(") continue;
        const int start = i+1;
        int end = start, depth = 0;
        do {
            const auto& token = tokens[end];
            if (token.text == "(") ++depth;
            if (token.text == ")") --depth;
            ++end;
        } while (depth && end < tokens.size());
        if (depth) continue;
        TableObject check;
        check.kind = TableObjectKind::Check;
        check.name = (i >= 2 && tokens[i-2].text.compare("CONSTRAINT",Qt::CaseInsensitive)==0)
            ? design::unquote(tokens[i-1].text) : "check_"+QString::number(i);
        check.originalName = check.name;
        check.expression = design::slice(table.rawSql,tokens[start].end,tokens[end-1].begin);
        table.objects.push_back(std::move(check));
        i = end-1;
    }
}

void addColumns(Project& project,const Rows& rows,const QHash<QString,int>& ids) {
    const auto kind = project.driver;
    for (const auto& row : rows) {
        const int index = ids.value(row[0].toString(),-1);
        if (index < 0) continue;
        auto& table = project.entities[index].table;
        TableColumn c;
        c.originalName = c.name = row[1].toString();
        c.type = row[2].toString();
        if (c.type.isEmpty() && kind == K::SQLite) c.type = "BLOB";
        c.nullable = truth(row[3]);
        c.defaultExpression = row[4].toString().trimmed();
        c.primaryKey = row[5].toInt();
        if (row[8].toBool()) c.protectedReason = "请先读取完整结构再修改生成字段或标识字段";
        c.comment = row[9].toString();
        c.collation = row.value(10).toString();
        c.defaultConstraint = row.value(12).toString();
        if (c.primaryKey) {
            c.nullable = false;
            table.primaryKeyName = row.value(11).toString();
        }
        if (isMySqlFamily(kind)) {
            const auto extra = row[6].toString();
            c.autoIncrement = extra.contains("auto_increment",Qt::CaseInsensitive);
            c.defaultExpression = mysqlDefault(row[4],c.type,extra,kind);
            c.generatedExpression = row[7].toString();
            if (!c.generatedExpression.isEmpty()) {
                c.generation = extra.contains("STORED") ? "STORED" : "VIRTUAL";
                c.defaultExpression.clear();
            }
            if (extra.contains("on update",Qt::CaseInsensitive))
                c.protectedReason = "请先读取完整结构再修改 ON UPDATE 字段";
        } else if (navidog::usesNativePostgres(kind)) {
            c.autoIncrement = !row[6].toString().isEmpty();
            if (c.autoIncrement) c.generation = row[6].toString() == "a" ? "ALWAYS" : "BY DEFAULT";
            if (!row[7].toString().isEmpty()) {
                c.generatedExpression = c.defaultExpression;
                c.defaultExpression.clear();
                c.generation = row[7].toString() == "v" ? "VIRTUAL" : "STORED";
            }
        }
        table.columns.push_back(std::move(c));
    }
}

void addObjects(Project& project,const Rows& rows,const QHash<QString,int>& ids,TableObjectKind kind) {
    int priorTable = -1;
    QString priorName;
    QStringList pieces;
    const auto finishIndex = [&] {
        if (priorTable < 0 || kind != TableObjectKind::Index) return;
        auto& o = project.entities[priorTable].table.objects.last();
        o.expression = pieces.join(", ");
    };
    for (const auto& row : rows) {
        const int index = ids.value(row[0].toString(),-1);
        if (index < 0) continue;
        auto& table = project.entities[index].table;
        const auto name = row[1].toString();
        if (index != priorTable || name != priorName || kind == TableObjectKind::Check) {
            finishIndex();
            pieces.clear();
            TableObject o;
            o.kind = kind;
            o.name = o.originalName = name;
            if (kind == TableObjectKind::ForeignKey) {
                o.referenceSchema = row[3].toString();
                o.referenceTable = row[4].toString();
                o.onDelete = row[6].toString();
                o.onUpdate = row[7].toString();
                o.enabled = truth(row[8]);
                o.deferrable = truth(row[9]);
                o.initiallyDeferred = truth(row[10]);
            } else if (kind == TableObjectKind::Index) {
                o.unique = truth(row[2]);
                o.constraint = truth(row[3]);
                o.method = row.value(9).toString();
                if (truth(row[6])) {
                    if (project.driver == K::SQLServer) o.predicate = row[7].toString();
                    else {
                        const auto sql = row[7].toString();
                        const int where = sql.indexOf(" WHERE ",0,Qt::CaseInsensitive);
                        if (where >= 0) o.predicate = sql.mid(where+7);
                        else o.protectedReason = "索引条件不可见";
                    }
                }
                if (project.driver == K::SQLite && !o.constraint && !row[7].toString().isEmpty())
                    o.sql = row[7].toString();
            } else {
                o.expression = row[2].toString();
                o.enabled = truth(row[3]);
            }
            table.objects.push_back(std::move(o));
            priorTable = index;
            priorName = name;
        }
        auto& o = table.objects.last();
        if (kind == TableObjectKind::ForeignKey) {
            o.columns << row[2].toString();
            o.referenceColumns << row[5].toString();
        } else if (kind == TableObjectKind::Index) {
            const auto column = row[4].toString();
            if (!column.isEmpty()) o.columns << column;
            QString part = row.value(10).toString();
            if (part.isEmpty() && !column.isEmpty()) {
                part = quoteIdentifier(column,project.driver);
                if (row.value(8).toInt() > 0) part += '('+row[8].toString()+')';
                if (truth(row[5])) part += " DESC";
            }
            if (part.isEmpty()) o.protectedReason = "请先读取表达式索引的完整结构";
            else pieces << part;
        }
    }
    finishIndex();
}

QString resolveSchema(Database& db,const ConnectionProfile& profile,const ImportOptions& options,
                      const Cancellation& cancel) {
    const auto kind = profile.driver;
    if (kind == K::SQLite) return "main";
    if (isMySqlFamily(kind)) return options.schema.isEmpty() ? profile.database : options.schema;
    if (!options.schema.isEmpty()) return options.schema;
    const auto sql = usesOracleSql(kind) ? "SELECT SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM dual"
                  : kind == K::SQLServer ? "SELECT SCHEMA_NAME()"
                  : "SELECT current_schema()";
    const auto rows = read(db,sql,cancel,2);
    return rows.value(0).value(0).toString();
}
}

bool supportsReverse(DatabaseKind kind) {
    return kind != K::Unknown;
}

ImportResult reverse(Database& db,const ConnectionProfile& profile,const ImportOptions& options,
                     const Cancellation& cancel,const std::function<void(int)>& progress) {
    ImportResult result;
    auto& project = result.project;
    project.driver = profile.driver;
    project.version = detectVersion(profile.driver,db.serverVersion());
    if (project.version == DatabaseVersion::Unknown) project.version = profile.version;
    project.profileId = profile.id;
    project.database = profile.database;
    const auto kind = project.driver;
    if (!supportsReverse(kind)) design::fail("当前数据库没有 ER 逆向读取器");
    const auto schema = resolveSchema(db,profile,options,cancel);
    if (schema.isEmpty()) design::fail("请选择具体的数据库或 Schema");
    project.schema = schema;
    project.title = schema + " · ER 图";
    if (isMySqlFamily(kind)) project.database = schema;
    const auto literal = [&](const QString& s) { return design::lit(s,kind); };
    const auto scope = literal(schema);
    const auto version = numericVersion(db.serverVersion(),kind);
    const auto q = isMySqlFamily(kind) ? mysqlQueries(kind,scope,options.includeViews,version)
        : navidog::usesNativePostgres(kind) ? postgresQueries(scope,options.includeViews,version)
        : usesOracleSql(kind) ? oracleQueries(scope,options.includeViews)
        : kind == K::SQLServer ? sqlServerQueries(scope,options.includeViews)
        : sqliteQueries(options.includeViews);
    if (q.checksPartial) result.notices << "此服务器的目录未提供 CHECK 表达式";
    if (usesOracleSql(kind) && options.includeViews) result.notices << "Oracle 大批量导入视图名称和字段，视图查询可在对象定义中补齐";
    result.notices << "批量目录用于浏览和设计，复杂对象修改前可读取完整表定义";
    const QSet<QString> filter(options.exactNames.cbegin(),options.exactNames.cend());
    QString after;
    int totalColumns = 0;
    while (!cancel->load()) {
        auto sql = q.objects;
        if (!filter.isEmpty() && filter.size() <= pageSize) {
            QStringList requested;
            for (const auto& name : filter) requested << literal(name);
            sql += " AND "+q.name+" IN ("+requested.join(',')+")";
        }
        if (!after.isEmpty()) sql += " AND "+q.name+">"+literal(after);
        sql += " ORDER BY "+q.name;
        if (q.fetchFirst) sql += " FETCH FIRST "+QString::number(pageSize)+" ROWS ONLY";
        else if (kind != K::SQLServer) sql += " LIMIT "+QString::number(pageSize);
        sql.replace("$limit",QString::number(pageSize));
        const auto rows = read(db,sql,cancel,pageSize);
        if (rows.isEmpty()) break;
        QHash<QString,int> page;
        QStringList names;
        for (const auto& row : rows) {
            const auto name = row[0].toString();
            if (!filter.isEmpty() && !filter.contains(name)) continue;
            if (!name.contains(options.nameContains,Qt::CaseInsensitive)) continue;
            TableDefinition table;
            table.node.name = table.node.object = name;
            table.node.schema = isMySqlFamily(kind) ? QString{} : schema;
            table.node.database = project.database;
            const auto isView = truth(row[1]);
            table.node.kind = isView ? "view" : "table";
            if (kind == K::SQLite || isView) table.rawSql = row[2].toString();
            table.comment = row[3].toString();
            page.insert(name,project.entities.size());
            names << literal(name);
            project.add(std::move(table));
        }
        if (!names.isEmpty()) {
            const auto load = [&](const auto& self,const QStringList& candidates) -> void {
                const auto list = candidates.join(',');
                const auto query = [&](QString text) {
                    return text.isEmpty() ? Rows{} : read(db,text.replace("$names",list),cancel);
                };
                Rows columns, keys, indexes, checks;
                try {
                    columns = query(q.columns);
                    keys = query(q.foreignKeys);
                    indexes = query(q.indexes);
                    checks = query(q.checks);
                } catch (const BatchLimit&) {
                    if (candidates.size() <= 1) throw;
                    const auto half = candidates.size()/2;
                    self(self,candidates.mid(0,half));
                    self(self,candidates.mid(half));
                    return;
                }
                totalColumns += int(columns.size());
                if (totalColumns > 2000000) design::fail("字段总量超出单模型上限，请分范围导入");
                addColumns(project,columns,page);
                addObjects(project,keys,page,TableObjectKind::ForeignKey);
                addObjects(project,indexes,page,TableObjectKind::Index);
                addObjects(project,checks,page,TableObjectKind::Check);
            };
            load(load,names);
            for (int i : page) {
                auto& entity = project.entities[i];
                auto& t = entity.table;
                if (kind == K::SQLite && t.node.kind == "table") {
                    sqliteChecks(t);
                    t.withoutRowid = t.rawSql.contains("WITHOUT ROWID",Qt::CaseInsensitive);
                    t.strict = QRegularExpression("\\bSTRICT\\s*;?\\s*$",QRegularExpression::CaseInsensitiveOption).match(t.rawSql).hasMatch();
                    if (t.rawSql.contains("AUTOINCREMENT",Qt::CaseInsensitive))
                        for (auto& c : t.columns) if (c.primaryKey == 1 && c.type.compare("INTEGER",Qt::CaseInsensitive)==0) c.autoIncrement = true;
                }
                t.exists = true;
                entity.baseline = t;
            }
        }
        if (progress) progress(int(project.entities.size()));
        const auto next = rows.back()[0].toString();
        if (next == after) design::fail("目录分页未前进，请缩小导入范围");
        after = next;
        if (rows.size() < pageSize) break;
    }
    if (cancel->load()) design::fail("逆向导入已停止");
    if (kind == K::SQLite) {
        const auto graph = makeGraph(project);
        for (const auto& edge : graph.edges) {
            auto& entity = project.entities[edge.from];
            auto& fk = entity.table.objects[edge.constraint];
            if (std::none_of(fk.referenceColumns.cbegin(),fk.referenceColumns.cend(),[](const auto& s){return s.isEmpty();})) continue;
            QVector<QString> names;
            const auto& target = std::as_const(project.entities)[edge.to].table;
            for (const auto& c : target.columns) {
                if (c.primaryKey > 0 && c.primaryKey <= target.columns.size()) {
                    if (names.size() < c.primaryKey) names.resize(c.primaryKey);
                    names[c.primaryKey-1] = c.name;
                }
            }
            if (names.size() == fk.referenceColumns.size())
                for (int i = 0; i < names.size(); ++i) fk.referenceColumns[i] = names[i];
            entity.baseline = entity.table;
        }
    }
    return result;
}
}
