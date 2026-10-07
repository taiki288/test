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
    core::MatchConfig compound_match{0, {60}, {20}, distance_map,
        {{1, {1}, 1}, {2, {2}, 1}, {3, {3}, 1}}, {{0}}, 20, 1, 1, 2};
    core::DailyState compound_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 20}}, {}, {}};
    simulator::MatchProgress compound_progress;
    optimizer::StructuredSolution compound_solution{{{0, {0, 1}, {}}}, {}};
    std::mt19937_64 compound_random(30013);
    if (!optimizer::apply_neighborhood(compound_solution,
            optimizer::Neighborhood::ReplaceAndRelocate,
            {compound_match, compound_daily, compound_progress}, compound_random)) return 1;
    if (compound_solution.patrol_routes[0].spot_indices.size() != 2
        || std::find(compound_solution.patrol_routes[0].spot_indices.begin(),
                     compound_solution.patrol_routes[0].spot_indices.end(), 2)
            == compound_solution.patrol_routes[0].spot_indices.end()) return 1;
    core::MatchConfig replacement_match{0, {60}, {20}, distance_map,
        {{1, {1}, 1}, {1, {2}, 2}, {1, {20}, 4}}, {{0}}, 20, 1, 1, 2};
    core::DailyState replacement_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 20}}, {}, {}};
    simulator::MatchProgress replacement_progress;
    optimizer::StructuredSolution replacement_solution{{{0, {0}, {}}}, {}};
    std::mt19937_64 replacement_random(30013);
    if (!optimizer::apply_neighborhood(replacement_solution,
            optimizer::Neighborhood::ReplaceSameBrand,
            {replacement_match, replacement_daily, replacement_progress}, replacement_random)) return 1;
    if (replacement_solution.patrol_routes[0].spot_indices != std::vector<std::size_t>{2}) return 1;
    core::MatchConfig meeting_match{0, {60}, {20}, distance_map,
        {{1, {9}, 1}}, {{0}, {1}}, 20, 1, 1, 2};
    core::DailyState meeting_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 20},
         {core::AgentKind::Supply, {1}, 20}}, {}, {}};
    simulator::MatchProgress meeting_progress;
    optimizer::StructuredSolution meeting_solution{{{0, {0}, {}}}, {}};
    std::mt19937_64 meeting_random(30013);
    if (!optimizer::apply_neighborhood(meeting_solution,
            optimizer::Neighborhood::AddRendezvous,
            {meeting_match, meeting_daily, meeting_progress}, meeting_random)) return 1;
    if (meeting_solution.rendezvous.size() != 1
        || (meeting_solution.rendezvous[0].cell != core::CellIndex{0}
            && meeting_solution.rendezvous[0].cell != core::CellIndex{9})) return 1;
    core::MatchConfig readiness_match{0, {60}, {20}, distance_map,
        {{1, {0}, 2}}, {{0}, {1}}, 20, 1, 1, 2};
    core::DailyState readiness_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 20},
         {core::AgentKind::Patrol, {1}, 20}}, {}, {}};
    const auto readiness_sim = test::simulation(readiness_daily.own_agents);
    const auto readiness = planner::daily_readiness(
        readiness_match, readiness_daily, readiness_sim, {{0}, {0}});
    if (readiness.uncollected_spot_reachability != 2) return 1;

    auto insertion_map = std::move(core::MapDefinition::create(8, 8,
        std::vector<core::Terrain>(64, core::Terrain::Plain)).value());
    core::MatchConfig insertion_match{0, {60}, {6}, insertion_map,
        {{1, {8}, 1}, {2, {10}, 1}}, {{0}}, 20, 1, 1, 2};
    core::DailyState insertion_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 20}}, {}, {}};
    simulator::MatchProgress insertion_progress;
    insertion_progress.acquired_brands.insert(1);
    const auto insertion_plan = planner::make_greedy_plan(
        {insertion_match, insertion_daily, insertion_progress}, {40, 30013},
        std::chrono::steady_clock::time_point::max());
    if (!insertion_plan || insertion_plan.value().simulation.total_balls != 2) return 1;
    if (insertion_plan.value().visited_spots[0] != std::vector<std::size_t>({0, 1})) return 1;
    auto stock_map = std::move(core::MapDefinition::create(2, 2,
        std::vector<core::Terrain>(4, core::Terrain::Plain)).value());
    core::MatchConfig stock_match{0, {60}, {2}, stock_map,
        {{7, {1}, 2}}, {{0}, {0}}, 10, 1, 1, 2};
    core::DailyState stock_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 10},
         {core::AgentKind::Patrol, {0}, 10}}, {}, {}};
    simulator::MatchProgress stock_progress;
    const auto stock_plan = planner::make_greedy_plan(
        {stock_match, stock_daily, stock_progress}, {20, 30013},
        std::chrono::steady_clock::time_point::max());
    if (!stock_plan || stock_plan.value().simulation.total_balls != 2) return 1;
    if (stock_plan.value().visited_spots[0] != std::vector<std::size_t>{0}
        || stock_plan.value().visited_spots[1] != std::vector<std::size_t>{0}) return 1;

    core::MatchConfig value_match{0, {60}, {20}, distance_map,
        {{1, {1}, 1}, {2, {2}, 4}}, {{0}}, 20, 1, 1, 2};
    core::DailyState value_daily{100, 0,
        {{core::AgentKind::Patrol, {0}, 20}}, {}, {}};
    simulator::MatchProgress value_progress;
    optimizer::StructuredSolution value_solution{{{0, {}, {}}}, {}};
    std::mt19937_64 value_random(30013);
    if (!optimizer::apply_neighborhood(value_solution, optimizer::Neighborhood::AddSpot,
            {value_match, value_daily, value_progress}, value_random)) return 1;
    if (value_solution.patrol_routes[0].spot_indices != std::vector<std::size_t>{1}) return 1;

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
