#pragma once
#include "navidog/metadata_cache.hpp"
#include "navidog/sql/compiler/completion.hpp"
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
struct EditorCompletionItem {
  QString label, insertText, detail, kind;
  int begin = 0, end = 0, cursor = 0, selection = 0;
};
struct EditorCompletionResult {
  QVector<EditorCompletionItem> items;
  bool explicitRequest = false;
  QString status;
};
Q_DECLARE_METATYPE(EditorCompletionResult)
// One active task per editor, two worker threads globally; context/revision
// changes cancel old work. No drivers or live catalog calls are used here.
class SqlCompletionController final : public QObject {
  Q_OBJECT
public:
  enum class Activation { Automatic, ColumnList, Manual };
  using Read = std::function<std::pair<QString, int>()>;
  SqlCompletionController(Read, QObject *parent = nullptr);
  ~SqlCompletionController() override;
  void setContext(navidog::DatabaseKind, const QString &, const QString &, bool calciteExtensions);
  void setMetadataSnapshot(navidog::MetadataSnapshotPtr);
  void invalidate();
  void request(Activation = Activation::Automatic);
signals:
  void ready(const EditorCompletionResult &);
  void cleared();

private:
  Read read_;
  QTimer timer_;
  navidog::DatabaseKind kind_ = navidog::DatabaseKind::Unknown;
  QString database_, schema_;
  navidog::MetadataSnapshotPtr metadata_;
  bool calciteExtensions_ = false;
  std::uint64_t generation_ = 0, runningGeneration_ = 0;
  bool running_ = false, requested_ = false;
  Activation activation_ = Activation::Automatic;
  std::shared_ptr<std::atomic_bool> cancel_;
  void start();
};
