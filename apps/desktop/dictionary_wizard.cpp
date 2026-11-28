#include "dictionary_wizard.hpp"
#include "components.hpp"
#include "database_job.hpp"
#include "dictionary_document.hpp"
#include "report_export.hpp"
#include "export_paths.hpp"
#include "task_center.hpp"
#include "navidog/local_store.hpp"

#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFormLayout>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <algorithm>
#include <array>

namespace navidog::ui {
namespace {
using K = DatabaseKind;
using Resolve = std::function<std::optional<ConnectionProfile>(const QString&, const CatalogNode&)>;
using KnownCatalog = std::function<QVector<CatalogNode>(const QString&)>;
constexpr auto presetKey = "dictionary-configurations";
constexpr qint64 configurationBytes = 8*1024*1024;
enum Role { Type = Qt::UserRole, Connection, DatabaseNode, Key, ObjectIndex, Fetched };
enum class Row { ConnectionGroup, Profile, Namespace, ServerInfo, ScopeInfo, Kind, Object };
enum Step { Scope, Objects, Templates, Layout, Export };
enum FormatPage { Cover, Contents, HeaderFooter, Body, Paper };

QString rowText(QTreeWidgetItem* item) { return item->text(0); }
Row rowType(const QTreeWidgetItem* item) { return Row(item->data(0, Type).toInt()); }
CatalogNode nodeFor(const QTreeWidgetItem* item) { return item->data(0,DatabaseNode).value<CatalogNode>(); }

QTreeWidgetItem* treeRow(QTreeWidgetItem* parent, Row type, const QString& label, const QIcon& icon = {}) {
    auto* item = new QTreeWidgetItem(parent, QStringList{label});
    item->setData(0, Type, int(type));
    item->setIcon(0,icon);
    return item;
}
void checkRow(QTreeWidgetItem* item, bool checked, bool group = false) {
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable | (group ? Qt::ItemIsAutoTristate : Qt::NoItemFlags));
    item->setCheckState(0,checked ? Qt::Checked : Qt::Unchecked);
}
QString scopeLabel(const CatalogNode& node) {
    return node.kind == "schema" ? node.schema : node.name.isEmpty() ? node.database : node.name;
}
bool separateSchemas(K kind) { return kind == K::PostgreSQL || kind == K::SQLServer; }

QString objectLabel(const DatabaseDictionaryTable& table) {
    return table.object.isEmpty() ? table.name : table.object;
}
void saveFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
}
QImage chooseImage(QWidget* parent) {
    const auto path = QFileDialog::getOpenFileName(parent,"选择图片",{},"图片 (*.png *.jpg *.jpeg *.webp *.bmp)");
    if (path.isEmpty()) return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || QFileInfo(path).size() > 24*1024*1024 ||
        qint64(size.width())*size.height() > 32000000)
        throw std::runtime_error("无法读取图片或图片尺寸过大");
    reader.setScaledSize(size.scaled(850,700,Qt::KeepAspectRatio));
    auto image = reader.read();
    if (image.isNull()) throw std::runtime_error(reader.errorString().toStdString());
    return image;
}

void filterTree(QTreeWidget* tree, const QString& term) {
    const auto filter = [&](const auto& self, QTreeWidgetItem* item, bool ancestor) -> bool {
        const bool matches = ancestor || term.isEmpty() || item->text(0).contains(term,Qt::CaseInsensitive) ||
                             item->toolTip(0).contains(term,Qt::CaseInsensitive);
        bool child = false;
        for (int i = 0; i < item->childCount(); ++i) child |= self(self,item->child(i),matches);
        item->setHidden(!matches && !child);
        if (child && !term.isEmpty() && !matches) item->setExpanded(true);
        return matches || child;
    };
    auto* root = tree->invisibleRootItem();
    for (int i = 0; i < root->childCount(); ++i) filter(filter,root->child(i),false);
}

class PaperPreview final : public QGraphicsView {
public:
    PaperPreview() {
        setScene(&scene_);
        setFrameShape(QFrame::NoFrame);
        setBackgroundBrush(QColor("#E9EFF4"));
        setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        sheet_ = scene_.addRect({},QPen(QColor("#C5D2DF")),QBrush(Qt::white));
        document_ = scene_.addText({});
        document_->document()->setDocumentMargin(0);
        pageNumber_ = scene_.addSimpleText("1");
        pageNumber_->setBrush(QColor("#526578"));
    }
    void showPage(const QString& html, const DictionaryLayout& layout, bool cover) {
        const auto page = layout.pageLayout().fullRect(QPageLayout::Point);
        const auto margins = layout.pageLayout().margins(QPageLayout::Point);
        document_->setPos(margins.left(),margins.top());
        document_->setTextWidth(page.width()-margins.left()-margins.right());
        document_->setHtml(html);
        pageHeight_ = page.height();
        sheet_->setRect(0,0,page.width(),qMax(page.height(),document_->boundingRect().height()+margins.top()+margins.bottom()));
        pageNumber_->setVisible(!cover && layout.pageNumbers);
        auto footerFont = QFont(layout.footerFont.family.isEmpty() ? QApplication::font().family() : layout.footerFont.family,
                               layout.footerFont.size);
        footerFont.setBold(layout.footerFont.bold);
        pageNumber_->setFont(footerFont);
        pageNumber_->setPos(page.width()-margins.right()-pageNumber_->boundingRect().width(),
                            sheet_->rect().height()-margins.bottom()/2-pageNumber_->boundingRect().height()/2);
        setSceneRect(sheet_->rect().adjusted(-12,-12,12,12));
        fit();
    }
    void setZoom(int index) { mode_ = index; factor_ = 1.; fit(); }
    void changeZoom(qreal factor) { factor_ = qBound(0.4,factor_*factor,3.5); fit(); }
protected:
    void resizeEvent(QResizeEvent* event) override { QGraphicsView::resizeEvent(event); fit(); }
private:
    void fit() {
        if (!sheet_) return;
        const auto rect = sheet_->rect();
        if (rect.isEmpty()) return;
        const auto width = qMax(1,viewport()->width()-18) / rect.width();
        const auto height = qMax(1,viewport()->height()-18) / pageHeight_;
        const auto scale = (mode_ == 2 ? 1. : mode_ == 1 ? width : qMin(width,height))*factor_;
        setTransform(QTransform::fromScale(scale,scale));
    }
    QGraphicsScene scene_;
    QGraphicsRectItem* sheet_ = nullptr;
    QGraphicsTextItem* document_;
    QGraphicsSimpleTextItem* pageNumber_;
    int mode_ = 0;
    qreal factor_ = 1., pageHeight_ = 1.;
};

struct LoadedScope {
    DictionaryScopeChoice choice;
    DatabaseDictionary dictionary;
};

