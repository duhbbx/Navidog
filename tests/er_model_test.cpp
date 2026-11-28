#include <QUrl>
#include "navidog/er_model.hpp"
#include "er_canvas.hpp"
#include "report_export.hpp"
#include "navidog/database_tools.hpp"
#include <algorithm>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

namespace m = navidog::er;
using navidog::TableDefinition;
using navidog::TableColumn;
using navidog::TableObject;
using navidog::TableObjectKind;

namespace {
QString read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}
void run(navidog::Database& db,const QString& sql) {
    const auto result = db.executeDefinition(sql,{},navidog::cancellation());
    if (!result.error.isEmpty()) throw std::runtime_error((result.error+"\n"+sql).toStdString());
}
navidog::ConnectionProfile fixture(const QString& path) {
    navidog::ConnectionProfile p;
    p.database = path;
    QFile db(path);
    if (!db.open(QIODevice::WriteOnly)) throw std::runtime_error(db.errorString().toStdString());
    return p;
}
TableDefinition table(const QString& name) {
    TableDefinition t;
    t.node.kind = "table";
    t.node.name = t.node.object = name;
    t.node.schema = "main";
    t.detailsLoaded = true;
    TableColumn id;
    id.name = "id";
    id.type = "INTEGER";
    id.nullable = false;
    id.primaryKey = 1;
    t.columns << id;
    TableColumn owner;
    owner.name = "owner_id";
    owner.type = "INTEGER";
    t.columns << owner;
    TableColumn label;
    label.name = "display_name";
    label.type = "TEXT";
    t.columns << label;
    return t;
}
TableObject fk(const QString& target) {
    TableObject value;
    value.kind = TableObjectKind::ForeignKey;
    value.name = "owner_fk";
    value.referenceTable = target;
    value.referenceSchema = "main";
    value.columns = QStringList{"owner_id"};
    value.referenceColumns = QStringList{"id"};
    return value;
}
}

