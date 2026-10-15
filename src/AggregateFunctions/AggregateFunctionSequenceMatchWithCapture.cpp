#include <AggregateFunctions/AggregateFunctionSequenceMatch.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnTuple.h>
#include <Common/Arena.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeObject.h>
#include <DataTypes/DataTypeTuple.h>
#include <IO/VarInt.h>

#include <unordered_set>

namespace DB
{

namespace
{

/// State of the sequenceMatchEvents*WithCapture functions: the events, plus for every condition an
/// event satisfies, the value of that condition's capture argument on the event's row.
///
/// Capture values are JSON values serialized into the aggregation arena with
/// IColumn::serializeValueIntoArena (as groupArray does for non-trivial types), so sorting the events
/// only moves small entries and only the captures of matched events are ever read back.
template <typename T>
struct AggregateFunctionSequenceMatchCaptureData final
{
    using Timestamp = T;
    using Events = std::bitset<max_events>;

    /// `first` and `second` as in the std::pair the matcher reads for the other functions.
    struct TimestampEvents
    {
        Timestamp first;
        Events second;
        /// index in `captures` of the capture of this event's lowest set condition
        UInt64 captures_offset;
    };

    struct Comparator
    {
        bool operator()(const TimestampEvents & lhs, const TimestampEvents & rhs) const { return lhs.first < rhs.first; }
    };

    static constexpr bool stores_captures = true;

    bool sorted = true;
    PODArrayWithStackMemory<TimestampEvents, 64> events_list;
    /// One per set condition of each event, in condition order; an empty StringRef is a NULL capture.
    PODArrayWithStackMemory<StringRef, 64> captures;
    /// sequenceMatch conditions met at least once in events_list
    Events conditions_met;

    static StringRef serializeCapture(const IColumn & column, size_t row_num, Arena & arena)
    {
        const IColumn * values = &column;
        if (const auto * nullable = typeid_cast<const ColumnNullable *>(values))
        {
            if (nullable->isNullAt(row_num))
                return {};
            values = &nullable->getNestedColumn();
        }
        const char * begin = nullptr;
        return values->serializeValueIntoArena(row_num, arena, begin);
    }

    static StringRef copyCapture(StringRef capture, Arena & arena)
    {
        if (!capture.size)
            return {};
        return {arena.insert(capture.data, capture.size), capture.size};
    }

    /// `capture_columns[i]` is the capture argument of condition i.
    void add(const Timestamp timestamp, const Events & events, const IColumn ** capture_columns, size_t row_num, Arena & arena)
    {
        /// store information exclusively for rows with at least one event
        if (!events.any())
            return;

        events_list.push_back(TimestampEvents{timestamp, events, captures.size()});
        for (size_t condition = 0; condition < max_events; ++condition)
            if (events.test(condition))
                captures.push_back(serializeCapture(*capture_columns[condition], row_num, arena));
        sorted = false;
        conditions_met |= events;
    }

