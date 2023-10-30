#pragma once

#include <cstddef>

namespace lockfree_queues
{
[[nodiscard]] constexpr size_t next_power_of_two(size_t v) noexcept
{
  size_t power = 1;
  while (power < v)
  {
    power = power * 2;
  }
  return power;
}

[[nodiscard]] constexpr bool is_power_of_two(size_t n) noexcept
{
  return (n != 0) && ((n & (n - 1)) == 0);
}
} // namespace lockfree_queues