#pragma once
#include "compiler.hpp"
#include <memory>

namespace navidog::sql::compiler {
// One analysis operation owns one context. It overlays declarations made by a
// hypothetical script on an immutable catalog; it never changes that catalog,
// persists state, opens a connection, or assumes the script has been executed.
class ScriptContext final : public CatalogSnapshot {
public:
  ScriptContext(Dialect, const CatalogSnapshot *, NameResolutionContext = {});
  ~ScriptContext();
  ScriptContext(const ScriptContext &) = delete;
  ScriptContext &operator=(const ScriptContext &) = delete;

  std::optional<Diagnostic> observe(const ModelProgram &, QueryId);
  std::optional<Diagnostic> declareRoutine(const std::vector<std::string> &,
                                           RoutineDefinition, SourceRange = {},
                                           bool ifNotExists = false);
  // Unmodeled namespace or transaction changes remove assumptions.
  void invalidate();
  bool current() const override;
  bool relationNameEquals(std::string_view, std::string_view) const override;
  bool columnNameEquals(std::string_view, std::string_view) const override;
  std::optional<TableDefinition>
  findTable(const NameResolutionContext &,
            const std::vector<std::string> &) const override;
  bool tableLookupComplete(const NameResolutionContext &,
                           const std::vector<std::string> &) const override;
  std::vector<RoutineDefinition> findRoutines(const NameResolutionContext &,
                                              const std::vector<std::string> &,
                                              bool) const override;
  bool routineLookupComplete(const NameResolutionContext &,
                             const std::vector<std::string> &,
                             bool) const override;
  std::vector<CatalogObjectDefinition> listObjects(const NameResolutionContext&,CatalogObjectKind,std::size_t) const override;
  std::vector<TableDefinition> listRelations(const NameResolutionContext &,
                                             std::size_t) const override;
  std::vector<RoutineDefinition> listRoutines(const NameResolutionContext &,
                                              bool, std::size_t) const override;
  std::vector<TableDefinition> matchingRelations(const NameResolutionContext &,std::size_t,
                                                 const CatalogNameFilter &) const override;
  std::vector<RoutineDefinition> matchingRoutines(const NameResolutionContext &,bool,std::size_t,
                                                  const CatalogNameFilter &) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
} // namespace navidog::sql::compiler
