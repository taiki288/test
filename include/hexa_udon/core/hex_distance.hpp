#pragma once

#include "hexa_udon/core/map_definition.hpp"

#include <cstdint>
#include <cstdlib>

namespace hexa_udon::core {

// Distance for the even-row-shifted offset coordinates used by MapDefinition.
// Convert odd-r offset coordinates to axial coordinates before measuring the
// cube distance. Keeping this in one place prevents planner tie-breaks from
// using different interpretations of the map topology.
[[nodiscard]] inline std::int64_t hex_distance(
    const MapDefinition& map, const CellIndex left, const CellIndex right) noexcept
{
    const auto left_coordinate = map.coordinate(left);
    const auto right_coordinate = map.coordinate(right);
    if (!left_coordinate || !right_coordinate) return 0;

    const auto axial_q = [](const HexCoord coordinate) {
        return static_cast<std::int64_t>(coordinate.col)
            - static_cast<std::int64_t>((coordinate.row + (coordinate.row & 1)) / 2);
    };
    const auto axial_r = [](const HexCoord coordinate) {
        return static_cast<std::int64_t>(coordinate.row);
    };

    const auto left_q = axial_q(*left_coordinate);
    const auto left_r = axial_r(*left_coordinate);
    const auto right_q = axial_q(*right_coordinate);
    const auto right_r = axial_r(*right_coordinate);
    const auto left_x = left_q;
    const auto left_z = left_r;
    const auto left_y = -left_x - left_z;
    const auto right_x = right_q;
    const auto right_z = right_r;
    const auto right_y = -right_x - right_z;
    return (std::llabs(left_x - right_x) + std::llabs(left_y - right_y)
        + std::llabs(left_z - right_z)) / 2;
}

}  // namespace hexa_udon::core
