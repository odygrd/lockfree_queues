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

  constexpr size_t MAX_READERS = 1;

  std::vector<size_t> total_objects(MAX_READERS, 0);

  lockfree_queues::SPBroadcastQueue<TestObj, MAX_READERS> q1{queue_size, reader_batch_size};
  lockfree_queues::SPBroadcastQueue<TestObj, MAX_READERS> q2{queue_size, reader_batch_size};

  auto t = std::thread(
    [&q1, &q2, iterations]
    {
      auto q1_cid = q1.subscribe();

      for (int i = 0; i < iterations; ++i)
      {
        while (!q1.front(q1_cid))
          ;

        while (!q2.try_emplace(*q1.front(q1_cid)))
          ;
        q1.pop(q1_cid);
      }
    });

  auto q2_cid = q2.subscribe();

  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i)
  {
    while (!q1.try_emplace(i, 1u))
      ;

    while (!q2.front(q2_cid))
      ;
    q2.pop(q2_cid);
  }
  auto stop = std::chrono::steady_clock::now();

  t.join();
  std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count() / iterations << " ns RTT";
}