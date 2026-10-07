#include "hexa_udon/planner/refuel_planner.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace hexa_udon::planner {
namespace {

struct Prefix {
    std::vector<simulator::Action> actions;
    core::CellIndex position;
    std::int64_t elapsed = 0;
    std::int64_t travel = 0;
    std::int64_t waits = 0;
    std::size_t refuels = 0;
};

struct Candidate {
    std::vector<Prefix> prefixes;
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    OfficialScore score;
    std::vector<std::vector<std::size_t>> visits;
    std::vector<RendezvousEvent> meetings;
    DailyReadiness readiness;
};

bool better_candidate(const Candidate& left, const Candidate& right) {
    if (left.score != right.score) return better_official_score(left.score, right.score);
    if (better_daily_readiness(left.readiness, right.readiness)) return true;
    if (better_daily_readiness(right.readiness, left.readiness)) return false;
    return false;
}

void append_wait(Prefix& prefix, const std::int64_t steps) {
    if (steps <= 0) return;
    prefix.actions.emplace_back(simulator::WaitAction{static_cast<core::Quantity>(steps)});
    prefix.elapsed += steps;
    prefix.waits += steps;
}

void append_route(Prefix& prefix, const pathfinding::Route& route) {
    for (const auto direction : route.directions) {
        prefix.actions.emplace_back(simulator::MoveAction{direction});
    }
    prefix.position = route.destination;
    prefix.elapsed += route.cost.travel_steps;
    prefix.travel += route.cost.travel_steps;
}

simulator::DayActionPlan complete_plan(
    const std::vector<Prefix>& prefixes, const core::Quantity day_steps) {
    simulator::DayActionPlan result;
    result.reserve(prefixes.size());
    for (const auto& prefix : prefixes) {
        auto actions = prefix.actions;
        const auto remaining = static_cast<std::int64_t>(day_steps) - prefix.elapsed;
        if (remaining > 0) {
            actions.emplace_back(simulator::WaitAction{static_cast<core::Quantity>(remaining)});
        }
        result.push_back(std::move(actions));
    }
    return result;
}

std::vector<Prefix> prefixes_from_greedy(
    const PlannerInput& input, const PlannerResult& greedy) {
    std::vector<Prefix> result;
    result.reserve(input.daily.own_agents.size());
    for (std::size_t agent = 0; agent < input.daily.own_agents.size(); ++agent) {
        Prefix prefix;
        prefix.position = greedy.simulation.end_agents[agent].position;
        prefix.elapsed = greedy.agent_travel_steps[agent];
        prefix.travel = greedy.agent_travel_steps[agent];
        for (const auto& action : greedy.plan[agent]) {
            if (const auto* move = std::get_if<simulator::MoveAction>(&action)) {
                prefix.actions.emplace_back(*move);
            }
        }
        result.push_back(std::move(prefix));
    }
    return result;
}

std::vector<std::vector<TimelinePoint>> timeline_from_trace(
    const PlannerInput& input, const simulator::DaySimulationResult& simulation) {
    std::vector<std::vector<TimelinePoint>> result(input.daily.own_agents.size());
    for (std::size_t agent = 0; agent < input.daily.own_agents.size(); ++agent) {
        result[agent].push_back({0, input.daily.own_agents[agent].position, std::nullopt,
                                 input.daily.own_agents[agent].fuel, true});
    }
    for (const auto& event : simulation.trace) {
        if (!event.agent_index.has_value()) continue;
        const auto agent = *event.agent_index;
        result[agent].push_back({event.step,
            event.new_position.value_or(event.old_position.value_or(core::CellIndex{})),
            event.phase,
            event.new_fuel.value_or(event.old_fuel.value_or(0)),
            event.phase == simulator::SimulationPhase::ActionReservation});
    }
    return result;
}

}  // namespace

RefuelPlannerOutcome::RefuelPlannerOutcome(
    std::variant<RefuelPlannerResult, PlannerError> storage) : storage_(std::move(storage)) {}
