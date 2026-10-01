#include "hexa_udon/simulator/simulator.hpp"

#include "hexa_udon/core/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hexa_udon::simulator {

namespace {

struct PendingMove {
    core::CellIndex destination;
    core::Quantity remaining_steps;
    core::Quantity fuel_cost;
};

struct PendingWait {
    core::Quantity remaining_steps;
};

using PendingAction = std::variant<std::monostate, PendingMove, PendingWait>;

struct WorkingState {
    std::vector<core::AgentState> agents;
    std::vector<core::Quantity> stock;
    std::vector<AgentAcquisition> acquisitions;
    std::vector<std::vector<bool>> acquired_spots;
    std::set<core::Quantity> distinct_brands;
    core::Quantity total_balls = 0;
    std::vector<core::Quantity> road_traffic;
    std::vector<PendingAction> pending;
    std::vector<std::size_t> next_command;
    std::vector<TraceEvent> trace;
};

[[nodiscard]] SimulationError error(
    const SimulationErrorCode code,
    std::string message,
    const std::optional<std::size_t> agent = std::nullopt,
    const std::optional<std::size_t> command = std::nullopt,
    const std::optional<core::Quantity> step = std::nullopt)
{
    return {code, std::move(message), agent, command, step};
}

void add_trace(
    WorkingState& state,
    const TraceMode mode,
    TraceEvent event)
{
    if (mode == TraceMode::Enabled) {
        state.trace.push_back(std::move(event));
    }
}

[[nodiscard]] std::optional<SimulationError> validate_input(
    const DaySimulationInput& input,
    std::vector<std::optional<core::RoadStatus>>& road_status)
{
    if (input.day_steps <= 0) {
        return error(SimulationErrorCode::InvalidInputState, "day_steps must be positive");
    }
    if (input.fuel_limit <= 0) {
        return error(SimulationErrorCode::InvalidInputState, "fuel_limit must be positive");
    }

    road_status.resize(static_cast<std::size_t>(input.map.cell_count()));
    for (const auto& traffic : input.traffic) {
        if (!input.map.contains(traffic.position)) {
            return error(SimulationErrorCode::InvalidTrafficState, "traffic position is outside the map");
        }
        if (!core::road_status_from_int(core::to_int(traffic.status)).has_value()) {
            return error(SimulationErrorCode::InvalidTrafficState, "traffic status is invalid");
        }
        const auto terrain = input.map.terrain_at(traffic.position);
        if (!terrain.has_value() || *terrain != core::Terrain::Road) {
            return error(SimulationErrorCode::InvalidTrafficState, "traffic state refers to a non-road cell");
        }
        auto& entry = road_status[static_cast<std::size_t>(traffic.position.value)];
        if (entry.has_value()) {
            return error(SimulationErrorCode::InvalidTrafficState, "road cell has duplicate traffic states");
        }
        entry = traffic.status;
    }

    for (core::Quantity cell = 0; cell < input.map.cell_count(); ++cell) {
        const core::CellIndex index{cell};
        if (input.map.terrain_at(index) == core::Terrain::Road
            && !road_status[static_cast<std::size_t>(cell)].has_value()) {
            return error(SimulationErrorCode::MissingRoadStatus, "road cell is missing a road status");
        }
    }

    for (std::size_t i = 0; i < input.agents.size(); ++i) {
        const auto& agent = input.agents[i];
        if (!input.map.contains(agent.position)) {
            return error(SimulationErrorCode::InvalidInputState, "agent position is outside the map", i);
        }
        if (input.map.terrain_at(agent.position) == core::Terrain::Pond) {
            return error(SimulationErrorCode::InvalidInputState, "agent cannot start on a pond", i);
        }
        if (!core::agent_kind_from_int(core::to_int(agent.kind)).has_value()) {
            return error(SimulationErrorCode::InvalidInputState, "agent kind is invalid", i);
        }
        if (agent.kind == core::AgentKind::Patrol
            && (agent.fuel < 0 || agent.fuel > input.fuel_limit)) {
            return error(SimulationErrorCode::InvalidInputState, "patrol fuel is outside the valid range", i);
        }
    }

    std::vector<bool> occupied(static_cast<std::size_t>(input.map.cell_count()), false);
    for (std::size_t i = 0; i < input.spots.size(); ++i) {
        const auto& spot = input.spots[i];
        if (!input.map.contains(spot.position)
            || input.map.terrain_at(spot.position) != core::Terrain::Plain
            || spot.max_stock <= 0) {
            return error(SimulationErrorCode::InvalidInputState, "spot definition is invalid");
        }
        const auto spot_position = static_cast<std::size_t>(spot.position.value);
        if (occupied[spot_position]) {
            return error(SimulationErrorCode::InvalidInputState, "multiple spots occupy one cell");
        }
        occupied[spot_position] = true;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<core::MovementCost> cost_from_origin(
    const DaySimulationInput& input,
    const std::vector<std::optional<core::RoadStatus>>& road_status,
    const core::CellIndex origin)
{
    const auto terrain = input.map.terrain_at(origin);
    if (!terrain.has_value()) {
        return std::nullopt;
    }
    if (*terrain == core::Terrain::Road) {
        const auto status = road_status[static_cast<std::size_t>(origin.value)];
        return status.has_value() ? core::movement_cost(*status) : std::nullopt;
    }
    const auto non_road = core::as_non_road(*terrain);
    return non_road.has_value() ? core::movement_cost(*non_road) : std::nullopt;
}

[[nodiscard]] std::optional<SimulationError> start_action(
    const DaySimulationInput& input,
    const DayActionPlan& plan,
    const std::vector<std::optional<core::RoadStatus>>& road_status,
    WorkingState& state,
    const std::size_t agent_index,
    const core::Quantity step,
    const TraceMode trace_mode)
{
    const auto command_index = state.next_command[agent_index];
    if (command_index >= plan[agent_index].size()) {
        return error(
            SimulationErrorCode::ActionTimeInsufficient,
            "action sequence ended before the day finished",
            agent_index,
            command_index,
            step);
    }

    const auto remaining_day_steps = input.day_steps - step;
    const auto& action = plan[agent_index][command_index];
    std::int32_t action_value = 0;
    core::Quantity action_duration = 0;
    if (const auto* wait = std::get_if<WaitAction>(&action)) {
        if (wait->steps <= 0) {
            return error(
                SimulationErrorCode::InvalidWaitDuration,
                "wait duration must be positive",
                agent_index,
                command_index,
                step);
        }
        if (wait->steps > remaining_day_steps) {
            return error(
                SimulationErrorCode::ActionTimeExceeded,
                "wait action exceeds the remaining day steps",
                agent_index,
                command_index,
                step);
        }
        state.pending[agent_index] = PendingWait{wait->steps};
        action_value = -wait->steps;
        action_duration = wait->steps;
    } else {
        const auto move = std::get<MoveAction>(action);
        if (!core::direction_from_int(core::to_int(move.direction)).has_value()) {
            return error(
                SimulationErrorCode::InvalidActionValue,
                "movement direction is invalid",
                agent_index,
                command_index,
                step);
        }
        const auto destination = input.map.neighbor(
            state.agents[agent_index].position, move.direction);
        if (!destination.has_value()) {
            return error(
                SimulationErrorCode::OutOfBoundsMove,
                "movement leaves the map",
                agent_index,
                command_index,
                step);
        }
        if (input.map.terrain_at(*destination) == core::Terrain::Pond) {
            return error(
                SimulationErrorCode::MoveIntoPond,
                "movement enters a pond",
                agent_index,
                command_index,
                step);
        }
        const auto cost = cost_from_origin(
            input, road_status, state.agents[agent_index].position);
        if (!cost.has_value()) {
            return error(
                SimulationErrorCode::InvalidInputState,
                "movement cost is unavailable for the origin cell",
                agent_index,
                command_index,
                step);
        }
        if (state.agents[agent_index].kind == core::AgentKind::Patrol
            && state.agents[agent_index].fuel < cost->fuel) {
            return error(
                SimulationErrorCode::InsufficientFuel,
                "patrol lacks fuel when movement is reserved",
                agent_index,
                command_index,
                step);
        }
        if (cost->steps > remaining_day_steps) {
            return error(
                SimulationErrorCode::IncompleteMove,
                "movement cannot finish by the final step",
                agent_index,
                command_index,
                step);
        }
        state.pending[agent_index] = PendingMove{
            *destination,
            cost->steps,
            cost->fuel,
        };
        action_value = core::to_int(move.direction);
        action_duration = cost->steps;
    }

    ++state.next_command[agent_index];
    add_trace(state, trace_mode, {
        step,
        SimulationPhase::ActionReservation,
        EventKind::ActionStarted,
        agent_index,
        state.agents[agent_index].position,
        state.agents[agent_index].position,
        state.agents[agent_index].fuel,
        state.agents[agent_index].fuel,
        std::nullopt,
        action_value,
        action_duration,
    });
    return std::nullopt;
}

void decrement_actions(WorkingState& state)
{
    for (auto& pending : state.pending) {
        if (auto* move = std::get_if<PendingMove>(&pending)) {
            --move->remaining_steps;
            if (move->remaining_steps == 0) {
                pending = std::monostate{};
            }
        } else if (auto* wait = std::get_if<PendingWait>(&pending)) {
            --wait->remaining_steps;
            if (wait->remaining_steps == 0) {
                pending = std::monostate{};
            }
        }
    }
}

}  // namespace

SimulationOutcome::SimulationOutcome(
    std::variant<DaySimulationResult, SimulationError> storage)
    : storage_(std::move(storage))
{
}

SimulationOutcome SimulationOutcome::success(DaySimulationResult result)
{
    return SimulationOutcome(std::move(result));
}

SimulationOutcome SimulationOutcome::failure(SimulationError error_value)
{
    return SimulationOutcome(std::move(error_value));
}

bool SimulationOutcome::has_value() const noexcept
{
    return std::holds_alternative<DaySimulationResult>(storage_);
}

SimulationOutcome::operator bool() const noexcept
{
    return has_value();
}

const DaySimulationResult& SimulationOutcome::value() const&
{
    return std::get<DaySimulationResult>(storage_);
}

DaySimulationResult&& SimulationOutcome::value() &&
{
    return std::get<DaySimulationResult>(std::move(storage_));
}

const SimulationError& SimulationOutcome::error() const&
{
    return std::get<SimulationError>(storage_);
}

SimulationOutcome simulate_day(
    const DaySimulationInput& input,
    const DayActionPlan& plan,
    const TraceMode trace_mode)
{
    std::vector<std::optional<core::RoadStatus>> road_status;
    if (const auto input_error = validate_input(input, road_status)) {
        return SimulationOutcome::failure(*input_error);
    }
    if (plan.size() != input.agents.size()) {
        return SimulationOutcome::failure(error(
            SimulationErrorCode::AgentCountMismatch,
            "action plan count must match the agent count"));
    }

    WorkingState state{
        .agents = std::vector<core::AgentState>(input.agents.begin(), input.agents.end()),
        .stock = {},
        .acquisitions = std::vector<AgentAcquisition>(input.agents.size()),
        .acquired_spots = std::vector<std::vector<bool>>(
            input.agents.size(), std::vector<bool>(input.spots.size(), false)),
        .distinct_brands = {},
        .total_balls = 0,
        .road_traffic = std::vector<core::Quantity>(
            static_cast<std::size_t>(input.map.cell_count()), 0),
        .pending = std::vector<PendingAction>(input.agents.size()),
        .next_command = std::vector<std::size_t>(input.agents.size(), 0),
        .trace = {},
    };
    state.stock.reserve(input.spots.size());
    for (const auto& spot : input.spots) {
        state.stock.push_back(spot.max_stock);
    }

    for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
        if (const auto action_error = start_action(
                input, plan, road_status, state, agent, 0, trace_mode)) {
            return SimulationOutcome::failure(*action_error);
        }
    }

    for (core::Quantity step = 1; step <= input.day_steps; ++step) {
        std::vector<bool> completing_move(state.agents.size(), false);
        for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
            const auto* move = std::get_if<PendingMove>(&state.pending[agent]);
            completing_move[agent] = move != nullptr && move->remaining_steps == 1;
            if (completing_move[agent]
                && state.agents[agent].kind == core::AgentKind::Patrol) {
                const auto old_fuel = state.agents[agent].fuel;
                state.agents[agent].fuel -= move->fuel_cost;
                add_trace(state, trace_mode, {
                    step,
                    SimulationPhase::FuelConsumption,
                    EventKind::FuelConsumed,
                    agent,
                    state.agents[agent].position,
                    state.agents[agent].position,
                    old_fuel,
                    state.agents[agent].fuel,
                    std::nullopt,
                });
            }
        }

        for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
            if (!completing_move[agent]) {
                continue;
            }
            const auto move = std::get<PendingMove>(state.pending[agent]);
            const auto old_position = state.agents[agent].position;
            state.agents[agent].position = move.destination;
            add_trace(state, trace_mode, {
                step,
                SimulationPhase::Movement,
                EventKind::Moved,
                agent,
                old_position,
                move.destination,
                state.agents[agent].fuel,
                state.agents[agent].fuel,
                std::nullopt,
            });
        }
        decrement_actions(state);