class ErModelTest : public QObject {
    Q_OBJECT
private slots:
    void reverseForwardAndChangesRoundTrip() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        try {
            auto input = fixture(temp.filePath("source.sqlite"));
            auto db = navidog::connectDatabase(input);
            run(*db,"CREATE TABLE crm_parent (code INTEGER, region INTEGER, name TEXT, PRIMARY KEY(code,region))");
            run(*db,"CREATE TABLE crm_child (id INTEGER PRIMARY KEY, owner INTEGER, region INTEGER, value INTEGER, "
                "CONSTRAINT fk_parent FOREIGN KEY(owner,region) REFERENCES crm_parent(code,region) ON DELETE CASCADE, "
                "CONSTRAINT ck_value CHECK(value >= 0))");
            run(*db,"CREATE UNIQUE INDEX crm_child_value ON crm_child(value) WHERE value IS NOT NULL");
            run(*db,"CREATE VIEW sales_overview AS SELECT id, value FROM crm_child");
            const auto token = navidog::cancellation();
            auto imported = m::reverse(*db,input,{},token).project;
            QCOMPARE(imported.entities.size(),3);
            const auto graph = m::makeGraph(imported);
            QCOMPARE(graph.edges.size(),1);
            const auto& edge = graph.edges.front();
            const auto& child = imported.entities[edge.from].table;
            const auto& rel = child.objects[edge.constraint];
            QCOMPARE(rel.columns,QStringList({"owner","region"}));
            QCOMPARE(rel.referenceColumns,QStringList({"code","region"}));
            QCOMPARE(rel.onDelete,QString("CASCADE"));
            QVERIFY(std::any_of(child.objects.cbegin(),child.objects.cend(),[](const auto& o){return o.kind==TableObjectKind::Check && o.name=="ck_value";}));
            QVERIFY(std::any_of(child.objects.cbegin(),child.objects.cend(),[](const auto& o){return o.kind==TableObjectKind::Index && o.unique && !o.predicate.isEmpty();}));
            QVERIFY(m::ddl(imported,true).isEmpty());

            auto output = fixture(temp.filePath("clone.sqlite"));
            auto clone = navidog::connectDatabase(output);
            for (const auto& sql : m::ddl(imported,false)) run(*clone,sql);
            const auto cloned = m::reverse(*clone,output,{},token).project;
            QCOMPARE(m::makeGraph(cloned).edges.size(),1);

            auto updated = child;
            TableColumn textColumn;
            textColumn.name = "new_text";
            textColumn.type = "TEXT";
            updated.columns << textColumn;
            m::replaceEntity(imported,edge.from,updated);
            const auto changes = m::ddl(imported,true);
            QCOMPARE(changes.size(),1);
            QVERIFY(changes.front().contains("ADD \"new_text\" TEXT"));
            run(*db,changes.front());

            const auto projectPath = temp.filePath("roundtrip.naver.json");
            imported.entities[edge.from].position = QPointF(32,57);
            m::save(imported,projectPath);
            auto reopened = m::open(projectPath);
            QCOMPARE(reopened.entities[edge.from].position.value(),QPointF(32,57));
            QCOMPARE(m::ddl(reopened,true),changes);
            m::ImportOptions selection;
            selection.exactNames << "crm_child";
            const auto chosen = m::reverse(*db,input,selection,token).project;
            QCOMPARE(chosen.entities.size(),1);
            QCOMPARE(chosen.entities.front().table.node.object,QString("crm_child"));
        } catch (const std::exception& e) { QFAIL(e.what()); }
    }
    void twoPhaseForeignKeysAndValidation() {
        m::Project model;
        model.driver = navidog::DatabaseKind::PostgreSQL;
        model.schema = "public";
        auto a = table("data_a");
        auto b = table("data_b");
        for (auto* t : {&a,&b}) {
            t->node.schema = "public";
            t->objects << fk(t == &a ? "data_b" : "data_a");
            t->objects.last().referenceSchema = "public";
        }
        model.add(a);
        model.add(b);
        auto script = m::ddl(model,false);
        QCOMPARE(script.size(),4);
        QVERIFY(script[0].startsWith("CREATE TABLE"));
        QVERIFY(script[1].startsWith("CREATE TABLE"));
        QVERIFY(script[2].contains("ADD CONSTRAINT"));
        QVERIFY(script[3].contains("ADD CONSTRAINT"));
        auto renamed = a;
        renamed.node.object = "data_renamed";
        m::replaceEntity(model,0,renamed);
        QCOMPARE(m::makeGraph(model).edges.size(),2);
        QCOMPARE(model.entities[1].table.objects[0].referenceTable,QString("data_renamed"));
        model.entities[0].table.columns[0].type = "VARCHAR(32)";
        QVERIFY(!m::validate(model,m::makeGraph(model)).isEmpty());
    }
    void expandingColumnsExtendsTheCardAndMakesRoomBelow() {
        m::Project project;
        auto wide = table("sales_a");
        wide.comment = "表级注释\n记录订单和客户信息。";
        wide.columns.front().comment = "字段注释需要保留完整内容。该字段由数据库生成，用于唯一标识订单记录，并用于其他表的引用。";
        for (int i = 0; i < 6; ++i) {
            TableColumn column;
            column.name = QString("extra_field_%1").arg(i);
            column.type = "TEXT";
            wide.columns.push_back(column);
        }
        project.add(std::move(wide));
        for (const auto* name : {"sales_b", "sales_c", "sales_d"}) project.add(table(name));

        const auto graph = m::makeGraph(project);
        const auto original = m::arrange(project,graph);
        const auto card = original.boxes[0];
        QVERIFY(original.boxes[1].top() == card.top());
        QVERIFY(original.boxes[3].top() >= card.bottom());
        m::SpatialIndex index;
        index.reset(original);
        navidog::ui::er::Canvas canvas;
        canvas.resize(1000,900);
        canvas.reset(project,graph,original,std::move(index));
        canvas.show();
        qreal zoom = 0;
        canvas.zoomed = [&](qreal value) { zoom = value; };
        canvas.select(0,true);
        const auto toView = [&](const QPointF& point) {
            return (QPointF(canvas.width()/2.,canvas.height()/2.)+(point-card.center())*zoom).toPoint();
        };
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,toView({card.center().x(),card.bottom()-12}));
        const auto expanded = canvas.arrangement().boxes[0];
        const auto extra = expanded.height()-card.height();
        QCOMPARE(expanded.topLeft(),card.topLeft());
        QCOMPARE(expanded.width(),card.width());
        QVERIFY(extra > 0);
        QCOMPARE(canvas.arrangement().boxes[1],original.boxes[1]);
        const auto directlyBelow = canvas.arrangement().boxes[2];
        QCOMPARE(directlyBelow,original.boxes[2].translated(0,extra));
        QVERIFY(!expanded.intersects(directlyBelow));
        const auto below = canvas.arrangement().boxes[3];
        QCOMPARE(below,original.boxes[3].translated(0,extra));

        int selected = -1;
        canvas.selected = [&](int i) { selected = i; };
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,toView({card.center().x(),card.bottom()+20}));
        QCOMPARE(selected,0);
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::NoModifier,toView(below.topLeft()+QPointF(20,8)));
        QCOMPARE(selected,3);
        canvas.select(0,false);
        QVERIFY(canvas.canShowAnnotations());
        canvas.setAnnotationsVisible(true);
        QVERIFY(canvas.annotationsVisible());
        const auto annotated = canvas.arrangement().boxes[0];
        QVERIFY(annotated.height() > expanded.height()+45);
        QCOMPARE(annotated.topLeft(),card.topLeft());
        const auto capture = qEnvironmentVariable("NAVIDOG_TEST_ER_EXPORT_DIR");
        if (!capture.isEmpty()) QVERIFY(canvas.grab().save(QDir(capture).filePath("er-canvas-comments.png")));
        canvas.setAnnotationsVisible(false);
        QVERIFY(!canvas.annotationsVisible());
        QCOMPARE(canvas.arrangement().boxes[0],expanded);
        canvas.zoom(1.1);
        QCOMPARE(canvas.arrangement().boxes[0],expanded);
        QTest::keyClick(&canvas,Qt::Key_Escape);
        QCOMPARE(canvas.arrangement().boxes,original.boxes);
        QVERIFY(!project.entities[3].position.has_value());
    }
    void rendersFiftyThousandTablesWithIndexedViewport() {
        m::Project project;
        project.entities.reserve(50000);
        for (int i = 0; i < 50000; ++i) {
            const auto name = [](int n) { return QString("schema%1_table%2").arg(n/500).arg(n); };
            auto t = table(name(i));
            if (i) t.objects << fk(name(i-1));
            project.add(std::move(t));
        }
        QElapsedTimer clock;
        clock.start();
        const auto graph = m::makeGraph(project);
        auto arrangement = m::arrange(project,graph);
        const auto layoutMs = clock.elapsed();
        QCOMPARE(graph.active.size(),50000);
        QCOMPARE(graph.edges.size(),49999);
        QCOMPARE(arrangement.groups.size(),100);
        navidog::ui::er::Canvas canvas;
        canvas.resize(1200,800);
        m::SpatialIndex diagramIndex;
        clock.restart();
        diagramIndex.reset(arrangement);
        canvas.reset(project,graph,std::move(arrangement),std::move(diagramIndex));
        const auto indexMs = clock.elapsed();
        canvas.show();
        clock.restart();
        const auto overview = canvas.grab();
        const auto overviewMs = clock.elapsed();
        QVERIFY(!overview.isNull());
        canvas.select(24012,true);
        canvas.grab(); // Prime system font discovery separately from steady-state drawing.
        clock.restart();
        const auto zoomed = canvas.grab();
        const auto detailMs = clock.elapsed();
        QVERIFY(!zoomed.isNull());
        m::SpatialIndex index;
        index.reset(canvas.arrangement());
        const auto visible = canvas.arrangement().boxes[24012].adjusted(-500,-450,500,450);
        const auto found = index.intersecting(visible,canvas.arrangement());
        QVERIFY(found.contains(24012));
        QVERIFY(found.size() < 100);
        qInfo().noquote() << QString("50,000 tables 49,999 relations | graph and gravity layout %1 ms | index %2 ms | overview %3 ms | zoomed paint %4 ms | visible %5")
                    .arg(layoutMs).arg(indexMs).arg(overviewMs).arg(detailMs).arg(found.size());
    }
    void groupedDocumentsIncludeOnePngPerGroup() {
        QTemporaryDir temp;
        const auto dest = qEnvironmentVariable("NAVIDOG_TEST_ER_EXPORT_DIR",temp.path());
        m::Project project;
        project.title = "订单数据库 ER 图";
        project.add(table("crm_customers"));
        auto order = table("sales_orders");
        order.comment = "客户下单记录";
        order.columns.front().comment = "主键由订单服务分配";
        order.columns[2].defaultExpression = "'未命名 & 待确认'";
        order.columns[2].comment = "保留原始名称 <必填>\n核对时使用完整名称";
        for (int i = 0; i < 38; ++i) {
            TableColumn column;
            column.name = QString("optional_field_%1").arg(i,2,10,QChar('0'));
            column.type = "VARCHAR(240)";
            column.comment = i < 37 ? "由订单系统记录，用于历史追踪。" : "最后一个字段说明";
            if (i == 7) {
                column.name = "extended_fulfillment_confirmation_reference";
                column.defaultExpression = "JSON_OBJECT('source', 'customer_portal', 'requires_confirmation', TRUE)";
                column.comment = "存储原始确认引用。当客户修改配送地址、发票信息或联系人时，这个值用于将更正关联到先前的业务记录。";
            }
            order.columns.push_back(std::move(column));
        }
        order.objects << fk("crm_customers");
        project.add(std::move(order));
        auto detail = table("sales_order_items");
        detail.objects << fk("sales_orders");
        project.add(std::move(detail));
        const auto path = navidog::ui::er::exportReport(project,dest,
            navidog::ui::er::Markdown|navidog::ui::er::Pdf|navidog::ui::er::Word,navidog::cancellation(),{},
            {"本地连接", "业务库", "public"});
        const QDir folder(path);
        const auto images = QDir(folder.filePath("images")).entryList({"*.png"},QDir::Files);
        QCOMPARE(images.size(),2);
        for (const auto& name : images)
            QVERIFY(!QImage(folder.filePath("images/"+name)).isNull());
        const auto files = folder.entryList({"*.md"},QDir::Files);
        QCOMPARE(files.size(),1);
        QVERIFY(files.front().startsWith("本地连接-业务库-public-ER文档-"));
        const auto stem = QFileInfo(files.front()).completeBaseName();
        const auto md = read(folder.filePath(files.front()));
        for (const auto& name : images) {
            QVERIFY(name.startsWith("本地连接-业务库-public-ER图-分组"));
            QVERIFY(md.contains("images/"+QUrl::toPercentEncoding(name)));
        }
        QVERIFY(md.contains("sales\\_orders"));
        QFile pdf(folder.filePath(stem + ".pdf"));
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        QCOMPARE(pdf.read(4),QByteArray("%PDF"));
#if QT_VERSION >= QT_VERSION_CHECK(6,8,0)
        QVERIFY(pdf.readAll().count("/Subtype /Link") >= 10);
#endif
        QFile docx(folder.filePath(stem + ".docx"));
        QVERIFY(docx.open(QIODevice::ReadOnly));
        QCOMPARE(docx.read(2),QByteArray("PK"));
        const auto parts = docx.readAll(); // The exporter stores its XML parts as uncompressed ZIP entries.
        QCOMPARE(parts.count("<w:tbl>"),3);
        QCOMPARE(parts.count("<w:tblHeader/>"),3);
        QVERIFY(parts.contains("<w:tblLayout w:type=\"fixed\"/>"));
        QVERIFY(parts.contains("&lt;必填&gt;</w:t><w:br/>"));
        QVERIFY(parts.contains("最后一个字段说明"));
        QVERIFY(parts.contains("w:instr=\"PAGE\""));
        QCOMPARE(parts.count("<w:bookmarkStart"),5);
        QCOMPARE(parts.count("<w:hyperlink w:anchor=\"NavidogEr"),5);
        QCOMPARE(parts.count("w:instr=\"PAGEREF NavidogEr"),5);
        QVERIFY(parts.contains("<w:updateFields w:val=\"true\"/>"));
        if (dest != temp.path()) qInfo().noquote() << path;
    }
    void databaseDictionaryExportsCommentsInPdfAndWord() {
        using namespace navidog;
        using namespace navidog::ui;
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir folder(qEnvironmentVariable("NAVIDOG_TEST_DICTIONARY_EXPORT_DIR", temp.path()));
        DatabaseDictionary dictionary{"sales / public", {}};
        DatabaseDictionaryTable orders;
        orders.name = "public.orders";
        orders.comment = "订单主表 <内部>\n记录订单创建与处理状态";
        TableColumn id;
        id.name = "order_id";
        id.type = "bigint";
        id.primaryKey = 1;
        id.nullable = false;
        id.comment = "订单唯一标识 & 关联发货记录";
        orders.columns.push_back(id);
        for (int i = 1; i <= 42; ++i) {
            TableColumn note;
            note.name = QString("shipping_note_%1").arg(i, 2, 10, QChar('0'));
            note.type = "varchar(250)";
            note.comment = QString("配送说明 %1\n保留原始配送指示和联系方式").arg(i);
            orders.columns.push_back(note);
        }
        dictionary.tables.push_back(std::move(orders));
        auto customers = table("public.customers");
        dictionary.tables.push_back({"public.customers", "客户档案与收货信息", std::move(customers.columns)});

        const auto token = cancellation();
        const auto wordPath = folder.filePath("database-dictionary.docx");
        const auto pdfPath = folder.filePath("database-dictionary.pdf");
        exportDatabaseDictionary(dictionary, wordPath, ReportDocumentFormat::Word, token);
        exportDatabaseDictionary(dictionary, pdfPath, ReportDocumentFormat::Pdf, token);
        QFile word(wordPath);
        QVERIFY(word.open(QIODevice::ReadOnly));
        const auto docx = word.readAll();
        QVERIFY(docx.startsWith("PK"));
        QVERIFY(docx.contains("订单主表 &lt;内部&gt;</w:t><w:br/>"));
        QVERIFY(docx.contains("订单唯一标识 &amp; 关联发货记录"));
        QVERIFY(docx.contains("配送说明 42"));
        QCOMPARE(docx.count("<w:tblHeader/>"), 2);
        QCOMPARE(docx.count("w:instr=\"PAGEREF NavidogEr"), 2);
        QFile pdf(pdfPath);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const auto bytes = pdf.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
#if QT_VERSION >= QT_VERSION_CHECK(6,8,0)
        QVERIFY(bytes.count("/Subtype /Link") >= 4);
#endif
        QVERIFY(bytes.count("/Type /Page\n") >= 3);
    }
    void contentsSpansPagesWithSeparateGroupAndObjectLinks() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto dest = qEnvironmentVariable("NAVIDOG_TEST_ER_EXPORT_DIR",temp.path());
        m::Project project;
        project.title = "多页目录验证";
        constexpr int tableCount = 70;
        for (int i = 0; i < tableCount; ++i) {
            auto entry = table(QString("team_table_%1").arg(i,2,10,QChar('0')));
            if (i == 3) entry.node.name = entry.node.object = "team_客户历史记录";
            if (i == 9) entry.node.name = entry.node.object = QString("team_%1").arg(QString(130,QChar(0x8868)));
            project.add(std::move(entry));
        }
        auto view = table("review_summary");
        view.node.kind = "view";
        project.add(std::move(view));
        const QDir folder(navidog::ui::er::exportReport(project,dest,
            navidog::ui::er::Pdf|navidog::ui::er::Word,navidog::cancellation()));
        QFile pdf(folder.filePath(folder.entryList({"*.pdf"},QDir::Files).value(0)));
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const auto content = pdf.readAll();
        QVERIFY(content.startsWith("%PDF"));
