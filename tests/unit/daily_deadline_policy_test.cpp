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
    stages.worker = [](const planner::PlannerInput&, const planner::PlannerResult&,
                       const planner::RefuelPlannerResult&, const simulator::DaySimulationResult&,
                       protocol::SteadyTime, std::chrono::milliseconds, optimizer::OptimizerClock) {
        optimizer::DailyDeadlineStages::WorkerResult result;
        result.reason = "worker-timeout";
        result.record = {{"workerObservations", {{{"workerIndex", 1}, {"workerCount", 2},
            {"termination", "timeout"}, {"adoption", "baseline-retained"}}}}};
        return result;
    };
    stages.improve = [](const planner::PlannerInput&, const planner::PlannerResult&,
                        const planner::RefuelPlannerResult&, const optimizer::OptimizerConfig&,
                        protocol::SteadyTime, optimizer::OptimizerClock) {
        return optimizer::OptimizerOutcome::failure({planner::PlannerErrorCode::InvalidInput, "fixture strict failure"});
    };
    const auto result = optimizer::run_daily_deadline_policy(input, test::policy(),
        protocol::SteadyTime{} + std::chrono::seconds{2}, {4, 30013}, {4, 4, 2, 30013}, {},
        [] { return protocol::SteadyTime{}; }, stages, true, std::chrono::milliseconds{1000});
    if (result.record.value("candidateSource", "") != "baseline") return 1;
    if (result.record.value("adoptionReason", "") != "baseline-retained") return 1;
    if (result.record.value("replyGraceMs", 0) != 100) return 1;
}