    void merge(const AggregateFunctionSequenceMatchCaptureData & other, Arena * arena)
    {
        if (other.events_list.empty())
            return;

        const size_t captures_shift = captures.size();
        for (const auto & event : other.events_list)
            events_list.push_back(TimestampEvents{event.first, event.second, event.captures_offset + captures_shift});
        for (const auto & capture : other.captures)
            captures.push_back(copyCapture(capture, *arena));
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

    /// The capture of `condition` (which the event satisfies) on the event at `event_index`.
    StringRef capture(size_t event_index, size_t condition) const
    {
        const auto & event = events_list[event_index];
        const Events lower_conditions = event.second & Events((UInt64{1} << condition) - 1);
        return captures[event.captures_offset + lower_conditions.count()];
    }

    void serialize(WriteBuffer & buf) const
    {
        writeBinary(sorted, buf);
        writeBinary(events_list.size(), buf);

        for (size_t i = 0; i < events_list.size(); ++i)
        {
            const auto & event = events_list[i];
            writeBinary(event.first, buf);
            writeBinary(event.second.to_ulong(), buf);
            for (size_t condition = 0; condition < max_events; ++condition)
            {
                if (!event.second.test(condition))
                    continue;
                const StringRef value = capture(i, condition);
                writeVarUInt(value.size, buf);
                buf.write(value.data, value.size);
            }
        }
    }

    void deserialize(ReadBuffer & buf, Arena * arena)
    {
        readBinary(sorted, buf);

        size_t size = 0;
        readBinary(size, buf);

        /// As for the other functions' state: the flags are not serialized.
        conditions_met.set();

        events_list.clear();
        captures.clear();
        events_list.reserve(size);

        for (size_t i = 0; i < size; ++i)
        {
            Timestamp timestamp{};
            readBinary(timestamp, buf);

            UInt64 events = 0;
            readBinary(events, buf);

            events_list.push_back(TimestampEvents{timestamp, Events{events}, captures.size()});
            for (size_t condition = 0; condition < max_events; ++condition)
            {
                if (!((events >> condition) & 1))
                    continue;
                size_t value_size = 0;
                readVarUInt(value_size, buf);
                if (!value_size)
                {
                    captures.push_back(StringRef{});
                    continue;
                }
                char * value = arena->alloc(value_size);
                buf.readStrict(value, value_size);
                captures.push_back(StringRef{value, value_size});
            }
        }
    }
};


/// Which matched chain(s) a WithCapture function reports: the same as its twin.
enum class SequenceEventsVariant : uint8_t
{
    Longest, /// sequenceMatchEvents
    First,
    Last,
    All,
};

/// sequenceMatchEvents*WithCapture(pattern)(timestamp, cond1, ..., condN, capture1, ..., captureN):
/// the twin's result, with every event reported as (t, capture), where `capture` is the value of
/// the capture argument of the condition of the pattern step the event matched, on the event's row.
template <typename T, typename Data, SequenceEventsVariant variant>
class AggregateFunctionSequenceMatchEventsWithCapture final
    : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsWithCapture<T, Data, variant>>
{
    using Base = AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsWithCapture<T, Data, variant>>;

public:
    AggregateFunctionSequenceMatchEventsWithCapture(const DataTypes & arguments, const Array & params, const String & pattern_)
        : Base(arguments, params, pattern_, makeResultType(arguments), (arguments.size() - 1) / 2)
    {
    }

    String getName() const override
    {
        if constexpr (variant == SequenceEventsVariant::Longest)
            return "sequenceMatchEventsWithCapture";
        else if constexpr (variant == SequenceEventsVariant::First)
            return "sequenceMatchEventsFirstWithCapture";
        else if constexpr (variant == SequenceEventsVariant::Last)
            return "sequenceMatchEventsLastWithCapture";
        else
            return "sequenceMatchEventsAllWithCapture";
    }

    bool allocatesMemoryInArena() const override { return true; }

    /// A NULL capture is a value of the result (capture = NULL), not a row to skip: handle Nullable capture
    /// arguments here rather than through the -Null combinator, which would drop those rows from matching ...
    AggregateFunctionPtr getOwnNullAdapter(
        const AggregateFunctionPtr & nested_function, const DataTypes &, const Array &, const AggregateFunctionProperties &) const override
    {
        return nested_function;
    }

    /// ... and would turn the whole function into NULL for a NULL literal capture.
    std::unordered_set<size_t> getArgumentsThatCanBeOnlyNull() const override
    {
        std::unordered_set<size_t> captures;
        for (size_t i = 1 + this->num_conditions; i <= 2 * this->num_conditions; ++i)
            captures.insert(i);
        return captures;
    }

    /// The JSON type of the captures: that of the capture arguments that are not NULL literals (the
    /// factory checks they all have the same one), or plain JSON if all of them are.
    static DataTypePtr captureType(const DataTypes & arguments)
    {
        const size_t num_conditions = (arguments.size() - 1) / 2;
        for (size_t i = 1 + num_conditions; i < arguments.size(); ++i)
        {
            auto type = removeNullable(arguments[i]);
            if (!isNothing(type))
                return type;
        }
        return DataTypeFactory::instance().get("JSON");
    }

    static DataTypePtr makeResultType(const DataTypes & arguments)
    {
        DataTypePtr event = std::make_shared<DataTypeTuple>(
            DataTypes{arguments[0], makeNullable(captureType(arguments))}, Strings{"t", "capture"});
        DataTypePtr chain = std::make_shared<DataTypeArray>(event);
        if constexpr (variant == SequenceEventsVariant::All)
            return std::make_shared<DataTypeArray>(chain);
        else
            return chain;
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        auto & data = this->data(place);
        data.sort();

        const auto events_begin = std::begin(data.events_list);
        const auto events_end = std::end(data.events_list);
        auto events_it = events_begin;

        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        if constexpr (variant == SequenceEventsVariant::All)
        {
            const auto all_matches = this->backtrackingMatchEventsAll(events_it, events_end);
            ColumnArray & chains_to = assert_cast<ColumnArray &>(arr_to.getData());
            for (const auto & match : all_matches)
                insertChain(data, match, chains_to);
            arr_to.getOffsets().push_back(arr_to.getOffsets().back() + all_matches.size());
        }
        else
        {
            EventIndices match;
            if constexpr (variant == SequenceEventsVariant::Longest)
                match = this->backtrackingMatchEvents(events_it, events_end);
            else if constexpr (variant == SequenceEventsVariant::First)
                match = this->backtrackingMatchEventsFirst(events_it, events_end);
            else
                match = this->backtrackingMatchEventsLast(events_it, events_end);
            insertChain(data, match, arr_to);
        }
    }

private:
    /// One chain as Array(Tuple(t, capture)).
    void insertChain(const Data & data, const EventIndices & match, ColumnArray & chain_to) const
    {
        auto & event_to = assert_cast<ColumnTuple &>(chain_to.getData());
        auto & timestamps_to = assert_cast<ColumnVector<T> &>(event_to.getColumn(0)).getData();
        auto & captures_to = assert_cast<ColumnNullable &>(event_to.getColumn(1));

        for (size_t step = 0; step < match.size(); ++step)
        {
            const size_t index = match[step];
            timestamps_to.push_back(data.events_list[index].first);

            /// The matcher records events as a prefix of the pattern's steps: this one matched step `step`.
            const StringRef capture = data.capture(index, this->step_conditions[step]);
            if (capture.size)
            {
                std::ignore = captures_to.getNestedColumn().deserializeAndInsertFromArena(capture.data);
                captures_to.getNullMapData().push_back(0);
            }
            else
                captures_to.insertDefault();
        }
        chain_to.getOffsets().push_back(chain_to.getOffsets().back() + match.size());
    }
};

template <typename T, typename Data>
using AggregateFunctionSequenceMatchEventsWithCaptureLongest
    = AggregateFunctionSequenceMatchEventsWithCapture<T, Data, SequenceEventsVariant::Longest>;
template <typename T, typename Data>
using AggregateFunctionSequenceMatchEventsWithCaptureFirst
    = AggregateFunctionSequenceMatchEventsWithCapture<T, Data, SequenceEventsVariant::First>;
template <typename T, typename Data>
using AggregateFunctionSequenceMatchEventsWithCaptureLast
    = AggregateFunctionSequenceMatchEventsWithCapture<T, Data, SequenceEventsVariant::Last>;
template <typename T, typename Data>
using AggregateFunctionSequenceMatchEventsWithCaptureAll
    = AggregateFunctionSequenceMatchEventsWithCapture<T, Data, SequenceEventsVariant::All>;

/// f(pattern)(timestamp, cond1, ..., condN, capture1, ..., captureN), 2 <= N <= max_events, conditions UInt8,
/// captures JSON / Nullable(JSON) / NULL, all of the same JSON type.
template <template <typename, typename> typename AggregateFunction, template <typename> typename Data>
AggregateFunctionPtr createAggregateFunctionSequenceWithCapture(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *)
{
    if (params.size() != 1)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Aggregate function {} requires exactly one parameter.", name);

    const auto arg_count = argument_types.size();
    /// An odd count most likely means a forgotten capture: say so before anything else.
    if (arg_count >= 1 && (arg_count - 1) % 2 != 0)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Aggregate function {} requires a timestamp, the conditions and then exactly one capture per condition, "
            "got {} arguments after the timestamp.", name, arg_count - 1);
    if (arg_count < 5)
        throw Exception(ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION,
            "Aggregate function {} requires a timestamp, at least 2 conditions and one capture per condition.", name);

