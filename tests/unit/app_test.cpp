#include "test_fixture.hpp"
#include "hexa_udon/core/hex_distance.hpp"

#include <cassert>

int main() {
    using namespace hexa_udon;
    const auto distance_map = std::move(core::MapDefinition::create(8, 8,
        std::vector<core::Terrain>(64, core::Terrain::Plain)).value());
    if (core::hex_distance(distance_map, {0}, {0}) != 0) return 1;
    // Even-row offset: both lower cells are adjacent to (row=0,col=0).
    if (core::hex_distance(distance_map, {0}, {8}) != 1) return 1;
    if (core::hex_distance(distance_map, {0}, {9}) != 1) return 1;
    // Odd-row offset: both upper cells are adjacent to (row=1,col=1).
    if (core::hex_distance(distance_map, {8}, {0}) != 1) return 1;
    if (core::hex_distance(distance_map, {9}, {0}) != 1) return 1;
    if (core::hex_distance(distance_map, {9}, {1}) != 1) return 1;
    if (core::hex_distance(distance_map, {0}, {16}) != 2) return 1;
    if (core::hex_distance(distance_map, {0}, {27}) != 4) return 1;
    const optimizer::ScoreBounds phase_bounds{10, 30, 100};
    if (optimizer::optimization_phase({9, 30, 100}, phase_bounds)
        != optimizer::OptimizationPhase::TotalUniqueBrands) return 1;
    if (optimizer::optimization_phase({10, 29, 100}, phase_bounds)
        != optimizer::OptimizationPhase::DailyUniqueBrands) return 1;
    if (optimizer::optimization_phase({10, 30, 99}, phase_bounds)
        != optimizer::OptimizationPhase::TotalBowls) return 1;

    core::MapDefinition map = std::move(core::MapDefinition::create(16, 16,
        std::vector<core::Terrain>(256, core::Terrain::Plain)).value());
    core::MatchConfig match{0, {60}, {100}, map, {},
                            std::vector<core::CellIndex>{{0}, {0}, {0}, {0}},
                            100, 1, 1, 2};
    core::DailyState daily{};
    simulator::MatchProgress progress{};
    const auto input = test::make_input(match, daily, progress, map);
    const auto base_sim = test::simulation(daily.own_agents);
    auto stages = test::base_stages(base_sim);
    stages.worker = [base_sim](const planner::PlannerInput&, const planner::PlannerResult&,
                               const planner::RefuelPlannerResult&, const simulator::DaySimulationResult&,
                               protocol::SteadyTime, std::chrono::milliseconds, optimizer::OptimizerClock) {
        optimizer::DailyDeadlineStages::WorkerResult result;
        result.plan = simulator::DayActionPlan(base_sim.end_agents.size());
        result.simulation = base_sim;
        result.record = {{"score", {1, 0, 1}}, {"readiness", {1, 1, 1, 1}},
                         {"workerObservations", {{{"workerIndex", 0}, {"workerCount", 2},
                            {"requestIdDigest", "digest"}, {"strictRevalidation", "passed"},
                            {"adoption", "worker"}}}}};
        result.reason = "worker-candidate-adopted";
        return result;
    };
    stages.improve = [](const planner::PlannerInput&, const planner::PlannerResult&,
                        const planner::RefuelPlannerResult&, const optimizer::OptimizerConfig&,
                        protocol::SteadyTime, optimizer::OptimizerClock) {
        return optimizer::OptimizerOutcome::failure({planner::PlannerErrorCode::InvalidInput, "fixture no main candidate"});
    };
    const auto result = optimizer::run_daily_deadline_policy(input, test::policy(),
        protocol::SteadyTime{} + std::chrono::seconds{2}, {4, 30013}, {4, 4, 2, 30013}, {},
        [] { return protocol::SteadyTime{}; }, stages, true, std::chrono::milliseconds{1000});
    if (result.record.value("candidateSource", "") != "worker") return 1;
    if (result.record.value("adoptionReason", "") != "official-score-improved") return 1;
    if (!result.record.value("workerCandidateReturned", false)) return 1;
}
