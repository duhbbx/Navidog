#include "quick_entry_bar.hpp"
#include "quick_action_icons.hpp"

namespace navidog::ui {

QuickEntryBar::QuickEntryBar(QWidget *parent) : OverflowStrip(parent) {
    setObjectName("globalToolbar");
    setFixedHeight(68);
    row()->setContentsMargins(8, 4, 8, 4);
    row()->setSpacing(2);

    using Icon = QuickActionIcon;
    const auto addButton = [this](const QString &title, const QString &name, Icon icon) {
        auto *control = new Button(title, name, this);
        control->setIcon(quickActionIcon(icon));
        control->setIconSize({32, 32});
        control->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        row()->addWidget(control);
        return control;
    };
    QList<QToolButton *> actions;
    const auto addAction = [&](const QString &title, const QString &name, Icon icon) {
        auto *control = addButton(title, name, icon);
        actions.append(control);
        return control;
    };

    connectionButton_ = addAction("连接", "addConnection", Icon::Connection);
    connect(connectionButton_, &QToolButton::clicked, this, &QuickEntryBar::newConnectionRequested);
    connect(addAction("新建查询", "newQuery", Icon::NewQuery), &QToolButton::clicked,
            this, &QuickEntryBar::newQueryRequested);

    struct Category {
        const char *title;
        const char *kind;
        Icon icon;
    };
    const Category categories[]{{"表", "tables", Icon::Tables},
                                {"视图", "views", Icon::Views},
                                {"函数", "functions", Icon::Functions},
                                {"存储过程", "procedures", Icon::Procedures}};
    for (const auto &category : categories) {
        const QString kind(category.kind);
        auto *control = addAction(category.title, "browse_" + kind, category.icon);
        control->setProperty("categorySelected", false);
        categoryButtons_.insert(kind, control);
        connect(control, &QToolButton::clicked, this,
                [this, kind] { emit browseFolderRequested(kind); });
    }

    connect(addAction("用户", "browse_users", Icon::Users), &QToolButton::clicked,
            this, [this] { emit serverObjectsRequested(false); });
    connect(addAction("事件", "browse_events", Icon::Events), &QToolButton::clicked,
            this, [this] { emit serverObjectsRequested(true); });
    connect(addAction("查询", "browse_queries", Icon::Query), &QToolButton::clicked,
            this, &QuickEntryBar::queryLibraryRequested);
    connect(addAction("备份", "openBackup", Icon::Backup), &QToolButton::clicked,
            this, &QuickEntryBar::backupRequested);
    connect(addAction("自动运行", "openAutomation", Icon::Automation), &QToolButton::clicked,
            this, &QuickEntryBar::automationRequested);
    auto *model = addAction("模型", "openModels", Icon::Model);
    modelMenu_ = new QMenu(model);
    model->setMenu(modelMenu_);
    model->setPopupMode(QToolButton::InstantPopup);
    connect(addAction("BI", "openBi", Icon::Bi), &QToolButton::clicked,
            this, &QuickEntryBar::biRequested);

    stopCatalogButton_ = addButton("停止加载", "stopCatalog", Icon::Stop);
    stopCatalogButton_->hide();
    connect(stopCatalogButton_, &QToolButton::clicked, this, &QuickEntryBar::stopCatalogRequested);
    complete(actions);
    row()->addStretch();
}

void QuickEntryBar::setNewConnectionEnabled(bool enabled) {
    connectionButton_->setEnabled(enabled);
}

void QuickEntryBar::setCatalogBusy(bool busy) {
    stopCatalogButton_->setVisible(busy);
}

void QuickEntryBar::setSelectedCategory(const QString &kind) {
    for (auto entry = categoryButtons_.cbegin(); entry != categoryButtons_.cend(); ++entry) {
        auto *control = entry.value();
        const bool selected = entry.key() == kind;
        if (control->property("categorySelected").toBool() == selected)
            continue;
        control->setProperty("categorySelected", selected);
        control->style()->unpolish(control);
        control->style()->polish(control);
        control->update();
    }
}

void QuickEntryBar::setModelActions(const QList<QAction *> &actions) {
    modelMenu_->clear();
    modelMenu_->addActions(actions);
}

} // namespace navidog::ui
