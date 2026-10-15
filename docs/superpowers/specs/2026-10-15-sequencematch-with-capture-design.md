# sequenceMatchEvents*WithCapture — design

Branch: `sequencematch-fast-matcher-25.4.5.24` (fastmatch-timerange). Agreed with the requester on 2026-10-15.

## Goal

Let audiencebuilder's Output Fields carry, for each event of a matched behavior, a JSON object of
fields computed on that event's row (e.g. the product viewed at step 1), next to its timestamp.

## Functions

| New function                          | Twin                       | Result type                                            |
|---------------------------------------|----------------------------|--------------------------------------------------------|
| `sequenceMatchEventsAllWithCapture`   | `sequenceMatchEventsAll`   | `Array(Array(Tuple(t T, capture Nullable(J))))`        |
| `sequenceMatchEventsFirstWithCapture` | `sequenceMatchEventsFirst` | `Array(Tuple(t T, capture Nullable(J)))`               |
| `sequenceMatchEventsLastWithCapture`  | `sequenceMatchEventsLast`  | `Array(Tuple(t T, capture Nullable(J)))`               |
| `sequenceMatchEventsWithCapture`      | `sequenceMatchEvents`      | `Array(Tuple(t T, capture Nullable(J)))`               |

`T` is the timestamp type (as for the twin); `J` is the JSON type of the capture arguments (plain
`JSON` when every capture is a `NULL` literal).

Signature: `f(pattern)(timestamp, cond1, …, condN, capture1, …, captureN)`.

- 2 ≤ N ≤ 32 (the twins' limits). The arguments after the timestamp are split in half; an odd
  count is an error.
- `cond_i`: `UInt8` (as for the twins).
- `capture_i`: `J`, `Nullable(J)` or a `NULL` literal (`Nullable(Nothing)`), where every non-NULL
  capture argument has the same JSON type `J` (any `JSON(...)` type; the in-memory format of a JSON
  value depends on its typed paths, so all captures must share one type). Anything else is an error.
- `(?K)` in the pattern refers to conditions 1..N only.

## Semantics

- Matching is exactly the twin's: same pattern syntax (including `(?t>=X&&t<=Y)`), same chains,
  same partial-chain rules, same iteration budget behaviour.
- Each event of a reported chain is `(t, capture)`: `t` is its timestamp, `capture` is the value of
  `capture_i` on that event's row, where `i` is the condition of the pattern step the event matched
  (the step decides, even when the row satisfies other conditions too). A NULL capture literal or
  value gives `capture = NULL`.
- Invariant: `arrayMap(x -> x.t, fWithCapture(...))` (per chain for `All`) equals the twin's result
  on the same input.
- Combinators (`-State`, `-Merge`, `-If`, `-Array`, ...) work as for any aggregate function. The
  serialized state is only exchanged between servers of the same build (like `groupArray` of a
  non-trivial type).

## Implementation

### State

`AggregateFunctionSequenceMatchCaptureData<T>`:

- `events_list`: `PODArray` of `{T first; Events second; UInt32 captures_offset;}` — the `first`
  and `second` names keep the existing matcher code (which reads `->first`/`->second`) unchanged.
- `captures`: `PODArray<StringRef>`, one per set condition bit of each event, in bit order:
  condition `c` of event `e` is `captures[e.captures_offset + popcount(e.second & ((1 << c) - 1))]`.
  A NULL capture is an empty `StringRef`.
- Capture values live in the aggregation `Arena`, written with `IColumn::serializeValueIntoArena`
  on the (nested, non-Nullable) JSON column and read back with `deserializeAndInsertFromArena` —
  the `groupArray` approach: no JSON text formatting or parsing, sorting moves only 24-byte entries.
- `merge` appends the other state's events (offsets shifted) and copies its capture blobs into the
  destination arena. `serialize`/`deserialize` write events and the raw blobs (size + bytes).
- `allocatesMemoryInArena()` is true for the capture functions.

### Matching

The matcher (backtrackingMatch and the First/Last/All/Events helpers) records the *index* of each
matched event instead of its timestamp. Timestamps are looked up at output time; the k-th recorded
event of a chain matched the k-th `(?N)` step of the pattern (the matcher records a prefix of the
steps, in order), so its condition is `step_conditions[k]`, computed once when the pattern is parsed.
This is the only change to the matcher; the twins map indices back to timestamps.

### Base class

`AggregateFunctionSequenceBase` gets a `num_conditions` (default: all arguments after the
timestamp), used by `add` and by the pattern's `(?K)` range check, so capture arguments are never
read as conditions. The capture functions override `add` (conditions + captures), `merge`,
`serialize` and `deserialize`.

### Factory

`createAggregateFunctionSequenceWithCapture` validates the argument layout and capture types
described above and creates the function for the timestamp type (unsigned integers, `Date`,
`DateTime`), like `createAggregateFunctionSequenceBase`.

## Errors

`NUMBER_OF_ARGUMENTS_DOESNT_MATCH` (odd count after the timestamp), `TOO_FEW_ARGUMENTS_FOR_FUNCTION`
(N < 2), `TOO_MANY_ARGUMENTS_FOR_FUNCTION` (N > 32), `ILLEGAL_TYPE_OF_ARGUMENT` (condition not
UInt8, capture not JSON/Nullable(JSON)/NULL, captures of different JSON types, bad timestamp type),
plus the twins' pattern errors.

## Testing

1. Stateless test `03303_sequence_match_events_with_capture`: the example from the request, NULL
   captures, a row satisfying several conditions, all four functions, `Date`/`DateTime`/integer
   timestamps, `-State`/`-Merge`, every error above.
2. Invariant check on the existing random query sets (20 000 + 12 000 queries incl. time ranges):
   for each query and each function, the capturing twin with `capture_i = {"c": i, "t": t}` must give
   the twin's timestamps, and every captured object must carry the condition of its pattern step and
   its own row's timestamp.
3. The existing suites and the old-vs-new comparison must still pass (the matcher now records
   indices); the benchmark must not regress.
4. Server tests: GROUP BY over many threads and a distributed `remote(...)` query (state merging
   across arenas and over the network), and a large input under a memory cap.

Built on wolfhouse with `docker/statwolf/run_build.sh`, tested locally within 16 GB.
