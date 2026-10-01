#pragma once

#include "hexa_udon/core/map_definition.hpp"
#include "hexa_udon/core/models.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hexa_udon::pathfinding {

enum class RouteObjective {
    Fastest,
    FuelEfficient,
};

struct RouteCost {
    std::int64_t travel_steps = 0;
    std::int64_t patrol_fuel = 0;
    std::int64_t edge_count = 0;

    [[nodiscard]] bool operator==(const RouteCost&) const = default;
};

struct Route {
    core::CellIndex source;
    core::CellIndex destination;
    std::vector<core::CellIndex> cells;
    std::vector<core::Direction> directions;
    RouteCost cost;
};

enum class PathfindingErrorCode {
    InvalidSource,
    InvalidDestination,
    PondEndpoint,
    InvalidTrafficPosition,
    InvalidRoadStatus,
    TrafficOnNonRoad,
    DuplicateRoadStatus,
    MissingRoadStatus,
    CostOverflow,
    CorruptPredecessor,
    InvalidImportantPoint,
};

struct PathfindingError {
    PathfindingErrorCode code;
    std::string message;
    std::optional<core::CellIndex> cell;
};

template <typename T>
class PathfindingResult {
public:
    [[nodiscard]] static PathfindingResult success(T value)
    {
        return PathfindingResult(std::move(value));
    }

    [[nodiscard]] static PathfindingResult failure(PathfindingError error)
    {
        return PathfindingResult(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }
    [[nodiscard]] const PathfindingError& error() const& { return std::get<PathfindingError>(storage_); }

private:
    explicit PathfindingResult(std::variant<T, PathfindingError> storage)
        : storage_(std::move(storage))
    {
    }

    std::variant<T, PathfindingError> storage_;
};

struct PathSnapshot;

class ShortestPathTree {
public:
    [[nodiscard]] core::CellIndex source() const noexcept { return source_; }
    [[nodiscard]] RouteObjective objective() const noexcept { return objective_; }
    [[nodiscard]] bool reachable(core::CellIndex destination) const noexcept;
    [[nodiscard]] PathfindingResult<std::optional<Route>> route_to(
        core::CellIndex destination) const;
    [[nodiscard]] std::size_t stored_cell_count() const noexcept { return labels_.size(); }

private:
    friend class Pathfinder;

    struct Label {
        std::optional<RouteCost> cost;
        std::optional<core::CellIndex> predecessor;
        std::optional<core::Direction> direction;
    };

    ShortestPathTree(
        std::shared_ptr<const PathSnapshot> snapshot,
        core::CellIndex source,
        RouteObjective objective,
        std::vector<Label> labels);

    std::shared_ptr<const PathSnapshot> snapshot_;
    core::CellIndex source_;
    RouteObjective objective_;
    std::vector<Label> labels_;
};

class Pathfinder {
public:
    [[nodiscard]] static PathfindingResult<Pathfinder> create(
        const core::MapDefinition& map,
        std::span<const core::TrafficState> traffic);

    [[nodiscard]] PathfindingResult<ShortestPathTree> shortest_path_tree(
        core::CellIndex source, RouteObjective objective) const;
    [[nodiscard]] PathfindingResult<std::optional<Route>> find_route(
        core::CellIndex source,
        core::CellIndex destination,
        RouteObjective objective) const;

    [[nodiscard]] const std::string& snapshot_key() const noexcept;
    [[nodiscard]] const core::MapDefinition& map() const noexcept;

private:
    explicit Pathfinder(std::shared_ptr<const PathSnapshot> snapshot);
    std::shared_ptr<const PathSnapshot> snapshot_;
};

}  // namespace hexa_udon::pathfinding
