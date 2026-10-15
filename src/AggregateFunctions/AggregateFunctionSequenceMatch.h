#pragma once

/// Internal to AggregateFunctionSequenceMatch.cpp and AggregateFunctionSequenceMatchWithCapture.cpp:
/// the state of the sequenceMatch family, the pattern parser and the backtracking matcher.

#include <AggregateFunctions/Helpers.h>
#include <AggregateFunctions/AggregateFunctionFactory.h>

#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>

#include <AggregateFunctions/IAggregateFunction.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeObject.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnArray.h>
#include <Common/Arena.h>
#include <IO/VarInt.h>
#include <Common/assert_cast.h>
#include "Functions/array/length.h"
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <base/range.h>

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>


namespace DB
{

struct Settings;

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int TOO_MANY_ARGUMENTS_FOR_FUNCTION;
    extern const int TOO_FEW_ARGUMENTS_FOR_FUNCTION;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int SYNTAX_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
}

namespace
{

/// helper type for comparing `std::pair`s using solely the .first member
template <template <typename> class Comparator>
struct ComparePairFirst final
{
    template <typename T1, typename T2>
    bool operator()(const std::pair<T1, T2> & lhs, const std::pair<T1, T2> & rhs) const
    {
        return Comparator<T1>{}(lhs.first, rhs.first);
    }
};

constexpr size_t max_events = 32;

/// Matched events as indices into the (sorted) events list of the state.
using EventIndices = std::vector<size_t>;

template <typename T>
struct AggregateFunctionSequenceMatchData final
{
    using Timestamp = T;
    using Events = std::bitset<max_events>;
    using TimestampEvents = std::pair<Timestamp, Events>;
    using Comparator = ComparePairFirst<std::less>;

    static constexpr bool stores_captures = false;

    bool sorted = true;
    PODArrayWithStackMemory<TimestampEvents, 64> events_list;
    /// sequenceMatch conditions met at least once in events_list
    Events conditions_met;

    void add(const Timestamp timestamp, const Events & events)
    {
        /// store information exclusively for rows with at least one event
        if (events.any())
        {
            events_list.emplace_back(timestamp, events);
            sorted = false;
            conditions_met |= events;
        }
    }

    void merge(const AggregateFunctionSequenceMatchData & other, Arena *)
    {
        if (other.events_list.empty())
            return;

        events_list.insert(std::begin(other.events_list), std::end(other.events_list));
        sorted = false;
        conditions_met |= other.conditions_met;
    }

    void sort()
    {
        if (sorted)
            return;

        ::sort(std::begin(events_list), std::end(events_list), Comparator{});
        sorted = true;
    }

    void serialize(WriteBuffer & buf) const
    {
        writeBinary(sorted, buf);
        writeBinary(events_list.size(), buf);

        for (const auto & events : events_list)
        {
            writeBinary(events.first, buf);
            writeBinary(events.second.to_ulong(), buf);
        }
    }

    void deserialize(ReadBuffer & buf, Arena *)
    {
        readBinary(sorted, buf);

        size_t size;
        readBinary(size, buf);

        /// If we lose these flags, functionality is broken
        /// If we serialize/deserialize these flags, we have compatibility issues
        /// If we set these flags to 1, we have a minor performance penalty, which seems acceptable
        conditions_met.set();

        events_list.clear();
        events_list.reserve(size);

        for (size_t i = 0; i < size; ++i)
        {
            Timestamp timestamp;
            readBinary(timestamp, buf);

            UInt64 events;
            readBinary(events, buf);

            events_list.emplace_back(timestamp, Events{events});
        }
    }
};


/// Max number of iterations to match the pattern against a sequence. When exceeded, the search
/// is abandoned and treated as "no match" (rather than failing the whole query).
constexpr auto sequence_match_max_iterations = 1000000;


template <typename T, typename Data, typename Derived>
class AggregateFunctionSequenceBase : public IAggregateFunctionDataHelper<Data, Derived>
{
public:
    /// `num_conditions_`: how many arguments after the timestamp are conditions (default: all of them;
    /// the WithCapture functions have one capture argument per condition after the conditions).
    AggregateFunctionSequenceBase(const DataTypes & arguments, const Array & params, const String & pattern_, const DataTypePtr & result_type_,
        size_t num_conditions_ = std::numeric_limits<size_t>::max())
        : IAggregateFunctionDataHelper<Data, Derived>(arguments, params, result_type_)
        , pattern(pattern_)
    {
        num_conditions = num_conditions_ == std::numeric_limits<size_t>::max() ? arguments.size() - 1 : num_conditions_;
        parsePattern();
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, const size_t row_num, Arena * arena) const override
    {
        const auto timestamp = assert_cast<const ColumnVector<T> *>(columns[0])->getData()[row_num];

        typename Data::Events events;
        for (const auto i : collections::range(1, num_conditions + 1))
        {
            const auto event = assert_cast<const ColumnUInt8 *>(columns[i])->getData()[row_num];
            events.set(i - 1, event);
        }

        if constexpr (Data::stores_captures)
            this->data(place).add(timestamp, events, columns + 1 + num_conditions, row_num, *arena);
        else
            this->data(place).add(timestamp, events);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * arena) const override
    {
        this->data(place).merge(this->data(rhs), arena);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).serialize(buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena * arena) const override
    {
        this->data(place).deserialize(buf, arena);
    }

    bool haveSameStateRepresentationImpl(const IAggregateFunction & rhs) const override
    {
        return this->getName() == rhs.getName() && this->haveEqualArgumentTypes(rhs);
    }

private:
    enum class PatternActionType : uint8_t
    {
        SpecificEvent,
        AnyEvent,
        KleeneStar,
        TimeLessOrEqual,
        TimeLess,
        TimeGreaterOrEqual,
        TimeGreater,
        TimeEqual,
        /// `(?t>=X&&t<=Y)`: the time since the previous event lies in the inclusive range [extra, upper]
        TimeRange
    };

