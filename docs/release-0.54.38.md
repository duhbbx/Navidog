# Navidog 0.54.38

- Add SQL execution safety preferences for risky writes, editor warnings, confirmation, bounded impact preview, and row-count and percentage thresholds.
- Flag unfiltered updates and deletes, provably true predicates, and destructive statements. Explain risky statements and their measured impact before execution.
- Preview supported single-table writes with read-only queries on an independent connection. Parameter values are bound separately, and failed or timed-out previews leave the static checks available.
- Support native MongoDB multi-write and Redis flush safety checks. Optional preview while editing is off by default.