#if QT_VERSION >= QT_VERSION_CHECK(6,8,0)
        QVERIFY(content.count("/Subtype /Link") >= (tableCount+3)*2);
#endif
        QFile docx(folder.filePath(folder.entryList({"*.docx"},QDir::Files).value(0)));
        QVERIFY(docx.open(QIODevice::ReadOnly));
        const auto parts = docx.readAll();
        QCOMPARE(parts.count("w:instr=\"PAGEREF NavidogEr"),tableCount+3);
        QVERIFY(parts.contains("review_summary"));
        if (dest != temp.path()) qInfo().noquote() << folder.absolutePath();
    }

    void dictionaryLayoutAndConfigurationRoundTrip() {
        using namespace navidog;
        using namespace navidog::ui;
        DictionaryConfiguration original;
        original.layout.theme = DictionaryTheme::Jade;
        original.layout.title = "仓储数据字典";
        original.layout.coverHeader = "仓储应用";
        original.layout.notes = "内部说明\n供数据维护使用";
        original.layout.headerText = "仓储文档";
        original.layout.footerText = "数据维护";
        original.layout.introductionText = "此文档记录仓储数据库结构。";
        original.layout.paper = QPageSize::Letter;
        original.layout.orientation = QPageLayout::Landscape;
        original.layout.margins = {16,13,19,17};
        original.layout.background = DictionaryBackground::Image;
        original.layout.imageFit = DictionaryImageFit::Tile;
        original.layout.opacity = 62;
        original.layout.logo = QImage(56,42,QImage::Format_RGB32);
        original.layout.logo.fill(QColor("#186856"));
        original.layout.coverImage = QImage(75,60,QImage::Format_RGB32);
        original.layout.coverImage.fill(QColor("#DBEEE8"));
        original.scopes.push_back({"test-profile","warehouse","public",true,{"stock","events"},{"events"}});
        original.serverInfo["test-profile"] = false;
        const auto json = dictionaryConfigurationJson(original);
        const auto copied = dictionaryConfigurationFromJson(json);
        QCOMPARE(dictionaryConfigurationJson(copied),json);
        QCOMPARE(copied.scopes.front().omitted,QStringList{"events"});

        auto fields = table("public.stock");
        fields.columns[0].comment = "货品编号";
        fields.objects << fk("warehouse");
        DatabaseDictionary data;
        data.scope = "warehouse / public";
        data.sections.push_back({0,"warehouse / public","仓储 Schema"});
        data.tables.push_back({"public.stock","库存主表",fields.columns,"table","stock",fields.objects});
        const auto token = cancellation();
        const auto html = dictionaryHtml(data,copied.layout,DictionaryPreviewSection::Full,token);
        QVERIFY(html.contains("仓储数据字典"));
        QVERIFY(html.contains("库存主表"));
        QVERIFY(html.contains("货品编号"));
        QVERIFY(html.contains("定义或引用"));
        QVERIFY(html.contains("data:image/png;base64,"));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir folder(qEnvironmentVariable("NAVIDOG_TEST_DICTIONARY_EXPORT_DIR",temporary.path()));
        const auto pdf = folder.filePath("configured-dictionary.pdf");
        const auto word = folder.filePath("configured-dictionary.docx");
        exportDatabaseDictionary(data,copied.layout,pdf,ReportDocumentFormat::Pdf,token);
        exportDatabaseDictionary(data,copied.layout,word,ReportDocumentFormat::Word,token);
        QFile docx(word);
        QVERIFY(docx.open(QIODevice::ReadOnly));
        const auto bytes = docx.readAll();
        QVERIFY(bytes.contains("w:orient=\"landscape\""));
        QVERIFY(bytes.contains("word/media/cover-logo.png"));
        QVERIFY(bytes.contains("word/media/cover-art.png"));
        QVERIFY(bytes.contains("仓储文档"));
        QVERIFY(bytes.contains("数据维护"));
        QVERIFY(bytes.contains("货品编号"));
        QFile pdfFile(pdf);
        QVERIFY(pdfFile.open(QIODevice::ReadOnly));
        const auto pdfBytes = pdfFile.readAll();
        QVERIFY(pdfBytes.startsWith("%PDF"));
        QVERIFY(pdfBytes.contains("/MediaBox [0 0 792.000000 612.000000]"));
    }
};
QTEST_MAIN(ErModelTest)
#include "er_model_test.moc"