        for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
            if (state.agents[agent].kind != core::AgentKind::Patrol) {
                continue;
            }
            for (std::size_t spot = 0; spot < input.spots.size(); ++spot) {
                if (input.spots[spot].position != state.agents[agent].position
                    || state.acquired_spots[agent][spot]) {
                    continue;
                }
                const bool succeeds = state.stock[spot] > 0;
                add_trace(state, trace_mode, {
                    step,
                    SimulationPhase::Acquisition,
                    EventKind::UdonAcquisitionAttempted,
                    agent,
                    state.agents[agent].position,
                    state.agents[agent].position,
                    state.agents[agent].fuel,
                    state.agents[agent].fuel,
                    spot,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    succeeds,
                });
                if (succeeds) {
                    --state.stock[spot];
                    state.acquired_spots[agent][spot] = true;
                    state.acquisitions[agent].spot_indices.push_back(spot);
                    state.acquisitions[agent].brands.push_back(input.spots[spot].brand);
                    state.distinct_brands.insert(input.spots[spot].brand);
                    ++state.total_balls;
                    add_trace(state, trace_mode, {
                        step,
                        SimulationPhase::Acquisition,
                        EventKind::UdonAcquired,
                        agent,
                        state.agents[agent].position,
                        state.agents[agent].position,
                        state.agents[agent].fuel,
                        state.agents[agent].fuel,
                        spot,
                    });
                }
            }
        }

        for (std::size_t patrol = 0; patrol < state.agents.size(); ++patrol) {
            if (state.agents[patrol].kind != core::AgentKind::Patrol) {
                continue;
            }
            bool supply_present = false;
            for (std::size_t supply = 0; supply < state.agents.size(); ++supply) {
                const auto& agent = state.agents[supply];
                if (agent.kind == core::AgentKind::Supply
                    && agent.position == state.agents[patrol].position) {
                    supply_present = true;
                    add_trace(state, trace_mode, {
                        step,
                        SimulationPhase::Refuel,
                        EventKind::RefuelPresence,
                        patrol,
                        state.agents[patrol].position,
                        state.agents[patrol].position,
                        state.agents[patrol].fuel,
                        state.agents[patrol].fuel,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        supply,
                    });
                }
            }
            if (supply_present && state.agents[patrol].fuel != input.fuel_limit) {
                const auto old_fuel = state.agents[patrol].fuel;
                state.agents[patrol].fuel = input.fuel_limit;
                add_trace(state, trace_mode, {
                    step,
                    SimulationPhase::Refuel,
                    EventKind::Refueled,
                    patrol,
                    state.agents[patrol].position,
                    state.agents[patrol].position,
                    old_fuel,
                    state.agents[patrol].fuel,
                    std::nullopt,
                });
            }
        }

        for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
            const auto position = state.agents[agent].position;
            if (input.map.terrain_at(position) == core::Terrain::Road) {
                ++state.road_traffic[static_cast<std::size_t>(position.value)];
                add_trace(state, trace_mode, {
                    step,
                    SimulationPhase::Traffic,
                    EventKind::TrafficIncremented,
                    agent,
                    position,
                    position,
                    state.agents[agent].fuel,
                    state.agents[agent].fuel,
                    std::nullopt,
                });
            }
        }

        if (step < input.day_steps) {
            for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
                if (std::holds_alternative<std::monostate>(state.pending[agent])) {
                    if (const auto action_error = start_action(
                            input, plan, road_status, state, agent, step, trace_mode)) {
                        return SimulationOutcome::failure(*action_error);
                    }
                }
            }
        }
    }

    for (std::size_t agent = 0; agent < state.agents.size(); ++agent) {
        if (!std::holds_alternative<std::monostate>(state.pending[agent])) {
            return SimulationOutcome::failure(error(
                SimulationErrorCode::ActionTimeExceeded,
                "agent has an unfinished action after the final step",
                agent,
                state.next_command[agent] - 1,
                input.day_steps));
        }
        if (state.next_command[agent] != plan[agent].size()) {
            return SimulationOutcome::failure(error(
                SimulationErrorCode::ActionTimeExceeded,
                "commands remain after the final step",
                agent,
                state.next_command[agent],
                input.day_steps));
        }
    }

    return SimulationOutcome::success({
        .end_agents = std::move(state.agents),
        .acquisitions = std::move(state.acquisitions),
        .distinct_brands = std::move(state.distinct_brands),
        .total_balls = state.total_balls,
        .remaining_stock = std::move(state.stock),
        .road_traffic_by_cell = std::move(state.road_traffic),
        .trace = std::move(state.trace),
    });
}

SimulationOutcome simulate_day(
    const DaySimulationInput& input,
    const RawDayActionPlan& raw_plan,
    const TraceMode trace_mode)
{
    auto parsed = parse_action_plan(raw_plan);
    if (!parsed) {
        return SimulationOutcome::failure(parsed.error());
    }
    return simulate_day(input, std::move(parsed).value(), trace_mode);
}

MatchProgress accumulate_progress(
    const MatchProgress& previous, const DaySimulationResult& day_result)
{
    MatchProgress result = previous;
    result.acquired_brands.insert(
        day_result.distinct_brands.begin(), day_result.distinct_brands.end());
    result.total_balls += day_result.total_balls;
    result.daily_distinct_brand_counts.push_back(
        static_cast<core::Quantity>(day_result.distinct_brands.size()));
    return result;
}

}  // namespace hexa_udon::simulator
