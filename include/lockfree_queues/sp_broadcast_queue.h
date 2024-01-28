#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "lockfree_queues/utilities.h"

namespace lockfree_queues
{

/***
 * A bounded single-producer multiple-consumer queue.
 * The queue can function as both a Single-Producer, Single-Consumer (SPSC) queue and
 * a Single-Producer, Multiple-Consumer (SPMC) queue, depending on the template arguments provided.
 *
 * It can handle both simple and more complex data types, ensuring that the producer takes care of
 * creating and destroying objects in the queue.
 * The producer synchronizes with consumers and waits for the slowest one when the queue is full.
 * To use this queue, consumers need to first "subscribe()" to it, and then they can start
 * consuming messages.
 * Importantly, all consumers will see all the messages in the queue.
 *
 * In addition, special attention has been given to optimizing the queue to avoid performance
 * bottlenecks, such as false sharing and cache issues. This optimization leads to increased
 * throughput, especially when the number of consumers grows.
 *
 * Both the producer and the consumer will cache the indexes locally, loading them only when they
 * can't produce or consume anymore.
 * Moreover, consumers will update their index after consuming multiple messages,
 * rather than updating it each time. By default, the queue is split into four batches
 *
 *
 * @tparam T Type of th element
 * @tparam MAX_READERS Max consumers that can subscribe to this queue
 * @tparam Allocator An allocator used to allocate memory
 */
template <typename T, size_t MAX_READERS = 1, typename Allocator = std::allocator<T>>
class SPBroadcastQueue
{
public:
  using value_type = T;

  /**
   * Constructor
   * @param capacity Max element capacity
   * @param reader_batch_size Readers commit their reads to the producer in batches to increase throughput
   * @param allocator memory allocator
   */
  explicit SPBroadcastQueue(size_t capacity, size_t reader_batch_size = 4,
                            Allocator const& allocator = Allocator())
    : _capacity(std::max(size_t{16}, next_power_of_two(capacity))),
      _capacity_minus_one(_capacity - 1),
      _items_per_batch_minus_one((_capacity / reader_batch_size) - 1),
      _allocator(allocator)
  {
    if (!is_power_of_two(_items_per_batch_minus_one + 1))
    {
      throw std::runtime_error{"items per batch must be power of 2"};
    }

    // we add some padding to the start and end of the buffer to protect it from false sharing
    // --- padding --- | --- slots* ---- | --- padding --- |
    _buffer = std::allocator_traits<Allocator>::allocate(_allocator, _capacity + (2u * PADDING));
    _slots = _buffer + PADDING;

    for (size_t i = 0; i < MAX_READERS; ++i)
    {
      _reader_cache[i].reset();
    }

    for (size_t i = 0; i < MAX_READERS; ++i)
    {
      _read_idx[i].store(std::numeric_limits<size_t>::max());
    }

    _write_idx.store(0);
    _subscribe_lock.store(false);
  }

  /**
   * Destructor is expected to run by the writer
   */
  ~SPBroadcastQueue()
  {
    size_t const write_idx = _write_idx.load(std::memory_order_relaxed);
    size_t const n = (write_idx >= _capacity) ? _capacity : write_idx;

    for (size_t i = 0; i < n; ++i)
    {
      _slots[i].~T();
    }

    std::allocator_traits<Allocator>::deallocate(_allocator, _buffer, _capacity + (2u * PADDING));
  }

  /** Deleted **/
  SPBroadcastQueue(SPBroadcastQueue const&) = delete;
  SPBroadcastQueue& operator=(SPBroadcastQueue const&) = delete;

  template <typename... Args>
  [[gnu::always_inline, gnu::hot]] void emplace(Args&&... args)
  {
    while (!try_emplace(std::forward<Args>(args)...))
    {
      // retry
    }
  }

  template <typename... Args>
  [[gnu::always_inline, gnu::hot, nodiscard]] bool try_emplace(Args&&... args)
  {
    size_t const write_idx = _write_idx.load(std::memory_order_relaxed);

    if ((_min_read_idx_cache == std::numeric_limits<size_t>::max()) ||
        ((write_idx - _min_read_idx_cache) == _capacity))
    {
      _min_read_idx_cache = _read_idx[0].load(std::memory_order_acquire);

      if constexpr (MAX_READERS > 1)
      {
        // Find the min read_idx if more than one reader
        for (size_t i = 1; i < _read_idx.size(); ++i)
        {
          _min_read_idx_cache = std::min(_min_read_idx_cache, _read_idx[i].load(std::memory_order_acquire));
        }
      }

      if ((_min_read_idx_cache == std::numeric_limits<size_t>::max()) ||
          ((write_idx - _min_read_idx_cache) == _capacity))
      {
        return false;
      }
    }

    value_type* slot = &_slots[write_idx & _capacity_minus_one];

    if constexpr (!std::is_trivially_destructible_v<value_type>)
    {
      if (_write_idx >= _capacity)
      {
        // do not call the destructor until we have wrapped around at least once
        slot->~value_type();
      }
    }

    ::new (static_cast<void*>(slot)) value_type{std::forward<Args>(args)...};
    _write_idx.store(write_idx + 1, std::memory_order_release);

    return true;
  }

