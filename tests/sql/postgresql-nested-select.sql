WITH base(n) AS (
    SELECT 7
), active AS (
    SELECT n FROM base WHERE n > 0
)
SELECT CASE WHEN q.n = 7 THEN q.n + 1 ELSE 0 END AS answer,
       (SELECT MAX(a.n) FROM active a WHERE a.n = q.n) AS related
FROM active q
WHERE EXISTS (SELECT 1 FROM base b WHERE b.n = q.n)
ORDER BY answer;
