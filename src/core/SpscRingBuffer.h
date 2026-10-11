#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Single-producer, single-consumer queue. Storage is allocated in the constructor.
// push, pop, discard, and available() do not allocate or lock.
template <typename T>
class SpscRingBuffer
{
public:
    explicit SpscRingBuffer(int minCapacity)
        : capacity(nextPowerOfTwo(std::max(2, minCapacity))),
          mask(static_cast<uint32_t>(capacity - 1)),
          data(static_cast<size_t>(capacity))
    {
    }

    SpscRingBuffer() : SpscRingBuffer(1024) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    int getCapacity() const noexcept { return capacity; }

    // Snapshot. Slightly stale across threads, never negative.
    int available() const noexcept
    {
        const auto w = writeIndex.load(std::memory_order_acquire);
        const auto r = readIndex.load(std::memory_order_relaxed);
        return static_cast<int>(w - r);
    }

    int freeSpace() const noexcept { return capacity - available(); }

    int push(const T* src, int count) noexcept
    {
        if (count <= 0 || src == nullptr)
            return 0;

        const auto w = writeIndex.load(std::memory_order_relaxed);
        const auto r = readIndex.load(std::memory_order_acquire);
        const int space = capacity - static_cast<int>(w - r);
        const int n = std::min(count, std::max(0, space));
        if (n == 0)
            return 0;

        const int index = static_cast<int>(w & mask);
        const int first = std::min(n, capacity - index);
        std::memcpy(data.data() + index, src, sizeof(T) * static_cast<size_t>(first));
        if (n > first)
            std::memcpy(data.data(), src + first, sizeof(T) * static_cast<size_t>(n - first));

        writeIndex.store(w + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    int pop(T* dst, int count) noexcept
    {
        if (count <= 0 || dst == nullptr)
            return 0;

        const auto r = readIndex.load(std::memory_order_relaxed);
        const auto w = writeIndex.load(std::memory_order_acquire);
        const int have = static_cast<int>(w - r);
        const int n = std::min(count, std::max(0, have));
        if (n == 0)
            return 0;

        const int index = static_cast<int>(r & mask);
        const int first = std::min(n, capacity - index);
        std::memcpy(dst, data.data() + index, sizeof(T) * static_cast<size_t>(first));
        if (n > first)
            std::memcpy(dst + first, data.data(), sizeof(T) * static_cast<size_t>(n - first));

        readIndex.store(r + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    int discard(int count) noexcept
    {
        if (count <= 0)
            return 0;

        const auto r = readIndex.load(std::memory_order_relaxed);
        const auto w = writeIndex.load(std::memory_order_acquire);
        const int have = static_cast<int>(w - r);
        const int n = std::min(count, std::max(0, have));
        readIndex.store(r + static_cast<uint32_t>(n), std::memory_order_release);
        return n;
    }

    // Only when both threads are idle.
    void reset() noexcept
    {
        writeIndex.store(0, std::memory_order_relaxed);
        readIndex.store(0, std::memory_order_relaxed);
    }

private:
    static int nextPowerOfTwo(int n) noexcept
    {
        int p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    int capacity = 0;
    uint32_t mask = 0;
    std::vector<T> data;
    std::atomic<uint32_t> writeIndex { 0 };
    std::atomic<uint32_t> readIndex { 0 };
};
