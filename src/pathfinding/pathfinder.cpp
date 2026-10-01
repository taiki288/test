#include "hexa_udon/pathfinding/pathfinder.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <queue>
#include <tuple>
#include <utility>

namespace hexa_udon::pathfinding {

struct PathSnapshot {
    core::MapDefinition map;
    std::vector<std::optional<core::RoadStatus>> road_status;
    std::string key;
};

namespace {

constexpr auto kInfinity = std::numeric_limits<std::int64_t>::max();

void append_i32(std::string& output, const std::int32_t value)
{
    const auto bits = static_cast<std::uint32_t>(value);
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<char>((bits >> shift) & 0xffU));
    }
}

[[nodiscard]] std::string make_snapshot_key(
    const core::MapDefinition& map,
    const std::vector<std::optional<core::RoadStatus>>& road_status)
{
    std::string key;
    key.reserve(8U + map.cells().size() * 2U);
    append_i32(key, map.height());
    append_i32(key, map.width());
    for (std::size_t i = 0; i < map.cells().size(); ++i) {
        key.push_back(static_cast<char>(core::to_int(map.cells()[i])));
        const auto status = road_status[i];
        key.push_back(status.has_value()
            ? static_cast<char>(core::to_int(*status) + 1)
            : static_cast<char>(0));
    }
    return key;
}

[[nodiscard]] std::optional<core::MovementCost> movement_cost(
    const PathSnapshot& snapshot, const core::CellIndex origin)
{
    const auto terrain = snapshot.map.terrain_at(origin);
    if (!terrain.has_value()) {
        return std::nullopt;
    }
    if (*terrain == core::Terrain::Road) {
        const auto status = snapshot.road_status[static_cast<std::size_t>(origin.value)];
        return status.has_value() ? core::movement_cost(*status) : std::nullopt;
    }
    const auto non_road = core::as_non_road(*terrain);
    return non_road.has_value() ? core::movement_cost(*non_road) : std::nullopt;
}

[[nodiscard]] auto metric(const RouteCost& cost, const RouteObjective objective)
{
    if (objective == RouteObjective::Fastest) {
        return std::tuple{cost.travel_steps, cost.patrol_fuel, cost.edge_count};
    }
    return std::tuple{cost.patrol_fuel, cost.travel_steps, cost.edge_count};
}

[[nodiscard]] bool add_cost(
    const RouteCost& base, const core::MovementCost edge, RouteCost& result)
{
    if (base.travel_steps > kInfinity - edge.steps
        || base.patrol_fuel > kInfinity - edge.fuel
        || base.edge_count == kInfinity) {
        return false;
    }
    result = {
        base.travel_steps + edge.steps,
        base.patrol_fuel + edge.fuel,
        base.edge_count + 1,
    };
    return true;
}

struct QueueEntry {
    RouteCost cost;
    core::CellIndex cell;
};

struct QueueLater {
    RouteObjective objective;

    [[nodiscard]] bool operator()(const QueueEntry& left, const QueueEntry& right) const
    {
        return std::tuple{metric(left.cost, objective), left.cell.value}
            > std::tuple{metric(right.cost, objective), right.cell.value};
    }
};

[[nodiscard]] PathfindingError make_error(
    const PathfindingErrorCode code,
    std::string message,
    const std::optional<core::CellIndex> cell = std::nullopt)
{
    return {code, std::move(message), cell};
}

}  // namespace

ShortestPathTree::ShortestPathTree(
    std::shared_ptr<const PathSnapshot> snapshot,
    const core::CellIndex source,
    const RouteObjective objective,
    std::vector<Label> labels)
    : snapshot_(std::move(snapshot)), source_(source), objective_(objective), labels_(std::move(labels))
{
}

bool ShortestPathTree::reachable(const core::CellIndex destination) const noexcept
{
    return snapshot_->map.contains(destination)
        && labels_[static_cast<std::size_t>(destination.value)].cost.has_value();
}

