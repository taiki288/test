#pragma once

#include "hexa_udon/optimizer/daily_deadline_policy.hpp"
#include "hexa_udon/protocol/request_control.hpp"

#include <chrono>
#include <set>
#include <utility>
#include <vector>

namespace hexa_udon::test {

inline planner::PlannerInput make_input(
    core::MatchConfig& match, core::DailyState& daily, simulator::MatchProgress& progress,
    core::MapDefinition& map) {
    map = std::move(core::MapDefinition::create(16, 16,
        std::vector<core::Terrain>(256, core::Terrain::Plain)).value());
    match = core::MatchConfig{0, {60}, {100}, map, {},
                              std::vector<core::CellIndex>{{0}, {0}, {0}, {0}},
                              100, 1, 1, 2};
    daily = core::DailyState{100, 0,
        {{core::AgentKind::Patrol, {0}, 100},
         {core::AgentKind::Patrol, {0}, 100},
         {core::AgentKind::Patrol, {0}, 100},
         {core::AgentKind::Supply, {0}, 100}}, {}, {}};
    progress = simulator::MatchProgress{};
    return {match, daily, progress};
}

inline simulator::DaySimulationResult simulation(
    const std::vector<core::AgentState>& agents,
    std::set<core::Quantity> brands = {}, core::Quantity balls = 0) {
    simulator::DaySimulationResult result;
    result.end_agents = agents;
    result.acquisitions.resize(agents.size());
    result.distinct_brands = std::move(brands);
    result.total_balls = balls;
    return result;
}

inline planner::PlannerResult greedy_result(const simulator::DaySimulationResult& value) {
    planner::PlannerResult result;
    result.plan.resize(value.end_agents.size());
    result.simulation = value;
    result.termination = planner::PlannerTermination::Completed;
    return result;
}

inline planner::RefuelPlannerResult refuel_result(const simulator::DaySimulationResult& value) {
    planner::RefuelPlannerResult result;
    result.plan.resize(value.end_agents.size());
    result.simulation = value;
    result.visited_spots.resize(value.end_agents.size());
    result.termination = planner::RefuelTermination::Completed;
    return result;
}

inline optimizer::DailyDeadlineStages base_stages(
    const simulator::DaySimulationResult& baseline) {
    optimizer::DailyDeadlineStages stages;
    stages.greedy = [baseline](const planner::PlannerInput&, const planner::PlannerConfig&,
                               protocol::SteadyTime, planner::PlannerClock) {
        return planner::PlannerOutcome::success(greedy_result(baseline));
    };
    stages.refuel = [baseline](const planner::PlannerInput&, const planner::PlannerResult&,
                               const planner::RefuelPlannerConfig&, protocol::SteadyTime,
                               planner::PlannerClock) {
        return planner::RefuelPlannerOutcome::success(refuel_result(baseline));
    };
    stages.simulate = [baseline](const simulator::DaySimulationInput&,
                                 const simulator::DayActionPlan&, simulator::TraceMode) {
        return simulator::SimulationOutcome::success(baseline);
    };
    return stages;
}

inline optimizer::DailyDeadlinePolicy policy() {
    return optimizer::DailyDeadlinePolicy::for_measurement(
        16, std::chrono::milliseconds{1000}, std::chrono::milliseconds{10000}).value();
}

}  // namespace hexa_udon::test
