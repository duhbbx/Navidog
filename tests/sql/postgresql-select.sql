SELECT i.id, i.price::numeric(10, 2) AS amount
FROM public.items AS i
WHERE i.title ILIKE $1
ORDER BY amount DESC NULLS LAST
LIMIT 10 OFFSET 5;