PathfindingResult<std::optional<Route>> ShortestPathTree::route_to(
    const core::CellIndex destination) const
{
    if (!snapshot_->map.contains(destination)) {
        return PathfindingResult<std::optional<Route>>::failure(make_error(
            PathfindingErrorCode::InvalidDestination,
            "destination is outside the map",
            destination));
    }
    if (snapshot_->map.terrain_at(destination) == core::Terrain::Pond) {
        return PathfindingResult<std::optional<Route>>::failure(make_error(
            PathfindingErrorCode::PondEndpoint,
            "destination is a pond",
            destination));
    }
    const auto& destination_label = labels_[static_cast<std::size_t>(destination.value)];
    if (!destination_label.cost.has_value()) {
        return PathfindingResult<std::optional<Route>>::success(std::nullopt);
    }

    Route route{source_, destination, {}, {}, *destination_label.cost};
    auto current = destination;
    route.cells.push_back(current);
    while (current != source_) {
        if (route.directions.size() >= labels_.size()) {
            return PathfindingResult<std::optional<Route>>::failure(make_error(
                PathfindingErrorCode::CorruptPredecessor,
                "predecessor chain contains a cycle",
                current));
        }
        const auto& label = labels_[static_cast<std::size_t>(current.value)];
        if (!label.predecessor.has_value() || !label.direction.has_value()) {
            return PathfindingResult<std::optional<Route>>::failure(make_error(
                PathfindingErrorCode::CorruptPredecessor,
                "predecessor chain is incomplete",
                current));
        }
        if (snapshot_->map.neighbor(*label.predecessor, *label.direction) != current) {
            return PathfindingResult<std::optional<Route>>::failure(make_error(
                PathfindingErrorCode::CorruptPredecessor,
                "predecessor direction does not reach the child",
                current));
        }
        route.directions.push_back(*label.direction);
        current = *label.predecessor;
        route.cells.push_back(current);
    }
    std::reverse(route.cells.begin(), route.cells.end());
    std::reverse(route.directions.begin(), route.directions.end());

    RouteCost recomputed{};
    for (std::size_t i = 0; i < route.directions.size(); ++i) {
        if (snapshot_->map.neighbor(route.cells[i], route.directions[i]) != route.cells[i + 1]) {
            return PathfindingResult<std::optional<Route>>::failure(make_error(
                PathfindingErrorCode::CorruptPredecessor,
                "restored route contains a non-adjacent edge",
                route.cells[i]));
        }
        const auto edge = movement_cost(*snapshot_, route.cells[i]);
        RouteCost next{};
        if (!edge.has_value() || !add_cost(recomputed, *edge, next)) {
            return PathfindingResult<std::optional<Route>>::failure(make_error(
                edge.has_value() ? PathfindingErrorCode::CostOverflow
                                 : PathfindingErrorCode::CorruptPredecessor,
                "restored route cost cannot be computed",
                route.cells[i]));
        }
        recomputed = next;
    }
    if (recomputed != route.cost) {
        return PathfindingResult<std::optional<Route>>::failure(make_error(
            PathfindingErrorCode::CorruptPredecessor,
            "restored route cost differs from the shortest-path label",
            destination));
    }
    return PathfindingResult<std::optional<Route>>::success(std::move(route));
}

Pathfinder::Pathfinder(std::shared_ptr<const PathSnapshot> snapshot)
    : snapshot_(std::move(snapshot))
{
}

PathfindingResult<Pathfinder> Pathfinder::create(
    const core::MapDefinition& map,
    const std::span<const core::TrafficState> traffic)
{
    std::vector<std::optional<core::RoadStatus>> statuses(
        static_cast<std::size_t>(map.cell_count()));
    for (const auto& item : traffic) {
        if (!map.contains(item.position)) {
            return PathfindingResult<Pathfinder>::failure(make_error(
                PathfindingErrorCode::InvalidTrafficPosition,
                "traffic position is outside the map",
                item.position));
        }
        if (map.terrain_at(item.position) != core::Terrain::Road) {
            return PathfindingResult<Pathfinder>::failure(make_error(
                PathfindingErrorCode::TrafficOnNonRoad,
                "traffic state refers to a non-road cell",
                item.position));
        }
        if (!core::road_status_from_int(core::to_int(item.status)).has_value()) {
            return PathfindingResult<Pathfinder>::failure(make_error(
                PathfindingErrorCode::InvalidRoadStatus,
                "traffic status is outside the official enumeration",
                item.position));
        }
        auto& slot = statuses[static_cast<std::size_t>(item.position.value)];
        if (slot.has_value()) {
            return PathfindingResult<Pathfinder>::failure(make_error(
                PathfindingErrorCode::DuplicateRoadStatus,
                "road cell has duplicate traffic states",
                item.position));
        }
        slot = item.status;
    }
    for (std::int32_t value = 0; value < map.cell_count(); ++value) {
        const core::CellIndex cell{value};
        if (map.terrain_at(cell) == core::Terrain::Road
            && !statuses[static_cast<std::size_t>(value)].has_value()) {
            return PathfindingResult<Pathfinder>::failure(make_error(
                PathfindingErrorCode::MissingRoadStatus,
                "road cell is missing a road status",
                cell));
        }
    }

    auto snapshot = std::make_shared<PathSnapshot>(PathSnapshot{
        map,
        std::move(statuses),
        {},
    });
    snapshot->key = make_snapshot_key(snapshot->map, snapshot->road_status);
    return PathfindingResult<Pathfinder>::success(Pathfinder(std::move(snapshot)));
}

