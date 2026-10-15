-- sequenceMatchEvents*WithCapture(pattern)(timestamp, cond1, ..., condN, capture1, ..., captureN):
-- the twin's matching, with every matched event reported as (t, capture), where capture is the
-- value of the capture argument of the condition of the pattern step the event matched.

DROP TABLE IF EXISTS seq_capture;
CREATE TABLE seq_capture (id UInt8, t UInt32, e UInt8, product String, price Float64) ENGINE = Memory;
-- id 1: view(1) at 0, view at 3, purchase(2) at 7, view at 20, purchase at 22
-- id 2: view at 0, purchase at 100 (outside the time window)
-- id 3: a row that is both a view and a purchase (e = 3) at 5, after a view at 0
INSERT INTO seq_capture VALUES
    (1, 0, 1, 'shoes', 10), (1, 3, 1, 'hat', 5), (1, 7, 2, 'hat', 5), (1, 20, 1, 'bag', 30), (1, 22, 2, 'bag', 30),
    (2, 0, 1, 'shoes', 10), (2, 100, 2, 'shoes', 10),
    (3, 0, 1, 'shoes', 10), (3, 5, 3, 'scarf', 7);

SELECT 'the example: capture at the first step only';
SELECT id, sequenceMatchEventsAllWithCapture('(?1)(?t>=1&&t<=7)(?2)')(
    t, bitTest(e, 0), bitTest(e, 1), map('somefield', product, 'someotherfield', toString(price))::JSON, NULL)
FROM seq_capture GROUP BY id ORDER BY id FORMAT JSONEachRow;

SELECT 'all four functions, captures at both steps';
SELECT id,
    sequenceMatchEventsWithCapture('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, map('p', product)::JSON) AS longest,
    sequenceMatchEventsFirstWithCapture('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, map('p', product)::JSON) AS first,
    sequenceMatchEventsLastWithCapture('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, map('p', product)::JSON) AS last,
    sequenceMatchEventsAllWithCapture('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, map('p', product)::JSON) AS all
FROM seq_capture GROUP BY id ORDER BY id FORMAT JSONEachRow;

SELECT 'timestamps are exactly the twins results';
SELECT id,
    arrayMap(x -> x.t, sequenceMatchEventsWithCapture('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, NULL))
        = sequenceMatchEvents('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1)),
    arrayMap(c -> arrayMap(x -> x.t, c), sequenceMatchEventsAllWithCapture('(?1)(?t<=7)(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, NULL))
        = sequenceMatchEventsAll('(?1)(?t<=7)(?2)')(t, bitTest(e, 0), bitTest(e, 1))
FROM seq_capture GROUP BY id ORDER BY id;

SELECT 'the step decides which capture a row satisfying both conditions carries';
SELECT id,
    sequenceMatchEventsAllWithCapture('(?1)(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('as', 'view')::JSON, map('as', 'purchase')::JSON),
    sequenceMatchEventsAllWithCapture('(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('as', 'view')::JSON, map('as', 'purchase')::JSON)
FROM seq_capture WHERE id = 3 GROUP BY id FORMAT JSONEachRow;

SELECT 'Nullable(JSON) captures, NULL on some rows';
SELECT sequenceMatchEventsAllWithCapture('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), if(price > 9, map('v', product)::JSON, NULL), map('p', product)::JSON)
FROM seq_capture WHERE id = 1 FORMAT JSONEachRow;

SELECT 'result types';
SELECT toTypeName(sequenceMatchEventsAllWithCapture('(?1)(?2)')(t, bitTest(e, 0), bitTest(e, 1), NULL, NULL)) FROM seq_capture;
SELECT toTypeName(sequenceMatchEventsFirstWithCapture('(?1)(?2)')(toDate(t), bitTest(e, 0), bitTest(e, 1), map('a', 1)::JSON, NULL)) FROM seq_capture;
SELECT toTypeName(sequenceMatchEventsLastWithCapture('(?1)(?2)')(toUInt64(t), bitTest(e, 0), bitTest(e, 1), NULL, map('a', 1)::JSON(a UInt8))) FROM seq_capture;

SELECT 'DateTime timestamps';
SELECT sequenceMatchEventsWithCapture('(?1)(?t>=86400&&t<=604800)(?2)')(ts, e = 1, e = 2, map('step', 'view')::JSON, map('step', 'buy')::JSON)
FROM values('ts DateTime(\'UTC\'), e UInt8', ('2026-01-01 00:00:00', 1), ('2026-01-01 06:00:00', 1), ('2026-01-03 00:00:00', 2)) FORMAT JSONEachRow;

SELECT 'State / Merge';
SELECT id, sequenceMatchEventsAllWithCaptureMerge('(?1).*(?2)')(s)
FROM (
    SELECT id, sequenceMatchEventsAllWithCaptureState('(?1).*(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, map('p', product)::JSON) AS s
    FROM seq_capture GROUP BY id, t % 2
)
GROUP BY id ORDER BY id FORMAT JSONEachRow;

SELECT 'State with a NULL literal capture';
SELECT id, finalizeAggregation(s)
FROM (
    SELECT id, sequenceMatchEventsAllWithCaptureState('(?1)(?t>=1&&t<=7)(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('v', product)::JSON, NULL) AS s
    FROM seq_capture GROUP BY id
)
ORDER BY id FORMAT JSONEachRow;

SELECT 'errors';
SELECT sequenceMatchEventsAllWithCapture('(?1)(?2)')(t, bitTest(e, 0), bitTest(e, 1), NULL) FROM seq_capture; -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT sequenceMatchEventsAllWithCapture('(?1)')(t, bitTest(e, 0), NULL) FROM seq_capture; -- { serverError TOO_FEW_ARGUMENTS_FOR_FUNCTION }
SELECT sequenceMatchEventsAllWithCapture('(?1)(?2)')(t, bitTest(e, 0), bitTest(e, 1), product, NULL) FROM seq_capture; -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT sequenceMatchEventsAllWithCapture('(?1)(?2)')(t, bitTest(e, 0), bitTest(e, 1), map('a', 1)::JSON, map('a', 1)::JSON(a UInt8)) FROM seq_capture; -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT sequenceMatchEventsAllWithCapture('(?1)(?2)')(t, toUInt16(e), bitTest(e, 1), NULL, NULL) FROM seq_capture; -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT sequenceMatchEventsAllWithCapture('(?1)(?3)')(t, bitTest(e, 0), bitTest(e, 1), NULL, NULL) FROM seq_capture; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatchEventsAllWithCapture('(?1)(?2)')(toString(t), bitTest(e, 0), bitTest(e, 1), NULL, NULL) FROM seq_capture; -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }

DROP TABLE seq_capture;
