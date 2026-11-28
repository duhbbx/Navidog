#include "task_center.hpp"
#include "navidog/query_parameters.hpp"
#include <algorithm>
#include <QtTest>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QToolButton>
#include <QTableView>
#include <QTabWidget>
#include <QTabBar>
#include <QAbstractButton>
#include <QTreeWidget>
#include <QScrollBar>
#include <QSplitter>
#include <QDropEvent>
#include <QMimeData>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QProcess>
#include <QStandardPaths>
#include <QPushButton>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QComboBox>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QMessageBox>
#include <QLabel>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QClipboard>
#include "main_window.hpp"
#include "export_paths.hpp"
#include "metadata_controller.hpp"
#include "application_help.hpp"
#include "startup_timing.hpp"
#include "operation_monitor.hpp"
#include "navidog/operation_timing.hpp"
#include "navigation_pane.hpp"
#include "quick_entry_bar.hpp"
#include "sql_formatting.hpp"
#include "sql_conversion.hpp"
#include "sql_optimization.hpp"
#include "application_settings.hpp"
#include "navidog/local_store.hpp"
#include "navidog/secret_store.hpp"
#include "dictionary_wizard.hpp"
#include <QListWidget>
#include <QStackedWidget>
#include "editor.hpp"
#include "table_pane.hpp"
#include "data_pane.hpp"
#include "source_pane.hpp"
#include "objects_pane.hpp"
#include "query_pane.hpp"
#include "er_pane.hpp"
#include "result_grid.hpp"
#include "connection_dialog.hpp"
#include "connection_controller.hpp"
#include "namespace_dialog.hpp"
#include "table_task_dialog.hpp"
#include "table_report_dialog.hpp"
#include <QGraphicsView>
#include <QTreeView>
#include <QStandardItemModel>
#include <QAccessible>
#include <QGraphicsScene>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QHelpEvent>
#include <QToolTip>
#include <QScreen>
#include <QFontMetrics>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextBrowser>

class UiTest : public QObject {
    Q_OBJECT
    static void checkDialogAfterMouseRelease(QWidget* target,const QPoint& point,const QString& identity) {
        int opened = 0;
        QTimer closeDialog;
        connect(&closeDialog,&QTimer::timeout,target,[&] {
            for (auto* dialog : target->window()->findChildren<QDialog*>()) {
                if (dialog->isVisible() && (dialog->objectName() == identity || dialog->windowTitle() == identity)) {
                    ++opened;
                    dialog->reject();
                    break;
                }
            }
        });
        closeDialog.start(5);
        QTest::mouseClick(target,Qt::LeftButton,Qt::NoModifier,point);
        QTest::mouseDClick(target,Qt::LeftButton,Qt::NoModifier,point);
        QTest::qWait(25);
        const auto prematurelyOpened = opened;
        QTest::mouseRelease(target,Qt::LeftButton,Qt::NoModifier,point);
        QCOMPARE(prematurelyOpened,0);
        QTRY_COMPARE(opened,1);
        QVERIFY(!QApplication::activeModalWidget());
    }

private slots:
    void queryToolbarOverflowKeepsHiddenCommandsReachable() {
        navidog::ui::OverflowStrip strip;QList<QToolButton*> buttons;int clicks=0;
        for(int i=0;i<8;++i){auto* b=new navidog::ui::Button(QString("操作 %1").arg(i));strip.row()->addWidget(b);buttons.push_back(b);connect(b,&QToolButton::clicked,this,[&]{++clicks;});}
        strip.complete(buttons);strip.resize(200,45);strip.show();auto* more=strip.findChild<QToolButton*>("queryToolbarMore");QTRY_VERIFY(more->isVisible());QVERIFY(buttons.back()->isHidden());
        more->menu()->popup(strip.mapToGlobal(QPoint(0,40)));QTRY_VERIFY(!more->menu()->actions().isEmpty());auto* action=more->menu()->actions().back();QCOMPARE(action->text(),QString("操作 7"));action->trigger();more->menu()->hide();QCOMPARE(clicks,1);
        strip.resize(2400,45);QTRY_VERIFY(!more->isVisible());for(auto* b:buttons)QVERIFY(!b->isHidden());
    }
    void scriptDdlDeclarationsParticipateInEditorChecks() {
      using namespace navidog;auto token=std::make_shared<std::atomic_bool>(false);
      auto checked=checkEditorSql("CREATE TABLE script_owned(id INTEGER);ALTER TABLE script_owned ADD COLUMN title TEXT;SELECT missing FROM script_owned;",DatabaseKind::PostgreSQL,{},{},token);
      QVERIFY(std::any_of(checked.diagnostics.begin(),checked.diagnostics.end(),[](const auto &d){return d.error && d.message.contains("missing");}));
      checked=checkEditorSql("CREATE TABLE script_owned(id INTEGER);SELECT id FROM script_owned;",DatabaseKind::PostgreSQL,{},{},token);QVERIFY(checked.diagnostics.isEmpty());
      checked=checkEditorSql("CREATE TABLE script_owned(id INTEGER);DROP TABLE script_owned;SELECT id FROM script_owned;",DatabaseKind::PostgreSQL,{},{},token);QVERIFY(!checked.diagnostics.isEmpty());QVERIFY(checked.diagnostics.back().error);
    }
    void routineDiagnosticsCheckScopesAndCurrentCatalogWithoutExecuting() {
        using namespace navidog;auto token=std::make_shared<std::atomic_bool>(false);
        auto broken=checkEditorSql("CREATE FUNCTION f(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN missing_variable;",DatabaseKind::MySQL,"owned","owned",token);
        QVERIFY(std::any_of(broken.diagnostics.begin(),broken.diagnostics.end(),[](const auto &d){return d.error&&d.message.contains("变量");}));
        auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::MySQL;snapshot->database=snapshot->schema="owned";snapshot->searchPath={"owned"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;
        auto checked=checkEditorSql("DELIMITER $$\nCREATE PROCEDURE p(IN v INTEGER) SQL SECURITY INVOKER BEGIN INSERT INTO missing_table(n) VALUES(v); END;$$\nDELIMITER ;",DatabaseKind::MySQL,"owned","owned",token,snapshot);
        QVERIFY(std::any_of(checked.diagnostics.begin(),checked.diagnostics.end(),[](const auto &d){return d.metadata&&d.message.contains("未找到该表");}));
        auto good=checkEditorSql("CREATE FUNCTION f(p INTEGER) RETURNS INTEGER DETERMINISTIC RETURN p+1;",DatabaseKind::MySQL,"owned","owned",token);
        QVERIFY2(good.diagnostics.isEmpty(),qPrintable(good.status));

        const QString nested =
            "DELIMITER $$\nCREATE FUNCTION f_get_quantity(_id INT) RETURNS INT READS SQL DATA\n"
            "BEGIN IF _id > 0 THEN RETURN (SELECT SUM(id) FROM t_activity WHERE id = _id);\n"
            "ELSE RETURN (SELECT SUM(id) FROM t_activity); END IF;\n"
            "IF _id = 0 THEN RETURN 1; END IF; END$$";
        good = checkEditorSql(nested, DatabaseKind::MySQL, "owned", "owned", token);
        QVERIFY2(good.diagnostics.isEmpty(), qPrintable(good.status));
        MetadataObject activity;
        activity.key = activity.name = "t_activity";
        activity.kind = "table";
        activity.database = activity.schema = "owned";
        activity.columns = {{"other_column", "int"}};
        snapshot->objects.append(activity);
        const auto missing = checkEditorSql(nested, DatabaseKind::MySQL, "owned", "owned",
                                            token, snapshot);
        QVERIFY(!missing.diagnostics.isEmpty());
        for (const auto &mark : missing.diagnostics) {
            QVERIFY(mark.metadata);
            QVERIFY(mark.message.contains("字段未找到"));
            QCOMPARE(nested.mid(mark.begin, mark.end - mark.begin), QString("id"));
        }
        const QString cursor =
            "DELIMITER //\nCREATE PROCEDURE p() BEGIN DECLARE v INT; "
            "DECLARE row_cursor CURSOR FOR SELECT 1; FETCH row_cursor INTO v; END//";
        const auto cursorCheck = checkEditorSql(cursor, DatabaseKind::MySQL, "owned", "owned", token);
        QCOMPARE(cursorCheck.diagnostics.size(), 1);
        QVERIFY(!cursorCheck.diagnostics.front().error);
        QVERIFY(cursorCheck.status.contains("游标运行提示"));
        const auto mark = cursorCheck.diagnostics.front();
        QCOMPARE(cursor.mid(mark.begin, mark.end - mark.begin), QString("row_cursor"));
        const QString prepared="CREATE PROCEDURE p() BEGIN PREPARE sample FROM 'SELECT ?,?'; "
            "EXECUTE sample USING @only_one; END;";
        const auto preparedCheck=checkEditorSql(prepared,DatabaseKind::MySQL,"owned","owned",token);
        QCOMPARE(preparedCheck.diagnostics.size(),1);
        const auto preparedMark=preparedCheck.diagnostics.front();
        QVERIFY(!preparedMark.error);
        QVERIFY(preparedCheck.status.contains("预处理语句运行提示"));
        QVERIFY(preparedMark.message.contains("参数个数"));
        QVERIFY(prepared.mid(preparedMark.begin,preparedMark.end-preparedMark.begin).startsWith("EXECUTE sample"));
    }
    void localOptimizationShowsPlansAndPersistsOnlyManualStatistics() {
        QTemporaryDir directory;QVERIFY(directory.isValid());
        const auto path=navidog::localStorePath(directory.path());
        bool complete=false,saved=false;QString failure;int stage=0;QTimer poll;poll.setInterval(30);
        connect(&poll,&QTimer::timeout,this,[&]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;
            auto* start=dialog->findChild<QToolButton*>("optimizeSqlNow");auto* status=dialog->findChild<QLabel*>("optimizationStatus");
            if(stage==0){if(!start||!status){failure="missing optimization controls";dialog->reject();return;}stage=1;QTest::mouseClick(start,Qt::LeftButton);return;}
            if(!start->isEnabled())return;
            if(!status->text().startsWith("基线")){failure=status->text();dialog->reject();return;}
            auto* tree=dialog->findChild<QTreeWidget*>("optimizationBestPlan");auto* grid=dialog->findChild<QTableView*>("optimizationStatistics");
            if(!tree || !tree->topLevelItemCount() || !grid || grid->model()->rowCount()!=1){failure="plan/statistics absent";dialog->reject();return;}
            if(stage==1){complete=true;grid->model()->setData(grid->model()->index(0,1),"12345");stage=2;QTest::mouseClick(dialog->findChild<QToolButton*>("saveOptimizationStatistics"),Qt::LeftButton);return;}
            saved=true;dialog->accept();
        });poll.start();
        QTimer deadline;deadline.setSingleShot(true);connect(&deadline,&QTimer::timeout,this,[&]{failure="local optimization timed out";if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()))d->reject();});deadline.start(10000);
        navidog::sql::compiler::NameResolutionContext context;context.dialect=navidog::sql::compiler::Dialect::PostgreSQL;
        showSqlOptimization(nullptr,context.dialect,"SELECT a.id FROM a WHERE a.id=1;",context,{},path,"owned.optimizer.statistics");poll.stop();deadline.stop();
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(complete);QVERIFY(saved);
        navidog::LocalStore store(path);auto bytes=store.document("owned.optimizer.statistics");QVERIFY(bytes);auto tables=QJsonDocument::fromJson(*bytes).object()["tables"].toArray();QCOMPARE(tables.size(),1);QCOMPARE(tables[0].toObject()["rows"].toDouble(),12345.);
    }
    void localOptimizationCanEnableCalciteUnpivot() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTimer poll;
        poll.setInterval(25);
        int stage=0;
        QString failure;
        connect(&poll,&QTimer::timeout,this,[&] {
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *start=dialog->findChild<QToolButton*>("optimizeSqlNow");
            auto *status=dialog->findChild<QLabel*>("optimizationStatus");
            auto *calcite=dialog->findChild<QCheckBox*>("optimizationCalciteSyntax");
            auto *best=dialog->findChild<QTreeWidget*>("optimizationBestPlan");
            auto *preview=dialog->findChild<QPlainTextEdit*>("optimizationPlanSql");
            if (!start || !status || !calcite || !best || !preview) {
                failure="Calcite analysis controls missing";
                dialog->reject();
                return;
            }
            if (!start->isEnabled()) return;
            if (stage==0) {
                if (calcite->isChecked()) {
                    failure="Calcite syntax enabled by default";
                    dialog->reject();
                    return;
                }
                stage=1;
                start->click();
            } else if (stage==1) {
                if (!status->text().startsWith("无法完成本地优化")) {
                    failure=status->text();
                    dialog->reject();
                    return;
                }
                calcite->setChecked(true);
                stage=2;
                start->click();
            } else {
                QTreeWidgetItemIterator it(best);
                for (; *it; ++it)
                    if ((*it)->text(0).contains("unpivot",Qt::CaseInsensitive)) break;
                if (*it && preview->isReadOnly() &&
                    preview->toPlainText().contains("CROSS JOIN LATERAL",Qt::CaseInsensitive))
                    stage=3;
                else failure="UNPIVOT plan SQL missing: "+preview->placeholderText();
                dialog->accept();
            }
        });
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline,&QTimer::timeout,this,[&] {
            failure="Calcite analysis timed out";
            if (auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
        });
        poll.start();
        deadline.start(10000);
        navidog::sql::compiler::NameResolutionContext context;
        context.dialect=navidog::sql::compiler::Dialect::PostgreSQL;
        showSqlOptimization(nullptr,context.dialect,
            "SELECT u.* FROM (VALUES (1,2,3,4)) s(q1,v1,q2,v2) "
            "UNPIVOT ((total,units) FOR (label,period) IN ((q1,v1),(q2,v2))) u",
            context,{},navidog::localStorePath(directory.path()),"owned.optimizer.calcite");
        QVERIFY2(failure.isEmpty(),qPrintable(failure));
        QCOMPARE(stage,3);
    }
    void calciteEditorSyntaxAndCompletionFollowAnalysisContext() {
        SqlEditor editor;
        auto *completion=editor.findChild<SqlCompletionController*>();
        QVERIFY(completion);
        QSignalSpy ready(completion,&SqlCompletionController::ready);
        const auto labels=[&] {
            QStringList result;
            const auto snapshot=qvariant_cast<EditorCompletionResult>(ready.takeLast().at(0));
            for (const auto &item:snapshot.items) result.push_back(item.label);
            return result;
        };
        editor.setDiagnosticContext(navidog::DatabaseKind::PostgreSQL);
        editor.setPlainText("SELECT * FROM (VALUES (1,2,3)) s(id,q1,q2) UNPIVOT INC");
        editor.moveCursor(QTextCursor::End);
        completion->request(SqlCompletionController::Activation::Manual);
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(!labels().contains("INCLUDE NULLS"));
        editor.setDiagnosticContext(navidog::DatabaseKind::PostgreSQL,{},{},true);
        completion->request(SqlCompletionController::Activation::Manual);
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(labels().contains("INCLUDE NULLS"));
        editor.setPlainText("SELECT * FROM (VALUES (1,2,3)) s(id,q1,q2) "
                            "UNPIVOT INCLUDE NULLS (total FOR label IN (q1,q2)) u");
        QTRY_VERIFY_WITH_TIMEOUT(editor.diagnosticStatus().contains("未发现问题"),3000);
        QVERIFY(editor.diagnostics().isEmpty());
        editor.setDiagnosticContext(navidog::DatabaseKind::PostgreSQL);
        QTRY_VERIFY_WITH_TIMEOUT(!editor.diagnostics().isEmpty(),3000);
        editor.setDiagnosticContext(navidog::DatabaseKind::OceanBaseOracle);
        editor.setPlainText("SELECT rownu");
        editor.moveCursor(QTextCursor::End);
        completion->request(SqlCompletionController::Activation::Manual);
        QTRY_VERIFY(!ready.isEmpty());
        QVERIFY(labels().contains("ROWNUM"));
    }
    void extendedEditorChecksAndCompletesNativeSyntax() {
        using navidog::DatabaseKind;
        SqlEditor editor;
        auto* completion = editor.findChild<SqlCompletionController*>();
        QVERIFY(completion);
        QSignalSpy ready(completion, &SqlCompletionController::ready);
        auto request = [&](const QString& text) {
            ready.clear();
            editor.setPlainText(text);
            editor.moveCursor(QTextCursor::End);
            completion->request(SqlCompletionController::Activation::Manual);
        };
        editor.setDiagnosticContext(DatabaseKind::MongoDb);
        request("{\"fi");
        QTRY_VERIFY(!ready.isEmpty());
        const auto commands = qvariant_cast<EditorCompletionResult>(ready.takeLast().at(0)).items;
        QVERIFY(!commands.isEmpty());
        QCOMPARE(commands.first().insertText, QString("find\": "));
        request("{\"find\": \"orders\", \"fi");
        QTRY_VERIFY(!ready.isEmpty());
        const auto options = qvariant_cast<EditorCompletionResult>(ready.takeLast().at(0)).items;
        QVERIFY(!options.isEmpty());
        QCOMPARE(options.first().label, "filter");
        editor.setPlainText("{\"find\":\"orders\",\"filter\":[]}");
        QTRY_VERIFY(!editor.diagnostics().isEmpty());
        QVERIFY(editor.diagnostics().first().message.contains("filter"));
        editor.setDiagnosticContext(DatabaseKind::ClickHouse);
        request("SELECT id FROM events PREW");
        QTRY_VERIFY(!ready.isEmpty());
        const auto sqlItems = qvariant_cast<EditorCompletionResult>(ready.takeLast().at(0)).items;
        QVERIFY(std::any_of(sqlItems.begin(), sqlItems.end(),
            [](const auto& item) { return item.label == "PREWHERE"; }));
    }
    void completionReplacementPreservesUnicodeDocumentOffsets() {
        using namespace navidog;
        auto metadata=std::make_shared<MetadataSnapshot>();
        metadata->driver=DatabaseKind::PostgreSQL;
        metadata->database="owned";
        metadata->schema="public";
        metadata->searchPath={"public"};
        metadata->refreshedMs=QDateTime::currentMSecsSinceEpoch();
        metadata->verified=metadata->tablesComplete=metadata->columnsComplete=true;
        MetadataObject table;
        table.kind="table";
        table.database="owned";
        table.schema="public";
        table.name="users";
        table.columns={{"display_name","text"},{"display_order","integer"}};
        metadata->objects.push_back(table);
        SqlEditor editor;
        editor.setDiagnosticContext(DatabaseKind::PostgreSQL,"owned","public");
        editor.setMetadataSnapshot(metadata);
        const QString text="-- 中文 😀\nSELECT u.disp FROM users u";
        editor.setPlainText(text);
        auto cursor=editor.textCursor();
        const auto start=text.indexOf("disp");
        cursor.setPosition(int(start+4));
        editor.setTextCursor(cursor);
        auto *controller=editor.findChild<SqlCompletionController*>();
        QVERIFY(controller);
        QSignalSpy ready(controller,&SqlCompletionController::ready);
        controller->request(SqlCompletionController::Activation::Manual);
        QTRY_VERIFY(!ready.isEmpty());
        const auto output=qvariant_cast<EditorCompletionResult>(ready.takeFirst().at(0));
        int matches=0;
        for (const auto &item:output.items)
            if (item.label=="display_name" || item.label=="display_order") {
                QCOMPARE(item.begin,start);
                QCOMPARE(item.end,start+4);
                QCOMPARE(text.mid(item.begin,item.end-item.begin),QString("disp"));
                ++matches;
            }
        QCOMPARE(matches,2);
    }
    void automaticCompletionAcceptsEnterAndResumesAfterEscape() {
        SqlEditor editor;editor.setDiagnosticContext(navidog::DatabaseKind::PostgreSQL);editor.resize(800,500);editor.show();editor.activateWindow();editor.setFocus();
        auto* popup=editor.findChild<QTreeView*>("sqlCompletionPopup");QVERIFY(popup);QSignalSpy completionReady(editor.findChild<SqlCompletionController*>(),&SqlCompletionController::ready);
        QTest::keyClicks(&editor,"S");QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(),QString("SELECT"));
        const auto caretBottom=editor.viewport()->mapToGlobal(editor.cursorRect().bottomLeft()).y();
        QVERIFY(popup->mapToGlobal(QPoint()).y()>=caretBottom+24);
        QTest::keyClicks(&editor,"EL");QTRY_VERIFY(completionReady.count()>=2);QTRY_VERIFY(popup->isVisible());QCOMPARE(editor.toPlainText(),QString("SEL"));
        auto selected=popup->currentIndex();QVERIFY(selected.isValid());
        auto expected=selected.data(Qt::UserRole).toString();
        QTest::keyClick(&editor,Qt::Key_Return);QCOMPARE(editor.toPlainText(),expected);QVERIFY(!popup->isVisible());
        editor.setPlainText({});editor.setFocus();QTest::keyClicks(&editor,"S");QTRY_VERIFY2(popup->isVisible(),qPrintable(QString("focus=%1 active=%2 text=%3 status=%4 rows=%5").arg(editor.hasFocus()).arg(editor.isActiveWindow()).arg(editor.toPlainText()).arg(editor.property("sqlCompletionStatus").toString()).arg(popup->model()->rowCount())));
        QTest::keyClick(&editor,Qt::Key_Escape);QVERIFY(!popup->isVisible());QCOMPARE(editor.toPlainText(),QString("S"));
        QTest::qWait(100);QVERIFY(!popup->isVisible());
        QTest::keyClicks(&editor,"E");QTRY_VERIFY(popup->isVisible());QCOMPARE(editor.toPlainText(),QString("SE"));
        expected=popup->currentIndex().data(Qt::UserRole).toString();
        QTest::keyClick(&editor,Qt::Key_Tab);QCOMPARE(editor.toPlainText(),expected);
        editor.setPlainText({});editor.setFocus();QTest::keyClicks(&editor,"S");QTRY_VERIFY2(popup->isVisible(),qPrintable(QString("focus=%1 active=%2 text=%3 status=%4 rows=%5").arg(editor.hasFocus()).arg(editor.isActiveWindow()).arg(editor.toPlainText()).arg(editor.property("sqlCompletionStatus").toString()).arg(popup->model()->rowCount())));
        QTest::keyClick(&editor,Qt::Key_Return,Qt::ShiftModifier);QCOMPARE(editor.toPlainText(),QString("S\n"));
        editor.setPlainText({});editor.setFocus();QTest::keyClicks(&editor,"S");QTest::keyClick(&editor,Qt::Key_Escape);QTest::qWait(150);QVERIFY(!popup->isVisible());QCOMPARE(editor.toPlainText(),QString("S"));QTest::keyClicks(&editor,"E");QTRY_VERIFY(popup->isVisible());QTest::keyClick(&editor,Qt::Key_Escape);
        editor.setPlainText("SELECT '");auto stringCursor=editor.textCursor();stringCursor.movePosition(QTextCursor::End);editor.setTextCursor(stringCursor);QTest::keyClicks(&editor,"x");QTest::qWait(150);QVERIFY(!popup->isVisible());
        editor.applyPreferences(12,4,false,false,1);editor.setPlainText({});editor.setFocus();QTest::keyClicks(&editor,"SEL");QTest::qWait(250);QVERIFY(!popup->isVisible());
    }
    void projectionCompletionAfterSpaceAndSharedPreviewHighlighting() {
        SqlEditor editor;
        editor.setDiagnosticContext(navidog::DatabaseKind::MySQL);
        editor.resize(800,500);
        editor.show();
        editor.activateWindow();
        editor.setFocus();
        auto* popup=editor.findChild<QTreeView*>("sqlCompletionPopup");
        QVERIFY(popup);
        QTest::keyClicks(&editor,"SELECT ");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(),QString("* FROM"));
        QTest::keyClick(&editor,Qt::Key_Tab);
        QCOMPARE(editor.toPlainText(),QString("SELECT * FROM table_name"));
        QCOMPARE(editor.textCursor().selectedText(),QString("table_name"));
        QTest::keyClicks(&editor,"items ");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(),QString("WHERE"));
        QTest::keyClick(&editor,Qt::Key_Escape);
        QVERIFY(!popup->isVisible());
        QCOMPARE(editor.toPlainText(),QString("SELECT * FROM items "));

        SqlReadOnlyView preview;
        preview.setDatabaseKind(navidog::DatabaseKind::SQLServer);
        preview.setPlainText(QString::fromUtf8("CREATE TABLE [表名] (id INT);"));
        preview.show();
        QTRY_VERIFY(preview.document()->firstBlock().layout()->formats().size()>=3);
        QVERIFY(preview.isReadOnly());
        QCOMPARE(preview.document()->documentMargin(), qreal(0));
        QCOMPARE(preview.cursorRect(QTextCursor(preview.document())).topLeft(), QPoint(0, 0));
        const auto formats=preview.document()->firstBlock().layout()->formats();
        const auto typeStart=preview.toPlainText().indexOf("INT");
        QVERIFY(std::any_of(formats.begin(),formats.end(),[typeStart](const auto& format) {
            return format.start==typeStart && format.length==3;
        }));
    }
    void erToolbarTogglesCommentsForTheSelectedTable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ErPane pane(dir.path());
        navidog::er::Project project;
        navidog::TableDefinition annotated;
        annotated.node.kind = "table";
        annotated.node.object = "annotated";
        navidog::TableColumn column;
        column.type = "INTEGER";
        column.comment = "字段说明";
        for (int i = 1; i <= 5; ++i) {
            column.name = QString("field_%1").arg(i);
            annotated.columns.push_back(column);
        }
        project.add(annotated);
        auto plain = annotated;
        plain.node.object = "plain";
        column.comment.clear();
        plain.columns = {column};
        project.add(plain);
        pane.useProject(std::move(project));
        pane.resize(1150,750);
        pane.show();

        auto* comments = pane.findChild<QToolButton*>("erToggleComments");
        auto* list = pane.findChild<QListView*>("erTableList");
        auto* canvas = static_cast<navidog::ui::er::Canvas*>(pane.findChild<QWidget*>("erCanvas"));
        QVERIFY(comments && comments->isVisible() && !comments->isEnabled());
        list->setCurrentIndex(list->model()->index(0,0));
        QVERIFY(comments->isEnabled());
        const auto collapsed = canvas->arrangement().boxes[0];
        canvas->zoom(.2);
        QTest::mouseClick(comments,Qt::LeftButton);
        QCOMPARE(comments->text(),QString("隐藏注释"));
        QVERIFY(comments->isChecked());
        QVERIFY(canvas->arrangement().boxes[0].height() > collapsed.height());

        list->setCurrentIndex(list->model()->index(1,0));
        QVERIFY(!comments->isEnabled());
        QVERIFY(!comments->isChecked());
        list->setCurrentIndex(list->model()->index(0,0));
        QVERIFY(comments->isChecked());
        QTest::mouseClick(comments,Qt::LeftButton);
        QCOMPARE(canvas->arrangement().boxes[0],collapsed);
        QTest::mouseClick(comments,Qt::LeftButton);
        QTest::keyClick(canvas,Qt::Key_Escape);
        QVERIFY(!comments->isChecked());
        QCOMPARE(comments->text(),QString("显示注释"));
        QVERIFY(!pane.dirty());
    }
    void erCanvasEditingWaitsForMouseRelease() {
        QTemporaryDir directory;
        ErPane pane(directory.path());
        navidog::er::Project project;
        navidog::TableDefinition table;
        table.node.kind = "table";
        table.node.object = "owned";
        project.add(table);
        pane.useProject(std::move(project));
        pane.resize(1150,750);
        pane.show();
        auto* canvas = static_cast<navidog::ui::er::Canvas*>(pane.findChild<QWidget*>("erCanvas"));
        canvas->select(0,true);
        checkDialogAfterMouseRelease(canvas,canvas->rect().center(),"编辑模型对象");
    }

    void erListEditingWaitsForMouseRelease() {
        QTemporaryDir directory;
        ErPane pane(directory.path());
        navidog::er::Project project;
        navidog::TableDefinition table;
        table.node.kind = "table";
        table.node.object = "owned";
        project.add(table);
        pane.useProject(std::move(project));
        pane.show();
        auto* list = pane.findChild<QListView*>("erTableList");
        const auto point = list->visualRect(list->model()->index(0,0)).center();
        checkDialogAfterMouseRelease(list->viewport(),point,"编辑模型对象");
    }

    void erWorkspaceImportsLocatesEditsAndRestores() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            MainWindow window(dir.path());
            window.show();
            const auto p = navidog::ProfileStore(navidog::localStorePath(dir.path())).load().front();
            auto db = navidog::connectDatabase(p);
            const auto token = navidog::cancellation();
            for (const auto* sql : {"CREATE TABLE er_parent (id INTEGER PRIMARY KEY, name TEXT, address TEXT, city TEXT, postcode TEXT, phone TEXT)",
                 "CREATE TABLE er_child (id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES er_parent(id))"}) {
                const auto result = db->execute(sql,{},token);
                QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
            }
            auto* tabs = window.findChild<QTabWidget*>("queryTabs");
            auto* originalQuery = tabs->currentWidget();
            window.findChild<QAction*>("menuReverseErModel")->trigger();
            auto* dialog = window.findChild<QDialog*>("erImportDialog");
            QVERIFY(dialog && dialog->isVisible());
            QVERIFY(!QApplication::activeModalWidget());
            QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);
            QVERIFY(tabs->currentWidget() != originalQuery);
            auto* names = dialog->findChild<QLineEdit*>("erImportNames");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(names && buttons);
            names->setText("er_parent, er_child");
            const QPointer<QDialog> importDialog(dialog);
            bool formGoneBeforeTab = false;
            const auto tabOpened = connect(tabs,&QTabWidget::currentChanged,&window,[&] {
                if (qobject_cast<ErPane*>(tabs->currentWidget()))
                    formGoneBeforeTab = !importDialog;
            });
            auto* import = buttons->button(QDialogButtonBox::Save);
            QTest::mouseClick(import,Qt::LeftButton);
            QVERIFY(importDialog && importDialog->isVisible());
            QVERIFY(!import->isEnabled());
            QTRY_VERIFY(window.findChild<ErPane*>());
            disconnect(tabOpened);
            QVERIFY(formGoneBeforeTab);
            auto* pane = window.findChild<ErPane*>();
            QVERIFY(!QApplication::activeModalWidget());
            const auto clickTab = [&](QWidget* target) {
                auto* bar = tabs->tabBar();
                const auto tab = bar->tabRect(tabs->indexOf(target));
                QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,
                                  bar->mapTo(&window,QPoint(tab.left()+12,tab.center().y())));
            };
            clickTab(originalQuery);
            QCOMPARE(tabs->currentWidget(),originalQuery);
            QTRY_COMPARE(pane->project().entities.size(),2);
            QCOMPARE(tabs->currentWidget(),originalQuery);
            clickTab(pane);
            QCOMPARE(tabs->currentWidget(),pane);
            auto* tree = window.findChild<QTreeWidget*>("connectionTree");
            auto* root = tree->topLevelItem(0);
            const auto clickDisclosure = [&](QTreeWidgetItem* item) {
                const auto row = tree->visualItemRect(item);
                const QPoint arrow(row.left()-tree->indentation()/2,row.center().y());
                QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,
                                  tree->viewport()->mapTo(&window,arrow));
            };
            QVERIFY(!root->isExpanded());
            clickDisclosure(root);
            QTRY_COMPARE(root->childCount(),1);
            auto* database = root->child(0);
            clickDisclosure(database);
            QTRY_VERIFY(database->childCount() > 0);
            clickDisclosure(database);
            QVERIFY(!database->isExpanded());
            QCOMPARE(tabs->currentWidget(),pane);

#ifdef Q_OS_MACOS
            // Cocoa menus are tracked natively; the offscreen platform permits
            // a programmatic dismissal once the navigation menu is observed.
            if (QGuiApplication::platformName() != "cocoa") {
#endif
                QSignalSpy menuRequests(tree,&QWidget::customContextMenuRequested);
                QTimer closeMenu;
                connect(&closeMenu,&QTimer::timeout,&window,[] {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    if (menu && menu->objectName() == "nodeContextMenu")
                        menu->close();
                });
                closeMenu.start(10);
                const auto at = tree->viewport()->mapTo(&window,tree->visualItemRect(database).center());
                QTest::mouseClick(window.windowHandle(),Qt::RightButton,Qt::NoModifier,at);
                QTRY_VERIFY(!menuRequests.isEmpty());
                QCoreApplication::processEvents();
                QCOMPARE(menuRequests.size(),1);
                QCOMPARE(tree->currentItem(),database);
                QVERIFY(!QApplication::activePopupWidget());
#ifdef Q_OS_MACOS
            }
#endif

            auto* list = pane->findChild<QListView*>("erTableList");
            auto* search = pane->findChild<QLineEdit*>("erSearch");
            auto* canvas = static_cast<navidog::ui::er::Canvas*>(pane->findChild<QWidget*>("erCanvas"));
            search->setText("child");
            QTRY_COMPARE(list->model()->rowCount(),1);
            list->setCurrentIndex(list->model()->index(0,0));
            QCOMPARE(list->currentIndex().data().toString(),QString("er_child"));
            const auto& entities = pane->project().entities;
            const auto target = std::find_if(entities.cbegin(),entities.cend(),[](const auto& e){ return e.table.node.object == "er_parent"; });
            QVERIFY(target!=entities.cend());
            qreal zoom = 0;
            const auto updateZoom = canvas->zoomed;
            canvas->zoomed = [&](qreal scale) { zoom = scale; updateZoom(scale); };
            canvas->select(int(target-entities.cbegin()),true);
            QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,canvas->rect().center());
            QCOMPARE(list->currentIndex().data().toString(),QString("er_parent"));
            QVERIFY(search->text().isEmpty());
            const int targetIndex = int(target-entities.cbegin());
            const auto collapsed = canvas->arrangement().boxes[targetIndex];
            const auto link = canvas->rect().center() + QPoint(0,qRound((navidog::er::cardHeight/2-12)*zoom));
            QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,link);
            const auto expanded = canvas->arrangement().boxes[targetIndex];
            QCOMPARE(expanded.topLeft(),collapsed.topLeft());
            QCOMPARE(expanded.width(),collapsed.width());
            QVERIFY(expanded.height() > collapsed.height());
            QVERIFY(!QApplication::activePopupWidget());
            QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,link+QPoint(0,qRound((expanded.height()-collapsed.height())*zoom)));
            QCOMPARE(canvas->arrangement().boxes[targetIndex],collapsed);
            canvas->zoomed = updateZoom;

            QTimer::singleShot(30,&window,[] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                auto* name = dialog->findChild<QLineEdit*>("tableName");
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                if (!name || !buttons) { dialog->reject(); return; }
                name->setText("sales_note");
                buttons->button(QDialogButtonBox::Save)->click();
            });
            QTest::mouseClick(pane->findChild<QToolButton*>("erNewTable"),Qt::LeftButton);
            QCOMPARE(pane->project().entities.size(),3);
            QCOMPARE(pane->project().entities.back().table.node.object,QString("sales_note"));
            QString generated;
            QElapsedTimer timeout;
            timeout.start();
            QTimer previewDriver;
            connect(&previewDriver,&QTimer::timeout,&window,[&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (auto* text = dialog->findChild<QPlainTextEdit*>("erDdlPreview")) generated = text->toPlainText();
                if (!generated.isEmpty() || timeout.elapsed()>4000) dialog->reject();
            });
            previewDriver.start(15);
            QTest::mouseClick(pane->findChild<QToolButton*>("erDDL"),Qt::LeftButton);
            previewDriver.stop();
            QVERIFY(generated.contains("CREATE TABLE \"main\".\"sales_note\""));
            const auto capture = qEnvironmentVariable("NAVIDOG_TEST_ER_WORKSPACE_SCREENSHOT");
            if (!capture.isEmpty()) {
                QTest::mouseClick(pane->findChild<QToolButton*>("erFit"),Qt::LeftButton);
                QVERIFY(pane->grab().save(capture));
            }
            QVERIFY(window.close());
        }
        MainWindow restored(dir.path());
        auto* pane = restored.findChild<ErPane*>();
        QVERIFY(pane);
        QTRY_COMPARE(pane->project().entities.size(),3);
        QVERIFY(pane->dirty());
    }
    void tableDoubleClickKeepsWorkspaceTabsAndBranchesInteractive() {
        QTemporaryDir dir;
        MainWindow window(dir.path());
        window.show();
        window.activateWindow();
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");
        auto* original=tabs->currentWidget();
        auto* root=tree->topLevelItem(0);
        root->setExpanded(true);
        QTRY_COMPARE(root->childCount(),1);
        auto* database=root->child(0);
        database->setExpanded(true);
        QTRY_VERIFY(database->childCount()>0);
        auto* folder=database->child(0);
        folder->setExpanded(true);
        QTRY_VERIFY(folder->childCount()>0);
        auto* table=folder->child(0);
        QCOMPARE(table->data(0,Qt::UserRole+2).value<navidog::CatalogNode>().kind,QString("table"));

        const auto point=tree->visualItemRect(table).center();
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QSignalSpy activated(tree,&QTreeWidget::itemDoubleClicked);
        QTest::mouseDClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QCOMPARE(activated.count(),1);
        QCOMPARE(tabs->currentWidget(),original);
        QTest::mouseRelease(tree->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QTRY_VERIFY(dynamic_cast<DataPane*>(tabs->currentWidget()));
        const auto previewIndex=tabs->currentIndex();
        auto clickWindow=[&](QWidget* widget,QPoint at) {
            QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,widget->mapTo(&window,at));
        };
        auto* bar=tabs->tabBar();
        const auto initialTab=bar->tabRect(tabs->indexOf(original));
        clickWindow(bar,{initialTab.left()+12,initialTab.center().y()});
        QCOMPARE(tabs->currentWidget(),original);
        auto* close=bar->tabButton(previewIndex,QTabBar::RightSide);
        QVERIFY(close);
        clickWindow(close,close->rect().center());
        QTRY_COMPARE(tabs->count(),1);

        const auto row=tree->visualItemRect(folder);
        clickWindow(tree->viewport(),{row.left()-tree->indentation()/2,row.center().y()});
        QVERIFY(!folder->isExpanded());
        clickWindow(tree->viewport(),{row.left()-tree->indentation()/2,row.center().y()});
        QVERIFY(folder->isExpanded());
    }
    void navigationRowsFillTheViewportAndKeepLongNamesScrollable() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.resize(1200, 780);
        window.show();
        auto *splitter = window.findChild<QSplitter *>("workspaceSplitter");
        auto *tree = window.findChild<QTreeWidget *>("connectionTree");
        auto *row = tree->topLevelItem(0);
        QVERIFY(row);

        for (int width : {510, 275}) {
            splitter->setSizes({width, 1200 - width});
            QTRY_COMPARE(tree->columnWidth(0), tree->viewport()->width());
            const auto farEdge = QPoint(tree->viewport()->width() - 4, tree->visualItemRect(row).center().y());
            QCOMPARE(tree->itemAt(farEdge), row);
        }

        const auto point = QPoint(tree->viewport()->width() - 4, tree->visualItemRect(row).center().y());
        tree->clearSelection();
        const auto before = tree->viewport()->grab();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(tree->currentItem(), row);
        QVERIFY(row->isSelected());
        const auto after = tree->viewport()->grab();
        const auto pixel = point * after.devicePixelRatio();
        QVERIFY(after.toImage().pixelColor(pixel) != before.toImage().pixelColor(pixel));

        const auto name = row->text(0);
        row->setText(0, QString(180, 'W'));
        QTRY_VERIFY(tree->horizontalScrollBar()->maximum() > 0);
        QVERIFY(tree->columnWidth(0) > tree->viewport()->width());
        row->setText(0, name);
        QTRY_COMPARE(tree->columnWidth(0), tree->viewport()->width());
        QTRY_COMPARE(tree->horizontalScrollBar()->maximum(), 0);
    }

    void navigationDoubleClickTogglesWithoutClosingItsSession_data() {
        QTest::addColumn<bool>("connectionNode");
        QTest::newRow("connection") << true;
        QTest::newRow("database") << false;
    }

    void navigationDoubleClickTogglesWithoutClosingItsSession() {
        QFETCH(bool, connectionNode);
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *navigation = window.findChild<NavigationPane *>("navigationPane");
        auto *tree = navigation->findChild<QTreeWidget *>("connectionTree");
        auto *service = window.findChild<navidog::QueryService *>();
        auto *connection = navigation->connectionItem("harbor-demo");
        auto *node = connection;
        if (!connectionNode) {
            connection->setExpanded(true);
            QTRY_COMPARE(connection->childCount(), 1);
            node = connection->child(0);
        }
        const auto isOpen = [&] {
            return connectionNode ? service->isConnected("harbor-demo")
                                  : navigation->namespaceOpen(node);
        };
        const auto doubleClickNode = [&] {
            const auto point = tree->visualItemRect(node).center();
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        };

        QVERIFY(!node->isExpanded());
        doubleClickNode();
        QTRY_VERIFY(node->isExpanded());
        QTRY_VERIFY(node->childCount() > 0);
        QVERIFY(isOpen());
        const auto firstChild = navigation->indexFor(node->child(0));
        const auto sessions = service->sessionCount();
        QVERIFY(sessions > 0);
        QSignalSpy catalogResults(service, &navidog::QueryService::catalogCompleted);

        doubleClickNode();
        QTRY_VERIFY(!node->isExpanded());
        QVERIFY(firstChild.isValid());
        QVERIFY(isOpen());
        QCOMPARE(service->sessionCount(), sessions);

        doubleClickNode();
        QTRY_VERIFY(node->isExpanded());
        QCOMPARE(navigation->indexFor(node->child(0)), firstChild);
        QVERIFY(!service->catalogBusy());
        QCOMPARE(catalogResults.count(), 0);
    }

    void navigationIconsKeepTheirArtworkAndDimWhenClosed() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *navigation = window.findChild<NavigationPane *>("navigationPane");
        auto *tree = navigation->findChild<QTreeWidget *>("connectionTree");
        auto *service = window.findChild<navidog::QueryService *>();
        auto *connection = navigation->connectionItem("harbor-demo");
        QVERIFY(connection);

        const auto paintIcon = [&](QTreeWidgetItem *item) {
            QStyleOptionViewItem option;
            option.initFrom(tree);
            option.widget = tree;
            option.rect = QRect(0, 0, 200, 28);
            option.decorationSize = QSize(16, 16);
            option.state = QStyle::State_Enabled;
            QImage row(option.rect.size(), QImage::Format_RGB32);
            row.fill(Qt::white);
            {
                QPainter painter(&row);
                tree->itemDelegate()->paint(&painter, option, navigation->indexFor(item));
            }
            return row.copy(0, 0, 20, row.height());
        };
        const auto contrast = [](const QImage &icon) {
            int darkness = 0;
            for (int y = 0; y < icon.height(); ++y)
                for (int x = 0; x < icon.width(); ++x)
                    darkness += 255 - qGray(icon.pixel(x, y));
            return darkness;
        };

        const auto connectionIcon = connection->icon(0).cacheKey();
        const auto closedConnection = paintIcon(connection);
        navigation->openConnection(connection);
        QTRY_COMPARE(connection->childCount(), 1);
        QVERIFY(service->isConnected("harbor-demo"));
        QCOMPARE(connection->icon(0).cacheKey(), connectionIcon);
        const auto openConnection = paintIcon(connection);
        QVERIFY(contrast(closedConnection) > 0);
        QVERIFY(contrast(closedConnection) < contrast(openConnection));

        auto *database = connection->child(0);
        const auto databaseIcon = database->icon(0).cacheKey();
        const auto closedDatabase = paintIcon(database);
        navigation->openDatabase(database);
        QTRY_VERIFY(navigation->namespaceOpen(database));
        QCOMPARE(database->icon(0).cacheKey(), databaseIcon);
        const auto openDatabase = paintIcon(database);
        QVERIFY(contrast(closedDatabase) > 0);
        QVERIFY(contrast(closedDatabase) < contrast(openDatabase));

        database->setExpanded(false);
        QCOMPARE(paintIcon(database), openDatabase);
        connection->setExpanded(false);
        QCOMPARE(paintIcon(connection), openConnection);

        navigation->clearNamespace(database);
        QCOMPARE(database->icon(0).cacheKey(), databaseIcon);
        QCOMPARE(paintIcon(database), closedDatabase);
        service->disconnectProfile("harbor-demo");
        QTRY_VERIFY(!service->isConnected("harbor-demo"));
        QCOMPARE(connection->icon(0).cacheKey(), connectionIcon);
        QCOMPARE(paintIcon(connection), closedConnection);
    }

    void navigationObjectFolderOpensListOnDoubleClick() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *navigation = window.findChild<NavigationPane *>("navigationPane");
        auto *tree = navigation->findChild<QTreeWidget *>("connectionTree");
        auto *tabs = window.findChild<QTabWidget *>("queryTabs");
        auto *initialTab = tabs->currentWidget();

        auto *connection = navigation->connectionItem("harbor-demo");
        connection->setExpanded(true);
        QTRY_COMPARE(connection->childCount(), 1);
        auto *database = connection->child(0);
        database->setExpanded(true);
        QTRY_COMPARE(database->childCount(), 3);
        auto *folder = navigation->findCatalogItem("harbor-demo", [](const auto &node) {
            return node.kind == "tables";
        });
        QVERIFY(folder);
        const auto clickFolder = [&] {
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                              tree->visualItemRect(folder).center());
        };
        const auto doubleClickFolder = [&] {
            clickFolder();
            const auto point = tree->visualItemRect(folder).center();
            QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        };

        clickFolder();
        QCOMPARE(tree->currentItem(), folder);
        QVERIFY(!folder->isExpanded());
        QCOMPARE(tabs->currentWidget(), initialTab);

        doubleClickFolder();
        QTRY_VERIFY(folder->isExpanded());
        auto *objects = dynamic_cast<ObjectsPane *>(tabs->currentWidget());
        QVERIFY(objects);
        QCOMPARE(objects->folder.kind, QString("tables"));
        QTRY_COMPARE(objects->findChild<QTableView *>("objectList")->model()->rowCount(), 2);

        tabs->setCurrentWidget(initialTab);
        doubleClickFolder();
        QTRY_VERIFY(!folder->isExpanded());
        QCOMPARE(tabs->currentWidget(), initialTab);

        doubleClickFolder();
        QTRY_VERIFY(folder->isExpanded());
        QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(objects));
    }

    void navigationSearchRevealsLoadedObjectsAndRestoresExpansion() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *navigation = window.findChild<NavigationPane *>("navigationPane");
        QVERIFY(navigation);
        auto *tree = navigation->findChild<QTreeWidget *>("connectionTree");
        auto *search = navigation->findChild<QLineEdit *>("connectionSearch");
        auto *collapse = navigation->findChild<QToolButton *>("collapseNavigation");
        auto *locate = navigation->findChild<QToolButton *>("locateNavigationItem");
        QVERIFY(tree && search && collapse && locate);

        auto *connection = navigation->connectionItem("harbor-demo");
        QVERIFY(connection);
        connection->setExpanded(true);
        QTRY_VERIFY(connection->childCount() > 0);
        auto *database = connection->child(0);
        database->setExpanded(true);
        QTRY_VERIFY(database->childCount() > 0);
        auto *folder = navigation->findCatalogItem("harbor-demo", [](const auto &node) {
            return node.kind == "tables";
        });
        QVERIFY(folder);
        folder->setExpanded(true);
        QTRY_VERIFY(folder->childCount() > 0);
        QTest::mouseClick(collapse, Qt::LeftButton);
        QVERIFY(!connection->isExpanded());
        QVERIFY(!database->isExpanded());
        QVERIFY(!folder->isExpanded());

        auto *service = window.findChild<navidog::QueryService *>();
        QSignalSpy completed(service, &navidog::QueryService::catalogCompleted);
        search->setText("orders");
        QVERIFY(connection->isExpanded());
        QVERIFY(database->isExpanded());
        QVERIFY(folder->isExpanded());
        QVERIFY(!folder->isHidden());
        search->clear();
        QVERIFY(!connection->isExpanded());
        QVERIFY(!database->isExpanded());
        QVERIFY(!folder->isExpanded());
        QVERIFY(!locate->isEnabled());

        auto *orders = navigation->findCatalogItem("harbor-demo", [](const auto &node) {
            return node.kind == "table" && node.object == "orders";
        });
        auto *customers = navigation->findCatalogItem("harbor-demo", [](const auto &node) {
            return node.kind == "table" && node.object == "customers";
        });
        QVERIFY(orders && customers);
        search->setText("orders");
        QVERIFY(customers->isHidden());
        tree->setCurrentItem(orders);
        QVERIFY(locate->isEnabled());
        QTest::mouseClick(locate, Qt::LeftButton);
        QVERIFY(search->text().isEmpty());
        QCOMPARE(tree->currentItem(), orders);
        QVERIFY(!customers->isHidden());
        QVERIFY(connection->isExpanded());
        QVERIFY(database->isExpanded());
        QVERIFY(folder->isExpanded());
        QVERIFY(tree->viewport()->rect().intersects(tree->visualItemRect(orders)));
        QVERIFY(!locate->isEnabled());
        QCoreApplication::processEvents();
        QVERIFY(!service->catalogBusy());
        QCOMPARE(completed.count(), 0);
    }

    void navigationFiltersCachedNamesWithoutConnecting() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = localStorePath(directory.path());
        ConnectionProfile offline;
        offline.id = "offline-navigation";
        offline.name = "Offline fixture";
        offline.driver = DatabaseKind::PostgreSQL;
        offline.database = "owned";
        ProfileStore(path).save({offline});

        MetadataSnapshot snapshot;
        snapshot.profileId = offline.id;
        snapshot.identity = metadataIdentity(offline);
        snapshot.database = offline.database;
        snapshot.schema = "public";
        snapshot.driver = offline.driver;
        snapshot.key = metadataKey(offline, snapshot.database, snapshot.schema);
        snapshot.refreshedMs = QDateTime::currentMSecsSinceEpoch() - 2 * MetadataSnapshot::ttlMs;
        MetadataObject table;
        table.key = table.name = "offline_marker";
        table.kind = "table";
        table.database = snapshot.database;
        table.schema = snapshot.schema;
        table.columns.push_back({"offline_marker_id", "integer"});
        snapshot.objects.push_back(table);
        { MetadataStore cache(path); cache.replace(snapshot); }

        MainWindow window(directory.path());
        window.show();
        auto *service = window.findChild<QueryService *>();
        auto *search = window.findChild<QLineEdit *>("connectionSearch");
        auto *results = window.findChild<QTreeWidget *>("cachedNavigationResults");
        QVERIFY(search && service && results);
        QSignalSpy catalog(service, &QueryService::catalogCompleted);
        search->setText("OFFLINE_MARKER");
        QTRY_VERIFY(results->isVisible());
        QCOMPARE(results->topLevelItemCount(), 1);
        auto *connection = results->topLevelItem(0);
        QCOMPARE(connection->text(0), offline.name);
        QCOMPARE(connection->childCount(), 2);
        QVERIFY(connection->child(0)->text(0).startsWith("offline_marker  ·"));
        QVERIFY(connection->child(1)->text(0).startsWith("offline_marker_id  ·"));
        QVERIFY(connection->child(1)->icon(0).isNull());
        QVERIFY(!service->isConnected(offline.id));
        QCOMPARE(catalog.count(), 0);

        const auto point = results->visualItemRect(connection->child(0)).center();
        QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseDClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(results->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(search->text().isEmpty());
        QVERIFY(!results->isVisible());
        auto *tree = window.findChild<QTreeWidget *>("connectionTree");
        QVERIFY(tree && tree->currentItem());
        QCOMPARE(tree->currentItem()->text(0), offline.name);
        { MetadataStore cache(path); cache.invalidate(offline.id); }
        search->setText("offline_marker");
        QTest::qWait(350);
        QVERIFY(!results->isVisible());
        QCOMPARE(catalog.count(), 0);
    }

    void closingConnectionKeepsOtherNavigationInteractive() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto database = navidog::ensureDemoDatabase(dir.path());
        navidog::ProfileStore store(navidog::localStorePath(dir.path()));
        QVector<navidog::ConnectionProfile> profiles;
        for (auto name : {"owned-first", "owned-second", "owned-third"}) {
            navidog::ConnectionProfile profile;
            profile.id = profile.name = name;
            profile.database = database;
            profiles << profile;
        }
        store.save(profiles);

        MainWindow window(dir.path());
        window.show();
        window.activateWindow();
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        auto* service = window.findChild<navidog::QueryService*>();
        auto* tabs = window.findChild<QTabWidget*>("queryTabs");
        auto* query = qobject_cast<QueryPane*>(tabs->currentWidget());
        QVERIFY(query);
        const auto byId = [&](const QString& id) -> QTreeWidgetItem* {
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
                if (auto* root = tree->topLevelItem(i); root->data(0,Qt::UserRole).toString() == id)
                    return root;
            return nullptr;
        };
        auto* first = byId("owned-first");
        auto* second = byId("owned-second");
        auto* third = byId("owned-third");
        QVERIFY(first && second && third);
        first->setExpanded(true);
        QTRY_VERIFY(service->isConnected("owned-first"));
        QTRY_COMPARE(first->childCount(),1);

        query->connection->setCurrentIndex(query->connection->findData("owned-first"));
        query->editor->setPlainText("SELECT 42;");
        QSignalSpy executed(&window,&MainWindow::queryFinished);
        window.runCurrent();
        QTRY_COMPARE(executed.count(),1);

        tree->setCurrentItem(first);
        auto* close = window.findChild<QAction*>("menuCloseConnection");
        QVERIFY(close->isEnabled());
        bool menuSelected = false;
        bool confirmed = false;
        QTimer menuDriver;
        connect(&menuDriver,&QTimer::timeout,&window,[&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                confirmed = true;
                box->button(QMessageBox::Yes)->click();
            } else if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); menu && !menuSelected) {
                for (auto* action : menu->actions()) {
                    if (action->text() != close->text()) continue;
                    menuSelected = true;
                    QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,menu->actionGeometry(action).center());
                    return;
                }
                menu->close();
            }
        });
        menuDriver.start(10);
        if (QGuiApplication::platformName() == "offscreen") {
            QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(first).center());
            QTRY_VERIFY(menuSelected);
        } else {
            close->trigger();
        }
        QTRY_VERIFY(confirmed);
        menuDriver.stop();
        QTRY_COMPARE(first->childCount(),0);

        second->setExpanded(true);
        third->setExpanded(true);
        QTRY_COMPARE(second->childCount(),1);
        QTRY_COMPARE(third->childCount(),1);
        QVERIFY(!service->isConnected("owned-first"));
        QCOMPARE(query->editor->toPlainText(),QString("SELECT 42;"));
        QCOMPARE(tabs->currentWidget(),query);

        const auto row = tree->visualItemRect(third);
        const QPoint arrow(row.left() - tree->indentation()/2,row.center().y());
        const auto clickArrow = [&] {
            QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,
                              tree->viewport()->mapTo(&window,arrow));
        };
        clickArrow();
        QVERIFY(!third->isExpanded());
        clickArrow();
        QVERIFY(third->isExpanded());

        first->setExpanded(true);
        QTRY_COMPARE(first->childCount(),1);
    }
    void committedEditorDdlReconcilesLoadedTreeWithoutReopeningBranches() {
        QTemporaryDir dir;
        MainWindow window(dir.path());
        window.show();
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        auto* query=dynamic_cast<QueryPane*>(tabs->currentWidget());
        QVERIFY(query);
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");
        QSignalSpy executed(window.findChild<navidog::QueryService*>(),&navidog::QueryService::completed);
        QTimer confirmDdl;
        connect(&confirmDdl, &QTimer::timeout, &window, [&] {
            if (auto* dialog = window.findChild<QMessageBox*>("confirmQuerySafety"); dialog && dialog->isVisible())
                dialog->findChild<QPushButton*>("executeRiskySql")->click();
        });
        confirmDdl.start(10);
        auto run=[&](const QString& sql) {
            const int before=executed.count();
            query->editor->setPlainText(sql);
            QTRY_VERIFY(query->run->isEnabled());
            query->run->click();
            QTRY_COMPARE(executed.count(),before+1);
            const auto result=executed.last().front().value<navidog::QueryOutcome>();
            QVERIFY2(result.result.error.isEmpty(),qPrintable(result.result.error));
        };
        run("CREATE TABLE owned_drop_a(id INTEGER); CREATE TABLE owned_drop_b(id INTEGER);");
        auto* root=tree->topLevelItem(0);
        root->setExpanded(true);
        QTRY_COMPARE(root->childCount(),1);
        auto* database=root->child(0);
        database->setExpanded(true);
        QTRY_VERIFY(database->childCount()>0);
        auto* folder=database->child(0);
        folder->setExpanded(true);
        const auto lookup=[&](const QString& name)->QTreeWidgetItem* {
            for(int i=0;i<folder->childCount();++i)if(folder->child(i)->text(0)==name)return folder->child(i);
            return nullptr;
        };
        QTRY_VERIFY(lookup("owned_drop_a") && lookup("owned_drop_b"));
        const QPersistentModelIndex retained=tree->indexFromItem(lookup("owned_drop_b"));
        folder->setExpanded(false);
        run("DROP TABLE owned_drop_a;");
        QTRY_VERIFY(!lookup("owned_drop_a"));
        QVERIFY(!folder->isExpanded());
        QVERIFY(retained.isValid());
        QCOMPARE(tree->itemFromIndex(retained),lookup("owned_drop_b"));

        run("BEGIN; DROP TABLE owned_drop_b;");
        QVERIFY(!executed.last().front().value<navidog::QueryOutcome>().catalogChanged);
        QVERIFY(lookup("owned_drop_b"));
        run("ROLLBACK;");
        QVERIFY(!executed.last().front().value<navidog::QueryOutcome>().catalogChanged);
        QVERIFY(lookup("owned_drop_b"));
        run("BEGIN; DROP TABLE owned_drop_b; COMMIT;");
        QVERIFY(executed.last().front().value<navidog::QueryOutcome>().catalogChanged);
        QTRY_VERIFY(!lookup("owned_drop_b"));
        QVERIFY(!retained.isValid());
        QVERIFY(!folder->isExpanded());
    }
    void dangerousWritesWarnPreviewAndCancelBeforeExecution() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.id = "owned-write-review";
        profile.name = "Write review fixture";
        profile.database = ensureDemoDatabase(directory.path());
        ProfileStore(localStorePath(directory.path())).save({profile});
        auto db = connectDatabase(profile);
        const auto token = cancellation();
        QVERIFY(db->execute("CREATE TABLE owned_guard(id INTEGER PRIMARY KEY, status INTEGER)", {}, token).error.isEmpty());
        QVERIFY(db->execute("INSERT INTO owned_guard VALUES (1,1),(2,1),(3,1)", {}, token).error.isEmpty());

        auto settings = loadApplicationSettings(localStorePath(directory.path()));
        settings.querySafety.previewWhileEditing = true;
        settings.querySafety.timeoutMs = 1500;
        saveApplicationSettings(localStorePath(directory.path()), settings);
        QCOMPARE(loadApplicationSettings(localStorePath(directory.path())).querySafety.timeoutMs, 1500);
        MainWindow window(directory.path());
        window.show();
        auto* pane = qobject_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());
        QVERIFY(pane);
        QSignalSpy completed(window.findChild<QueryService*>(), &QueryService::completed);
        pane->editor->setPlainText("SELECT id FROM owned_guard");
        pane->run->click();
        QTRY_COMPARE(completed.size(), 1);

        pane->editor->setPlainText("DELETE FROM owned_guard");
        QTRY_VERIFY(pane->editor->diagnosticAtPosition(0).contains("缺少筛选条件"));
        pane->editor->setFocus();
        pane->editor->setPlainText("SELECT 1;\nDELETE FROM owned_guard WHERE status = 1");
        pane->editor->moveCursor(QTextCursor::End);
        QTRY_VERIFY_WITH_TIMEOUT(pane->editor->diagnosticAtPosition(10).contains("3 条 / 共 3 条"), 5000);

        QString review;
        bool accept = false;
        QTimer responder;
        connect(&responder, &QTimer::timeout, &window, [&] {
            if (auto* dialog = window.findChild<QMessageBox*>("confirmQuerySafety"); dialog && dialog->isVisible()) {
                review = dialog->detailedText();
                auto* button = accept ? dialog->findChild<QPushButton*>("executeRiskySql") :
                                        dialog->button(QMessageBox::Cancel);
                button->click();
            }
        });
        responder.start(10);
        pane->editor->setPlainText("DELETE FROM owned_guard WHERE status = 1");
        pane->run->click();
        QTRY_VERIFY_WITH_TIMEOUT(review.contains("3 条 / 共 3 条"), 5000);
        QCOMPARE(completed.size(), 1);
        const auto messages = pane->messages->toPlainText();
        QVERIFY(messages.contains("执行前预查 · 第 1 条 · 匹配行数"));
        QVERIFY(messages.contains("执行前预查 · 第 1 条 · 目标总行数"));
        QVERIFY(messages.contains("SELECT COUNT(*) FROM (SELECT 1 AS navidog_row FROM owned_guard"));
        QVERIFY(!messages.contains("DELETE FROM owned_guard"));
        QCOMPARE(db->execute("SELECT COUNT(*) FROM owned_guard", {}, token).rows.front().front().toInt(), 3);
        accept = true;
        pane->run->click();
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 5000);
        QVERIFY(pane->messages->toPlainText().contains("DELETE FROM owned_guard WHERE status = 1"));
        QCOMPARE(db->execute("SELECT COUNT(*) FROM owned_guard", {}, token).rows.front().front().toInt(), 0);

        // Safe predicates that cannot be measured (e.g. no read permission)
        // proceed unless the optional unmeasured confirmation is enabled.
        pane->editor->setPlainText("UPDATE owned_guard SET status=2 WHERE ABS(id) = 1");
        review.clear();
        pane->run->click();
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 3, 3000);
        QVERIFY(review.isEmpty());
    }

    void sqlMessagesShowSubmittedTextAndLocateSelection() {
        using namespace navidog;
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto* pane = qobject_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());
        QVERIFY(pane);
        const QString first = QString::fromUtf8("SELECT '中文😀' AS marker;\n");
        const QString longStatement = QString("SELECT '") + QString(2500, QChar('x')) + "TAIL_SENTINEL' AS value";
        pane->editor->setPlainText(first + longStatement);
        QTextCursor selection(pane->editor->document());
        selection.setPosition(int(first.size()));
        selection.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        pane->editor->setTextCursor(selection);
        QSignalSpy completed(window.findChild<QueryService*>(), &QueryService::completed);
        pane->run->click();
        QTRY_COMPARE(completed.size(), 1);
        const auto output = pane->messages->toPlainText();
        QVERIFY(output.contains("执行 SQL · 第 1 条 · 第 2 行 · 定位 SQL"));
        QVERIFY(output.contains("显示已截断"));
        QVERIFY(!output.contains("TAIL_SENTINEL"));
        QVERIFY(!output.contains("AS marker"));
        pane->resultTabs->setCurrentWidget(pane->messages);
        pane->editor->setTextCursor(QTextCursor(pane->editor->document()));
        pane->messages->moveCursor(QTextCursor::Start);
        QTest::keyClick(pane->messages, Qt::Key_Return);
        QCOMPARE(pane->editor->textCursor().selectionStart(), int(first.size()));
        QCOMPARE(pane->editor->textCursor().selectedText(), longStatement);

        pane->editor->setPlainText(first + "SELECT 27 AS second");
        auto atSecond = pane->editor->textCursor();
        atSecond.movePosition(QTextCursor::End);
        pane->editor->setTextCursor(atSecond);
        pane->runStatement->click();
        QTRY_COMPARE(completed.size(), 2);
        QVERIFY(pane->messages->toPlainText().contains("SELECT 27 AS second"));
        auto latest = pane->messages->document()->findBlockByNumber(0);
        for (auto block = latest; block.isValid(); block = block.next())
            if (block.text().contains("定位 SQL")) latest = block;
        pane->resultTabs->setCurrentWidget(pane->messages);
        pane->messages->setTextCursor(QTextCursor(latest));
        QTest::keyClick(pane->messages, Qt::Key_Return);
        QCOMPARE(pane->editor->textCursor().selectedText(), QString("SELECT 27 AS second"));

        pane->editor->setPlainText("SELECT 1");
        QTest::keyClick(pane->messages, Qt::Key_Return);
        QVERIFY(!pane->editor->textCursor().hasSelection());
    }
    void completionLeavesTreeBranchesAndWorkspaceTabsClickable() {
        QWidget window;
        QHBoxLayout layout(&window);
        navidog::ui::ObjectTree tree;
        tree.setHeaderHidden(true);
        auto* group = new QTreeWidgetItem(&tree, {"Owned group"});
        new QTreeWidgetItem(group, {"Owned child"});
        layout.addWidget(&tree);

        navidog::ui::WorkspaceTabs tabs;
        auto* alternate = new QWidget;
        auto* editor = new SqlEditor;
        tabs.addTab(alternate, "Alternate");
        tabs.addTab(editor, "SQL");
        tabs.setCurrentWidget(editor);
        layout.addWidget(&tabs, 3);
        connect(&tabs, &QTabWidget::tabCloseRequested, &tabs, &QTabWidget::removeTab);
        window.resize(1000, 640);
        window.show();
        window.activateWindow();
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        auto* popup = editor->findChild<QTreeView*>("sqlCompletionPopup");
        QVERIFY(popup);
        auto clickWindow = [&](QWidget* target, QPoint pos) {
            QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                              target->mapTo(&window, pos));
        };
        const auto item = tree.visualItemRect(group);
        const QPoint arrow(item.left() - tree.indentation() / 2, item.center().y());

        QTest::keyClicks(editor, "S");
        QTRY_VERIFY(popup->isVisible());
        QVERIFY(!QApplication::activePopupWidget());
        clickWindow(tree.viewport(), arrow);
        QVERIFY(group->isExpanded());
        QVERIFY(!popup->isVisible());
        clickWindow(tree.viewport(), arrow);
        QVERIFY(!group->isExpanded());

        editor->setFocus();
        QTest::keyClicks(editor, "E");
        QTRY_VERIFY(popup->isVisible());
        auto* bar = tabs.tabBar();
        const auto alternateTab = bar->tabRect(0);
        clickWindow(bar, {alternateTab.left() + 12, alternateTab.center().y()});
        QCOMPARE(tabs.currentWidget(), alternate);
        QVERIFY(!popup->isVisible());

        tabs.setCurrentWidget(editor);
        editor->setFocus();
        QTest::keyClicks(editor, "L");
        QTRY_VERIFY(popup->isVisible());
        const auto firstCandidate = popup->model()->index(0, 0);
        const auto insertText = firstCandidate.data(Qt::UserRole).toString();
        clickWindow(popup->viewport(), popup->visualRect(firstCandidate).center());
        QCOMPARE(editor->toPlainText(), insertText);
        QVERIFY(editor->hasFocus());
        QVERIFY(!popup->isVisible());

        editor->clear();
        QTest::keyClicks(editor, "S");
        QTRY_VERIFY(popup->isVisible());
        auto* close = bar->tabButton(1, QTabBar::RightSide);
        QVERIFY(close && close->isVisible());
        clickWindow(close, close->rect().center());
        QCOMPARE(tabs.count(), 1);
        QCOMPARE(tabs.currentWidget(), alternate);
        QVERIFY(!popup->isVisible());
    }
    void diagnosticHoverCardIsBoundedEscapedAndNonActivating() {
      SqlEditor editor;editor.applyPreferences(14,2,false,false);editor.setDiagnosticContext(navidog::DatabaseKind::PostgreSQL);
      editor.setPlainText("SELECT FROM");editor.resize(780,460);editor.show();editor.activateWindow();editor.setFocus();
      QTRY_VERIFY_WITH_TIMEOUT(!editor.diagnostics().isEmpty(),4000);
      const auto mark=editor.diagnostics().front();auto cursor=editor.textCursor();cursor.setPosition(mark.begin);editor.setTextCursor(cursor);
      const auto caret=editor.cursorRect(cursor);QHelpEvent help(QEvent::ToolTip,caret.center(),editor.viewport()->mapToGlobal(caret.center()));
      QApplication::sendEvent(editor.viewport(),&help);auto *card=editor.findChild<QFrame*>("sqlDiagnosticCard");QVERIFY(card);QVERIFY(card->isVisible());
      QVERIFY(card->testAttribute(Qt::WA_ShowWithoutActivating));QVERIFY(card->testAttribute(Qt::WA_TransparentForMouseEvents));QVERIFY(editor.hasFocus());
      auto *title=card->findChild<QLabel*>("diagnosticCardTitle"),*body=card->findChild<QLabel*>("diagnosticCardBody"),*footer=card->findChild<QLabel*>("diagnosticCardFooter");
      QVERIFY(title && body && footer);QVERIFY(title->text().contains("SQL"));QVERIFY(footer->text().contains("第 1 行"));QCOMPARE(body->textFormat(),Qt::PlainText);QVERIFY(card->width()<=440);
      const auto screenshot=qEnvironmentVariable("NAVIDOG_TEST_HOVER_SCREENSHOT");if(!screenshot.isEmpty())QVERIFY(card->grab().save(screenshot));
      QTest::keyClick(&editor,Qt::Key_End);QVERIFY(!card->isVisible());QCOMPARE(editor.toPlainText(),QString("SELECT FROM"));
      // A completion popup above the cursor must also leave a full gap.
      QWidget hint;hint.resize(300,220);auto area=QGuiApplication::primaryScreen()->availableGeometry();
      QRect lowCaret(area.left()+30,area.bottom()-45,2,20);QVERIFY(navidog::ui::placeEditorHint(&hint,lowCaret,32));
      QVERIFY(hint.geometry().bottom()<=lowCaret.top()-32);QVERIFY(area.contains(hint.geometry()));
    }
    void completionSettingsMigrateThePreviousDefault() {
        QTemporaryDir owned;const auto path=owned.filePath("settings.json");ApplicationSettings settings;QCOMPARE(settings.completionMinPrefix,1);settings.completionMinPrefix=2;saveApplicationSettings(path,settings);
        QCOMPARE(loadApplicationSettings(path).completionMinPrefix,2);
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));auto root=QJsonDocument::fromJson(file.readAll()).object();file.close();root.remove("completionBehaviorVersion");QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(root).toJson());file.close();QCOMPARE(loadApplicationSettings(path).completionMinPrefix,1);
        root["completionMinPrefix"]=3;QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(root).toJson());file.close();QCOMPARE(loadApplicationSettings(path).completionMinPrefix,3);
    }
    void sqlEditorContextCommandsPreserveSelectionAndRouteToOwner() {
        QTemporaryDir owned;MainWindow window(owned.path());window.show();
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        pane->editor->setPlainText("SELECT 1;\nSELECT 2;");auto cursor=pane->editor->textCursor();cursor.setPosition(10);cursor.movePosition(QTextCursor::End,QTextCursor::KeepAnchor);pane->editor->setTextCursor(cursor);
        const auto selection=pane->editor->textCursor().selectedText();
        QSignalSpy executed(&window,&MainWindow::queryFinished);QString failure;int seen=0;
        for(const auto& command:{QString("editorConvertSql"),QString("editorOptimizeSql")}) {
            QTimer poll;poll.setInterval(10);bool routed=false,selected=false;
            connect(&poll,&QTimer::timeout,[&]{
                if(auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget())){
                    auto* action=menu->findChild<QAction*>(command);if(!action){failure="missing editor command";menu->close();return;}
                    if(!selected){selected=true;action->trigger();menu->close();}
                }
                if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())){
                    const auto expected=command=="editorConvertSql"?QString("sqlConversionDialog"):QString("sqlOptimizationDialog");
                    if(dialog->objectName()!=expected){failure="wrong editor command target";dialog->reject();return;}
                    auto* source=dialog->findChild<SqlEditor*>(command=="editorConvertSql"?"conversionSource":"optimizationSource");
                    routed=source && source->toPlainText()==selection;dialog->reject();++seen;
                }
            });
            QTimer deadline;deadline.setSingleShot(true);connect(&deadline,&QTimer::timeout,[&]{failure="context command timed out";if(auto* w=QApplication::activePopupWidget())w->close();if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()))d->reject();});deadline.start(5000);poll.start();
            QContextMenuEvent event(QContextMenuEvent::Keyboard,QPoint(20,20),pane->editor->mapToGlobal(QPoint(20,20)));QCoreApplication::sendEvent(pane->editor,&event);
            QTRY_VERIFY_WITH_TIMEOUT(routed || !failure.isEmpty(),5000);poll.stop();deadline.stop();QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(selected && routed);QCOMPARE(pane->editor->textCursor().selectedText(),selection);
        }
        QCOMPARE(seen,2);QCOMPARE(executed.count(),0);
    }
    void sourceSqlToolsUseTheirOwnSelectionWhileAnotherTabIsActive() {
        QTemporaryDir directory;
        navidog::ensureDemoDatabase(directory.path());
        QJsonObject node{{"kind", "view"}, {"object", "owned_view"}, {"schema", "main"}};
        QJsonObject source{{"kind", "source"}, {"profileId", "harbor-demo"}, {"node", node},
                           {"sql", "CREATE VIEW owned_view AS\nSELECT 271 AS value\nFROM customers;"}};
        QJsonObject query{{"kind", "query"}, {"profileId", "harbor-demo"}, {"sql", "SELECT 42;"}};
        navidog::LocalStore(navidog::localStorePath(directory.path())).putDocument(
            "workspace-session", QJsonDocument(QJsonObject{{"queries", QJsonArray{query, source}},
                                                            {"active", 0}}).toJson());
        MainWindow window(directory.path());
        window.show();
        auto *pane = window.findChild<SourcePane *>();
        auto *tabs = window.findChild<QTabWidget *>("queryTabs");
        QVERIFY(pane && tabs);
        QVERIFY(qobject_cast<QueryPane *>(tabs->currentWidget()));
        auto cursor = pane->editor->textCursor();
        cursor.setPosition(QString("CREATE VIEW owned_view AS\n").size());
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        pane->editor->setTextCursor(cursor);
        const QString selected = "SELECT 271 AS value\nFROM customers;";
        QCOMPARE(pane->editor->selectedSqlOrAll(), selected);

        QSignalSpy executed(&window, &MainWindow::queryFinished);
        for (const auto convert : {true, false}) {
            QString dialogName, input;
            QTimer closeDialog;
            connect(&closeDialog, &QTimer::timeout, &window, [&] {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                    dialogName = dialog->objectName();
                    if (auto *editor = dialog->findChild<SqlEditor *>(
                            convert ? "conversionSource" : "optimizationSource"))
                        input = editor->toPlainText();
                    dialog->reject();
                }
            });
            closeDialog.start(10);
            QVERIFY(QMetaObject::invokeMethod(pane->editor,
                convert ? "conversionRequested" : "optimizationRequested", Qt::DirectConnection));
            QCOMPARE(dialogName, QString(convert ? "sqlConversionDialog" : "sqlOptimizationDialog"));
            QCOMPARE(input, selected);
        }
        QCOMPARE(executed.count(), 0);
        QVERIFY(qobject_cast<QueryPane *>(tabs->currentWidget()));
    }
    void contextualCompletionInsertsColumnsAndTemplates() {
        using namespace navidog;
        SqlEditor editor;editor.setDiagnosticContext(DatabaseKind::PostgreSQL,"owned","public");
        auto metadata=std::make_shared<MetadataSnapshot>();metadata->driver=DatabaseKind::PostgreSQL;metadata->database="owned";metadata->schema="public";metadata->verified=true;metadata->refreshedMs=QDateTime::currentMSecsSinceEpoch();metadata->tablesComplete=metadata->columnsComplete=true;metadata->searchPath={"public"};
        MetadataObject object;object.kind="table";object.database="owned";object.schema="public";object.name="owned_users";object.columns={{"name","text",{},0,true,false,false},{"id","integer",{},1,true,false,false}};metadata->objects.push_back(object);editor.setMetadataSnapshot(metadata);
        editor.resize(800,500);editor.show();editor.activateWindow();editor.setFocus();editor.setPlainText("SELECT u.na FROM owned_users u;");auto cursor=editor.textCursor();cursor.setPosition(11);editor.setTextCursor(cursor);
        auto* popup=editor.findChild<QTreeView*>("sqlCompletionPopup");QVERIFY(popup);QTest::keyClick(&editor,Qt::Key_Space,Qt::ControlModifier);QTRY_VERIFY(popup->isVisible());
        auto index=popup->model()->index(0,0);QCOMPARE(index.data().toString(),QString("name"));QTest::keyClick(&editor,Qt::Key_Tab);QCOMPARE(editor.toPlainText(),QString("SELECT u.name FROM owned_users u;"));editor.undo();QCOMPARE(editor.toPlainText(),QString("SELECT u.na FROM owned_users u;"));
        editor.setPlainText("CREATE TA");cursor=editor.textCursor();cursor.movePosition(QTextCursor::End);editor.setTextCursor(cursor);editor.activateWindow();editor.setFocus();QTest::keyClick(&editor,Qt::Key_Space,Qt::ControlModifier);QTRY_VERIFY2(popup->isVisible(),qPrintable(QString("focus=%1 rows=%2 status=%3").arg(editor.hasFocus()).arg(popup->model()->rowCount()).arg(editor.property("sqlCompletionStatus").toString())));
        QModelIndex selected;for(int i=0;i<popup->model()->rowCount();++i){auto item=popup->model()->index(i,0);if(item.data(Qt::UserRole).toString().startsWith("CREATE TABLE"))selected=item;}QVERIFY(selected.isValid());popup->setCurrentIndex(selected);QTest::keyClick(&editor,Qt::Key_Return);QVERIFY(editor.toPlainText().startsWith("CREATE TABLE object_name"));QCOMPARE(editor.textCursor().selectedText(),QString("object_name"));
        editor.setPlainText("SELECT u. FROM owned_users u;");cursor=editor.textCursor();cursor.setPosition(9);editor.setTextCursor(cursor);editor.activateWindow();editor.setFocus();QInputMethodEvent composition;composition.setCommitString("n");QCoreApplication::sendEvent(&editor,&composition);QTRY_VERIFY(popup->isVisible());QTest::keyClick(&editor,Qt::Key_Tab);QCOMPARE(editor.toPlainText(),QString("SELECT u.name FROM owned_users u;"));
        const auto completeColumn = [&](const QString &source, const QString &tail, const QString &expected) {
            editor.setPlainText(source + tail);
            auto at = editor.textCursor();
            at.setPosition(source.size());
            editor.setTextCursor(at);
            editor.activateWindow();
            editor.setFocus();
            QTest::keyClick(&editor,Qt::Key_Space,Qt::ControlModifier);
            QTRY_VERIFY(popup->isVisible());
            QCOMPARE(popup->model()->rowCount(),1);
            QCOMPARE(popup->model()->index(0,0).data().toString(),QString("name"));
            QTest::keyClick(&editor,Qt::Key_Tab);
            QCOMPARE(editor.toPlainText(),expected);
        };
        completeColumn("INSERT INTO owned_users(id, na",") VALUES (1, 'n');",
                       "INSERT INTO owned_users(id, name) VALUES (1, 'n');");
        completeColumn("SELECT * FROM owned_users u JOIN owned_users v USING (id, na",");",
                       "SELECT * FROM owned_users u JOIN owned_users v USING (id, name);");
        auto typePunctuation = [&](const QString &text, const char *punctuation) {
            editor.setPlainText(text);
            auto at = editor.textCursor();
            at.movePosition(QTextCursor::End);
            editor.setTextCursor(at);
            editor.setFocus();
            QTest::keyClicks(&editor,punctuation);
        };
        typePunctuation("INSERT INTO owned_users", "(");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->model()->rowCount(),2);
        // With zero document margin the caret can reach the viewport edge
        // when the editor scrolls horizontally to the newly inserted comma.
        editor.resize(400, 500);
        typePunctuation("SELECT * FROM owned_users u JOIN owned_users v USING (id", ",");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->model()->rowCount(),1);
        QCOMPARE(popup->model()->index(0,0).data().toString(),QString("name"));
        editor.resize(800, 500);
        QSignalSpy punctuationReady(editor.findChild<SqlCompletionController*>(),&SqlCompletionController::ready);
        typePunctuation("CREATE TABLE own_ref(id int REFERENCES owned_users", "(");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->model()->rowCount(),2);
        typePunctuation("SELECT COUNT(*) OVER ", "(");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->model()->rowCount(),2);
        QStringList windowChoices;
        for (int row=0;row<popup->model()->rowCount();++row)
            windowChoices.push_back(popup->model()->index(row,0).data().toString());
        QVERIFY(windowChoices.contains("PARTITION BY") && windowChoices.contains("ORDER BY"));
        punctuationReady.clear();
        typePunctuation("SELECT COALESCE", "(");
        QTRY_VERIFY(!punctuationReady.isEmpty());
        QVERIFY(!popup->isVisible());
        editor.setDiagnosticContext(DatabaseKind::MySQL,"different",{});editor.setPlainText("SELECT * FROM own");cursor=editor.textCursor();cursor.movePosition(QTextCursor::End);editor.setTextCursor(cursor);QTest::keyClick(&editor,Qt::Key_Space,Qt::ControlModifier);QTest::qWait(300);QVERIFY(!popup->isVisible());
    }
    void conversionDoesNotRequireTablesInAFreshDirectorySnapshot() {
        using namespace navidog;using namespace navidog::sql::compiler;
        auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::PostgreSQL;snapshot->database="owned";snapshot->schema="public";snapshot->searchPath={"public"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;
        NameResolutionContext names;names.dialect=Dialect::PostgreSQL;names.database="owned";names.schema="public";
        const QString text="SELECT id FROM future_orders;";auto cancelled=std::make_shared<std::atomic_bool>(false);
        auto checked=checkEditorSql(text,DatabaseKind::PostgreSQL,"owned","public",cancelled,snapshot);QVERIFY(std::any_of(checked.diagnostics.begin(),checked.diagnostics.end(),[](const auto& d){return d.metadata;}));
        QTimer poll;poll.setInterval(10);bool started=false,converted=false;QString failure;
        connect(&poll,&QTimer::timeout,[&]{if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget())){
            if(d->objectName()!="sqlConversionDialog")return;auto* run=d->findChild<QToolButton*>("convertSqlNow");auto* output=d->findChild<QPlainTextEdit*>("conversionOutput");
            if(!started){started=true;run->click();return;}
            if(!run->isEnabled())return;
            converted=output->toPlainText().contains("future_orders");if(!converted)failure=d->findChild<QLabel*>("conversionStatus")->text();d->reject();
        }});
        QTimer deadline;deadline.setSingleShot(true);connect(&deadline,&QTimer::timeout,[&]{failure="conversion timed out";if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()))d->reject();});deadline.start(5000);poll.start();showSqlConversion(nullptr,Dialect::PostgreSQL,text,names,snapshot);poll.stop();deadline.stop();QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(converted);
    }
    void sqlConversionDialogIsAtomicAndInvalidatesEditedResults() {
        using namespace navidog::sql::compiler;
        const QString original="CREATE FUNCTION f(p INTEGER) RETURNS INTEGER DETERMINISTIC SQL SECURITY INVOKER BEGIN DECLARE v INTEGER DEFAULT p+1; RETURN v; END;";
        QTimer poll;poll.setInterval(10);int stage=0;bool success=false,scope=false,cleared=false,failed=false,explained=false,highlighted=false;QPointer<QDialog> active;
        connect(&poll,&QTimer::timeout,[&]{
            auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!d||d->objectName()!="sqlConversionDialog")return;active=d;
            auto* input=d->findChild<SqlEditor*>("conversionSource");auto* output=d->findChild<QPlainTextEdit*>("conversionOutput");auto* model=d->findChild<QPlainTextEdit*>("conversionModel");auto* run=d->findChild<QToolButton*>("convertSqlNow");auto* copy=d->findChild<QToolButton*>("copyConvertedSql");auto* errors=d->findChild<QListWidget*>("conversionDiagnostics");
            auto* changes=d->findChild<QListWidget*>("conversionChanges");
            if(stage==0){stage=1;run->click();}
            else if(stage==1 && !output->toPlainText().isEmpty()) {success=input->toPlainText()==original;scope=model->toPlainText().contains("boundSymbols");explained=changes->count()>0 && changes->item(0)->text().contains("作用域");highlighted=output->isReadOnly() && !output->document()->firstBlock().layout()->formats().isEmpty();copy->click();success=success && QApplication::clipboard()->text()==output->toPlainText();input->setPlainText("SELECT NOW();");cleared=output->toPlainText().isEmpty() && model->toPlainText().isEmpty() && !copy->isEnabled() && changes->count()==0;stage=2;run->click();}
            else if(stage==2 && run->isEnabled() && errors->count()>0){failed=output->toPlainText().isEmpty() && !copy->isEnabled();stage=3;d->reject();}
        });
        poll.start();QTimer watchdog;watchdog.setSingleShot(true);connect(&watchdog,&QTimer::timeout,[&]{if(active)active->reject();else if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()))d->reject();});watchdog.start(5000);
        showSqlConversion(nullptr,Dialect::MySQL,original);poll.stop();QCOMPARE(stage,3);QVERIFY(success);QVERIFY(scope);QVERIFY(explained);QVERIFY(highlighted);QVERIFY(cleared);QVERIFY(failed);
    }
    void queryToolbarHitTargetsFollowTheVisibleTab() {
        QTemporaryDir owned;MainWindow window(owned.path());window.resize(1728,970);window.show();
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        window.findChild<QToolButton*>("newQuery")->click();
        QTest::qWait(50);
        auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        QVERIFY(!pane->findChild<QToolButton*>("formatSettings"));
        for(auto* widget:QList<QWidget*>{pane->run,pane->format,pane->connection,pane->databaseContext}) {
            const auto center=widget->mapTo(&window,widget->rect().center());
            auto* target=window.childAt(center);
            QVERIFY2(target==widget || widget->isAncestorOf(target),qPrintable(QString("%1 hit %2 (%3)").arg(widget->objectName(),target?target->objectName():"null",target?target->metaObject()->className():"null")));
        }
    }
    void sourceDesignerOffersLocalFormatting() {
        navidog::ConnectionProfile profile;profile.driver=navidog::DatabaseKind::PostgreSQL;profile.id="format-test";
        navidog::CatalogNode node;node.kind="function";node.object="f";
        SourcePane pane(profile,node,false);QSignalSpy format(&pane,&SourcePane::formatRequested);
        auto* button=pane.findChild<QToolButton*>("formatSource");QVERIFY(button);button->click();QCOMPARE(format.count(),1);
        QVERIFY(!pane.findChild<QToolButton*>("formatSourceSettings"));
    }
    void restoredDefinitionKeepsTextClearOfLineNumbers() {
        using namespace navidog;
        ConnectionProfile profile;
        profile.driver = DatabaseKind::MySQL;
        CatalogNode node;
        node.kind = "function";
        node.object = "f_get_quantity";
        SourcePane pane(profile, node, false);
        auto* editor = pane.editor;
        auto* gutter = editor->findChild<QWidget*>("sqlLineNumbers");
        QVERIFY(gutter);
        editor->applyPreferences(15, 2, false);
        pane.resize(900, 690);

        auto draft = pane.sessionState();
        const QString sql =
            "DELIMITER $$\nCREATE FUNCTION f_get_quantity(_id INT) RETURNS INT\nBEGIN\n" +
            QString("  -- restored draft\n").repeated(27) + "  RETURN _id;\nEND$$";
        draft["sql"] = sql;
        pane.restoreState(draft);
        pane.show();
        QTRY_VERIFY(editor->viewport()->isVisible());
        QCOMPARE(editor->toPlainText(), sql);
        QCOMPARE(gutter->geometry().right() + 1, editor->viewport()->geometry().left());
        QCOMPARE(gutter->width(), editor->gutterWidth());
        QCOMPARE(editor->document()->documentMargin(), qreal(0));
        const auto caret = editor->cursorRect(QTextCursor(editor->document()));
        QCOMPARE(caret.topLeft(), QPoint(0, 0));

        draft["sql"] = "SELECT 1;";
        pane.restoreState(draft);
        QCOMPARE(gutter->geometry().right() + 1, editor->viewport()->geometry().left());
        QCOMPARE(gutter->width(), editor->gutterWidth());
        editor->applyPreferences(20, 2, true);
        QCOMPARE(gutter->geometry().right() + 1, editor->viewport()->geometry().left());
    }
    void mysqlFunctionDesignerCompletesParameterInDelimitedDraft() {
        navidog::ConnectionProfile profile;
        profile.driver = navidog::DatabaseKind::MySQL;
        navidog::CatalogNode node;
        node.kind = "function";
        node.object = "f_get_quantity";
        SourcePane pane(profile, node, false);
        pane.resize(900, 660);
        pane.show();
        pane.activateWindow();
        auto* editor = pane.editor;
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        const QString prefix =
            "DELIMITER $$\nCREATE FUNCTION f_get_quantity(_id INT) RETURNS INT READS SQL DATA\n"
            "BEGIN\n  IF _id > 0 THEN\n    ";
        const QString suffix =
            "\n    RETURN (SELECT SUM(id) FROM t_activity WHERE id = _id);\n"
            "  ELSE\n    RETURN (SELECT SUM(id) FROM t_activity);\n  END IF;\nEND$$";
        editor->setPlainText(prefix + suffix);
        auto cursor = editor->textCursor();
        cursor.setPosition(prefix.size());
        editor->setTextCursor(cursor);
        QTest::keyClicks(editor, "_i");
        auto* popup = editor->findChild<QTreeView*>("sqlCompletionPopup");
        QVERIFY(popup);
        QTRY_VERIFY2(popup->isVisible(), qPrintable(editor->property("sqlCompletionStatus").toString()));
        QCOMPARE(popup->currentIndex().data().toString(), QString("_id"));
        QTest::keyClick(editor, Qt::Key_Tab);
        QCOMPARE(editor->toPlainText(), prefix + "SET _id = " + suffix);

        const QString branch = prefix + "RETURN 1;\n  ";
        const QString tail = "\nEND$$";
        editor->setPlainText(branch + tail);
        cursor = editor->textCursor();
        cursor.setPosition(branch.size());
        editor->setTextCursor(cursor);
        QTest::keyClicks(editor, "end");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(), QString("END IF;"));
        QTest::keyClick(editor, Qt::Key_Tab);
        QCOMPARE(editor->toPlainText(), branch + "END IF;" + tail);

        editor->setPlainText(branch + tail);
        cursor = editor->textCursor();
        cursor.setPosition(branch.size());
        editor->setTextCursor(cursor);
        QTest::keyClicks(editor, "end i");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(), QString("END IF;"));
        QTest::keyClick(editor, Qt::Key_Tab);
        QCOMPARE(editor->toPlainText(), branch + "END IF;" + tail);

        const QString nextStatement = branch + "END IF;\n  ";
        editor->setPlainText(nextStatement + tail);
        cursor = editor->textCursor();
        cursor.setPosition(nextStatement.size());
        editor->setTextCursor(cursor);
        QTest::keyClicks(editor, "if");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(), QString("IF"));
        QTest::keyClick(editor, Qt::Key_Tab);
        QCOMPARE(editor->toPlainText(),
                 nextStatement + "IF condition THEN\n    \n  END IF;" + tail);
        QCOMPARE(editor->textCursor().selectedText(), QString("condition"));
    }
    void sqlEditorCompletesFuzzyNamesWithOriginalSpelling() {
        using namespace navidog;
        auto snapshot = std::make_shared<MetadataSnapshot>();
        snapshot->driver = DatabaseKind::MySQL;
        snapshot->database = snapshot->schema = "owned";
        snapshot->searchPath = {"owned"};
        snapshot->verified = true;
        snapshot->refreshedMs = QDateTime::currentMSecsSinceEpoch();
        snapshot->tablesComplete = snapshot->columnsComplete = true;
        MetadataObject object;
        object.key = object.name = "order_items";
        object.kind = "table";
        object.database = object.schema = "owned";
        object.columns = {{"user_id", "int"}};
        snapshot->objects.append(object);
        SqlEditor editor;
        editor.resize(900, 440);
        editor.setDiagnosticContext(DatabaseKind::MySQL, "owned", "owned");
        editor.setMetadataSnapshot(snapshot);
        editor.show();
        editor.activateWindow();
        editor.setFocus();
        QTRY_VERIFY(editor.hasFocus());
        auto* popup = editor.findChild<QTreeView*>("sqlCompletionPopup");
        QVERIFY(popup);
        editor.setPlainText("SELECT * FROM ");
        editor.moveCursor(QTextCursor::End);
        QTest::keyClicks(&editor, "orderitems");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(), QString("order_items"));
        QTest::keyClick(&editor, Qt::Key_Tab);
        QCOMPARE(editor.toPlainText(), QString("SELECT * FROM order_items"));

        editor.setPlainText("SELECT o. FROM order_items o");
        auto cursor = editor.textCursor();
        cursor.setPosition(QString("SELECT o.").size());
        editor.setTextCursor(cursor);
        QTest::keyClicks(&editor, "usrid");
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(popup->currentIndex().data().toString(), QString("user_id"));
        QTest::keyClick(&editor, Qt::Key_Tab);
        QCOMPARE(editor.toPlainText(), QString("SELECT o.user_id FROM order_items o"));
    }
    void procedureDesignerCompletesVariablesAndCachedRelations() {
        using namespace navidog;
        QTemporaryDir directory;
        const auto path = localStorePath(directory.path());
        ConnectionProfile profile;
        profile.id = "owned-routine-pg";
        profile.name = "Offline routine fixture";
        profile.driver = DatabaseKind::PostgreSQL;
        profile.database = "owned";
        ProfileStore(path).save({profile});

        MetadataSnapshot snapshot;
        snapshot.database = profile.database;
        snapshot.schema = "public";
        snapshot.driver = profile.driver;
        snapshot.profileId = profile.id;
        snapshot.identity = metadataIdentity(profile);
        snapshot.key = metadataKey(profile, snapshot.database, snapshot.schema);
        snapshot.refreshedMs = QDateTime::currentMSecsSinceEpoch();
        MetadataObject object;
        object.name = "owned_relation";
        object.key = object.name;
        object.kind = "table";
        object.database = snapshot.database;
        object.schema = snapshot.schema;
        snapshot.objects.append(object);
        { MetadataStore cache(path); cache.replace(snapshot); }

        const QJsonObject node{{"kind", "procedure"}, {"object", "p"},
                               {"database", "owned"}, {"schema", "public"}};
        const QJsonObject tab{{"kind", "source"}, {"profileId", profile.id},
                             {"database", profile.database}, {"node", node},
                             {"baselineNode", node}, {"exists", false}};
        LocalStore(path).putDocument("workspace-session",
            QJsonDocument(QJsonObject{{"version", 3}, {"queries", QJsonArray{tab}},
                                      {"active", 0}}).toJson());

        MainWindow window(directory.path());
        auto* service = window.findChild<QueryService*>();
        QSignalSpy metadataReady(service, &QueryService::metadataCompleted);
        window.show();
        window.activateWindow();
        QTRY_VERIFY(!metadataReady.isEmpty());
        auto* pane = window.findChild<SourcePane*>("sourceDesigner");
        QVERIFY(pane);
        auto* editor = pane->editor;
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        auto* popup = editor->findChild<QTreeView*>("sqlCompletionPopup");
        QVERIFY(popup);
        QSignalSpy ready(editor->findChild<SqlCompletionController*>(), &SqlCompletionController::ready);
        const auto prefix = QString("CREATE PROCEDURE p(p_value INT) LANGUAGE plpgsql AS $$\n"
                                    "DECLARE local_value INT := p_value;\nBEGIN\n  ");
        editor->setPlainText(prefix + "local_value := ");
        editor->moveCursor(QTextCursor::End);
        QTest::keyClicks(editor, "loc");
        QCOMPARE(editor->toPlainText(), prefix + "local_value := loc");
        QCOMPARE(editor->textCursor().position(), editor->toPlainText().size());
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 3000);
        QTRY_VERIFY2(popup->isVisible(), qPrintable(editor->property("sqlCompletionStatus").toString()));
        const auto completion = qvariant_cast<EditorCompletionResult>(ready.back().at(0));
        QStringList candidates;
        for (const auto &item : completion.items) candidates << item.label;
        QVERIFY2(candidates.contains("local_value"), qPrintable(candidates.join(", ") + " · " + completion.status));
        QCOMPARE(popup->currentIndex().data().toString(), QString("local_value"));
        QTest::keyClick(editor, Qt::Key_Tab);
        QVERIFY(editor->toPlainText().endsWith("local_value := local_value"));

        QTRY_VERIFY(std::any_of(metadataReady.begin(), metadataReady.end(), [&](const auto &entry) {
            const auto metadata = qvariant_cast<MetadataResult>(entry.at(1));
            return metadata.snapshot && metadata.snapshot->key == snapshot.key;
        }));
        editor->setPlainText(prefix + "SELECT * FROM ");
        editor->moveCursor(QTextCursor::End);
        QTest::keyClicks(editor, "owned_rel");
        QTRY_VERIFY2(popup->isVisible(), qPrintable(editor->property("sqlCompletionStatus").toString()));
        QCOMPARE(popup->currentIndex().data().toString(), QString("owned_relation"));
        QVERIFY(ready.count() >= 2);
        const auto cte = prefix + "RAISE NOTICE 'editing';\n"
            "WITH staged AS (SELECT 1 AS staged_id) SELECT s. INTO local_value FROM staged s;\nEND; $$;";
        editor->setPlainText(cte);
        auto caret = editor->textCursor();
        caret.setPosition(cte.indexOf("s. INTO") + 2);
        editor->setTextCursor(caret);
        QTest::keyClicks(editor, "sta");
        QTRY_VERIFY2(popup->isVisible(), qPrintable(editor->property("sqlCompletionStatus").toString()));
        QCOMPARE(popup->currentIndex().data().toString(), QString("staged_id"));
        QTest::keyClick(editor, Qt::Key_Tab);
        QVERIFY(editor->toPlainText().contains("s.staged_id INTO"));
        const auto bad = prefix + "missing := p_value;\nEND; $$;";
        editor->setPlainText(bad);
        QTRY_VERIFY(!editor->diagnostics().isEmpty());
        const auto error = editor->diagnostics().front();
        QVERIFY(error.error && !error.metadata);
        QCOMPARE(bad.mid(error.begin, error.end - error.begin), QString("missing"));
        QCOMPARE(service->sessionCount(), 0);
    }
    void sqlPreviewFormatsAndCopiesWithoutLosingNewerSource() {
        using namespace navidog::sql::compiler;
        QTemporaryDir directory;
        const auto path = navidog::localStorePath(directory.path());
        ApplicationSettings settings;
        settings.formatting.keywordCase = KeywordCase::Lower;
        saveApplicationSettings(path, settings);
        QWidget owner;
        setSqlPreviewFormattingPath(owner, path);
        auto *preview = new SqlReadOnlyView(&owner);
        preview->setDialect(Dialect::SQLite);
        auto *panel = preview->withActions();
        auto *format = panel->findChild<QToolButton *>("formatSqlPreview");
        auto *copy = panel->findChild<QToolButton *>("copySqlPreview");
        QVERIFY(format && copy);
        QVERIFY(preview->isReadOnly());
        QCOMPARE(preview->fontMetrics().horizontalAdvance('i'),
                 preview->fontMetrics().horizontalAdvance('W'));
        QVERIFY(!format->isEnabled() && !copy->isEnabled());

        const QString original = "SELECT a,b FROM owned_items WHERE a=1";
        preview->setSourceSql(original);
        QVERIFY(format->isEnabled() && copy->isEnabled());
        format->click();
        QTRY_VERIFY_WITH_TIMEOUT(format->isEnabled(), 3000);
        const auto formatted = preview->toPlainText();
        QVERIFY(formatted.startsWith("select a, b"));
        QVERIFY(formatted.contains("\nfrom owned_items"));
        QVERIFY(formatted.contains("a = 1"));
        preview->setSourceSql(original);
        QCOMPARE(preview->toPlainText(), formatted);
        copy->click();
        QCOMPARE(QApplication::clipboard()->text(), formatted);

        format->click();
        const QString replacement = "SELECT 421";
        preview->setSourceSql(replacement);
        QTRY_VERIFY_WITH_TIMEOUT(format->isEnabled(), 3000);
        QCOMPARE(preview->toPlainText(), replacement);

        QueryPane definition({}, navidog::WorkspaceTabKind::Definition);
        definition.setConnectionCapabilities(navidog::DatabaseKind::SQLite, true);
        navidog::QueryOutcome outcome;
        outcome.result.columns = {"sql"};
        outcome.result.rows = {{original}};
        outcome.results = {outcome.result};
        definition.applyOutcome(outcome, {}, nullptr);
        auto *definitionPage = definition.resultTabs->currentWidget();
        QVERIFY(definitionPage->findChild<SqlReadOnlyView *>("returnedDefinitionSql"));
        QVERIFY(definitionPage->findChild<QToolButton *>("formatSqlPreview")->isEnabled());
        QVERIFY(definitionPage->findChild<QToolButton *>("copySqlPreview")->isEnabled());
    }
    void formatterSelectionUndoAndStaleResult() {
        QTemporaryDir dir;SqlEditor editor;
        const QString original="-- preserve prefix\nselect a,b from t where a=1 and b=2;\n-- preserve suffix";
        editor.setPlainText(original);auto cursor=editor.textCursor();const auto start=original.indexOf("select");const auto finish=original.indexOf(";",start)+1;cursor.setPosition(start);cursor.setPosition(finish,QTextCursor::KeepAnchor);editor.setTextCursor(cursor);
        QString message;formatEditorAsync(&editor,navidog::sql::compiler::Dialect::SQLite,dir.filePath("style"),{},[]{return true;},[&](const QString& s){message=s;});
        QTRY_VERIFY_WITH_TIMEOUT(!editor.property("sqlFormatting").toBool(),5000);
        QVERIFY(editor.toPlainText().startsWith("-- preserve prefix\nSELECT"));QVERIFY(editor.toPlainText().endsWith("\n-- preserve suffix"));QVERIFY(editor.toPlainText().contains("\nFROM t"));editor.undo();QCOMPARE(editor.toPlainText(),original);
        cursor=editor.textCursor();cursor.clearSelection();editor.setTextCursor(cursor);
        formatEditorAsync(&editor,navidog::sql::compiler::Dialect::SQLite,dir.filePath("style"),{},[]{return true;},[&](const QString& s){message=s;});
        editor.insertPlainText("changed");const auto changed=editor.toPlainText();
        QTRY_VERIFY_WITH_TIMEOUT(!editor.property("sqlFormatting").toBool(),5000);QCOMPARE(editor.toPlainText(),changed);QVERIFY(message.contains("忽略"));
    }
    void unifiedSettingsCategoriesTabsAndAtomicSave() {
        QTemporaryDir dir;const auto path=dir.filePath("settings.json");QFile legacy(dir.filePath("sql-format.yaml"));QVERIFY(legacy.open(QIODevice::WriteOnly));legacy.write("IndentWidth: 3\n");legacy.close();QCOMPARE(loadApplicationSettings(path).formatting.indentWidth,3u);
        bool saw=false;
        QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto* categories=dialog->findChild<QListWidget*>("settingsCategories");auto* pages=dialog->findChild<QStackedWidget*>("settingsPages");auto* tabs=dialog->findChild<QTabWidget*>("formatCategoryTabs");auto* spin=dialog->findChild<QSpinBox*>("formatIndentWidth");if(categories && pages && tabs && spin){saw=dialog->windowTitle()=="设置" && categories->count()==7 && pages->count()==7 && tabs->count()==4 && categories->currentRow()==3;categories->setCurrentRow(1);saw=saw && pages->currentIndex()==1;dialog->findChild<QSpinBox*>("settingsEditorFontSize")->setValue(16);dialog->findChild<QSpinBox*>("settingsEditorIndentWidth")->setValue(2);dialog->findChild<QCheckBox*>("settingsEditorWordWrap")->setChecked(true);spin->setValue(4);dialog->findChild<QCheckBox*>("settingsEditorCompletion")->setChecked(false);dialog->findChild<QSpinBox*>("settingsCompletionMinPrefix")->setValue(3);dialog->findChild<QSpinBox*>("settingsOptimizerAssumedRows")->setValue(33333);dialog->findChild<QSpinBox*>("settingsOptimizerTimeMs")->setValue(1500);dialog->findChild<QCheckBox*>("settingsOptimizerHep")->setChecked(false);}dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});
        QVERIFY(editApplicationSettings(nullptr,path,navidog::sql::compiler::Dialect::PostgreSQL,"select a,b from t;",3));QVERIFY(saw);auto saved=loadApplicationSettings(path);QCOMPARE(saved.formatting.indentWidth,4u);QCOMPARE(saved.editorFontSize,16);QCOMPARE(saved.editorIndentWidth,2);QVERIFY(saved.editorWordWrap);QVERIFY(!saved.editorCompletion);QCOMPARE(saved.completionMinPrefix,3);QCOMPARE(saved.optimizerAssumedRows,33333);QVERIFY(!saved.optimizerHep);QVERIFY(!optimizationOptions(saved).heuristic.enabled);QCOMPARE(optimizationOptions(saved).maxTimeMs,std::size_t(1500));
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();
        QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;dialog->findChild<QSpinBox*>("formatIndentWidth")->setValue(7);dialog->reject();});
        QVERIFY(!editApplicationSettings(nullptr,path,navidog::sql::compiler::Dialect::PostgreSQL,"select 1;"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);file.close();
        MainWindow window(dir.path());auto* pane=dynamic_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());QVERIFY(pane);QCOMPARE(pane->editor->font().pointSize(),16);QCOMPARE(pane->editor->lineWrapMode(),QPlainTextEdit::WidgetWidth);pane->editor->setPlainText("select 1");pane->editor->indentLines();QVERIFY(pane->editor->toPlainText().startsWith("  select"));
        QVERIFY(window.findChild<QAction*>("applicationSettingsAction"));QVERIFY(!window.findChild<QAction*>("sqlFormatSettingsAction"));
    }
    void exportPreferencesApplyToOpenQueryAndFileFormat() {
        using namespace navidog::ui;
        const QDateTime time(QDate(2026, 11, 27), QTime(21, 2, 3));
        QCOMPARE(exportStem({"QA:/East", {}, "public"}, "查询:结果", time),
                 QString("QA__East-public-查询_结果-20261127-210203"));
        QCOMPARE(exportStem({}, "查询脚本", time), QString("Navidog-查询脚本-20261127-210203"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("exports"));
        const auto folder = directory.filePath("exports");
        MainWindow window(directory.path());
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("queryTabs");
        QVERIFY(tabs);
        auto *pane = dynamic_cast<QueryPane *>(tabs->currentWidget());
        QVERIFY(pane);
        QCOMPARE(exportDirectory(pane), systemDownloadDirectory());

        auto *settings = window.findChild<QAction *>("applicationSettingsAction");
        QVERIFY(settings);
        auto setPreference = [&](bool reset) {
            QTimer::singleShot(0, &window, [&window, folder, reset] {
                auto *dialog = window.findChild<QDialog *>("applicationSettings");
                if (!dialog) return;
                auto *edit = dialog->findChild<QLineEdit *>("settingsExportDirectory");
                auto *resetButton = dialog->findChild<QPushButton *>("settingsResetExportDirectory");
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                if (!edit || !resetButton || !buttons) { dialog->reject(); return; }
                if (reset) resetButton->click();
                else edit->setText(folder);
                buttons->button(QDialogButtonBox::Save)->click();
            });
            settings->trigger();
        };
        setPreference(false);
        QCOMPARE(exportDirectory(pane), folder);
        const auto storePath = navidog::localStorePath(directory.path());
        QCOMPARE(loadApplicationSettings(storePath).exportDirectory, folder);

        pane->editor->setPlainText("SELECT 7 AS number");
        QSignalSpy complete(&window, &MainWindow::queryFinished);
        window.runCurrent();
        QTRY_COMPARE_WITH_TIMEOUT(complete.size(), 1, 4000);
        QString suggested, initialDirectory;
        QTimer::singleShot(0, &window, [&] {
            auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!picker) picker = window.findChild<QFileDialog *>();
            if (!picker) return;
            initialDirectory = picker->directory().absolutePath();
            suggested = QFileInfo(picker->selectedFiles().value(0)).fileName();
            picker->selectNameFilter("JSON (*.json)");
            QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        });
        pane->exportButton->click();
        QCOMPARE(initialDirectory, folder);
        QVERIFY(QRegularExpression(R"(^Navidog 示例库-harbor-demo-查询结果-\d{8}-\d{6}\.csv$)").match(suggested).hasMatch());
        auto expected = suggested;
        expected.chop(4);
        expected += ".json";
        QFile result(QDir(folder).filePath(expected));
        QTRY_VERIFY_WITH_TIMEOUT(result.exists(), 4000);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QVERIFY(result.readAll().contains("\"number\""));

        setPreference(true);
        QVERIFY(loadApplicationSettings(storePath).exportDirectory.isEmpty());
        QCOMPARE(exportDirectory(pane), systemDownloadDirectory());
    }
    void mysqlWindowsAndDialectErrorsDoNotSuppressOtherStatementCatalogChecks(){
        using namespace navidog;auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::MySQL;snapshot->database=snapshot->schema="owned";snapshot->searchPath={"owned"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;
        MetadataObject table;table.kind="table";table.name="orders";table.schema=table.database="owned";table.key="orders";table.columns={{"id","int",{},1},{"user_id","int",{},2}};snapshot->objects<<table;
        const QString sql="INSERT INTO missing_target(id) SELECT ROW_NUMBER() OVER (ORDER BY id) FROM orders ON CONFLICT(id) DO UPDATE SET id=EXCLUDED.id;\nWITH q AS(SELECT ROW_NUMBER() OVER(PARTITION BY absent ORDER BY id) AS rn FROM orders) SELECT * FROM q JOIN absent_table t ON t.id=q.rn;";
        auto result=checkEditorSql(sql,DatabaseKind::MySQL,"owned","owned",std::make_shared<std::atomic_bool>(false),snapshot);bool dialect=false,tableMissing=false,columnMissing=false;for(const auto& mark:result.diagnostics){if(mark.error && mark.message.contains("ON DUPLICATE"))dialect=true;if(mark.metadata && mark.message.contains("未找到该表"))tableMissing=true;if(mark.metadata && mark.message.contains("absent"))columnMissing=true;QVERIFY(mark.begin>=0 && mark.end<=sql.size());}QVERIFY(dialect);QVERIFY(tableMissing);QVERIFY(columnMissing);
        auto offline=checkEditorSql("SELECT ROW_NUMBER() OVER(ORDER BY id) FROM absent",DatabaseKind::MySQL,"owned","owned",std::make_shared<std::atomic_bool>(false));QVERIFY(offline.diagnostics.isEmpty());QVERIFY(offline.status.contains("表/字段未校验"));
    }

    void mergeChecksBothRelationsUsingCurrentCatalog(){
        using namespace navidog;auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::PostgreSQL;snapshot->database="owned";snapshot->schema="public";snapshot->searchPath={"public"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=true;
        const QString sql="MERGE INTO missing_target t USING missing_source s ON t.id=s.id WHEN MATCHED THEN UPDATE SET n=s.n;";
        auto result=checkEditorSql(sql,DatabaseKind::PostgreSQL,"owned","public",std::make_shared<std::atomic_bool>(false),snapshot);QStringList missing;
        for(const auto &mark:result.diagnostics)if(mark.metadata && mark.message.contains("未找到该表"))missing<<sql.mid(mark.begin,mark.end-mark.begin);
        QVERIFY(missing.contains("missing_target"));QVERIFY(missing.contains("missing_source"));
    }

    void connectionDialogOffersEncryptedRememberingWithoutKeychain(){
        using namespace navidog;ConnectionProfile legacy;legacy.id="owned-ui-password";legacy.name="Owned";legacy.driver=DatabaseKind::MySQL;legacy.credentialSource=CredentialSource::SystemStore;legacy.savePassword=true;legacy.password="owned-runtime-only";QueryService service;ConnectionDialog dialog(legacy,true,{},service);dialog.show();auto* control=dialog.findChild<QCheckBox*>("saveConnectionPassword");QVERIFY(control && control->isVisible() && control->isEnabled());QVERIFY(!control->isChecked());control->setChecked(true);auto profile=dialog.profile();QCOMPARE(profile.password,legacy.password);QCOMPARE(profile.credentialSource,CredentialSource::LocalEncrypted);QVERIFY(profile.savePassword);control->setChecked(false);QVERIFY(!dialog.profile().savePassword);
    }
    void renameDialogDoesNotClaimUnreadableSavedPasswordWasLoaded(){
        navidog::ConnectionProfile saved;saved.id="owned-unloaded";saved.name="Owned";saved.driver=navidog::DatabaseKind::MySQL;saved.credentialSource=navidog::CredentialSource::LocalEncrypted;saved.savePassword=true;
        navidog::QueryService service;ConnectionDialog dialog(saved,true,{},service);auto result=dialog.profile();QVERIFY(result.savePassword);QVERIFY(result.password.isEmpty());QVERIFY(!result.secretsLoaded);
    }
    void sqliteMigrationKeepsLegacyFilesAndWorkspaceDrafts(){
        using namespace navidog;QTemporaryDir owned;ConnectionProfile p;p.id="owned-migration";p.name="Legacy connection";p.database=ensureDemoDatabase(owned.path());p.groupId="legacy-group";ProfileStore legacy(owned.filePath("connections.json"));legacy.saveWorkspace({{p},{{"legacy-group","Legacy group"}}});ApplicationSettings preferences;preferences.editorFontSize=15;saveApplicationSettings(owned.filePath("settings.json"),preferences);
        QJsonObject draft{{"version",3},{"active",0},{"queries",QJsonArray{QJsonObject{{"kind","query"},{"title","Original draft"},{"profileId",p.id},{"sql","SELECT migration_owned_draft"}}}}};QFile session(owned.filePath("session.json"));QVERIFY(session.open(QIODevice::WriteOnly));const auto sessionBytes=QJsonDocument(draft).toJson();session.write(sessionBytes);session.close();QFile oldProfiles(owned.filePath("connections.json"));QVERIFY(oldProfiles.open(QIODevice::ReadOnly));const auto profileBytes=oldProfiles.readAll();oldProfiles.close();
        {MainWindow window(owned.path());auto* pane=dynamic_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());QVERIFY(pane);QCOMPARE(pane->editor->toPlainText(),QString("SELECT migration_owned_draft"));QCOMPARE(pane->editor->font().pointSize(),15);QVERIFY(window.close());}
        LocalStore central(localStorePath(owned.path()));QVERIFY(central.document("connections"));QVERIFY(central.document("application-settings"));QVERIFY(central.document("workspace-session"));QVERIFY(session.open(QIODevice::ReadOnly));QCOMPARE(session.readAll(),sessionBytes);QVERIFY(oldProfiles.open(QIODevice::ReadOnly));QCOMPARE(oldProfiles.readAll(),profileBytes);
        auto loaded=ProfileStore(localStorePath(owned.path())).loadWorkspace();QVERIFY(!loaded.groups.isEmpty());QCOMPARE(loaded.groups.front().name,QString("Legacy group"));
    }
    void queryContextHidesRedundantMySqlSchemaAndDropdownStillOpens(){
        using namespace navidog;QTemporaryDir owned;ConnectionProfile mysql;mysql.id="owned-dropdown";mysql.name="Owned MySQL";mysql.driver=DatabaseKind::MySQL;mysql.database="owned";ProfileStore(localStorePath(owned.path())).save({mysql});MainWindow window(owned.path());window.show();auto* pane=dynamic_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());pane->connection->setCurrentIndex(pane->connection->findData(mysql.id));QVERIFY(pane->schemaContext->isHidden());QVERIFY(!pane->databaseContext->isHidden());QVERIFY(!window.findChild<QToolButton*>("syncMetadata"));QCOMPARE(window.findChild<QueryService*>()->sessionCount(),0);
        for (const auto *database : {"owned_one", "owned_two"}) {
            pane->databaseContext->setEditText(database);
            QSignalSpy changes(pane, &QueryPane::contextChangeRequested);
            QVERIFY(QMetaObject::invokeMethod(pane->databaseContext->lineEdit(), "editingFinished"));
            QCOMPARE(changes.count(), 1);
            QCOMPARE(pane->database, QString(database));
            pane->connection->setCurrentIndex(pane->connection->findData("harbor-demo"));
            QVERIFY(!pane->databaseContext->isEditable());
            pane->connection->setCurrentIndex(pane->connection->findData(mysql.id));
            QVERIFY(pane->databaseContext->isEditable());
        }
        navidog::ui::Select select;navidog::ui::applyTheme(&select);select.addItems({"First","Second"});select.resize(180,32);select.show();QTest::mouseClick(&select,Qt::LeftButton,{},QPoint(select.width()-10,select.height()/2));QTRY_VERIFY(select.view()->isVisible());select.hidePopup();
    }
    void namespaceDropDialogRequiresTheExactName() {
        using namespace navidog;
        NamespaceDropDialog dialog(DatabaseKind::PostgreSQL, NamespaceKind::Schema,
                                   "Owned Schema", "DROP SCHEMA \"Owned Schema\";", nullptr);
        auto *name = dialog.findChild<QLineEdit *>("confirmNamespaceDrop");
        auto *drop = dialog.findChild<QPushButton *>("executeNamespaceDrop");
        QVERIFY(name && drop);
        name->setText("owned schema");
        QVERIFY(!drop->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        name->setText("Owned Schema");
        QVERIFY(drop->isEnabled());
        drop->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void missingJoinedTablesAreMarkedEvenWithAnInvalidAlias(){
        using namespace navidog;auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::MySQL;snapshot->database="owned";snapshot->searchPath={"owned"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=true;
        const QString sql="SELECT *\nFROM abc a\nLEFT JOIN cccc c2\nON a.b=c.df\nWHERE 1=4;";
        auto result=checkEditorSql(sql,DatabaseKind::MySQL,"owned",{},std::make_shared<std::atomic_bool>(false),snapshot);QStringList missing;for(const auto& mark:result.diagnostics)if(mark.metadata && mark.message.contains("未找到该表"))missing<<sql.mid(mark.begin,mark.end-mark.begin);QVERIFY(missing.contains("abc"));QVERIFY(missing.contains("cccc"));QVERIFY(result.status.contains("对象提示"));
        snapshot->verified=false;result=checkEditorSql(sql,DatabaseKind::MySQL,"owned",{},std::make_shared<std::atomic_bool>(false),snapshot);for(const auto& mark:result.diagnostics)QVERIFY(!mark.metadata);QVERIFY(result.status.contains("表/字段未校验"));
    }
    void trailingInvalidStatementKeepsEarlierTableChecksAndVisibleRedWaves(){
        using namespace navidog;auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::MySQL;snapshot->database="hello";snapshot->searchPath={"hello"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;
        const QString text="-- 示例 SQL\nSELECT o.id, c.name\nFROM orders AS o\nJOIN customers AS c ON c.id=o.customer_id;\n\na";
        auto cancel=std::make_shared<std::atomic_bool>(false);auto checked=checkEditorSql(text,DatabaseKind::MySQL,"hello",{},cancel,snapshot);QStringList missing;bool syntax=false;for(const auto& mark:checked.diagnostics){if(mark.metadata)missing<<text.mid(mark.begin,mark.end-mark.begin);else if(mark.error)syntax=true;}QVERIFY(missing.contains("orders"));QVERIFY(missing.contains("customers"));QVERIFY(syntax);QVERIFY(checked.status.contains("对象提示"));
        auto barrier=checkEditorSql("SELECT * FROM orders; a; SELECT * FROM later_table;",DatabaseKind::MySQL,"hello",{},cancel,snapshot);missing.clear();for(const auto& mark:barrier.diagnostics)if(mark.metadata)missing<<mark.message;QVERIFY(std::any_of(missing.begin(),missing.end(),[](const auto& message){return message.contains("orders");}));QVERIFY(std::none_of(missing.begin(),missing.end(),[](const auto& message){return message.contains("later_table");}));
        auto ddl=checkEditorSql("SELECT * FROM orders; CREATE TABLE orders(id int); SELECT id FROM orders;",DatabaseKind::MySQL,"hello",{},cancel,snapshot);int missingBeforeDeclaration=0;for(const auto& mark:ddl.diagnostics)if(mark.metadata && mark.message.contains("未找到该表")){++missingBeforeDeclaration;QVERIFY(mark.begin<20);}QCOMPARE(missingBeforeDeclaration,1);
        snapshot->verified=false;auto disk=checkEditorSql(text,DatabaseKind::MySQL,"hello",{},cancel,snapshot);for(const auto& mark:disk.diagnostics)QVERIFY(!mark.metadata);snapshot->verified=true;
        SqlEditor editor;editor.resize(900,420);editor.applyPreferences(14,2,false);editor.setDiagnosticContext(DatabaseKind::MySQL,"hello",{});editor.setMetadataSnapshot(snapshot);editor.setPlainText(text);editor.show();QTRY_VERIFY_WITH_TIMEOUT(editor.diagnostics().size()>=3,4000);QVERIFY(editor.diagnosticAtPosition(text.indexOf("orders")).contains("未找到该表"));
        QTextCursor cursor(editor.document());cursor.setPosition(text.indexOf("orders"));const auto start=editor.cursorRect(cursor);cursor.setPosition(text.indexOf("orders")+6);const auto finish=editor.cursorRect(cursor);auto raster=editor.viewport()->grab().toImage();const auto scale=qreal(raster.width())/editor.viewport()->width();int pixels=0;
        for(int y=qMax(0,qRound((start.bottom()-5)*scale));y<qMin(raster.height(),qRound((start.bottom()+2)*scale));++y)for(int x=qMax(0,qRound(start.left()*scale));x<qMin(raster.width(),qRound(finish.left()*scale));++x){const auto color=raster.pixelColor(x,y);if(color.red()>140 && color.green()<95 && color.blue()<125)++pixels;}QVERIFY2(pixels>15,"Expected a clearly visible red wave below orders");
        const auto capture=qEnvironmentVariable("NAVIDOG_TEST_DIAGNOSTIC_SCREENSHOT");if(!capture.isEmpty())QVERIFY(editor.grab().save(capture));
    }
    void fullwidthCommaIsVisibleAndDoesNotHideMissingTablesInTheSameStatement(){
        using namespace navidog;auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::MySQL;snapshot->database="hello";snapshot->searchPath={"hello"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;
        const QString text=QString::fromUtf8(R"SQL(-- Navidog 示例库 · 100,000 条订单

SELECT
  o.order_no,
  c.name AS
  -- 选中一条 SQL 运行，或运行当前编辑器全部内容
  customer，
  c.city,
  o.status,
  ROUND(o.total_cents / 100.0, 2) AS amount,
  o.created_at
FROM orders AS o
JOIN customers AS c
ON c.id = o.customer_id
WHERE o.status = 'paid'
ORDER BY o.id DESC
LIMIT 100;
a
)SQL");
        auto cancel=std::make_shared<std::atomic_bool>(false);auto checked=checkEditorSql(text,DatabaseKind::MySQL,"hello",{},cancel,snapshot);QStringList missing;bool comma=false,tail=false;
        for(const auto& mark:checked.diagnostics){if(mark.metadata)missing<<text.mid(mark.begin,mark.end-mark.begin);if(mark.message.startsWith("疑似中文")){QCOMPARE(mark.begin,text.lastIndexOf(QChar(0xff0c)));QCOMPARE(mark.end,mark.begin+1);QVERIFY(mark.error);comma=true;}if(mark.error && text.mid(mark.begin,mark.end-mark.begin)=="a")tail=true;}
        QVERIFY(comma);QVERIFY(tail);QVERIFY(missing.contains("orders"));QVERIFY(missing.contains("customers"));QVERIFY(checked.status.contains("对象提示"));
        auto offline=checkEditorSql(text,DatabaseKind::MySQL,"hello",{},cancel);QVERIFY(std::any_of(offline.diagnostics.begin(),offline.diagnostics.end(),[](const auto& mark){return mark.message.startsWith("疑似中文");}));for(const auto& mark:offline.diagnostics)QVERIFY(!mark.metadata);
        const auto before=text;SqlEditor editor;editor.resize(940,680);editor.applyPreferences(13,2,false);editor.setDiagnosticContext(DatabaseKind::MySQL,"hello",{});editor.setMetadataSnapshot(snapshot);editor.setPlainText(text);editor.show();QTRY_VERIFY_WITH_TIMEOUT(editor.diagnosticAtPosition(text.lastIndexOf(QChar(0xff0c))).contains("疑似中文"),4000);QVERIFY(editor.diagnosticAtPosition(text.indexOf("orders AS")).contains("未找到该表"));QCOMPARE(editor.toPlainText(),before);const auto capture=qEnvironmentVariable("NAVIDOG_TEST_PUNCTUATION_SCREENSHOT");if(!capture.isEmpty())QVERIFY(editor.grab().save(capture));
        const QString emoji="SELECT 1 AS 前😀缀， 2 FROM absent;";auto unicode=checkEditorSql(emoji,DatabaseKind::MySQL,"hello",{},cancel,snapshot);for(const auto& mark:unicode.diagnostics)if(mark.message.startsWith("疑似中文"))QCOMPARE(mark.begin,emoji.indexOf(QChar(0xff0c)));
        const QString fragment="SELECT * FROM odd，name;";auto exact=checkEditorSql(fragment,DatabaseKind::MySQL,"hello",{},cancel,snapshot);for(const auto& mark:exact.diagnostics)if(mark.metadata)QCOMPARE(fragment.mid(mark.begin,mark.end-mark.begin),QString("odd，name"));
    }
    void silentCatalogSyncChecksTablesEvenWithAFullwidthProjectionComma(){
        using namespace navidog;QTemporaryDir owned;ConnectionProfile p;p.id="owned-punctuation-catalog";p.name="Owned empty SQLite";p.database=owned.filePath("empty.sqlite");{QFile file(p.database);QVERIFY(file.open(QIODevice::WriteOnly));}ProfileStore(localStorePath(owned.path())).save({p});
        MainWindow window(owned.path());window.show();auto* pane=dynamic_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());QVERIFY(pane);pane->connection->setCurrentIndex(pane->connection->findData(p.id));const QString text="SELECT o.id AS customer， c.name FROM orders o JOIN customers c ON c.id=o.id; a";pane->editor->setPlainText(text);QSignalSpy executed(&window,&MainWindow::queryFinished);CatalogNode root;root.kind="connection";QVERIFY(window.findChild<QueryService*>()->catalog(p,root,"owned-punctuation-sync"));
        QTRY_VERIFY2_WITH_TIMEOUT(pane->editor->diagnosticAtPosition(text.indexOf("orders o")).contains("未找到该表"),qPrintable(pane->editor->diagnosticStatus()),5000);QVERIFY(pane->editor->diagnosticAtPosition(text.indexOf("customers c")).contains("未找到该表"));QVERIFY(pane->editor->diagnosticAtPosition(text.indexOf(QChar(0xff0c))).contains("疑似中文"));QCOMPARE(executed.count(),0);QCOMPARE(pane->editor->toPlainText(),text);
    }
    void punctuationInsideStringsCommentsAndQuotedNamesIsOpaque(){
        const auto cancel=std::make_shared<std::atomic_bool>(false);
        const QList<QPair<navidog::DatabaseKind,QString>> examples={
          {navidog::DatabaseKind::MySQL,"-- 注释，；（）\nSELECT '字符串，；（）', `字段，名` FROM `表，名`;"},
          {navidog::DatabaseKind::PostgreSQL,"SELECT $$字符串，；（）$$, \"字段，名\" FROM \"表，名\";"},
          {navidog::DatabaseKind::SQLServer,"SELECT N'字符串，', [字段，名] FROM [表，名];"},
          {navidog::DatabaseKind::Oracle,"SELECT q'[字符串，；]', \"字段，名\" FROM \"表，名\";"},
          {navidog::DatabaseKind::SQLite,"SELECT '字符串，', \"字段，名\" FROM \"表，名\";"}};
        for(const auto& example:examples){auto checked=checkEditorSql(example.second,example.first,"owned",{},cancel);for(const auto& mark:checked.diagnostics)QVERIFY2(!mark.message.startsWith("疑似中文"),qPrintable(example.second));}
    }
    void metadataHintsUseFreshSnapshotsAndWithdrawOnInvalidation(){
        using namespace navidog;auto snapshot=std::make_shared<MetadataSnapshot>();snapshot->driver=DatabaseKind::PostgreSQL;snapshot->database="owned";snapshot->schema="public";snapshot->searchPath={"pg_catalog","public"};snapshot->verified=true;snapshot->refreshedMs=QDateTime::currentMSecsSinceEpoch();snapshot->tablesComplete=snapshot->columnsComplete=snapshot->routinesComplete=true;
        MetadataObject table;table.kind="table";table.name="items";table.schema="public";table.database="owned";table.key="items";table.columns={{"id","integer",{},1}};snapshot->objects<<table;
        SqlEditor editor;editor.setDiagnosticContext(DatabaseKind::PostgreSQL,"owned","public");editor.setMetadataSnapshot(snapshot);editor.setPlainText("SELECT absent FROM public.items");
        QTRY_COMPARE_WITH_TIMEOUT(editor.diagnostics().size(),1,4000);QVERIFY(editor.diagnostics()[0].metadata);QVERIFY(!editor.diagnostics()[0].error);QVERIFY(editor.diagnosticAtPosition(editor.diagnostics()[0].begin).contains("后台"));
        bool red=false;for(const auto& mark:editor.extraSelections())if(mark.format.underlineColor()==QColor("#d00020"))red=true;QVERIFY(red);
        auto stale=std::make_shared<MetadataSnapshot>(*snapshot);stale->invalidated=true;editor.setMetadataSnapshot(stale);QVERIFY(editor.diagnostics().isEmpty());QTRY_VERIFY_WITH_TIMEOUT(editor.diagnosticStatus().contains("失效"),4000);QVERIFY(editor.diagnostics().isEmpty());
        auto disk=std::make_shared<MetadataSnapshot>(*snapshot);disk->verified=false;editor.setMetadataSnapshot(disk);QTRY_VERIFY_WITH_TIMEOUT(editor.diagnosticStatus().contains("待验证"),4000);QVERIFY(editor.diagnostics().isEmpty());
        auto cancel=std::make_shared<std::atomic_bool>(false);auto ddl=checkEditorSql("CREATE TABLE fresh_owned(id int);SELECT id FROM fresh_owned",DatabaseKind::PostgreSQL,"owned","public",cancel,snapshot);for(const auto& mark:ddl.diagnostics)QVERIFY(!mark.metadata);
    }
    void metadataSyncInQueryWindowIsLocalAndDdlInvalidates(){
        using namespace navidog;QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);pane->editor->setPlainText("SELECT absent FROM orders");
        QSignalSpy done(&window,&MainWindow::queryFinished);auto profile=navidog::ProfileStore(navidog::localStorePath(dir.path())).load().front();navidog::CatalogNode root;root.kind="connection";QVERIFY(!window.findChild<QToolButton*>("syncMetadata"));QVERIFY(window.findChild<navidog::QueryService*>()->catalog(profile,root,"owned-background-sync"));QTRY_VERIFY2_WITH_TIMEOUT(pane->editor->diagnosticStatus().contains("元数据有效"),qPrintable(pane->editor->diagnosticStatus()),5000);QCOMPARE(done.count(),0);QVERIFY(!pane->editor->diagnostics().isEmpty());QVERIFY(pane->editor->diagnostics()[0].metadata);QVERIFY(QFile::exists(navidog::localStorePath(dir.path())));
        pane->editor->setPlainText("SELECT id FROM orders");QTRY_VERIFY_WITH_TIMEOUT(pane->editor->diagnosticStatus().contains("未发现问题"),4000);QVERIFY(pane->editor->diagnostics().isEmpty());
        pane->beginTransaction->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);QCOMPARE(pane->transactionState,TransactionState::Active);
        pane->editor->setPlainText("CREATE TABLE metadata_ui_transaction_owned(n int)");pane->run->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QCOMPARE(pane->transactionState,TransactionState::Active);
        pane->editor->setPlainText("SELECT absent FROM orders");QTRY_VERIFY_WITH_TIMEOUT(pane->editor->diagnosticStatus().contains("未同步"),4000);QVERIFY(pane->editor->diagnostics().isEmpty());
        pane->rollbackTransaction->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,3000);QTRY_VERIFY_WITH_TIMEOUT(pane->editor->diagnosticStatus().contains("元数据有效"),5000);
        pane->editor->setPlainText("SELECT n FROM metadata_ui_transaction_owned");QTRY_VERIFY_WITH_TIMEOUT(!pane->editor->diagnostics().isEmpty(),4000);QVERIFY(pane->editor->diagnostics()[0].metadata);
    }
    void liveSqlDiagnosticsUnderlineHoverAndClearAfterEdit(){
        using namespace navidog;SqlEditor editor;editor.resize(900,400);editor.show();
        editor.setDiagnosticContext(DatabaseKind::PostgreSQL,"offline","public");
        const QString sql=QString::fromUtf8("SELECT '中文😀';\r\nSELECT (1 + );\nUPDATE t SET n=;\nSELECT 1");
        editor.setPlainText(sql);editor.document()->setModified(false);
        QTRY_COMPARE_WITH_TIMEOUT(editor.diagnostics().size(),2,4000);
        const auto actual=editor.toPlainText();const int bad=actual.indexOf(")");
        QCOMPARE(editor.diagnostics()[0].begin,bad);QCOMPARE(editor.diagnostics()[0].end,bad+1);
        QCOMPARE(editor.diagnostics()[1].begin,actual.indexOf(';',actual.indexOf("UPDATE")));
        QVERIFY(editor.diagnosticAtPosition(bad).contains("缺少表达式"));QVERIFY(!editor.document()->isModified());
        int red=0;for(const auto& mark:editor.extraSelections())if(mark.format.underlineStyle()==QTextCharFormat::WaveUnderline){++red;QCOMPARE(mark.format.underlineColor(),QColor("#d00020"));}QCOMPARE(red,2);
        auto cursor=editor.textCursor();cursor.setPosition(bad);editor.setTextCursor(cursor);
        QCOMPARE(editor.extraSelections().size(),3);auto point=editor.cursorRect(cursor).center()+QPoint(2,0);
        QHelpEvent help(QEvent::ToolTip,point,editor.viewport()->mapToGlobal(point));QApplication::sendEvent(editor.viewport(),&help);
        auto *hover=editor.findChild<QFrame*>("sqlDiagnosticCard");QVERIFY(hover && hover->isVisible());QVERIFY(hover->findChild<QLabel*>("diagnosticCardBody")->text().contains("缺少表达式"));
        editor.setPlainText("SELECT 1");QVERIFY(editor.diagnostics().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(editor.diagnosticStatus().contains("未发现问题"),4000);QVERIFY(editor.diagnostics().isEmpty());QVERIFY(QToolTip::text().isEmpty());
    }
    void localSqlChecksDistinguishUnsupportedAndBoundWork(){
        using namespace navidog;auto cancel=std::make_shared<std::atomic_bool>(false);
        auto result=checkEditorSql("SELECT sum(n) OVER(ORDER BY n ROWS UNBOUNDED PRECEDING EXCLUDE NO OTHERS) FROM t",DatabaseKind::PostgreSQL,{},{},cancel);
        QVERIFY(result.diagnostics.isEmpty());
        result=checkEditorSql("VACUUM (ANALYZE) t",DatabaseKind::PostgreSQL,{},{},cancel);
        QVERIFY(result.diagnostics.isEmpty());
        result=checkEditorSql("COPY t TO PROGRAM 'not-executed'",DatabaseKind::PostgreSQL,{},{},cancel);
        QVERIFY(!result.diagnostics.isEmpty());for(const auto& mark:result.diagnostics)QVERIFY(!mark.error);
        result=checkEditorSql("safak; as; SELECT FROM",DatabaseKind::MySQL,{},{},cancel);QCOMPARE(result.diagnostics.size(),3);for(const auto& mark:result.diagnostics)QVERIFY(mark.error);
        result=checkEditorSql("SHOW TABLES; SHOW ENGINE INNODB STATUS; CALL p(); COMMIT",DatabaseKind::MySQL,{},{},cancel);QCOMPARE(result.diagnostics.size(),1);QVERIFY(!result.diagnostics.front().error);
        result=checkEditorSql("SELECT 1 +",DatabaseKind::SQLite,{},{},cancel);QCOMPARE(result.diagnostics.size(),1);QVERIFY(result.diagnostics[0].error);QCOMPARE(result.diagnostics[0].begin,9);QCOMPARE(result.diagnostics[0].end,10);
        result=checkEditorSql("SELECT 'not closed",DatabaseKind::MySQL,{},{},cancel);QVERIFY(!result.diagnostics.isEmpty());QVERIFY(result.diagnostics[0].error);
        QString broken=QString("SELECT '")+QChar(0xd800)+"'; SELECT FROM";result=checkEditorSql(broken,DatabaseKind::MySQL,{},{},cancel);QVERIFY(!result.diagnostics.isEmpty());QCOMPARE(result.diagnostics[0].begin,broken.indexOf("FROM"));
        result=checkEditorSql("CREATE TABLE t(a int,a int)",DatabaseKind::PostgreSQL,{},{},cancel);QVERIFY(!result.diagnostics.isEmpty());QVERIFY(result.diagnostics[0].error);
        result=checkEditorSql(QString(1024*1024+1,'x'),DatabaseKind::SQLite,{},{},cancel);QVERIFY(result.status.contains("上限"));QVERIFY(result.diagnostics.isEmpty());
        cancel->store(true);result=checkEditorSql("SELECT 1",DatabaseKind::SQLite,{},{},cancel);QVERIFY(result.status.contains("未完成"));QVERIFY(result.diagnostics.isEmpty());
    }
    void editorDiagnosticContextAndStaleResultsAreSafe(){
        using namespace navidog;auto* editor=new SqlEditor;editor->setDiagnosticContext(DatabaseKind::PostgreSQL);editor->setPlainText("SELECT `id` FROM items");
        QTRY_VERIFY_WITH_TIMEOUT(!editor->diagnostics().isEmpty(),4000);QVERIFY(editor->diagnostics()[0].error);
        editor->setDiagnosticContext(DatabaseKind::MySQL);QVERIFY(editor->diagnostics().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(editor->diagnosticStatus().contains("未发现问题"),4000);
        editor->setPlainText("SELECT FROM");QTest::qWait(355);editor->setPlainText("SELECT 1");
        QTRY_VERIFY_WITH_TIMEOUT(editor->diagnosticStatus().contains("未发现问题"),4000);QVERIFY(editor->diagnostics().isEmpty());
        editor->setPlainText("SELECT FROM");QTest::qWait(355);delete editor;QTest::qWait(100);
    }
    void queryEditorChecksDoNotExecuteOrOpenConnections(){
        QTemporaryDir dir;MainWindow window(dir.path());auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        QSignalSpy done(&window,&MainWindow::queryFinished);pane->editor->setPlainText("DELETE FROM never_created WHERE id=; SELECT FROM");
        QTRY_COMPARE_WITH_TIMEOUT(pane->editor->diagnostics().size(),2,4000);QCOMPARE(done.count(),0);QCOMPARE(pane->transactionState,navidog::TransactionState::Idle);QVERIFY(!pane->executing);
        QVERIFY(pane->findChild<QLabel*>("sqlCheckStatus")->text().contains("2 处"));
    }
    void unifiedAstCanBeViewedWithoutExecutingAndKeepsPreviousResults(){
        using namespace navidog;QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        QSignalSpy done(&window,&MainWindow::queryFinished);pane->editor->setPlainText("SELECT 17 AS saved_result");pane->run->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);QCOMPARE(pane->model->index(0,0).data().toInt(),17);
        pane->editor->setPlainText("INSERT INTO ast_never_created(id) SELECT id FROM source_items WHERE EXISTS(SELECT 1 FROM other_items);UPDATE ast_never_created SET id=3;DELETE FROM ast_never_created");
        pane->parseAst->click();QVERIFY(pane->executing);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QCOMPARE(pane->model->index(0,0).data().toInt(),17);QCOMPARE(pane->resultTabs->currentWidget(),pane->astView);QVERIFY(pane->generatedSql->isReadOnly());QVERIFY(pane->generatedSql->toPlainText().contains("INSERT INTO"));
        auto* tree=pane->findChild<QTreeView*>("queryAstTree");QVERIFY(tree);QCOMPARE(tree->model()->rowCount(),3);QVERIFY(tree->model()->index(0,0).data().toString().contains("INSERT"));
        QCOMPARE(pane->transactionState,TransactionState::Idle);QVERIFY(pane->analysis->toPlainText().contains("仅解析 · 未执行"));
        pane->findChild<QToolButton*>("copyUnifiedAst")->click();auto json=QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).array();QCOMPARE(json.size(),3);QVERIFY(!json[0].toObject()["unifiedAst"].toObject()["queries"].toArray().isEmpty());
        pane->editor->setPlainText("SELECT FROM");pane->parseAst->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,3000);QCOMPARE(tree->model()->rowCount(),1);QCOMPARE(pane->model->index(0,0).data().toInt(),17);QVERIFY(pane->analysis->toPlainText().contains("本地未能解析"));
    }
    void astSelectionCanResetAndCloseWithAccessibilityActive(){
        using namespace navidog;QAccessible::setActive(true);QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());
        pane->editor->setPlainText("SELECT 1,(SELECT 2)");pane->editor->document()->setModified(false);QSignalSpy done(&window,&MainWindow::queryFinished);pane->parseAst->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
        auto* tree=pane->findChild<QTreeView*>("queryAstTree");auto* interface=QAccessible::queryAccessibleInterface(tree);QVERIFY(interface);QVERIFY(interface->tableInterface());
        tree->expandAll();auto index=tree->model()->index(0,0);tree->setCurrentIndex(index);tree->selectionModel()->select(index,QItemSelectionModel::Select|QItemSelectionModel::Rows);QVERIFY(!tree->selectionModel()->selectedIndexes().isEmpty());
        auto selected=interface->tableInterface()->selectedCells();QVERIFY(!selected.isEmpty());for(auto* cell:selected){QVERIFY(cell);QVERIFY(cell->isValid());}
        pane->parseAst->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QVERIFY(tree->selectionModel()->selectedIndexes().isEmpty());
        tree->setCurrentIndex(tree->model()->index(0,0));pane->astView->prepareForClose();QVERIFY(tree->selectionModel()->selectedIndexes().isEmpty());QVERIFY(interface->tableInterface()->selectedCells().isEmpty());
        QPointer<QueryPane> pending=pane;window.findChild<QAction*>("menuCloseTab")->trigger();QTRY_VERIFY(pending.isNull());QCOMPARE(tabs->count(),0);
    }
    void standardCloseButtonsFollowMovedTabsAndLastTabCanClose(){
        QTemporaryDir dir;
        {MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        QPointer<QWidget> first=tabs->widget(0);QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);QPointer<QWidget> second=tabs->currentWidget();
        QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);QPointer<QWidget> third=tabs->currentWidget();
        QVERIFY(tabs->tabsClosable());tabs->tabBar()->moveTab(2,0);
        auto* button=qobject_cast<QAbstractButton*>(tabs->tabBar()->tabButton(tabs->indexOf(first),QTabBar::RightSide));QVERIFY(button);
        QTest::mouseClick(button,Qt::LeftButton);QTRY_VERIFY(first.isNull());QCOMPARE(tabs->count(),2);QVERIFY(second);QVERIFY(third);
        tabs->setCurrentWidget(second);window.findChild<QAction*>("menuCloseTab")->trigger();QTRY_VERIFY(second.isNull());QCOMPARE(tabs->count(),1);
        button=qobject_cast<QAbstractButton*>(tabs->tabBar()->tabButton(0,QTabBar::RightSide));QVERIFY(button);QTest::mouseClick(button,Qt::LeftButton);QTRY_COMPARE(tabs->count(),0);QVERIFY(window.close());}
        MainWindow restored(dir.path());QVERIFY(restored.restoredSession());QCOMPARE(restored.findChild<QTabWidget*>("queryTabs")->count(),0);
        QVERIFY(restored.findChildren<QueryPane*>().isEmpty());
        QCOMPARE(restored.findChild<navidog::QueryService*>()->sessionCount(),0);restored.findChild<QToolButton*>("newQuery")->click();QCOMPARE(restored.findChild<QTabWidget*>("queryTabs")->count(),1);
    }
    void dirtySqlAndActiveTransactionCanCancelCloseThenRollback(){
        using namespace navidog;QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        auto* service=window.findChild<QueryService*>();auto profile=ProfileStore(dir.filePath("navidog.sqlite")).load().front();auto db=connectDatabase(profile);auto token=cancellation();db->execute("CREATE TABLE close_tx_owned(n INTEGER)",{},token);
        pane->editor->setPlainText("INSERT INTO close_tx_owned VALUES(19)");pane->editor->document()->setModified(true);
        QTimer prompts;prompts.setInterval(5);connect(&prompts,&QTimer::timeout,&window,[&]{if(auto* box=window.findChild<QMessageBox*>("workspaceSaveSql");box && box->isVisible())box->button(QMessageBox::Cancel)->click();});prompts.start();
        window.findChild<QAction*>("menuCloseTab")->trigger();QTRY_VERIFY(window.findChild<QLabel*>("queryTransactionStatus"));QTest::qWait(30);QCOMPARE(tabs->count(),1);QVERIFY(pane->dirty());prompts.stop();
        pane->editor->document()->setModified(false);pane->autoCommit->setChecked(false);QSignalSpy done(&window,&MainWindow::queryFinished);pane->run->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);QCOMPARE(pane->transactionState,TransactionState::Active);
        bool accept=false;QTimer transaction;transaction.setInterval(5);connect(&transaction,&QTimer::timeout,&window,[&]{if(auto* box=window.findChild<QMessageBox*>("workspaceCloseTransaction");box && box->isVisible())box->button(accept?QMessageBox::Yes:QMessageBox::Cancel)->click();});transaction.start();
        window.findChild<QAction*>("menuCloseTab")->trigger();QTest::qWait(30);QCOMPARE(tabs->count(),1);QCOMPARE(pane->transactionState,TransactionState::Active);
        accept=true;window.findChild<QAction*>("menuCloseTab")->trigger();QTRY_COMPARE(tabs->count(),0);QTRY_COMPARE(service->sessionCount(),0);
        QCOMPARE(db->execute("SELECT count(*) FROM close_tx_owned",{},token).rows[0][0].toInt(),0);
    }
    void runningQueryCanStopAndCloseWithoutFreezingOtherTabs(){
        QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* first=dynamic_cast<QueryPane*>(tabs->currentWidget());QPointer<QueryPane> pending=first;
        first->editor->setPlainText("WITH RECURSIVE n(x) AS(SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<1000000000) SELECT sum(x) FROM n");first->editor->document()->setModified(false);
        first->run->click();QVERIFY(first->executing);window.findChild<QToolButton*>("newQuery")->click();auto* other=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(other!=first);
        QTimer prompts;prompts.setInterval(5);connect(&prompts,&QTimer::timeout,&window,[&]{if(auto* box=window.findChild<QMessageBox*>("workspaceStopAndClose");box && box->isVisible())box->button(QMessageBox::Yes)->click();});prompts.start();
        auto* close=qobject_cast<QAbstractButton*>(tabs->tabBar()->tabButton(tabs->indexOf(first),QTabBar::RightSide));QVERIFY(close);close->click();QTRY_VERIFY_WITH_TIMEOUT(pending.isNull(),3000);QCOMPARE(tabs->count(),1);QCOMPARE(tabs->currentWidget(),other);QVERIFY(other->run->isEnabled());
        other->editor->setPlainText("SELECT 31");QSignalSpy done(&window,&MainWindow::queryFinished);other->run->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);QCOMPARE(other->model->index(0,0).data().toInt(),31);
    }
    void connectionEditCanCloseRelatedDraftsOrReturnWithoutPartialClosure(){
        using namespace navidog;QTemporaryDir dir;ConnectionProfile p;p.id="owned-close-edit";p.name="Owned edit";p.database=ensureDemoDatabase(dir.path());ProfileStore store(dir.filePath("navidog.sqlite"));store.save({p});MainWindow window(dir.path());window.show();
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(1);auto* tabs=window.findChild<QTabWidget*>("queryTabs");QPointer<QWidget> unrelated=tabs->widget(0);tree->setCurrentItem(root);window.findChild<QToolButton*>("newQuery")->click();auto* query=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(query);QPointer<QueryPane> target=query;
        query->editor->setPlainText("SELECT 'draft retained'");query->editor->document()->setModified(true);
        root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);tree->setCurrentItem(database);QVERIFY(window.findChild<QAction*>("menuNewTable")->isEnabled());window.findChild<QAction*>("menuNewTable")->trigger();auto* table=dynamic_cast<TablePane*>(tabs->currentWidget());QVERIFY(table);QPointer<TablePane> draft=table;QVERIFY(table->dirty());tree->setCurrentItem(root);
        int mode=0;bool sawPrompt=false,edited=false;QTimer prompts;prompts.setInterval(5);connect(&prompts,&QTimer::timeout,&window,[&]{
            if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if(box->objectName()=="connectionCloseTabsPrompt"){sawPrompt=true;QCOMPARE(tabs->count(),3);box->findChild<QPushButton*>(mode==0?"handleConnectionTabsManually":"closeConnectionTabsAndEdit")->click();}
                else if(box->objectName()=="workspaceSaveSql")box->button(QMessageBox::Discard)->click();
                else if(box->objectName()=="workspaceDiscardChanges")box->button(mode==1?QMessageBox::Cancel:QMessageBox::Discard)->click();
                else box->reject();
            }else if(auto* dialog=window.findChild<QDialog*>("connectionDialog");dialog && dialog->isVisible()){edited=true;dialog->reject();}
        });prompts.start();
        window.findChild<QAction*>("menuEditConnection")->trigger();QVERIFY(sawPrompt);QCOMPARE(tabs->count(),3);QVERIFY(target);QVERIFY(draft);QVERIFY(!edited);
        mode=1;tree->setCurrentItem(root);window.findChild<QAction*>("menuEditConnection")->trigger();QCOMPARE(tabs->count(),3);QVERIFY(target);QVERIFY(draft);QVERIFY(target->dirty());QVERIFY(!edited);
        mode=2;tree->setCurrentItem(root);window.findChild<QAction*>("menuEditConnection")->trigger();QTRY_VERIFY_WITH_TIMEOUT(edited,3000);QTRY_COMPARE(tabs->count(),1);QCOMPARE(tabs->widget(0),unrelated.data());QTRY_VERIFY(target.isNull());QTRY_VERIFY(draft.isNull());QCOMPARE(store.load().size(),2);
    }
    void typedContextAndItemsWithoutDataKeepTheirDatabaseNames(){
        using namespace navidog;QTemporaryDir dir;ConnectionProfile pg;pg.id="owned-typed-context";pg.name="Owned context";pg.driver=DatabaseKind::PostgreSQL;pg.database="initial";pg.credentialSource=CredentialSource::Passwordless;ProfileStore(QDir(dir.path()).filePath("navidog.sqlite")).save({pg});
        MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        pane->connection->setCurrentIndex(pane->connection->findData(pg.id));pane->editor->setPlainText("SELECT owned_draft");
        QCOMPARE(pane->databaseContext->insertPolicy(),QComboBox::NoInsert);QCOMPARE(pane->schemaContext->insertPolicy(),QComboBox::NoInsert);
        auto* edit=pane->databaseContext->lineEdit();edit->setFocus();edit->selectAll();QTest::keyClicks(edit,"typed_database");QTest::keyClick(edit,Qt::Key_Return);
        QCOMPARE(pane->database,QString("typed_database"));
        pane->databaseContext->addItem("legacy_inserted_database");auto index=pane->databaseContext->count()-1;pane->databaseContext->setCurrentIndex(index);QMetaObject::invokeMethod(pane->databaseContext,"activated",Qt::DirectConnection,Q_ARG(int,index));
        QCOMPARE(pane->database,QString("legacy_inserted_database"));QCOMPARE(pane->databaseContext->currentData().toString(),QString("legacy_inserted_database"));
        pane->schemaContext->addItem("legacy_inserted_schema");index=pane->schemaContext->count()-1;pane->schemaContext->setCurrentIndex(index);QMetaObject::invokeMethod(pane->schemaContext,"activated",Qt::DirectConnection,Q_ARG(int,index));
        QCOMPARE(pane->schema,QString("legacy_inserted_schema"));QCOMPARE(pane->editor->toPlainText(),QString("SELECT owned_draft"));
        QCOMPARE(pane->sessionState().value("database").toString(),QString("legacy_inserted_database"));QCOMPARE(window.findChild<QueryService*>()->sessionCount(),0);
    }
    void executingQueryDisplaysAnalysisAndKeepsUnsupportedSqlUsable(){
        QTemporaryDir dir;MainWindow window(dir.path());window.show();QSignalSpy done(&window,&MainWindow::queryFinished);
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);
        pane->editor->setPlainText("WITH q AS (SELECT 41 AS n) SELECT CASE WHEN n=41 THEN n+1 ELSE 0 END AS answer FROM q");
        QTest::mouseClick(pane->run,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
        QCOMPARE(pane->model->index(0,0).data().toInt(),42);QVERIFY(pane->analysis->toPlainText().contains("结构已识别"));
        QVERIFY(pane->analysis->toPlainText().contains("2 个语句 / 子查询节点"));QVERIFY(pane->analysis->toPlainText().contains("执行成功"));
        QCOMPARE(pane->resultTabs->tabText(pane->resultTabs->indexOf(pane->analysis)),QString("SQL 分析"));
        auto* analysis=pane->analysis;pane->editor->setPlainText("SELECT sum(1) OVER(ROWS UNBOUNDED PRECEDING EXCLUDE NO OTHERS) AS answer");
        QTest::mouseClick(pane->run,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);
        QCOMPARE(pane->model->index(0,0).data().toInt(),1);QCOMPARE(pane->analysis,analysis);
        QVERIFY(pane->analysis->toPlainText().contains("结构已识别"));
        pane->editor->setPlainText("WITH RECURSIVE q(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM q WHERE n<3) SELECT MAX(n) AS answer FROM q");
        QTest::mouseClick(pane->run,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,3000);
        QCOMPARE(pane->model->index(0,0).data().toInt(),3);QCOMPARE(pane->analysis,analysis);
        QVERIFY(pane->analysis->toPlainText().contains("结构已识别"));
        pane->editor->setPlainText("PRAGMA schema_version");
        QTest::mouseClick(pane->run,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),4,3000);
        QVERIFY(pane->analysis->toPlainText().contains("PRAGMA · 结构已识别 · 执行成功"));
        pane->editor->setPlainText("ANALYZE");
        QTest::mouseClick(pane->run,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),5,3000);
        QVERIFY(pane->analysis->toPlainText().contains("本地未能解析 · 执行成功"));
        pane->resultTabs->setCurrentWidget(analysis);QCOMPARE(pane->resultTabs->currentWidget(),analysis);
    }
    void queryEntrypointsCarryTreeContextAndSelectorsPreserveDrafts(){
        using namespace navidog;QTemporaryDir dir;ConnectionProfile pg;pg.id="owned-context-pg";pg.name="Owned PG";pg.driver=DatabaseKind::PostgreSQL;pg.database="initial_db";pg.credentialSource=CredentialSource::Passwordless;ProfileStore(QDir(dir.path()).filePath("navidog.sqlite")).save({pg});
        {MainWindow window(dir.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");QTreeWidgetItem* root=nullptr;for(int i=0;i<tree->topLevelItemCount();++i)if(tree->topLevelItem(i)->data(0,Qt::UserRole).toString()==pg.id)root=tree->topLevelItem(i);QVERIFY(root);root->setData(0,Qt::UserRole+3,true);
        auto* selected=new QTreeWidgetItem(root,{"owned_column"});CatalogNode n;n.kind="column";n.database="owned_db";n.schema="Odd Schema";selected->setData(0,Qt::UserRole,pg.id);selected->setData(0,Qt::UserRole+2,QVariant::fromValue(n));tree->setCurrentItem(selected);
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        auto initialCount=tabs->count();
        QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);
        QCOMPARE(tabs->count(),initialCount+1);
        auto* newPane=dynamic_cast<QueryPane*>(tabs->currentWidget());
        QVERIFY(newPane);QCOMPARE(newPane->profileId,pg.id);QCOMPARE(newPane->database,QString("owned_db"));
        QCOMPARE(newPane->schema,QString("Odd Schema"));QVERIFY(newPane->schemaContext->isEnabled());
        QTest::mouseClick(window.findChild<QToolButton*>("browse_queries"),Qt::LeftButton);
        QCOMPARE(tabs->count(),initialCount+2);
        auto* library=dynamic_cast<WorkspacePane*>(tabs->currentWidget());QVERIFY(library);
        QCOMPARE(library->objectName(),QString("queryLibrary"));QCOMPARE(library->profileId,pg.id);
        QCOMPARE(library->database,QString("owned_db"));QCOMPARE(library->sessionState()["schema"].toString(),QString("Odd Schema"));
        auto count=tabs->count();window.findChild<QAction*>("menuNewQuery")->trigger();QCOMPARE(tabs->count(),count+1);auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());pane->editor->setPlainText("SELECT owned_draft");pane->databaseContext->setEditText("other_db");QMetaObject::invokeMethod(pane->databaseContext->lineEdit(),"editingFinished",Qt::DirectConnection);QCOMPARE(pane->database,QString("other_db"));QVERIFY(pane->schema.isEmpty());pane->schemaContext->setEditText("other_schema");QMetaObject::invokeMethod(pane->schemaContext->lineEdit(),"editingFinished",Qt::DirectConnection);QCOMPARE(pane->schema,QString("other_schema"));
        const auto session=pane->sessionId;pane->transactionState=TransactionState::Active;QTimer cancelPrompt;cancelPrompt.setInterval(10);connect(&cancelPrompt,&QTimer::timeout,&window,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* message=qobject_cast<QMessageBox*>(widget);message && message->isVisible()){message->reject();}});cancelPrompt.start();pane->schemaContext->setEditText("cancelled_schema");QMetaObject::invokeMethod(pane->schemaContext->lineEdit(),"editingFinished",Qt::DirectConnection);QCOMPARE(pane->schema,QString("other_schema"));QCOMPARE(pane->schemaContext->currentText(),QString("other_schema"));QCOMPARE(pane->sessionId,session);pane->transactionState=TransactionState::Idle;cancelPrompt.stop();
        pane->connection->setCurrentIndex(pane->connection->findData("harbor-demo"));QVERIFY(!pane->databaseContext->isEnabled());QVERIFY(!pane->schemaContext->isEnabled());QCOMPARE(pane->editor->toPlainText(),QString("SELECT owned_draft"));pane->connection->setCurrentIndex(pane->connection->findData(pg.id));QCOMPARE(pane->database,QString("initial_db"));QVERIFY(pane->schema.isEmpty());pane->schemaContext->setEditText("persisted_schema");QMetaObject::invokeMethod(pane->schemaContext->lineEdit(),"editingFinished",Qt::DirectConnection);QCOMPARE(window.findChild<QueryService*>()->sessionCount(),0);window.close();}
        MainWindow restored(dir.path());auto* pane=dynamic_cast<QueryPane*>(restored.findChild<QTabWidget*>("queryTabs")->currentWidget());QVERIFY(pane);QCOMPARE(pane->schema,QString("persisted_schema"));QCOMPARE(pane->editor->toPlainText(),QString("SELECT owned_draft"));QCOMPARE(restored.findChild<QueryService*>()->sessionCount(),0);
    }
    void queryLibraryStoresSqlWithoutExecutingIt() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MainWindow window(dir.path());
        window.show();
        auto* tabs = window.findChild<QTabWidget*>("queryTabs");
        auto* query = dynamic_cast<QueryPane*>(tabs->currentWidget());
        QVERIFY(query);
        const QString sql = "SELECT 42 AS owned_query";
        query->editor->setPlainText(sql);
        QTimer::singleShot(0, &window, [&window] {
            for (auto* prompt : window.findChildren<QInputDialog*>()) {
                prompt->setTextValue("Owned saved query");
                prompt->accept();
            }
        });
        window.findChild<QAction*>("menuSaveQueryInLibrary")->trigger();
        QVERIFY(!query->script.path.isEmpty());
        QCOMPARE(navidog::readScriptDocument(query->script.path).text, sql);
        window.findChild<QToolButton*>("browse_queries")->click();
        auto* library = tabs->currentWidget();
        QCOMPARE(library->objectName(), QString("queryLibrary"));
        auto* grid = library->findChild<QTableView*>("savedQueriesGrid");
        QCOMPARE(grid->model()->rowCount(), 1);
        grid->setCurrentIndex(grid->model()->index(0, 0));
        library->findChild<QToolButton*>("openSavedQuery")->click();
        QCOMPARE(tabs->currentWidget(), query);
        QCOMPARE(window.findChild<navidog::QueryService*>()->sessionCount(), 0);
    }
    void automationWritesAnIndependentSQLiteCopy() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkdir("backups"));
        MainWindow window(dir.path());
        window.show();
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("quickAutomationDialog");
            if (!dialog) return;
            dialog->findChild<QLineEdit*>("automationBackupFolder")->setText(root.filePath("backups"));
            for (auto* button : dialog->findChildren<QPushButton*>())
                if (button->text() == "保存并运行一次") { button->click(); return; }
            dialog->reject();
        });
        window.findChild<QToolButton*>("openAutomation")->click();
        QDir backups(root.filePath("backups"));
        QTRY_COMPARE_WITH_TIMEOUT(backups.entryList({"*.sqlite"}, QDir::Files).size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(navidog::LocalStore(navidog::localStorePath(dir.path()))
            .document("quick-backup-schedule").value_or(QByteArray{}).contains("lastFinished"), 5000);
        navidog::ConnectionProfile copy;
        copy.database = backups.filePath(backups.entryList({"*.sqlite"}, QDir::Files).first());
        copy.readOnly = true;
        auto db = navidog::connectDatabase(copy);
        const auto check = db->execute("SELECT COUNT(*) FROM orders", {}, navidog::cancellation());
        QVERIFY2(check.error.isEmpty(), qPrintable(check.error));
        QVERIFY(check.rows.value(0).value(0).toLongLong() > 0);
    }
    void biUsesTheCurrentCachedQueryResult() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MainWindow window(dir.path());
        window.show();
        auto* pane = dynamic_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());
        QVERIFY(pane);
        pane->editor->setPlainText("SELECT 'owned item' AS category, 7 AS amount");
        QSignalSpy result(&window, &MainWindow::queryFinished);
        pane->run->click();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        window.findChild<QToolButton*>("openBi")->click();
        auto* chart = window.findChild<QDialog*>("quickBiDialog");
        QVERIFY(chart && chart->isVisible());
        QCOMPARE(chart->findChild<QComboBox*>("biMetric")->currentText(), QString("amount"));
        QVERIFY(chart->findChild<QWidget*>("quickResultChart"));
        chart->close();
    }
    void quickEntryOpensAnUnloadedFolderUnlessTheUserSwitchesTabs() {
        for (const bool switchToQuery : {false, true}) {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            MainWindow window(dir.path());
            window.show();
            auto* toolbar = window.findChild<navidog::ui::QuickEntryBar*>("globalToolbar");
            QVERIFY(toolbar);
            auto* tableButton = toolbar->findChild<QToolButton*>("browse_tables");
            auto* modelButton = toolbar->findChild<QToolButton*>("openModels");
            QVERIFY(tableButton && modelButton && modelButton->menu());
            const QList<QAction*> modelActions{
                window.findChild<QAction*>("menuNewErModel"),
                window.findChild<QAction*>("menuOpenErModel"),
                window.findChild<QAction*>("menuReverseErModel")};
            QCOMPARE(modelButton->menu()->actions(), modelActions);
            auto* tree = window.findChild<QTreeWidget*>("connectionTree");
            auto* tabs = window.findChild<QTabWidget*>("queryTabs");
            auto* root = tree->topLevelItem(0);
            root->setExpanded(true);
            QTRY_COMPARE(root->childCount(), 1);
            auto* database = root->child(0);
            QCOMPARE(database->childCount(), 0);
            tree->setCurrentItem(database);

            QVERIFY(!tableButton->property("categorySelected").toBool());
            QTest::mouseClick(tableButton, Qt::LeftButton);
            auto* query = tabs->currentWidget();
            if (switchToQuery) {
                QTest::mouseClick(window.findChild<QToolButton*>("browse_queries"), Qt::LeftButton);
                QVERIFY(tabs->currentWidget() != query);
                query = tabs->currentWidget();
            }
            QTRY_VERIFY(database->childCount() > 0);
            if (switchToQuery) {
                QCOMPARE(tabs->currentWidget(), query);
                QCOMPARE(tabs->count(), 2);
            } else {
                auto* objects = dynamic_cast<ObjectsPane*>(tabs->currentWidget());
                QVERIFY(objects);
                QCOMPARE(objects->folder.kind, QString("tables"));
                QCOMPARE(tree->currentItem()->parent(), database);
                QVERIFY(tableButton->property("categorySelected").toBool());
            }
        }
    }
    void restoringObjectTabsDoesNotOverwritePreviousTableDraftTitle(){
        QTemporaryDir dir;QStringList titles;
        {MainWindow window(dir.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* db=root->child(0);db->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(db->childCount(),3,3000);tree->setCurrentItem(db);window.findChild<QAction*>("menuNewTable")->trigger();auto* tabs=window.findChild<QTabWidget*>("queryTabs");QVERIFY(dynamic_cast<TablePane*>(tabs->currentWidget()));tabs->setTabText(tabs->currentIndex(),"owned table draft title");auto* folder=db->child(0);QMetaObject::invokeMethod(tree,"itemDoubleClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,folder),Q_ARG(int,0));QVERIFY(dynamic_cast<ObjectsPane*>(tabs->currentWidget()));QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<navidog::QueryService*>()->busy(),3000);for(int i=0;i<tabs->count();++i)titles<<tabs->tabText(i);QVERIFY(window.close());}
        MainWindow restored(dir.path());auto* tabs=restored.findChild<QTabWidget*>("queryTabs");QCOMPARE(tabs->count(),titles.size());for(int i=0;i<tabs->count();++i)QCOMPARE(tabs->tabText(i),titles[i]);QVERIFY(dynamic_cast<TablePane*>(tabs->widget(1)));
    }
    void databaseErrorsRemainVisibleWithoutBlockingNewQuery() {
        QTemporaryDir dir;
        MainWindow window(dir.path());
        window.show();
        auto* service = window.findChild<navidog::QueryService*>();
        navidog::QueryOutcome error;
        error.result.error = "owned catalog failure\nSQLSTATE: XX000\nHint: <plain text>";
        service->completed(error);
        auto* dialog = window.findChild<QDialog*>("databaseErrorDialog");
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(!dialog->isModal());
        auto* details = dialog->findChild<QPlainTextEdit*>("databaseErrorDetails");
        QVERIFY(details && details->isVisible() && details->isReadOnly());
        QCOMPARE(details->toPlainText(), error.result.error);

        error.result.error = "new owned error detail\nsecond line";
        service->completed(error);
        QCOMPARE(window.findChildren<QDialog*>("databaseErrorDialog").size(), 1);
        QCOMPARE(details->toPlainText(), error.result.error);
        auto* tabs = window.findChild<QTabWidget*>("queryTabs");
        const auto count = tabs->count();
        QTest::mouseClick(window.findChild<QToolButton*>("newQuery"), Qt::LeftButton);
        QCOMPARE(tabs->count(), count + 1);
        QVERIFY(dialog->isVisible());
        dialog->accept();
    }
    void rightClickMenusStartAfterMouseReleaseAndDoNotBlockOtherControls() {
        class ProbeTree : public navidog::ui::ObjectTree {
        public:
            bool inRelease = false;
        protected:
            void mouseReleaseEvent(QMouseEvent* event) override {
                inRelease = true;
                ObjectTree::mouseReleaseEvent(event);
                inRelease = false;
            }
        };
        QWidget window;
        QVBoxLayout layout(&window);
        ProbeTree tree;
        tree.setColumnCount(1);
        const auto* item = new QTreeWidgetItem(&tree,{"owned"});
        tree.setContextMenuPolicy(Qt::CustomContextMenu);
        QPushButton button("Query");
        layout.addWidget(&tree);
        layout.addWidget(&button);
        window.show();
        QSignalSpy clicked(&button,&QPushButton::clicked);
        int menus = 0;
        connect(&tree,&QWidget::customContextMenuRequested,&tree,[&](QPoint point) {
            QVERIFY(!tree.inRelease);
            QMenu menu;
            menu.addAction("owned");
            QTimer::singleShot(10,&menu,&QMenu::close);
            menu.exec(tree.viewport()->mapToGlobal(point));
            ++menus;
        });

        const auto row = tree.viewport()->mapTo(&window,tree.visualItemRect(item).center());
        for (int i = 0; i < 3; ++i) {
            QTest::mouseClick(window.windowHandle(),Qt::RightButton,Qt::NoModifier,row);
            QTRY_COMPARE(menus,i+1);
            QCoreApplication::processEvents();
            QCOMPARE(menus,i+1);
            QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,
                              button.mapTo(&window,button.rect().center()));
            QCOMPARE(clicked.count(),i+1);
        }
    }
    void objectGridRightClickQueuesOneContextMenu() {
        navidog::ui::ObjectGrid grid;
        QStandardItemModel model(1,1);
        grid.setModel(&model);
        grid.setContextMenuPolicy(Qt::CustomContextMenu);
        grid.show();
        const auto cell = grid.viewport()->mapTo(&grid,grid.visualRect(model.index(0,0)).center());
        QSignalSpy menuRequests(&grid,&QWidget::customContextMenuRequested);
        QTest::mouseClick(grid.windowHandle(),Qt::RightButton,Qt::NoModifier,cell);
        QTRY_COMPARE(menuRequests.size(),1);
        QCoreApplication::processEvents();
        QCOMPARE(menuRequests.size(),1);
    }
    void sqlLineCommandsRespectSelectionBoundaryUndoAndReadOnly(){
        SqlEditor editor;editor.setPlainText("SELECT 中文;\n  FROM owned;\nWHERE id=1;");auto cursor=editor.textCursor();cursor.setPosition(0);cursor.setPosition(QString("SELECT 中文;\n  FROM owned;\n").size(),QTextCursor::KeepAnchor);editor.setTextCursor(cursor);
        QTest::keyClick(&editor,Qt::Key_Tab);QCOMPARE(editor.toPlainText(),QString("    SELECT 中文;\n      FROM owned;\nWHERE id=1;"));editor.undo();QCOMPARE(editor.toPlainText(),QString("SELECT 中文;\n  FROM owned;\nWHERE id=1;"));
        cursor.setPosition(0);cursor.setPosition(QString("SELECT 中文;\n  FROM owned;\n").size(),QTextCursor::KeepAnchor);editor.setTextCursor(cursor);editor.toggleLineComment();QCOMPARE(editor.toPlainText(),QString("-- SELECT 中文;\n  -- FROM owned;\nWHERE id=1;"));editor.toggleLineComment();QCOMPARE(editor.toPlainText(),QString("SELECT 中文;\n  FROM owned;\nWHERE id=1;"));editor.undo();QVERIFY(editor.toPlainText().startsWith("-- SELECT"));editor.undo();QVERIFY(editor.toPlainText().startsWith("SELECT"));
        editor.setPlainText("\tfirst\n  second\n\nthird");cursor=editor.textCursor();cursor.setPosition(16);cursor.setPosition(0,QTextCursor::KeepAnchor);editor.setTextCursor(cursor);editor.indentLines(true);QCOMPARE(editor.toPlainText(),QString("first\nsecond\n\nthird"));QVERIFY(editor.textCursor().position()<editor.textCursor().anchor());editor.undo();QCOMPARE(editor.toPlainText(),QString("\tfirst\n  second\n\nthird"));
        editor.setPlainText("    SELECT 1");cursor=editor.textCursor();cursor.setPosition(7);editor.setTextCursor(cursor);QTest::keyClick(&editor,Qt::Key_Backtab);QCOMPARE(editor.toPlainText(),QString("SELECT 1"));QCOMPARE(editor.textCursor().position(),3);editor.undo();
        editor.setReadOnly(true);const auto original=editor.toPlainText();QTest::keyClick(&editor,Qt::Key_Tab);QTest::keyClick(&editor,Qt::Key_Backtab);editor.indentLines();editor.toggleLineComment();QCOMPARE(editor.toPlainText(),original);
    }
    void sqlGoToLineUsesDocumentBoundsWithoutEditing(){
        SqlEditor editor;editor.show();editor.setPlainText("SELECT 1;\nSELECT 2;\nSELECT 3;");bool checked=false;QTimer::singleShot(20,&editor,[&]{auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);QCOMPARE(dialog->intMaximum(),3);dialog->setIntValue(3);checked=true;dialog->accept();});editor.goToLine();QVERIFY(checked);QCOMPARE(editor.textCursor().blockNumber(),2);QCOMPARE(editor.textCursor().positionInBlock(),0);QCOMPARE(editor.toPlainText(),QString("SELECT 1;\nSELECT 2;\nSELECT 3;"));
    }
    void sqlFindReplaceWrapsHonorsIdentifiersAndUndoesAsOneEdit(){
        SqlEditor editor;editor.show();editor.setPlainText("foo Foo foo_bar $foo foo$\nfoo");editor.moveCursor(QTextCursor::Start);editor.showFind(true);
        auto* find=editor.findChild<QLineEdit*>("sqlFindText");auto* replacement=editor.findChild<QLineEdit*>("sqlReplaceText");auto* whole=editor.findChild<QCheckBox*>("sqlFindWord");QVERIFY(find && replacement && whole);
        find->setText("foo");whole->setChecked(true);QVERIFY(editor.findNext());QCOMPARE(editor.textCursor().selectionStart(),0);QVERIFY(editor.findNext());QCOMPARE(editor.textCursor().selectedText(),QString("Foo"));QVERIFY(editor.findNext());QCOMPARE(editor.textCursor().selectionStart(),26);QVERIFY(editor.findNext());QCOMPARE(editor.textCursor().selectionStart(),0);QVERIFY(editor.findNext(true));QCOMPARE(editor.textCursor().selectionStart(),26);
        replacement->setText("\\1$literal");editor.replaceAll();QCOMPARE(editor.toPlainText(),QString("\\1$literal \\1$literal foo_bar $foo foo$\n\\1$literal"));editor.undo();QCOMPARE(editor.toPlainText(),QString("foo Foo foo_bar $foo foo$\nfoo"));
        editor.findChild<QCheckBox*>("sqlFindCase")->setChecked(true);editor.moveCursor(QTextCursor::Start);editor.findNext();replacement->setText("done");editor.replaceCurrent();QVERIFY(editor.toPlainText().startsWith("done Foo"));editor.undo();QVERIFY(editor.toPlainText().startsWith("foo Foo"));
        editor.setReadOnly(true);editor.replaceAll();QVERIFY(editor.toPlainText().startsWith("foo Foo"));editor.hide();QVERIFY(!editor.findChild<QDialog*>("sqlFindDialog")->isVisible());
    }
    void sqlReplaceAllRefusesOversizedChangesWithoutPartialEdits(){
        SqlEditor editor;editor.show();editor.setPlainText(QString("z ").repeated(100001));editor.showFind(true);auto* find=editor.findChild<QLineEdit*>("sqlFindText");auto* replacement=editor.findChild<QLineEdit*>("sqlReplaceText");find->setText("z");replacement->setText("x");const auto original=editor.toPlainText();editor.replaceAll();QCOMPARE(editor.toPlainText(),original);QVERIFY(editor.findChild<QLabel*>("sqlFindStatus")->text().contains("100000"));
        editor.setPlainText(QString("z ").repeated(100));replacement->setText(QString(65536,QChar('x')));const auto small=editor.toPlainText();editor.replaceAll();QCOMPARE(editor.toPlainText(),small);QVERIFY(editor.findChild<QLabel*>("sqlFindStatus")->text().contains("4 MiB"));
        replacement->clear();editor.replaceAll();QCOMPARE(editor.toPlainText(),QString(100,QChar(' ')));editor.undo();QCOMPARE(editor.toPlainText(),small);
    }
    void findMenuTargetsCurrentSqlTabAndHidesOnTabChange(){
        QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* original=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(original);QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);auto* current=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(current && current!=original);current->editor->setPlainText("SELECT owned_token");current->editor->moveCursor(QTextCursor::Start);window.findChild<QAction*>("menuFindSql")->trigger();auto* dialog=current->editor->findChild<QDialog*>("sqlFindDialog");QVERIFY(dialog && dialog->isVisible());QVERIFY(!original->editor->findChild<QDialog*>("sqlFindDialog"));dialog->findChild<QLineEdit*>("sqlFindText")->setText("owned_token");current->editor->findNext();QCOMPARE(current->editor->textCursor().selectedText(),QString("owned_token"));tabs->setCurrentWidget(original);QVERIFY(!dialog->isVisible());
    }
    void resultCellInspectionWaitsForMouseRelease() {
        QWidget window;
        QVBoxLayout layout(&window);
        navidog::ui::ObjectGrid grid;
        layout.addWidget(&grid);
        ResultModel model;
        navidog::QueryResult result;
        result.columns = {"owned"};
        result.rows = {{1}};
        model.setResult(result);
        grid.setModel(&model);
        navidog::ui::configureResultGrid(&grid);
        window.show();
        checkDialogAfterMouseRelease(grid.viewport(),grid.visualRect(model.index(0,0)).center(),
                                     "resultCellDialog");
    }

    void resultGridsInspectJsonAndCopyEmptyNullBinaryAndHeaders(){
        navidog::ui::ObjectGrid grid;ResultModel model;grid.setModel(&model);navidog::ui::configureResultGrid(&grid);navidog::QueryResult result;result.columns={"first","second"};result.columnTypes={"TEXT","BLOB"};result.rows={{QString(""),QVariant{}},{QString("a\tb"),QByteArray::fromHex("00ff")}};model.setResult(result);grid.selectAll();
        QCOMPARE(navidog::ui::resultSelectionText(&grid),QString("\tNULL\n\"a\tb\"\t00ff"));QCOMPARE(navidog::ui::resultSelectionText(&grid,true),QString("first\tsecond\n\tNULL\n\"a\tb\"\t00ff"));QCOMPARE(model.headerData(0,Qt::Horizontal,Qt::DisplayRole).toString(),QString("first\nTEXT"));
        grid.horizontalHeader()->moveSection(0,1);QCOMPARE(navidog::ui::resultSelectionText(&grid,true),QString("second\tfirst\nNULL\t\n00ff\t\"a\tb\""));grid.horizontalHeader()->moveSection(0,1);
        result.rows={{QString("{\"owned\": 1}"),QVariant{}}};model.setResult(result);grid.show();bool inspected=false;
        QTimer::singleShot(20,&grid,[&]{auto* dialog=grid.findChild<QDialog*>("resultCellDialog");if(!dialog)return;auto* text=dialog->findChild<QPlainTextEdit*>("resultCellText");QVERIFY(text && text->isReadOnly());QVERIFY(dialog->findChild<QPlainTextEdit*>("resultCellJson")->toPlainText().contains("owned"));inspected=true;dialog->reject();});
        grid.doubleClicked(model.index(0,0));QVERIFY(inspected);
        grid.setCurrentIndex(model.index(0,0));bool contextOpened=false;QTimer::singleShot(20,&grid,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;QStringList labels;for(auto* action:menu->actions())labels<<action->text();QVERIFY(labels.contains("复制并包含列名"));contextOpened=true;menu->close();});QTest::keyClick(&grid,Qt::Key_F10,Qt::ShiftModifier);QVERIFY(contextOpened);
        result.columns={"bounded"};result.columnTypes.clear();result.rows.clear();for(int i=0;i<10001;++i)result.rows<<QVector<QVariant>{i};model.setResult(result);grid.selectAll();QVERIFY_THROWS_EXCEPTION(std::runtime_error,navidog::ui::resultSelectionText(&grid));
    }
    void oracleUserDialogMasksPasswordAndValidatesConfirmationWithoutConnecting(){
        navidog::QueryService service;navidog::ConnectionProfile p;p.driver=navidog::DatabaseKind::Oracle;p.database="owned_pdb";
        NamespaceDialog dialog(p,navidog::NamespaceKind::Schema,service);auto password="N9_"+QUuid::createUuid().toString(QUuid::Id128).left(16);
        dialog.findChild<QLineEdit*>("namespaceName")->setText("OWNED_UI_USER");auto* input=dialog.findChild<QLineEdit*>("namespacePassword");input->setText(password);QCOMPARE(input->echoMode(),QLineEdit::Password);
        auto* preview=dialog.findChild<QPlainTextEdit*>("namespaceSqlPreview");QVERIFY(preview->toPlainText().contains("不一致"));auto* ok=dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);QVERIFY(!ok->isEnabled());
        dialog.findChild<QLineEdit*>("namespaceConfirmPassword")->setText(password);QVERIFY(ok->isEnabled());QVERIFY(preview->toPlainText().contains("<password>"));QVERIFY(!preview->toPlainText().contains(password));QVERIFY(!service.busy());
        // No event loop here: the deferred options request is cancelled with the dialog.
    }
    void sqlFileDocumentsSaveAndRestoreDraftsWithoutOverwritingExternalChanges(){
        QTemporaryDir directory;const auto path=directory.filePath("owned.sql");navidog::ScriptDocument empty;auto script=navidog::writeScriptDocument(empty,path,"SELECT 71;\n");
        {
            MainWindow window(directory.path());window.show();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);pane->setScriptDocument(script);QVERIFY(!pane->dirty());pane->editor->setPlainText("SELECT 73;\n");QVERIFY(pane->dirty());QVERIFY(pane->saveSqlFile());QCOMPARE(navidog::readScriptDocument(path).text,QString("SELECT 73;\n"));
            pane->editor->setPlainText("SELECT 79;\n");QFile external(path);QVERIFY(external.open(QIODevice::WriteOnly|QIODevice::Truncate));external.write("SELECT 83;\n");external.close();QTimer::singleShot(20,&window,[]{auto* dialog=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(dialog)dialog->button(QMessageBox::Cancel)->click();});QVERIFY(!pane->saveSqlFile());QCOMPARE(navidog::readScriptDocument(path).text,QString("SELECT 83;\n"));QVERIFY(pane->dirty());window.close();
        }
        MainWindow restored(directory.path());auto* tabs=restored.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<QueryPane*>(tabs->currentWidget());QVERIFY(pane);QCOMPARE(pane->script.path,QFileInfo(path).canonicalFilePath());QCOMPARE(pane->editor->toPlainText(),QString("SELECT 79;\n"));QVERIFY(pane->dirty());QCOMPARE(navidog::readScriptDocument(path).text,QString("SELECT 83;\n"));QCOMPARE(restored.findChild<QTreeWidget*>("connectionTree")->topLevelItem(0)->childCount(),0);
    }
    void largeSqlFilesLoadHighlightEditAndRestore() {
        QTemporaryDir directory;
        const auto path = directory.filePath("large.sql");
        const QByteArray row = "INSERT INTO owned VALUES (123, '" + QByteArray(152, 'a') + "');\n";
        const int sizeMiB = qBound(8, qEnvironmentVariableIntValue("NAVIDOG_TEST_LARGE_SQL_MIB"), 120);
        const int rows = sizeMiB * 1024 * 1024 / row.size();
        const QByteArray longLine = "INSERT INTO owned VALUES ('" + QByteArray(550000, 'x') + "');\n";
        const QByteArray tail = "SELECT unique_marker;\n";
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(row.repeated(rows)), qint64(row.size()) * rows);
            QCOMPARE(file.write(longLine), qint64(longLine.size()));
            QCOMPARE(file.write(tail), qint64(tail.size()));
        }
        const int length = row.size() * rows + longLine.size() + tail.size();
        QString canonicalPath;
        {
            MainWindow window(directory.path());
            window.show();
            auto *tabs = window.findChild<QTabWidget *>("queryTabs");
            auto *pane = dynamic_cast<QueryPane *>(tabs->currentWidget());
            QVERIFY(pane);
            auto *editor = pane->editor;
            pane->editor->applyPreferences(12, 4, true);
            qint64 lastTick = 0, longestTickMs = 0, firstTextMs = -1;
            QElapsedTimer duration;
            QTimer heartbeat;
            heartbeat.setInterval(10);
            connect(&heartbeat, &QTimer::timeout, pane, [&] {
                const auto now = duration.elapsed();
                longestTickMs = qMax(longestTickMs, now - lastTick);
                lastTick = now;
                if (firstTextMs < 0 && !editor->document()->isEmpty())
                    firstTextMs = now;
            });
            duration.start();
            heartbeat.start();
            pane->openScriptFile(path);
            QVERIFY(!pane->scriptReady());
            QVERIFY(editor->isReadOnly());
            QVERIFY(pane->findChild<QLabel *>("sqlScriptPolicy")->isVisible());
            QTRY_VERIFY_WITH_TIMEOUT(pane->scriptReady(), 25000);
            longestTickMs = qMax(longestTickMs, duration.elapsed() - lastTick);
            const auto openMs = duration.restart();
            heartbeat.stop();
            QVERIFY(firstTextMs >= 0);
            QVERIFY(firstTextMs < openMs);
            qInfo() << "First text ms" << firstTextMs << "longest GUI heartbeat ms" << longestTickMs;
            if (qEnvironmentVariableIsSet("NAVIDOG_TEST_LARGE_SQL_MIB"))
                qInfo().noquote() << navidog::operationLogSnapshot().text;
            canonicalPath = pane->script.path;
            QCOMPARE(editor->document()->characterCount(), length + 1);
            QCOMPARE(editor->document()->documentMargin(), qreal(0));
            QVERIFY(editor->largeDocument());
            QVERIFY(!editor->isReadOnly());
            QVERIFY(!pane->dirty());
            QVERIFY(!pane->run->isEnabled());
            QVERIFY(!pane->parseAst->isEnabled());
            QCOMPARE(editor->lineWrapMode(), QPlainTextEdit::NoWrap);
            QVERIFY(pane->findChild<QLabel *>("sqlScriptPolicy")->text().contains("快速高亮"));
            QTRY_VERIFY(!editor->document()->firstBlock().layout()->formats().isEmpty());
            for (int i = 1; i <= 15; ++i) {
                editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum() * i / 16);
                editor->viewport()->grab();
            }
            const auto scrollMs = duration.restart();
            QTextCursor cursor(editor->document()->findBlockByNumber(rows + 1));
            editor->setTextCursor(cursor);
            editor->ensureCursorVisible();
            QTRY_VERIFY(!cursor.block().layout()->formats().isEmpty());
            QVERIFY(editor->document()->firstBlock().layout()->formats().isEmpty());
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
            QVERIFY(pane->run->isEnabled());
            QVERIFY(pane->parseAst->isEnabled());
            editor->showCompletion();
            QVERIFY(!editor->findChild<QTreeView *>("sqlCompletionPopup")->isVisible());
            editor->insertPlainText("SELECT edited_marker;");
            QVERIFY(pane->dirty());
            editor->undo();
            QCOMPARE(cursor.block().text(), QString("SELECT unique_marker;"));
            QVERIFY(!pane->dirty());
            editor->showFind(true);
            editor->findChild<QLineEdit *>("sqlFindText")->setText("unique_marker");
            editor->findChild<QLineEdit *>("sqlReplaceText")->setText("saved_marker");
            QVERIFY(editor->findNext(true));
            editor->replaceCurrent();
            QCOMPARE(cursor.block().text(), QString("SELECT saved_marker;"));
            const auto editMs = duration.restart();
            QVERIFY(pane->saveSqlFile());
            QCOMPARE(QFileInfo(path).size(), qint64(length - 1));
            const auto saveMs = duration.restart();
            editor->findChild<QDialog *>("sqlFindDialog")->hide();
            cursor.setPosition(cursor.block().position());
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
            editor->insertPlainText("SELECT unsaved_draft_marker;");
            const auto session = pane->sessionState();
            QVERIFY(session["externalSql"].toBool());
            QVERIFY(!session.contains("sql"));
            QVERIFY(window.close());
            qInfo() << "Large SQL" << length << "bytes"
                    << "open ms" << openMs << "15 scroll jumps ms" << scrollMs
                    << "edit and find ms" << editMs << "save ms" << saveMs
                    << "save workspace ms" << duration.elapsed();
        }
        MainWindow restored(directory.path());
        restored.show();
        auto *pane = dynamic_cast<QueryPane *>(restored.findChild<QTabWidget *>("queryTabs")->currentWidget());
        QVERIFY(pane);
        QTRY_VERIFY_WITH_TIMEOUT(pane->scriptReady(), 25000);
        QCOMPARE(pane->script.path, canonicalPath);
        QVERIFY(pane->dirty());
        QCOMPARE(pane->editor->document()->findBlockByNumber(rows + 1).text(),
                 QString("SELECT unsaved_draft_marker;"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.seek(file.size() - 25));
        QVERIFY(file.readAll().endsWith("SELECT saved_marker;\n"));
    }

    void sqlOutlineSelectsAndCopiesOriginalUnicodeSql() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.resize(1250, 760);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("queryTabs");
        auto *query = dynamic_cast<QueryPane *>(tabs->currentWidget());
        QVERIFY(query);
        query->editor->setDiagnosticContext(navidog::DatabaseKind::MySQL);
        query->editor->setPlainText(QString::fromUtf8(
            "-- 中文 😀\nCREATE TABLE owned (id INT);\n"
            "DELIMITER $$\nCREATE PROCEDURE owned_p(IN p INT) BEGIN\n"
            "IF p > 0 THEN SELECT '实际;' AS note; ELSE SELECT 0; END IF;\n"
            "END$$\nDELIMITER ;\nDROP TABLE owned;"));
        auto *toggle = query->findChild<QToolButton *>("toggleSqlOutline");
        auto *tree = query->findChild<QTreeWidget *>("sqlOutlineTree");
        QVERIFY(toggle && tree);
        QVERIFY(!tree->isVisible());
        toggle->click();
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 3, 4500);
        QVERIFY(tree->isEnabled());
        tree->expandAll();
        QTreeWidgetItemIterator cursor(tree);
        while (*cursor && !(*cursor)->text(0).startsWith("SELECT '实际;'")) ++cursor;
        auto *node = *cursor;
        QVERIFY(node);
        tree->scrollToItem(node);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(node).center());
        QCOMPARE(query->editor->textCursor().selectedText(), QString("SELECT '实际;' AS note;"));
        QCOMPARE(query->editor->textCursor().position(), query->editor->textCursor().selectionStart());
        bool copied = false;
        QTimer::singleShot(20, tree, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            auto *action = menu->findChild<QAction *>("sqlOutlineCopy");
            if (action) {
                action->trigger();
                copied = true;
            }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                                          Q_ARG(QPoint, tree->visualItemRect(node).center())));
        QVERIFY(copied);
        QCOMPARE(QApplication::clipboard()->text(), QString("SELECT '实际;' AS note;"));
        query->editor->setPlainText("DROP TABLE next_table;");
        QVERIFY(!tree->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(tree->isEnabled() && tree->topLevelItemCount() == 1, 4500);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("DROP TABLE next_table"));
        QCOMPARE(query->sessionState()["outlineOpen"].toBool(), true);

        navidog::ConnectionProfile profile;
        profile.driver = navidog::DatabaseKind::PostgreSQL;
        navidog::CatalogNode object;
        object.kind = "function";
        object.name = object.object = "owned_f";
        SourcePane source(profile, object, true);
        source.resize(900, 600);
        source.show();
        navidog::SourceDefinition definition;
        definition.node = object;
        definition.exists = true;
        definition.sql = "CREATE FUNCTION owned_f() RETURNS INT AS $$BEGIN RETURN 5; END;$$ LANGUAGE plpgsql;";
        source.acceptDefinition(definition);
        auto *sourceTree = source.findChild<QTreeWidget *>("sqlOutlineTree");
        QVERIFY(sourceTree && sourceTree->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(sourceTree->topLevelItemCount(), 1, 4500);
        QVERIFY(sourceTree->topLevelItem(0)->text(0).contains("owned_f"));
        definition.sql.replace("owned_f", "reloaded_f");
        source.acceptDefinition(definition);
        QTRY_VERIFY_WITH_TIMEOUT(sourceTree->topLevelItemCount() == 1 &&
            sourceTree->topLevelItem(0)->text(0).contains("reloaded_f"), 4500);
    }

    void oversizedSqlShowsReadOnlyPrefix() {
        QTemporaryDir directory;
        const auto path = directory.filePath("oversized.sql");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const QByteArray prefix = QByteArray("SELECT 1;\n").repeated(4096);
            QCOMPARE(file.write(prefix), qint64(prefix.size()));
            QVERIFY(file.resize(qint64(navidog::editableScriptBytes) + 1));
        }
        QueryPane pane({}, navidog::WorkspaceTabKind::Query);
        pane.show();
        pane.openScriptFile(path);
        auto *policy = pane.findChild<QLabel *>("sqlScriptPolicy");
        QTRY_VERIFY_WITH_TIMEOUT(policy->text().contains("128 MiB"), 5000);
        QVERIFY(policy->text().contains("仅预览文件开头"));
        QCOMPARE(pane.editor->document()->firstBlock().text(), QString("SELECT 1;"));
        QVERIFY(pane.editor->isReadOnly());
        QVERIFY(!pane.scriptReady());
        QVERIFY(!pane.run->isEnabled());
        QVERIFY(!pane.saveSqlFile());
        QCOMPARE(QFileInfo(path).size(), qint64(navidog::editableScriptBytes) + 1);
    }

    void informationalDiagnosticsUseDistinctHighContrastMarking() {
      SqlEditor editor;editor.resize(900,400);editor.setDiagnosticContext(navidog::DatabaseKind::MySQL);editor.setPlainText("SHOW ENGINE INNODB STATUS");editor.show();
      QTRY_VERIFY_WITH_TIMEOUT(!editor.diagnostics().isEmpty(),4000);QVERIFY(!editor.diagnostics()[0].error);QVERIFY(!editor.diagnostics()[0].metadata);
      bool found=false;for(const auto &mark:editor.extraSelections())if(mark.format.underlineStyle()==QTextCharFormat::DotLine) {
        found=true;QCOMPARE(mark.format.underlineColor(),QColor("#a56309"));QCOMPARE(mark.format.background().color(),QColor("#fff4d6"));
      }
      QVERIFY(found);const auto image=qEnvironmentVariable("NAVIDOG_TEST_NOTE_SCREENSHOT");if(!image.isEmpty())QVERIFY(editor.grab().save(image));
    }
    void estimatedPlanModesShowRealRequestAndFlatNumericRows() {
      navidog::QueryResult raw;raw.columns={"plan"};raw.rows={{QString(R"P([{"Plan":{"Node Type":"Nested Loop","Plan Rows":20,"Total Cost":42,"Plans":[{"Node Type":"Seq Scan","Relation Name":"a","Plan Rows":100,"Filter":"customer_id > 1"},{"Node Type":"Index Scan","Relation Name":"b","Plan Rows":2,"Index Name":"b_idx"}]}}])P")}};
      auto plan=navidog::decodeEstimatedPlan(navidog::DatabaseKind::PostgreSQL,raw);QVERIFY(plan.complete);
      plan.request.issued=plan.request.received=true;plan.request.connectionName="owned server";plan.request.database="owned";plan.request.serverVersion="PostgreSQL 15";plan.request.commands={"EXPLAIN (FORMAT JSON, ANALYZE FALSE) SELECT * FROM a"};
      EstimatedPlanView view;view.resize(1200,600);view.setPlan(plan);view.show();
      auto *source=view.findChild<QLabel*>("estimatedPlanSource");QVERIFY(source->text().contains("已收到响应"));QVERIFY(source->text().contains("owned server"));QCOMPARE(source->textFormat(),Qt::PlainText);
      view.findChild<QToolButton*>("planTableMode")->click();auto *table=view.findChild<QTableView*>("estimatedPlanTable");QVERIFY(table && table->isVisible());QCOMPARE(table->model()->rowCount(),3);const auto image=qEnvironmentVariable("NAVIDOG_TEST_PLAN_SCREENSHOT");if(!image.isEmpty())QVERIFY(view.grab().save(image));
      QCOMPARE(table->model()->headerData(0,Qt::Horizontal).toString(),QString("算子"));
      QCOMPARE(table->model()->headerData(2,Qt::Horizontal).toString(),QString("估计行数"));
      auto *details=view.findChild<QPlainTextEdit*>("estimatedPlanDetails");auto *toggle=view.findChild<QToolButton*>("estimatedPlanToggleDetails");QVERIFY(details && details->isHidden());QVERIFY(toggle && !toggle->isChecked());
      toggle->click();QVERIFY(details->isVisible());toggle->click();QVERIFY(details->isHidden());
      table->sortByColumn(2,Qt::AscendingOrder);QCOMPARE(table->model()->index(0,1).data().toString(),QString("b"));
      auto *search=view.findChild<QLineEdit*>("estimatedPlanSearch");search->setText("CUSTOMER_ID");QCOMPARE(table->model()->rowCount(),1);table->setCurrentIndex(table->model()->index(0,3));QVERIFY(view.findChild<QPlainTextEdit*>("estimatedPlanDetails")->toPlainText().contains("customer_id"));
      view.findChild<QToolButton*>("planRawMode")->click();QVERIFY(view.findChild<QPlainTextEdit*>("estimatedPlanRaw")->isVisible());QVERIFY(view.findChild<QPlainTextEdit*>("estimatedPlanRaw")->toPlainText().contains("Node Type"));QVERIFY(!search->isEnabled());
      view.findChild<QToolButton*>("planRequestMode")->click();auto *request=view.findChild<QPlainTextEdit*>("estimatedPlanRequest");QVERIFY(request->isVisible());QVERIFY(request->toPlainText().contains("ANALYZE FALSE"));
      plan.request={};view.setPlan(plan);QVERIFY(source->text().contains("未附在线请求"));QVERIFY(!request->toPlainText().contains("实际交给驱动"));
      view.setPlan({});QCOMPARE(table->model()->rowCount(),0);QVERIFY(request->toPlainText().contains("没有在线计划"));
    }
    void estimatedPlanSearchFindsConditionsPreservesAncestorsAndSortsNumbers(){
        navidog::QueryResult raw;raw.columns={"plan"};raw.rows={{QString(R"PLAN([{"Plan":{"Node Type":"Nested Loop","Plans":[{"Node Type":"Seq Scan","Relation Name":"a","Plan Rows":100,"Filter":"customer_id > 1"},{"Node Type":"Index Scan","Relation Name":"b","Plan Rows":2,"Index Name":"b_idx"}]}}])PLAN")}};
        auto plan=navidog::decodeEstimatedPlan(navidog::DatabaseKind::PostgreSQL,raw);QVERIFY(plan.complete);
        EstimatedPlanView view;view.setPlan(plan);view.show();auto *search=view.findChild<QLineEdit*>("estimatedPlanSearch");auto *tree=view.findChild<QTreeView*>("estimatedPlanTree");QVERIFY(search && tree);
        auto root=tree->model()->index(0,0);QCOMPARE(tree->model()->rowCount(root),2);
        search->setText("CUSTOMER_ID");root=tree->model()->index(0,0);QCOMPARE(tree->model()->rowCount(root),1);
        tree->setCurrentIndex(tree->model()->index(0,0,root));QVERIFY(view.findChild<QPlainTextEdit*>("estimatedPlanDetails")->toPlainText().contains("customer_id"));
        search->setText("absent_marker");QCOMPARE(tree->model()->rowCount(),0);search->clear();
        tree->sortByColumn(2,Qt::AscendingOrder);root=tree->model()->index(0,0);QCOMPARE(tree->model()->index(0,1,root).data().toString(),QString("b / b_idx"));
        search->setText("b_idx");view.setPlan({});QVERIFY(search->text().isEmpty());QCOMPARE(tree->model()->rowCount(),0);
    }
    void explainButtonUsesPlanResultsWithoutExecutingSubmittedDelete() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        QSignalSpy done(&window, &MainWindow::queryFinished);
        auto *tabs = window.findChild<QTabWidget *>("queryTabs");
        auto *pane = qobject_cast<QueryPane *>(tabs->currentWidget());
        QVERIFY(pane);
        auto *request = pane->planView->findChild<QPlainTextEdit *>("estimatedPlanRequest");
        QVERIFY(request);
        const QString first = "SELECT '订单';\n";
        const QString second = "DELETE FROM orders\nWHERE id=1";
        pane->editor->setPlainText(first + second);
        auto cursor = pane->editor->textCursor();
        cursor.setPosition(first.size());
        pane->editor->setTextCursor(cursor);
        QTest::mouseClick(pane->explain, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 3000);
        QCOMPARE(pane->resultTabs->tabText(0), QString("执行计划"));
        QVERIFY(pane->model->rowCount() > 0);
        QVERIFY(request->toPlainText().contains(QRegularExpression("EXPLAIN QUERY PLAN\\s+DELETE")));
        QCOMPARE(pane->resultTabs->currentWidget(), static_cast<QWidget *>(pane->planView));
        QVERIFY(pane->planView->findChild<QTreeView *>("estimatedPlanTree")->model()->rowCount() > 0);

        pane->editor->setPlainText(second + ";\n" + first);
        cursor = pane->editor->textCursor();
        cursor.setPosition(second.size());
        cursor.setPosition(0, QTextCursor::KeepAnchor);
        pane->editor->setTextCursor(cursor);
        auto *explainAction = window.findChild<QAction *>("menuExplainSql");
        QVERIFY(explainAction);
        explainAction->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 3000);
        QVERIFY(request->toPlainText().contains("DELETE FROM orders\nWHERE id=1"));
        QVERIFY(!request->toPlainText().contains("订单"));

        pane->editor->setPlainText("SELECT count(*) FROM orders");
        QTest::mouseClick(pane->run, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 3, 3000);
        QCOMPARE(pane->model->index(0, 0).data().toInt(), 100000);
    }
    void queryTabsExecuteConcurrentlyRouteResultsAndStopOnlyTheirOwnWork(){
        QTemporaryDir directory;MainWindow window(directory.path());window.show();QSignalSpy finished(&window,&MainWindow::queryFinished);auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* first=tabs->currentWidget();first->findChild<SqlEditor*>("sqlEditor")->setPlainText("WITH RECURSIVE t(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM t WHERE x<1000000000) SELECT sum(x) FROM t");QTest::mouseClick(first->findChild<QToolButton*>("runQuery"),Qt::LeftButton);QVERIFY(first->findChild<QToolButton*>("stopQuery")->isEnabled());QVERIFY(!first->findChild<QToolButton*>("runStatement")->isEnabled());
        QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);auto* second=tabs->currentWidget();QVERIFY(first!=second);auto* run=second->findChild<QToolButton*>("runQuery");QVERIFY(run->isEnabled());second->findChild<SqlEditor*>("sqlEditor")->setPlainText("SELECT 47 AS owned_result");QTest::mouseClick(run,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,3000);auto* grid=second->findChild<QTableView*>("resultTable");QCOMPARE(grid->model()->index(0,0).data().toInt(),47);QVERIFY(first->findChild<QToolButton*>("stopQuery")->isEnabled());
        tabs->setCurrentWidget(first);QTest::mouseClick(first->findChild<QToolButton*>("stopQuery"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,3000);QVERIFY(first->findChild<QPlainTextEdit*>("queryMessages")->toPlainText().contains("停止"));QCOMPARE(grid->model()->index(0,0).data().toInt(),47);QVERIFY(second->findChild<QToolButton*>("runQuery")->isEnabled());
    }
    void objectBrowserFiltersOpensDesignsAndRestoresWithoutConnecting(){
        QTemporaryDir directory;
        {
            MainWindow window(directory.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* connection=tree->topLevelItem(0);connection->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(connection->childCount(),1,3000);
            auto* database=connection->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);auto* folder=database->child(0);
            QMetaObject::invokeMethod(tree,"itemDoubleClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,folder),Q_ARG(int,0));
            auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<ObjectsPane*>(tabs->currentWidget());QVERIFY(pane);auto* list=pane->findChild<QTableView*>("objectList");QTRY_COMPARE(list->model()->rowCount(),2);
            auto* filter=pane->findChild<QLineEdit*>("objectListFilter");filter->setText("orders");QCOMPARE(list->model()->rowCount(),1);list->setCurrentIndex(list->model()->index(0,2));QCOMPARE(pane->selectedNode().object,QString("orders"));
            QTest::mouseClick(pane->findChild<QToolButton*>("designBrowserObject"),Qt::LeftButton);auto* design=dynamic_cast<TablePane*>(tabs->currentWidget());QVERIFY(design);QTRY_VERIFY_WITH_TIMEOUT(design->isLoaded(),3000);
            tabs->setCurrentWidget(pane);QTest::mouseClick(pane->findChild<QToolButton*>("openBrowserObject"),Qt::LeftButton);auto* data=dynamic_cast<DataPane*>(tabs->currentWidget());QVERIFY(data);QTRY_VERIFY_WITH_TIMEOUT(data->isLoaded(),3000);
            tabs->setCurrentWidget(pane);bool generated=false;QTimer::singleShot(30,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* action:menu->actions())if(action->menu() && action->text()=="生成 SQL"){for(auto* sql:action->menu()->actions())if(sql->text()=="SELECT"){sql->trigger();generated=true;break;}}menu->close();});
            pane->contextRequested(pane->selectedNode(),QPoint(100,100));QVERIFY(generated);QTRY_VERIFY_WITH_TIMEOUT(tabs->currentWidget()->findChild<SqlEditor*>("sqlEditor")!=nullptr,3000);auto* draft=tabs->currentWidget()->findChild<SqlEditor*>("sqlEditor");QVERIFY(draft->toPlainText().contains("orders"));auto* result=tabs->currentWidget()->findChild<QTableView*>("resultTable");if(result)QCOMPARE(result->model()->rowCount(),0);
            tabs->setCurrentWidget(pane);window.close();
        }
        MainWindow restored(directory.path());auto* tabs=restored.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<ObjectsPane*>(tabs->currentWidget());QVERIFY(pane);QCOMPARE(pane->folder.kind,QString("tables"));QCOMPARE(pane->findChild<QTableView*>("objectList")->model()->rowCount(),0);QCOMPARE(restored.findChild<QTreeWidget*>("connectionTree")->topLevelItem(0)->childCount(),0);
    }
    void metadataControllerScopesSnapshotsAndClearsRemovedProfiles() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = localStorePath(directory.path());
        QVector<ConnectionProfile> profiles;
        {
            MetadataStore store(path);
            for (const auto &name : {QString("first"), QString("second")}) {
                ConnectionProfile profile;
                profile.id = profile.name = name;
                profile.database = directory.filePath(name + ".sqlite");
                profiles << profile;

                MetadataSnapshot snapshot;
                snapshot.key = metadataKey(profile, profile.database, {});
                snapshot.profileId = profile.id;
                snapshot.identity = metadataIdentity(profile);
                snapshot.database = profile.database;
                snapshot.driver = profile.driver;
                snapshot.refreshedMs = QDateTime::currentMSecsSinceEpoch();
                store.replace(snapshot);
            }
        }

        QueryService service;
        QTabWidget tabs;
        MetadataController metadata(service, tabs, profiles, path, false);
        auto *pane = new QueryPane(profiles, WorkspaceTabKind::Query);
        tabs.addTab(pane, "Query");
        pane->database = "main";
        pane->profileId = profiles[0].id;
        metadata.updateContext(*pane);
        pane->profileId = profiles[1].id;
        metadata.updateContext(*pane);

        QTRY_VERIFY(metadata.snapshot(profiles[0], "main", {}));
        QTRY_VERIFY(metadata.snapshot(profiles[1], "main", {}));
        QCOMPARE(metadata.snapshot(profiles[1], "main", {})->database, profiles[1].database);

        metadata.removeProfile(profiles[0].id);
        QVERIFY(!metadata.snapshot(profiles[0], "main", {}));
        QVERIFY(metadata.snapshot(profiles[1], "main", {}));
        QCOMPARE(service.sessionCount(), 0);
    }
    void queryTabsOwnTransactionsAndShowMultipleResults(){
        QTemporaryDir dir;MainWindow window(dir.path());window.show();auto* editor=window.findChild<SqlEditor*>();QVERIFY(editor);editor->setPlainText("SELECT 1 AS first; SELECT 2 AS second;");QSignalSpy done(&window,&MainWindow::queryFinished);window.runCurrent();QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);auto* results=window.findChild<QTabWidget*>("resultTabs");QCOMPARE(results->count(),7);QCOMPARE(results->tabText(1),QString("结果 2"));auto* second=qobject_cast<QTableView*>(results->widget(1));QCOMPARE(second->model()->index(0,0).data(Qt::UserRole).toInt(),2);
        auto* begin=window.findChild<QToolButton*>("beginTransaction");auto* rollback=window.findChild<QToolButton*>("rollbackTransaction");QVERIFY(begin->isEnabled());begin->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QVERIFY(rollback->isEnabled());QCOMPARE(results->count(),7);QVERIFY(!begin->isEnabled());rollback->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,3000);QVERIFY(!rollback->isEnabled());QCOMPARE(results->count(),7);QVERIFY(begin->isEnabled());
        auto* current=window.findChild<QToolButton*>("runStatement");editor->setPlainText("SELECT 3; SELECT 4;");auto cursor=editor->textCursor();cursor.setPosition(14);editor->setTextCursor(cursor);current->click();QTRY_COMPARE_WITH_TIMEOUT(done.count(),4,3000);auto* first=window.findChild<QTableView*>("resultTable");QCOMPARE(first->model()->index(0,0).data(Qt::UserRole).toInt(),4);
    }
    void objectListSourceEditorKeepsWorkspaceTabsInteractive() {
        using namespace navidog;
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        window.activateWindow();
        ConnectionProfile profile;
        profile.database=QDir(directory.path()).filePath("harbor-demo.db");
        auto db=connectDatabase(profile);
        const auto token=cancellation();
        const auto created=db->execute("CREATE VIEW owned_editor_view AS SELECT 1 AS id",{},token);
        QVERIFY2(created.error.isEmpty(),qPrintable(created.error));
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        auto* service=window.findChild<navidog::QueryService*>();
        auto* root=tree->topLevelItem(0);
        root->setExpanded(true);
        QTRY_VERIFY(root->childCount()>0);
        auto* database=root->child(0);
        database->setExpanded(true);
        QTRY_VERIFY(database->childCount()>0);
        QTreeWidgetItem* views=nullptr;
        for(int i=0;i<database->childCount();++i) {
            auto* item=database->child(i);
            if(item->data(0,Qt::UserRole+2).value<CatalogNode>().kind=="views")views=item;
        }
        QVERIFY(views);
        views->setExpanded(true);
        QTRY_VERIFY(views->childCount()>0 && !service->busy());
        tree->setCurrentItem(views);
        window.findChild<QToolButton*>("browse_views")->click();
        auto* browser=tabs->currentWidget();
        auto* list=browser->findChild<QTableView*>("objectList");
        QVERIFY(list);
        QModelIndex view;
        for(int i=0;i<list->model()->rowCount();++i) {
            auto index=list->model()->index(i,0);
            if(index.data().toString()=="owned_editor_view")view=index;
        }
        QVERIFY(view.isValid());
        const auto point=list->visualRect(view).center();
        QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QTRY_VERIFY(browser->findChild<QToolButton*>("openBrowserObject")->isEnabled() && !service->busy());
        QSignalSpy doubleClicked(list,&QTableView::doubleClicked);
        QTest::mouseDClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QCOMPARE(doubleClicked.count(),1);
        QCOMPARE(tabs->currentWidget(),browser);
        QTest::mouseRelease(list->viewport(),Qt::LeftButton,Qt::NoModifier,point);
        QTRY_VERIFY(dynamic_cast<DataPane*>(tabs->currentWidget()) && !service->busy());

        const auto clickTab=[&](QWidget* target) {
            auto* tabBar=tabs->tabBar();
            const auto rect=tabBar->tabRect(tabs->indexOf(target));
            QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,
                              tabBar->mapTo(&window,QPoint(rect.left()+15,rect.center().y())));
        };
        clickTab(browser);
        QCOMPARE(tabs->currentWidget(),browser);
        auto* design=browser->findChild<QToolButton*>("designBrowserObject");
        QVERIFY(design->isEnabled());
        QTest::mouseClick(design,Qt::LeftButton);
        QTRY_VERIFY(dynamic_cast<SourcePane*>(tabs->currentWidget()));
        QPointer<SourcePane> pane=static_cast<SourcePane*>(tabs->currentWidget());
        QTRY_VERIFY(pane && pane->loaded && !service->busy());
        clickTab(browser);
        QCOMPARE(tabs->currentWidget(),browser);
        clickTab(pane);
        QCOMPARE(tabs->currentWidget(),pane.data());

        const auto dropped=db->execute("DROP VIEW owned_editor_view",{},token);
        QVERIFY2(dropped.error.isEmpty(),qPrintable(dropped.error));
        QSignalSpy finished(service,&navidog::QueryService::sourceDesignCompleted);
        pane->reload->click();
        QVERIFY(pane->loading);
        QTRY_COMPARE(finished.count(),1);
        QVERIFY(!QApplication::activeModalWidget());
        const auto* popUp=window.findChild<QDialog*>("databaseErrorDialog");
        QVERIFY(!popUp || !popUp->isVisible());
        QVERIFY(!pane->message->text().isEmpty());
        QVERIFY(pane->reload->isEnabled());

        pane->reload->click();
        QVERIFY(pane->loading);
        emit tabs->tabCloseRequested(tabs->indexOf(pane));
        QTRY_VERIFY(pane.isNull());
        QTRY_VERIFY(!service->busy());
        clickTab(browser);
        QCOMPARE(tabs->currentWidget(),browser);
    }
    void viewDefinitionEditorSavesAndRestoresItsDraft(){
        using namespace navidog;QTemporaryDir own;{MainWindow window(own.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);QTreeWidgetItem* views=nullptr;for(int i=0;i<database->childCount();++i)if(database->child(i)->data(0,Qt::UserRole+2).value<CatalogNode>().kind=="views")views=database->child(i);QVERIFY(views);tree->setCurrentItem(views);bool invoked=false;QTimer::singleShot(40,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* action:menu->actions())if(action->text()=="新建视图…"){action->trigger();invoked=true;break;}menu->close();});QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,tree->visualItemRect(views).center()));QVERIFY(invoked);auto* pane=window.findChild<SourcePane*>("sourceDesigner");QVERIFY(pane);pane->name->setText("view_ui_owned");QVERIFY(pane->editor->toPlainText().contains("view_ui_owned"));QSignalSpy done(&window,&MainWindow::queryFinished);pane->save->click();QTRY_VERIFY_WITH_TIMEOUT(pane->baseline.exists,3000);QVERIFY(pane->loaded);auto definition=pane->editor->toPlainText();definition.replace("SELECT 1 AS id","SELECT 3 AS id");pane->editor->setPlainText(definition);QVERIFY(pane->dirty());QTest::qWait(50);QTRY_VERIFY(pane->reload->isEnabled());window.close();QVERIFY(!window.isVisible());}
        MainWindow restored(own.path());auto* pane=restored.findChild<SourcePane*>("sourceDesigner");QVERIFY(pane);QVERIFY(pane->dirty());QVERIFY(pane->editor->toPlainText().contains("SELECT 3 AS id"));ConnectionProfile p;p.database=QDir(own.path()).filePath("harbor-demo.db");auto db=connectDatabase(p);QCOMPARE(db->execute("SELECT id FROM view_ui_owned",{},cancellation()).rows[0][0].toInt(),1);
    }
    void runRealQueryAndOpenTable() {
        QTemporaryDir directory; MainWindow window(directory.path()); window.show();
        auto* run = window.findChild<QToolButton*>("runQuery"); QVERIFY(run);
        QSignalSpy finished(&window, &MainWindow::queryFinished);
        QTest::mouseClick(run, Qt::LeftButton);
        QVERIFY(!run->isEnabled()); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        auto* table = window.findChild<QTableView*>("resultTable"); QVERIFY(table);
        QCOMPARE(table->model()->rowCount(), 100); QCOMPARE(table->model()->columnCount(), 6); QVERIFY(run->isEnabled());
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QVERIFY(tree); QCOMPARE(tree->topLevelItemCount(), 1);
        auto* root=tree->topLevelItem(0); QCOMPARE(root->childCount(),0);
        root->setExpanded(true); QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);
        auto* database=root->child(0); QCOMPARE(database->text(0),QString("main"));
        database->setExpanded(true); QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);
        auto* tables=database->child(0); tables->setExpanded(true); QTRY_COMPARE_WITH_TIMEOUT(tables->childCount(),2,3000);
        auto* orders=tables->child(1); QCOMPARE(orders->text(0),QString("orders"));
        orders->setExpanded(true); QTRY_COMPARE_WITH_TIMEOUT(orders->childCount(),5,3000);
        auto* columns=orders->child(0); columns->setExpanded(true); QTRY_VERIFY_WITH_TIMEOUT(columns->childCount()>0,3000);
        QVERIFY(columns->child(0)->text(0).startsWith("id INTEGER"));
        QVERIFY(columns->child(0)->icon(0).isNull());
        if(!qEnvironmentVariableIsEmpty("NAVIDOG_UI_SCREENSHOT")) QVERIFY(window.grab().save(qEnvironmentVariable("NAVIDOG_UI_SCREENSHOT")));
        auto* tabs = window.findChild<QTabWidget*>("queryTabs"); QVERIFY(tabs);
        QTest::mouseClick(window.findChild<QToolButton*>("newQuery"), Qt::LeftButton); QCOMPARE(tabs->count(), 2);
        run=tabs->currentWidget()->findChild<QToolButton*>("runQuery");
        auto* editor = tabs->currentWidget()->findChild<SqlEditor*>(); QVERIFY(editor); editor->setPlainText("SELECT NULL AS n, 'hello' AS greeting");
        QTest::mouseClick(run, Qt::LeftButton); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 3000);
        auto* second = tabs->currentWidget()->findChild<QTableView*>(); QCOMPARE(second->model()->index(0, 0).data().toString(), QString("NULL"));
        QCOMPARE(second->model()->index(0, 1).data().toString(), QString("hello"));
        editor->setPlainText("select 1 from orders where id = 1");
        QTest::mouseClick(tabs->currentWidget()->findChild<QToolButton*>("formatQuery"), Qt::LeftButton); QTRY_VERIFY(editor->toPlainText().contains('\n'));
        editor->setPlainText("SELECT * FROM not_a_table"); QTest::mouseClick(run, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 3000);
        auto* messages = tabs->currentWidget()->findChild<QPlainTextEdit*>("queryMessages"); QVERIFY(messages->toPlainText().contains("执行失败"));
        auto* error=window.findChild<QDialog*>("databaseErrorDialog");QVERIFY(error);QVERIFY(error->isVisible());QVERIFY(error->findChild<QPlainTextEdit*>("databaseErrorDetails")->toPlainText().contains("not_a_table"));
        auto* copy=error->findChild<QPushButton*>("copyDatabaseError");QVERIFY(copy);copy->click();
        QTRY_VERIFY(QApplication::clipboard()->text().contains("not_a_table"));
    }
    void groupsCanBeCreatedRenamedAndMoved() {
        QTemporaryDir directory; MainWindow window(directory.path()); window.show();
        auto* create=window.findChild<QAction*>("menuNewGroup"); QVERIFY(create);
        QTimer::singleShot(30,&window,[] {
            if (auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { dialog->setTextValue("Development"); dialog->accept(); }
        });
        create->trigger();
        navidog::ProfileStore store(directory.filePath("navidog.sqlite"));
        auto saved=store.loadWorkspace(); QCOMPARE(saved.groups.size(),1); QCOMPARE(saved.groups[0].name,QString("Development"));
        auto* tree=window.findChild<QTreeWidget*>("connectionTree"); QCOMPARE(tree->topLevelItemCount(),2);
        auto* profile=tree->topLevelItem(1); tree->setCurrentItem(profile);
        bool moved=false;
        QTimer::singleShot(30,&window,[&] {
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget()); if(!menu) return;
            for(auto* action:menu->findChildren<QMenu*>()) if(action->title()=="移动到分组") {
                for(auto* target:action->actions()) if(target->text()=="Development") { target->trigger(); moved=true; break; }
                break;
            }
            menu->close();
        });
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,tree->visualItemRect(profile).center()));
        QVERIFY(moved); saved=store.loadWorkspace(); QCOMPARE(saved.connections[0].groupId,saved.groups[0].id);
        QCOMPARE(tree->topLevelItemCount(),1); auto* group=tree->topLevelItem(0); QCOMPARE(group->childCount(),1); tree->setCurrentItem(group);
        bool renamed=false;
        QTimer::singleShot(30,&window,[&] {
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget()); if(!menu) return;
            for(auto* action:menu->actions()) if(action->text()=="重命名分组…") {
                QTimer::singleShot(30,&window,[&] {
                    if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {dialog->setTextValue("Production");dialog->accept();renamed=true;}
                }); action->trigger();break;
            }
            menu->close();
        });
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,tree->visualItemRect(group).center()));
        QTRY_VERIFY(renamed); QCOMPARE(store.loadWorkspace().groups[0].name,QString("Production"));
        MainWindow restored(directory.path()); auto* restoredTree=restored.findChild<QTreeWidget*>("connectionTree");
        QCOMPARE(restoredTree->topLevelItemCount(),1); QCOMPARE(restoredTree->topLevelItem(0)->text(0),QString("Production")); QCOMPARE(restoredTree->topLevelItem(0)->childCount(),1);

        bool deleted = false;
        QTimer::singleShot(30, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions()) {
                if (action->text() != "删除分组（保留连接）") continue;
                action->trigger();
                deleted = true;
                break;
            }
            menu->close();
        });
        group = tree->topLevelItem(0);
        QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, tree->visualItemRect(group).center()));
        QVERIFY(deleted);
        saved = store.loadWorkspace();
        QVERIFY(saved.groups.isEmpty());
        QCOMPARE(saved.connections.size(), 1);
        QVERIFY(saved.connections[0].groupId.isEmpty());
        QCOMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->text(0), saved.connections[0].name);
    }

    void navigationDragSortsGroupsAndMovesConnections() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ProfileStore store(localStorePath(directory.path()));
        WorkspaceProfiles workspace;
        workspace.groups = {{"g1", "Group 1"}, {"g2", "Group 2"}};
        auto add = [&](const QString &id, const QString &group) {
            ConnectionProfile profile;
            profile.id = profile.name = id;
            profile.groupId = group;
            profile.database = directory.filePath(id + ".sqlite");
            workspace.connections.push_back(profile);
        };
        add("a", "g1");
        add("x", {});
        add("other", "g2");
        add("b", "g1");
        add("y", {});
        store.saveWorkspace(workspace);

        const auto profileOrder = [](const WorkspaceProfiles &state, const QString &group) {
            QStringList ids;
            for (const auto &profile : state.connections)
                if (profile.groupId == group)
                    ids << profile.id;
            return ids;
        };

        {
            MainWindow window(directory.path());
            window.show();
            auto *tree = window.findChild<QTreeWidget *>("connectionTree");
            QVERIFY(tree);
            QCOMPARE(tree->topLevelItemCount(), 5);
            auto *group1 = tree->topLevelItem(0);
            auto *group2 = tree->topLevelItem(1);
            auto *demo = tree->topLevelItem(2);
            auto *x = tree->topLevelItem(3);
            auto *y = tree->topLevelItem(4);
            auto *a = group1->child(0);
            auto *b = group1->child(1);
            QVERIFY(a && b);
            const auto drag = [&](QTreeWidgetItem *source, QTreeWidgetItem *target, bool after) {
                tree->setCurrentItem(source);
                auto *view = static_cast<navidog::ui::ObjectTree *>(tree);
                std::unique_ptr<QMimeData> data(tree->model()->mimeData({view->indexFromItem(source)}));
                if (!data)
                    return false;
                QPoint point(100, tree->viewport()->height() - 30);
                if (target) {
                    const auto rect = tree->visualItemRect(target);
                    point = {rect.center().x(), after ? rect.bottom() - 1 : rect.top() + 1};
                }
                QDragEnterEvent enter(point, Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &enter);
                if (!enter.isAccepted())
                    return false;
                QDragMoveEvent move(point, Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &move);
                QDropEvent drop(QPointF(point), Qt::MoveAction, data.get(), Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tree->viewport(), &drop);
                return move.isAccepted() && drop.isAccepted();
            };

            QVERIFY(!drag(group2, x, true));
            QVERIFY(drag(group1, group2, true));
            QCOMPARE(tree->topLevelItem(0), group2);
            QCOMPARE(tree->topLevelItem(1), group1);
            QVERIFY(group1->isExpanded());
            QVERIFY(drag(b, a, false));
            QCOMPARE(group1->child(0), b);

            demo->setExpanded(true);
            QTRY_VERIFY(demo->childCount() > 0);
            auto *database = demo->child(0);
            auto *view = static_cast<navidog::ui::ObjectTree *>(tree);
            const QPersistentModelIndex databaseIndex(view->indexFromItem(database));
            QVERIFY(!drag(database, x, false));
            QVERIFY(drag(y, demo, false));
            QCOMPARE(tree->topLevelItem(0), group2);
            QCOMPARE(tree->topLevelItem(1), group1);
            QCOMPARE(tree->topLevelItem(2), y);
            QCOMPARE(tree->topLevelItem(3), demo);
            QVERIFY(demo->isExpanded());
            QCOMPARE(view->itemFromIndex(databaseIndex), database);
            QCOMPARE(tree->currentItem(), y);

            auto *other = group2->child(0);
            QVERIFY(drag(b, other, false));
            QCOMPARE(group1->childCount(), 1);
            QCOMPARE(group2->child(0), b);
            QCOMPARE(group2->child(1), other);

            group2->setExpanded(false);
            QVERIFY(drag(demo, group2, false));
            QVERIFY(group2->isExpanded());
            QVERIFY(demo->isExpanded());
            QCOMPARE(demo->child(0), database);
            QCOMPARE(group2->child(2), demo);

            QVERIFY(drag(b, y, false));
            QCOMPARE(tree->topLevelItem(2), b);
            QVERIFY(drag(demo, nullptr, true));
            QCOMPARE(tree->topLevelItem(5), demo);
            QVERIFY(demo->isExpanded());
            QCOMPARE(demo->child(0), database);
        }

        const auto saved = store.loadWorkspace();
        QCOMPARE(saved.groups[0].id, QString("g2"));
        QCOMPARE(saved.groups[1].id, QString("g1"));
        QCOMPARE(profileOrder(saved, {}), (QStringList{"b", "y", "x", "harbor-demo"}));
        QCOMPARE(profileOrder(saved, "g1"), (QStringList{"a"}));
        QCOMPARE(profileOrder(saved, "g2"), (QStringList{"other"}));

        MainWindow restored(directory.path());
        auto *tree = restored.findChild<QTreeWidget *>("connectionTree");
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("Group 2"));
        QCOMPARE(tree->topLevelItem(1)->child(0)->text(0), QString("a"));
        QCOMPARE(tree->topLevelItem(2)->text(0), QString("b"));
    }


    void tableMenusRenameKeepDataContextAndRequireTypedDropConfirmation() {
        QTemporaryDir directory;navidog::ConnectionProfile p;p.database=navidog::ensureDemoDatabase(directory.path());auto db=navidog::connectDatabase(p);db->execute("CREATE TABLE ui_actions(id INTEGER PRIMARY KEY,label TEXT)",{},navidog::cancellation());db->execute("INSERT INTO ui_actions VALUES(1,'owned')",{},navidog::cancellation());
        MainWindow window(directory.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);auto* folder=database->child(0);folder->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(folder->childCount(),3,3000);auto* object=folder->child(2);QCOMPARE(object->text(0),QString("ui_actions"));tree->scrollToItem(object);QSignalSpy loaded(&window,&MainWindow::queryFinished);tree->itemDoubleClicked(object,0);QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,3000);auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* data=dynamic_cast<DataPane*>(tabs->currentWidget());QVERIFY(data);QVERIFY(data->isLoaded());
        bool renamed=false;
        QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="重命名…"){QTimer::singleShot(20,&window,[&]{auto* dialog=window.findChild<TableTaskDialog*>("tableTaskDialog");if(!dialog)return;dialog->findChild<QLineEdit*>("taskTargetName")->setText("ui_renamed");dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();renamed=true;});a->trigger();break;}menu->close();});
        QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(object).center());QTRY_VERIFY(renamed);QCOMPARE(data->tabTitle(), QString("ui_renamed · 数据"));QCOMPARE(tabs->tabText(tabs->indexOf(data)), data->tabTitle());QVERIFY(!data->isLoaded());QTRY_COMPARE_WITH_TIMEOUT(folder->child(2)->text(0),QString("ui_renamed"),3000);auto *refresh = data->findChild<QToolButton *>("refreshData");QVERIFY(refresh);QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),3000);QTest::mouseClick(refresh,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),2,3000);QVERIFY(data->isLoaded());QCOMPARE(data->cachedRows().rows.size(),1);
        // Refreshing tables must not change the database's open/close state.
        bool hasClose=false;QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="关闭数据库")hasClose=true;menu->close();});QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(database).center());QTRY_VERIFY(hasClose);
        object=folder->child(2);QCOMPARE(object->text(0),QString("ui_renamed"));tree->scrollToItem(object);bool confirmed=false;
        QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="删除表…"){QTimer::singleShot(20,&window,[&]{auto* dialog=window.findChild<TableTaskDialog*>("tableTaskDialog");if(!dialog)return;auto* ok=dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);QVERIFY(!ok->isEnabled());dialog->findChild<QLineEdit*>("taskConfirmName")->setText("ui_renamed");QVERIFY(ok->isEnabled());ok->click();confirmed=true;});a->trigger();break;}menu->close();});
        QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(object).center());QTRY_VERIFY(confirmed);QVERIFY(!db->tables().contains("ui_renamed"));QCOMPARE(tabs->count(),1);
    }
    void tableGroupingAndFilteredViewConfigurationsPersistLocally() {
        QTemporaryDir directory;MainWindow window(directory.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);auto* folder=database->child(0);folder->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(folder->childCount(),2,3000);auto* customers=folder->child(0);tree->scrollToItem(customers);
        QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="管理组"){QTimer::singleShot(20,&window,[]{if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())){dialog->setTextValue("Owned group");dialog->accept();}});a->menu()->actions()[0]->trigger();break;}menu->close();});
        QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(customers).center());QTRY_COMPARE(folder->child(0)->text(0),QString("Owned group"));auto* group=folder->child(0);QCOMPARE(group->childCount(),1);customers=group->child(0);QCOMPARE(customers->text(0),QString("customers"));group->setExpanded(true);tree->scrollToItem(customers);
        tree->itemDoubleClicked(customers,0);auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* data=dynamic_cast<DataPane*>(tabs->currentWidget());QVERIFY(data);QTRY_VERIFY_WITH_TIMEOUT(data->isLoaded(),3000);
        auto *column = data->findChild<QComboBox *>("dataFilterColumn");auto *operation = data->findChild<QComboBox *>("dataFilterOperator");column->setCurrentIndex(column->findData(0));operation->setCurrentIndex(operation->findData(int(navidog::DataFilter::Equal)));data->findChild<QLineEdit *>("dataFilterValue")->setText("1");QTest::mouseClick(data->findChild<QToolButton *>("applyDataFilter"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(data->cachedRows().rows.size(),1,3000);
        bool saved=false;QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="通过配置文件打开表"){for(auto* child:a->menu()->actions())if(child->text()=="保存当前筛选为配置…"){QTimer::singleShot(20,&window,[]{if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())){dialog->setTextValue("Only customer 1");dialog->accept();}});child->trigger();saved=true;break;}break;}menu->close();});QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(customers).center());QTRY_VERIFY(saved);
        navidog::LocalStore settings(navidog::localStorePath(directory.path()));auto stored=settings.document("table-views");QVERIFY(stored);auto bytes=*stored;QVERIFY(bytes.contains("Owned group"));QVERIFY(bytes.contains("Only customer 1"));QVERIFY(!bytes.contains("password"));
        bool opened=false;QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="通过配置文件打开表"){for(auto* child:a->menu()->actions())if(child->text()=="Only customer 1"){child->trigger();opened=true;break;}break;}menu->close();});QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(customers).center());QTRY_VERIFY(opened);auto* restored=dynamic_cast<DataPane*>(tabs->currentWidget());QVERIFY(restored!=data);QTRY_VERIFY_WITH_TIMEOUT(restored->isLoaded(),3000);QCOMPARE(restored->viewSettings()["filter"].toInt(), int(navidog::DataFilter::Equal));QCOMPARE(restored->cachedRows().rows.size(),1);

        bool deleted = false;
        QTimer::singleShot(20, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions()) {
                if (action->text() != "删除组（保留表）") continue;
                action->trigger();
                deleted = true;
                break;
            }
            menu->close();
        });
        QTest::mouseClick(tree->viewport(), Qt::RightButton, Qt::NoModifier,
                          tree->visualItemRect(group).center());
        QTRY_VERIFY(deleted);
        const auto afterDelete = settings.document("table-views");
        QVERIFY(afterDelete);
        QVERIFY(!afterDelete->contains("Owned group"));
        QVERIFY(afterDelete->contains("Only customer 1"));
        QCOMPARE(folder->child(0)->text(0), QString("customers"));
        QCOMPARE(restored->cachedRows().rows.size(), 1);
    }
    void navigationMultiSelectStaysInsideOneDirectory() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.database=ensureDemoDatabase(directory.path());
        auto db=connectDatabase(profile);
        auto token=cancellation();
        for (const auto* name : {"choose_a","choose_b","choose_c"})
            QVERIFY(db->execute(QString("CREATE TABLE %1(id INTEGER)").arg(name),{},token).error.isEmpty());
        QVERIFY(db->execute("CREATE VIEW choose_other AS SELECT 1",{},token).error.isEmpty());
        MainWindow window(directory.path());
        window.resize(1100,900);
        window.show();
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");
        QVERIFY(tree);
        auto* connection=tree->topLevelItem(0);
        connection->setExpanded(true);
        QTRY_COMPARE(connection->childCount(),1);
        auto* database=connection->child(0);
        database->setExpanded(true);
        QTRY_COMPARE(database->childCount(),3);
        auto* tables=database->child(0);
        auto* views=database->child(1);
        tables->setExpanded(true);
        views->setExpanded(true);
        QTRY_VERIFY(views->childCount()>0);
        const auto pick=[&](QTreeWidgetItem* parent,const QString& name) -> QTreeWidgetItem* {
            for (int i=0;i<parent->childCount();++i)
                if (parent->child(i)->text(0)==name) return parent->child(i);
            return nullptr;
        };
        QTRY_VERIFY(pick(tables,"choose_c") && pick(views,"choose_other"));
        auto* a=pick(tables,"choose_a");
        auto* b=pick(tables,"choose_b");
        auto* c=pick(tables,"choose_c");
        auto* other=pick(views,"choose_other");
        const auto click=[&](QTreeWidgetItem* item, Qt::KeyboardModifiers modifiers) {
            tree->scrollToItem(item);
            QTest::mouseClick(tree->viewport(),Qt::LeftButton,modifiers,tree->visualItemRect(item).center());
        };
        click(a,Qt::NoModifier);
        click(c,Qt::ControlModifier);
        QCOMPARE(tree->selectedItems().size(),2);
        const auto selectedNames=[&] {
            QStringList names;
            for (auto* selected : tree->selectedItems()) names << selected->text(0);
            names.sort();
            return names;
        };
        QCOMPARE(selectedNames(), QStringList({"choose_a","choose_c"}));
        click(other,Qt::ControlModifier);
        QCOMPARE(selectedNames(), QStringList({"choose_a","choose_c"}));
        click(a,Qt::NoModifier);
        click(c,Qt::ShiftModifier);
        QCOMPARE(selectedNames(), QStringList({"choose_a","choose_b","choose_c"}));
        click(other,Qt::ShiftModifier);
        QCOMPARE(selectedNames(), QStringList({"choose_a","choose_b","choose_c"}));

        bool batchMenu = false;
        QTimer::singleShot(40,&window,[&] {
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            auto* generate=menu->findChild<QMenu*>("contextGenerateBatchSql");
            batchMenu=generate && menu->findChild<QMenu*>("contextExportBatchSql") &&
                menu->findChild<QAction*>("contextBatchDrop");
            if (batchMenu)
                for (auto* action : generate->actions())
                    if (action->text()=="SELECT") { action->trigger(); break; }
            menu->close();
        });
        tree->scrollToItem(b);
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,
                                  Q_ARG(QPoint,tree->visualItemRect(b).center()));
        QVERIFY(batchMenu);
        QCOMPARE(tree->selectedItems().size(),3);
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");
        QVERIFY(tabs);
        QTRY_VERIFY(qobject_cast<QueryPane*>(tabs->currentWidget()) &&
                    qobject_cast<QueryPane*>(tabs->currentWidget())->editor->toPlainText().contains("choose_c"));
        const auto draft=qobject_cast<QueryPane*>(tabs->currentWidget())->editor->toPlainText();
        QVERIFY(draft.contains("choose_a") && draft.contains("choose_b"));
    }

    void navigationObjectPropertiesAndConfirmedDropUseTheSelectedView() {
        using namespace navidog;
        QTemporaryDir directory;
        auto token = cancellation();
        ConnectionProfile profile;
        profile.database = ensureDemoDatabase(directory.path());
        auto db = connectDatabase(profile);
        QVERIFY(db->execute("CREATE VIEW object_properties_owned AS SELECT 93 AS number", {}, token).error.isEmpty());
        QVERIFY(db->execute("CREATE VIEW object_preserved_owned AS SELECT 95 AS number", {}, token).error.isEmpty());
        MainWindow window(directory.path());
        window.show();
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        QVERIFY(tree);
        auto* connection = tree->topLevelItem(0);
        connection->setExpanded(true);
        QTRY_COMPARE(connection->childCount(), 1);
        auto* database = connection->child(0);
        database->setExpanded(true);
        QTRY_COMPARE(database->childCount(), 3);
        auto viewFolder = database->child(1);
        QCOMPARE(viewFolder->data(0, Qt::UserRole + 2).value<CatalogNode>().kind, QString("views"));
        viewFolder->setExpanded(true);
        const auto findTarget = [&]() -> QTreeWidgetItem* {
            for (int i = 0; i < viewFolder->childCount(); ++i)
                if (viewFolder->child(i)->text(0) == "object_properties_owned") return viewFolder->child(i);
            return nullptr;
        };
        QTRY_VERIFY(findTarget());
        const auto invokeMenu = [&](const QString& id) {
            bool invoked = false;
            QTimer::singleShot(40, &window, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) return;
                if (auto* action = menu->findChild<QAction*>(id); action && action->isEnabled()) {
                    invoked = true;
                    action->trigger();
                }
                menu->close();
            });
            QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, tree->visualItemRect(findTarget()).center()));
            return invoked;
        };
        QVERIFY(invokeMenu("contextObjectProperties"));
        auto* properties = window.findChild<QDialog*>("objectPropertiesDialog");
        QVERIFY(properties);
        auto* sql = properties->findChild<QPlainTextEdit*>("objectPropertiesSql");
        QVERIFY(sql);
        QTRY_VERIFY(sql->toPlainText().contains("SELECT 93"));
        const auto* model = properties->findChild<QTableView*>("objectPropertiesGrid")->model();
        QCOMPARE(model->index(0, 1).data().toString(), QString("object_properties_owned"));
        QVERIFY(!window.findChild<SourcePane*>());
        properties->close();
        QSignalSpy modified(&window, &MainWindow::queryFinished);
        bool correctPreview = false;
        QTimer confirmation;
        confirmation.setInterval(10);
        connect(&confirmation, &QTimer::timeout, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("sourceDropDialog");
            if (!dialog || !dialog->isVisible()) return;
            confirmation.stop();
            auto* name = dialog->findChild<QLineEdit*>("confirmObjectDrop");
            auto* drop = dialog->findChild<QPushButton*>("executeObjectDrop");
            auto* preview = dialog->findChild<QPlainTextEdit*>("dropObjectSqlPreview");
            correctPreview = name && drop && !drop->isEnabled() && preview &&
                preview->toPlainText().contains("DROP VIEW") && preview->toPlainText().contains("object_properties_owned");
            if (!correctPreview) { dialog->reject(); return; }
            name->setText("object_properties_owned");
            drop->click();
        });
        confirmation.start();
        QVERIFY(invokeMenu("contextDropObject"));
        QVERIFY(correctPreview);
        QTRY_COMPARE(modified.size(), 1);
        QVERIFY(db->execute("SELECT name FROM sqlite_schema WHERE name='object_properties_owned'", {}, token).rows.isEmpty());
        QCOMPARE(db->execute("SELECT number FROM object_preserved_owned", {}, token).rows.front().front().toInt(), 95);
        QTRY_VERIFY(!findTarget());
    }

    void queryParameterDialogValidatesExecutesAndCancels() {
        using namespace navidog;
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto* pane = qobject_cast<QueryPane*>(window.findChild<QTabWidget*>("queryTabs")->currentWidget());
        QVERIFY(pane);
        pane->editor->setPlainText("SELECT CAST(:id AS INTEGER) AS result, :id AS repeated, ':skip' AS literal");
        QSignalSpy completed(&window, &MainWindow::queryFinished);
        const auto intercept = [&](std::function<void(QDialog*)> action) {
            auto* timer = new QTimer(&window);
            timer->setInterval(10);
            connect(timer, &QTimer::timeout, &window, [&, timer, action] {
                auto* dialog = window.findChild<QDialog*>("queryParameterDialog");
                if (!dialog || !dialog->isVisible()) return;
                timer->stop();
                timer->deleteLater();
                action(dialog);
            });
            timer->start();
        };
        bool validated = false;
        intercept([&](QDialog* dialog) {
            const auto values = dialog->findChildren<QLineEdit*>("queryParameterValue");
            const auto types = dialog->findChildren<QComboBox*>("queryParameterType");
            auto* run = dialog->findChild<QPushButton*>("confirmQueryParameterBindings");
            if (values.size() != 1 || types.size() != 1 || !run ||
                types.front()->currentData().toInt() != int(QueryParameterType::Integer)) {
                dialog->reject();
                return;
            }
            values.front()->setText("invalid");
            run->click();
            validated = dialog->isVisible() && !dialog->findChild<QLabel*>("queryParameterInputError")->text().isEmpty();
            values.front()->setText("42");
            run->click();
        });
        window.runCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(validated, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(pane->model->result().rows.size(), 1);
        QCOMPARE(pane->model->result().rows.front().value(0).toInt(), 42);
        QCOMPARE(pane->model->result().rows.front().value(1).toInt(), 42);
        bool cancelled = false;
        intercept([&](QDialog* dialog) {
            cancelled = true;
            dialog->reject();
        });
        window.runCurrent();
        QTRY_VERIFY_WITH_TIMEOUT(cancelled, 5000);
        QCOMPARE(completed.size(), 1);
    }

    void taskCenterRestoresGenerationAndTracksExportFile() {
        using namespace navidog;
        QTemporaryDir directory;
        ConnectionProfile profile;
        profile.database = ensureDemoDatabase(directory.path());
        auto db = connectDatabase(profile);
        const auto token = cancellation();
        QVERIFY(db->execute("CREATE TABLE queued_generation(id INTEGER PRIMARY KEY, label TEXT)", {}, token).error.isEmpty());
        QMainWindow window;
        BackgroundTasks tasks(&window);
        navidog::ui::TaskCenter center(tasks, *window.statusBar(), window);
        window.show();
        QueryService service;
        const auto gate = cancellation();
        const auto blocker = tasks.submit({"等待测试", {}, QFileInfo(profile.database).canonicalFilePath(), {}},
            [gate](const TaskContext& task) {
                while (!gate->load() && !task.cancel->load()) QThread::msleep(4);
                return TaskResult{};
            });
        QVERIFY(!blocker.isEmpty());

        TableTask request;
        request.action = TableAction::GenerateData;
        request.node.kind = "table";
        request.node.schema = "main";
        request.node.object = "queued_generation";
        request.maxRows = 25;
        TableTaskDialog generation(profile, request, service, &window);
        QString generationId;
        connect(&generation, &TableTaskDialog::backgroundStarted, &window, [&](const QString& id) {
            generationId = id;
            center.rememberDialog(id, generation);
        });
        generation.show();
        auto* buttons = generation.findChild<QDialogButtonBox*>();
        auto* okay = buttons->button(QDialogButtonBox::Ok);
        QTRY_VERIFY(okay->isEnabled());
        okay->click();
        QVERIFY(!generationId.isEmpty());
        QCOMPARE(tasks.info(generationId)->state, TaskState::Queued);
        QVERIFY(!service.busy());
        QVERIFY(generation.findChild<QWidget*>("tableTaskProgress")->isVisible());
        generation.close();
        QVERIFY(!generation.isVisible());
        QCOMPARE(tasks.info(generationId)->state, TaskState::Queued);
        window.findChild<QPushButton*>("taskCenterButton")->click();
        auto* list = window.findChild<QTableView*>("backgroundTaskList");
        QVERIFY(list->isVisible());
        list->selectRow(0);
        auto* restore = window.findChild<QPushButton*>("restoreTaskWindow");
        QVERIFY(restore->isEnabled());
        restore->click();
        QVERIFY(generation.isVisible());
        QCOMPARE(generation.findChild<QLineEdit*>("generationSize")->text(), QString("25"));
        buttons->button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!generation.isVisible());
        gate->store(true);
        QTRY_COMPARE(tasks.info(generationId)->state, TaskState::Succeeded);
        QCOMPARE(tasks.info(generationId)->progress.done, qint64(25));
        QCOMPARE(db->execute("SELECT count(*) FROM queued_generation", {}, token).rows[0][0].toInt(), 25);

        request.action = TableAction::ExportCsv;
        request.path = directory.filePath("generated.csv");
        TableTaskDialog exporter(profile, request, service, &window);
        QString exportId;
        connect(&exporter, &TableTaskDialog::backgroundStarted, &window,
                [&](const QString& id) { exportId = id; });
        exporter.show();
        exporter.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QVERIFY(!exportId.isEmpty());
        QTRY_COMPARE(tasks.info(exportId)->state, TaskState::Succeeded);
        QCOMPARE(tasks.info(exportId)->artifacts, QStringList{request.path});
        QFile exported(request.path);
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(exported.readAll().count('\n'), 26);
        list->selectRow(0);
        QVERIFY(window.findChild<QPushButton*>("openTaskFile")->isEnabled());
        QVERIFY(window.findChild<QPushButton*>("openTaskFolder")->isEnabled());
    }

    void dataGenerationAndReportsHaveWorkingNativeControls() {
        using namespace navidog;QTemporaryDir directory;ConnectionProfile p;p.database=ensureDemoDatabase(directory.path());auto db=connectDatabase(p);db->execute("CREATE TABLE ui_generated(id INTEGER PRIMARY KEY,title TEXT UNIQUE NOT NULL,n INTEGER NOT NULL)",{},cancellation());QueryService service;TableTask task;task.node.kind="table";task.node.schema="main";task.node.object="ui_generated";task.action=TableAction::GenerateData;task.maxRows=3;
        TableTaskDialog dialog(p, task, service);
        dialog.show();
        auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QTRY_VERIFY_WITH_TIMEOUT(ok->isEnabled(), 3000);
        auto* grid = dialog.findChild<QTableView*>("generationRules");
        QCOMPARE(grid->model()->rowCount(), 3);
        auto* size = dialog.findChild<QLineEdit*>("generationSize");
        auto* estimate = dialog.findChild<QPushButton*>("estimateGeneration");
        auto* message = dialog.findChild<QLabel*>("taskMessage");
        QVERIFY(size && estimate && message);
        size->setText("100,000,000,000,000");
        estimate->click();
        QTRY_VERIFY_WITH_TIMEOUT(message->text().contains("此规模仅估算"), 3000);
        QCOMPARE(db->execute("SELECT count(*) FROM ui_generated", {}, cancellation()).rows[0][0].toInt(), 0);
        size->setText("3");
        ok->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.succeeded(), 3000);
        QCOMPARE(db->execute("SELECT count(*) FROM ui_generated", {}, cancellation()).rows[0][0].toInt(), 3);
        dialog.reject();
        task.action=TableAction::ReverseModel;auto model=runTableTask(p,task,cancellation());QVERIFY(model.applied);auto* report=new TableReportDialog(task,model);navidog::ui::applyTheme(report);report->show();auto* view=report->findChild<QGraphicsView*>("tableModelView");QVERIFY(view);QVERIFY(view->scene()->items().size()>=4);
        if(!qEnvironmentVariableIsEmpty("NAVIDOG_TABLE_MODEL_SCREENSHOT"))QVERIFY(report->grab().save(qEnvironmentVariable("NAVIDOG_TABLE_MODEL_SCREENSHOT")));report->close();
        task.action=TableAction::BuildBI;task.dimension="title";auto result=runTableTask(p,task,cancellation());QVERIFY(result.applied);auto* chart=new TableReportDialog(task,result);navidog::ui::applyTheme(chart);chart->show();QVERIFY(chart->findChild<QWidget*>("distributionChart"));if(!qEnvironmentVariableIsEmpty("NAVIDOG_TABLE_BI_SCREENSHOT"))QVERIFY(chart->grab().save(qEnvironmentVariable("NAVIDOG_TABLE_BI_SCREENSHOT")));chart->close();
    }
    void dataGenerationConfigurationsRoundTripWithoutWritingRows() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ConnectionProfile profile;
        profile.database = ensureDemoDatabase(directory.path());
        auto db = connectDatabase(profile);
        const auto cancel = cancellation();
        QVERIFY(db->execute("CREATE TABLE preset_source(title TEXT NOT NULL, age INTEGER NOT NULL)",
                            {}, cancel).error.isEmpty());
        QVERIFY(db->execute("CREATE TABLE preset_target(age INTEGER NOT NULL, extra TEXT NOT NULL)",
                            {}, cancel).error.isEmpty());
        QueryService service;
        const auto storePath = localStorePath(directory.path());
        TableTask request;
        request.action = TableAction::GenerateData;
        request.node.kind = "table";
        request.node.schema = "main";
        request.node.object = "preset_source";

        const auto enterPreset = [&](TableTaskDialog& dialog, const char* id, bool naming) {
            auto* button = dialog.findChild<QPushButton*>(id);
            QVERIFY(button && button->isEnabled());
            QTimer answer;
            connect(&answer, &QTimer::timeout, &dialog, [&] {
                if (auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    if (naming) prompt->setTextValue("Age distribution");
                    prompt->accept();
                }
            });
            answer.start(20);
            button->click();
        };
        const auto pickFile = [&](TableTaskDialog& dialog, const char* id, const QString& path) {
            auto* button = dialog.findChild<QPushButton*>(id);
            QVERIFY(button && button->isEnabled());
            QTimer answer;
            int attempts = 0;
            connect(&answer, &QTimer::timeout, &dialog, [&] {
                auto* picker = dialog.findChild<QFileDialog*>();
                if (!picker || !picker->isVisible()) return;
                if (++attempts > 80) { picker->reject(); return; }
                if (auto* name = picker->findChild<QLineEdit*>("fileNameEdit")) {
                    name->setText(path);
                    QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                }
            });
            answer.start(20);
            button->click();
        };
        const auto seed = QString("18446744073709551614");
        QJsonObject expectedRules;
        {
            TableTaskDialog dialog(profile, request, service, nullptr, storePath);
            dialog.show();
            QTRY_VERIFY(dialog.findChild<QPushButton*>("saveGenerationConfiguration")->isEnabled());
            dialog.findChild<QComboBox*>("generationSizeMode")->setCurrentIndex(1);
            dialog.findChild<QLineEdit*>("generationSize")->setText("1.5 TiB");
            dialog.findChild<QLineEdit*>("generationSeed")->setText(seed);
            auto* rules = dialog.findChild<QTableView*>("generationRules")->model();
            QCOMPARE(rules->rowCount(), 2);
            QVERIFY(rules->setData(rules->index(0, 3), "[A-Z]{3}-[0-9]{4}"));
            QVERIFY(rules->setData(rules->index(1, 3), "18..65"));
            QVERIFY(rules->setData(rules->index(1, 4), "偏低"));
            expectedRules = dialog.task().generationRules;
            enterPreset(dialog, "saveGenerationConfiguration", true);
            QVERIFY(dialog.findChild<QLabel*>("taskMessage")->text().contains("已保存"));
        }

        const auto configPath = directory.filePath("generation.json");
        {
            TableTaskDialog reopened(profile, request, service, nullptr, storePath);
            reopened.show();
            QTRY_VERIFY(reopened.findChild<QPushButton*>("selectGenerationConfiguration")->isEnabled());
            enterPreset(reopened, "selectGenerationConfiguration", false);
            QCOMPARE(reopened.findChild<QComboBox*>("generationSizeMode")->currentIndex(), 1);
            QCOMPARE(reopened.findChild<QLineEdit*>("generationSize")->text(), QString("1.5 TiB"));
            QCOMPARE(reopened.findChild<QLineEdit*>("generationSeed")->text(), seed);
            QCOMPARE(reopened.task().generationRules, expectedRules);
            pickFile(reopened, "exportGenerationConfiguration", configPath);
            QFile exported(configPath);
            QVERIFY(exported.open(QIODevice::ReadOnly));
            const auto json = QJsonDocument::fromJson(exported.readAll()).object();
            QCOMPARE(json.value("format").toString(), QString("navidog-data-generation"));
            QCOMPARE(json.value("seed").toString(), seed);
            QCOMPARE(json.value("rules").toObject(), expectedRules);
            QCOMPARE(json.size(), 6);
        }

        request.node.object = "preset_target";
        TableTaskDialog destination(profile, request, service, nullptr, storePath);
        destination.show();
        QTRY_VERIFY(destination.findChild<QPushButton*>("importGenerationConfiguration")->isEnabled());
        const auto defaultExtra = destination.task().generationRules.value("extra");
        pickFile(destination, "importGenerationConfiguration", configPath);
        QCOMPARE(destination.task().generation.size, QString("1.5 TiB"));
        QCOMPARE(destination.task().generationRules.value("age"), expectedRules.value("age"));
        QCOMPARE(destination.task().generationRules.value("extra"), defaultExtra);
        QVERIFY(!destination.task().generationRules.contains("title"));
        QVERIFY(destination.findChild<QLabel*>("taskMessage")->text().contains("1 条与当前表不匹配"));
        for (const auto& table : {"preset_source", "preset_target"})
            QCOMPARE(db->execute(QString("SELECT count(*) FROM ") + table, {}, cancel).rows[0][0].toInt(), 0);
    }
    void namespaceDialogCreatesFileAndConnectionAndKeepsFailureDraft() {
        QTemporaryDir directory;MainWindow window(directory.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);tree->setCurrentItem(root);
        bool requested=false,created=false;
        QTimer::singleShot(20,&window,[&]{
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;
            auto* create=menu->findChild<QAction*>("contextNewDatabase");if(!create){menu->close();return;}requested=true;
            QTimer::singleShot(20,&window,[&]{
                auto* dialog=window.findChild<NamespaceDialog*>("namespaceDialog");if(!dialog)return;
                dialog->findChild<QLineEdit*>("namespaceName")->setText("Owned database");dialog->findChild<QLineEdit*>("namespaceFile")->setText(directory.filePath("created.db"));
                auto* preview=dialog->findChild<QPlainTextEdit*>("namespaceSqlPreview");QVERIFY(preview->toPlainText().contains("created.db"));
                if(!qEnvironmentVariableIsEmpty("NAVIDOG_NAMESPACE_SCREENSHOT")){dialog->findChild<QTabWidget*>("namespaceTabs")->setCurrentIndex(0);QVERIFY(dialog->grab().save(qEnvironmentVariable("NAVIDOG_NAMESPACE_SCREENSHOT")));}
                auto* ok=dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);QVERIFY(ok->isEnabled());ok->click();created=true;
            });
            create->trigger();menu->close();
        });
        QTest::mouseClick(tree->viewport(),Qt::RightButton,Qt::NoModifier,tree->visualItemRect(root).center());QTRY_VERIFY(requested);QTRY_VERIFY(created);
        navidog::ProfileStore store(directory.filePath("navidog.sqlite"));auto profiles=store.load();auto found=std::find_if(profiles.begin(),profiles.end(),[](const auto& p){return p.name=="Owned database";});QVERIFY(found!=profiles.end());QCOMPARE(found->database,directory.filePath("created.db"));auto db=navidog::connectDatabase(*found);QVERIFY(db->tables().isEmpty());QCOMPARE(tree->topLevelItemCount(),2);
        navidog::QueryService service;navidog::ConnectionProfile p;NamespaceDialog failure(p,navidog::NamespaceKind::Database,service,&window);failure.findChild<QLineEdit*>("namespaceName")->setText("Existing");failure.findChild<QLineEdit*>("namespaceFile")->setText(found->database);failure.show();
        QTimer dismiss;connect(&dismiss,&QTimer::timeout,&failure,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->accept();});dismiss.start(20);
        failure.accept();QTRY_VERIFY_WITH_TIMEOUT(!service.busy(),3000);QVERIFY(failure.isVisible());QCOMPARE(failure.findChild<QLineEdit*>("namespaceName")->text(),QString("Existing"));QVERIFY(failure.findChild<QLabel*>("namespaceMessage")->text().contains("已存在"));
    }
    void namespaceMenusFollowDriverAndSeparateGroups() {
        QTemporaryDir directory;MainWindow window(directory.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);
        auto inspect=[&](QTreeWidgetItem* item,const QStringList& expected,bool enabled){
            bool checked=false;tree->scrollToItem(item);
            QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;QStringList labels;for(auto* a:menu->actions())labels<< (a->isSeparator()?"---":a->text());const auto* create=menu->findChild<QAction*>("contextNewSchema");const auto* drop=menu->findChild<QAction*>("contextDropNamespace");const auto screenshot=qEnvironmentVariable("NAVIDOG_MENU_SCREENSHOT");const bool captured=item!=root || screenshot.isEmpty() || menu->grab().save(screenshot);checked=true;menu->close();QCOMPARE(labels,expected);if(create)QCOMPARE(create->isEnabled(),enabled);if(drop)QCOMPARE(drop->isEnabled(),enabled);QVERIFY(captured);});
            tree->setCurrentItem(item);QTest::keyClick(tree,Qt::Key_F10,Qt::ShiftModifier);QVERIFY(checked);
        };
        inspect(root,{"打开连接","---","编辑连接…","新建连接…","删除连接…","复制连接…","重命名…","导出连接…","---","新建数据库…","新建查询","在连接中查找对象…","---","管理组","---","刷新"},true);
        // Catalog fixtures only; never connect these profiles to any server.
        navidog::ProfileStore store(directory.filePath("navidog.sqlite"));auto profiles=store.load();navidog::ConnectionProfile pg;pg.id="pg-menu-fixture";pg.name="PG fixture";pg.driver=navidog::DatabaseKind::PostgreSQL;pg.database="owned";pg.readOnly=true;profiles<<pg;store.save(profiles);
        MainWindow pgWindow(directory.path());pgWindow.show();tree=pgWindow.findChild<QTreeWidget*>("connectionTree");auto* parent=tree->topLevelItem(1);
        navidog::CatalogNode node;node.kind="database";node.name=node.database="owned";auto* database=new QTreeWidgetItem(parent,{node.name});database->setData(0,Qt::UserRole,pg.id);database->setData(0,Qt::UserRole+2,QVariant::fromValue(node));
        // Mark the parent loaded before expanding to avoid fixture network access.
        parent->setData(0,Qt::UserRole+3,true);parent->setExpanded(true);
        inspect(database,{"打开数据库","---","编辑数据库…","新建数据库…","新建 Schema…","删除数据库…","数据库属性","---","新建查询","---","命令列","运行 SQL 文件…","转储 SQL 文件","数据库字典…","逆向到 ER 图…","在数据库中查找…","---","刷新","复制名称"},false);
        node.kind="schema";node.name=node.schema="draft";database->setData(0,Qt::UserRole+2,QVariant::fromValue(node));
        inspect(database,{"打开 Schema","---","新建数据库…","新建 Schema…","删除 Schema…","级联删除Schema…","Schema 属性","---","新建查询","新建表…","新建对象","---","命令列","运行 SQL 文件…","转储 SQL 文件","数据库字典…","逆向到 ER 图…","在 Schema 中查找…","---","刷新","复制名称"},false);
    }
    void databaseMenuRunsConsoleSearchAndDictionary() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *navigation = window.findChild<NavigationPane *>("navigationPane");
        auto *tree = window.findChild<QTreeWidget *>("connectionTree");
        auto *root = tree->topLevelItem(0);
        root->setExpanded(true);
        QTRY_COMPARE(root->childCount(), 1);
        auto *database = root->child(0);

        const auto invoke = [&](const QString& name) {
            QMenu menu;
            navigation->populateContextMenu(&menu, database);
            auto *action = menu.findChild<QAction *>(name);
            if (!action || !action->isEnabled()) return false;
            action->trigger();
            return true;
        };
        QVERIFY(invoke("contextDatabaseConsole"));
        QTRY_VERIFY(window.findChild<QLineEdit *>("databaseConsoleInput"));
        auto *command = window.findChild<QLineEdit *>("databaseConsoleInput");
        QSignalSpy consoleFinished(&window, &MainWindow::queryFinished);
        command->setText("SELECT 'session_marker' AS command_result");
        QTest::keyClick(command, Qt::Key_Return);
        auto *output = window.findChild<QPlainTextEdit *>("databaseConsoleOutput");
        QTRY_COMPARE(consoleFinished.count(), 1);
        QTRY_VERIFY(command->isEnabled());
        QVERIFY2(output->toPlainText().contains("\ncommand_result\nsession_marker\n"),
                 qPrintable(output->toPlainText()));

        QVERIFY(invoke("contextDatabaseSearch"));
        QTRY_VERIFY(window.findChild<QLineEdit *>("databaseSearchTerm"));
        auto *term = window.findChild<QLineEdit *>("databaseSearchTerm");
        term->setText("paid");
        window.findChild<QToolButton *>("runDatabaseSearch")->click();
        auto *grid = window.findChild<QTableView *>("databaseSearchResults");
        QTRY_VERIFY_WITH_TIMEOUT(grid->model()->rowCount() > 0, 10000);
        QVERIFY(grid->model()->index(0, 1).data().toLongLong() > 0);

        QVERIFY(invoke("contextDatabaseDictionary"));
        QTRY_VERIFY(window.findChild<QDialog *>("databaseDictionary"));
        auto *dictionaryDialog = window.findChild<QDialog *>("databaseDictionary");
        auto *next = dictionaryDialog->findChild<QToolButton *>("dictionaryNext");
        auto *objects = dictionaryDialog->findChild<QTreeWidget *>("dictionaryObjectTree");
        QVERIFY(next->isEnabled());
        next->click();
        QTRY_VERIFY(objects->isVisible());
        QVERIFY(objects->topLevelItemCount() > 0);
        auto *tables = objects->topLevelItem(0)->child(1)->child(1);
        int orderIndex = -1;
        for (int i = 0; i < tables->childCount(); ++i)
            if (tables->child(i)->text(0) == "orders") orderIndex = i;
        QVERIFY(orderIndex > 0);
        objects->setCurrentItem(tables->child(orderIndex));
        dictionaryDialog->findChild<QToolButton *>("dictionaryMoveUp")->click();
        QCOMPARE(tables->child(orderIndex-1)->text(0), QString("orders"));
        next->click();
        dictionaryDialog->findChild<QToolButton *>("dictionaryTheme4")->click();
        next->click();
        auto *title = dictionaryDialog->findChild<QLineEdit *>("dictionaryTitle");
        QVERIFY(title->isVisible());
        title->setText("测试字典标题");
        const auto presetAction = [&](const char* button, const QString& label) -> QAction* {
            const auto commands = dictionaryDialog->findChild<QToolButton *>(button)->menu()->actions();
            for (auto* action : commands) if (action->text() == label) return action;
            return nullptr;
        };
        const auto runPresetDialog = [&](QAction* action,bool saving) {
            QVERIFY(action);
            QTimer respond;
            connect(&respond,&QTimer::timeout,dictionaryDialog,[&] {
                auto* prompt = dictionaryDialog->findChild<QInputDialog *>();
                if (!prompt || !prompt->isVisible()) return;
                if (saving) prompt->setTextValue("测试方案");
                prompt->accept();
            });
            respond.start(25);
            action->trigger();
        };
        runPresetDialog(presetAction("dictionarySaveConfiguration","保存为本地方案…"),true);
        const auto configPath = directory.filePath("dictionary-settings.json");
        const auto selectConfigurationFile = [&](QAction* action) {
            QVERIFY(action);
            QTimer reply;
            int attempts = 0;
            connect(&reply,&QTimer::timeout,dictionaryDialog,[&] {
                auto* picker = dictionaryDialog->findChild<QFileDialog *>();
                if (!picker || !picker->isVisible()) return;
                if (++attempts >= 100) { picker->reject(); return; }
                if (auto* name = picker->findChild<QLineEdit *>("fileNameEdit")) {
                    name->setText(configPath);
                    QMetaObject::invokeMethod(picker,"accept",Qt::DirectConnection);
                }
            });
            reply.start(25);
            action->trigger();
        };
        selectConfigurationFile(presetAction("dictionarySaveConfiguration","另存为配置文件…"));
        QFile configFile(configPath);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(configFile.readAll()).object()["format"].toString(),QString("navidog-dictionary"));
        configFile.close();
        selectConfigurationFile(presetAction("dictionaryLoadConfiguration","从配置文件加载…"));
        QTRY_VERIFY(dictionaryDialog->findChild<QTreeWidget *>("dictionaryScopeTree")->isVisible());
        title->setText("临时修改");
        runPresetDialog(presetAction("dictionaryLoadConfiguration","选择本地方案…"),false);
        QTRY_VERIFY(dictionaryDialog->findChild<QTreeWidget *>("dictionaryScopeTree")->isVisible());
        next->click();
        QTRY_VERIFY(objects->isVisible());
        QCOMPARE(objects->topLevelItem(0)->child(1)->child(1)->child(orderIndex-1)->text(0),QString("orders"));
        next->click();
        QVERIFY(dictionaryDialog->findChild<QToolButton *>("dictionaryTheme4")->isChecked());
        next->click();
        QCOMPARE(title->text(),QString("测试字典标题"));
        next->click();
        auto *dictionary = dictionaryDialog->findChild<QTextBrowser *>("dictionaryExportPreview");
        QTRY_VERIFY(dictionary->toPlainText().contains("orders"));
        QVERIFY(dictionary->toPlainText().contains("测试字典标题"));
        const auto verifyExport = [&](const char* buttonName, const char* extension, const QByteArray& header) {
            const auto path = directory.filePath(QString("dictionary.") + extension);
            auto *button = dictionaryDialog->findChild<QToolButton *>(buttonName);
            QVERIFY(button && button->isEnabled());
            QTimer selectPath;
            int attempts = 0;
            connect(&selectPath, &QTimer::timeout, dictionaryDialog, [&] {
                auto *picker = dictionaryDialog->findChild<QFileDialog *>();
                if (!picker || !picker->isVisible()) return;
                if (++attempts >= 100) { picker->reject(); return; }
                if (auto *name = picker->findChild<QLineEdit *>("fileNameEdit")) {
                    name->setText(path);
                    QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                }
            });
            selectPath.start(25);
            button->click();
            selectPath.stop();
            QTRY_VERIFY(QFile::exists(path));
            QTRY_VERIFY(button->isEnabled());
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.read(header.size()), header);
        };
        verifyExport("dictionaryExportPdf", "pdf", "%PDF");
        verifyExport("dictionaryExportWord", "docx", "PK");
        verifyExport("dictionaryExportHtml", "html", "<!doctype html>");
        dictionaryDialog->close();

        QVERIFY(invoke("contextEditDatabase"));
        QTRY_VERIFY(window.findChild<QSpinBox *>("databasePropertyVersion"));
        auto *version = window.findChild<QSpinBox *>("databasePropertyVersion");
        QTRY_VERIFY(version->isEnabled());
        version->setValue(17);
        auto *preview = window.findChild<QPlainTextEdit *>("databasePropertiesSql");
        QVERIFY(preview->toPlainText().contains("user_version = 17"));
        auto *edit = window.findChild<QDialog *>("databasePropertiesEditor");
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!window.findChild<QDialog *>("databasePropertiesEditor"));
        navidog::ConnectionProfile sqlite;
        sqlite.database = directory.filePath("harbor-demo.db");
        auto db = navidog::connectDatabase(sqlite);
        QCOMPARE(db->execute("PRAGMA user_version", {}, navidog::cancellation()).rows.value(0).value(0).toInt(), 17);
    }

    void dictionaryCombinesScopesInSelectedOrder() {
        using namespace navidog;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVector<ConnectionProfile> profiles;
        for (const auto& name : {QString("Fixture A"),QString("Fixture B")}) {
            ConnectionProfile profile;
            profile.name = profile.id = name;
            profile.database = dir.filePath(name + ".sqlite");
            { QFile file(profile.database); QVERIFY(file.open(QIODevice::WriteOnly)); }
            const auto table = profiles.isEmpty() ? "first_table" : "second_table";
            auto db = connectDatabase(profile);
            QVERIFY(db->execute(QString("CREATE TABLE ")+table+" (id INTEGER)",{},cancellation()).error.isEmpty());
            profiles.push_back(profile);
        }
        QWidget owner;
        owner.show();
        CatalogNode node;
        node.kind = "database";
        node.name = node.database = "main";
        ui::showDictionaryWizard(&owner,profiles,profiles[0].id,node,dir.filePath("presets.sqlite"),
            [&](const QString& id,const CatalogNode&) -> std::optional<ConnectionProfile> {
                for (const auto& profile : profiles) if (profile.id == id) return profile;
                return {};
            },[](const QString&) { return QVector<CatalogNode>{}; });
        auto* wizard = owner.findChild<QDialog*>("databaseDictionary");
        auto* scopes = wizard->findChild<QTreeWidget*>("dictionaryScopeTree");
        scopes->topLevelItem(0)->child(1)->child(0)->setCheckState(0,Qt::Checked);
        auto* next = wizard->findChild<QToolButton*>("dictionaryNext");
        next->click();
        auto* objects = wizard->findChild<QTreeWidget*>("dictionaryObjectTree");
        QTRY_COMPARE(objects->topLevelItemCount(),2);
        objects->setCurrentItem(objects->topLevelItem(1));
        wizard->findChild<QToolButton*>("dictionaryMoveUp")->click();
        next->click();
        next->click();
        next->click();
        auto* browser = wizard->findChild<QTextBrowser*>("dictionaryExportPreview");
        QTRY_VERIFY(browser->toPlainText().contains("first_table"));
        const auto text = browser->toPlainText();
        QVERIFY(text.contains("second_table"));
        QVERIFY(text.indexOf("second_table") < text.indexOf("first_table"));
    }

    void databaseMenuRunsAndDumpsSqlFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        navidog::ConnectionProfile profile;
        profile.id = "owned-sql-files";
        profile.name = "SQL file fixture";
        profile.database = directory.filePath("fixture.sqlite");
        { QFile file(profile.database); QVERIFY(file.open(QIODevice::WriteOnly)); }
        navidog::ProfileStore(navidog::localStorePath(directory.path())).save({profile});
        const auto script = directory.filePath("setup.sql");
        { QFile file(script); QVERIFY(file.open(QIODevice::WriteOnly));
          QVERIFY(file.write("CREATE TABLE menu_fixture (value TEXT);\n"
                             "INSERT INTO menu_fixture VALUES ('owned-marker');\n") > 0); }

        MainWindow window(directory.path());
        window.show();
        auto *navigation = window.findChild<NavigationPane *>("navigationPane");
        auto *root = navigation->connectionItem(profile.id);
        QVERIFY(root);
        root->setExpanded(true);
        QTRY_COMPARE(root->childCount(), 1);
        QMenu menu;
        navigation->populateContextMenu(&menu, root->child(0));

        QString selectedPath;
        bool confirmScript = false;
        int dialogTicks = 0;
        QTimer respond;
        connect(&respond, &QTimer::timeout, &window, [&] {
            auto *modal = QApplication::activeModalWidget();
            if (!modal) modal = window.findChild<QFileDialog *>();
            if (auto *picker = qobject_cast<QFileDialog *>(modal); picker && picker->isVisible()) {
                if (++dialogTicks >= 100) { picker->reject(); return; }
                if (auto *name = picker->findChild<QLineEdit *>("fileNameEdit")) {
                    name->setText(selectedPath);
                    QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                }
            } else if (auto *question = qobject_cast<QMessageBox *>(modal)) {
                if (confirmScript && question->button(QMessageBox::Yes)) {
                    confirmScript = false;
                    question->button(QMessageBox::Yes)->click();
                }
            }
        });
        selectedPath = script;
        confirmScript = true;
        respond.start(25);
        auto *run = menu.findChild<QAction *>("contextRunDatabaseSqlFile");
        QVERIFY(run && run->isEnabled());
        QSignalSpy finished(&window, &MainWindow::queryFinished);
        run->trigger();
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!confirmScript);
        auto db = navidog::connectDatabase(profile);
        const auto rows = db->execute("SELECT value FROM menu_fixture", {}, navidog::cancellation()).rows;
        QCOMPARE(rows.value(0).value(0).toString(), QString("owned-marker"));

        const auto client = QStandardPaths::findExecutable("sqlite3");
        if (client.isEmpty()) return;
        for (const bool withData : {false, true}) {
            auto *action = menu.findChild<QAction *>(withData ? "dumpDatabaseWithData" : "dumpDatabaseSchemaOnly");
            QVERIFY(action && action->isEnabled());
            selectedPath = directory.filePath(withData ? "all.sql" : "schema.sql");
            dialogTicks = 0;
            action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(selectedPath), 10000);
            QFile file(selectedPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto sql = file.readAll();
            QVERIFY(sql.contains("CREATE TABLE menu_fixture"));
            QCOMPARE(sql.contains("owned-marker"), withData);
            auto restored = profile;
            restored.database = selectedPath + ".sqlite";
            QProcess restore;
            restore.setStandardInputFile(selectedPath);
            restore.start(client, {"-batch", "-bail", "-init", QProcess::nullDevice(), restored.database});
            QVERIFY(restore.waitForFinished(5000));
            QCOMPARE(restore.exitCode(), 0);
            const auto restoredRows = navidog::connectDatabase(restored)->execute(
                "SELECT value FROM menu_fixture", {}, navidog::cancellation()).rows;
            QCOMPARE(restoredRows.size(), withData ? 1 : 0);
        }
    }

    void catalogCompletionWaitsForAnOpenNavigationMenu() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");
        auto* service=window.findChild<navidog::QueryService*>();
        auto* navigation=window.findChild<NavigationPane*>("navigationPane");
        QVERIFY(tree && service && navigation);
        auto* root=tree->topLevelItem(0);
        root->setExpanded(true);
        QTRY_VERIFY(root->childCount()>0);
        auto* database=root->child(0);
        database->setExpanded(true);
        QTRY_VERIFY(database->childCount()>0);
        auto* folder=database->child(0);
        folder->setExpanded(true);
        QTRY_VERIFY(folder->childCount()>0);
        auto* table=folder->child(folder->childCount()-1);
        QCOMPARE(table->data(0,Qt::UserRole+2).value<navidog::CatalogNode>().kind,QString("table"));
        QCOMPARE(table->text(0), QString("orders"));
        const auto request=folder->data(0,Qt::UserRole+4).toString();
        QVERIFY(!request.isEmpty());
        const QPersistentModelIndex selected=tree->indexFromItem(table);
        tree->scrollToItem(table);
        {
            navidog::ConnectionProfile fixture;
            fixture.database = directory.filePath("harbor-demo.db");
            auto db = navidog::connectDatabase(fixture);
            const auto dropped = db->execute("DROP TABLE orders", {}, navidog::cancellation());
            QVERIFY2(dropped.error.isEmpty(), qPrintable(dropped.error));
        }
        bool menuPreservedNode=false;
        QPointer<QMenu> openMenu;
        connect(service, &navidog::QueryService::catalogCompleted, &window,
                [&](const QString &id, const navidog::CatalogResult &result) {
            if (id != request || !openMenu)
                return;
            menuPreservedNode = result.error.isEmpty() && selected.isValid();
            openMenu->close();
        });
        QTimer::singleShot(20,&window,[&] {
            openMenu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!openMenu) return;
            QTimer::singleShot(3000, openMenu.data(), &QWidget::close);
            navigation->expandCatalog(folder, true);
        });
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,
                                  Q_ARG(QPoint,tree->visualItemRect(table).center()));
        QVERIFY(menuPreservedNode);
        QTRY_VERIFY(!selected.isValid());
    }
    void contextMenusAndOpenDatabaseAndCompactTabs() {
        QTemporaryDir directory;MainWindow window(directory.path());window.show();
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* connection=tree->topLevelItem(0);connection->setExpanded(true);
        QTRY_COMPARE_WITH_TIMEOUT(connection->childCount(),1,3000);auto* database=connection->child(0);
        bool openOffered=false;
        QTimer::singleShot(20,&window,[&] {
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;
            QStringList labels;for(auto* a:menu->actions())labels<<a->text();
            QVERIFY(!labels.contains("新建连接…"));QVERIFY(!labels.contains("删除连接…"));
            for(auto* a:menu->actions())if(a->text()=="打开数据库"){a->trigger();openOffered=true;break;}
            menu->close();
        });
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,tree->visualItemRect(database).center()));
        QVERIFY(openOffered);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);QVERIFY(database->isExpanded());
        auto* tables=database->child(0);tables->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(tables->childCount(),2,3000);
        auto* table=tables->child(0);tree->scrollToItem(table);bool checked=false;
        QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;QStringList labels;for(auto* a:menu->actions())labels<<a->text();QVERIFY(!labels.contains("新建连接…"));QVERIFY(!labels.contains("删除连接…"));QVERIFY(labels.contains("打开表"));checked=true;menu->close();});
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,tree->visualItemRect(table).center()));QVERIFY(checked);
        QVERIFY(window.findChild<QToolButton*>("addConnection")->isEnabled());QVERIFY(!window.findChild<QToolButton*>("removeConnection")->isEnabled());
        auto* tabs=window.findChild<QTabWidget*>("queryTabs");QVERIFY(!tabs->tabBar()->expanding());QVERIFY(tabs->tabBar()->height()<=36);
        QVERIFY(!tabs->cornerWidget(Qt::TopRightCorner));QVERIFY(tabs->tabBar()->tabButton(0,QTabBar::RightSide));
        QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);QCOMPARE(tabs->count(),2);auto* close=tabs->tabBar()->tabButton(0,QTabBar::RightSide);QTest::mouseClick(close,Qt::LeftButton);QTRY_COMPARE(tabs->count(),1);
        tree->scrollToItem(database);bool closed=false;
        QTimer::singleShot(20,&window,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->text()=="关闭数据库"){a->trigger();closed=true;break;}menu->close();});
        QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,tree->visualItemRect(database).center()));QVERIFY(closed);QCOMPARE(database->childCount(),0);
    }
    void queryTabsRestoreWithoutExecutingSql() {
        QTemporaryDir directory;
        {MainWindow window(directory.path());window.show();auto* editor=window.findChild<SqlEditor*>();editor->setPlainText("SELECT 'saved work';");QVERIFY(window.close());}
        MainWindow restored(directory.path());QVERIFY(restored.restoredSession());auto* editor=restored.findChild<QTabWidget*>("queryTabs")->currentWidget()->findChild<SqlEditor*>();QCOMPARE(editor->toPlainText(),QString("SELECT 'saved work';"));
        auto* table=restored.findChild<QTableView*>("resultTable");QCOMPARE(table->model()->rowCount(),0);
    }
    void tableDesignerCreatesChangesAndRestoresDrafts() {
        QTemporaryDir directory;
        {
            MainWindow window(directory.path());window.show();
            auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);
            auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);auto* folder=database->child(0);tree->setCurrentItem(folder);
            auto* action=window.findChild<QAction*>("menuNewTable");QVERIFY(action->isEnabled());action->trigger();
            auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<TablePane*>(tabs->currentWidget());QVERIFY(pane);
            QVERIFY(!pane->findChild<QToolButton*>("runQuery"));QVERIFY(pane->save->isVisible());QVERIFY(pane->mapTo(&window,QPoint(0,0)).y()>tabs->tabBar()->mapTo(&window,QPoint(0,0)).y());
            pane->name->setText("ui_designed_table");QVERIFY(pane->sql->toPlainText().contains("CREATE TABLE"));QVERIFY(pane->save->isEnabled());
            QSignalSpy done(&window,&MainWindow::queryFinished);QTest::mouseClick(pane->save,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
            QVERIFY(pane->originalDefinition().exists);QCOMPARE(pane->originalDefinition().columns.size(),1);QVERIFY(!pane->dirty());
            pane->model->addColumn(navidog::DatabaseKind::SQLite);pane->model->setData(pane->model->index(1,0),"note",Qt::EditRole);pane->model->setData(pane->model->index(1,1),"TEXT",Qt::EditRole);
            QVERIFY(pane->sql->toPlainText().contains("ADD"));QTRY_VERIFY(pane->save->isEnabled());QTest::mouseClick(pane->save,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QCOMPARE(pane->originalDefinition().columns.size(),2);
            navidog::ProfileStore store(directory.filePath("navidog.sqlite"));auto profile=store.load().first();auto db=navidog::connectDatabase(profile);auto result=db->execute("SELECT * FROM ui_designed_table",{},navidog::cancellation());QCOMPARE(result.columns,QStringList({"id","note"}));
            tabs->setCurrentIndex(0);auto* queryTools=tabs->currentWidget()->findChild<QWidget*>("queryToolbar");QVERIFY(queryTools->isVisible());QVERIFY(!pane->save->isVisible());tabs->setCurrentWidget(pane);
            if(!qEnvironmentVariableIsEmpty("NAVIDOG_TABLE_SCREENSHOT"))QVERIFY(window.grab().save(qEnvironmentVariable("NAVIDOG_TABLE_SCREENSHOT")));
            pane->model->addColumn(navidog::DatabaseKind::SQLite);pane->model->setData(pane->model->index(2,0),"pending_draft",Qt::EditRole);QVERIFY(pane->dirty());QVERIFY(window.close());
        }
        MainWindow restored(directory.path());QVERIFY(restored.restoredSession());auto* tabs=restored.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<TablePane*>(tabs->currentWidget());QVERIFY(pane);QCOMPARE(pane->model->rowCount(),3);QCOMPARE(pane->originalDefinition().columns.size(),2);QVERIFY(pane->dirty());
    }
    void tableDesignerProtectsDataDraftsAndInvalidatesLoadedRows() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *tree = window.findChild<QTreeWidget *>("connectionTree");
        auto *root = tree->topLevelItem(0);
        root->setExpanded(true);
        QTRY_COMPARE_WITH_TIMEOUT(root->childCount(), 1, 3000);
        auto *database = root->child(0);
        database->setExpanded(true);
        QTRY_COMPARE_WITH_TIMEOUT(database->childCount(), 3, 3000);
        auto *tables = database->child(0);
        tables->setExpanded(true);
        QTRY_COMPARE_WITH_TIMEOUT(tables->childCount(), 2, 3000);
        auto *customers = tables->child(0);
        QCOMPARE(customers->text(0), QString("customers"));
        tree->itemDoubleClicked(customers, 0);

        auto *tabs = window.findChild<QTabWidget *>("queryTabs");
        auto *data = qobject_cast<DataPane *>(tabs->currentWidget());
        QVERIFY(data);
        QTRY_VERIFY_WITH_TIMEOUT(data->isLoaded(), 3000);
        auto *grid = data->findChild<QTableView *>("dataGrid");
        auto *model = grid->model();
        const auto name = model->index(0, 1);
        QVERIFY(model->setData(name, "edited only in the data tab", Qt::EditRole));
        QVERIFY(data->dirty());

        tree->setCurrentItem(customers);
        window.findChild<QAction *>("menuAlterTable")->trigger();
        auto *design = qobject_cast<TablePane *>(tabs->currentWidget());
        QVERIFY(design);
        QTRY_VERIFY_WITH_TIMEOUT(design->isLoaded(), 3000);
        QVERIFY(data->matchesTable(design->profileId, design->tableNode()));
        design->model->addColumn(navidog::DatabaseKind::SQLite);
        const auto column = design->model->rowCount() - 1;
        QVERIFY(design->model->setData(design->model->index(column, 0), "design_owned_note", Qt::EditRole));
        QVERIFY(design->model->setData(design->model->index(column, 1), "TEXT", Qt::EditRole));
        auto *service = window.findChild<navidog::QueryService *>();
        QVERIFY(service);
        QTRY_VERIFY_WITH_TIMEOUT(!service->busy(), 3000);
        QVERIFY(design->canSave());
        QVERIFY(data->dirty());
        QSignalSpy finished(&window, &MainWindow::queryFinished);
        QString prompt;
        QTimer closePrompt;
        closePrompt.setInterval(10);
        connect(&closePrompt, &QTimer::timeout, &window, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                prompt = box->text();
                box->accept();
            }
        });
        closePrompt.start();
        design->requestSave();
        closePrompt.stop();
        QVERIFY(!design->designInProgress());
        QVERIFY(prompt.contains("请先提交或放弃此表数据页中的修改"));
        QCOMPARE(finished.count(), 0);
        QVERIFY(data->dirty());
        QVERIFY(design->dirty());

        data->findChild<QToolButton *>("discardData")->click();
        QVERIFY(!data->dirty());
        design->requestSave();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QVERIFY(!design->dirty());
        QVERIFY(!(model->flags(model->index(0, 1)) & Qt::ItemIsEditable));
        QVERIFY(data->findChild<QLabel *>("dataSummary")->text().contains("表结构已更新"));
    }
    void tableDesignerChildPagesSaveAndRestoreObjectDrafts() {
        using namespace navidog;QTemporaryDir dir;auto file=ensureDemoDatabase(dir.path());ConnectionProfile profile;profile.database=file;auto db=connectDatabase(profile);auto token=cancellation();db->execute("CREATE TABLE ui_children(id INTEGER PRIMARY KEY,note TEXT)",{},token);db->execute("CREATE TABLE ui_child_audit(note TEXT)",{},token);
        {
            MainWindow window(dir.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);auto* folder=database->child(0);folder->setExpanded(true);QTRY_VERIFY_WITH_TIMEOUT(folder->childCount()>=4,3000);QTreeWidgetItem* table=nullptr;for(int i=0;i<folder->childCount();++i)if(folder->child(i)->text(0)=="ui_children")table=folder->child(i);QVERIFY(table);tree->setCurrentItem(table);window.findChild<QAction*>("menuAlterTable")->trigger();auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<TablePane*>(tabs->currentWidget());QVERIFY(pane);QTRY_VERIFY_WITH_TIMEOUT(pane->isLoaded(),3000);QVERIFY(!pane->dirty());QSignalSpy done(&window,&MainWindow::queryFinished);
            pane->focusObject("indexes",{},1);pane->commitEditor();auto* indexes=pane->objectModels[int(TableObjectKind::Index)];QCOMPARE(indexes->rowCount(),1);indexes->setData(indexes->index(0,1),"note");QVERIFY(pane->sql->toPlainText().contains("CREATE INDEX"));QTRY_VERIFY(pane->save->isEnabled());QTest::mouseClick(pane->save,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);QVERIFY(!pane->dirty());QCOMPARE(db->execute("SELECT count(*) FROM pragma_index_list('ui_children')",{},token).rows[0][0].toInt(),1);
            pane->focusObject("checks",{},1);pane->commitEditor();auto* checks=pane->objectModels[int(TableObjectKind::Check)];checks->setData(checks->index(0,1),"length(note)<20");QTRY_VERIFY(pane->save->isEnabled());QTest::mouseClick(pane->save,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QVERIFY(!pane->dirty());
            pane->focusObject("triggers",{},1);pane->commitEditor();pane->triggerSql->setPlainText("CREATE TRIGGER trigger_1 AFTER INSERT ON ui_children BEGIN INSERT INTO ui_child_audit VALUES(NEW.note); END");QTRY_VERIFY(pane->save->isEnabled());QTest::mouseClick(pane->save,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,3000);QVERIFY(!pane->dirty());db->execute("INSERT INTO ui_children VALUES(1,'saved')",{},token);QCOMPARE(db->execute("SELECT note FROM ui_child_audit",{},token).rows[0][0].toString(),QString("saved"));
            pane->focusObject("index",pane->objectModels[int(TableObjectKind::Index)]->objects[0].name,2);QVERIFY(pane->dirty());QVERIFY(pane->sql->toPlainText().contains("DROP INDEX"));QVERIFY(window.close());
        }
        MainWindow restored(dir.path());auto* pane=dynamic_cast<TablePane*>(restored.findChild<QTabWidget*>("queryTabs")->currentWidget());QVERIFY(pane);QVERIFY(pane->dirty());QCOMPARE(pane->originalDefinition().objects.size(),3);QCOMPARE(pane->definition().objects.size(),2);QCOMPARE(db->execute("SELECT count(*) FROM pragma_index_list('ui_children')",{},token).rows[0][0].toInt(),1);
    }
    void legacyDockerProfilesBecomeManualWithoutDockerCalls() {
        QTemporaryDir dir;navidog::ConnectionProfile profile;profile.id="old-docker";profile.name="Stopped Docker";profile.driver=navidog::DatabaseKind::MySQL;profile.database="owned";profile.credentialSource=navidog::CredentialSource::LocalDocker;profile.credentialContainer="does-not-exist";profile.credentialVariable="PASSWORD";
        navidog::ProfileStore store(dir.filePath("navidog.sqlite"));store.save({profile});MainWindow window(dir.path());auto saved=store.load();auto it=std::find_if(saved.begin(),saved.end(),[](auto p){return p.id=="old-docker";});QVERIFY(it!=saved.end());QCOMPARE(it->credentialSource,navidog::CredentialSource::Prompt);QVERIFY(it->credentialContainer.isEmpty());
        for(auto* label:window.findChildren<QLabel*>())QVERIFY(!label->text().contains("密码仅在本次会话"));
    }
    void dataGridCommitsPagesFiltersAndKeepsFailedDrafts() {
        QTemporaryDir directory;navidog::ConnectionProfile profile;profile.database=navidog::ensureDemoDatabase(directory.path());auto db=navidog::connectDatabase(profile);auto token=navidog::cancellation();
        db->execute("CREATE TABLE ui_data(id INTEGER PRIMARY KEY,label TEXT UNIQUE NOT NULL DEFAULT 'todo',note TEXT,payload BLOB)",{},token);
        db->execute("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<125) INSERT INTO ui_data SELECT x,'row-'||x,'note',X'00FF' FROM n",{},token);
        {
            MainWindow window(directory.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* root=tree->topLevelItem(0);root->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(root->childCount(),1,3000);auto* database=root->child(0);database->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(database->childCount(),3,3000);auto* folder=database->child(0);folder->setExpanded(true);QTRY_COMPARE_WITH_TIMEOUT(folder->childCount(),3,3000);auto* object=folder->child(2);QCOMPARE(object->text(0),QString("ui_data"));tree->scrollToItem(object);
            QSignalSpy done(&window,&MainWindow::queryFinished);tree->itemDoubleClicked(object,0);QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,3000);
            auto* tabs=window.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<DataPane*>(tabs->currentWidget());QVERIFY(pane);QVERIFY(pane->isLoaded());
            auto *grid = pane->findChild<QTableView *>("dataGrid");
            QVERIFY(grid);
            auto *model = qobject_cast<DataGridModel *>(grid->model());
            QVERIFY(model);
            const auto button = [pane](const char *name) { return pane->findChild<QToolButton *>(name); };
            auto *next = button("nextDataPage");
            auto *commit = button("commitData");
            auto *filterColumn = pane->findChild<QComboBox *>("dataFilterColumn");
            auto *filterOp = pane->findChild<QComboBox *>("dataFilterOperator");
            auto *sortColumn = pane->findChild<QComboBox *>("dataSortColumn");
            auto *direction = pane->findChild<QComboBox *>("dataSortDirection");
            QVERIFY(model->editable);QCOMPARE(model->rowCount(),100);QVERIFY(next->isEnabled());QVERIFY(model->headerData(0,Qt::Horizontal,Qt::DisplayRole).toString().contains("INTEGER"));
            auto index=model->index(0,1);grid->setCurrentIndex(index);grid->edit(index);auto* cellEditor=grid->findChild<QLineEdit*>();QVERIFY(cellEditor);cellEditor->selectAll();QTest::keyClicks(cellEditor,"edited-from-ui");QTest::keyClick(cellEditor,Qt::Key_Return);QTRY_COMPARE(model->rows[0].values[1].toString(),QString("edited-from-ui"));
            model->setData(model->index(0,2),QVariant{});QVERIFY(pane->dirty());QVERIFY(!next->isEnabled());QVERIFY(commit->isEnabled());
            grid->setCurrentIndex(model->index(1,0));QTest::mouseClick(button("deleteDataRow"),Qt::LeftButton);QCOMPARE(model->rows[1].state,navidog::DataRowState::Deleted);
            QTest::mouseClick(button("insertDataRow"),Qt::LeftButton);QCOMPARE(model->rowCount(),101);model->setData(model->index(100,1),"inserted-from-ui");model->setData(model->index(100,2),QString(""));
            if(!qEnvironmentVariableIsEmpty("NAVIDOG_DATA_SCREENSHOT"))QVERIFY(window.grab().save(qEnvironmentVariable("NAVIDOG_DATA_SCREENSHOT")));
            QTest::mouseClick(commit,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QVERIFY(!pane->dirty());
            auto actual=db->execute("SELECT label,note FROM ui_data WHERE id=1",{},token);QCOMPARE(actual.rows[0][0].toString(),QString("edited-from-ui"));QVERIFY(!actual.rows[0][1].isValid());actual=db->execute("SELECT count(*) FROM ui_data",{},token);QCOMPARE(actual.rows[0][0].toInt(),125);
            auto *service = window.findChild<navidog::QueryService *>();
            QVERIFY(service);
            bool keyset = false;
            connect(service, &navidog::QueryService::dataPageCompleted, &window,
                    [&keyset](const QString &, const navidog::DataPageResult &result) {
                        keyset = result.keyset;
                    });
            QTest::mouseClick(next,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),3,3000);QVERIFY(keyset);QCOMPARE(model->rowCount(),25);QCOMPARE(model->rows[0].values[0].toInt(),102);
            QTest::mouseClick(button("previousDataPage"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),4,3000);QCOMPARE(model->rows[0].values[0].toInt(),1);
            filterColumn->setCurrentIndex(filterColumn->findData(1));filterOp->setCurrentIndex(filterOp->findData(int(navidog::DataFilter::Contains)));pane->findChild<QLineEdit *>("dataFilterValue")->setText("edited");QTest::mouseClick(button("applyDataFilter"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),5,3000);QCOMPARE(model->rowCount(),1);
            filterOp->setCurrentIndex(filterOp->findData(int(navidog::DataFilter::None)));sortColumn->setCurrentIndex(sortColumn->findData(0));direction->setCurrentIndex(1);QTest::mouseClick(button("applyDataFilter"),Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),6,3000);QCOMPARE(model->rows[0].values[0].toInt(),126);
            model->addRow();model->setData(model->index(100,1),"inserted-from-ui");QTest::mouseClick(commit,Qt::LeftButton);QTRY_COMPARE_WITH_TIMEOUT(done.count(),7,3000);auto* error=window.findChild<QDialog*>("databaseErrorDialog");QVERIFY(error);QVERIFY(error->findChild<QPlainTextEdit*>("databaseErrorDetails")->toPlainText().contains("回滚"));QVERIFY(pane->dirty());error->accept();model->discard();QVERIFY(window.close());
        }
        MainWindow restored(directory.path());auto* tabs=restored.findChild<QTabWidget*>("queryTabs");auto* pane=dynamic_cast<DataPane*>(tabs->currentWidget());QVERIFY(pane);QVERIFY(!pane->isLoaded());QVERIFY(!pane->dirty());QVERIFY(pane->viewSettings()["descending"].toBool());
    }
    void dataPanesIgnoreClosedReadAndRouteNextResultToItsOwner() {
        QTemporaryDir directory;
        navidog::ConnectionProfile profile;
        profile.id = "data-pane-owned";
        profile.database = navidog::ensureDemoDatabase(directory.path());
        navidog::CatalogNode customers;
        customers.kind = "table";
        customers.object = customers.name = "customers";
        auto orders = customers;
        orders.object = orders.name = "orders";
        navidog::QueryService service;
        const auto resolve = [&profile] { return std::optional{profile}; };
        DataPane first(profile, customers, service, resolve);
        DataPane second(profile, orders, service, resolve);
        QSignalSpy completed(&service, &navidog::QueryService::dataPageCompleted);
        QSignalSpy firstFinished(&first, &DataPane::operationFinished);
        QSignalSpy secondFinished(&second, &DataPane::operationFinished);

        first.requestRefresh();
        QVERIFY(service.busy());
        first.cancelLoad();
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);
        QVERIFY(!first.isLoaded());
        QCOMPARE(firstFinished.count(), 0);

        second.requestRefresh();
        QTRY_COMPARE_WITH_TIMEOUT(secondFinished.count(), 1, 3000);
        QCOMPARE(completed.count(), 2);
        QVERIFY(second.isLoaded());
        QVERIFY(second.cachedRows().columns.contains("order_no"));
        QVERIFY(!first.isLoaded());
        QCOMPARE(firstFinished.count(), 0);
    }
    void connectionDraftCanBeTestedWithoutOpeningOrSavingIt() {
        QTemporaryDir dir;navidog::ConnectionProfile profile;profile.id="test-only";profile.name="Test draft";profile.database=navidog::ensureDemoDatabase(dir.path());
        navidog::QueryService service;ConnectionDialog dialog(profile,false,{},service);dialog.show();
        QSignalSpy tested(&service,&navidog::QueryService::connectionTestCompleted);
        auto* test=dialog.findChild<QPushButton*>("testConnection");QVERIFY(test);test->click();QTRY_COMPARE_WITH_TIMEOUT(tested.count(),1,3000);
        QCOMPARE(service.sessionCount(),0);QVERIFY(!service.isConnected(profile.id));QVERIFY(dialog.findChild<QLabel*>("connectionTestResult")->text().startsWith("连接成功"));
        auto* path=dialog.findChild<QLineEdit*>("databasePath");path->setText(dir.filePath("missing.db"));
        auto* buttons=dialog.findChild<QDialogButtonBox*>();buttons->button(QDialogButtonBox::Save)->click();QVERIFY(dialog.isVisible());QVERIFY(!QFileInfo::exists(path->text()));
        dialog.findChild<QPushButton*>("connectionBack")->click();dialog.findChild<QToolButton*>("choose_mysql")->click();dialog.findChild<QPushButton*>("connectionNext")->click();dialog.findChild<QLineEdit*>("initialDatabase")->clear();
        QCOMPARE(dialog.findChild<QSpinBox*>("connectionPort")->value(),3306);
        buttons->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));QVERIFY(dialog.profile().database.isEmpty());
    }
    void connectionSettingsRoundTripAndTunnelExclusion(){
        using namespace navidog;ConnectionProfile p;p.driver=DatabaseKind::PostgreSQL;p.name="Owned PG";p.database="postgres";p.host="db.example";p.advanced.readTimeout=0;p.advanced.connectTimeout=12;p.notes="连接备注";p.ssl.serverName="tls.example";QueryService service;ConnectionDialog dialog(p,true,{},service);dialog.show();auto* tabs=dialog.findChild<QTabWidget*>("connectionSettingsTabs");QStringList labels;for(int i=0;i<tabs->count();++i)labels<<tabs->tabText(i);QCOMPARE(labels,QStringList({"常规","高级","SSL","SSH","HTTP","备注"}));auto actual=dialog.profile();QCOMPARE(actual.notes,p.notes);QCOMPARE(actual.advanced,p.advanced);QCOMPARE(actual.ssl,p.ssl);
        auto* ssh=dialog.findChild<QCheckBox*>("connection_useSsh");auto* http=dialog.findChild<QCheckBox*>("connection_useHttp");ssh->setChecked(true);QVERIFY(!http->isEnabled());ssh->setChecked(false);http->setChecked(true);QVERIFY(!ssh->isEnabled());QVERIFY(dialog.profile().http.enabled);QCOMPARE(service.sessionCount(),0);
        ConnectionProfile oracle;oracle.driver=DatabaseKind::Oracle;oracle.oracle.addressKind=OracleAddressKind::TNS;oracle.database="owned_tns";oracle.oracle.role=OracleRole::SysDBA;ConnectionDialog oracleDialog(oracle,true,{},service);QVERIFY(!oracleDialog.findChild<QLineEdit*>("connectionHost")->isEnabled());QCOMPARE(oracleDialog.profile().oracle.addressKind,OracleAddressKind::TNS);QCOMPARE(oracleDialog.profile().oracle.role,OracleRole::SysDBA);
    }
    void oceanBaseTenantModeCanBeSelectedAndEdited() {
        using namespace navidog;
        using K=DatabaseKind;
        QueryService service;
        ConnectionProfile initial;
        ConnectionDialog dialog(initial,false,{},service,nullptr,true);
        dialog.show();
        auto* card=dialog.findChild<QToolButton*>("choose_oceanbase");
        auto* next=dialog.findChild<QPushButton*>("connectionNext");
        QVERIFY(card && next);
        card->click();next->click();
        auto* mode=dialog.findChild<QComboBox*>("oceanBaseTenantMode");
        QVERIFY(mode && mode->isVisible());
        QCOMPARE(dialog.profile().driver,K::OceanBase);
        QCOMPARE(dialog.profile().port,2881);
        dialog.findChild<QLineEdit*>("connectionHost")->setText("test.example");
        dialog.findChild<QSpinBox*>("connectionPort")->setValue(3881);
        dialog.findChild<QLineEdit*>("connectionUser")->setText("mysql_user@test");
        dialog.findChild<QLineEdit*>("connectionPassword")->setText("fixture-draft");
        mode->setCurrentIndex(1);
        auto oracle=dialog.profile();
        QCOMPARE(oracle.driver,K::OceanBaseOracle);
        QCOMPARE(oracle.host,QString("test.example"));
        QCOMPARE(oracle.port,3881);
        QVERIFY(oracle.user.isEmpty());
        QVERIFY(oracle.password.isEmpty());
        QVERIFY(oracle.database.isEmpty());
        QVERIFY(!dialog.findChild<QLineEdit*>("databasePath")->isVisible());
        mode->setCurrentIndex(0);
        QCOMPARE(dialog.profile().user,QString("mysql_user@test"));
        QCOMPARE(dialog.profile().password,QString("fixture-draft"));
        mode->setCurrentIndex(1);
        dialog.findChild<QLineEdit*>("connectionUser")->setText("APP@tenant");
        oracle=dialog.profile();
        ConnectionDialog edit(oracle,true,{},service);
        edit.show();
        auto* savedMode=edit.findChild<QComboBox*>("oceanBaseTenantMode");
        QVERIFY(savedMode && savedMode->isVisible());
        QCOMPARE(savedMode->currentIndex(),1);
        QCOMPARE(edit.profile().driver,K::OceanBaseOracle);
        QCOMPARE(edit.profile().user,QString("APP@tenant"));
        QCOMPARE(service.sessionCount(),0);
    }
    void damengConnectionKeepsSslAndSshOptions() {
        navidog::ConnectionProfile profile;
        profile.driver = navidog::DatabaseKind::Dameng;
        profile.tls = false;
        navidog::QueryService service;
        ConnectionDialog dialog(profile, true, {}, service);
        dialog.show();
        auto* tabs = dialog.findChild<QTabWidget*>("connectionSettingsTabs");
        QStringList labels;
        for (int i = 0; i < tabs->count(); ++i)
            labels << tabs->tabText(i);
        QCOMPARE(labels, QStringList({"常规","高级","SSL","SSH","备注"}));
        tabs->setCurrentIndex(labels.indexOf("SSL"));
        auto* tls = dialog.findChild<QCheckBox*>("connectionTls");
        auto* store = dialog.findChild<QLineEdit*>("connection_damengKeystoreDirectory");
        auto* password = dialog.findChild<QLineEdit*>("connection_damengKeystorePassword");
        QVERIFY(store && store->isVisible());
        QVERIFY(!store->isEnabled());
        QVERIFY(password && password->echoMode() == QLineEdit::Password);
        QVERIFY(!dialog.findChild<QComboBox*>("connection_sslVerification")->isVisible());
        tls->setChecked(true);
        QVERIFY(store->isEnabled());
        store->setText("/fixtures/client");
        password->setText("fixture-secret");
        dialog.findChild<QCheckBox*>("connection_saveDamengKeystorePassword")->setChecked(true);
        if (auto* ssh = dialog.findChild<QCheckBox*>("connection_useSsh")) {
            ssh->setChecked(true);
            QVERIFY(dialog.profile().ssh.enabled);
        }
        const auto configured = dialog.profile();
        QVERIFY(configured.tls);
        QCOMPARE(configured.ssl.damengKeystoreDirectory, "/fixtures/client");
        QCOMPARE(configured.ssl.damengKeystorePassword, "fixture-secret");
        QVERIFY(configured.ssl.saveDamengKeystorePassword);
        tls->setChecked(false);
        QVERIFY(!dialog.profile().ssl.saveDamengKeystorePassword);
    }

    void connectionWizardPreservesDraftsAndFiltersTypes() {
        QTemporaryDir dir;navidog::ConnectionProfile p;p.database=navidog::ensureDemoDatabase(dir.path());
        navidog::QueryService service;ConnectionDialog dialog(p,false,{},service,nullptr,true);dialog.show();
        auto* next=dialog.findChild<QPushButton*>("connectionNext");QVERIFY(!next->isEnabled());QTest::keyClick(&dialog,Qt::Key_Return);QVERIFY(dialog.isVisible());QCOMPARE(dialog.result(),0);
        if(!qEnvironmentVariableIsEmpty("NAVIDOG_WIZARD_SCREENSHOT")){QApplication::processEvents();QVERIFY(dialog.grab().save(qEnvironmentVariable("NAVIDOG_WIZARD_SCREENSHOT")));}
        auto* search=dialog.findChild<QLineEdit*>("connectionTypeSearch");search->setText("postgres");
        auto* pg=dialog.findChild<QToolButton*>("choose_postgres");QVERIFY(pg->isVisible());QVERIFY(!dialog.findChild<QToolButton*>("choose_mysql")->isVisible());
        pg->setChecked(true);QVERIFY(next->isEnabled());next->click();if(!qEnvironmentVariableIsEmpty("NAVIDOG_PG_FORM_SCREENSHOT")){QApplication::processEvents();QVERIFY(dialog.grab().save(qEnvironmentVariable("NAVIDOG_PG_FORM_SCREENSHOT")));}QCOMPARE(dialog.profile().driver,navidog::DatabaseKind::PostgreSQL);QCOMPARE(dialog.profile().port,5432);QCOMPARE(dialog.profile().database,QString("postgres"));
        auto* host=dialog.findChild<QLineEdit*>("connectionHost");host->setText("draft-host");
        dialog.findChild<QLineEdit*>("connectionPassword")->setText("draft-only");
        dialog.findChild<QPushButton*>("connectionBack")->click();search->clear();
        dialog.findChild<QToolButton*>("choose_mysql")->click();next->click();QCOMPARE(dialog.profile().port,3306);QVERIFY(dialog.profile().password.isEmpty());QCOMPARE(dialog.profile().user,QString("root"));
        dialog.findChild<QPushButton*>("connectionBack")->click();
        search->setText("TiDB");
        auto* tidb=dialog.findChild<QToolButton*>("choose_tidb");
        QVERIFY(tidb && tidb->isVisible());
        tidb->click();next->click();
        QCOMPARE(dialog.profile().driver,navidog::DatabaseKind::TiDB);
        QCOMPARE(dialog.profile().port,4000);
        QVERIFY(dialog.profile().password.isEmpty());
        search->clear();
        dialog.findChild<QPushButton*>("connectionBack")->click();pg->click();next->click();QCOMPARE(host->text(),QString("draft-host"));QCOMPARE(dialog.profile().password,QString("draft-only"));
        QCOMPARE(service.sessionCount(),0);dialog.reject();QVERIFY(!QFileInfo::exists(dir.filePath("navidog.sqlite")));
    }
    void selectedConnectionLifecyclePreservesOtherConnectionsAndTabs() {
        QTemporaryDir dir;auto path=navidog::ensureDemoDatabase(dir.path());navidog::ConnectionProfile a,b;a.id="owned-a";a.name="Owned A";a.database=path;b=a;b.id="owned-b";b.name="Owned B";
        navidog::ProfileStore store(dir.filePath("navidog.sqlite"));store.save({a,b});MainWindow window(dir.path());window.show();
        auto* tree=window.findChild<QTreeWidget*>("connectionTree");auto* itemA=tree->topLevelItem(1);auto* itemB=tree->topLevelItem(2);
        auto* open=window.findChild<QAction*>("menuOpenConnection");auto* close=window.findChild<QAction*>("menuCloseConnection");
        tree->setCurrentItem(itemA);QVERIFY(open->isEnabled());QVERIFY(!close->isEnabled());open->trigger();QTRY_VERIFY_WITH_TIMEOUT(itemA->data(0,Qt::UserRole+6).toBool(),3000);QTRY_COMPARE_WITH_TIMEOUT(itemA->childCount(),1,3000);
        tree->setCurrentItem(itemB);open->trigger();QTRY_VERIFY_WITH_TIMEOUT(itemB->data(0,Qt::UserRole+6).toBool(),3000);QTRY_COMPARE_WITH_TIMEOUT(itemB->childCount(),1,3000);auto* cachedB=itemB->child(0);
        tree->setCurrentItem(itemA);QVERIFY(!open->isEnabled());QVERIFY(close->isEnabled());
        QTimer::singleShot(30,&window,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->button(QMessageBox::Yes)->click();});close->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!itemA->data(0,Qt::UserRole+6).toBool(),3000);QVERIFY(itemB->data(0,Qt::UserRole+6).toBool());QCOMPARE(itemB->child(0),cachedB);QCOMPARE(itemA->childCount(),0);
        QTimer::singleShot(30,&window,[]{if(auto* input=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())){input->setTextValue("Owned A renamed");input->accept();}});
        window.findChild<QAction*>("menuRenameConnection")->trigger();QCOMPARE(itemA->text(0),QString("Owned A renamed"));QVERIFY(itemB->data(0,Qt::UserRole+6).toBool());QCOMPARE(itemB->child(0),cachedB);
        tree->setCurrentItem(itemB);QTimer::singleShot(30,&window,[]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())){dialog->findChild<QLineEdit*>("connectionName")->setText("Owned B edited");dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();}});
        window.findChild<QAction*>("menuEditConnection")->trigger();QVERIFY(itemB->data(0,Qt::UserRole+6).toBool());QCOMPARE(itemB->child(0),cachedB);QCOMPARE(itemB->text(0),QString("Owned B edited"));
        tree->setCurrentItem(itemA);QTest::mouseClick(window.findChild<QToolButton*>("newQuery"),Qt::LeftButton);auto* tabs=window.findChild<QTabWidget*>("queryTabs");QCOMPARE(tabs->count(),2);
        QTimer::singleShot(30,&window,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->button(QMessageBox::Yes)->click();});window.findChild<QAction*>("menuDeleteConnection")->trigger();
        QCOMPARE(tabs->count(),1);QCOMPARE(store.load().size(),2);QVERIFY(QFileInfo::exists(path));QVERIFY(itemB->data(0,Qt::UserRole+6).toBool());QCOMPARE(itemB->child(0),cachedB);
        tree->setCurrentItem(cachedB);QVERIFY(!open->isEnabled());QVERIFY(!close->isEnabled());QVERIFY(!window.findChild<QAction*>("menuEditConnection")->isEnabled());
        if(!qEnvironmentVariableIsEmpty("NAVIDOG_CONNECTION_SCREENSHOT")){ConnectionDialog preview(b,true,{},*window.findChild<navidog::QueryService*>(),&window);preview.show();QApplication::processEvents();QVERIFY(preview.grab().save(qEnvironmentVariable("NAVIDOG_CONNECTION_SCREENSHOT")));}
    }
    void githubReleaseCheckUsesNumericVersionsAndIgnoresInvalidReleases_data() {
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<bool>("hasUpdate");
        QTest::addColumn<bool>("hasError");
        QTest::newRow("newer") << QByteArray(R"({"tag_name":"v0.53.100"})") << true << false;
        QTest::newRow("equal") << QByteArray(R"({"tag_name":"v0.53.93"})") << false << false;
        QTest::newRow("older") << QByteArray(R"({"tag_name":"v0.53.9"})") << false << false;
        QTest::newRow("preview") << QByteArray(R"({"tag_name":"v0.53.100","prerelease":true})") << false << true;
        QTest::newRow("invalid") << QByteArray(R"({"tag_name":"not-a-version"})") << false << true;
    }

    void githubReleaseCheckUsesNumericVersionsAndIgnoresInvalidReleases() {
        QFETCH(QByteArray, response);
        QFETCH(bool, hasUpdate);
        QFETCH(bool, hasError);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                if (!socket->readAll().startsWith("GET "))
                    return;
                ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(response.size()) +
                              "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });

        ReleaseChecker checker("0.53.93", QUrl(QString("http://127.0.0.1:%1/releases/latest").arg(server.serverPort())));
        std::optional<ReleaseCheckResult> result;
        connect(&checker, &ReleaseChecker::finished, this,
                [&](const ReleaseCheckResult &value) { result = value; });
        checker.check();
        checker.check();
        QVERIFY(!result.has_value());
        QTRY_VERIFY(result.has_value());
        QCOMPARE(requests, 1);
        QCOMPARE(!result->error.isEmpty(), hasError);
        QCOMPARE(!result->releasePage.isEmpty(), hasUpdate);
        if (hasUpdate)
            QCOMPARE(result->releasePage, QUrl("https://github.com/duhbbx/Navidog/releases/tag/v0.53.100"));
    }

    void startupLogOpensWithCurrentLaunchTimings() {
        QTemporaryDir directory;
        StartupTiming startup;
        startup.mark("launch_timing_regression");
        startup.mark("short_stage");
        MainWindow window(directory.path(), nullptr, &startup);
        window.show();
        window.findChild<QAction *>("menuStartupLog")->trigger();
        auto *log = window.findChild<QPlainTextEdit *>("startupLogText");
        QVERIFY(log && log->isVisible() && log->isReadOnly());
        QVERIFY(log->toPlainText().contains("launch_timing_regression"));
        QVERIFY(log->toPlainText().contains("elapsed_ms"));
        QVERIFY(log->toPlainText().contains("total_ms"));
        const QFontMetricsF metrics(log->document()->defaultFont());
        QCOMPARE(metrics.horizontalAdvance('i'), metrics.horizontalAdvance('W'));

        const auto columnPosition = [log](const QTextBlock &block, int offset) {
            QTextCursor cursor(block);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, offset);
            return log->cursorRect(cursor).x();
        };
        const auto header = log->document()->findBlockByNumber(1);
        const auto firstStage = header.next();
        const auto secondStage = firstStage.next();
        const QRegularExpression rowPattern(QStringLiteral(R"(^\S+ +(\S+) +(\S+)$)"));
        const auto headings = rowPattern.match(header.text());
        const auto first = rowPattern.match(firstStage.text());
        const auto second = rowPattern.match(secondStage.text());
        QVERIFY(headings.hasMatch() && first.hasMatch() && second.hasMatch());
        for (auto column : {1, 2}) {
            const auto expected = columnPosition(header, int(headings.capturedStart(column)));
            QCOMPARE(columnPosition(firstStage, int(first.capturedStart(column))), expected);
            QCOMPARE(columnPosition(secondStage, int(second.capturedStart(column))), expected);
        }
    }

    void operationTimingsShowThreadTypeAndPersistOnShutdown() {
        QTemporaryDir directory;
        const auto previous =
            QByteArray("time_utc\tthread\toperation\telapsed_ms\n"
                       "2025-01-01T00:00:00.000Z\tworker\ter_export\t7841\n");
        const auto path = directory.filePath("operation-timing.log");
        {
            QFile logFile(path);
            QVERIFY(logFile.open(QIODevice::WriteOnly));
            QCOMPARE(logFile.write(previous), previous.size());
        }

        {
            OperationMonitor monitor(directory.path());
            monitor.start();
            MainWindow window(directory.path());
            window.show();
            navidog::recordOperationTiming(navidog::TimedOperation::UiEventLoopDelay, 281);
            std::unique_ptr<QThread> work(QThread::create([] {
                navidog::recordOperationTiming(navidog::TimedOperation::QueryExecute, 314);
            }));
            work->start();
            QVERIFY(work->wait(3000));
            QTRY_VERIFY(navidog::operationLogSnapshot().text.contains("er_export\t7841"));

            window.findChild<QAction *>("menuOperationLog")->trigger();
            auto *log = window.findChild<QPlainTextEdit *>("operationLogText");
            QVERIFY(log && log->isVisible() && log->isReadOnly());
            QVERIFY(log->toPlainText().contains(QRegularExpression("ui +ui_event_loop_delay +281")));
            QVERIFY(log->toPlainText().contains(QRegularExpression("worker +query_execute +314")));
            navidog::recordOperationTiming(navidog::TimedOperation::ApplyCatalog, 217);
            window.findChild<QPushButton *>("refreshOperationLog")->click();
            QVERIFY(log->toPlainText().contains(QRegularExpression("ui +apply_catalog +217")));
        }
        QFile logFile(path);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        const auto persisted = logFile.readAll();
        QVERIFY(persisted.startsWith(previous));
        QVERIFY(persisted.contains("\tworker\tquery_execute\t314\n"));
        QVERIFY(persisted.contains("\tui\tapply_catalog\t217\n"));
    }

    void newConnectionEntriesWorkFromAnyNavigationSelection() {
        QTemporaryDir directory;
        MainWindow window(directory.path());
        window.show();
        auto *tree = window.findChild<QTreeWidget *>("connectionTree");
        auto *quickEntry = window.findChild<QToolButton *>("addConnection");
        auto *menuEntry = window.findChild<QAction *>("menuNewConnection");
        QVERIFY(quickEntry->isEnabled());

        auto *connection = tree->topLevelItem(0);
        connection->setExpanded(true);
        QTRY_COMPARE(connection->childCount(), 1);
        auto *database = connection->child(0);
        database->setExpanded(true);
        QTRY_VERIFY(database->childCount() > 0);
        auto *tables = database->child(0);
        tables->setExpanded(true);
        QTRY_VERIFY(tables->childCount() > 0);
        auto *table = tables->child(0);
        for (auto *item : {connection, database, tables, table}) {
            tree->setCurrentItem(item);
            QVERIFY(quickEntry->isEnabled());
            QVERIFY(menuEntry->isEnabled());
        }

        const auto opensConnectionWizard = [&](auto open) {
            bool dialogOpened = false;
            QTimer dismiss;
            dismiss.setSingleShot(true);
            connect(&dismiss, &QTimer::timeout, &window, [&] {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                    dialogOpened = dialog->findChild<QPushButton *>("connectionNext") != nullptr;
                    dialog->reject();
                }
            });
            dismiss.start(30);
            open();
            return dialogOpened;
        };
        QVERIFY(opensConnectionWizard([&] { QTest::mouseClick(quickEntry, Qt::LeftButton); }));
        QVERIFY(opensConnectionWizard([&] { menuEntry->trigger(); }));

        tree->clearSelection();
        QCOMPARE(tree->currentItem(), table);
        QVERIFY(tree->selectedItems().isEmpty());
        QVERIFY(quickEntry->isEnabled());
        QVERIFY(menuEntry->isEnabled());
        QVERIFY(opensConnectionWizard([&] { QTest::mouseClick(quickEntry, Qt::LeftButton); }));
    }

    void saveConnectionThroughDialog() {
        QTemporaryDir directory; MainWindow window(directory.path()); window.show();
        bool filled = false;
        QTimer::singleShot(50, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* next=dialog->findChild<QPushButton*>("connectionNext");
            if(!next || next->isEnabled()){dialog->reject();return;}
            dialog->findChild<QToolButton*>("choose_mysql")->click();next->click();
            auto* versions=dialog->findChild<QComboBox*>("versionSelect");
            if(!versions || versions->count()!=5){dialog->reject();return;}
            dialog->findChild<QPushButton*>("connectionBack")->click();
            dialog->findChild<QToolButton*>("choose_sqlite")->click();next->click();
            if(versions->count()!=2){dialog->reject();return;}
            auto* name = dialog->findChild<QLineEdit*>("connectionName");
            auto* database = dialog->findChild<QLineEdit*>("databasePath");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            if (!name || !database || !buttons) { dialog->reject(); return; }
            name->setText("Local test connection"); database->setText(directory.filePath("harbor-demo.db")); filled = true;
            QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        });
        QTest::mouseClick(window.findChild<QToolButton*>("addConnection"), Qt::LeftButton);
        QVERIFY(filled);
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QCOMPARE(tree->topLevelItemCount(), 2);
        navidog::ProfileStore store(directory.filePath("navidog.sqlite")); QCOMPARE(store.load().size(), 2);
    }
    void connectionArchiveMenuExportsAndImportsEncryptedProfiles() {
        using namespace navidog;
        QTemporaryDir sourceDirectory, destinationDirectory;
        QVERIFY(sourceDirectory.isValid() && destinationDirectory.isValid());
        const auto file = sourceDirectory.filePath("transfer.ndconn");
        const auto passphrase = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ConnectionProfile original;
        original.id = "transfer-source";
        original.name = "Transfer connection";
        original.groupId = "transfer-group";
        original.driver = DatabaseKind::MySQL;
        original.host = "db.example.invalid";
        original.user = "transfer-user";
        original.password = QUuid::createUuid().toString(QUuid::WithoutBraces);
        original.savePassword = original.secretsLoaded = true;
        original.credentialSource = CredentialSource::LocalEncrypted;
        const auto sourcePath = localStorePath(sourceDirectory.path());
        ProfileStore(sourcePath).saveWorkspace({{original}, {{original.groupId, "Transfer group"}}});
        MainWindow source(sourceDirectory.path());
        source.show();

        bool includeWasOptional = false;
        const auto driveTransfer = [&](MainWindow& window, const char* action) {
            auto* command = window.findChild<QAction*>(action);
            QVERIFY(command);
            QTimer reply;
            int attempts = 0;
            connect(&reply, &QTimer::timeout, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (++attempts >= 120) { dialog->reject(); return; }
                if (dialog->objectName() == "exportConnectionsDialog") {
                    auto* include = dialog->findChild<QCheckBox*>("includeConnectionPasswords");
                    includeWasOptional = include && !include->isChecked();
                    include->setChecked(true);
                    dialog->findChild<QLineEdit*>("connectionArchivePassword")->setText(passphrase);
                    dialog->findChild<QLineEdit*>("confirmConnectionArchivePassword")->setText(passphrase);
                    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                } else if (auto* picker = qobject_cast<QFileDialog*>(dialog)) {
                    if (auto* name = picker->findChild<QLineEdit*>("fileNameEdit")) {
                        name->setText(file);
                        QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                    }
                } else if (auto* input = qobject_cast<QInputDialog*>(dialog)) {
                    input->setTextValue(passphrase);
                    input->accept();
                } else if (auto* error = qobject_cast<QMessageBox*>(dialog)) {
                    error->reject();
                }
            });
            reply.start(20);
            command->trigger();
        };
        driveTransfer(source, "menuExportConnections");
        QVERIFY(includeWasOptional);
        QVERIFY(QFileInfo::exists(file));

        MainWindow destination(destinationDirectory.path());
        destination.show();
        driveTransfer(destination, "menuImportConnections");
        auto workspace = ProfileStore(localStorePath(destinationDirectory.path())).loadWorkspace();
        const auto restored = std::find_if(workspace.connections.begin(), workspace.connections.end(),
            [&](const auto& profile) { return profile.name == original.name; });
        QVERIFY(restored != workspace.connections.end());
        QVERIFY(restored->id != original.id);
        loadConnectionSecrets(*restored);
        QCOMPARE(restored->password, original.password);
        auto* tree = destination.findChild<QTreeWidget*>("connectionTree");
        const auto groups = tree->findItems("Transfer group", Qt::MatchExactly, 0);
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.front()->childCount(), 1);
        QCOMPARE(groups.front()->child(0)->text(0), original.name);
    }
    void connectionControllerDuplicatesWithoutReusingRuntimeCredentials() {
        using namespace navidog;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = localStorePath(directory.path());
        ProfileStore store(path);
        QueryService service;
        QWidget owner;
        ConnectionProfile original;
        original.id = "owned-original";
        original.name = "Owned MySQL";
        original.driver = DatabaseKind::MySQL;
        original.user = "owned_user";
        original.port = 3306;
        original.password = "runtime-fixture-only";
        ConnectionProfile existingCopy = original;
        existingCopy.id = "owned-copy";
        existingCopy.name += " 副本";
        QVector<ConnectionProfile> profiles{original, existingCopy};
        bool workspaceEditRequested = false;
        ConnectionController controller(service, store, profiles, path,
            {[] { return QVector<ConnectionGroup>{}; }, [] { return false; },
             [&](const QString &) { workspaceEditRequested = true; return false; }},
            &owner);
        QSignalSpy saved(&controller, &ConnectionController::connectionSaved);
        bool sanitized = false;
        QTimer::singleShot(30, &owner, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *name = dialog->findChild<QLineEdit *>("connectionName");
            auto *password = dialog->findChild<QLineEdit *>("connectionPassword");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (!name || !password || !buttons) { dialog->reject(); return; }
            sanitized = name->text() == "Owned MySQL 副本 2" && password->text().isEmpty();
            if (sanitized)
                buttons->button(QDialogButtonBox::Save)->click();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        controller.duplicate(original.id);
        QVERIFY(sanitized);
        QVERIFY(!workspaceEditRequested);
        QCOMPARE(saved.count(), 1);
        const auto persisted = store.load();
        QCOMPARE(persisted.size(), 3);
        QVERIFY(persisted.back().id != original.id);
        QCOMPARE(persisted.back().name, QString("Owned MySQL 副本 2"));
        QVERIFY(persisted.back().password.isEmpty());
        QCOMPARE(service.sessionCount(), 0);
    }

    void existingConnectionsCanBeEditedWithoutSavingPassword() {
        QTemporaryDir dir;navidog::ConnectionProfile p;p.id="edit-owned";p.name="Before";p.database=navidog::ensureDemoDatabase(dir.path());navidog::ProfileStore store(dir.filePath("navidog.sqlite"));store.save({p});MainWindow window(dir.path());window.show();auto* tree=window.findChild<QTreeWidget*>("connectionTree");tree->setCurrentItem(tree->topLevelItem(1));auto* edit=window.findChild<QAction*>("menuEditConnection");QVERIFY(edit->isEnabled());bool filled=false;
        QTimer::singleShot(30,&window,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto* name=dialog->findChild<QLineEdit*>("connectionName");auto* password=dialog->findChild<QLineEdit*>("connectionPassword");if(!name || !password){dialog->reject();return;}QCOMPARE(name->text(),QString("Before"));name->setText("After");password->setText("never-save-test-password");filled=true;dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});edit->trigger();QVERIFY(filled);auto saved=store.load();QCOMPARE(saved.size(),2);auto it=std::find_if(saved.begin(),saved.end(),[](auto x){return x.id=="edit-owned";});QVERIFY(it!=saved.end());QCOMPARE(it->name,QString("After"));QVERIFY(it->password.isEmpty());
    }
};
QTEST_MAIN(UiTest)
#include "ui_test.moc"