    struct PatternAction final
    {
        PatternActionType type;
        std::uint64_t extra;
        /// inclusive upper bound of a TimeRange (`extra` is its inclusive lower bound)
        std::uint64_t upper;

        PatternAction() = default;
        explicit PatternAction(const PatternActionType type_, const std::uint64_t extra_ = 0, const std::uint64_t upper_ = 0)
            : type{type_}, extra{extra_}, upper{upper_} {}
    };

    using PatternActions = PODArrayWithStackMemory<PatternAction, 64>;

    Derived & derived() { return static_cast<Derived &>(*this); }

    void parsePattern()
    {
        actions.clear();
        actions.emplace_back(PatternActionType::KleeneStar);
        step_conditions.clear();

        dfa_states.clear();
        dfa_states.emplace_back(true);

        pattern_has_time = false;

        const char * pos = pattern.data();
        const char * begin = pos;
        const char * end = pos + pattern.size();

        auto throw_exception = [&](const std::string & msg)
        {
            throw Exception(ErrorCodes::SYNTAX_ERROR, "{} '{}' at position {}", msg, std::string(pos, end), toString(pos - begin));
        };

        auto match = [&pos, end](const char * str) mutable
        {
            size_t length = strlen(str);
            if (pos + length <= end && 0 == memcmp(pos, str, length))
            {
                pos += length;
                return true;
            }
            return false;
        };

        while (pos < end)
        {
            if (match("(?"))
            {
                if (match("t"))
                {
                    const auto parse_time_operand = [&]
                    {
                        PatternActionType operand_type = PatternActionType::TimeEqual;
                        if (match("<="))
                            operand_type = PatternActionType::TimeLessOrEqual;
                        else if (match("<"))
                            operand_type = PatternActionType::TimeLess;
                        else if (match(">="))
                            operand_type = PatternActionType::TimeGreaterOrEqual;
                        else if (match(">"))
                            operand_type = PatternActionType::TimeGreater;
                        else if (match("=="))
                            operand_type = PatternActionType::TimeEqual;
                        else
                            throw_exception("Unknown time condition");

                        UInt64 operand_duration = 0;
                        const auto * prev_pos = pos;
                        pos = tryReadIntText(operand_duration, pos, end);
                        if (pos == prev_pos)
                            throw_exception("Could not parse number");
                        return std::pair{operand_type, operand_duration};
                    };

                    auto [type, duration] = parse_time_operand();
                    UInt64 range_upper = 0;

                    /// A two-sided bound, e.g. '(?t>=86400&&t<=604800)': more than a day but at most a week
                    /// after the previous event. Stored as one TimeRange action with inclusive bounds.
                    if (match("&&"))
                    {
                        static constexpr auto equal_not_combinable
                            = "Time condition 't==' cannot be combined with '&&', use it as a standalone condition";
                        if (type == PatternActionType::TimeEqual)
                            throw Exception(ErrorCodes::BAD_ARGUMENTS, equal_not_combinable);
                        if (!match("t"))
                            throw_exception("Expected 't' after '&&' in time condition "
                                "(both operands must be written out, e.g. '(?t>=10&&t<=30)'), found");

                        auto [second_type, second_duration] = parse_time_operand();
                        if (second_type == PatternActionType::TimeEqual)
                            throw Exception(ErrorCodes::BAD_ARGUMENTS, equal_not_combinable);
                        if (pos + 2 <= end && 0 == memcmp(pos, "&&", 2))
                            throw_exception("Only one '&&' is allowed in a time condition, found");

                        const auto is_lower_bound = [](PatternActionType t)
                        { return t == PatternActionType::TimeGreaterOrEqual || t == PatternActionType::TimeGreater; };
                        if (is_lower_bound(type) == is_lower_bound(second_type))
                            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                                "Time condition with '&&' must combine one lower bound ('t>' or 't>=') with one upper bound "
                                "('t<' or 't<='), but both operands are {} bounds", is_lower_bound(type) ? "lower" : "upper");
                        if (!is_lower_bound(type))
                        {
                            std::swap(type, second_type);
                            std::swap(duration, second_duration);
                        }

                        UInt64 lower = duration;
                        if (type == PatternActionType::TimeGreater)
                        {
                            if (duration == std::numeric_limits<UInt64>::max())
                                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                                    "Time condition 't>{}' can never be satisfied, the range is empty", duration);
                            lower = duration + 1;
                        }
                        UInt64 upper = second_duration;
                        if (second_type == PatternActionType::TimeLess)
                        {
                            if (second_duration == 0)
                                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Time condition 't<0' can never be satisfied, the range is empty");
                            upper = second_duration - 1;
                        }
                        if (lower > upper)
                            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                                "Time range is empty: inclusive lower bound {} is greater than inclusive upper bound {}", lower, upper);

                        type = PatternActionType::TimeRange;
                        duration = lower;
                        range_upper = upper;
                    }

                    if (actions.back().type != PatternActionType::SpecificEvent &&
                        actions.back().type != PatternActionType::AnyEvent &&
                        actions.back().type != PatternActionType::KleeneStar)
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Temporal condition should be preceded by an event condition");

                    pattern_has_time = true;
                    actions.emplace_back(type, duration, range_upper);
                }
                else
                {
                    UInt64 event_number = 0;
                    const auto * prev_pos = pos;
                    pos = tryReadIntText(event_number, pos, end);
                    if (pos == prev_pos)
                        throw_exception("Could not parse number");

                    if (event_number > num_conditions)
                        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Event number {} is out of range", event_number);

                    actions.emplace_back(PatternActionType::SpecificEvent, event_number - 1);
                    step_conditions.push_back(event_number - 1);
                    dfa_states.back().transition = DFATransition::SpecificEvent;
                    dfa_states.back().event = static_cast<uint32_t>(event_number - 1);
                    dfa_states.emplace_back();
                    conditions_in_pattern.set(event_number - 1);
                }

