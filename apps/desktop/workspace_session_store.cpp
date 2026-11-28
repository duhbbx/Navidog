#include "workspace_session_store.hpp"
#include "workspace_pane.hpp"
#include "query_pane.hpp"
#include "navidog/local_store.hpp"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QUuid>
#include <QJsonDocument>
#include <QMainWindow>
#include <QSplitter>
#include <QTabWidget>

namespace {
constexpr auto sessionKey = "workspace-session";

QByteArray decodeState(const QJsonObject &data, const char *key, int limit) {
    const auto encoded = data[key].toString();
    return encoded.size() <= limit ? QByteArray::fromBase64(encoded.toLatin1()) : QByteArray{};
}
} // namespace

WorkspaceSessionStore::WorkspaceSessionStore(const QString &dataDirectory)
    : path_(navidog::localStorePath(dataDirectory)),
      legacyPath_(QDir(dataDirectory).filePath("session.json")) {}

std::optional<WorkspaceSession> WorkspaceSessionStore::load() const {
    const auto bytes = navidog::localDocument(path_, sessionKey, legacyPath_);
    if (!bytes)
        return {};
    const auto data = QJsonDocument::fromJson(*bytes).object();
    WorkspaceSession session;
    if (data["queries"].isArray())
        session.tabs = data["queries"].toArray();
    session.activeTab = data["active"].toInt(0);
    session.geometry = decodeState(data, "windowGeometry", 8192);
    session.splitterState = decodeState(data, "workspaceSplitter", 4096);
    session.sidebarVisible = data["sidebarVisible"].toBool(true);
    return session;
}

QString WorkspaceSessionStore::sqlDraftPath(const QString &id) const {
    const QUuid uuid(id);
    if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id)
        return {};
    return QDir(QFileInfo(path_).absolutePath()).filePath("workspace-sql-drafts/" + id + ".sql");
}

void WorkspaceSessionStore::save(const QMainWindow &window, const QTabWidget &tabs,
                                  bool sidebarVisible) const {
    QJsonArray saved;
    QStringList created;
    QSet<QString> retained;
    const auto previous = load();
    int active = 0;
    try {
        for (int i = 0; i < tabs.count(); ++i) {
            auto *pane = dynamic_cast<const WorkspacePane *>(tabs.widget(i));
            if (!pane)
                continue;
            auto state = pane->sessionState();
            if (auto *query = dynamic_cast<const QueryPane *>(pane);
                query && query->needsExternalDraft()) {
                const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                const auto path = sqlDraftPath(id);
                if (!QDir().mkpath(QFileInfo(path).absolutePath()))
                    throw std::runtime_error("无法创建 SQL 草稿目录");
                navidog::writeScriptDocument({}, path, query->editor->toPlainText(),
                                              false, navidog::editableScriptBytes * 2);
                created.append(path);
                state["draft"] = id;
            }
            const auto draft = state["draft"].toString();
            if (!draft.isEmpty())
                retained.insert(draft);
            state["title"] = tabs.tabText(i);
            if (i == tabs.currentIndex())
                active = saved.size();
            saved.append(state);
        }
        QJsonObject document{
            {"version", 4},
            {"queries", saved},
            {"active", active},
            {"windowGeometry", QString::fromLatin1(window.saveGeometry().toBase64())},
            {"sidebarVisible", sidebarVisible}};
        if (auto *splitter = window.findChild<QSplitter *>("workspaceSplitter"))
            document["workspaceSplitter"] = QString::fromLatin1(splitter->saveState().toBase64());
        navidog::LocalStore store(path_);
        store.putDocument(sessionKey, QJsonDocument(document).toJson());
    } catch (...) {
        for (const auto &path : created)
            QFile::remove(path);
        throw;
    }
    if (previous && previous->tabs)
        for (const auto &tab : *previous->tabs) {
            const auto id = tab.toObject()["draft"].toString();
            if (!id.isEmpty() && !retained.contains(id))
                QFile::remove(sqlDraftPath(id));
        }
}
