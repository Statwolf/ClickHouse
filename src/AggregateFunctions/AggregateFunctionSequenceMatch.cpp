#include <AggregateFunctions/AggregateFunctionSequenceMatch.h>

namespace DB
{

namespace
{

template <typename T, typename Data>
class AggregateFunctionSequenceMatch final : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatch<T, Data>>
{
public:
    AggregateFunctionSequenceMatch(const DataTypes & arguments, const Array & params, const String & pattern_)
        : AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatch<T, Data>>(arguments, params, pattern_, std::make_shared<DataTypeUInt8>()) {}

    using AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatch<T, Data>>::AggregateFunctionSequenceBase;

    String getName() const override { return "sequenceMatch"; }

    bool allocatesMemoryInArena() const override { return false; }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        auto & output = assert_cast<ColumnUInt8 &>(to).getData();
        if ((this->conditions_in_pattern & this->data(place).conditions_met) != this->conditions_in_pattern)
        {
            output.push_back(false);
            return;
        }
        this->data(place).sort();

        const auto & data_ref = this->data(place);

        const auto events_begin = std::begin(data_ref.events_list);
        const auto events_end = std::end(data_ref.events_list);
        auto events_it = events_begin;

        bool match = (this->pattern_has_time ?
            (this->couldMatchDeterministicParts(events_begin, events_end) && this->backtrackingMatch(events_it, events_end)) :
            this->dfaMatch(events_it, events_end));
        output.push_back(match);
    }
};

template <typename T, typename Data>
class AggregateFunctionSequenceMatchEvents final : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEvents<T, Data>>
{
public:
    AggregateFunctionSequenceMatchEvents(const DataTypes & arguments, const Array & params, const String & pattern_)
        : AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEvents<T, Data>>(arguments, params, pattern_, std::make_shared<DataTypeArray>(arguments[0])) {}

    using AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEvents<T, Data>>::AggregateFunctionSequenceBase;

    String getName() const override { return "sequenceMatchEvents"; }

    bool allocatesMemoryInArena() const override { return false; }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        this->data(place).sort();
        auto result_vec = getEvents(place);
        size_t size = result_vec.size();

        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = arr_to.getOffsets();

        offsets_to.push_back(offsets_to.back() + size);
        if (size)
        {
            typename ColumnVector<T>::Container & data_to = assert_cast<ColumnVector<T> &>(arr_to.getData()).getData();
            const auto & events = this->data(place).events_list;

            for (auto index : result_vec)
                data_to.push_back(events[index].first);
        }
    }

private:
    EventIndices getEvents(ConstAggregateDataPtr __restrict place) const
    {
        EventIndices res;
        const auto & data_ref = this->data(place);

        const auto events_begin = std::begin(data_ref.events_list);
        const auto events_end = std::end(data_ref.events_list);
        auto events_it = events_begin;

        res = this->backtrackingMatchEvents(events_it, events_end);

        return res;
    }
};

template <typename T, typename Data>
class AggregateFunctionSequenceMatchEventsFirst final : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsFirst<T, Data>>
{
public:
    AggregateFunctionSequenceMatchEventsFirst(const DataTypes & arguments, const Array & params, const String & pattern_)
        : AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsFirst<T, Data>>(arguments, params, pattern_, std::make_shared<DataTypeArray>(arguments[0])) {}

    using AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsFirst<T, Data>>::AggregateFunctionSequenceBase;

    String getName() const override { return "sequenceMatchEventsFirst"; }

    bool allocatesMemoryInArena() const override { return false; }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        this->data(place).sort();
        auto result_vec = getEvents(place);
        size_t size = result_vec.size();

        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = arr_to.getOffsets();

        offsets_to.push_back(offsets_to.back() + size);
        if (size)
        {
            typename ColumnVector<T>::Container & data_to = assert_cast<ColumnVector<T> &>(arr_to.getData()).getData();
            const auto & events = this->data(place).events_list;

            for (auto index : result_vec)
                data_to.push_back(events[index].first);
        }
    }