    const size_t num_conditions = (arg_count - 1) / 2;
    if (num_conditions > max_events)
        throw Exception(ErrorCodes::TOO_MANY_ARGUMENTS_FOR_FUNCTION, "Aggregate function {} supports up to {} event arguments.", name, max_events);

    for (size_t i = 1; i <= num_conditions; ++i)
    {
        if (!isUInt8(argument_types[i]))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal type {} of argument {} of aggregate function {}, must be UInt8", argument_types[i]->getName(), i + 1, name);
    }

    DataTypePtr capture_type;
    for (size_t i = num_conditions + 1; i < arg_count; ++i)
    {
        const auto type = removeNullable(argument_types[i]);
        if (isNothing(type))
            continue;
        const auto * json_type = typeid_cast<const DataTypeObject *>(type.get());
        if (!json_type || json_type->getSchemaFormat() != DataTypeObject::SchemaFormat::JSON)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal type {} of argument {} of aggregate function {}: a capture must be JSON, Nullable(JSON) or NULL",
                argument_types[i]->getName(), i + 1, name);
        if (!capture_type)
            capture_type = type;
        else if (!capture_type->equals(*type))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Illegal type {} of argument {} of aggregate function {}: all captures must have the same JSON type, "
                "but an earlier capture is {}", argument_types[i]->getName(), i + 1, name, capture_type->getName());
    }

