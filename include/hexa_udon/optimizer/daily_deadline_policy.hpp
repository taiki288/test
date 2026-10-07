#pragma once
#include "hexa_udon/optimizer/optimizer.hpp"
#include <nlohmann/json.hpp>

namespace hexa_udon::optimizer {
[[nodiscard]] std::optional<std::size_t> search_candidate_multiplier(std::size_t size);

struct DailyDeadlinePolicy {
    static constexpr const char* version = "daily-deadline-policy-v1";
    std::size_t size = 0;
    std::chrono::milliseconds baseline_max{0};
    std::chrono::milliseconds improvement_max{0};
    std::chrono::milliseconds reserve{0};
    std::chrono::milliseconds minimum_improvement{250};
    bool production_approved = false;
    [[nodiscard]] static std::optional<DailyDeadlinePolicy> for_size(std::size_t size);
    [[nodiscard]] static std::optional<DailyDeadlinePolicy> for_production_size(std::size_t size);
    // Offline measurement only: preserves the locked reserve/version and changes
    // the baseline cap under an explicit measurement identity.
    [[nodiscard]] static std::optional<DailyDeadlinePolicy> for_measurement(
        std::size_t size, std::chrono::milliseconds baseline,
        std::chrono::milliseconds improvement);
    [[nodiscard]] nlohmann::json identity() const;
};
// Wall time is sampled once. All subsequent scheduling uses the monotonic deadline.
[[nodiscard]] std::optional<std::chrono::steady_clock::time_point> observed_daily_deadline(
    core::UnixTimestamp ends_at, std::chrono::system_clock::time_point wall_now,
    std::chrono::steady_clock::time_point steady_now);
struct DailyDeadlineStages {
    std::function<planner::PlannerOutcome(const planner::PlannerInput&, const planner::PlannerConfig&,
        std::chrono::steady_clock::time_point, planner::PlannerClock)> greedy = planner::make_greedy_plan;
    std::function<planner::RefuelPlannerOutcome(const planner::PlannerInput&, const planner::PlannerResult&,
        const planner::RefuelPlannerConfig&, std::chrono::steady_clock::time_point, planner::PlannerClock)> refuel = planner::make_refuel_plan;
    std::function<OptimizerOutcome(const planner::PlannerInput&, const planner::PlannerResult&,
        const planner::RefuelPlannerResult&, const OptimizerConfig&,
        std::chrono::steady_clock::time_point, OptimizerClock)> improve = optimize;
    std::function<simulator::SimulationOutcome(const simulator::DaySimulationInput&,
        const simulator::DayActionPlan&, simulator::TraceMode)> simulate = [](const simulator::DaySimulationInput& input, const simulator::DayActionPlan& plan, simulator::TraceMode trace) { return simulator::simulate_day(input, plan, trace); };
    struct WorkerResult {
        std::optional<simulator::DayActionPlan> plan;
        std::optional<simulator::DaySimulationResult> simulation;
        nlohmann::json record;
        std::string reason = "worker-unavailable";
    };
    std::function<WorkerResult(const planner::PlannerInput&, const planner::PlannerResult&,
        const planner::RefuelPlannerResult&, const simulator::DaySimulationResult&,
        std::chrono::steady_clock::time_point, std::chrono::milliseconds,
        OptimizerClock)> worker;
};
// WorkerResult is a strict candidate handoff only. The App layer owns claim
// revalidation and the final OfficialScore/Readiness adoption decision.
struct DailyDeadlineResult {
    std::optional<simulator::DayActionPlan> plan;
    std::optional<simulator::DaySimulationResult> simulation;
    nlohmann::json record;
};
[[nodiscard]] DailyDeadlineResult run_daily_deadline_policy(
    const planner::PlannerInput& input, const DailyDeadlinePolicy& policy,
    std::optional<std::chrono::steady_clock::time_point> observed_deadline,
    const planner::PlannerConfig& greedy, const planner::RefuelPlannerConfig& refuel,
    const OptimizerConfig& improvement, OptimizerClock now = [] { return std::chrono::steady_clock::now(); },
    const DailyDeadlineStages& stages = {}, bool run_improvement = true,
    std::chrono::milliseconds worker_configured_timeout = std::chrono::milliseconds{0});
} // namespace hexa_udon::optimizer