PathfindingResult<ShortestPathTree> Pathfinder::shortest_path_tree(
    const core::CellIndex source, const RouteObjective objective) const
{
    if (!snapshot_->map.contains(source)) {
        return PathfindingResult<ShortestPathTree>::failure(make_error(
            PathfindingErrorCode::InvalidSource, "source is outside the map", source));
    }
    if (snapshot_->map.terrain_at(source) == core::Terrain::Pond) {
        return PathfindingResult<ShortestPathTree>::failure(make_error(
            PathfindingErrorCode::PondEndpoint, "source is a pond", source));
    }

    std::vector<ShortestPathTree::Label> labels(
        static_cast<std::size_t>(snapshot_->map.cell_count()));
    labels[static_cast<std::size_t>(source.value)].cost = RouteCost{};
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueLater> queue{
        QueueLater{objective}};
    queue.push({RouteCost{}, source});

    while (!queue.empty()) {
        const auto entry = queue.top();
        queue.pop();
        const auto& current_label = labels[static_cast<std::size_t>(entry.cell.value)];
        if (!current_label.cost.has_value() || *current_label.cost != entry.cost) {
            continue;
        }
        const auto edge = movement_cost(*snapshot_, entry.cell);
        if (!edge.has_value()) {
            continue;
        }
        for (std::int32_t raw_direction = 0; raw_direction < 6; ++raw_direction) {
            const auto direction = *core::direction_from_int(raw_direction);
            const auto next = snapshot_->map.neighbor(entry.cell, direction);
            if (!next.has_value() || snapshot_->map.terrain_at(*next) == core::Terrain::Pond) {
                continue;
            }
            RouteCost candidate{};
            if (!add_cost(entry.cost, *edge, candidate)) {
                return PathfindingResult<ShortestPathTree>::failure(make_error(
                    PathfindingErrorCode::CostOverflow,
                    "route cost exceeds int64 range",
                    entry.cell));
            }
            auto& next_label = labels[static_cast<std::size_t>(next->value)];
            const auto candidate_tie = std::tuple{raw_direction, entry.cell.value};
            const auto old_tie = std::tuple{
                next_label.direction.has_value() ? core::to_int(*next_label.direction) : 6,
                next_label.predecessor.has_value() ? next_label.predecessor->value
                                                   : std::numeric_limits<std::int32_t>::max()};
            if (!next_label.cost.has_value()
                || metric(candidate, objective) < metric(*next_label.cost, objective)
                || (metric(candidate, objective) == metric(*next_label.cost, objective)
                    && candidate_tie < old_tie)) {
                next_label.cost = candidate;
                next_label.predecessor = entry.cell;
                next_label.direction = direction;
                queue.push({candidate, *next});
            }
        }
    }
    return PathfindingResult<ShortestPathTree>::success(ShortestPathTree(
        snapshot_, source, objective, std::move(labels)));
}

PathfindingResult<std::optional<Route>> Pathfinder::find_route(
    const core::CellIndex source,
    const core::CellIndex destination,
    const RouteObjective objective) const
{
    if (!snapshot_->map.contains(destination)) {
        return PathfindingResult<std::optional<Route>>::failure(make_error(
            PathfindingErrorCode::InvalidDestination,
            "destination is outside the map",
            destination));
    }
    if (snapshot_->map.terrain_at(destination) == core::Terrain::Pond) {
        return PathfindingResult<std::optional<Route>>::failure(make_error(
            PathfindingErrorCode::PondEndpoint, "destination is a pond", destination));
    }
    auto tree = shortest_path_tree(source, objective);
    if (!tree) {
        return PathfindingResult<std::optional<Route>>::failure(tree.error());
    }
    return tree.value().route_to(destination);
}

const std::string& Pathfinder::snapshot_key() const noexcept
{
    return snapshot_->key;
}

const core::MapDefinition& Pathfinder::map() const noexcept
{
    return snapshot_->map;
}

}  // namespace hexa_udon::pathfinding
