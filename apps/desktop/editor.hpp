#pragma once
#include <QPlainTextEdit>
#include <QHash>
#include <QTextBlock>
#include "navidog/sql/compiler/formatter.hpp"
#include "editor_diagnostics.hpp"
#include "editor_completion.hpp"
#include <optional>
class QContextMenuEvent;
class QDialog;
class QLineEdit;
class QCheckBox;
class QLabel;
class QHideEvent;
class QTreeView;
namespace navidog::ui { class DiagnosticCard; }

class SqlHighlighter;
class SqlReadOnlyView final : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit SqlReadOnlyView(QWidget* parent=nullptr);
    void setDialect(navidog::sql::compiler::Dialect);
    void setDatabaseKind(navidog::DatabaseKind);
    void setFormatStyle(const navidog::sql::compiler::FormatStyle &);
    void setSourceSql(const QString &);
    void setFormattingEnabled(bool enabled);
    QWidget *withActions(bool showCopy = true);
    void formatSql();
    void copySql();
signals:
    void formatStatusChanged(const QString &);
    void formatAvailabilityChanged();
protected:
    void contextMenuEvent(QContextMenuEvent *) override;
private:
    bool canFormat() const;
    SqlHighlighter* highlighter_;
    navidog::sql::compiler::Dialect dialect_ = navidog::sql::compiler::Dialect::PostgreSQL;
    std::optional<navidog::sql::compiler::FormatStyle> formatStyle_;
    QString sourceSql_;
    quint64 formatContext_ = 0;
    bool formattingEnabled_ = true, formatting_ = false;
};

class SqlEditor final : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit SqlEditor(QWidget* parent = nullptr);
    QString selectedSqlOrAll() const;
    static constexpr int largeTextThreshold = 1024 * 1024;
    static constexpr int largeLineCount = 50000;
    static constexpr int highlightedLinePrefix = 8192;
    static constexpr int sqlSelectionLimit = 256 * 1024;
    bool largeDocument() const { return largeDocument_; }
    bool hasBoundedSqlSelection() const;
    void installLargeDocument(QTextDocument *source);
    void setPlainText(const QString &text);

    void applyPreferences(int fontSize,int indentWidth,bool wordWrap,bool completion=true,int completionPrefix=1);
    void setSqlToolsEnabled(bool conversion,bool optimization){conversionEnabled_=conversion;optimizationEnabled_=optimization;}
    void paintGutter(QPaintEvent*);
    int gutterWidth() const;
    void showFind(bool replace=false);
    void showCompletion(){if(!isReadOnly() && !largeDocument_){setFocus();completionController_->request(SqlCompletionController::Activation::Manual);}}
    bool findNext(bool backward=false);
    void replaceCurrent();
    void replaceAll();
    void indentLines(bool remove=false);
    void toggleLineComment();
    void goToLine();
    void setHighlightDialect(navidog::sql::compiler::Dialect);
    navidog::sql::compiler::Dialect highlightDialect() const { return highlightDialect_; }
    void setDiagnosticContext(navidog::DatabaseKind, const QString& database={}, const QString& schema={},
                              bool calciteExtensions=false);
    void setMetadataSyncState(const QString&);
    void setMetadataSnapshot(navidog::MetadataSnapshotPtr);
    const QVector<SqlEditorDiagnostic>& diagnostics() const { return diagnostics_; }
    QString diagnosticAtPosition(int position) const;
    QString diagnosticStatus() const { return diagnosticStatus_; }
signals:
    void largeDocumentChanged(bool);
    void highlightDialectChanged();
    void sourceDocumentReplaced();
    void diagnosticsChanged();
    void diagnosticStatusChanged(const QString&);
    void conversionRequested();
    void optimizationRequested();

protected:
    bool eventFilter(QObject*,QEvent*) override;
    void paintEvent(QPaintEvent*) override;
    bool viewportEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void hideEvent(QHideEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    SqlHighlighter* highlighter_;
    void enableLargeDocument();
    void checkDocumentSize(int position);
    void observeDocument();
    void highlightVisibleSql();
    struct HighlightedBlock {
        QTextBlock block;
        bool commentBefore;
        bool commentAfter;
    };
    QHash<int, HighlightedBlock> highlightedBlocks_;
    bool largeDocument_ = false;
    bool highlightInvalidated_ = true;
    int gutterBlockCountHint_ = 0;
    navidog::sql::compiler::Dialect highlightDialect_ = navidog::sql::compiler::Dialect::PostgreSQL;
    int indentWidth_=4;
    bool completionEnabled_=true;
    int completionPrefix_=1;
    bool conversionEnabled_=false,optimizationEnabled_=false;
    SqlDiagnosticController* diagnosticController_;
    QVector<SqlEditorDiagnostic> diagnostics_;
    QString diagnosticStatus_;
    void updateEditorSelections();
    void scheduleEditorSelections();
    void requestCompletionForText(const QString&);
    bool selectionUpdatePending_=false;
    navidog::ui::DiagnosticCard* diagnosticCard_;
    QWidget* gutter_;
    QTreeView* completionList_;
    SqlCompletionController* completionController_;
    int completionRevision_=0;
    void acceptCompletion(const QModelIndex&);
    void showCompletionList();
    void dismissCompletion();
    QDialog* findDialog_=nullptr;
    QLineEdit *findText_=nullptr,*replaceText_=nullptr;
    QCheckBox *findCase_=nullptr,*findWord_=nullptr;
    QLabel* findStatus_=nullptr;
    QWidget* replaceRow_=nullptr;
    void updateMargins();
    void transformLines(int operation);
};