RefuelPlannerOutcome RefuelPlannerOutcome::success(RefuelPlannerResult result) {
    return RefuelPlannerOutcome(std::move(result));
}
RefuelPlannerOutcome RefuelPlannerOutcome::failure(PlannerError error) {
    return RefuelPlannerOutcome(std::move(error));
}
bool RefuelPlannerOutcome::has_value() const noexcept {
    return std::holds_alternative<RefuelPlannerResult>(storage_);
}
RefuelPlannerOutcome::operator bool() const noexcept { return has_value(); }
const RefuelPlannerResult& RefuelPlannerOutcome::value() const& {
    return std::get<RefuelPlannerResult>(storage_);
}
RefuelPlannerResult&& RefuelPlannerOutcome::value() && {
    return std::get<RefuelPlannerResult>(std::move(storage_));
}
const PlannerError& RefuelPlannerOutcome::error() const& {
    return std::get<PlannerError>(storage_);
}

RefuelPlannerOutcome make_refuel_plan(
    const PlannerInput& input, const PlannerResult& greedy,
    const RefuelPlannerConfig& config,
    const std::chrono::steady_clock::time_point deadline, PlannerClock now) {
    const auto started = now();
    if (greedy.plan.size() != input.daily.own_agents.size()) {
        return RefuelPlannerOutcome::failure({PlannerErrorCode::InvalidInput,
                                               "greedy agent count mismatch"});
    }
    const auto day_steps = input.match.day_steps.at(static_cast<std::size_t>(input.daily.day));
    auto prefixes = prefixes_from_greedy(input, greedy);
    auto traced = simulator::simulate_day({input.match.map, input.match.spots,
        input.match.fuel_limit, day_steps, input.daily.own_agents, input.daily.traffic},
        greedy.plan, simulator::TraceMode::Enabled);
    if (!traced) {
        return RefuelPlannerOutcome::failure({PlannerErrorCode::BaselineSimulationFailed,
                                               traced.error().message});
    }
    Candidate best{prefixes, greedy.plan, std::move(traced).value(), greedy.score,
                   greedy.visited_spots, {}, {}};
    best.readiness = daily_readiness(input.match, input.daily, best.simulation, best.visits);
    std::vector<core::Quantity> assigned(input.match.spots.size(), 0);
    for (const auto& visits : best.visits)
        for (const auto spot : visits) ++assigned[spot];
    auto pathfinder_result = pathfinding::Pathfinder::create(input.match.map, input.daily.traffic);
    if (!pathfinder_result) {
        return RefuelPlannerOutcome::failure({PlannerErrorCode::InvalidInput,
                                               pathfinder_result.error().message});
    }
    auto pathfinder = std::move(pathfinder_result).value();
    std::size_t evaluated = 0, simulations = 1, accepted = 0, rendezvous_count = 0;
    std::size_t tie_group_count = 1;
    RefuelTermination termination = RefuelTermination::NoImprovingCandidate;

    while (true) {
        if (now() >= deadline) { termination = RefuelTermination::Deadline; break; }
        std::optional<Candidate> round_best;
        bool stopped = false;
        for (std::size_t patrol = 0; patrol < prefixes.size() && !stopped; ++patrol) {
            if (input.daily.own_agents[patrol].kind != core::AgentKind::Patrol
                || prefixes[patrol].refuels >= config.maximum_refuels_per_patrol) continue;
            for (std::size_t supply = 0; supply < prefixes.size() && !stopped; ++supply) {
                if (input.daily.own_agents[supply].kind != core::AgentKind::Supply) continue;
                auto supply_route_result = pathfinder.find_route(prefixes[supply].position,
                    prefixes[patrol].position, pathfinding::RouteObjective::Fastest);
                if (!supply_route_result || !supply_route_result.value().has_value()) continue;
                const auto supply_route = *supply_route_result.value();
                for (std::size_t spot = 0; spot < input.match.spots.size() && !stopped; ++spot) {
                    if (std::find(best.visits[patrol].begin(), best.visits[patrol].end(), spot)
                            != best.visits[patrol].end()
                        || assigned[spot] >= input.match.spots[spot].max_stock) continue;
                    for (const auto objective : {pathfinding::RouteObjective::Fastest,
                                                 pathfinding::RouteObjective::FuelEfficient}) {
                        if (now() >= deadline) { termination = RefuelTermination::Deadline; stopped = true; break; }
                        if (evaluated >= config.maximum_candidate_evaluations) {
                            termination = RefuelTermination::EvaluationLimit; stopped = true; break;
                        }
                        if (rendezvous_count >= config.maximum_rendezvous_candidates) {
                            termination = RefuelTermination::RendezvousLimit; stopped = true; break;
                        }
                        ++rendezvous_count;
                        auto patrol_route_result = pathfinder.find_route(prefixes[patrol].position,
                            input.match.spots[spot].position, objective);
                        if (!patrol_route_result || !patrol_route_result.value().has_value()) continue;
                        const auto patrol_route = *patrol_route_result.value();
                        if (patrol_route.cost.patrol_fuel > input.match.fuel_limit) continue;
                        auto candidate_prefixes = prefixes;
                        auto& patrol_prefix = candidate_prefixes[patrol];
                        auto& supply_prefix = candidate_prefixes[supply];
                        append_route(supply_prefix, supply_route);
                        const auto meeting_step = std::max<std::int64_t>(
                            1, std::max(patrol_prefix.elapsed, supply_prefix.elapsed));
                        append_wait(patrol_prefix, meeting_step - patrol_prefix.elapsed);
                        append_wait(supply_prefix, meeting_step - supply_prefix.elapsed);
                        if (meeting_step + patrol_route.cost.travel_steps > day_steps) continue;
                        append_route(patrol_prefix, patrol_route);
                        ++patrol_prefix.refuels;
                        auto plan = complete_plan(candidate_prefixes, day_steps);
                        ++evaluated;
                        auto simulation = simulator::simulate_day({input.match.map, input.match.spots,
                            input.match.fuel_limit, day_steps, input.daily.own_agents,
                            input.daily.traffic}, plan, simulator::TraceMode::Enabled);
                        ++simulations;
                        if (!simulation) continue;
                        std::optional<RendezvousEvent> meeting;
                        for (const auto& event : simulation.value().trace) {
                            if (event.kind == simulator::EventKind::Refueled
                                && event.agent_index == patrol && event.step == meeting_step
                                && event.new_position == prefixes[patrol].position) {
                                meeting = RendezvousEvent{patrol, supply, prefixes[patrol].position,
                                    event.step, event.old_fuel.value_or(0), event.new_fuel.value_or(0)};
                                break;
                            }
                        }
                        if (!meeting.has_value()) continue;
                        auto visits = best.visits;
                        visits[patrol].push_back(spot);
                        Candidate candidate{std::move(candidate_prefixes), std::move(plan),
                            std::move(simulation).value(), {}, std::move(visits), best.meetings, {}};
                        candidate.meetings.push_back(*meeting);
                        candidate.score = official_score(input.previous_progress, candidate.simulation);
                        candidate.readiness = daily_readiness(input.match, input.daily,
                                                              candidate.simulation, candidate.visits);
                        if (candidate.score == best.score) ++tie_group_count;
                        if (better_candidate(candidate, best)
                            && (!round_best || better_candidate(candidate, *round_best))) {
                            round_best = std::move(candidate);
                        }
                    }
                }
            }
        }
        if (!round_best) {
            if (!stopped) termination = RefuelTermination::NoImprovingCandidate;
            break;
        }
        best = std::move(*round_best);
        prefixes = best.prefixes;
        std::fill(assigned.begin(), assigned.end(), 0);
        for (const auto& visits : best.visits)
            for (const auto spot : visits) ++assigned[spot];
        ++accepted;
        termination = RefuelTermination::Completed;
    }

    std::vector<SupplySchedule> supplies;
    for (std::size_t agent = 0; agent < prefixes.size(); ++agent) {
        if (input.daily.own_agents[agent].kind != core::AgentKind::Supply) continue;
        SupplySchedule schedule{agent, prefixes[agent].travel, prefixes[agent].waits, {}};
        for (const auto& meeting : best.meetings) {
            if (meeting.supply_agent == agent) schedule.rendezvous.push_back(meeting);
        }
        supplies.push_back(std::move(schedule));
    }
    RefuelPlannerResult result{best.plan, best.simulation, greedy.score, best.score,
        best.readiness, tie_group_count, best.visits, std::move(supplies), best.meetings,
        timeline_from_trace(input, best.simulation), termination, evaluated, simulations,
        accepted, rendezvous_count,
        std::chrono::duration_cast<std::chrono::microseconds>(now() - started), config.seed};
    return RefuelPlannerOutcome::success(std::move(result));
}

}  // namespace hexa_udon::planner
