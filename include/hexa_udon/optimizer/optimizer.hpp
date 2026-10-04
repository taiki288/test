#pragma once

#include "hexa_udon/planner/refuel_planner.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

namespace hexa_udon::optimizer {

enum class Neighborhood : std::uint8_t {
    SwapWithin,
    RelocateWithin,
    AddSpot,
    AddUncollectedBrand,
    RemoveSpot,
    ReplaceSameBrand,
    MoveBetweenPatrols,
    SwapBetweenPatrols,
    ReverseSubsequence,
    SetFastest,
    SetFuelEfficient,
    MoveRendezvousCell,
    AdjustRendezvousWait,
    ChangeSupply,
    SwapSupplyTasks,
    MoveTaskBetweenSupplies,
    RemoveRendezvous,
    AddRendezvous,
    Count,
};

struct PatrolRoutePlan {
    std::size_t agent_index = 0;
    std::vector<std::size_t> spot_indices;
    std::vector<pathfinding::RouteObjective> objectives;
};

struct RendezvousTask {
    std::size_t patrol_agent = 0;
    std::size_t supply_agent = 0;
    core::CellIndex cell{};
    std::int32_t wait_adjustment = 0;
};

struct StructuredSolution {
    std::vector<PatrolRoutePlan> patrol_routes;
    std::vector<RendezvousTask> rendezvous;
};

struct ScoreBounds {
    std::uint64_t total_unique_brands = 0;
    std::uint64_t cumulative_daily_unique_brands = 0;
    std::uint64_t total_bowls = 0;
};

enum class OrdinalError { NegativeScore, OutOfBounds, Overflow };

struct OrdinalFailure { OrdinalError code; std::string message; };

class OrdinalResult {
public:
    static OrdinalResult success(std::uint64_t value);
    static OrdinalResult failure(OrdinalFailure error);
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::uint64_t value() const;
    [[nodiscard]] const OrdinalFailure& error() const;
private:
    explicit OrdinalResult(std::variant<std::uint64_t, OrdinalFailure> storage);
    std::variant<std::uint64_t, OrdinalFailure> storage_;
};

[[nodiscard]] OrdinalResult score_ordinal(
    const planner::OfficialScore& score, const ScoreBounds& bounds);

struct NeighborhoodStatistics {
    std::size_t generated = 0;
    std::size_t prefiltered = 0;
    std::size_t simulated = 0;
    std::size_t valid = 0;
    std::size_t accepted = 0;
    std::size_t improved = 0;
};

struct CandidateDiagnostic {
    std::string neighborhood_kind;
    std::string fallback_reason;
    std::int64_t acquired_brand_count = 0;
    std::int64_t newly_acquired_brand_count = 0;
    std::int64_t uncollected_brand_count = 0;
    planner::OfficialScore official_score_delta;
    planner::DailyReadiness daily_readiness_delta;
    bool accepted = false;
    std::string rejection_reason;
    std::string candidate_hash;
    std::string action_hash;
    std::string plan_hash;
};

enum class OptimizerTermination { Completed, Deadline, IterationLimit, InvalidLimit, Fallback };
enum class CoolingSchedule { Geometric, Linear };

struct OptimizerConfig {
    std::uint64_t seed = 30013;
    std::size_t maximum_iterations = 10000;
    double initial_temperature = 8.0;
    double final_temperature = 0.05;
    CoolingSchedule cooling = CoolingSchedule::Geometric;
    std::array<double, static_cast<std::size_t>(Neighborhood::Count)> neighborhood_weights{};
    std::size_t maximum_consecutive_invalid = 250;
    bool reheat = true;
    bool prefer_daily_readiness_on_tie = false;
};

struct OptimizerResult {
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    planner::OfficialScore initial_score;
    planner::OfficialScore score;
    planner::InternalTieBreak tie_break;
    planner::DailyReadiness initial_readiness;
    planner::DailyReadiness readiness;
    StructuredSolution solution;
    std::vector<planner::SupplySchedule> supply_schedules;
    std::vector<planner::RendezvousEvent> rendezvous;
    std::uint64_t seed = 0;
    std::size_t iterations = 0;
    std::size_t generated_candidates = 0;
    std::size_t prefiltered_candidates = 0;
    std::size_t simulator_runs = 0;
    std::size_t valid_candidates = 0;
    std::size_t invalid_candidates = 0;
    std::size_t accepted_candidates = 0;
    std::size_t improvements = 0;
    std::array<NeighborhoodStatistics, static_cast<std::size_t>(Neighborhood::Count)> neighborhoods{};
    std::vector<CandidateDiagnostic> candidate_diagnostics;
    OptimizerTermination termination = OptimizerTermination::Fallback;
    std::chrono::microseconds elapsed{0};
    std::string diagnostic;
};

enum class DailyImprovementDecisionReason {
    BaselineRetained,
    OfficialScoreImproved,
    ReadinessTieBreak,
};

struct DailyImprovementDecision {
    bool adopt = false;
    DailyImprovementDecisionReason reason = DailyImprovementDecisionReason::BaselineRetained;
};

[[nodiscard]] DailyImprovementDecision evaluate_daily_improvement(
    const planner::OfficialScore& baseline_score,
    const planner::DailyReadiness& baseline_readiness,
    const planner::OfficialScore& candidate_score,
    const planner::DailyReadiness& candidate_readiness) noexcept;

class OptimizerOutcome {
public:
    static OptimizerOutcome success(OptimizerResult result);
    static OptimizerOutcome failure(planner::PlannerError error);
    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const OptimizerResult& value() const&;
    [[nodiscard]] OptimizerResult&& value() &&;
    [[nodiscard]] const planner::PlannerError& error() const&;
private:
    explicit OptimizerOutcome(std::variant<OptimizerResult, planner::PlannerError> storage);
    std::variant<OptimizerResult, planner::PlannerError> storage_;
};

using OptimizerClock = std::function<std::chrono::steady_clock::time_point()>;

[[nodiscard]] StructuredSolution solution_from_baseline(
    const planner::PlannerInput& input,
    const planner::PlannerResult& greedy,
    const planner::RefuelPlannerResult& refuel);

[[nodiscard]] bool apply_neighborhood(
    StructuredSolution& solution, Neighborhood neighborhood,
    const planner::PlannerInput& input, std::mt19937_64& random,
    std::string* mutation_kind = nullptr);

[[nodiscard]] OptimizerOutcome optimize(
    const planner::PlannerInput& input,
    const planner::PlannerResult& greedy,
    const planner::RefuelPlannerResult& baseline,
    const OptimizerConfig& config,
    std::chrono::steady_clock::time_point deadline,
    OptimizerClock now = [] { return std::chrono::steady_clock::now(); });

}  // namespace hexa_udon::optimizer