class DictionaryWizard final : public Dialog {
public:
    DictionaryWizard(QWidget* parent, const QVector<ConnectionProfile>& profiles, const QString& profile,
                     const CatalogNode& initial, QString storePath, Resolve resolve, KnownCatalog known)
        : Dialog(parent), storePath_(std::move(storePath)), resolve_(std::move(resolve)), known_(std::move(known)) {
        setObjectName("databaseDictionary");
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle("数据字典");
        setMinimumSize(775,555);
        resize(1050,720);
        for (const auto& p : profiles) {
            profiles_[p.id] = {p.name,p.driver,p.database};
            profileOrder_ << p.id;
        }
        if (!profile.isEmpty() && (initial.kind == "schema" || initial.kind == "database")) {
            const auto info = profiles_.value(profile);
            const auto database = info.driver == K::SQLite ? QString("main")
                : usesOracleSql(info.driver) ? info.defaultDatabase : initial.database;
            const auto schema = initial.kind == "schema" ? initial.schema : QString{};
            config_.scopes.push_back({profile,database,schema,true,{},{}});
        }
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0,0,0,0);
        root->setSpacing(0);
        auto* top = strip("#edf3f8");
        auto* topLayout = new QVBoxLayout(top);
        topLayout->setContentsMargins(22,13,22,12);
        topLayout->setSpacing(5);
        steps_ = new QLabel;
        steps_->setStyleSheet("color:#4e6880");
        heading_ = new QLabel;
        auto font = heading_->font();
        font.setPointSize(font.pointSize()+4);
        font.setBold(true);
        heading_->setFont(font);
        topLayout->addWidget(steps_);
        topLayout->addWidget(heading_);
        root->addWidget(top);
        pages_ = new QStackedWidget;
        root->addWidget(pages_,1);
        scopeTree_ = selectionPage("dictionaryScopeTree",false);
        objectTree_ = selectionPage("dictionaryObjectTree",true);
        buildTemplatePage();
        buildLayoutPage();
        buildExportPage();

        auto* bottom = strip("#edf3f8");
        auto* controls = new QHBoxLayout(bottom);
        controls->setContentsMargins(16,9,16,9);
        save_ = menuButton("保存配置","dictionarySaveConfiguration", [this](QMenu* menu) {
            menu->addAction("保存为本地方案…",this,[this] { savePreset(); });
            menu->addAction("另存为配置文件…",this,[this] { saveConfigurationFile(); });
        });
        load_ = menuButton("加载配置","dictionaryLoadConfiguration", [this](QMenu* menu) {
            menu->addAction("选择本地方案…",this,[this] { loadPreset(); });
            menu->addAction("从配置文件加载…",this,[this] { loadConfigurationFile(); });
        });
        controls->addWidget(save_);
        controls->addWidget(load_);
        status_ = new QLabel;
        status_->setObjectName("dictionaryStatus");
        status_->setWordWrap(true);
        status_->setTextFormat(Qt::PlainText);
        controls->addWidget(status_,1);
        previous_ = new Button("上一步","dictionaryPrevious");
        next_ = new Button("下一步","dictionaryNext");
        next_->setProperty("primary",true);
        controls->addWidget(previous_);
        controls->addWidget(next_);
        root->addWidget(bottom);
        connect(previous_,&Button::clicked,this,[this] {
            if (step_ == Objects) captureObjects();
            showStep(Step(int(step_)-1));
        });
        connect(next_,&Button::clicked,this,[this] { advance(); });
        connect(scopeTree_,&QTreeWidget::itemExpanded,this,[this](QTreeWidgetItem* item) {
            auto* tree = static_cast<ObjectTree*>(scopeTree_);
            const QPersistentModelIndex index(tree->indexFromItem(item));
            tree->afterDoubleClick(item,[this,tree,index](QTreeWidgetItem*) {
                QTimer::singleShot(0,this,[this,tree,index] {
                    if (step_ == Scope && index.isValid()) fetchScope(tree->itemFromIndex(index));
                });
            });
        });
        connect(scopeTree_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* item) { scopeChecked(item); });
        connect(objectTree_,&QTreeWidget::itemChanged,this,[this] { updateNavigation(); });
        previewTimer_.setSingleShot(true);
        previewTimer_.setInterval(80);
        connect(&previewTimer_,&QTimer::timeout,this,[this] { updatePaper(); });
        rebuildScopes(profile);
        loadLayout();
        showStep(Scope);
    }
    ~DictionaryWizard() override { if (cancel_) cancel_->store(true); }
    void reject() override {
        if (!exportJob_.isEmpty()) { hide(); return; }
        Dialog::reject();
    }

