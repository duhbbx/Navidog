#include "sql_outline.hpp"
#include "components.hpp"
#include "editor.hpp"
#include "navidog/sql/compiler/outline.hpp"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPersistentModelIndex>
#include <QRunnable>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QTextBlock>
#include <QThreadPool>
#include <QVBoxLayout>
#include <chrono>

namespace sc = navidog::sql::compiler;

namespace {
constexpr int outlineBytes = 1024 * 1024;
constexpr int outlineNodes = 2000;
enum ItemRole { BeginRole = Qt::UserRole, EndRole, KeyRole };

struct OutlineResult {
    sc::SqlOutline outline;
    QVector<int> offsets;
};

class OutlineTask final : public QObject, public QRunnable {
    Q_OBJECT
public:
    QString source;
    sc::Dialect dialect;
    std::shared_ptr<std::atomic_bool> cancel;
    std::shared_ptr<OutlineResult> result = std::make_shared<OutlineResult>();

    void run() override {
        try {
            const auto bytes = source.toUtf8();
            if (bytes.size() > outlineBytes) {
                result->outline.truncated = true;
            } else if (!cancel->load()) {
                sc::LanguageOptions options;
                options.limits.sourceBytes = outlineBytes;
                options.limits.nodes = outlineNodes;
                options.limits.tokens = 60000;
                options.limits.nesting = 40;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(450);
                options.shouldStop = [flag = cancel, deadline] {
                    return flag->load() || std::chrono::steady_clock::now() >= deadline;
                };
                result->outline = sc::outlineSql(dialect, bytes.toStdString(), options);
                if (!cancel->load() && !result->outline.roots.empty()) {
                    result->offsets.resize(bytes.size() + 1);
                    int utf16 = 0;
                    for (int i = 0; i < bytes.size(); ++i) {
                        const auto lead = static_cast<unsigned char>(bytes[i]);
                        result->offsets[i] = utf16;
                        if ((lead & 0xc0) != 0x80) utf16 += lead >= 0xf0 ? 2 : 1;
                    }
                    result->offsets[bytes.size()] = utf16;
                }
            }
        } catch (const std::exception &) {
            result->outline = {};
            result->outline.partial = true;
        }
        emit finished();
    }
signals:
    void finished();
};

QThreadPool &outlinePool() {
    static auto pool = [] {
        auto threads = std::make_unique<QThreadPool>();
        threads->setMaxThreadCount(2);
        return threads;
    }();
    return *pool;
}

void expandedKeys(QTreeWidgetItem &item, QSet<QString> &keys) {
    for (int i = 0; i < item.childCount(); ++i) {
        auto *child = item.child(i);
        if (child->isExpanded()) keys.insert(child->data(0, KeyRole).toString());
        expandedKeys(*child, keys);
    }
}

void populateTree(QTreeWidgetItem &parent, const std::vector<sc::SqlOutlineNode> &nodes,
                  const QVector<int> &offsets, const QTextDocument &document,
                  const QString &prefix, const QSet<QString> &expanded, int depth, bool first) {
    QHash<QString, int> repetitions;
    for (const auto &node : nodes) {
        if (node.range.begin >= node.range.end || node.range.end >= std::size_t(offsets.size()))
            continue;
        const int begin = offsets[int(node.range.begin)], end = offsets[int(node.range.end)];
        const auto label = QString::fromStdString(node.label).simplified();
        const auto key = prefix + "/" + label + "#" + QString::number(repetitions[label]++);
        auto *item = new QTreeWidgetItem(&parent, {label});
        item->setData(0, BeginRole, begin);
        item->setData(0, EndRole, end);
        item->setData(0, KeyRole, key);
        item->setToolTip(0, QString("第 %1 行 · %2")
            .arg(document.findBlock(begin).blockNumber() + 1)
            .arg(node.recovered ? QString("已识别的结构") : QString("点击定位并选中 SQL")));
        populateTree(*item, node.children, offsets, document, key, expanded, depth + 1, first);
        if (item->childCount() && ((first && depth <= 1) || expanded.contains(key)))
            item->setExpanded(true);
    }
}
} // namespace

