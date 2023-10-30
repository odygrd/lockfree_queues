#include "lockfree_queues/sp_broadcast_queue.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

struct TestObj
{
  size_t x;
  size_t y;
};

int main()
{
  size_t const queue_size = 65536;
  size_t const reader_batch_size = 4;
  int64_t const iterations = 10000000;

  constexpr size_t MAX_READERS = 3;

  lockfree_queues::SPBroadcastQueue<TestObj, MAX_READERS> q{queue_size, reader_batch_size};

  std::vector<std::thread> reader_threads;
  std::vector<size_t> total_objects(MAX_READERS, 0);

  for (size_t tid = 0; tid < MAX_READERS; ++tid)
  {
    reader_threads.emplace_back(
      [&q, &total_objects, tid, iterations]
      {
        size_t cid = q.subscribe();

        size_t n = 0;
        while (n < (iterations - 1))
        {
          TestObj const* item = q.front(cid);
          while (!item)
          {
            item = q.front(cid);
          }

          total_objects[tid] += item->y;
          n = item->x;

          q.pop(cid);
        }
      });
  }

  auto start = std::chrono::steady_clock::now();

  for (size_t i = 0; i < iterations; ++i)
  {
    while (!q.try_emplace(i, 1u))
      ;
  }

  for (auto& rt : reader_threads)
  {
    rt.join();
  }

  auto stop = std::chrono::steady_clock::now();
  std::cout << iterations * 1000000 /
      std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
            << " ops/ms, total_duration: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count() << " ms";
}