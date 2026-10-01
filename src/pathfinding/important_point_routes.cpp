#include "hexa_udon/pathfinding/important_point_routes.hpp"

#include <algorithm>
#include <tuple>

namespace hexa_udon::pathfinding {

bool ImportantPointRoutes::Key::operator<(const Key& other) const noexcept
{
    return std::tie(snapshot, objective, source)
        < std::tie(other.snapshot, other.objective, other.source);
}

PathfindingResult<ImportantPointSet> ImportantPointRoutes::collect_points(
    const core::MapDefinition& map, const std::span<const core::CellIndex> points)
{
    ImportantPointSet result;
    result.original_to_unique.reserve(points.size());
    for (const auto point : points) {
        if (!map.contains(point) || map.terrain_at(point) == core::Terrain::Pond) {
            return PathfindingResult<ImportantPointSet>::failure({
                PathfindingErrorCode::InvalidImportantPoint,
                "important point must be a traversable map cell",
                point,
            });
        }
        const auto found = std::find(result.unique_cells.begin(), result.unique_cells.end(), point);
        if (found == result.unique_cells.end()) {
            result.unique_cells.push_back(point);
            result.original_to_unique.push_back(result.unique_cells.size() - 1U);
        } else {
            result.original_to_unique.push_back(
                static_cast<std::size_t>(std::distance(result.unique_cells.begin(), found)));
        }
    }
    return PathfindingResult<ImportantPointSet>::success(std::move(result));
}

PathfindingResult<std::size_t> ImportantPointRoutes::precompute(
    const Pathfinder& pathfinder,
    const std::span<const core::CellIndex> points,
    const std::span<const RouteObjective> objectives)
{
    const auto collected = collect_points(pathfinder.map(), points);
    if (!collected) {
        return PathfindingResult<std::size_t>::failure(collected.error());
    }
    const auto before = tree_build_count_;
    for (const auto objective : objectives) {
        for (const auto source : collected.value().unique_cells) {
            const Key key{pathfinder.snapshot_key(), objective, source.value};
            if (trees_.find(key) != trees_.end()) {
                continue;
            }
            auto tree = pathfinder.shortest_path_tree(source, objective);
            if (!tree) {
                return PathfindingResult<std::size_t>::failure(tree.error());
            }
            trees_.emplace(key, std::move(tree).value());
            ++tree_build_count_;
        }
    }
    return PathfindingResult<std::size_t>::success(tree_build_count_ - before);
}

PathfindingResult<std::optional<Route>> ImportantPointRoutes::route(
    const Pathfinder& pathfinder,
    const core::CellIndex source,
    const core::CellIndex destination,
    const RouteObjective objective)
{
    const Key key{pathfinder.snapshot_key(), objective, source.value};
    auto found = trees_.find(key);
    if (found == trees_.end()) {
        auto tree = pathfinder.shortest_path_tree(source, objective);
        if (!tree) {
            return PathfindingResult<std::optional<Route>>::failure(tree.error());
        }
        found = trees_.emplace(key, std::move(tree).value()).first;
        ++tree_build_count_;
    }
    return found->second.route_to(destination);
}

void ImportantPointRoutes::clear() noexcept
{
    trees_.clear();
    tree_build_count_ = 0;
}

std::size_t ImportantPointRoutes::stored_label_count() const noexcept
{
    std::size_t result = 0;
    for (const auto& [key, tree] : trees_) {
        static_cast<void>(key);
        result += tree.stored_cell_count();
    }
    return result;
}

}  // namespace hexa_udon::pathfinding
