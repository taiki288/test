#pragma once

#include "hexa_udon/core/map_definition.hpp"
#include "hexa_udon/core/models.hpp"
#include "hexa_udon/simulator/action.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <variant>
#include <vector>

namespace hexa_udon::simulator {

enum class TraceMode {
    Disabled,
    Enabled,
};

enum class SimulationPhase {
    ActionReservation,
    FuelConsumption,
    Movement,
    Acquisition,
    Refuel,
    Traffic,
};

enum class EventKind {
    ActionStarted,
    FuelConsumed,
    Moved,
    UdonAcquisitionAttempted,
    UdonAcquired,
    RefuelPresence,
    Refueled,
    TrafficIncremented,
};

struct TraceEvent {
    core::Quantity step;
    SimulationPhase phase;
    EventKind kind;
    std::optional<std::size_t> agent_index;
    std::optional<core::CellIndex> old_position;
    std::optional<core::CellIndex> new_position;
    std::optional<core::Quantity> old_fuel;
    std::optional<core::Quantity> new_fuel;
    std::optional<std::size_t> spot_index;
    std::optional<std::int32_t> action_value;
    std::optional<core::Quantity> action_duration;
    std::optional<std::size_t> counterpart_agent_index;
    bool succeeded = true;

    TraceEvent(core::Quantity event_step, SimulationPhase event_phase, EventKind event_kind,
        std::optional<std::size_t> event_agent_index,
        std::optional<core::CellIndex> event_old_position,
        std::optional<core::CellIndex> event_new_position,
        std::optional<core::Quantity> event_old_fuel,
        std::optional<core::Quantity> event_new_fuel,
        std::optional<std::size_t> event_spot_index,
        std::optional<std::int32_t> event_action_value = std::nullopt,
        std::optional<core::Quantity> event_action_duration = std::nullopt,
        std::optional<std::size_t> event_counterpart_agent_index = std::nullopt,
        bool event_succeeded = true)
        : step(event_step), phase(event_phase), kind(event_kind),
          agent_index(event_agent_index), old_position(event_old_position),
          new_position(event_new_position), old_fuel(event_old_fuel), new_fuel(event_new_fuel),
          spot_index(event_spot_index), action_value(event_action_value),
          action_duration(event_action_duration),
          counterpart_agent_index(event_counterpart_agent_index), succeeded(event_succeeded) {}
};

struct DaySimulationInput {
    const core::MapDefinition& map;
    std::span<const core::Spot> spots;
    core::Quantity fuel_limit;
    core::Quantity day_steps;
    std::span<const core::AgentState> agents;
    std::span<const core::TrafficState> traffic;
};

struct AgentAcquisition {
    std::vector<std::size_t> spot_indices;
    std::vector<core::Quantity> brands;
};

struct DaySimulationResult {
    std::vector<core::AgentState> end_agents;
    std::vector<AgentAcquisition> acquisitions;
    std::set<core::Quantity> distinct_brands;
    core::Quantity total_balls;
    std::vector<core::Quantity> remaining_stock;
    std::vector<core::Quantity> road_traffic_by_cell;
    std::vector<TraceEvent> trace;
};

struct MatchProgress {
    std::set<core::Quantity> acquired_brands;
    core::Quantity total_balls = 0;
    std::vector<core::Quantity> daily_distinct_brand_counts;
};

class SimulationOutcome {
public:
    [[nodiscard]] static SimulationOutcome success(DaySimulationResult result);
    [[nodiscard]] static SimulationOutcome failure(SimulationError error);

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const DaySimulationResult& value() const&;
    [[nodiscard]] DaySimulationResult&& value() &&;
    [[nodiscard]] const SimulationError& error() const&;

private:
    explicit SimulationOutcome(std::variant<DaySimulationResult, SimulationError> storage);

    std::variant<DaySimulationResult, SimulationError> storage_;
};

[[nodiscard]] SimulationOutcome simulate_day(
    const DaySimulationInput& input,
    const DayActionPlan& plan,
    TraceMode trace_mode = TraceMode::Disabled);

[[nodiscard]] SimulationOutcome simulate_day(
    const DaySimulationInput& input,
    const RawDayActionPlan& raw_plan,
    TraceMode trace_mode = TraceMode::Disabled);

[[nodiscard]] MatchProgress accumulate_progress(
    const MatchProgress& previous, const DaySimulationResult& day_result);

}  // namespace hexa_udon::simulator
