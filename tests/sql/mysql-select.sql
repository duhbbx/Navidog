-- Original source is retained while the model normalizes pagination.
SELECT i.id, i.title, 1 + 2 * 3 AS score
FROM `items` AS i
WHERE i.id BETWEEN 1 AND 50 AND i.title IS NOT NULL
ORDER BY i.id DESC
LIMIT 5, 10;
