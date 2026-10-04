#pragma once

#include "hexa_udon/optimizer/optimizer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace hexa_udon::planner {

struct PreMatchTypeSelectorConfig {
    // 1 preserves the historical one-supply-only candidate set. Explicitly set 2
    // to compare zero, one, and two supply agents; set 0 to evaluate all patrol.
    std::size_t maximum_supply_agents = 1;
    std::size_t minimum_supply_agents = 0;
    // When present, this allowlist takes precedence over the min/max range.
    // It is used by explicitly approved non-contiguous profiles only.
    std::vector<std::size_t> allowed_supply_counts;
    std::size_t maximum_candidates = 35;
    std::size_t greedy_candidate_limit = 1000;
    std::size_t refuel_candidate_limit = 1000;
    std::size_t rendezvous_candidate_limit = 2000;
    std::size_t maximum_refuels_per_patrol = 1;
    std::chrono::milliseconds per_candidate_budget{500};
    std::chrono::milliseconds selector_budget{3000};
    std::chrono::milliseconds minimum_refuel_budget_per_candidate{20};
    std::chrono::milliseconds minimum_optimizer_budget{100};
    std::chrono::milliseconds optimizer_budget_per_candidate{150};
    std::chrono::milliseconds safety_reserve{1000};
    std::size_t optimizer_iterations = 300;
    std::uint64_t seed = 30013;
};

struct TypeCandidate {
    std::vector<core::AgentKind> kinds;
    [[nodiscard]] bool operator==(const TypeCandidate&) const = default;
};

enum class TypeEvaluationMethod { Greedy, Refuel, Optimized, WaitFallback };
enum class TypeSelectionTermination { Completed, Deadline, EvaluationFailure, Fallback };

struct TypeEvaluation {
    TypeCandidate candidate;
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    OfficialScore score;
    InternalTieBreak tie_break;
    TypeEvaluationMethod method = TypeEvaluationMethod::WaitFallback;
    std::string termination;
    std::chrono::microseconds elapsed{0};
    std::string explanation;
};

[[nodiscard]] bool better_type_evaluation(const TypeEvaluation& left,
                                          const TypeEvaluation& right);

struct PreMatchTypeSelectionResult {
    TypeCandidate selected;
    std::vector<TypeEvaluation> evaluations;
    std::size_t unevaluated_candidates = 0;
    std::size_t total_candidates = 0;
    std::size_t evaluated_candidates = 0;
    std::string selection_method = "all-wait-fallback";
    std::string optimizer_status = "not-started";
    std::string selection_warning;
    std::vector<std::size_t> evaluated_by_supply_count;
    std::vector<std::size_t> total_by_supply_count;
    TypeSelectionTermination termination = TypeSelectionTermination::Fallback;
    std::string fallback_reason;
    bool optimizer_phase_skipped_for_fairness = false;
    bool confidence_limited = false;
    std::uint64_t seed = 0;
    std::string initial_positions_hash;
    std::string candidate_set_hash;
    std::int64_t configured_budget_milliseconds = 0;
    std::int64_t effective_budget_milliseconds = 0;
    std::int64_t post_reserve_milliseconds = 0;
    std::int64_t started_remaining_milliseconds = 0;
    std::int64_t baseline_pass_remaining_milliseconds = 0;
    std::int64_t final_remaining_milliseconds = 0;
    std::int64_t candidate_enumeration_microseconds = 0;
    std::int64_t greedy_microseconds = 0;
    std::int64_t refuel_microseconds = 0;
    std::int64_t optimizer_microseconds = 0;
    bool strict_simulator_timing_available = false;
    bool second_pass_attempted = false;
    std::size_t second_pass_top_k = 0;
    std::string second_pass_reason = "not-configured-type-selector-plan-unit";
    std::chrono::microseconds elapsed{0};
};

enum class TypeSelectorErrorCode { InvalidInput, InvalidDayZeroRoads, NoCandidates };
struct TypeSelectorError { TypeSelectorErrorCode code; std::string message; };

class TypeSelectionOutcome {
public:
    [[nodiscard]] static TypeSelectionOutcome success(PreMatchTypeSelectionResult result);
    [[nodiscard]] static TypeSelectionOutcome failure(TypeSelectorError error);
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const PreMatchTypeSelectionResult& value() const&;
    [[nodiscard]] PreMatchTypeSelectionResult&& value() &&;
    [[nodiscard]] const TypeSelectorError& error() const&;
private:
    explicit TypeSelectionOutcome(std::variant<PreMatchTypeSelectionResult, TypeSelectorError> value);
    std::variant<PreMatchTypeSelectionResult, TypeSelectorError> storage_;
};

using TypeSelectorClock = std::function<std::chrono::steady_clock::time_point()>;

[[nodiscard]] std::vector<TypeCandidate> make_type_candidates(
    std::size_t agent_count, const PreMatchTypeSelectorConfig& config);
[[nodiscard]] bool validate_type_candidates(
    const std::vector<TypeCandidate>& candidates, std::size_t agent_count);

[[nodiscard]] TypeSelectionOutcome select_types_for_match(
    const core::MatchConfig& match,
    const core::DailyState& day_zero,
    const simulator::MatchProgress& previous_progress,
    const PreMatchTypeSelectorConfig& config,
    std::chrono::steady_clock::time_point deadline,
    TypeSelectorClock now = [] { return std::chrono::steady_clock::now(); });

}  // namespace hexa_udon::planner