    String pattern = params.front().safeGet<std::string>();

    AggregateFunctionPtr res(createWithUnsignedIntegerType<AggregateFunction, Data>(*argument_types[0], argument_types, params, pattern));
    if (res)
        return res;

    WhichDataType which(argument_types.front().get());
    if (which.isDateTime())
        return std::make_shared<AggregateFunction<DataTypeDateTime::FieldType, Data<DataTypeDateTime::FieldType>>>(argument_types, params, pattern);
    if (which.isDate())
        return std::make_shared<AggregateFunction<DataTypeDate::FieldType, Data<DataTypeDate::FieldType>>>(argument_types, params, pattern);

    throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
        "Illegal type {} of first argument of aggregate function {}, must be DateTime", argument_types.front()->getName(), name);
}

}

void registerAggregateFunctionsSequenceMatchWithCapture(AggregateFunctionFactory & factory)
{
    factory.registerFunction("sequenceMatchEventsWithCapture",
        createAggregateFunctionSequenceWithCapture<AggregateFunctionSequenceMatchEventsWithCaptureLongest, AggregateFunctionSequenceMatchCaptureData>);
    factory.registerFunction("sequenceMatchEventsFirstWithCapture",
        createAggregateFunctionSequenceWithCapture<AggregateFunctionSequenceMatchEventsWithCaptureFirst, AggregateFunctionSequenceMatchCaptureData>);
    factory.registerFunction("sequenceMatchEventsLastWithCapture",
        createAggregateFunctionSequenceWithCapture<AggregateFunctionSequenceMatchEventsWithCaptureLast, AggregateFunctionSequenceMatchCaptureData>);
    factory.registerFunction("sequenceMatchEventsAllWithCapture",
        createAggregateFunctionSequenceWithCapture<AggregateFunctionSequenceMatchEventsWithCaptureAll, AggregateFunctionSequenceMatchCaptureData>);
}

}
