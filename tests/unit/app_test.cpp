#include "test_fixture.hpp"

#include <cassert>

int main() {
    using namespace hexa_udon;
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
