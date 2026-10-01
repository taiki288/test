#pragma once

#include "hexa_udon/core/models.hpp"
#include "hexa_udon/pathfinding/pathfinder.hpp"
#include "hexa_udon/simulator/simulator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace hexa_udon::planner {

struct OfficialScore {
    std::int64_t total_unique_brands = 0;
    std::int64_t cumulative_daily_unique_brands = 0;
    std::int64_t total_bowls = 0;

    [[nodiscard]] bool operator==(const OfficialScore&) const = default;
};

[[nodiscard]] bool better_official_score(
    const OfficialScore& left, const OfficialScore& right) noexcept;

struct InternalTieBreak {
    std::int64_t patrol_fuel_remaining = 0;
    std::int64_t total_travel_steps = 0;
    std::int64_t total_edges = 0;
    std::vector<std::int32_t> deterministic_order;
};

// Deterministic, non-official comparison used only after OfficialScore is equal.
// It uses the current day's end state and never predicts future traffic.
struct DailyReadiness {
    std::int64_t uncollected_spot_reachability = 0;
    std::int64_t fuel_reserve = 0;
    std::int64_t patrol_dispersion = 0;
    std::int64_t rendezvous_readiness = 0;
    std::vector<std::int32_t> deterministic_order;
};

[[nodiscard]] bool better_daily_readiness(
    const DailyReadiness& left, const DailyReadiness& right) noexcept;

[[nodiscard]] DailyReadiness daily_readiness(
    const core::MatchConfig& match,
    const core::DailyState& daily,
    const simulator::DaySimulationResult& simulation,
    const std::vector<std::vector<std::size_t>>& visited_spots);

[[nodiscard]] bool better_internal_tie_break(
    const InternalTieBreak& left, const InternalTieBreak& right) noexcept;

enum class PlannerTermination {
    Completed,
    NoImprovingCandidate,
    Deadline,
    EvaluationLimit,
    Fallback,
};

struct PlannerConfig {
    std::size_t maximum_candidate_evaluations = 2000;
    std::uint64_t seed = 30013;
};

struct PlannerInput {
    const core::MatchConfig& match;
    const core::DailyState& daily;
    const simulator::MatchProgress& previous_progress;
};

struct PlannerResult {
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    OfficialScore baseline_score;
    OfficialScore score;
    InternalTieBreak tie_break;
    DailyReadiness daily_readiness;
    std::size_t official_score_tie_group_count = 1;
    std::vector<std::vector<std::size_t>> visited_spots;
    std::vector<std::vector<pathfinding::RouteObjective>> route_objectives;
    std::vector<std::int64_t> agent_travel_steps;
    PlannerTermination termination = PlannerTermination::Completed;
    std::size_t evaluated_candidates = 0;
    std::size_t accepted_improvements = 0;
    std::size_t simulator_runs = 0;
    std::chrono::microseconds elapsed{0};
    std::uint64_t seed = 0;
    std::string diagnostic;
};

enum class PlannerErrorCode {
    InvalidInput,
    BaselineSimulationFailed,
};

struct PlannerError {
    PlannerErrorCode code;
    std::string message;
};

class PlannerOutcome {
public:
    [[nodiscard]] static PlannerOutcome success(PlannerResult result);
    [[nodiscard]] static PlannerOutcome failure(PlannerError error);
    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const PlannerResult& value() const&;
    [[nodiscard]] PlannerResult&& value() &&;
    [[nodiscard]] const PlannerError& error() const&;

private:
    explicit PlannerOutcome(std::variant<PlannerResult, PlannerError> storage);
    std::variant<PlannerResult, PlannerError> storage_;
};

using PlannerClock = std::function<std::chrono::steady_clock::time_point()>;

[[nodiscard]] OfficialScore official_score(
    const simulator::MatchProgress& previous,
    const simulator::DaySimulationResult& day);

[[nodiscard]] PlannerOutcome make_greedy_plan(
    const PlannerInput& input,
    const PlannerConfig& config,
    std::chrono::steady_clock::time_point deadline,
    PlannerClock now = [] { return std::chrono::steady_clock::now(); });

}  // namespace hexa_udon::planner
