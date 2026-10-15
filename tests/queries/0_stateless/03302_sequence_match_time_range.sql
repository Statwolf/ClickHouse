-- Two-sided time conditions: '(?t>=X&&t<=Y)' constrains the time since the previous event to an
-- inclusive range; strict operands are normalized ('t>X' is 't>=X+1', 't<Y' is 't<=Y-1').
-- The expected results come from the reference implementation (Statwolf 26.8.2.7 build).

DROP TABLE IF EXISTS seq_range;
CREATE TABLE seq_range (id UInt8, t UInt32, e UInt8) ENGINE = Memory;
-- id 1: A at 0, B at 3 / 7 / 12; id 2: A at 0, B at 100; id 3: A A B with gaps 2 and 6; id 4: only A
INSERT INTO seq_range VALUES (1, 0, 1), (1, 3, 2), (1, 7, 2), (1, 12, 2), (2, 0, 1), (2, 100, 2), (3, 0, 1), (3, 2, 1), (3, 8, 2), (4, 5, 1);

SELECT 'inclusive bounds';
SELECT id,
    sequenceMatch('(?1)(?t>=5&&t<=10)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t>=7&&t<=7)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t>=8&&t<=11)(?2)')(t, e = 1, e = 2),
    sequenceCount('(?1)(?t>=5&&t<=10)(?2)')(t, e = 1, e = 2),
    sequenceMatchEvents('(?1)(?t>=5&&t<=10)(?2)')(t, e = 1, e = 2)
FROM seq_range GROUP BY id ORDER BY id;

SELECT 'strict bounds and operand order';
SELECT id,
    sequenceMatch('(?1)(?t>3&&t<7)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t>3&&t<8)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t<=10&&t>=5)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t<8&&t>2)(?2)')(t, e = 1, e = 2)
FROM seq_range GROUP BY id ORDER BY id;

SELECT 'variants';
SELECT id,
    sequenceMatchEventsFirst('(?1)(?t>=5&&t<=12)(?2)')(t, e = 1, e = 2),
    sequenceMatchEventsLast('(?1)(?t>=5&&t<=12)(?2)')(t, e = 1, e = 2),
    sequenceMatchEventsAll('(?1)(?t>=1&&t<=7)(?2)')(t, e = 1, e = 2)
FROM seq_range GROUP BY id ORDER BY id;

SELECT 'combined with other pattern elements';
SELECT id,
    sequenceMatch('(?1).*(?t>=5&&t<=7)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?1)(?t>=5&&t<=6)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t>=1&&t<=5)(?2)(?t>=4&&t<=4)(?2)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t>=0&&t<=10)')(t, e = 1, e = 2),
    sequenceMatch('(?1)(?t>=3&&t<=10)')(t, e = 1, e = 2)
FROM seq_range GROUP BY id ORDER BY id;

SELECT 'days';
SELECT sequenceMatch('(?1)(?t>=86400&&t<=604800)(?2)')(ts, e = 1, e = 2)
FROM values('ts DateTime, e UInt8', ('2026-01-01 00:00:00', 1), ('2026-01-03 00:00:00', 2));
SELECT sequenceMatch('(?1)(?t>=86400&&t<=604800)(?2)')(ts, e = 1, e = 2)
FROM values('ts DateTime, e UInt8', ('2026-01-01 00:00:00', 1), ('2026-01-01 12:00:00', 2), ('2026-01-10 00:00:00', 2));

SELECT 'errors';
SELECT sequenceMatch('(?1)(?t>=5 && t<=10)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError SYNTAX_ERROR }
SELECT sequenceMatch('(?1)(?t>=5&&t<=10&&t<=8)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError SYNTAX_ERROR }
SELECT sequenceMatch('(?1)(?t>=5&&x<=10)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError SYNTAX_ERROR }
SELECT sequenceMatch('(?1)(?t>=5&&t!=10)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError SYNTAX_ERROR }
SELECT sequenceMatch('(?1)(?t>=5&&t<=)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError SYNTAX_ERROR }
SELECT sequenceMatch('(?1)(?t==5&&t<=10)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>=5&&t==7)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>=5&&t>8)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t<=5&&t<8)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>=10&&t<=5)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>5&&t<6)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>=0&&t<0)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>18446744073709551615&&t<=5)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }
SELECT sequenceMatch('(?1)(?t>=5&&t<=7)(?t<=3)(?2)')(t, e = 1, e = 2) FROM seq_range; -- { serverError BAD_ARGUMENTS }

DROP TABLE seq_range;