SqlEditorArea::SqlEditorArea(SqlEditor *editor, QToolButton *outlineToggle, bool open, QWidget *parent)
    : QSplitter(Qt::Horizontal, parent), editor_(editor), toggle_(outlineToggle),
      outline_(new QWidget(this)), tree_(new navidog::ui::ObjectTree(outline_)),
      status_(new QLabel(outline_)) {
    setObjectName("sqlEditorArea");
    setChildrenCollapsible(false);
    outline_->setObjectName("sqlOutline");
    outline_->setMinimumWidth(156);
    auto *layout = new QVBoxLayout(outline_);
    layout->setContentsMargins(6, 5, 6, 4);
    layout->setSpacing(4);
    auto *title = new QLabel("SQL 结构", outline_);
    title->setObjectName("sectionTitle");
    layout->addWidget(title);
    tree_->setObjectName("sqlOutlineTree");
    tree_->setAccessibleName("SQL 结构导航");
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setMinimumWidth(0);
    layout->addWidget(tree_, 1);
    status_->setObjectName("sqlOutlineStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    addWidget(outline_);
    addWidget(editor_);
    setStretchFactor(0, 0);
    setStretchFactor(1, 1);
    setSizes({226, 650});

    toggle_->setCheckable(true);
    toggle_->setToolTip("显示或隐藏当前编辑器的 SQL 结构导航");
    connect(toggle_, &QToolButton::toggled, this, &SqlEditorArea::setOutlineOpen);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](auto *item) { locate(item, true); });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](auto *item) { locate(item, true); });
    connect(tree_, &QWidget::customContextMenuRequested, this, &SqlEditorArea::nodeMenu);
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout, this, &SqlEditorArea::launch);
    connect(editor_, &SqlEditor::highlightDialectChanged, this, &SqlEditorArea::refresh);
    connect(editor_, &SqlEditor::largeDocumentChanged, this, &SqlEditorArea::refresh);
    connect(editor_, &SqlEditor::sourceDocumentReplaced, this, &SqlEditorArea::observeDocument);
    observeDocument();
    setOutlineOpen(open);
}

SqlEditorArea::~SqlEditorArea() {
    if (cancel_) cancel_->store(true);
}

bool SqlEditorArea::outlineOpen() const { return !outline_->isHidden(); }

void SqlEditorArea::setOutlineOpen(bool open) {
    const bool changed = outlineOpen() != open;
    outline_->setVisible(open);
    if (toggle_->isChecked() != open) {
        const QSignalBlocker block(toggle_);
        toggle_->setChecked(open);
    }
    if (changed) refresh();
}

void SqlEditorArea::showEvent(QShowEvent *event) {
    QSplitter::showEvent(event);
    refresh();
}

void SqlEditorArea::hideEvent(QHideEvent *event) {
    QSplitter::hideEvent(event);
    debounce_.stop();
    if (cancel_) cancel_->store(true);
    ++generation_;
}

void SqlEditorArea::observeDocument() {
    disconnect(documentChanged_);
    documentChanged_ = connect(editor_->document(), &QTextDocument::contentsChanged, this, &SqlEditorArea::refresh);
    refresh();
}

QString SqlEditorArea::unavailableReason() const {
    if (editor_->largeDocument() || editor_->document()->characterCount() > outlineBytes + 1)
        return "大文件模式下暂停结构导航";
    if (editor_->document()->isEmpty()) return "输入 SQL 后显示结构";
    return {};
}

void SqlEditorArea::refresh() {
    ++generation_;
    debounce_.stop();
    if (cancel_) cancel_->store(true);
    if (!outlineOpen() || !isVisible()) return;
    tree_->setEnabled(false);
    const auto unavailable = unavailableReason();
    if (!unavailable.isEmpty()) {
        tree_->clear();
        status_->setText(unavailable);
        return;
    }
    status_->setText("正在更新结构…");
    debounce_.start(220);
}

