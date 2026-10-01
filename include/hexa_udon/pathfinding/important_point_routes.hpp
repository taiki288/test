#pragma once

#include "hexa_udon/pathfinding/pathfinder.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <vector>

namespace hexa_udon::pathfinding {

struct ImportantPointSet {
    std::vector<core::CellIndex> unique_cells;
    std::vector<std::size_t> original_to_unique;
};

class ImportantPointRoutes {
public:
    [[nodiscard]] static PathfindingResult<ImportantPointSet> collect_points(
        const core::MapDefinition& map, std::span<const core::CellIndex> points);

    [[nodiscard]] PathfindingResult<std::size_t> precompute(
        const Pathfinder& pathfinder,
        std::span<const core::CellIndex> points,
        std::span<const RouteObjective> objectives);

    [[nodiscard]] PathfindingResult<std::optional<Route>> route(
        const Pathfinder& pathfinder,
        core::CellIndex source,
        core::CellIndex destination,
        RouteObjective objective);

    void clear() noexcept;
    [[nodiscard]] std::size_t tree_count() const noexcept { return trees_.size(); }
    [[nodiscard]] std::size_t tree_build_count() const noexcept { return tree_build_count_; }
    [[nodiscard]] std::size_t stored_label_count() const noexcept;

private:
    struct Key {
        std::string snapshot;
        RouteObjective objective;
        std::int32_t source;

        [[nodiscard]] bool operator<(const Key& other) const noexcept;
    };

    std::map<Key, ShortestPathTree> trees_;
    std::size_t tree_build_count_ = 0;
};

}  // namespace hexa_udon::pathfinding
