#include "doctest/doctest.h"

#include "lockfree_queues/sp_broadcast_queue.h"

#include <array>
#include <memory>
#include <set>
#include <thread>
#include <vector>

TEST_SUITE_BEGIN("SPBroadcastQueue");

using namespace lockfree_queues;

// TestType tracks correct usage of constructors and destructors
struct SPBQTestType
{
  static std::set<SPBQTestType const*> constructed;

  SPBQTestType(uint32_t v) noexcept
  {
    x = v;
    REQUIRE_EQ(constructed.count(this), 0);
    constructed.insert(this);
  };

  SPBQTestType() noexcept
  {
    REQUIRE_EQ(constructed.count(this), 0);
    constructed.insert(this);
  };

  SPBQTestType(const SPBQTestType& other) noexcept
  {
    REQUIRE_EQ(constructed.count(this), 0);
    REQUIRE_EQ(constructed.count(&other), 1);
    constructed.insert(this);
  };

  SPBQTestType(SPBQTestType&& other) noexcept
  {
    REQUIRE_EQ(constructed.count(this), 0);
    REQUIRE_EQ(constructed.count(&other), 1);
    constructed.insert(this);
  };

  SPBQTestType& operator=(const SPBQTestType& other) noexcept
  {
    REQUIRE_EQ(constructed.count(this), 1);
    REQUIRE_EQ(constructed.count(&other), 1);
    return *this;
  };

  SPBQTestType& operator=(SPBQTestType&& other) noexcept
  {
    REQUIRE_EQ(constructed.count(this), 1);
    REQUIRE_EQ(constructed.count(&other), 1);
    return *this;
  }

  ~SPBQTestType() noexcept
  {
    REQUIRE_EQ(constructed.count(this), 1);
    constructed.erase(this);
  };

  uint32_t x;
};

std::set<const SPBQTestType*> SPBQTestType::constructed;

/***/
TEST_CASE("basic_produce_full_queue")
{
  {
    SPBroadcastQueue<SPBQTestType> q{16};
    size_t const rid = q.subscribe();

    REQUIRE_EQ(q.front(rid), nullptr);
    REQUIRE_EQ(q.capacity(), 16);

    for (size_t iter = 0; iter < 3; ++iter)
    {
      for (size_t i = 0; i < 16; i++)
      {
        REQUIRE_EQ(q.try_emplace(), true);
      }

      REQUIRE(q.front(rid));

      REQUIRE_EQ(SPBQTestType::constructed.size(), 16);
      REQUIRE_EQ(q.try_emplace(), false);

      for (size_t i = 0; i < 16; i++)
      {
        q.pop(rid);
      }
    }
  }
  REQUIRE_EQ(SPBQTestType::constructed.size(), 0);
}

/***/
TEST_CASE("basic_produce_partial_queue")
{
  {
    SPBroadcastQueue<SPBQTestType> q{16};
    size_t const rid = q.subscribe();

    REQUIRE_EQ(q.front(rid), nullptr);
    REQUIRE_EQ(q.capacity(), 16);

    for (size_t i = 0; i < 10; i++)
    {
      q.emplace();
    }

    REQUIRE(q.front(rid));

    REQUIRE_EQ(SPBQTestType::constructed.size(), 10);
  }
  REQUIRE_EQ(SPBQTestType::constructed.size(), 0);
}

/***/
TEST_CASE("over_subscribe")
{
  constexpr size_t MAX_CONSUMERS = 2;
  SPBroadcastQueue<size_t, MAX_CONSUMERS> q{10};
  size_t rid1 = q.subscribe();
  size_t rid2 = q.subscribe();
  REQUIRE_THROWS((void)q.subscribe());

  q.unsubscribe(rid1);
  REQUIRE_NOTHROW((void)q.subscribe());
}

/***/
TEST_CASE("single_produce_single_consumers")
{
  const size_t iter = 1'000'000;
  SPBroadcastQueue<size_t> q{1024};

  std::atomic<bool> flag(false);
  std::thread producer{[&q, &flag, iter]
                       {
                         while (!flag)
                           ;

                         for (size_t i = 0; i < iter; ++i)
                         {
                           q.emplace(i);
                         }
                       }};

  size_t rid = q.subscribe();
  size_t sum = 0;
  flag = true;
  for (size_t i = 0; i < iter; ++i)
  {
    while (!q.front(rid))
      ;
    sum += *q.front(rid);
    q.pop(rid);
  }

  REQUIRE_EQ(q.front(rid), nullptr);
  REQUIRE_EQ(sum, iter * (iter - 1) / 2);
  q.unsubscribe(rid);

  producer.join();
}

/***/
TEST_CASE("single_produce_multiple_consumers")
{
  // All Consumers subscribe before producer starts producing
  const size_t iter = 1'000'000;
  constexpr size_t MAX_CONSUMERS = 4;
  SPBroadcastQueue<size_t, MAX_CONSUMERS> q{1024};

  std::array<std::atomic<bool>, MAX_CONSUMERS> flags = {false};

  std::thread producer{[&q, &flags, iter]()
                       {
                         for (auto const& flag : flags)
                         {
                           while (!flag)
                             ;
                         }

                         for (size_t i = 0; i < iter; ++i)
                         {
                           q.emplace(i);
                         }
                       }};

  std::vector<std::thread> consumers;
  for (size_t tid = 0; tid < MAX_CONSUMERS; ++tid)
  {
    consumers.emplace_back(
      [&q, &flags, tid, iter]()
      {
        size_t rid = q.subscribe();
        size_t sum = 0;
        flags[tid] = true;

        for (size_t i = 0; i < iter; ++i)
        {
          while (!q.front(rid))
            ;
          sum += *q.front(rid);
          q.pop(rid);
        }

        REQUIRE_EQ(q.front(rid), nullptr);
        REQUIRE_EQ(sum, iter * (iter - 1) / 2);
        q.unsubscribe(rid);
      });
  }

  for (auto& c : consumers)
  {
    c.join();
  }
  producer.join();
}

/***/
TEST_CASE("single_produce_multiple_consumers_subscribe")
{
  // Consumers subscribe after the producer starts
  const size_t iter = 1'000'000;
  constexpr size_t MAX_CONSUMERS = 4;

  SPBroadcastQueue<SPBQTestType, MAX_CONSUMERS> q{1024};

  std::thread producer{[&q, iter]()
                       {
                         for (size_t i = 0; i < iter; ++i)
                         {
                           q.emplace(i);
                         }
                       }};

  // let the producer thread start,
  // the producer waits for at least one consumer before it is able to emplace
  std::this_thread::sleep_for(std::chrono::nanoseconds{200});

  std::vector<std::thread> consumers;
  for (size_t tid = 0; tid < MAX_CONSUMERS; ++tid)
  {
    consumers.emplace_back(
      [&q, iter]()
      {
        size_t rid = q.subscribe();

        while (true)
        {
          SPBQTestType const* item = q.front(rid);
          while (!item)
          {
            item = q.front(rid);
          }

          if (item->x == iter - 1)
          {
            // last item
            q.pop(rid);
            break;
          }

          q.pop(rid);
        }

        REQUIRE_EQ(q.front(rid), nullptr);
        q.unsubscribe(rid);
      });
  }

  for (auto& c : consumers)
  {
    c.join();
  }
  producer.join();
}
TEST_SUITE_END();