                if (!match(")"))
                    throw_exception("Expected closing parenthesis, found");

            }
            else if (match(".*"))
            {
                actions.emplace_back(PatternActionType::KleeneStar);
                dfa_states.back().has_kleene = true;
            }
            else if (match("."))
            {
                actions.emplace_back(PatternActionType::AnyEvent);
                dfa_states.back().transition = DFATransition::AnyEvent;
                dfa_states.emplace_back();
            }
            else
                throw_exception("Could not parse pattern, unexpected starting symbol");
        }
    }

protected:
    /// Uses a DFA based approach in order to better handle patterns without
    /// time assertions.
    ///
    /// NOTE: This implementation relies on the assumption that the pattern is *small*.
    ///
    /// This algorithm performs in O(mn) (with m the number of DFA states and N the number
    /// of events) with a memory consumption and memory allocations in O(m). It means that
    /// if n >>> m (which is expected to be the case), this algorithm can be considered linear.
    template <typename EventEntry>
    bool dfaMatch(EventEntry & events_it, const EventEntry events_end) const
    {
        using ActiveStates = std::vector<bool>;

        /// Those two vectors keep track of which states should be considered for the current
        /// event as well as the states which should be considered for the next event.
        ActiveStates active_states(dfa_states.size(), false);
        ActiveStates next_active_states(dfa_states.size(), false);
        active_states[0] = true;

        /// Keeps track of dead-ends in order not to iterate over all the events to realize that
        /// the match failed.
        size_t n_active = 1;

        for (/* empty */; events_it != events_end && n_active > 0 && !active_states.back(); ++events_it)
        {
            n_active = 0;
            next_active_states.assign(dfa_states.size(), false);

            for (size_t state = 0; state < dfa_states.size(); ++state)
            {
                if (!active_states[state])
                {
                    continue;
                }

                switch (dfa_states[state].transition)
                {
                    case DFATransition::None:
                        break;
                    case DFATransition::AnyEvent:
                        next_active_states[state + 1] = true;
                        ++n_active;
                        break;
                    case DFATransition::SpecificEvent:
                        if (events_it->second.test(dfa_states[state].event))
                        {
                            next_active_states[state + 1] = true;
                            ++n_active;
                        }
                        break;
                }

                if (dfa_states[state].has_kleene)
                {
                    next_active_states[state] = true;
                    ++n_active;
                }
            }
            swap(active_states, next_active_states);
        }

        return active_states.back();
    }

    /// State shared by all backtrackingMatch calls that compute one result over the same sorted events:
    /// the iteration budget, and a memo of the sub-searches that are known to fail.
    ///
    /// Without the memo, backtracking re-explores the same failing sub-searches from every start
    /// position, which is quadratic in the number of events for a single `.*` or time condition (and
    /// cubic for two of them), e.g. '(?1)(?t>1000)(?2)' needs ~n^2/2 steps on events that do not match.
    ///
    /// A sub-search ("state") is identified by the action it starts at and the index of the event it
    /// starts at. Its outcome does not depend on how it was reached: the time base it compares against
    /// is fixed by the previous action (the previous event after `(?N)` or `.`, the current event after
    /// `.*` or a time condition). Once a state has been explored completely without a match, it is
    /// recorded as failed and never explored again. So that the matched events reported by
    /// sequenceMatchEvents* stay exactly the same, the memo also records the length of the longest
    /// partial chain found inside each failed state and how to rebuild it: skipping a failed state
    /// then updates `best_matched_events` exactly as exploring it would have.
    ///
    /// With the memo every state is explored at most once, so the total work is
    /// O(actions * events * log(events)) instead of exponential in the number of `.*` and time conditions.
    class BacktrackingContext
    {
    public:
        static constexpr Int32 unknown = -1;

        /// Per action: whether it follows `.*` or a time condition (its states are the ones such an action
        /// retries, which are skipped in bulk), and whether it is `.*` or a time condition itself.
        static constexpr UInt8 retried_child = 1;
        static constexpr UInt8 retrying = 2;

        BacktrackingContext(std::vector<UInt8> action_kinds_, size_t num_events_)
            : action_kinds(std::move(action_kinds_))
            , num_events(num_events_)
            , memo_threshold(num_events_ < std::numeric_limits<UInt32>::max()
                ? 64 * (action_kinds.size() + num_events_) : std::numeric_limits<size_t>::max())
            , budget(std::max<size_t>(sequence_match_max_iterations, 64 * (action_kinds.size() + num_events_) + 16 * (action_kinds.size() + 1) * (num_events_ + 1)))
        {
            while (tree_size < num_events)
                tree_size *= 2;
        }

        /// A backtrack point: an action to retry, the index of the event it was last tried at and the
        /// index of the event its time conditions compare against.
        struct BacktrackPoint
        {
            size_t action;
            size_t event;
            size_t base;
        };

        /// A state on the current search path that is tracked by the memo, with the longest partial
        /// chain found so far among its completely explored children.
        struct PathState
        {
            size_t action;
            size_t event;
            /// number of backtrack points below the state: it has failed once one of those is popped
            size_t depth;
            Int32 longest_child = 0;
            size_t longest_child_event = 0;
            /// whether the state's own action is a specific event that matched
            bool matched = false;
        };

        /// Scratch space of backtrackingMatch, kept here so that repeated calls do not allocate.
        std::vector<BacktrackPoint> back_stack;
        std::vector<PathState> path;
        EventIndices current_matched_events;
        std::vector<size_t> current_matched_actions;

        size_t numEvents() const { return num_events; }

        /// Counts one iteration; returns false once the budget is exhausted. The memoized search needs
        /// O(actions * events) iterations, so the budget is only a safety net against runaway searches.
        bool step() { return ++iterations <= budget; }
        bool exhausted() const { return iterations > budget; }

        /// The memo is only switched on once the searches turned out to be expensive (realistic inputs
        /// take a small multiple of `events` iterations without it), so that searches that do not need it
        /// do not pay for maintaining it. Its cost is linear either way. Switching it on part way through
        /// a search is safe: the states entered before are simply never recorded.
        ALWAYS_INLINE bool memoEnabled()
        {
            if (memo_enabled || iterations < memo_threshold) [[likely]]
                return memo_enabled;
            memo_enabled = true;
            per_action.resize(action_kinds.size() + 1);
            return true;
        }

        /// Length of the longest partial chain inside the failed state (`action`, `event`), or `unknown`.
        Int32 failedLength(size_t action, size_t event) const
        {
            const auto & memo = per_action[action];
            if (!memo.lengths.empty())
                return memo.lengths[event];
            return memo.tree.empty() ? unknown : memo.tree[tree_size + event];
        }

        /// For a failed state starting at `.*` or a time condition: the event its longest partial chain
        /// continues from (i.e. its first child state, in search order, holding the longest chain).
        size_t longestChild(size_t action, size_t event) const { return per_action[action].longest_child[event]; }

        void setFailed(size_t action, size_t event, Int32 length, size_t longest_child)
        {
            auto & memo = allocate(action);
            if (action_kinds[action] & retried_child)
            {
                size_t node = tree_size + event;
                memo.tree[node] = length;
                for (node /= 2; node; node /= 2)
                    memo.tree[node] = std::max(memo.tree[2 * node], memo.tree[2 * node + 1]);
                memo.next_unknown[event] = static_cast<UInt32>(event + 1);
            }
            else
                memo.lengths[event] = length;
            if (action_kinds[action] & retrying)
                memo.longest_child[event] = static_cast<UInt32>(longest_child);
        }

        /// First event in [from, to) whose state at `action` is not known to fail, or `to`.
        size_t firstUnknown(size_t action, size_t from, size_t to)
        {
            auto & next = per_action[action].next_unknown;
            if (next.empty())
                return from;
            size_t root = from;
            while (next[root] != root)
                root = next[root];
            while (next[from] != root)
            {
                const size_t following = next[from];
                next[from] = static_cast<UInt32>(root);
                from = following;
            }
            return std::min(root, to);
        }

        /// The longest partial chain among the failed states (`action`, [from, to)), all of which must be
        /// known to fail, and the first event holding it.
        std::pair<Int32, size_t> longestFailed(size_t action, size_t from, size_t to) const
        {
            const auto & tree = per_action[action].tree;
            Int32 longest = unknown;
            for (size_t left = from + tree_size, right = to + tree_size; left < right; left /= 2, right /= 2)
            {
                if (left & 1)
                    longest = std::max(longest, tree[left++]);
                if (right & 1)
                    longest = std::max(longest, tree[--right]);
            }
            return {longest, firstAtLeast(tree, 1, 0, tree_size, from, to, longest)};
        }

    private:
        struct ActionMemo
        {
            /// For actions following `.*` or a time condition, whose states are skipped in bulk: a max
            /// segment tree over the events, leaves holding the longest partial chain of failed states
            /// (`unknown` for states not known to fail), and path-compressed union-find "next state not
            /// known to fail" pointers.
            std::vector<Int32> tree;
            std::vector<UInt32> next_unknown;
            /// For the other actions: the longest partial chain of failed states, or `unknown`.
            std::vector<Int32> lengths;
            /// For `.*` and time conditions: see longestChild.
            std::vector<UInt32> longest_child;
            bool allocated = false;
        };

        ActionMemo & allocate(size_t action)
        {
            auto & memo = per_action[action];
            if (!memo.allocated)
            {
                memo.allocated = true;
                if (action_kinds[action] & retried_child)
                {
                    memo.tree.assign(2 * tree_size, unknown);
                    memo.next_unknown.resize(num_events + 1);
                    std::iota(memo.next_unknown.begin(), memo.next_unknown.end(), UInt32{0});
                }
                else
                    memo.lengths.assign(num_events, unknown);
                if (action_kinds[action] & retrying)
                    memo.longest_child.assign(num_events, 0);
            }
            return memo;
        }

        static size_t firstAtLeast(
            const std::vector<Int32> & tree, size_t node, size_t node_from, size_t node_to, size_t from, size_t to, Int32 value)
        {
            if (node_to <= from || to <= node_from || tree[node] < value)
                return to;
            if (node_to - node_from == 1)
                return node_from;
            const size_t middle = (node_from + node_to) / 2;
            const size_t found = firstAtLeast(tree, 2 * node, node_from, middle, from, to, value);
            if (found != to)
                return found;
            return firstAtLeast(tree, 2 * node + 1, middle, node_to, from, to, value);
        }

        const std::vector<UInt8> action_kinds;
        const size_t num_events;
        const size_t memo_threshold;
        const size_t budget;
        size_t tree_size = 1;
        size_t iterations = 0;
        bool memo_enabled = false;
        std::vector<ActionMemo> per_action;
    };

    BacktrackingContext makeBacktrackingContext(size_t num_events) const
    {
        std::vector<UInt8> action_kinds(actions.size(), 0);
        for (size_t i = 0; i < actions.size(); ++i)
        {
            const bool retrying = actions[i].type != PatternActionType::SpecificEvent && actions[i].type != PatternActionType::AnyEvent;
            if (retrying)
            {
                action_kinds[i] |= BacktrackingContext::retrying;
                if (i + 1 < actions.size())
                    action_kinds[i + 1] |= BacktrackingContext::retried_child;
            }
        }
        return BacktrackingContext(std::move(action_kinds), num_events);
    }

    /// For a time condition being retried at `from` (the frame it was pushed at already satisfied it):
    /// the end of the run of events from `from` on that still satisfy it. Events are sorted by time, so
    /// `(?t<..)`, `(?t<=..)` and `(?t==..)` hold up to some point and `.*`, `(?t>..)`, `(?t>=..)` to the end.
    template <typename EventEntry>
    EventEntry conditionHoldsUntil(const PatternAction & condition, EventEntry from, EventEntry base, EventEntry events_end) const
    {
        if (condition.type == PatternActionType::TimeLess)
            return std::lower_bound(from, events_end, base->first + condition.extra,
                [](const auto & event, const auto & value) { return event.first < value; });
        if (condition.type == PatternActionType::TimeLessOrEqual || condition.type == PatternActionType::TimeEqual)
            return std::upper_bound(from, events_end, base->first + condition.extra,
                [](const auto & value, const auto & event) { return value < event.first; });
        if (condition.type == PatternActionType::TimeRange)
            return std::upper_bound(from, events_end, condition.upper,
                [&](UInt64 value, const auto & event) { return value < static_cast<UInt64>(event.first - base->first); });
        return events_end;
    }

    /// For `(?t>=X&&t<=Y)`: the first event from `from` on whose time since `base` is at least X (it may be
    /// past Y already), or events_end. Equivalent to trying the events one by one, as events are sorted by time.
    template <typename EventEntry>
    EventEntry firstEventReachingRange(const PatternAction & condition, EventEntry from, EventEntry base, EventEntry events_end) const
    {
        const auto elapsed = [&](const auto & event) { return static_cast<UInt64>(event.first - base->first); };
        for (size_t probe = 0; probe < 8 && from != events_end; ++probe, ++from)
            if (elapsed(*from) >= condition.extra)
                return from;
        return std::lower_bound(from, events_end, condition.extra,
            [&](const auto & event, UInt64 value) { return elapsed(event) < value; });
    }

    /// For `(?t>=..)`, `(?t>..)` and `(?t==..)`: the first event from `from` on that satisfies the
    /// condition, or events_end. Equivalent to trying the events one by one, as events are sorted by time.
    template <typename EventEntry>
    EventEntry firstEventSatisfying(const PatternAction & condition, EventEntry from, EventEntry base, EventEntry events_end) const
    {
        const auto target = base->first + condition.extra;

        /// The satisfying event is usually close: look at a few events before searching all of them.
        for (size_t probe = 0; probe < 8 && from != events_end; ++probe, ++from)
        {
            if (condition.type == PatternActionType::TimeGreater ? from->first > target : from->first >= target)
                return (condition.type == PatternActionType::TimeEqual && from->first != target) ? events_end : from;
        }
        if (from == events_end)
            return events_end;

        if (condition.type == PatternActionType::TimeGreater)
            return std::upper_bound(from, events_end, target, [](const auto & value, const auto & event) { return value < event.first; });

        const auto found = std::lower_bound(from, events_end, target, [](const auto & event, const auto & value) { return event.first < value; });
        if (condition.type == PatternActionType::TimeEqual && found != events_end && found->first != target)
            return events_end;
        return found;
    }

    /// `anchored`: require the match to start exactly at the given `events_it`, rather than allowing
    /// the search to silently retry starting at later events (which it otherwise does: the leading
    /// synthetic KleeneStar in `actions` lets the search skip ahead and match starting anywhere in
    /// [events_it, events_end)). Needed by callers that scan position-by-position and require the
    /// result to be tied to a specific, known anchor rather than "the best match found anywhere in
    /// the remaining suffix".
    /// `shared_context`: callers that make several backtrackingMatch calls over the same events to
    /// compute a single result (sequenceCount, or a position-by-position anchored scan) pass the same
    /// context into every call, so that the iteration budget bounds the *total* work for that result
    /// and the memo of failed states is reused across the calls. Its events must end at `events_end`.
    ///
    /// The search is a depth-first backtracking search: `.*` and time conditions push a backtrack point
    /// that retries them at the next event. On top of it (see BacktrackingContext), states known to fail
    /// are skipped, and `(?t>..)`/`(?t>=..)`/`(?t==..)` jump to the first satisfying event by binary
    /// search. Neither changes what the search returns, only how much work it does.
    template <typename EventEntry, bool remember_matched_events = false, bool anchored = false>
    bool backtrackingMatch(
        EventEntry & events_it,
        const EventEntry events_end,
        EventIndices * best_matched_events = nullptr,
        BacktrackingContext * shared_context = nullptr) const
    {
        std::optional<BacktrackingContext> local_context;
        if (!shared_context)
            shared_context = &local_context.emplace(makeBacktrackingContext(static_cast<size_t>(events_end - events_it)));
        BacktrackingContext & context = *shared_context;

        /// The memo addresses events by index: the context's events end at `events_end`.
        const auto events_origin = events_end - context.numEvents();
        const auto index_of = [&](EventEntry it) ALWAYS_INLINE { return static_cast<size_t>(it - events_origin); };

        const size_t num_actions = actions.size();
        size_t action = anchored ? 1 : 0;

        const auto events_begin = events_it;
        auto base_it = events_it;

        using BacktrackPoint = typename BacktrackingContext::BacktrackPoint;
        using PathState = typename BacktrackingContext::PathState;

        auto & back_stack = context.back_stack;
        auto & current_matched_events = context.current_matched_events;
        auto & current_matched_actions = context.current_matched_actions;
        /// The states on the current search path that are tracked by the memo (always a suffix of the path).
        auto & path = context.path;
        back_stack.clear();
        current_matched_events.clear();
        current_matched_actions.clear();
        path.clear();

        [[maybe_unused]] const auto rebuild_chain = [&](size_t chain_action, size_t chain_event, Int32 length)
        {
            for (Int32 rebuilt = 0; rebuilt < length; ++chain_action)
            {
                const auto type = actions[chain_action].type;
                if (type == PatternActionType::SpecificEvent)
                {
                    best_matched_events->push_back(chain_event);
                    ++rebuilt;
                    ++chain_event;
                }
                else if (type == PatternActionType::AnyEvent)
                    ++chain_event;
                else
                    chain_event = context.longestChild(chain_action, chain_event);
            }
        };

        const auto add_child = [&](Int32 length, size_t child_event) ALWAYS_INLINE
        {
            if (!path.empty() && length > path.back().longest_child)
            {
                path.back().longest_child = length;
                path.back().longest_child_event = child_event;
            }
        };

        /// A child state (`child_action`, `child_event`) of the last state on the path is known to fail:
        /// account for it exactly as if it had been explored again.
        const auto skip_failed_child = [&]([[maybe_unused]] size_t child_action, Int32 length, size_t child_event)
        {
            add_child(length, child_event);
            if constexpr (remember_matched_events)
            {
                if (length > 0 && current_matched_events.size() + length > best_matched_events->size())
                {
                    best_matched_events->assign(current_matched_events.begin(), current_matched_events.end());
                    rebuild_chain(child_action, child_event, length);
                }
            }
        };

        /// Every tracked state above `depth` backtrack points has now been explored completely.
        const auto record_failed = [&](size_t depth)
        {
            while (!path.empty() && path.back().depth > depth)
            {
                const PathState state = path.back();
                path.pop_back();
                const Int32 length = state.longest_child + (state.matched ? 1 : 0);
                context.setFailed(state.action, state.event, length, state.longest_child_event);
                add_child(length, state.event);
            }
        };

        /// Matched events are recorded only. Adding a backtrack point here would let this traversal skip
        /// ahead and accept chains the pattern does not authorise, and that the verdict traversal rejects.
        [[maybe_unused]] const auto do_push_event = [&]() ALWAYS_INLINE
        {
            current_matched_events.push_back(index_of(events_it));
            current_matched_actions.push_back(action);
            if (best_matched_events->size() < current_matched_events.size())
                best_matched_events->assign(current_matched_events.begin(), current_matched_events.end());
        };

        /// backtrack if possible
        const auto do_backtrack = [&]
        {
            while (!back_stack.empty())
            {
                const BacktrackPoint point = back_stack.back();
                back_stack.pop_back();

                action = point.action;
                events_it = events_origin + (point.event + 1);
                base_it = events_origin + point.base;

                const bool use_memo = context.memoEnabled();
                if (use_memo)
                    record_failed(back_stack.size());

                if constexpr (remember_matched_events)
                {
                    while (!current_matched_actions.empty() && current_matched_actions.back() >= action)
                    {
                        current_matched_events.pop_back();
                        current_matched_actions.pop_back();
                    }
                }

                /// `.*` or a time condition is retried at the next events: skip those whose child state is
                /// known to fail, as if each had been tried in turn. Only within the run of events for which
                /// the condition still holds, as the search stops retrying at the first one that fails it.
                if (use_memo && events_it != events_end)
                {
                    const size_t from = index_of(events_it);
                    const size_t to = index_of(conditionHoldsUntil(actions[action], events_it, base_it, events_end));
                    if (from < to)
                    {
                        const size_t first_unknown = context.firstUnknown(action + 1, from, to);
                        if (first_unknown > from)
                        {
                            const auto [length, child_event] = context.longestFailed(action + 1, from, first_unknown);
                            skip_failed_child(action + 1, length, child_event);
                            events_it = events_origin + first_unknown;
                        }
                    }
                }

                if (events_it != events_end)
                    return true;
            }

            return false;
        };

        /// Called whenever the search moves on to the next action; returns false if the search is over.
        const auto enter_state = [&]() ALWAYS_INLINE
        {
            if (action == num_actions || events_it == events_end || !context.memoEnabled())
                return true;

            const size_t event = index_of(events_it);
            const Int32 failed_length = context.failedLength(action, event);
            /// With no backtrack point below, a failure ends the whole search, and what it returns also
            /// depends on the states explored inside it: explore it rather than skip it.
            if (failed_length != BacktrackingContext::unknown && !back_stack.empty())
            {
                skip_failed_child(action, failed_length, event);
                return do_backtrack();
            }

            path.push_back({.action = action, .event = event, .depth = back_stack.size()});
            return true;
        };

        enter_state();
        while (action != num_actions && events_it != events_end)
        {
            const PatternAction & current = actions[action];
            if (current.type == PatternActionType::SpecificEvent)
            {
                if (events_it->second.test(current.extra))
                {
                    if constexpr (remember_matched_events)
                        do_push_event();
                    if (!path.empty() && path.back().action == action && path.back().event == index_of(events_it))
                        path.back().matched = true;

                    /// move to the next action and events
                    base_it = events_it;
                    ++action, ++events_it;
                    if (!enter_state())
                        break;
                }
                else if (!do_backtrack())
                    /// backtracking failed, bail out
                    break;
            }
            else if (current.type == PatternActionType::AnyEvent)
            {
                base_it = events_it;
                ++action, ++events_it;
                if (!enter_state())
                    break;
            }
            else if (current.type == PatternActionType::KleeneStar)
            {
                back_stack.push_back({action, index_of(events_it), index_of(base_it)});
                base_it = events_it;
                ++action;
                if (!enter_state())
                    break;
            }
            else if (current.type == PatternActionType::TimeLessOrEqual || current.type == PatternActionType::TimeLess)
            {
                const bool satisfied = current.type == PatternActionType::TimeLessOrEqual
                    ? events_it->first <= base_it->first + current.extra
                    : events_it->first < base_it->first + current.extra;
                if (satisfied)
                {
                    /// condition satisfied, move onto next action
                    back_stack.push_back({action, index_of(events_it), index_of(base_it)});
                    base_it = events_it;
                    ++action;
                    if (!enter_state())
                        break;
                }
                else if (!do_backtrack())
                    break;
            }
            else if (current.type == PatternActionType::TimeGreaterOrEqual || current.type == PatternActionType::TimeGreater
                || current.type == PatternActionType::TimeEqual)
            {
                bool satisfied = false;
                if (current.type == PatternActionType::TimeGreaterOrEqual)
                    satisfied = events_it->first >= base_it->first + current.extra;
                else if (current.type == PatternActionType::TimeGreater)
                    satisfied = events_it->first > base_it->first + current.extra;
                else
                    satisfied = events_it->first == base_it->first + current.extra;

                if (satisfied)
                {
                    back_stack.push_back({action, index_of(events_it), index_of(base_it)});
                    base_it = events_it;
                    ++action;
                    if (!enter_state())
                        break;
                }
                else
                {
                    /// skip straight to the first later event satisfying the condition
                    events_it = firstEventSatisfying(current, std::next(events_it), base_it, events_end);
                    if (events_it == events_end && !do_backtrack())
                        break;
                }
            }
            else if (current.type == PatternActionType::TimeRange)
            {
                /// Like `(?t>=extra)` for events that come too early (they are skipped), and like
                /// `(?t<=upper)` for an event that comes too late (backtrack: later events are later still).
                const UInt64 elapsed = static_cast<UInt64>(events_it->first - base_it->first);
                if (elapsed < current.extra)
                {
                    /// skip straight to the first later event that is not too early
                    events_it = firstEventReachingRange(current, std::next(events_it), base_it, events_end);
                    if (events_it == events_end && !do_backtrack())
                        break;
                }
                else if (elapsed <= current.upper)
                {
                    back_stack.push_back({action, index_of(events_it), index_of(base_it)});
                    base_it = events_it;
                    ++action;
                    if (!enter_state())
                        break;
                }
                else if (!do_backtrack())
                    break;
            }
            else
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Unknown PatternActionType");

            if (!context.step())
            {
                /// Iteration budget exhausted: give up and report "no match" instead of throwing
                /// and failing the whole query. Any partially matched events are discarded too,
                /// so the abandoned search contributes nothing to the result.
                if constexpr (remember_matched_events)
                    best_matched_events->clear();
                events_it = events_end;
                return false;
            }
        }

        /// match multiple empty strings at end
        while (action != num_actions
            && (actions[action].type == PatternActionType::KleeneStar
                || actions[action].type == PatternActionType::TimeLessOrEqual
                || actions[action].type == PatternActionType::TimeLess
                || (actions[action].type == PatternActionType::TimeGreaterOrEqual && actions[action].extra == 0)
                || (actions[action].type == PatternActionType::TimeRange && actions[action].extra == 0)))
            ++action;

        if (events_it == events_begin)
            ++events_it;

        return action == num_actions;
    }

    template <typename EventEntry>
    EventIndices backtrackingMatchEvents(EventEntry & events_it, const EventEntry events_end) const
    {

        EventIndices best_matched_events;
        backtrackingMatch<EventEntry, true>(events_it, events_end, &best_matched_events);

        return best_matched_events;
    }

    /// Find the first matching sequence chronologically
    template <typename EventEntry>
    EventIndices backtrackingMatchEventsFirst(EventEntry & events_it, const EventEntry events_end) const
    {
        /// Shared across every backtrackingMatch call made below so the iteration budget bounds the
        /// *total* work for this one result, not just each individual call - otherwise a
        /// position-by-position scan could do unbounded work overall despite each call on its own
        /// staying under the limit - and so the memo of failed states is reused by all of them.
        auto context = makeBacktrackingContext(static_cast<size_t>(events_end - events_it));

        /// A plain (unanchored) call already correctly finds the earliest-starting *complete* match
        /// if one exists anywhere: backtracking explores start positions in strict left-to-right
        /// order and returns the instant any full match completes, so it can never skip a match
        /// that starts earlier in favor of one that starts later. Try that first.
        auto probe_it = events_it;
        EventIndices whole_match;
        if (backtrackingMatch<EventEntry, true>(probe_it, events_end, &whole_match, &context))
            return whole_match;

        /// No complete match exists anywhere. Fall back to the earliest partial, testing each
        /// position in turn for a match starting exactly there: a plain (unanchored) call's partial
        /// result on total failure is the longest one found anywhere in the remaining suffix, not
        /// necessarily the one starting earliest, which would contradict the documented "first,
        /// unlike sequenceMatchEvents which returns the longest" semantics.
        ///
        /// Drive the scan with a separate `anchor` iterator, advanced by exactly one candidate
        /// position at a time, and pass a *copy* into backtrackingMatch: a time constraint
        /// (?t>..., ?t<..., etc.) scans forward internally even on failure, unlike a plain
        /// SpecificEvent action, so letting the anchored call mutate the scan's own iterator could
        /// skip candidate positions entirely.
        auto anchor = events_it;
        while (anchor != events_end && !context.exhausted())
        {
            auto probe_anchor = anchor;
            EventIndices current_match;
            backtrackingMatch<EventEntry, true, true>(probe_anchor, events_end, &current_match, &context);

            if (!current_match.empty())
                return current_match;

            ++anchor;
        }

        return {};
    }

    /// Find the last matching sequence chronologically. A complete match is always preferred over
    /// a partial one - a partial result is only ever returned when no complete match exists
    /// anywhere in the data, mirroring backtrackingMatchEventsFirst's own preference.
    template <typename EventEntry>
    EventIndices backtrackingMatchEventsLast(EventEntry & events_it, const EventEntry events_end) const
    {
        EventIndices last_matched_events;
        auto events_it_copy = events_it;
        bool found_complete_match = false;

        /// Shared across every backtrackingMatch call made below (see backtrackingMatchEventsFirst).
        auto context = makeBacktrackingContext(static_cast<size_t>(events_end - events_it));

        /// Phase 1: collect complete matches via plain (unanchored) calls. A complete match is
        /// always unambiguous (backtracking stops the instant one completes), so this reliably
        /// finds every non-overlapping complete match in order; the last one collected wins
        /// outright over anything found in phase 2 below.
        while (events_it_copy != events_end)
        {
            auto anchor = events_it_copy;
            EventIndices current_match;
            bool match_result = backtrackingMatch<EventEntry, true>(events_it_copy, events_end, &current_match, &context);

            if (match_result)
            {
                last_matched_events = current_match;
                found_complete_match = true;
                continue;
            }

            /// No complete match exists anywhere from `anchor` onward (an unanchored call that
            /// fails exhausts events_it_copy all the way to events_end, so rewind to where this
            /// attempt began before falling back to an anchored, position-by-position search).
            events_it_copy = anchor;
            break;
        }

        if (found_complete_match)
            return last_matched_events;

        /// Phase 2: no complete match exists anywhere; find the latest anchor with any (partial)
        /// result. A plain (unanchored) call's partial result on total failure is the longest one
        /// found anywhere in the remaining suffix, not necessarily the one starting latest.
        ///
        /// Drive the scan with a separate `anchor` iterator, advanced by exactly one candidate
        /// position at a time, and pass a *copy* into backtrackingMatch: a time constraint
        /// (?t>..., ?t<..., etc.) scans forward internally even on failure, unlike a plain
        /// SpecificEvent action, so letting the anchored call mutate the scan's own iterator could
        /// skip later candidate positions entirely.
        auto anchor = events_it_copy;
        while (anchor != events_end)
        {
            auto probe_anchor = anchor;
            EventIndices current_match;
            backtrackingMatch<EventEntry, true, true>(probe_anchor, events_end, &current_match, &context);

            /// Budget ran out before the scan finished, so the latest partial is unknown: no match.
            if (context.exhausted())
                return {};

            if (!current_match.empty())
                last_matched_events = current_match;

            ++anchor;
        }

        return last_matched_events;
    }

    /// Find all non-overlapping matching sequences
    template <typename EventEntry>
    std::vector<EventIndices> backtrackingMatchEventsAll(EventEntry & events_it, const EventEntry events_end) const
    {
        std::vector<EventIndices> all_matches;

        /// Shared across every backtrackingMatch call made below (see backtrackingMatchEventsFirst).
        auto context = makeBacktrackingContext(static_cast<size_t>(events_end - events_it));

        /// Phase 1: collect complete matches via plain (unanchored) calls (see backtrackingMatchEventsLast).
        while (events_it != events_end)
        {
            auto anchor = events_it;
            EventIndices current_match;
            bool match_result = backtrackingMatch<EventEntry, true>(events_it, events_end, &current_match, &context);

            if (match_result)
            {
                all_matches.push_back(current_match);
                continue;
            }

            events_it = anchor;
            break;
        }

        /// Phase 2: append at most one trailing partial - the latest anchor with any result.
        /// Drive the scan with a separate `anchor` iterator (see backtrackingMatchEventsLast).
        EventIndices trailing_partial;
        auto anchor = events_it;
        while (anchor != events_end)
        {
            auto probe_anchor = anchor;
            EventIndices current_match;
            backtrackingMatch<EventEntry, true, true>(probe_anchor, events_end, &current_match, &context);

            /// Budget ran out before the scan finished, so the latest partial is unknown: keep
            /// only the complete matches found so far.
            if (context.exhausted())
            {
                trailing_partial.clear();
                break;
            }

            if (!current_match.empty())
                trailing_partial = current_match;

            ++anchor;
        }
        if (!trailing_partial.empty())
            all_matches.push_back(trailing_partial);

        return all_matches;
    }

    /// Splits the pattern into deterministic parts separated by non-deterministic fragments
    /// (time constraints and Kleene stars), and tries to match the deterministic parts in their specified order,
    /// ignoring the non-deterministic fragments.
    /// This function can quickly check that a full match is not possible if some deterministic fragment is missing.
    template <typename EventEntry>
    bool couldMatchDeterministicParts(const EventEntry events_begin, const EventEntry events_end) const
    {
        auto events_it = events_begin;

        const auto actions_end = std::end(actions);
        auto actions_it = std::begin(actions);
        auto det_part_begin = actions_it;

        auto match_deterministic_part = [&events_it, events_end, det_part_begin, actions_it]()
        {
            auto events_it_init = events_it;
            auto det_part_it = det_part_begin;

            while (det_part_it != actions_it && events_it != events_end)
            {
                /// matching any event
                if (det_part_it->type == PatternActionType::AnyEvent)
                    ++events_it, ++det_part_it;

                /// matching specific event
                else
                {
                    if (events_it->second.test(det_part_it->extra))
                        ++events_it, ++det_part_it;

                    /// abandon current matching, try to match the deterministic fragment further in the list
                    else
                    {
                        events_it = ++events_it_init;
                        det_part_it = det_part_begin;
                    }
                }
            }

            return det_part_it == actions_it;
        };

        for (; actions_it != actions_end; ++actions_it)
            if (actions_it->type != PatternActionType::SpecificEvent && actions_it->type != PatternActionType::AnyEvent)
            {
                if (!match_deterministic_part())
                    return false;
                det_part_begin = std::next(actions_it);
            }

        return match_deterministic_part();
    }

