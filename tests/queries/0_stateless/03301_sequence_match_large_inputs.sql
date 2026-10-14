-- These inputs used to exceed the sequence matching iteration budget (1000000 iterations), which
-- first failed the whole query with TOO_SLOW, and then made the search report "no match". With the
-- memoized matcher they need O(actions * events) iterations and are matched exactly. The expected
-- results were cross-checked against an unbounded run of the original backtracking algorithm.

-- No match: '(?1)(?t>1000)(?2)' used to need ~n^2/2 iterations to find out. sequenceMatchEvents*
-- report the longest partial chain when there is no complete match.
SELECT sequenceMatch('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceCount('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEvents('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEventsFirst('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEventsLast('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);
SELECT sequenceMatchEventsAll('(?1)(?t>1000)(?2)')(toUInt32(number), number % 2 = 0, number = 1) FROM numbers(3000);

-- A match that the original algorithm only reaches after ~2500000 iterations: every earlier start
-- tries each event of its 1000 long window first.
SELECT sequenceMatch('(?1)(?t<1000)(?2)')(toUInt32(number), number % 2 = 0, number = 5999) FROM numbers(6000);
SELECT sequenceCount('(?1)(?t<1000)(?2)')(toUInt32(number), number % 2 = 0, number = 5999) FROM numbers(6000);
SELECT sequenceMatchEvents('(?1)(?t<1000)(?2)')(toUInt32(number), number % 2 = 0, number = 5999) FROM numbers(6000);
SELECT sequenceMatchEventsFirst('(?1)(?t<1000)(?2)')(toUInt32(number), number % 2 = 0, number = 5999) FROM numbers(6000);
SELECT sequenceMatchEventsLast('(?1)(?t<1000)(?2)')(toUInt32(number), number % 2 = 0, number = 5999) FROM numbers(6000);
SELECT sequenceMatchEventsAll('(?1)(?t<1000)(?2)')(toUInt32(number), number % 2 = 0, number = 5999) FROM numbers(6000);

-- Linear in the number of events, but more than 1000000 events.
SELECT sequenceCount('(?1)(?2)')(toUInt32(number), number % 2 = 0, number = 2000001) FROM numbers(2000002);

-- Groups are independent.
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
