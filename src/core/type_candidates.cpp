#include "hexa_udon/core/type_candidates.hpp"
#include <bit>
#include <cstdint>
namespace hexa_udon::core {
std::vector<std::vector<AgentKind>> complete_type_arrays(
    const std::size_t agents, const std::size_t minimum_supply, const std::size_t maximum_supply) {
    if (agents == 0 || agents > 7 || minimum_supply > maximum_supply || maximum_supply > agents)
        return {};
    std::vector<std::vector<AgentKind>> arrays;
    const auto limit = std::uint32_t{1} << agents;
    for (std::uint32_t bits = 0; bits < limit; ++bits) {
        const auto supply = static_cast<std::size_t>(std::popcount(bits));
        if (supply < minimum_supply || supply > maximum_supply) continue;
        std::vector<AgentKind> kinds;
        kinds.reserve(agents);
        for (std::size_t index = 0; index < agents; ++index)
            kinds.push_back((bits & (std::uint32_t{1} << (agents - index - 1))) != 0
                ? AgentKind::Supply : AgentKind::Patrol);
        arrays.push_back(std::move(kinds));
    }
    return arrays;
}
}
