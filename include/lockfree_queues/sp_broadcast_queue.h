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
 * All consumers will see the same message.
 * When the queue gets full the producer will wait on the slowest consumer
 * This queue can also be used as single-producer single-consumer queue
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

    for (size_t i = 0; i < MAX_READERS; ++i)
    {
      _reader_indexes[i].reset();
    }

    // we add some padding to the start and end of the buffer to protect it from false sharing
    // --- padding --- | --- slots* ---- | --- padding --- |
    _buffer = std::allocator_traits<Allocator>::allocate(_allocator, _capacity + (2u * PADDING));
    _slots = _buffer + PADDING;
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
  void emplace(Args&&... args)
  {
    size_t const write_idx = _write_idx.load(std::memory_order_relaxed);

    while ((_min_read_idx_cache == std::numeric_limits<size_t>::max()) ||
           ((write_idx - _min_read_idx_cache) == _capacity))
    {
      _min_read_idx_cache = _reader_indexes[0].read_idx.load(std::memory_order_acquire);
      for (size_t i = 1; i < _reader_indexes.size(); ++i)
      {
        _min_read_idx_cache =
          std::min(_min_read_idx_cache, _reader_indexes[i].read_idx.load(std::memory_order_acquire));
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
  }

  template <typename... Args>
  [[nodiscard]] bool try_emplace(Args&&... args)
  {
    size_t const write_idx = _write_idx.load(std::memory_order_relaxed);

    if ((_min_read_idx_cache == std::numeric_limits<size_t>::max()) ||
        ((write_idx - _min_read_idx_cache) == _capacity))
    {
      _min_read_idx_cache = _reader_indexes[0].read_idx.load(std::memory_order_acquire);
      for (size_t i = 1; i < _reader_indexes.size(); ++i)
      {
        _min_read_idx_cache =
          std::min(_min_read_idx_cache, _reader_indexes[i].read_idx.load(std::memory_order_acquire));
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

  [[nodiscard]] value_type const* front(size_t reader_id) noexcept
  {
    if (_reader_indexes[reader_id].read_local_idx == _reader_indexes[reader_id].write_idx_cache)
    {
      _reader_indexes[reader_id].write_idx_cache = _write_idx.load(std::memory_order_acquire);
      if (_reader_indexes[reader_id].read_local_idx == _reader_indexes[reader_id].write_idx_cache)
      {
        return nullptr;
      }
    }

    return reinterpret_cast<value_type const*>(
      &_slots[_reader_indexes[reader_id].read_local_idx & _capacity_minus_one]);
  }

  void pop(size_t reader_id) noexcept
  {
    ++_reader_indexes[reader_id].read_local_idx;

    if ((_reader_indexes[reader_id].read_local_idx & _items_per_batch_minus_one) == 0)
    {
      _reader_indexes[reader_id].read_idx.store(_reader_indexes[reader_id].read_local_idx, std::memory_order_release);
    }
  }

  [[nodiscard]] size_t capacity() const noexcept { return _capacity; }

  [[nodiscard]] size_t subscribe()
  {
    while (_subscribe_lock.exchange(true))
    {
      // wait for the lock
    }

    auto search_it = std::find_if(std::begin(_reader_indexes), std::end(_reader_indexes),
                                  [](ReaderIndexes const& ri) {
                                    return ri.write_idx_cache == std::numeric_limits<size_t>::max();
                                  });

    if (search_it == std::end(_reader_indexes))
    {
      _subscribe_lock.store(false);
      throw std::runtime_error{"Max consumers reached"};
    }

    size_t const index = std::distance(std::begin(_reader_indexes), search_it);
    size_t const write_idx = _write_idx.load(std::memory_order_acquire);
    size_t const last_write_idx = (write_idx == 0) ? 0 : write_idx - 1;

    search_it->set(last_write_idx);

    _subscribe_lock.store(false);
    return index;
  }

  void unsubscribe(size_t reader_id) noexcept
  {
    while (_subscribe_lock.exchange(true))
    {
      // wait for the lock
    }
    _reader_indexes[reader_id].reset();
    _subscribe_lock.store(false);
  }

private:
  static_assert(std::is_nothrow_destructible<value_type>::value, "T must be nothrow destructible");
  static_assert(MAX_READERS != 0, "MAX_READERS can not be zero");

  static constexpr size_t CACHE_LINE_SIZE{128u};

  static constexpr size_t PADDING =
    (CACHE_LINE_SIZE - 1) / sizeof(value_type) + 1; /** How many T can we fit in a cache line **/

  struct ReaderIndexes
  {
    void set(size_t v) noexcept
    {
      read_local_idx = v;
      write_idx_cache = v;
      read_idx.store(v, std::memory_order_release);
    }

    void reset() noexcept { set(std::numeric_limits<size_t>::max()); }

    /** Members **/
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_idx{std::numeric_limits<size_t>::max()};
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
  alignas(CACHE_LINE_SIZE) std::array<ReaderIndexes, MAX_READERS> _reader_indexes;
};
} // namespace lockfree_queues