  [[gnu::always_inline, gnu::hot, nodiscard]] value_type const* front(size_t reader_id) noexcept
  {
    if (_reader_cache[reader_id].read_local_idx == _reader_cache[reader_id].write_idx_cache)
    {
      _reader_cache[reader_id].write_idx_cache = _write_idx.load(std::memory_order_acquire);
      if (_reader_cache[reader_id].read_local_idx == _reader_cache[reader_id].write_idx_cache)
      {
        return nullptr;
      }
    }

    return reinterpret_cast<value_type const*>(&_slots[_reader_cache[reader_id].read_local_idx & _capacity_minus_one]);
  }

  [[gnu::always_inline, gnu::hot]] void pop(size_t reader_id) noexcept
  {
    _reader_cache[reader_id].read_local_idx += 1;

    if ((_reader_cache[reader_id].read_local_idx & _items_per_batch_minus_one) == 0)
    {
      _read_idx[reader_id].store(_reader_cache[reader_id].read_local_idx, std::memory_order_release);
    }
  }

  [[nodiscard]] size_t capacity() const noexcept { return _capacity; }

  [[nodiscard]] size_t subscribe()
  {
    while (_subscribe_lock.exchange(true))
    {
      // wait for the lock
    }

    auto search_it = std::find_if(std::begin(_read_idx), std::end(_read_idx),
                                  [](auto const& reader_idx)
                                  {
                                    return reader_idx.load(std::memory_order_acquire) ==
                                      std::numeric_limits<size_t>::max();
                                  });

    if (search_it == std::end(_read_idx))
    {
      _subscribe_lock.store(false);
      throw std::runtime_error{"Max consumers reached"};
    }

    size_t const index = std::distance(std::begin(_read_idx), search_it);
    size_t const write_idx = _write_idx.load(std::memory_order_acquire);
    size_t const last_write_idx = (write_idx == 0) ? 0 : write_idx - 1;

    _reader_cache[index].set(last_write_idx);
    _read_idx[index].store(last_write_idx, std::memory_order_release);
    _subscribe_lock.store(false);
    return index;
  }

  void unsubscribe(size_t reader_id) noexcept
  {
    while (_subscribe_lock.exchange(true))
    {
      // wait for the lock
    }

    _reader_cache[reader_id].reset();
    _read_idx[reader_id].store(std::numeric_limits<size_t>::max(), std::memory_order_release);
    _subscribe_lock.store(false);
  }

private:
  static_assert(std::is_nothrow_destructible<value_type>::value, "T must be nothrow destructible");
  static_assert(MAX_READERS != 0, "MAX_READERS can not be zero");

  static constexpr size_t CACHE_LINE_SIZE{128u};

  static constexpr size_t PADDING =
    (CACHE_LINE_SIZE - 1) / sizeof(value_type) + 1; /** How many T can we fit in a cache line **/

  struct ReaderCache
  {
    void set(size_t v) noexcept
    {
      read_local_idx = v;
      write_idx_cache = v;
    }

    void reset() noexcept { set(std::numeric_limits<size_t>::max()); }

    alignas(CACHE_LINE_SIZE) size_t read_local_idx{std::numeric_limits<size_t>::max()};
    size_t write_idx_cache{std::numeric_limits<size_t>::max()};
  };

private:
  /** Members **/
  size_t _capacity;
  size_t _capacity_minus_one;
  size_t _items_per_batch_minus_one;
  value_type* _slots = nullptr;
  value_type* _buffer = nullptr;
  std::atomic<bool> _subscribe_lock;
  Allocator _allocator;

  alignas(CACHE_LINE_SIZE) std::atomic<size_t> _write_idx = {0};
  alignas(CACHE_LINE_SIZE) size_t _min_read_idx_cache = std::numeric_limits<size_t>::max();
  alignas(CACHE_LINE_SIZE) std::array<std::atomic<size_t>, MAX_READERS> _read_idx;
  alignas(CACHE_LINE_SIZE) std::array<ReaderCache, MAX_READERS> _reader_cache;
};
} // namespace lockfree_queues