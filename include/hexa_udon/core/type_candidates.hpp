#pragma once
#include "hexa_udon/core/types.hpp"
#include <cstddef>
#include <vector>
namespace hexa_udon::core {
// Bounded structural enumeration, independent of strategy and communication.
// Invalid limits return empty. Complete arrays use official index order.
[[nodiscard]] std::vector<std::vector<AgentKind>> complete_type_arrays(
    std::size_t agents, std::size_t minimum_supply, std::size_t maximum_supply);
}