private:
    struct ProfileInfo { QString name; K driver = K::Unknown; QString defaultDatabase; };
    const QString storePath_;
    const Resolve resolve_;
    const KnownCatalog known_;
    QMap<QString,ProfileInfo> profiles_;
    QStringList profileOrder_;
    DictionaryConfiguration config_;
    QVector<LoadedScope> loaded_;
    DatabaseDictionary report_;
    QString exportJob_;
    Cancellation cancel_;
    bool loadingControls_ = false;
    Step step_ = Scope;
    QString missingScopes_;
    QLabel *steps_, *heading_, *status_;
    QStackedWidget *pages_, *formatForms_;
    QTreeWidget *scopeTree_, *objectTree_;
    Button *previous_, *next_, *save_, *load_;
    QButtonGroup* themes_;
    QTabBar* formatTabs_;
    PaperPreview* paperPreview_;
    QTextBrowser* exportPreview_;
    QTimer previewTimer_;
    QVector<std::function<void()>> setControls_;
    QMap<QString,QTreeWidgetItem*> profileRoots_;

    QWidget* strip(const QString& color) {
        auto* bar = new QWidget;
        bar->setAutoFillBackground(true);
        auto palette = bar->palette();
        palette.setColor(QPalette::Window,QColor(color));
        bar->setPalette(palette);
        return bar;
    }
    Button* menuButton(const QString& name,const QString& id,std::function<void(QMenu*)> populate) {
        auto* button = new Button(name,id);
        auto* menu = new QMenu(button);
        populate(menu);
        button->setMenu(menu);
        button->setPopupMode(QToolButton::InstantPopup);
        return button;
    }
    QTreeWidget* selectionPage(const QString& id,bool reorder) {
        auto* page = new QSplitter;
        page->setChildrenCollapsible(false);
        auto* panel = new QWidget;
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(12,10,12,10);
        auto* tree = new ObjectTree;
        tree->setObjectName(id);
        tree->setHeaderHidden(true);
        tree->setUniformRowHeights(true);
        tree->setAnimated(false);
        tree->setExpandsOnDoubleClick(true);
        tree->setIndentation(17);
        tree->setIconSize({18,18});
        layout->addWidget(tree,1);
        auto* bar = new QHBoxLayout;
        if (!reorder) {
            auto* refresh = new Button("读取目录","dictionaryReadCatalog");
            refresh->setEnabled(false);
            bar->addWidget(refresh);
            connect(tree,&QTreeWidget::currentItemChanged,this,[this,refresh](QTreeWidgetItem* item) {
                const auto driver = item ? profiles_.value(item->data(0,Connection).toString()).driver : K::Unknown;
                refresh->setEnabled(driver != K::Unknown && driver != K::SQLite);
            });
            connect(refresh,&Button::clicked,this,[this,tree] {
                auto* selected = tree->currentItem();
                if (!selected) return;
                if (rowType(selected) == Row::Namespace && nodeFor(selected).kind == "schema") selected = selected->parent();
                if (rowType(selected) == Row::Namespace && !separateSchemas(profiles_.value(selected->data(0,Connection).toString()).driver))
                    selected = selected->parent();
                selected->setData(0,Fetched,false);
                selected->setExpanded(true);
                fetchScope(selected);
            });
        }
        if (reorder) for (const int delta : {-1,1}) {
            auto* move = new Button(delta < 0 ? "↑" : "↓",delta < 0 ? "dictionaryMoveUp" : "dictionaryMoveDown");
            move->setAccessibleName(delta < 0 ? "上移所选项" : "下移所选项");
            move->setToolTip(move->accessibleName());
            bar->addWidget(move);
            connect(move,&Button::clicked,this,[this,tree,delta] { moveRow(tree,delta); });
        }
        auto* filter = new TextInput;
        filter->setPlaceholderText(reorder ? "筛选对象" : "筛选连接、数据库或 Schema");
        bar->addWidget(filter,1);
        layout->addLayout(bar);
        auto* inspector = strip("#f5f8fb");
        auto* details = new QVBoxLayout(inspector);
        details->setContentsMargins(18,17,18,17);
        auto* title = new QLabel(reorder ? "对象详情" : "范围详情");
        title->setObjectName("sectionTitle");
        auto* name = new QLabel;
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        auto* info = new QLabel(reorder ? "选择对象以查看字段数量和注释。" : "展开连接可读取可访问的数据库或 Schema。");
        info->setWordWrap(true);
        info->setTextFormat(Qt::PlainText);
        info->setTextInteractionFlags(Qt::TextSelectableByMouse);
        details->addWidget(title);
        details->addSpacing(10);
        details->addWidget(name);
        details->addWidget(info);
        details->addStretch();
        connect(filter,&QLineEdit::textChanged,tree,[tree](const QString& text) {
            const QSignalBlocker blocker(tree);
            filterTree(tree,text.trimmed());
        });
        connect(tree,&QTreeWidget::currentItemChanged,this,[this,name,info,reorder](QTreeWidgetItem* item) {
            if (!item) { name->clear(); return; }
            name->setText(rowText(item));
            if (reorder && rowType(item) == Row::Object) {
                const auto* table = tableFor(item);
                if (table) info->setText(QString("%1\n%2 个字段 · %3 个关联对象\n\n%4")
                    .arg(table->name).arg(table->columns.size()).arg(table->objects.size()).arg(table->comment));
            } else if (rowType(item) == Row::Namespace) {
                const auto node = nodeFor(item);
                info->setText(node.kind == "schema" ? "Schema  ·  " + node.schema : "数据库  ·  " + node.database);
            } else {
                info->setText(reorder ? "勾选需要写入文档的项目。使用上下箭头调整顺序。" : "勾选要纳入数据字典的范围。");
            }
        });
        page->addWidget(panel);
        page->addWidget(inspector);
        page->setStretchFactor(0,3);
        page->setStretchFactor(1,1);
        page->setSizes({720,280});
        pages_->addWidget(page);
        return tree;
    }

    void addScope(const QString& id, CatalogNode node) {
        const auto info = profiles_.value(id);
        auto* parent = profileRoots_.value(id);
        if (!parent) return;
        if (node.kind == "schema" && separateSchemas(info.driver)) {
            CatalogNode db;
            db.kind = "database";
            db.name = db.database = node.database;
            addScope(id,db);
            for (int i = 0; i < parent->childCount(); ++i) {
                auto* child = parent->child(i);
                if (nodeFor(child).database == node.database) { parent = child; break; }
            }
        }
        if (node.database.isEmpty() && usesOracleSql(info.driver)) node.database = info.defaultDatabase;
        if (info.driver == K::SQLite) { node.database = "main"; node.schema.clear(); }
        const auto key = dictionaryScopeKey({id,node.database,node.schema,true,{},{}});
        for (int i = 0; i < parent->childCount(); ++i)
            if (parent->child(i)->data(0,Key).toString() == key) return;
        auto* item = treeRow(parent,Row::Namespace,scopeLabel(node),objectIcon(node.kind));
        item->setData(0,Connection,id);
        item->setData(0,DatabaseNode,QVariant::fromValue(node));
        item->setData(0,Key,key);
        checkRow(item,std::any_of(config_.scopes.cbegin(),config_.scopes.cend(),[&](const auto& scope) {
            return dictionaryScopeKey(scope) == key;
        }));
        if (node.kind == "database" && separateSchemas(info.driver))
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }

    void rebuildScopes(const QString& current = {}) {
        QSignalBlocker blocker(scopeTree_);
        scopeTree_->clear();
        profileRoots_.clear();
        auto* connections = treeRow(scopeTree_->invisibleRootItem(),Row::ConnectionGroup,"我的连接",objectIcon("connection"));
        connections->setExpanded(true);
        for (const auto& id : profileOrder_) {
            const auto p = profiles_.value(id);
            auto* item = treeRow(connections,Row::Profile,p.name,databaseIcon(p.driver));
            item->setData(0,Connection,id);
            item->setToolTip(0,databaseKindName(p.driver));
            profileRoots_[id] = item;
            if (p.driver != K::SQLite) item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            for (const auto& node : known_(id)) {
                if (node.kind == "database" || node.kind == "schema") addScope(id,node);
            }
            if (p.driver == K::SQLite) {
                CatalogNode node;
                node.kind = "database";
                node.name = node.database = "main";
                addScope(id,node);
            } else if (!p.defaultDatabase.isEmpty() && !usesOracleSql(p.driver)) {
                CatalogNode node;
                node.kind = "database";
                node.name = node.database = p.defaultDatabase;
                addScope(id,node);
            }
        }
        for (const auto& scope : config_.scopes) {
            const auto profile = profiles_.value(scope.id);
            CatalogNode node;
            node.kind = scope.schema.isEmpty() ? "database" : "schema";
            node.name = scope.schema.isEmpty() ? scope.database : scope.schema;
            node.database = scope.database;
            node.schema = scope.schema;
            if (profile.driver != K::Unknown) addScope(scope.id,node);
        }
        auto* active = profileRoots_.value(current);
        for (QTreeWidgetItemIterator it(scopeTree_); *it; ++it) {
            auto* item = *it;
            if (rowType(item) != Row::Namespace || item->checkState(0) != Qt::Checked) continue;
            for (auto* parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
            active = item;
        }
        blocker.unblock();
        scopeTree_->setCurrentItem(active);
    }

    void fetchScope(QTreeWidgetItem* item) {
        if (cancel_ || item->data(0,Fetched).toBool()) return;
        const auto id = item->data(0,Connection).toString();
        const auto kind = profiles_.value(id).driver;
        auto node = nodeFor(item);
        if (!(rowType(item) == Row::Profile && kind != K::SQLite) &&
            !(rowType(item) == Row::Namespace && node.kind == "database" && separateSchemas(kind))) return;
        const auto profile = resolve_(id,node);
        if (!profile) return;
        const auto key = item->data(0,Key).toString();
        beginWork("正在读取目录…");
        runDatabaseJob(this,[profile = *profile,node,cancel = cancel_] {
            auto scoped = profile;
            scoped.advanced.initialSql.clear();
            scoped.readOnly = true;
            const auto db = connectDatabase(scoped);
            return loadCatalog(*db,scoped,node,cancel);
        },[this,id,key](std::optional<CatalogResult> result,const QString& error) {
            endWork();
            if (!result || !result->error.isEmpty()) {
                status_->setText(result ? result->error : error);
                return;
            }
            if (result->cancelled) return;
            const QSignalBlocker blocker(scopeTree_);
            if (key.isEmpty()) {
                if (auto* root = profileRoots_.value(id)) root->setData(0,Fetched,true);
            } else {
                for (QTreeWidgetItemIterator it(scopeTree_); *it; ++it)
                    if ((*it)->data(0,Key).toString() == key) { (*it)->setData(0,Fetched,true); break; }
            }
            for (const auto& child : result->nodes)
                if (child.kind == "database" || child.kind == "schema") addScope(id,child);
            status_->setText(result->truncated ? "目录达到读取上限，已显示可用结果" : QString{});
        });
    }

    void scopeChecked(QTreeWidgetItem* item) {
        if (rowType(item) != Row::Namespace) return;
        if (item->checkState(0) == Qt::Checked) {
            const QSignalBlocker blocked(scopeTree_);
            if (item->parent() && rowType(item->parent()) == Row::Namespace)
                item->parent()->setCheckState(0,Qt::Unchecked);
            for (int i = 0; i < item->childCount(); ++i) item->child(i)->setCheckState(0,Qt::Unchecked);
        }
        updateNavigation();
    }

    QVector<DictionaryScopeChoice> selectedScopes() const {
        QMap<QString,DictionaryScopeChoice> previous;
        for (const auto& scope : config_.scopes) previous.insert(dictionaryScopeKey(scope),scope);
        QVector<DictionaryScopeChoice> checked;
        for (QTreeWidgetItemIterator it(scopeTree_,QTreeWidgetItemIterator::Checked); *it; ++it) {
            const auto* item = *it;
            if (rowType(item) != Row::Namespace) continue;
            if (item->parent() && rowType(item->parent()) == Row::Namespace &&
                item->parent()->checkState(0) == Qt::Checked) continue;
            const auto node = nodeFor(item);
            const auto key = item->data(0,Key).toString();
            checked.push_back(previous.value(key,{item->data(0,Connection).toString(),node.database,node.schema,true,{},{}}));
        }
        QMap<QString,int> order;
        for (int i = 0; i < config_.scopes.size(); ++i) order[dictionaryScopeKey(config_.scopes[i])] = i;
        std::stable_sort(checked.begin(),checked.end(),[&](const auto& a,const auto& b) {
            return order.value(dictionaryScopeKey(a),order.size()) < order.value(dictionaryScopeKey(b),order.size());
        });
        return checked;
    }

    void readObjects() {
        const auto scopes = selectedScopes();
        if (scopes.isEmpty()) return;
        config_.scopes = scopes;
        QVector<QPair<DictionaryScopeChoice,ConnectionProfile>> todo;
        for (const auto& scope : scopes) {
            CatalogNode node;
            node.database = scope.database;
            node.schema = scope.schema;
            const auto profile = resolve_(scope.id,node);
            if (!profile) return;
            todo.push_back({scope,*profile});
        }
        beginWork("正在读取所选范围的对象和注释…");
        runDatabaseJob(this,[todo,cancel = cancel_] {
            QVector<LoadedScope> data;
            int count = 0;
            for (const auto& entry : todo) {
                if (cancel->load()) throw std::runtime_error("读取已停止");
                try {
                    auto dictionary = loadDatabaseDictionary(entry.second,entry.first.schema,cancel);
                    count += dictionary.tables.size();
                    if (count > 20000) throw std::runtime_error("对象超过 20000 个，请缩小范围");
                    data.push_back({entry.first,std::move(dictionary)});
                } catch (const std::exception& e) {
                    throw std::runtime_error((entry.second.name+" / "+entry.first.database+" · "+QString::fromUtf8(e.what())).toStdString());
                }
            }
            return data;
        },[this](std::optional<QVector<LoadedScope>> scopes,const QString& error) {
            endWork();
            if (!scopes) { status_->setText(error); return; }
            loaded_ = std::move(*scopes);
            populateObjects();
            showStep(Objects);
        });
    }

    void populateObjects() {
        const QSignalBlocker blocker(objectTree_);
        objectTree_->clear();
        QMap<QString,QTreeWidgetItem*> connections;
        for (int i = 0; i < loaded_.size(); ++i) {
            const auto& loaded = loaded_[i];
            const auto& scope = loaded.choice;
            const auto profile = profiles_.value(scope.id);
            auto*& connection = connections[scope.id];
            if (!connection) {
                connection = treeRow(objectTree_->invisibleRootItem(),Row::Profile,profile.name,databaseIcon(profile.driver));
                connection->setData(0,Connection,scope.id);
                connection->setExpanded(true);
                checkRow(treeRow(connection,Row::ServerInfo,"服务器信息",objectIcon("query")),config_.serverInfo.value(scope.id,true));
            }
            auto* target = treeRow(connection,Row::Namespace,loaded.dictionary.scope,
                                   objectIcon(scope.schema.isEmpty() ? "database" : "schema"));
            target->setData(0,ObjectIndex,i);
            target->setExpanded(true);
            checkRow(treeRow(target,Row::ScopeInfo,scope.schema.isEmpty() ? "数据库信息" : "Schema 信息",objectIcon("query")),scope.info);
            QHash<QString,int> rank;
            for (int j = 0; j < scope.order.size(); ++j) rank.insert(scope.order[j],j);
            const QSet<QString> omitted(scope.omitted.begin(),scope.omitted.end());
            QVector<int> order;
            for (int j = 0; j < loaded.dictionary.tables.size(); ++j) order.push_back(j);
            std::stable_sort(order.begin(),order.end(),[&](int a,int b) {
                return rank.value(loaded.dictionary.tables[a].name,rank.size()) < rank.value(loaded.dictionary.tables[b].name,rank.size());
            });
            QMap<QString,QTreeWidgetItem*> kinds;
            for (const auto j : order) {
                const auto& table = loaded.dictionary.tables[j];
                const auto kind = table.kind.isEmpty() ? QString("table") : table.kind;
                auto*& group = kinds[kind];
                if (!group) {
                    group = treeRow(target,Row::Kind,kind == "table" ? "表" : kind == "materializedView" ? "物化视图" : "视图",objectIcon(kind));
                    checkRow(group,true,true);
                    group->setExpanded(true);
                }
                auto* item = treeRow(group,Row::Object,objectLabel(table),objectIcon(kind));
                item->setData(0,ObjectIndex,j);
                item->setToolTip(0,table.name);
                checkRow(item,!omitted.contains(table.name));
            }
        }
        if (objectTree_->topLevelItemCount()) objectTree_->setCurrentItem(objectTree_->topLevelItem(0));
    }

    const DatabaseDictionaryTable* tableFor(const QTreeWidgetItem* item) const {
        const int scope = item->parent()->parent()->data(0,ObjectIndex).toInt();
        const int index = item->data(0,ObjectIndex).toInt();
        if (scope < 0 || scope >= loaded_.size() || index < 0 || index >= loaded_[scope].dictionary.tables.size()) return nullptr;
        return &loaded_[scope].dictionary.tables[index];
    }

    void moveRow(QTreeWidget* tree,int delta) {
        auto* item = tree->currentItem();
        if (!item || rowType(item) == Row::ServerInfo || rowType(item) == Row::ScopeInfo) return;
        auto* parent = item->parent() ? item->parent() : tree->invisibleRootItem();
        const auto index = parent->indexOfChild(item);
        const auto destination = index+delta;
        if (destination < 0 || destination >= parent->childCount() || rowType(parent->child(destination)) != rowType(item)) return;
        const bool expanded = item->isExpanded();
        parent->takeChild(index);
        parent->insertChild(destination,item);
        item->setExpanded(expanded);
        tree->setCurrentItem(item);
    }

    void captureObjects() {
        config_.scopes.clear();
        report_ = {};
        QStringList scopes;
        for (int i = 0; i < objectTree_->topLevelItemCount(); ++i) {
            auto* connection = objectTree_->topLevelItem(i);
            const auto id = connection->data(0,Connection).toString();
            const auto profile = profiles_.value(id);
            const bool info = connection->child(0)->checkState(0) == Qt::Checked;
            config_.serverInfo[id] = info;
            if (info && connection->childCount() > 1) {
                const auto source = connection->child(1)->data(0,ObjectIndex).toInt();
                report_.sections.push_back({int(report_.tables.size()),profile.name,
                    databaseKindName(profile.driver)+"  "+loaded_[source].dictionary.serverVersion});
            }
            for (int j = 1; j < connection->childCount(); ++j) {
                auto* namespaceItem = connection->child(j);
                const auto& source = loaded_[namespaceItem->data(0,ObjectIndex).toInt()];
                auto scope = source.choice;
                scope.info = namespaceItem->child(0)->checkState(0) == Qt::Checked;
                scope.order.clear();
                scope.omitted.clear();
                scopes << source.dictionary.scope;
                if (scope.info) report_.sections.push_back({int(report_.tables.size()),source.dictionary.scope,
                    scope.schema.isEmpty() ? "数据库" : "Schema"});
                for (int category = 1; category < namespaceItem->childCount(); ++category) {
                    auto* group = namespaceItem->child(category);
                    for (int k = 0; k < group->childCount(); ++k) {
                        const auto* item = group->child(k);
                        const auto* table = tableFor(item);
                        if (!table) continue;
                        scope.order << table->name;
                        if (item->checkState(0) == Qt::Checked) report_.tables.push_back(*table);
                        else scope.omitted << table->name;
                    }
                }
                config_.scopes.push_back(std::move(scope));
            }
        }
        report_.scope = scopes.join("  /  ");
    }

    void buildTemplatePage() {
        auto* page = new QWidget;
        auto* column = new QVBoxLayout(page);
        column->setContentsMargins(20,12,20,12);
        auto* tiles = new QHBoxLayout;
        tiles->setSpacing(13);
        themes_ = new QButtonGroup(this);
        column->addStretch();
        for (int i = 0; i < 5; ++i) {
            auto* button = new Button(dictionaryThemeName(DictionaryTheme(i)),"dictionaryTheme"+QString::number(i));
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->setCheckable(true);
            button->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
            button->setIconSize({106,146});
            button->setMinimumHeight(182);
            button->setStyleSheet(
                "QToolButton{background:white;border:1px solid #cbd8e4;border-radius:7px;padding:10px}"
                "QToolButton:hover{border-color:#729bbe}"
                "QToolButton:checked{background:#e6f0f9;border:2px solid #3d77a7;color:#1f557d}");
            DictionaryLayout sample;
            sample.theme = DictionaryTheme(i);
            const auto colors = dictionaryColors(sample.theme);
            QPixmap page(212,292);
            page.fill(Qt::white);
            QPainter painter(&page);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(colors.grid,2));
            painter.drawRect(page.rect().adjusted(1,1,-2,-2));
            painter.fillRect(QRect(19,25,29,6),colors.accent);
            painter.setPen(colors.accent);
            QFont font;
            font.setPixelSize(23);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(QPoint(19,91),"数据字典");
            painter.fillRect(QRect(19,107,119,4),colors.grid);
            painter.fillRect(QRect(19,121,81,4),colors.grid);
            painter.drawImage(QRect(16,169,180,90),dictionaryArtwork(sample));
            painter.end();
            button->setIcon(QIcon(page));
            themes_->addButton(button,i);
            tiles->addWidget(button,1);
            connect(button,&Button::clicked,this,[this,i] {
                config_.layout.theme = DictionaryTheme(i);
                schedulePreview();
            });
        }
        column->addLayout(tiles);
        column->addStretch();
        pages_->addWidget(page);
    }

    QFormLayout* layoutPanel() {
        auto* content = new QWidget;
        auto* form = new QFormLayout(content);
        form->setContentsMargins(18,13,18,15);
        form->setSpacing(10);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        formatForms_->addWidget(scroll);
        return form;
    }

    CheckBox* addFlag(QFormLayout* form,const QString& label,bool DictionaryLayout::*member,const QString& name = {}) {
        auto* check = new CheckBox(label);
        check->setObjectName(name);
        form->addRow(QString{},check);
        setControls_.push_back([this,check,member] { check->setChecked(config_.layout.*member); });
        connect(check,&QCheckBox::toggled,this,[this,member](bool checked) {
            if (!loadingControls_) { config_.layout.*member = checked; schedulePreview(); }
        });
        return check;
    }

    Button* fontButton(DictionaryFont DictionaryLayout::* member) {
        auto* button = new Button("字体…");
        setControls_.push_back([this,button,member] {
            const auto& font = config_.layout.*member;
            button->setText(QString("%1 · %2 pt").arg(font.family.isEmpty() ? "默认字体" : font.family).arg(font.size));
        });
        connect(button,&Button::clicked,this,[this,member] {
            auto& font = config_.layout.*member;
            QFont initial(font.family.isEmpty() ? QApplication::font().family() : font.family,font.size);
            initial.setBold(font.bold);
            bool ok = false;
            const auto value = QFontDialog::getFont(&ok,initial,this,"选择字体");
            if (!ok) return;
            font = {value.family(),qBound(6,value.pointSize(),52),value.bold()};
            loadLayout();
        });
        return button;
    }

    void addText(QFormLayout* form,const QString& label,QString DictionaryLayout::*member,
                 DictionaryFont DictionaryLayout::*font,const QString& name, bool multiline = false) {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0,0,0,0);
        if (multiline) {
            auto* input = new QPlainTextEdit;
            input->setObjectName(name);
            input->setMinimumHeight(92);
            input->setMaximumHeight(152);
            layout->addWidget(input,1);
            setControls_.push_back([this,input,member] { input->setPlainText(config_.layout.*member); });
            connect(input,&QPlainTextEdit::textChanged,this,[this,input,member] {
                if (!loadingControls_) { config_.layout.*member = input->toPlainText(); schedulePreview(); }
            });
        } else {
            auto* input = new TextInput;
            input->setObjectName(name);
            input->setMaxLength(512);
            if (member == &DictionaryLayout::title) input->setPlaceholderText("沿用所选数据库或 Schema 名称");
            layout->addWidget(input,1);
            setControls_.push_back([this,input,member] { input->setText(config_.layout.*member); });
            connect(input,&QLineEdit::textChanged,this,[this,member](const QString& text) {
                if (!loadingControls_) { config_.layout.*member = text; schedulePreview(); }
            });
        }
        layout->addWidget(fontButton(font),0,Qt::AlignTop);
        form->addRow(label,row);
    }

    void addImage(QFormLayout* form,const QString& label,QImage DictionaryLayout::*member) {
        auto* container = new QWidget;
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0,0,0,0);
        auto* choose = new Button("选择图片…");
        auto* clear = new Button("移除");
        layout->addWidget(choose);
        layout->addWidget(clear);
        layout->addStretch();
        form->addRow(label,container);
        setControls_.push_back([this,choose,clear,member] {
            const auto& img = config_.layout.*member;
            clear->setEnabled(!img.isNull());
            choose->setText(img.isNull() ? "选择图片…" : QString("更换图片 · %1×%2").arg(img.width()).arg(img.height()));
        });
        connect(choose,&Button::clicked,this,[this,member] {
            action([&] {
                auto image = chooseImage(this);
                if (!image.isNull()) {
                    config_.layout.*member = std::move(image);
                    if (member == &DictionaryLayout::coverImage) config_.layout.background = DictionaryBackground::Image;
                    loadLayout();
                }
            });
        });
        connect(clear,&Button::clicked,this,[this,member] { config_.layout.*member = {}; loadLayout(); });
    }

    template<class Value>
    Select* addSelect(QFormLayout* form,const QString& label,const QStringList& labels,
                      const QVector<Value>& values,Value DictionaryLayout::*member,const QString& id = {}) {
        auto* combo = new Select;
        combo->setObjectName(id);
        combo->addItems(labels);
        form->addRow(label,combo);
        setControls_.push_back([this,combo,values,member] { combo->setCurrentIndex(values.indexOf(config_.layout.*member)); });
        connect(combo,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,values,member](int index) {
            if (!loadingControls_ && index >= 0) { config_.layout.*member = values[index]; schedulePreview(); }
        });
        return combo;
    }

    void addResets(QFormLayout* form,FormatPage page,bool styles) {
        if (styles) {
            auto* reset = new Button("还原默认样式");
            form->addRow(QString{},reset);
            connect(reset,&Button::clicked,this,[this,page] { resetForm(page,true); });
        }
        auto* resetAll = new Button("还原此页默认值");
        form->addRow(QString{},resetAll);
        connect(resetAll,&Button::clicked,this,[this,page] { resetForm(page,false); });
    }

    void resetForm(FormatPage page,bool styles) {
        DictionaryLayout defaults;
        auto options = config_.layout;
        if (page == Cover) {
            options.titleFont = defaults.titleFont; options.subtitleFont = defaults.subtitleFont;
            options.notesFont = defaults.notesFont; options.coverHeaderFont = defaults.coverHeaderFont;
            options.coverFooterFont = defaults.coverFooterFont; options.opacity = defaults.opacity;
            options.background = defaults.background; options.imageFit = defaults.imageFit;
            if (!styles) {
                options.cover = defaults.cover; options.title.clear(); options.subtitle = defaults.subtitle;
                options.notes.clear(); options.coverHeader.clear(); options.coverFooter = defaults.coverFooter;
                options.logo = {}; options.coverImage = {};
            }
        } else if (page == Contents) options.contents = defaults.contents;
        else if (page == HeaderFooter) {
            options.headerFont = defaults.headerFont; options.footerFont = defaults.footerFont;
            options.headingFont = defaults.headingFont; options.objectFont = defaults.objectFont;
            if (!styles) {
                options.header = defaults.header; options.footer = defaults.footer; options.pageNumbers = defaults.pageNumbers;
                options.headerText.clear(); options.footerText.clear();
            }
        } else if (page == Body) {
            options.bodyFont = defaults.bodyFont;
            if (!styles) {
                options.introduction = defaults.introduction; options.introductionText = defaults.introductionText;
                options.details = defaults.details; options.comments = defaults.comments;
            }
        } else {
            options.paper = defaults.paper; options.orientation = defaults.orientation; options.margins = defaults.margins;
        }
        config_.layout = std::move(options);
        loadLayout();
    }

    void buildLayoutPage() {
        auto* page = new QWidget;
        auto* column = new QVBoxLayout(page);
        column->setContentsMargins(0,8,0,0);
        formatTabs_ = new QTabBar;
        formatTabs_->setObjectName("dictionaryLayoutTabs");
        formatTabs_->setExpanding(false);
        formatTabs_->setDrawBase(false);
        for (const auto* label : {"封面","目录","页眉或页脚","正文","纸张"}) formatTabs_->addTab(QString::fromUtf8(label));
        column->addWidget(formatTabs_,0,Qt::AlignHCenter);
        auto* split = new QSplitter;
        split->setChildrenCollapsible(false);
        auto* previewContainer = new QWidget;
        auto* previewLayout = new QVBoxLayout(previewContainer);
        previewLayout->setContentsMargins(0,0,0,0);
        previewLayout->setSpacing(0);
        paperPreview_ = new PaperPreview;
        paperPreview_->setObjectName("dictionaryPaperPreview");
        previewLayout->addWidget(paperPreview_,1);
        auto* zoom = strip("#eef3f8");
        auto* zoomLayout = new QHBoxLayout(zoom);
        zoomLayout->setContentsMargins(8,5,8,5);
        auto* more = new Button("+");
        more->setAccessibleName("放大预览");
        auto* less = new Button("−");
        less->setAccessibleName("缩小预览");
        auto* zoomMode = new Select;
        zoomMode->addItems({"整页","适应宽度","原始尺寸"});
        zoomLayout->addWidget(more);
        zoomLayout->addWidget(less);
        zoomLayout->addWidget(zoomMode);
        zoomLayout->addStretch();
        previewLayout->addWidget(zoom);
        connect(more,&Button::clicked,this,[this] { paperPreview_->changeZoom(1.2); });
        connect(less,&Button::clicked,this,[this] { paperPreview_->changeZoom(1./1.2); });
        connect(zoomMode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i) { paperPreview_->setZoom(i); });
        split->addWidget(previewContainer);
        formatForms_ = new QStackedWidget;
        split->addWidget(formatForms_);
        split->setStretchFactor(0,4);
        split->setStretchFactor(1,5);
        split->setSizes({430,590});
        column->addWidget(split,1);

        auto* cover = layoutPanel();
        addFlag(cover,"包含封面",&DictionaryLayout::cover,"dictionaryIncludeCover");
        addImage(cover,"封面标志",&DictionaryLayout::logo);
        addText(cover,"封面页眉",&DictionaryLayout::coverHeader,&DictionaryLayout::coverHeaderFont,"dictionaryCoverHeader");
        addText(cover,"标题",&DictionaryLayout::title,&DictionaryLayout::titleFont,"dictionaryTitle");
        addText(cover,"副标题",&DictionaryLayout::subtitle,&DictionaryLayout::subtitleFont,"dictionarySubtitle");
        addText(cover,"备注",&DictionaryLayout::notes,&DictionaryLayout::notesFont,"dictionaryNotes",true);
        auto* hint = new QLabel("可用占位符  [SCOPE] 范围  [DATE] 日期  [TIME] 时间");
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#536b80");
        cover->addRow(QString{},hint);
        addSelect<DictionaryBackground>(cover,"封面背景",{"模板图案","自定义图片","无"},
            {DictionaryBackground::Theme,DictionaryBackground::Image,DictionaryBackground::None},&DictionaryLayout::background);
        addImage(cover,"背景图片",&DictionaryLayout::coverImage);
        addSelect<DictionaryImageFit>(cover,"图片布局",{"填充","适应","平铺"},
            {DictionaryImageFit::Fill,DictionaryImageFit::Fit,DictionaryImageFit::Tile},&DictionaryLayout::imageFit);
        auto* opacity = new NumberInput;
        opacity->setRange(0,100);
        opacity->setSuffix(" %");
        cover->addRow("背景不透明度",opacity);
        setControls_.push_back([this,opacity] { opacity->setValue(config_.layout.opacity); });
        connect(opacity,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int value) {
            if (!loadingControls_) { config_.layout.opacity = value; schedulePreview(); }
        });
        addText(cover,"封面页脚",&DictionaryLayout::coverFooter,&DictionaryLayout::coverFooterFont,"dictionaryCoverFooter");
        addResets(cover,Cover,true);

        auto* contents = layoutPanel();
        addFlag(contents,"包含目录",&DictionaryLayout::contents,"dictionaryIncludeContents");
        addResets(contents,Contents,false);

        auto* running = layoutPanel();
        addFlag(running,"包含页眉",&DictionaryLayout::header);
        addText(running,"页眉文字",&DictionaryLayout::headerText,&DictionaryLayout::headerFont,"dictionaryHeader");
        running->addRow("章节标题字体",fontButton(&DictionaryLayout::headingFont));
        running->addRow("对象标题字体",fontButton(&DictionaryLayout::objectFont));
        addFlag(running,"包含左侧页脚",&DictionaryLayout::footer);
        addText(running,"页脚文字",&DictionaryLayout::footerText,&DictionaryLayout::footerFont,"dictionaryFooter");
        addFlag(running,"在右侧页脚显示页码",&DictionaryLayout::pageNumbers,"dictionaryPageNumbers");
        addResets(running,HeaderFooter,true);

        auto* body = layoutPanel();
        addFlag(body,"包含引言",&DictionaryLayout::introduction,"dictionaryIncludeIntroduction");
        addText(body,"引言",&DictionaryLayout::introductionText,&DictionaryLayout::bodyFont,"dictionaryIntroduction",true);
        addFlag(body,"包含表和字段注释",&DictionaryLayout::comments,"dictionaryIncludeComments");
        addFlag(body,"包含索引、外键和约束",&DictionaryLayout::details,"dictionaryIncludeDetails");
        addResets(body,Body,true);

        auto* paper = layoutPanel();
        addSelect<QPageSize::PageSizeId>(paper,"纸张",{"A4","A3","Letter","Legal"},
            {QPageSize::A4,QPageSize::A3,QPageSize::Letter,QPageSize::Legal},&DictionaryLayout::paper,"dictionaryPaper");
        addSelect<QPageLayout::Orientation>(paper,"方向",{"纵向","横向"},
            {QPageLayout::Portrait,QPageLayout::Landscape},&DictionaryLayout::orientation,"dictionaryOrientation");
        const QStringList labels{"左边距","上边距","右边距","下边距"};
        const std::array<qreal(QMarginsF::*)() const noexcept,4> get{&QMarginsF::left,&QMarginsF::top,&QMarginsF::right,&QMarginsF::bottom};
        const std::array<void(QMarginsF::*)(qreal) noexcept,4> set{&QMarginsF::setLeft,&QMarginsF::setTop,&QMarginsF::setRight,&QMarginsF::setBottom};
        for (int i = 0; i < 4; ++i) {
            auto* margin = new DecimalInput;
            margin->setObjectName("dictionaryMargin"+QString::number(i));
            margin->setRange(.6,4.);
            margin->setDecimals(2);
            margin->setSingleStep(.1);
            margin->setSuffix(" cm");
            paper->addRow(labels[i],margin);
            setControls_.push_back([this,margin,getter = get[i]] { margin->setValue((config_.layout.margins.*getter)()/10.); });
            connect(margin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,setter = set[i]](double value) {
                if (!loadingControls_) { (config_.layout.margins.*setter)(value*10.); schedulePreview(); }
            });
        }
        addResets(paper,Paper,false);

        connect(formatTabs_,&QTabBar::currentChanged,formatForms_,&QStackedWidget::setCurrentIndex);
        connect(formatTabs_,&QTabBar::currentChanged,this,[this] { schedulePreview(); });
        pages_->addWidget(page);
    }

    void loadLayout() {
        loadingControls_ = true;
        for (const auto& set : setControls_) set();
        if (auto* theme = themes_->button(int(config_.layout.theme))) theme->setChecked(true);
        loadingControls_ = false;
        schedulePreview();
    }

    void schedulePreview() { if (step_ == Layout) previewTimer_.start(); }
    void updatePaper() {
        if (step_ != Layout) return;
        const auto index = formatTabs_->currentIndex();
        const auto section = index == Cover ? DictionaryPreviewSection::Cover
            : index == Contents ? DictionaryPreviewSection::Contents
            : index == Body && config_.layout.introduction ? DictionaryPreviewSection::Introduction
            : DictionaryPreviewSection::Body;
        action([&] { paperPreview_->showPage(dictionaryHtml(report_,config_.layout,section,cancellation()),config_.layout,index == Cover); });
    }

    void buildExportPage() {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(12,12,12,10);
        exportPreview_ = new QTextBrowser;
        exportPreview_->setObjectName("dictionaryExportPreview");
        exportPreview_->setOpenExternalLinks(false);
        layout->addWidget(exportPreview_,1);
        auto* buttons = new QHBoxLayout;
        buttons->addStretch();
        const auto add = [&](const QString& label,const QString& name,const QString& filter,
                             std::optional<ReportDocumentFormat> format) {
            auto* button = new Button(label,name);
            buttons->addWidget(button);
            connect(button,&Button::clicked,this,[this,format,filter] { exportFile(format,filter); });
        };
        add("导出 PDF…","dictionaryExportPdf","PDF (*.pdf)",ReportDocumentFormat::Pdf);
        add("导出 Word…","dictionaryExportWord","Word (*.docx)",ReportDocumentFormat::Word);
        add("导出 HTML…","dictionaryExportHtml","HTML (*.html)",{});
        layout->addLayout(buttons);
        pages_->addWidget(page);
    }

    ExportContext fileContext() const {
        if (config_.scopes.isEmpty()) return {};
        const auto& first = config_.scopes.front();
        const auto info = profiles_.value(first.id);
        ConnectionProfile profile;
        profile.name = info.name;
        profile.driver = info.driver;
        profile.database = info.defaultDatabase;
        auto context = exportContext(profile,first.database,first.schema);
        for (const auto& scope : config_.scopes) {
            if (scope.id != first.id) return {};
            if (scope.database != first.database) context.database.clear();
            if (scope.database != first.database || scope.schema != first.schema)
                context.schema.clear();
        }
        return context;
    }

    void exportFile(std::optional<ReportDocumentFormat> format,const QString& filter) {
        if (cancel_ || !exportJob_.isEmpty()) return;
        const auto path = chooseExportFile(this,"导出数据字典",fileContext(),"数据字典",filter);
        if (path.isEmpty()) return;
        if (auto* tasks = backgroundTasksFor(this)) {
            const auto output = QFileInfo(path).absoluteFilePath();
            QString rejection;
            exportJob_ = tasks->submit({"导出数据字典", output, {}, output},
                [report = report_, settings = config_.layout, output, format](const TaskContext& task) {
                    task.report({0, 0, TaskUnit::Items, "正在排版数据字典"});
                    if (format) exportDatabaseDictionary(report, settings, output, *format, task.cancel);
                    else {
                        const auto html = dictionaryHtml(report, settings, DictionaryPreviewSection::Full, task.cancel);
                        if (task.cancel->load()) throw std::runtime_error("导出已停止");
                        saveFile(output, html.toUtf8());
                    }
                    TaskResult done;
                    done.detail = "数据字典已导出";
                    done.artifacts << output;
                    return done;
                }, &rejection);
            if (exportJob_.isEmpty()) { status_->setText(rejection); return; }
            setAttribute(Qt::WA_DeleteOnClose, false);
            rememberTaskDialog(exportJob_, *this);
            status_->setText("已提交导出任务。关闭窗口后可从任务列表恢复。");
            updateNavigation();
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(tasks, &BackgroundTasks::finished, this,
                    [this, connection](const QString& id, const TaskResult& result) {
                if (id != exportJob_) return;
                QObject::disconnect(*connection);
                exportJob_.clear();
                if (!isVisible()) { deleteLater(); return; }
                setAttribute(Qt::WA_DeleteOnClose);
                status_->setText(result.error.isEmpty() ? result.detail : result.error);
                updateNavigation();
            });
            return;
        }
        beginWork("正在导出数据字典…");
        runDatabaseJob(this,[report = report_,settings = config_.layout,path,format,cancel = cancel_] {
            if (format) exportDatabaseDictionary(report,settings,path,*format,cancel);
            else saveFile(path,dictionaryHtml(report,settings,DictionaryPreviewSection::Full,cancel).toUtf8());
            return true;
        },[this](std::optional<bool> result,const QString& error) {
            endWork();
            status_->setText(result ? "数据字典已导出" : error);
        });
    }

    void previewExport() {
        showStep(Export);
        exportPreview_->setPlainText("正在生成预览…");
        beginWork("正在生成预览…");
        runDatabaseJob(this,[report = report_,layout = config_.layout,cancel = cancel_] {
            return dictionaryHtml(report,layout,DictionaryPreviewSection::Full,cancel);
        },[this](std::optional<QString> result,const QString& error) {
            endWork();
            if (result) { exportPreview_->setHtml(*result); status_->clear(); }
            else { exportPreview_->setPlainText(error); status_->setText(error); }
        });
    }

    void beginWork(const QString& message) {
        cancel_ = cancellation();
        status_->setText(message);
        updateNavigation();
    }
    void endWork() { cancel_.reset(); updateNavigation(); }

    void updateNavigation() {
        const auto busy = bool(cancel_) || !exportJob_.isEmpty();
        pages_->setEnabled(!busy);
        save_->setEnabled(!busy);
        load_->setEnabled(!busy);
        previous_->setEnabled(!busy && step_ != Scope);
        next_->setEnabled(!busy && (step_ != Scope || *QTreeWidgetItemIterator(scopeTree_,QTreeWidgetItemIterator::Checked)));
    }

    void showStep(Step step) {
        static const QStringList descriptions{"选择用于生成数据字典的范围","勾选并排序要写入文档的对象",
                                              "选择文档模板","调整文档版式","预览并导出数据字典"};
        static const QStringList labels{"范围","对象","模板","版式","导出"};
        step_ = step;
        pages_->setCurrentIndex(int(step));
        steps_->setText(QString("%1 / %2   ·   %3").arg(1+int(step)).arg(labels.size()).arg(labels[int(step)]));
        heading_->setText(descriptions[int(step)]);
        next_->setText(step == Export ? "完成" : "下一步");
        status_->setText(step == Scope && !missingScopes_.isEmpty() ? missingScopes_ : QString{});
        updateNavigation();
        schedulePreview();
    }
    void advance() {
        if (cancel_) return;
        if (step_ == Scope) { readObjects(); return; }
        if (step_ == Objects) {
            captureObjects();
            if (report_.tables.isEmpty() && report_.sections.isEmpty()) {
                status_->setText("请选择至少一个对象或信息项。");
                return;
            }
        }
        if (step_ == Layout) previewExport();
        else if (step_ == Export) close();
        else showStep(Step(int(step_)+1));
    }

    void action(const std::function<void()>& perform) {
        try { perform(); } catch (const std::exception& error) { status_->setText(QString::fromUtf8(error.what())); }
    }

    void captureBeforeSave() {
        if (step_ == Scope) config_.scopes = selectedScopes();
        else if (step_ == Objects) captureObjects();
    }
    QJsonObject presets(LocalStore& store) const {
        const auto stored = store.document(presetKey);
        if (!stored) return {};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(*stored,&error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("无法读取本地数据字典方案");
        return document.object();
    }
    void savePreset() {
        action([&] {
            bool ok = false;
            const auto name = QInputDialog::getText(this,"保存本地方案","方案名称",QLineEdit::Normal,{},&ok).trimmed();
            if (!ok || name.isEmpty()) return;
            captureBeforeSave();
            LocalStore store(storePath_);
            auto saved = presets(store);
            if (saved.contains(name) && QMessageBox::question(this,"覆盖配置","已存在同名方案，是否覆盖？",
                QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
            auto config = dictionaryConfigurationJson(config_);
            store.transaction([&] {
                auto current = presets(store);
                current[name] = config;
                store.putDocument(presetKey,QJsonDocument(current).toJson(QJsonDocument::Compact));
            });
            status_->setText("方案已保存 · "+name);
        });
    }
    void loadPreset() {
        action([&] {
            LocalStore store(storePath_);
            const auto saved = presets(store);
            if (saved.isEmpty()) { status_->setText("还没有本地配置方案。"); return; }
            bool ok = false;
            const auto name = QInputDialog::getItem(this,"选择本地方案","方案",saved.keys(),0,false,&ok);
            if (ok) applyConfiguration(dictionaryConfigurationFromJson(saved.value(name).toObject()));
        });
    }
    void saveConfigurationFile() {
        action([&] {
            captureBeforeSave();
            const auto path = chooseExportFile(this,"保存配置文件",fileContext(),"数据字典配置","数据字典配置 (*.json)");
            if (path.isEmpty()) return;
            saveFile(path,QJsonDocument(dictionaryConfigurationJson(config_)).toJson(QJsonDocument::Indented));
            status_->setText("配置文件已保存");
        });
    }
    void loadConfigurationFile() {
        action([&] {
            const auto path = QFileDialog::getOpenFileName(this,"加载配置文件",{},"数据字典配置 (*.json)");
            if (path.isEmpty()) return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > configurationBytes)
                throw std::runtime_error("无法读取配置文件或文件过大");
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(file.readAll(),&error);
            if (error.error != QJsonParseError::NoError || !doc.isObject())
                throw std::runtime_error("配置文件不是有效的 JSON");
            applyConfiguration(dictionaryConfigurationFromJson(doc.object()));
        });
    }
    void applyConfiguration(DictionaryConfiguration config) {
        const auto count = config.scopes.size();
        config.scopes.erase(std::remove_if(config.scopes.begin(),config.scopes.end(),[this](const auto& scope) {
            return !profiles_.contains(scope.id);
        }),config.scopes.end());
        missingScopes_ = count == config.scopes.size() ? QString{} :
            QString("有 %1 个范围对应的连接不在本机，请重新选择。").arg(count-config.scopes.size());
        config_ = std::move(config);
        objectTree_->clear();
        loaded_.clear();
        report_ = {};
        rebuildScopes();
        loadLayout();
        showStep(Scope);
        if (missingScopes_.isEmpty()) status_->setText("配置已加载");
    }
};
}

void showDictionaryWizard(QWidget* parent,const QVector<ConnectionProfile>& profiles,const QString& profile,
                          const CatalogNode& node,const QString& storePath,Resolve resolve,KnownCatalog known) {
    auto* wizard = new DictionaryWizard(parent,profiles,profile,node,storePath,std::move(resolve),std::move(known));
    wizard->show();
}
}
