#include "hexa_udon/planner/prematch_type_selector.hpp"
#include "hexa_udon/core/type_candidates.hpp"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <utility>

namespace hexa_udon::planner {
namespace {

std::uint64_t fnv1a(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

InternalTieBreak tie_break_for(const core::DailyState& daily,
                               const simulator::DaySimulationResult& result,
                               const simulator::DayActionPlan& plan,
                               const TypeCandidate& candidate) {
    InternalTieBreak tie;
    for (std::size_t i = 0; i < result.end_agents.size(); ++i) {
        if (candidate.kinds[i] == core::AgentKind::Patrol) tie.patrol_fuel_remaining += result.end_agents[i].fuel;
        if (i >= plan.size()) continue;
        for (const auto& action : plan[i]) {
            if (std::holds_alternative<simulator::MoveAction>(action)) ++tie.total_edges;
        }
    }
    for (const auto& event : result.trace) {
        if (event.kind == simulator::EventKind::ActionStarted && event.action_value
            && *event.action_value >= 0 && event.action_duration) {
            tie.total_travel_steps += *event.action_duration;
        }
    }
    static_cast<void>(daily);
    tie.deterministic_order.reserve(candidate.kinds.size());
    for (const auto kind : candidate.kinds) tie.deterministic_order.push_back(core::to_int(kind));
    return tie;
}

simulator::DayActionPlan wait_plan(std::size_t count, core::Quantity steps) {
    simulator::DayActionPlan plan(count);
    for (auto& agent : plan) if (steps > 0) agent.emplace_back(simulator::WaitAction{steps});
    return plan;
}

std::optional<TypeEvaluation> evaluate_wait(
    const core::MatchConfig& match, const core::DailyState& daily,
    const simulator::MatchProgress& progress, const TypeCandidate& candidate,
    const std::string& explanation) {
    auto plan = wait_plan(candidate.kinds.size(), match.day_steps.front());
    const auto simulation = simulator::simulate_day(
        {match.map, match.spots, match.fuel_limit, match.day_steps.front(),
         daily.own_agents, daily.traffic}, plan);
    if (!simulation) return std::nullopt;
    TypeEvaluation result;
    result.candidate = candidate;
    result.plan = std::move(plan);
    result.simulation = simulation.value();
    result.score = official_score(progress, result.simulation);
    result.tie_break = tie_break_for(daily, result.simulation, result.plan, candidate);
    result.method = TypeEvaluationMethod::WaitFallback;
    result.termination = "fallback-verified";
    result.explanation = explanation;
    return result;
}

std::string positions_hash(const core::DailyState& daily) {
    std::string value;
    for (const auto& agent : daily.own_agents) {
        value += std::to_string(agent.position.value);
        value.push_back(';');
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << fnv1a(value);
    return out.str();
}

bool all_clear_day_zero(const core::MatchConfig& match, const core::DailyState& daily) {
    if (daily.day != 0) return false;
    const auto validation = core::validate(daily, match);
    if (!validation.empty()) return false;
    std::size_t road_count = 0;
    for (const auto terrain : match.map.cells()) if (terrain == core::Terrain::Road) ++road_count;
    if (road_count != daily.traffic.size()) return false;
    return std::all_of(daily.traffic.begin(), daily.traffic.end(), [](const auto& traffic) {
        return traffic.status == core::RoadStatus::Smooth;
    });
}

}  // namespace

bool better_type_evaluation(const TypeEvaluation& left, const TypeEvaluation& right) {
    if (better_official_score(left.score, right.score)) return true;
    if (better_official_score(right.score, left.score)) return false;
    if (better_internal_tie_break(left.tie_break, right.tie_break)) return true;
    if (better_internal_tie_break(right.tie_break, left.tie_break)) return false;
    return std::lexicographical_compare(
        left.candidate.kinds.begin(), left.candidate.kinds.end(),
        right.candidate.kinds.begin(), right.candidate.kinds.end(),
        [](core::AgentKind a, core::AgentKind b) { return core::to_int(a) < core::to_int(b); });
}

TypeSelectionOutcome::TypeSelectionOutcome(
    std::variant<PreMatchTypeSelectionResult, TypeSelectorError> value)
    : storage_(std::move(value)) {}
TypeSelectionOutcome TypeSelectionOutcome::success(PreMatchTypeSelectionResult result) {
    return TypeSelectionOutcome(std::move(result));
}
TypeSelectionOutcome TypeSelectionOutcome::failure(TypeSelectorError error) {
    return TypeSelectionOutcome(std::move(error));
}
TypeSelectionOutcome::operator bool() const noexcept {
    return std::holds_alternative<PreMatchTypeSelectionResult>(storage_);
}
const PreMatchTypeSelectionResult& TypeSelectionOutcome::value() const& {
    return std::get<PreMatchTypeSelectionResult>(storage_);
}
PreMatchTypeSelectionResult&& TypeSelectionOutcome::value() && {
    return std::get<PreMatchTypeSelectionResult>(std::move(storage_));
}
const TypeSelectorError& TypeSelectionOutcome::error() const& {
    return std::get<TypeSelectorError>(storage_);
}

std::vector<TypeCandidate> make_type_candidates(
    std::size_t agent_count, const PreMatchTypeSelectorConfig& config) {
    if (agent_count != 4 && agent_count != 5 && agent_count != 7) return {};
    if (config.minimum_supply_agents > config.maximum_supply_agents) return {};
    std::vector<TypeCandidate> candidates;
    if (!config.allowed_supply_counts.empty()) {
        auto allowed = config.allowed_supply_counts;
        std::sort(allowed.begin(), allowed.end());
        if (std::adjacent_find(allowed.begin(), allowed.end()) != allowed.end()
            || std::any_of(allowed.begin(), allowed.end(), [&](const auto count) {
                return count < config.minimum_supply_agents || count > config.maximum_supply_agents
                    || count > agent_count;
            })) return {};
        for (const auto supply : allowed)
            for (auto kinds : core::complete_type_arrays(agent_count, supply, supply))
                candidates.push_back(TypeCandidate{std::move(kinds)});
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            return left.kinds < right.kinds;
        });
    } else {
        // Preserve the existing production max=1 convention (one-supply only).
        if (config.maximum_supply_agents > 2) return {};
        const auto minimum = config.maximum_supply_agents == 1
            ? std::size_t{1} : config.minimum_supply_agents;
        for (auto kinds : core::complete_type_arrays(agent_count, minimum, config.maximum_supply_agents))
            candidates.push_back(TypeCandidate{std::move(kinds)});
    }
    if (candidates.size() > config.maximum_candidates) return {};
    return candidates;
}

bool validate_type_candidates(const std::vector<TypeCandidate>& candidates,
                              std::size_t agent_count) {
    if (agent_count == 0 || candidates.empty()) return false;
    std::vector<std::vector<std::int32_t>> seen;
    for (const auto& candidate : candidates) {
        if (candidate.kinds.size() != agent_count) return false;
        std::vector<std::int32_t> encoded;
        encoded.reserve(agent_count);
        for (const auto kind : candidate.kinds) {
            if (kind != core::AgentKind::Patrol && kind != core::AgentKind::Supply) return false;
            encoded.push_back(core::to_int(kind));
        }
        if (std::find(seen.begin(), seen.end(), encoded) != seen.end()) return false;
        seen.push_back(std::move(encoded));
    }
    return true;
}

TypeSelectionOutcome select_types_for_match(
    const core::MatchConfig& match, const core::DailyState& input_daily,
    const simulator::MatchProgress& previous_progress,
    const PreMatchTypeSelectorConfig& config,
    std::chrono::steady_clock::time_point deadline, TypeSelectorClock now) {
    const auto started = now();
    if (!core::validate(match).empty() || match.day_steps.empty()
        || input_daily.own_agents.size() != match.initial_agent_positions.size()) {
        return TypeSelectionOutcome::failure({TypeSelectorErrorCode::InvalidInput,
            "invalid match setting or initial agent count"});
    }
    if (config.maximum_candidates == 0
        || (config.allowed_supply_counts.empty() && config.maximum_supply_agents > 2)
        || (!config.allowed_supply_counts.empty() && config.maximum_supply_agents > 3)
        || config.minimum_supply_agents > config.maximum_supply_agents
        || config.greedy_candidate_limit == 0
        || config.refuel_candidate_limit == 0 || config.rendezvous_candidate_limit == 0
        || config.maximum_refuels_per_patrol == 0 || config.per_candidate_budget.count() <= 0
        || config.selector_budget.count() <= 0
        || config.minimum_refuel_budget_per_candidate.count() <= 0
        || config.minimum_optimizer_budget.count() < 0
        || config.optimizer_budget_per_candidate.count() < 0
        || config.safety_reserve.count() < 0 || config.optimizer_iterations == 0) {
        return TypeSelectionOutcome::failure({TypeSelectorErrorCode::InvalidInput,
            "selector limits and budgets must be within safe positive ranges"});
    }
    if (!all_clear_day_zero(match, input_daily)) {
        return TypeSelectionOutcome::failure({TypeSelectorErrorCode::InvalidDayZeroRoads,
            "selector accepts only day 0 with complete all-smooth road status; future snapshots are not accepted"});
    }
    for (std::size_t i = 0; i < input_daily.own_agents.size(); ++i) {
        if (input_daily.own_agents[i].position != match.initial_agent_positions[i]) {
            return TypeSelectionOutcome::failure({TypeSelectorErrorCode::InvalidInput,
                "Day0 state positions differ from the received setting's official agent order"});
        }
    }
    const auto candidates = make_type_candidates(input_daily.own_agents.size(), config);
    if (candidates.empty()) return TypeSelectionOutcome::failure(
        {TypeSelectorErrorCode::NoCandidates, "no bounded type candidates are configured"});

    PreMatchTypeSelectionResult result;
    result.seed = config.seed;
    result.initial_positions_hash = positions_hash(input_daily);
    result.total_candidates = candidates.size();
    const auto maximum_count = config.maximum_supply_agents;
    result.total_by_supply_count.assign(maximum_count + 1, 0);
    result.evaluated_by_supply_count.assign(maximum_count + 1, 0);
    for (const auto& candidate : candidates) {
        const auto supplies = static_cast<std::size_t>(std::count(
            candidate.kinds.begin(), candidate.kinds.end(), core::AgentKind::Supply));
        if (supplies < result.total_by_supply_count.size()) ++result.total_by_supply_count[supplies];
    }
    TypeCandidate all_patrol;
    all_patrol.kinds.assign(input_daily.own_agents.size(), core::AgentKind::Patrol);
    if (config.minimum_supply_agents > 0) all_patrol = candidates.front();
    result.selected = all_patrol;
    auto fallback_daily = input_daily;
    for (std::size_t i = 0; i < fallback_daily.own_agents.size(); ++i)
        fallback_daily.own_agents[i].kind = result.selected.kinds[i];
    auto fallback = evaluate_wait(match, fallback_daily, previous_progress, result.selected,
                                  "verified all-wait safety fallback respecting minimum supply");
    if (!fallback) return TypeSelectionOutcome::failure(
        {TypeSelectorErrorCode::InvalidInput, "all-wait fallback failed strict Simulator validation"});
    auto fallback_evaluation = std::move(*fallback);

    std::vector<std::optional<PlannerResult>> greedies(candidates.size());
    std::vector<std::optional<RefuelPlannerResult>> refuels(candidates.size());
    std::vector<std::size_t> evaluated_indices;
    const auto configured_deadline = std::min(deadline, started + config.selector_budget);
    const auto safe_deadline = configured_deadline - config.safety_reserve;
    const auto candidate_phase_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
        safe_deadline - now()) / static_cast<std::int64_t>(candidates.size());
    const auto fair_candidate_budget = std::min(config.per_candidate_budget,
                                                 candidate_phase_budget);
    const bool fair_refuel_pass_possible = fair_candidate_budget
        >= config.minimum_refuel_budget_per_candidate;
    if (!fair_refuel_pass_possible) {
        result.unevaluated_candidates = candidates.size();
        result.confidence_limited = true;
        result.termination = TypeSelectionTermination::Fallback;
        result.fallback_reason = "insufficient remaining time to give every candidate the minimum equal Refuel budget";
        fallback_evaluation.explanation = "verified wait fallback respecting minimum supply; candidate set not fairly evaluable";
        result.evaluations.push_back(std::move(fallback_evaluation));
        result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now() - started);
        return TypeSelectionOutcome::success(std::move(result));
    }
    bool candidate_budget_overrun = false;
    for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
        if (now() >= safe_deadline) break;
        if (fair_candidate_budget.count() < 10) break;
        auto daily = input_daily;
        for (std::size_t i = 0; i < daily.own_agents.size(); ++i)
            daily.own_agents[i].kind = candidates[candidate_index].kinds[i];
        const auto stop = std::min(safe_deadline, now() + fair_candidate_budget);
        const auto greedy = make_greedy_plan({match, daily, previous_progress},
            {config.greedy_candidate_limit, config.seed}, stop, now);
        if (now() > stop) { candidate_budget_overrun = true; break; }
        if (!greedy) {
            result.selection_warning = "greedy construction failed for one or more candidates";
            continue;
        }
        greedies[candidate_index] = greedy.value();
        TypeEvaluation evaluated;
        evaluated.candidate = candidates[candidate_index];
        const auto supply_count = static_cast<std::size_t>(std::count(
            evaluated.candidate.kinds.begin(), evaluated.candidate.kinds.end(), core::AgentKind::Supply));
        if (supply_count == 0) {
            evaluated.plan = greedy.value().plan;
            evaluated.simulation = greedy.value().simulation;
            evaluated.score = greedy.value().score;
            evaluated.method = TypeEvaluationMethod::Greedy;
            evaluated.termination = "greedy-day0-verified";
            evaluated.explanation = "zero-supply candidate evaluated with Greedy; Refuel phase omitted";
        } else {
            const auto refuel = make_refuel_plan({match, daily, previous_progress}, greedy.value(),
                {config.refuel_candidate_limit, config.rendezvous_candidate_limit,
                 config.maximum_refuels_per_patrol, config.seed}, stop, now);
            if (now() > stop) { candidate_budget_overrun = true; break; }
            if (!refuel) {
                result.selection_warning = "refuel planner failed for one or more candidates";
                continue;
            }
            refuels[candidate_index] = refuel.value();
            evaluated.plan = refuel.value().plan;
            evaluated.simulation = refuel.value().simulation;
            evaluated.score = refuel.value().score;
            evaluated.method = TypeEvaluationMethod::Refuel;
            evaluated.termination = refuel.value().termination == RefuelTermination::Deadline
                ? "deadline-verified" : "completed-verified";
            evaluated.explanation = "greedy-refuel Day0 plan verified by Simulator";
            evaluated.elapsed = refuel.value().elapsed;
        }
        evaluated.elapsed += greedy.value().elapsed;
        evaluated.tie_break = tie_break_for(daily, evaluated.simulation, evaluated.plan,
                                            evaluated.candidate);
        const auto existing = std::find_if(result.evaluations.begin(), result.evaluations.end(),
            [&](const auto& item) { return item.candidate == evaluated.candidate; });
        if (existing == result.evaluations.end()) result.evaluations.push_back(std::move(evaluated));
        else *existing = std::move(evaluated);
        evaluated_indices.push_back(candidate_index);
        if (supply_count < result.evaluated_by_supply_count.size())
            ++result.evaluated_by_supply_count[supply_count];
    }

    if (candidate_budget_overrun
        || (evaluated_indices.size() < candidates.size() && now() >= safe_deadline)) {
        // A non-interruptible planner exceeded its equal time slot. Discard the
        // prefix instead of comparing only candidates visited before the overrun.
        result.evaluations.clear();
        evaluated_indices.clear();
        std::fill(result.evaluated_by_supply_count.begin(), result.evaluated_by_supply_count.end(), 0);
        result.fallback_reason = "candidate exceeded its equal evaluation slot; discarded partial pass for fairness";
        fallback_evaluation.explanation = result.fallback_reason;
        result.evaluations.push_back(std::move(fallback_evaluation));
        result.unevaluated_candidates = candidates.size();
        result.confidence_limited = true;
        result.termination = TypeSelectionTermination::Fallback;
        result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now() - started);
        return TypeSelectionOutcome::success(std::move(result));
    }

    const auto optimizer_budget_total = config.optimizer_budget_per_candidate
        * static_cast<std::int64_t>(candidates.size());
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(safe_deadline - now());
    const bool optimizer_compatible_candidates = std::all_of(candidates.begin(), candidates.end(),
        [](const auto& candidate) {
            return std::find(candidate.kinds.begin(), candidate.kinds.end(), core::AgentKind::Supply)
                != candidate.kinds.end();
        });
    const bool optimize_all = optimizer_compatible_candidates
        && evaluated_indices.size() == candidates.size()
        && remaining >= optimizer_budget_total + config.safety_reserve
        && config.optimizer_budget_per_candidate >= config.minimum_optimizer_budget;
    if (optimize_all) {
        std::vector<std::optional<optimizer::OptimizerResult>> optimized_values;
        bool fair_optimizer_pass = true;
        for (const auto index : evaluated_indices) {
            if (now() >= safe_deadline) { fair_optimizer_pass = false; break; }
            auto daily = input_daily;
            for (std::size_t i = 0; i < daily.own_agents.size(); ++i)
                daily.own_agents[i].kind = candidates[index].kinds[i];
            optimizer::OptimizerConfig optimizer_config;
            optimizer_config.seed = config.seed;
            optimizer_config.maximum_iterations = config.optimizer_iterations;
            const auto stop = std::min(safe_deadline,
                                       now() + config.optimizer_budget_per_candidate);
            const auto optimized = optimizer::optimize({match, daily, previous_progress},
                *greedies[index], *refuels[index], optimizer_config, stop, now);
            if (!optimized || now() > stop) {
                fair_optimizer_pass = false;
                break;
            }
            optimized_values.emplace_back(optimized.value());
        }
        if (fair_optimizer_pass && optimized_values.size() == evaluated_indices.size()) {
            result.optimizer_status = "completed";
            for (std::size_t optimized_index = 0; optimized_index < evaluated_indices.size(); ++optimized_index) {
                const auto index = evaluated_indices[optimized_index];
                const auto& optimized = *optimized_values[optimized_index];
            auto& item = *std::find_if(result.evaluations.begin(), result.evaluations.end(),
                [&](const auto& value) { return value.candidate == candidates[index]; });
            if (better_official_score(optimized.score, item.score)
                || (optimized.score == item.score
                    && better_internal_tie_break(optimized.tie_break, item.tie_break))) {
                item.plan = optimized.plan;
                item.simulation = optimized.simulation;
                item.score = optimized.score;
                item.tie_break = optimized.tie_break;
                item.method = TypeEvaluationMethod::Optimized;
                item.termination = optimized.termination == optimizer::OptimizerTermination::Deadline
                    ? "optimizer-deadline-verified" : "optimized-verified";
                item.elapsed += optimized.elapsed;
                item.explanation = "optimized candidate; strict Simulator validation passed";
            }
            }
        } else {
            result.optimizer_phase_skipped_for_fairness = true;
            result.optimizer_status = "skipped-for-fairness";
        }
    } else {
        result.optimizer_phase_skipped_for_fairness = true;
        result.optimizer_status = "skipped-for-fairness";
    }

    const auto evaluated_candidate_count = evaluated_indices.size();
    result.evaluated_candidates = evaluated_candidate_count;
    result.unevaluated_candidates = candidates.size() - evaluated_candidate_count;
    result.confidence_limited = result.unevaluated_candidates > 0;
    result.termination = result.unevaluated_candidates == 0
        ? TypeSelectionTermination::Completed
        : now() >= safe_deadline ? TypeSelectionTermination::Deadline
        : TypeSelectionTermination::EvaluationFailure;
    if (result.unevaluated_candidates > 0 && result.selection_warning.empty())
        result.selection_warning = "candidates unevaluated; selection confidence is limited";

    const auto best = std::max_element(result.evaluations.begin(), result.evaluations.end(),
        [](const auto& a, const auto& b) { return better_type_evaluation(b, a); });
    if (best != result.evaluations.end()) {
        result.selected = best->candidate;
        result.selection_method = best->method == TypeEvaluationMethod::Optimized ? "optimized"
            : best->method == TypeEvaluationMethod::Refuel ? "greedy-refuel" : "greedy";
    }
    if (evaluated_candidate_count == 0) {
        result.termination = TypeSelectionTermination::Fallback;
        result.selection_method = "all-wait-fallback";
        result.optimizer_status = "not-started";
        result.optimizer_phase_skipped_for_fairness = false;
        if (result.fallback_reason.empty()) result.fallback_reason = "no candidate plan passed evaluation; returning verified wait fallback respecting minimum supply";
        fallback_evaluation.explanation = result.fallback_reason;
        result.evaluations.clear();
        result.evaluations.push_back(std::move(fallback_evaluation));
        result.selected = std::move(all_patrol);
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now() - started);
    return TypeSelectionOutcome::success(std::move(result));
}

}  // namespace hexa_udon::planner
