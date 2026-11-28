#include "optimizer_internal.hpp"
#include <algorithm>
#include <map>
namespace navidog::sql::optimizer {
namespace {
using namespace compiler;
}
void inspectAdvice(PlannerContext &c) {
  auto &r = c.result;
  std::set<std::string> seen;
  auto add = [&](AdviceKind kind, SourceRange range, const std::string &title,
                 const std::string &detail) {
    auto key = std::to_string(int(kind)) + ":" + std::to_string(range.begin) +
               ":" + title;
    if (r.advice.size() < 512 && seen.insert(key).second) {
      c.charge(sizeof(Advice) + title.size() + detail.size() + key.size());
      r.advice.push_back({kind, range, title, detail});
    }
  };
  for (const auto &rule : r.rules)
    if (rule.rule == "FilterToEmpty" || rule.rule == "FilterRemoveTrue" ||
        rule.rule == "FilterRemoveTrueConjunct")
      add(AdviceKind::ConstantPredicate, r.logical.nodes.at(rule.before).origin,
          rule.rule == "FilterToEmpty" ? "常量条件不会产生匹配行"
                                       : "可消除恒真的过滤条件",
          rule.reason + "。这是本地逻辑模型的候选；没有执行或改写原 "
                        "SQL。请确认这是否符合预期。");
  std::vector<PlanId> work{r.best.root};
  std::set<PlanId> visited;
  while (!work.empty()) {
    c.stop();
    auto id = work.back();
    work.pop_back();
    if (!id || id >= r.best.nodes.size() || !visited.insert(id).second)
      continue;
    const auto &p = r.best.nodes[id];
    const auto &n = r.logical.nodes.at(p.logical);
    if (p.kind == PhysicalKind::TableScan && p.rows > 10000)
      add(AdviceKind::FullScan, n.origin, "较大的全表扫描",
          "估计扫描 " + p.object +
              " 的全部行。应结合选择率、现有索引及原生执行计划判断是否需要索引"
              "；小表或低选择率时全表扫描也可能合理。");
    if (p.assumedStatistics &&
        (n.kind == RelKind::Scan || n.kind == RelKind::Filter ||
         n.kind == RelKind::Join))
      add(AdviceKind::MissingStatistics, n.origin, "部分估计使用缺省假设",
          "行数、NDV、NULL "
          "比例或选择率未完全来自本地统计；成本是当前假设下的相对值，不是实际耗"
          "时。优先核对统计，再判断改写收益。");
    if (n.kind == RelKind::Join && n.predicates.empty())
      add(AdviceKind::CartesianJoin, n.origin, "连接没有匹配谓词",
          "该关系产生笛卡尔积。确认是否遗漏连接条件；有意组合所有行时应保留。");
    if (p.self.memory > c.options.memoryBytes) {
      const bool sort =
          p.kind == PhysicalKind::Sort || p.kind == PhysicalKind::TopNSort ||
          p.kind == PhysicalKind::SortAggregate ||
          p.kind == PhysicalKind::SortSet || p.kind == PhysicalKind::Window;
      add(sort ? AdviceKind::SortMemory : AdviceKind::HashMemory, n.origin,
          sort ? "排序 / 窗口可能超过内存预算" : "哈希状态可能超过内存预算",
          "按当前行数 / "
          "行宽估计存在溢写；减少进入该算子的行数和列，或结合服务端实际内存设置"
          "核对。未自动修改数据库参数。");
    }
    if (n.kind == RelKind::Subquery && n.correlated)
      add(AdviceKind::CorrelatedSubquery, n.origin, "相关子查询需要重点核对",
          "本地模型按依赖关系估算重复工作；尚未执行完整去相关规则。服务端可能去"
          "相关或缓存，应对照原生执行计划的循环次数。");
    work.insert(work.end(), p.inputs.begin(), p.inputs.end());
  }
  for (const auto &q : r.logical.program.queries) {
    // Only predicate subtrees affect index access. A projection such as
    // SELECT lower(name),count(*) must not produce predicate or SELECT *
    // advice.
    for (const auto &p : q.projections) {
      const auto &e = q.expressions.at(p.expression);
      if (e.kind == ExprKind::Star)
        add(AdviceKind::StarProjection, e.origin, "检查 SELECT * 的必要性",
            "仅选择需要的列可减少传输、解码和排序 / "
            "哈希状态；若确实需要全部列，则保留。");
    }
    std::vector<ExprId> work;
    if (q.where)
      work.push_back(q.where);
    for (const auto &relation : q.relations)
      if (relation.condition)
        work.push_back(relation.condition);
    std::set<ExprId> examined;
    while (!work.empty()) {
      c.stop();
      auto id = work.back();
      work.pop_back();
      if (!examined.insert(id).second)
        continue;
      const auto &e = q.expressions.at(id);
      if (e.kind == ExprKind::Binary && e.operands.size() == 2) {
        const auto &left = q.expressions.at(e.operands[0]);
        const auto &right = q.expressions.at(e.operands[1]);
        auto wrappedColumn = [&](const ModelExpression &operand) {
          return (operand.kind == ExprKind::Call ||
                  operand.kind == ExprKind::Cast) &&
                 !referencedColumns(
                      c, {q.id, ExprId(&operand - q.expressions.data())})
                      .empty();
        };
        if ((wrappedColumn(left) || wrappedColumn(right)) &&
            (e.value == "=" || e.value == "<" || e.value == ">" ||
             e.value == "<=" || e.value == ">="))
          add(AdviceKind::NonSargablePredicate, e.origin,
              "谓词中的列包含函数 / 类型转换",
              "普通列索引可能无法直接定位；需核对函数索引、表达式索引和实际计划"
              "。未自动把转换移到另一侧，以免改变 NULL、精度或错误行为。");
        if (e.value == "LIKE" && right.kind == ExprKind::String &&
            !right.value.empty() &&
            (right.value[0] == '%' || right.value[0] == '_'))
          add(AdviceKind::LeadingWildcard, e.origin, "LIKE 使用前导通配符",
              "普通 B-tree "
              "往往不能按固定前缀定位；是否有专门索引取决于数据库、排序规则和检"
              "索语义。");
      }
      work.insert(work.end(), e.operands.begin(), e.operands.end());
    }
    // Build bounded candidates from mandatory conjunctions only. OR branches,
    // HAVING, expression indexes and cross-query references are not combined.
    struct Keys {
      std::vector<std::string> equality, range;
      SourceRange origin;
    };
    std::map<std::size_t, Keys> candidates;
    work.clear();
    if (q.where)
      work.push_back(q.where);
    for (const auto &relation : q.relations)
      if (relation.condition)
        work.push_back(relation.condition);
    examined.clear();
    auto addKey = [&](ExprId id, bool equality, SourceRange origin) {
      auto col = resolveColumn(r.logical, {q.id, id});
      if (!col || col->query != q.id || col->table >= q.tables.size())
        return;
      const auto &t = q.tables[col->table];
      if (t.query || t.pseudo || t.name.empty())
        return;
      auto &keys = candidates[col->table];
      keys.origin = origin;
      auto &list = equality ? keys.equality : keys.range;
      if (list.size() < 8 &&
          std::find(list.begin(), list.end(), col->name) == list.end())
        list.push_back(col->name);
    };
    auto scalar = [](const ModelExpression &e) {
      return e.kind == ExprKind::Number || e.kind == ExprKind::String ||
             e.kind == ExprKind::Boolean || e.kind == ExprKind::Parameter ||
             e.localSymbol;
    };
    while (!work.empty()) {
      c.stop();
      auto id = work.back();
      work.pop_back();
      if (!examined.insert(id).second)
        continue;
      const auto &e = q.expressions.at(id);
      if (e.kind == ExprKind::Binary && e.value == "AND") {
        work.insert(work.end(), e.operands.begin(), e.operands.end());
        continue;
      }
      if (e.kind == ExprKind::Binary && e.operands.size() == 2 &&
          (e.value == "=" || e.value == "<" || e.value == ">" ||
           e.value == "<=" || e.value == ">=")) {
        const auto &left = q.expressions.at(e.operands[0]),
                   &right = q.expressions.at(e.operands[1]);
        auto a = resolveColumn(r.logical, {q.id, e.operands[0]}),
             b = resolveColumn(r.logical, {q.id, e.operands[1]});
        const bool join = e.value == "=" && a && b &&
                          (a->query != b->query || a->table != b->table);
        if (scalar(right) || join)
          addKey(e.operands[0], e.value == "=", e.origin);
        if (scalar(left) || join)
          addKey(e.operands[1], e.value == "=", e.origin);
      }
    }
    for (const auto &entry : candidates) {
      c.stop();
      const auto &table = q.tables[entry.first];
      const auto &keys = entry.second;
      std::vector<std::string> columns = keys.equality;
      if (columns.size() > 3)
        columns.resize(3);
      if (columns.size() < 3)
        for (const auto &col : keys.range)
          if (std::find(columns.begin(), columns.end(), col) == columns.end()) {
            columns.push_back(col);
            break;
          }
      if (columns.empty())
        continue;
      bool covered = false;
      if (c.statistics)
        if (auto stats = c.statistics->table(q.context, table.name))
          for (const auto &index : stats->indexes) {
            if (index.columns.size() < columns.size())
              continue;
            bool matches = true;
            for (std::size_t i = 0; i < columns.size(); ++i) {
              if (i < keys.equality.size())
                matches &= std::find(keys.equality.begin(), keys.equality.end(),
                                     index.columns[i]) != keys.equality.end();
              else
                matches &= columns[i] == index.columns[i];
            }
            if (matches) {
              covered = true;
              break;
            }
          }
      if (covered)
        continue;
      std::string object;
      for (const auto &part : table.name) {
        if (!object.empty())
          object += '.';
        object += part;
      }
      std::string fields;
      for (const auto &col : columns) {
        if (!fields.empty())
          fields += ", ";
        fields += col;
      }
      add(AdviceKind::IndexCandidate, keys.origin,
          "可核对的索引候选：" + object + " (" + fields + ")",
          "来自同一查询作用域中必须满足的等值 / "
          "连接条件，随后至多一个范围键。当前统计未证明已有索引完整覆盖这些前导"
          "键；目录可能不完整。请先核对现有索引、选择率、写入开销、NULL / "
          "排序规则并用原生计划评估。没有创建索引，也没有把候选收益计入最佳计划"
          "成本。");
    }
  }
}
} // namespace navidog::sql::optimizer
