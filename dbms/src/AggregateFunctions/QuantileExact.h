#pragma once

#include <Common/PODArray.h>
#include <Common/NaNUtils.h>
#include <Core/Types.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadBuffer.h>
#include <IO/VarInt.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

/** Calculates quantile by collecting all values into array
  *  and applying n-th element (introselect) algorithm for the resulting array.
  *
  * It uses O(N) memory and it is very inefficient in case of high amount of identical values.
  * But it is very CPU efficient for not large datasets.
  */
template <typename Value>
struct QuantileExact
{
    /// The memory will be allocated to several elements at once, so that the state occupies 64 bytes.
    static constexpr size_t bytes_in_arena = 64 - sizeof(PODArray<Value>);
    using Array = PODArrayWithStackMemory<Value, bytes_in_arena>;
    Array array;

    void add(const Value & x)
    {
        /// We must skip NaNs as they are not compatible with comparison sorting.
        if (!isNaN(x))
            array.push_back(x);
    }

    template <typename Weight>
    void add(const Value &, const Weight &)
    {
        throw Exception("Method add with weight is not implemented for QuantileExact", ErrorCodes::NOT_IMPLEMENTED);
    }

    void merge(const QuantileExact & rhs)
    {
        array.insert(rhs.array.begin(), rhs.array.end());
    }

    void serialize(WriteBuffer & buf) const
    {
        size_t size = array.size();
        writeVarUInt(size, buf);
        buf.write(reinterpret_cast<const char *>(array.data()), size * sizeof(array[0]));
    }

    void deserialize(ReadBuffer & buf)
    {
        size_t size = 0;
        readVarUInt(size, buf);
        array.resize(size);
        buf.read(reinterpret_cast<char *>(array.data()), size * sizeof(array[0]));
    }

    /// Get the value of the `level` quantile. The level must be between 0 and 1.
    virtual Value get(Float64 level)
    {
        if (!array.empty())
        {
            size_t n = level < 1
                ? level * array.size()
                : (array.size() - 1);

            std::nth_element(array.begin(), array.begin() + n, array.end());    /// NOTE You can think of the radix-select algorithm.
            return array[n];
        }

        return std::numeric_limits<Value>::quiet_NaN();
    }

    /// Get the `size` values of `levels` quantiles. Write `size` results starting with `result` address.
    /// indices - an array of index levels such that the corresponding elements will go in ascending order.
    virtual void getMany(const Float64 * levels, const size_t * indices, size_t size, Value * result)
    {
        if (!array.empty())
        {
            size_t prev_n = 0;
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];

                size_t n = level < 1
                    ? level * array.size()
                    : (array.size() - 1);

                std::nth_element(array.begin() + prev_n, array.begin() + n, array.end());

                result[indices[i]] = array[n];
                prev_n = n;
            }
        }
        else
        {
            for (size_t i = 0; i < size; ++i)
                result[i] = Value();
        }
    }

    /// The same, but in the case of an empty state, NaN is returned.
    virtual Float64 getFloat(Float64)
    {
        throw Exception("Method getFloat is not implemented for QuantileExact", ErrorCodes::NOT_IMPLEMENTED);
    }

    virtual void getManyFloat(const Float64 *, const size_t *, size_t, Float64 *)
    {
        throw Exception("Method getManyFloat is not implemented for QuantileExact", ErrorCodes::NOT_IMPLEMENTED);
    }

    virtual ~QuantileExact() = default;
};

template <typename Value>
struct QuantileExactExclusive : public QuantileExact<Value>
{
    using QuantileExact<Value>::array;
    /// Get the value of the `level` quantile. The level must be between 0 and 1.
    Float64 getFloat(Float64 level) override
    {
        if (!array.empty())
        {
            Float64 h = level * (array.size() + 1);
            auto n = static_cast<size_t>(h);

            if (n >= array.size())
                return array[array.size() - 1];
            else if (n < 1)
                return array[0];

            std::nth_element(array.begin(), array.begin() + n - 1, array.end());
            std::nth_element(array.begin() + n, array.begin() + n, array.end());

            return array[n - 1] + (h - n) * (array[n] - array[n - 1]);
        }

        return std::numeric_limits<Float64>::quiet_NaN();
    }

    void getManyFloat(const Float64 * levels, const size_t * indices, size_t size, Float64 * result) override
    {
        if (!array.empty())
        {
            size_t prev_n = 0;
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];

                Float64 h = level * (array.size() + 1);
                auto n = static_cast<size_t>(h);

                if (n >= array.size())
                    result[indices[i]] = array[array.size() - 1];
                else if (n < 1)
                    result[indices[i]] = array[0];
                else
                {
                    std::nth_element(array.begin() + prev_n, array.begin() + n - 1, array.end());
                    std::nth_element(array.begin() + n, array.begin() + n, array.end());

                    result[indices[i]] = array[n - 1] + (h - n) * (array[n] - array[n - 1]);
                    prev_n = n;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < size; ++i)
                result[i] = std::numeric_limits<Float64>::quiet_NaN();
        }
    }
};

}
