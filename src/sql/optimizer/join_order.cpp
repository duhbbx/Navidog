#include "optimizer_internal.hpp"

#include <algorithm>
#include <tuple>

namespace navidog::sql::optimizer {
namespace {
bool singleton(JoinMask set) { return set && !(set & (set-1)); }

struct Choice {
  double score = 0, rows = 0, width = 0;
  unsigned cartesian = 0;
  bool available = false;
};

double bounded(double value) {
  return std::clamp(value,0.,1e18);
}
} // namespace

JoinOrder searchJoinOrder(PlannerContext &context, const std::vector<JoinSearchInput> &inputs,
                          const std::vector<JoinSearchTerm> &terms, bool leftDeep) {
  if (inputs.size()<2 || inputs.size()>maxJoinSearchInputs) return {};
  const auto states=std::size_t{1}<<inputs.size();
  context.charge(states*(sizeof(Choice)+sizeof(JoinMask)));
  std::vector<Choice> best(states);
  JoinOrder result;
  result.left.resize(states);
  for (std::size_t i=0; i<inputs.size(); ++i) {
    const auto subset=JoinMask{1}<<i;
    auto rows=inputs[i].rows;
    for (const auto &term:terms)
      if (term.inputs==subset) rows=bounded(rows*term.selectivity);
    best[subset]={0,rows,inputs[i].rowBytes,0,true};
  }

  for (JoinMask subset=1; subset<states; ++subset) {
    context.stop();
    if (singleton(subset)) continue;
    for (auto left=JoinMask((subset-1)&subset); left; left=JoinMask((left-1)&subset)) {
      context.stop();
      const auto right=JoinMask(subset^left);
      if (!best[left].available || !best[right].available || (leftDeep && !singleton(right)))
        continue;
      const auto &a=best[left], &b=best[right];
      Choice step;
      step.available=true;
      step.rows=bounded(a.rows*b.rows);
      step.width=a.width+b.width;
      bool connected=false;
      for (const auto &term:terms) {
        // Apply a condition exactly once, when its entire relation set
        // first spans both children of a join.
        if ((term.inputs&subset)==term.inputs && (term.inputs&left) && (term.inputs&right)) {
          step.rows=bounded(step.rows*term.selectivity);
          connected=true;
        }
      }
      step.cartesian=a.cartesian+b.cartesian+!connected;
      const auto buildBytes=bounded(b.rows*b.width);
      const auto &options=context.options;
      const auto spill=buildBytes>options.memoryBytes?2*buildBytes/options.pageBytes:0.;
      step.score=bounded(a.score+b.score+
          Cost{bounded(a.rows+b.rows+step.rows),spill,buildBytes}.score(options.weights));
      auto &current=best[subset];
      if (!current.available || std::tie(step.cartesian,step.score)<std::tie(current.cartesian,current.score)) {
        current=step;
        result.left[subset]=left;
      }
    }
  }
  if (!best.back().available) return {};
  return result;
}
} // namespace navidog::sql::optimizer
