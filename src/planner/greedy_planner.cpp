#include "hexa_udon/planner/greedy_planner.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>

namespace hexa_udon::planner {
namespace {

struct AgentRoute {
    core::CellIndex destination;
    std::vector<core::Direction> directions;
    std::vector<std::size_t> spots;
    std::vector<pathfinding::RouteObjective> objectives;
    std::int64_t travel_steps = 0;
    std::int64_t fuel = 0;
    std::int64_t edges = 0;
};

struct Candidate {
    std::vector<AgentRoute> routes;
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    OfficialScore score;
    InternalTieBreak tie;
    DailyReadiness readiness;
};

std::int64_t grid_distance(const core::MapDefinition& map,
                           const core::CellIndex left, const core::CellIndex right) {
    const auto width = static_cast<std::int64_t>(map.width());
    const auto left_value = static_cast<std::int64_t>(left.value);
    const auto right_value = static_cast<std::int64_t>(right.value);
    const auto left_row = left_value / width;
    const auto right_row = right_value / width;
    const auto left_col = left_value % width;
    const auto right_col = right_value % width;
    return std::llabs(left_row - right_row) + std::llabs(left_col - right_col);
}

[[nodiscard]] simulator::DayActionPlan make_plan(
    const PlannerInput& input, const std::vector<AgentRoute>& routes)
{
    const auto day = static_cast<std::size_t>(input.daily.day);
    const auto day_steps = input.match.day_steps[day];
    simulator::DayActionPlan plan(input.daily.own_agents.size());
    for (std::size_t agent = 0; agent < plan.size(); ++agent) {
        for (const auto direction : routes[agent].directions) {
            plan[agent].emplace_back(simulator::MoveAction{direction});
        }
        const auto remaining = static_cast<std::int64_t>(day_steps) - routes[agent].travel_steps;
        if (remaining > 0) {
            plan[agent].emplace_back(simulator::WaitAction{static_cast<core::Quantity>(remaining)});
        }
    }
    return plan;
}

[[nodiscard]] InternalTieBreak tie_break(
    const PlannerInput& input,
    const simulator::DaySimulationResult& simulation,
    const std::vector<AgentRoute>& routes)
{
    InternalTieBreak result;
    for (std::size_t agent = 0; agent < simulation.end_agents.size(); ++agent) {
        if (simulation.end_agents[agent].kind == core::AgentKind::Patrol) {
            result.patrol_fuel_remaining += simulation.end_agents[agent].fuel;
        }
        result.total_travel_steps += routes[agent].travel_steps;
        result.total_edges += routes[agent].edges;
        for (const auto spot : routes[agent].spots) {
            result.deterministic_order.push_back(static_cast<std::int32_t>(agent));
            result.deterministic_order.push_back(static_cast<std::int32_t>(spot));
        }
    }
    static_cast<void>(input);
    return result;
}

[[nodiscard]] bool better_candidate(const Candidate& left, const Candidate& right)
{
    if (left.score != right.score) {
        return better_official_score(left.score, right.score);
    }
    if (better_daily_readiness(left.readiness, right.readiness)) return true;
    if (better_daily_readiness(right.readiness, left.readiness)) return false;
    return better_internal_tie_break(left.tie, right.tie);
}

[[nodiscard]] simulator::SimulationOutcome simulate(
    const PlannerInput& input, const simulator::DayActionPlan& plan)
{
    const auto day = static_cast<std::size_t>(input.daily.day);
    return simulator::simulate_day({
        input.match.map,
        input.match.spots,
        input.match.fuel_limit,
        input.match.day_steps[day],
        input.daily.own_agents,
        input.daily.traffic,
    }, plan);
}

}  // namespace

bool better_official_score(const OfficialScore& left, const OfficialScore& right) noexcept
{
    return std::tie(left.total_unique_brands,
                    left.cumulative_daily_unique_brands,
                    left.total_bowls)
        > std::tie(right.total_unique_brands,
                   right.cumulative_daily_unique_brands,
                   right.total_bowls);
}

bool better_internal_tie_break(
    const InternalTieBreak& left, const InternalTieBreak& right) noexcept
{
    if (left.patrol_fuel_remaining != right.patrol_fuel_remaining) {
        return left.patrol_fuel_remaining > right.patrol_fuel_remaining;
    }
    if (left.total_travel_steps != right.total_travel_steps) {
        return left.total_travel_steps < right.total_travel_steps;
    }
    if (left.total_edges != right.total_edges) {
        return left.total_edges < right.total_edges;
    }
    return std::lexicographical_compare(
        left.deterministic_order.begin(), left.deterministic_order.end(),
        right.deterministic_order.begin(), right.deterministic_order.end());
}

bool better_daily_readiness(
    const DailyReadiness& left, const DailyReadiness& right) noexcept {
    return std::tie(left.uncollected_spot_reachability, left.fuel_reserve,
                    left.patrol_dispersion, left.rendezvous_readiness)
        > std::tie(right.uncollected_spot_reachability, right.fuel_reserve,
                   right.patrol_dispersion, right.rendezvous_readiness)
        || (left.uncollected_spot_reachability == right.uncollected_spot_reachability
            && left.fuel_reserve == right.fuel_reserve
            && left.patrol_dispersion == right.patrol_dispersion
            && left.rendezvous_readiness == right.rendezvous_readiness
            && std::lexicographical_compare(left.deterministic_order.begin(),
                                             left.deterministic_order.end(),
                                             right.deterministic_order.begin(),
                                             right.deterministic_order.end()));
}

DailyReadiness daily_readiness(
    const core::MatchConfig& match, const core::DailyState& daily,
    const simulator::DaySimulationResult& simulation,
    const std::vector<std::vector<std::size_t>>& visited_spots) {
    DailyReadiness result;
    std::vector<bool> visited(match.spots.size(), false);
    for (const auto& visits : visited_spots) {
        for (const auto spot : visits) {
            if (spot < visited.size()) visited[spot] = true;
        }
    }
    for (std::size_t spot = 0; spot < match.spots.size(); ++spot) {
        if (visited[spot]) continue;
        std::int64_t best = std::numeric_limits<std::int64_t>::max();
        for (std::size_t agent = 0; agent < simulation.end_agents.size(); ++agent) {
            if (simulation.end_agents[agent].kind != core::AgentKind::Patrol) continue;
            const auto distance = grid_distance(match.map, simulation.end_agents[agent].position,
                                                match.spots[spot].position);
            // Two steps per plain edge is a conservative, future-road-free estimate.
            if (distance * 2 <= simulation.end_agents[agent].fuel) best = std::min(best, distance);
        }
        if (best != std::numeric_limits<std::int64_t>::max()) ++result.uncollected_spot_reachability;
    }
    std::vector<std::size_t> patrols;
    std::vector<std::size_t> supplies;
    for (std::size_t agent = 0; agent < simulation.end_agents.size(); ++agent) {
        if (simulation.end_agents[agent].kind == core::AgentKind::Patrol) {
            patrols.push_back(agent);
            result.fuel_reserve += simulation.end_agents[agent].fuel;
        } else if (simulation.end_agents[agent].kind == core::AgentKind::Supply) {
            supplies.push_back(agent);
        }
    }
    for (std::size_t first = 0; first < patrols.size(); ++first) {
        for (std::size_t second = first + 1; second < patrols.size(); ++second) {
            result.patrol_dispersion += grid_distance(match.map,
                simulation.end_agents[patrols[first]].position,
                simulation.end_agents[patrols[second]].position);
        }
    }
    for (const auto patrol : patrols) {
        std::int64_t nearest = std::numeric_limits<std::int64_t>::max();
        for (const auto supply : supplies) {
            nearest = std::min(nearest, grid_distance(match.map,
                simulation.end_agents[patrol].position,
                simulation.end_agents[supply].position));
        }
        if (nearest != std::numeric_limits<std::int64_t>::max()) {
            result.rendezvous_readiness += 1000 / (nearest + 1);
        }
    }
    for (const auto& agent : simulation.end_agents) {
        result.deterministic_order.push_back(static_cast<std::int32_t>(agent.position.value));
        result.deterministic_order.push_back(agent.fuel);
    }
    static_cast<void>(daily);
    return result;
}

OfficialScore official_score(
    const simulator::MatchProgress& previous,
    const simulator::DaySimulationResult& day)
{
    auto brands = previous.acquired_brands;
    brands.insert(day.distinct_brands.begin(), day.distinct_brands.end());
    const auto daily_sum = std::accumulate(
        previous.daily_distinct_brand_counts.begin(),
        previous.daily_distinct_brand_counts.end(),
        std::int64_t{0});
    return {
        static_cast<std::int64_t>(brands.size()),
        daily_sum + static_cast<std::int64_t>(day.distinct_brands.size()),
        static_cast<std::int64_t>(previous.total_balls) + day.total_balls,
    };
}

PlannerOutcome::PlannerOutcome(std::variant<PlannerResult, PlannerError> storage)
    : storage_(std::move(storage))
{
}

PlannerOutcome PlannerOutcome::success(PlannerResult result)
{
    return PlannerOutcome(std::move(result));
}

PlannerOutcome PlannerOutcome::failure(PlannerError error)
{
    return PlannerOutcome(std::move(error));
}

bool PlannerOutcome::has_value() const noexcept
{
    return std::holds_alternative<PlannerResult>(storage_);
}

PlannerOutcome::operator bool() const noexcept { return has_value(); }
const PlannerResult& PlannerOutcome::value() const& { return std::get<PlannerResult>(storage_); }
PlannerResult&& PlannerOutcome::value() && { return std::get<PlannerResult>(std::move(storage_)); }
const PlannerError& PlannerOutcome::error() const& { return std::get<PlannerError>(storage_); }

PlannerOutcome make_greedy_plan(
    const PlannerInput& input,
    const PlannerConfig& config,
    const std::chrono::steady_clock::time_point deadline,
    PlannerClock now)
{
    const auto started = now();
    if (input.daily.day < 0
        || static_cast<std::size_t>(input.daily.day) >= input.match.day_steps.size()
        || input.daily.own_agents.empty()
        || config.maximum_candidate_evaluations == 0) {
        return PlannerOutcome::failure({PlannerErrorCode::InvalidInput, "invalid planner input"});
    }

    std::vector<AgentRoute> routes;
    routes.reserve(input.daily.own_agents.size());
    for (const auto& agent : input.daily.own_agents) {
        routes.push_back(AgentRoute{agent.position, {}, {}, {}, 0, 0, 0});
    }
    auto baseline_plan = make_plan(input, routes);
    auto baseline_simulation = simulate(input, baseline_plan);
    if (!baseline_simulation) {
        return PlannerOutcome::failure({
            PlannerErrorCode::BaselineSimulationFailed,
            "all-wait baseline failed strict simulation: " + baseline_simulation.error().message,
        });
    }

    Candidate best{
        routes,
        std::move(baseline_plan),
        std::move(baseline_simulation).value(),
        {},
        {},
        {},
    };
    best.score = official_score(input.previous_progress, best.simulation);
    best.tie = tie_break(input, best.simulation, best.routes);
    best.readiness = daily_readiness(input.match, input.daily, best.simulation, {});
    const auto baseline_score = best.score;
    std::size_t simulator_runs = 1;
    std::size_t evaluated = 0;
    std::size_t accepted = 0;
    std::size_t tie_group_count = 1;
    PlannerTermination termination = PlannerTermination::NoImprovingCandidate;
    std::string diagnostic;

    auto pathfinder_result = pathfinding::Pathfinder::create(input.match.map, input.daily.traffic);
    if (!pathfinder_result) {
        PlannerResult fallback{
            std::move(best.plan), std::move(best.simulation), baseline_score, best.score, best.tie,
            best.readiness,
            tie_group_count,
            std::vector<std::vector<std::size_t>>(routes.size()),
            std::vector<std::vector<pathfinding::RouteObjective>>(routes.size()),
            std::vector<std::int64_t>(routes.size(), 0),
            PlannerTermination::Fallback, evaluated, accepted, simulator_runs,
            std::chrono::duration_cast<std::chrono::microseconds>(now() - started), config.seed,
            pathfinder_result.error().message,
        };
        return PlannerOutcome::success(std::move(fallback));
    }
    auto pathfinder = std::move(pathfinder_result).value();
    std::set<std::size_t> assigned_spots;
    const auto day_steps = input.match.day_steps[static_cast<std::size_t>(input.daily.day)];

    while (true) {
        if (now() >= deadline) {
            termination = PlannerTermination::Deadline;
            break;
        }
        std::optional<Candidate> round_best;
        bool limit_reached = false;
        for (std::size_t agent = 0; agent < input.daily.own_agents.size() && !limit_reached; ++agent) {
            if (input.daily.own_agents[agent].kind != core::AgentKind::Patrol) {
                continue;
            }
            for (std::size_t spot = 0; spot < input.match.spots.size() && !limit_reached; ++spot) {
                if (assigned_spots.contains(spot)) {
                    continue;
                }
                for (const auto objective : {
                         pathfinding::RouteObjective::Fastest,
                         pathfinding::RouteObjective::FuelEfficient}) {
                    if (now() >= deadline) {
                        termination = PlannerTermination::Deadline;
                        limit_reached = true;
                        break;
                    }
                    if (evaluated >= config.maximum_candidate_evaluations) {
                        termination = PlannerTermination::EvaluationLimit;
                        limit_reached = true;
                        break;
                    }
                    auto route = pathfinder.find_route(
                        best.routes[agent].destination,
                        input.match.spots[spot].position,
                        objective);
                    if (!route) {
                        diagnostic = route.error().message;
                        continue;
                    }
                    if (!route.value().has_value()) {
                        continue;
                    }
                    const auto& segment = *route.value();
                    if (objective == pathfinding::RouteObjective::FuelEfficient) {
                        const auto fastest = pathfinder.find_route(
                            best.routes[agent].destination,
                            input.match.spots[spot].position,
                            pathfinding::RouteObjective::Fastest);
                        if (fastest && fastest.value().has_value()
                            && fastest.value()->directions == segment.directions) {
                            continue;
                        }
                    }
                    auto candidate_routes = best.routes;
                    auto& candidate_agent = candidate_routes[agent];
                    const auto new_time = candidate_agent.travel_steps + segment.cost.travel_steps;
                    const auto new_fuel = candidate_agent.fuel + segment.cost.patrol_fuel;
                    if (new_time > day_steps
                        || new_fuel > input.daily.own_agents[agent].fuel) {
                        continue;
                    }
                    candidate_agent.destination = input.match.spots[spot].position;
                    candidate_agent.directions.insert(
                        candidate_agent.directions.end(),
                        segment.directions.begin(), segment.directions.end());
                    candidate_agent.spots.push_back(spot);
                    candidate_agent.objectives.push_back(objective);
                    candidate_agent.travel_steps = new_time;
                    candidate_agent.fuel = new_fuel;
                    candidate_agent.edges += segment.cost.edge_count;
                    auto plan = make_plan(input, candidate_routes);
                    ++evaluated;
                    auto simulation = simulate(input, plan);
                    ++simulator_runs;
                    if (!simulation) {
                        continue;
                    }
                    Candidate candidate{
                        std::move(candidate_routes),
                        std::move(plan),
                        std::move(simulation).value(),
                        {},
                        {},
                        {},
                    };
                    candidate.score = official_score(input.previous_progress, candidate.simulation);
                    candidate.tie = tie_break(input, candidate.simulation, candidate.routes);
                    std::vector<std::vector<std::size_t>> candidate_visits;
                    candidate_visits.reserve(candidate.routes.size());
                    for (const auto& candidate_route : candidate.routes) {
                        candidate_visits.push_back(candidate_route.spots);
                    }
                    candidate.readiness = daily_readiness(input.match, input.daily,
                                                          candidate.simulation, candidate_visits);
                    if (candidate.score == best.score) ++tie_group_count;
                    if (better_candidate(candidate, best)
                        && (!round_best.has_value() || better_candidate(candidate, *round_best))) {
                        round_best = std::move(candidate);
                    }
                }
            }
        }
        if (round_best.has_value()) {
            best = std::move(*round_best);
            assigned_spots.clear();
            for (const auto& route : best.routes) {
                assigned_spots.insert(route.spots.begin(), route.spots.end());
            }
            ++accepted;
            termination = PlannerTermination::Completed;
        } else {
            if (!limit_reached) {
                termination = PlannerTermination::NoImprovingCandidate;
            }
            break;
        }
    }

    std::vector<std::vector<std::size_t>> visits;
    std::vector<std::vector<pathfinding::RouteObjective>> objectives;
    std::vector<std::int64_t> travel_steps;
    visits.reserve(best.routes.size());
    objectives.reserve(best.routes.size());
    travel_steps.reserve(best.routes.size());
    for (const auto& route : best.routes) {
        visits.push_back(route.spots);
        objectives.push_back(route.objectives);
        travel_steps.push_back(route.travel_steps);
    }
    PlannerResult result{
        std::move(best.plan), std::move(best.simulation), baseline_score, best.score, best.tie,
        best.readiness,
        tie_group_count,
        std::move(visits), std::move(objectives), std::move(travel_steps),
        termination, evaluated, accepted,
        simulator_runs,
        std::chrono::duration_cast<std::chrono::microseconds>(now() - started), config.seed,
        std::move(diagnostic),
    };
    return PlannerOutcome::success(std::move(result));
}

}  // namespace hexa_udon::planner