void SqlEditorArea::launch() {
    if (running_ || !outlineOpen() || !isVisible() || !unavailableReason().isEmpty()) return;
    auto *task = new OutlineTask;
    task->dialect = editor_->highlightDialect();
    task->cancel = std::make_shared<std::atomic_bool>(false);
    task->source = editor_->toPlainText();
    const auto stamp = generation_;
    auto cancel = task->cancel;
    connect(task, &OutlineTask::finished, this, [this, result = task->result, stamp] {
        running_ = false;
        cancel_.reset();
        if (stamp == generation_ && outlineOpen() && isVisible())
            showOutline(result->outline, result->offsets);
        else if (!debounce_.isActive() && outlineOpen() && isVisible() && unavailableReason().isEmpty())
            debounce_.start(0);
    }, Qt::QueuedConnection);
    if (!outlinePool().tryStart(task)) {
        delete task;
        debounce_.start(120);
        return;
    }
    running_ = true;
    cancel_ = std::move(cancel);
}

void SqlEditorArea::showOutline(const sc::SqlOutline &result, const QVector<int> &offsets) {
    QSet<QString> expanded;
    expandedKeys(*tree_->invisibleRootItem(), expanded);
    const bool first = !tree_->topLevelItemCount();
    const auto selected = tree_->currentItem() ? tree_->currentItem()->data(0, KeyRole).toString() : QString{};
    tree_->setUpdatesEnabled(false);
    tree_->clear();
    populateTree(*tree_->invisibleRootItem(), result.roots, offsets, *editor_->document(), {}, expanded, 0, first);
    if (!selected.isEmpty()) {
        QTreeWidgetItemIterator iter(tree_);
        while (*iter && (*iter)->data(0, KeyRole).toString() != selected) ++iter;
        if (*iter) tree_->setCurrentItem(*iter);
    }
    tree_->setUpdatesEnabled(true);
    shownGeneration_ = generation_;
    tree_->setEnabled(tree_->topLevelItemCount() != 0);
    if (result.truncated)
        status_->setText("已达到本地分析上限，显示已找到的结构");
    else if (result.partial)
        status_->setText("部分语法未完整解析，显示已识别的结构");
    else if (!tree_->topLevelItemCount())
        status_->setText("暂无可导航语句");
    else
        status_->setText(QString("%1 条顶层语句").arg(tree_->topLevelItemCount()));
}

bool SqlEditorArea::current() const {
    return shownGeneration_ == generation_ && tree_->isEnabled() && outlineOpen();
}

void SqlEditorArea::locate(QTreeWidgetItem *item, bool select) {
    if (!item || !current()) return;
    QTextCursor cursor(editor_->document());
    const int begin = item->data(0, BeginRole).toInt(), end = item->data(0, EndRole).toInt();
    cursor.setPosition(select ? end : begin);
    if (select) cursor.setPosition(begin, QTextCursor::KeepAnchor);
    editor_->setTextCursor(cursor);
    editor_->setFocus();
    editor_->ensureCursorVisible();
}

void SqlEditorArea::copy(QTreeWidgetItem *item) {
    if (!item || !current()) return;
    QTextCursor cursor(editor_->document());
    cursor.setPosition(item->data(0, BeginRole).toInt());
    cursor.setPosition(item->data(0, EndRole).toInt(), QTextCursor::KeepAnchor);
    QApplication::clipboard()->setText(cursor.selectedText().replace(QChar::ParagraphSeparator, '\n'));
}

void SqlEditorArea::nodeMenu(const QPoint &position) {
    auto *item = tree_->itemAt(position);
    if (!item || !current()) return;
    tree_->setCurrentItem(item);
    const QPersistentModelIndex index(tree_->indexFromItem(item));
    navidog::ui::ContextMenu menu(this);
    auto *jump = menu.addAction("定位到开头", this, [this, index] { if (index.isValid()) locate(tree_->itemFromIndex(index), false); });
    jump->setObjectName("sqlOutlineLocate");
    auto *select = menu.addAction("选中 SQL", this, [this, index] { if (index.isValid()) locate(tree_->itemFromIndex(index), true); });
    select->setObjectName("sqlOutlineSelect");
    auto *copyAction = menu.addAction("复制 SQL", this, [this, index] { if (index.isValid()) copy(tree_->itemFromIndex(index)); });
    copyAction->setObjectName("sqlOutlineCopy");
    menu.execAt(tree_->viewport()->mapToGlobal(position), tree_);
}

#include "sql_outline.moc"
