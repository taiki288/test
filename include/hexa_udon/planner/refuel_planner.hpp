#pragma once

#include "hexa_udon/planner/greedy_planner.hpp"

namespace hexa_udon::planner {

struct TimelinePoint {
    core::Quantity step = 0;
    core::CellIndex position{};
    std::optional<simulator::SimulationPhase> phase;
    core::Quantity fuel = 0;
    bool can_start_action = false;
};

struct RendezvousEvent {
    std::size_t patrol_agent = 0;
    std::size_t supply_agent = 0;
    core::CellIndex cell{};
    core::Quantity step = 0;
    core::Quantity fuel_before = 0;
    core::Quantity fuel_after = 0;
};

struct SupplySchedule {
    std::size_t agent_index = 0;
    std::int64_t travel_steps = 0;
    std::int64_t wait_steps = 0;
    std::vector<RendezvousEvent> rendezvous;
};

enum class RefuelTermination {
    Completed,
    NoImprovingCandidate,
    Deadline,
    EvaluationLimit,
    RendezvousLimit,
    Fallback,
};

struct RefuelPlannerConfig {
    std::size_t maximum_candidate_evaluations = 2000;
    std::size_t maximum_rendezvous_candidates = 4000;
    std::size_t maximum_refuels_per_patrol = 2;
    std::uint64_t seed = 30013;
};

struct RefuelPlannerResult {
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    OfficialScore greedy_score;
    OfficialScore score;
    DailyReadiness daily_readiness;
    std::size_t official_score_tie_group_count = 1;
    std::vector<std::vector<std::size_t>> visited_spots;
    std::vector<SupplySchedule> supply_schedules;
    std::vector<RendezvousEvent> rendezvous;
    std::vector<std::vector<TimelinePoint>> timeline;
    RefuelTermination termination = RefuelTermination::Fallback;
    std::size_t evaluated_candidates = 0;
    std::size_t simulator_runs = 0;
    std::size_t accepted_improvements = 0;
    std::size_t rendezvous_candidates = 0;
    std::chrono::microseconds elapsed{0};
    std::uint64_t seed = 0;
};

class RefuelPlannerOutcome {
public:
    [[nodiscard]] static RefuelPlannerOutcome success(RefuelPlannerResult result);
    [[nodiscard]] static RefuelPlannerOutcome failure(PlannerError error);
    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const RefuelPlannerResult& value() const&;
    [[nodiscard]] RefuelPlannerResult&& value() &&;
    [[nodiscard]] const PlannerError& error() const&;
private:
    explicit RefuelPlannerOutcome(std::variant<RefuelPlannerResult, PlannerError> storage);
    std::variant<RefuelPlannerResult, PlannerError> storage_;
};

[[nodiscard]] RefuelPlannerOutcome make_refuel_plan(
    const PlannerInput& input,
    const PlannerResult& greedy,
    const RefuelPlannerConfig& config,
    std::chrono::steady_clock::time_point deadline,
    PlannerClock now = [] { return std::chrono::steady_clock::now(); });

}  // namespace hexa_udon::planner