private:
    EventIndices getEvents(ConstAggregateDataPtr __restrict place) const
    {
        EventIndices res;
        const auto & data_ref = this->data(place);

        const auto events_begin = std::begin(data_ref.events_list);
        const auto events_end = std::end(data_ref.events_list);
        auto events_it = events_begin;

        res = this->backtrackingMatchEventsFirst(events_it, events_end);

        return res;
    }
};

template <typename T, typename Data>
class AggregateFunctionSequenceMatchEventsLast final : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsLast<T, Data>>
{
public:
    AggregateFunctionSequenceMatchEventsLast(const DataTypes & arguments, const Array & params, const String & pattern_)
        : AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsLast<T, Data>>(arguments, params, pattern_, std::make_shared<DataTypeArray>(arguments[0])) {}

    using AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsLast<T, Data>>::AggregateFunctionSequenceBase;

    String getName() const override { return "sequenceMatchEventsLast"; }

    bool allocatesMemoryInArena() const override { return false; }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        this->data(place).sort();
        auto result_vec = getEvents(place);
        size_t size = result_vec.size();

        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = arr_to.getOffsets();

        offsets_to.push_back(offsets_to.back() + size);
        if (size)
        {
            typename ColumnVector<T>::Container & data_to = assert_cast<ColumnVector<T> &>(arr_to.getData()).getData();
            const auto & events = this->data(place).events_list;

            for (auto index : result_vec)
                data_to.push_back(events[index].first);
        }
    }

private:
    EventIndices getEvents(ConstAggregateDataPtr __restrict place) const
    {
        EventIndices res;
        const auto & data_ref = this->data(place);

        const auto events_begin = std::begin(data_ref.events_list);
        const auto events_end = std::end(data_ref.events_list);
        auto events_it = events_begin;

        res = this->backtrackingMatchEventsLast(events_it, events_end);

        return res;
    }
};

template <typename T, typename Data>
class AggregateFunctionSequenceMatchEventsAll final : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsAll<T, Data>>
{
public:
    AggregateFunctionSequenceMatchEventsAll(const DataTypes & arguments, const Array & params, const String & pattern_)
        : AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsAll<T, Data>>(
            arguments, params, pattern_,
            std::make_shared<DataTypeArray>(std::make_shared<DataTypeArray>(arguments[0]))) {}

    using AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceMatchEventsAll<T, Data>>::AggregateFunctionSequenceBase;

    String getName() const override { return "sequenceMatchEventsAll"; }

    bool allocatesMemoryInArena() const override { return false; }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        this->data(place).sort();
        auto all_matches = getAllEvents(place);

        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = arr_to.getOffsets();

        ColumnArray & nested_arr = assert_cast<ColumnArray &>(arr_to.getData());
        ColumnArray::Offsets & nested_offsets = nested_arr.getOffsets();
        typename ColumnVector<T>::Container & nested_data = assert_cast<ColumnVector<T> &>(nested_arr.getData()).getData();

        const auto & events = this->data(place).events_list;
        for (const auto & match : all_matches)
        {
            for (auto index : match)
                nested_data.push_back(events[index].first);
            nested_offsets.push_back(nested_offsets.back() + match.size());
        }

        offsets_to.push_back(offsets_to.back() + all_matches.size());
    }

private:
    std::vector<EventIndices> getAllEvents(ConstAggregateDataPtr __restrict place) const
    {
        const auto & data_ref = this->data(place);

        const auto events_begin = std::begin(data_ref.events_list);
        const auto events_end = std::end(data_ref.events_list);
        auto events_it = events_begin;

        return this->backtrackingMatchEventsAll(events_it, events_end);
    }
};

template <typename T, typename Data>
class AggregateFunctionSequenceCount final : public AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceCount<T, Data>>
{
public:
    AggregateFunctionSequenceCount(const DataTypes & arguments, const Array & params, const String & pattern_)
        : AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceCount<T, Data>>(arguments, params, pattern_, std::make_shared<DataTypeUInt64>()) {}