private:
    enum class DFATransition : uint8_t
    {
        ///   .-------.
        ///   |       |
        ///   `-------'
        None,
        ///   .-------.  (?[0-9])
        ///   |       | ----------
        ///   `-------'
        SpecificEvent,
        ///   .-------.      .
        ///   |       | ----------
        ///   `-------'
        AnyEvent,
    };

    struct DFAState
    {
        explicit DFAState(bool has_kleene_ = false)
            : has_kleene{has_kleene_}, event{0}, transition{DFATransition::None}
        {}

        ///   .-------.
        ///   |       | - - -
        ///   `-------'
        ///     |_^
        bool has_kleene;
        /// In the case of a state transitions with a `SpecificEvent`,
        /// `event` contains the value of the event.
        uint32_t event;
        /// The kind of transition out of this state.
        DFATransition transition;
    };

    using DFAStates = std::vector<DFAState>;

protected:
    /// The condition (0-based) of each `(?N)` step of the pattern, in pattern order. The matcher records
    /// matched events as a prefix of these steps, so the k-th recorded event matched step_conditions[k].
    std::vector<size_t> step_conditions;

    /// Number of condition arguments (after the timestamp).
    size_t num_conditions;

    /// `True` if the parsed pattern contains time assertions (?t...), `false` otherwise.
    bool pattern_has_time;
    /// sequenceMatch conditions met at least once in the pattern
    std::bitset<max_events> conditions_in_pattern;

private:
    std::string pattern;
    PatternActions actions;

    DFAStates dfa_states;
};

}

}
