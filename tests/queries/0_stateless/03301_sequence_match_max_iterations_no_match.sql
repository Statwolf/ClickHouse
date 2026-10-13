-- Exceeding the sequence matching iteration budget (sequence_match_max_iterations = 1000000) used to
-- throw TOO_SLOW ("Pattern application proves too difficult, exceeding max iterations") and fail the
-- whole query. It must now be treated as "no match" for the affected group only.

-- Backtracking budget exhausted (the time constraint makes every even event a new start candidate).
SELECT sequenceMatch('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceCount('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEvents('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEventsFirst('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEventsLast('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEventsAll('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);

-- Deterministic-parts pre-check budget exhausted (more than 1000000 events scanned for '(?1)(?2)').
SELECT sequenceCount('(?1)(?2)')(toUInt32(number), number % 2 = 0, number = 2000001) FROM numbers(2000002);

-- Other groups of the same query are unaffected and still return their matches.
SELECT
    g,
    sequenceMatch('(?1)(?t>1000)(?2)')(t, c1, c2),
    sequenceCount('(?1)(?t>1000)(?2)')(t, c1, c2),
    sequenceMatchEvents('(?1)(?t>1000)(?2)')(t, c1, c2),
    sequenceMatchEventsFirst('(?1)(?t>1000)(?2)')(t, c1, c2),
    sequenceMatchEventsLast('(?1)(?t>1000)(?2)')(t, c1, c2),
    sequenceMatchEventsAll('(?1)(?t>1000)(?2)')(t, c1, c2)
FROM
(
    SELECT 0 AS g, toUInt32(number) AS t, number % 2 = 0 AS c1, number = 1 AS c2 FROM numbers(3000)
    UNION ALL
    SELECT 1 AS g, toUInt32(number) AS t, number = 0 AS c1, number = 2000 AS c2 FROM numbers(3000)
)
GROUP BY g
ORDER BY g;