    using AggregateFunctionSequenceBase<T, Data, AggregateFunctionSequenceCount<T, Data>>::AggregateFunctionSequenceBase;

    String getName() const override { return "sequenceCount"; }

    bool allocatesMemoryInArena() const override { return false; }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        auto & output = assert_cast<ColumnUInt64 &>(to).getData();
        if ((this->conditions_in_pattern & this->data(place).conditions_met) != this->conditions_in_pattern)
        {
            output.push_back(0);
            return;
        }
        this->data(place).sort();
        output.push_back(count(place));
    }

private:
    UInt64 count(ConstAggregateDataPtr __restrict place) const
    {
        const auto & data_ref = this->data(place);

        const auto events_begin = std::begin(data_ref.events_list);
        const auto events_end = std::end(data_ref.events_list);
        auto events_it = events_begin;

        size_t count = 0;
        // check if there is a chance of matching the sequence at least once
        if (this->couldMatchDeterministicParts(events_begin, events_end))
        {
            /// One context for all the calls, so the memo of failed states is shared by them.
            auto context = this->makeBacktrackingContext(static_cast<size_t>(events_end - events_begin));
            while (events_it != events_end && this->backtrackingMatch(events_it, events_end, nullptr, &context))
                ++count;
        }

        return count;
    }
};


template <template <typename, typename> typename AggregateFunction, template <typename> typename Data>
AggregateFunctionPtr createAggregateFunctionSequenceBase(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *)
{
    if (params.size() != 1)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Aggregate function {} requires exactly one parameter.",
            name);

    const auto arg_count = argument_types.size();

    if (arg_count < 3)
        throw Exception(ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION, "Aggregate function {} requires at least 3 arguments.",
            name);

    if (arg_count - 1 > max_events)
        throw Exception(ErrorCodes::TOO_MANY_ARGUMENTS_FOR_FUNCTION, "Aggregate function {} supports up to {} event arguments.", name, max_events);

    const auto * time_arg = argument_types.front().get();

    for (const auto i : collections::range(1, arg_count))
    {
        const auto * cond_arg = argument_types[i].get();
        if (!isUInt8(cond_arg))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "Illegal type {} of argument {} of aggregate function {}, must be UInt8",
                            cond_arg->getName(), toString(i + 1), name);
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
                    "Illegal type {} of first argument of aggregate function {}, must be DateTime",
                    time_arg->getName(), name);
}

}

void registerAggregateFunctionsSequenceMatchWithCapture(AggregateFunctionFactory & factory);

void registerAggregateFunctionsSequenceMatch(AggregateFunctionFactory & factory)
{
    factory.registerFunction("sequenceMatch", createAggregateFunctionSequenceBase<AggregateFunctionSequenceMatch, AggregateFunctionSequenceMatchData>);
    factory.registerFunction("sequenceCount", createAggregateFunctionSequenceBase<AggregateFunctionSequenceCount, AggregateFunctionSequenceMatchData>);
    factory.registerFunction("sequenceMatchEvents", createAggregateFunctionSequenceBase<AggregateFunctionSequenceMatchEvents, AggregateFunctionSequenceMatchData>);
    factory.registerFunction("sequenceMatchEventsFirst", createAggregateFunctionSequenceBase<AggregateFunctionSequenceMatchEventsFirst, AggregateFunctionSequenceMatchData>);
    factory.registerFunction("sequenceMatchEventsLast", createAggregateFunctionSequenceBase<AggregateFunctionSequenceMatchEventsLast, AggregateFunctionSequenceMatchData>);
    factory.registerFunction("sequenceMatchEventsAll", createAggregateFunctionSequenceBase<AggregateFunctionSequenceMatchEventsAll, AggregateFunctionSequenceMatchData>);
    registerAggregateFunctionsSequenceMatchWithCapture(factory);
}

}
