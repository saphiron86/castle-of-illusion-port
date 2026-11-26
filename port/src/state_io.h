// Save states (savestate.h): the byte writer and reader each part of the machine saves itself
// with. Values are written little-endian, field by field, so a state is the same on every
// platform the port builds for.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

class StateWriter {
public:
    std::vector<uint8_t> data;

    void bytes(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
    template <typename T>
    void put(T value)
    {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if constexpr (std::is_same_v<T, bool>) {
            data.push_back(value ? 1 : 0);
        } else {
            uint8_t b[sizeof(T)];
            std::memcpy(b, &value, sizeof(T));      // (every platform the port builds for is little-endian)
            bytes(b, sizeof(T));
        }
    }
    template <typename T, size_t N>
    void put_array(const T (&a)[N])
    {
        for (const T& v : a)
            put(v);
    }
};

class StateReader {
public:
    StateReader(const uint8_t* p, size_t n) : p(p), end(p + n) {}

    bool ok() const { return good; }
    bool done() const { return good && p == end; }
    size_t left() const { return size_t(end - p); }

    void bytes(void* out, size_t n)
    {
        if (!good || size_t(end - p) < n) {
            good = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, p, n);
        p += n;
    }
    template <typename T>
    T get()
    {
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t b = 0;
            bytes(&b, 1);
            return b != 0;
        } else {
            T value{};
            bytes(&value, sizeof(T));
            return value;
        }
    }
    template <typename T>
    void get(T& value) { value = get<T>(); }
    template <typename T, size_t N>
    void get_array(T (&a)[N])
    {
        for (T& v : a)
            v = get<T>();
    }

private:
    const uint8_t* p;
    const uint8_t* end;
    bool good = true;
};
