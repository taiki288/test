#include "hexa_udon/app/auto_client.hpp"

#include "hexa_udon/planner/refuel_planner.hpp"
#include "hexa_udon/planner/prematch_type_selector.hpp"
#include "hexa_udon/optimizer/optimizer.hpp"
#include "hexa_udon/optimizer/daily_deadline_policy.hpp"
#include "hexa_udon/app/production_profile.hpp"
#include "hexa_udon/protocol/json_codec.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

namespace hexa_udon::app {
namespace {

bool retryable(protocol::ErrorCode code) {
    using protocol::ErrorCode;
    return code == ErrorCode::AccessTime || code == ErrorCode::Http5xx ||
           code == ErrorCode::DnsFailure || code == ErrorCode::ConnectionRefused ||
           code == ErrorCode::ConnectionTimeout || code == ErrorCode::TransferTimeout ||
           code == ErrorCode::Disconnected;
}

std::chrono::system_clock::time_point unix_time(core::UnixTimestamp value) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{value}};
}

core::DailyState make_setting_day_zero(const core::MatchConfig& setting) {
    std::vector<core::AgentState> agents;
    agents.reserve(setting.initial_agent_positions.size());
    for (const auto position : setting.initial_agent_positions)
        agents.push_back({core::AgentKind::Patrol, position, setting.fuel_limit});
    std::vector<core::TrafficState> traffic;
    for (std::int32_t cell = 0; cell < setting.map.cell_count(); ++cell) {
        const core::CellIndex position{cell};
        if (setting.map.terrain_at(position) == core::Terrain::Road)
            traffic.push_back({position, core::RoadStatus::Smooth});
    }
    return {0, 0, std::move(agents), {}, std::move(traffic)};
}

std::string stable_hash(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string canonical_json_hash(const nlohmann::json& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value.dump()) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string type_identity(const std::vector<core::AgentKind>& kinds) {
    std::string encoded;
    for (const auto kind : kinds) encoded += std::to_string(core::to_int(kind));
    return stable_hash(encoded);
}

std::string daily_snapshot_identity(const core::MatchConfig& config,
                                    const core::DailyState& daily) {
    std::ostringstream encoded;
    encoded << daily.day << '|';
    for (std::int32_t cell = 0; cell < config.map.cell_count(); ++cell) {
        const auto terrain = config.map.terrain_at(core::CellIndex{cell});
        encoded << (terrain.has_value() ? static_cast<int>(*terrain) : -1) << ',';
    }
    for (const auto& traffic : daily.traffic)
        encoded << traffic.position.value << ':' << core::to_int(traffic.status) << ',';
    return stable_hash(encoded.str());
}

std::string agent_state_identity(const core::DailyState& daily) {
    std::ostringstream encoded;
    for (const auto& agent : daily.own_agents)
        encoded << core::to_int(agent.kind) << ':' << agent.position.value << ':' << agent.fuel << ',';
    return stable_hash(encoded.str());
}

std::string configured_spot_identity(const core::MatchConfig& config) {
    std::ostringstream encoded;
    for (const auto& spot : config.spots)
        encoded << spot.brand << ':' << spot.position.value << ':' << spot.max_stock << ',';
    return stable_hash(encoded.str());
}

nlohmann::json canonical_planner_input(const core::MatchConfig& config,
                                       const core::DailyState& daily,
                                       const simulator::MatchProgress& progress,
                                       const std::vector<core::AgentKind>& kinds,
                                       std::uint64_t seed, std::int64_t worker_budget_ms) {
    nlohmann::json map = {{"height", config.map.height()}, {"width", config.map.width()},
                          {"cells", nlohmann::json::array()}};
    for (std::int32_t cell = 0; cell < config.map.cell_count(); ++cell)
        map["cells"].push_back(core::to_int(*config.map.terrain_at({cell})));
    nlohmann::json spots = nlohmann::json::array();
    for (const auto& spot : config.spots)
        spots.push_back({{"brand", spot.brand}, {"position", spot.position.value}, {"stock", spot.max_stock}});
    nlohmann::json initial = nlohmann::json::array();
    for (const auto position : config.initial_agent_positions) initial.push_back(position.value);
    nlohmann::json agents = nlohmann::json::array();
    for (const auto& agent : daily.own_agents)
        agents.push_back({{"kind", core::to_int(agent.kind)}, {"position", agent.position.value}, {"fuel", agent.fuel}});
    nlohmann::json traffic = nlohmann::json::array();
    for (const auto& road : daily.traffic)
        traffic.push_back({{"position", road.position.value}, {"status", core::to_int(road.status)}});
    nlohmann::json type_values = nlohmann::json::array();
    for (const auto kind : kinds) type_values.push_back(core::to_int(kind));
    nlohmann::json acquired = nlohmann::json::array();
    for (const auto brand : progress.acquired_brands) acquired.push_back(brand);
    return {{"map", map}, {"startsAt", config.starts_at}, {"daySeconds", config.day_seconds},
            {"daySteps", config.day_steps}, {"spots", spots}, {"initialPositions", initial},
            {"fuelLimit", config.fuel_limit}, {"players", config.players},
            {"busyThreshold", config.busy_threshold}, {"jammedThreshold", config.jammed_threshold},
            {"daily", {{"endsAt", daily.ends_at}, {"day", daily.day}, {"agents", agents}, {"traffic", traffic}}},
            {"progress", {{"acquiredBrands", acquired}, {"totalBalls", progress.total_balls},
                           {"dailyDistinctBrandCounts", progress.daily_distinct_brand_counts}}},
            {"types", type_values}, {"plannerSeed", seed}, {"workerBudgetMs", worker_budget_ms}};
}

void add_daily_metadata(session::PlannerSubmissionMetadata& metadata,
                        const core::MatchConfig& config, const core::DailyState& daily,
                        const std::vector<core::AgentKind>& kinds,
                        const simulator::DaySimulationResult& simulation,
                        const std::chrono::milliseconds reserve,
                        const planner::DailyReadiness& readiness,
                        const std::string& reason) {
    metadata.match_identity = stable_hash(std::to_string(config.map.height()) + "x" +
                                          std::to_string(config.map.width()) + ":" +
                                          std::to_string(config.initial_agent_positions.size()));
    metadata.type_identity = type_identity(kinds);
    metadata.snapshot_identity = daily_snapshot_identity(config, daily);
    metadata.agent_state_identity = agent_state_identity(daily);
    std::ostringstream next_state;
    for (const auto& agent : simulation.end_agents)
        next_state << core::to_int(agent.kind) << ':' << agent.position.value << ':' << agent.fuel << ',';
    metadata.next_state_identity = stable_hash(next_state.str());
    std::ostringstream spots;
    for (const auto stock : simulation.remaining_stock) spots << stock << ',';
    metadata.spot_inventory_identity = stable_hash(spots.str());
    metadata.safety_reserve_milliseconds = reserve.count();
    metadata.daily_readiness = {readiness.uncollected_spot_reachability,
                                readiness.fuel_reserve, readiness.patrol_dispersion,
                                readiness.rendezvous_readiness};
    metadata.selection_reason = reason;
}

}  // namespace

protocol::SteadyTime SystemAppClock::now() const { return std::chrono::steady_clock::now(); }
std::chrono::system_clock::time_point SystemAppClock::wall_now() const {
    return std::chrono::system_clock::now();
}
void SystemAppClock::wait_until(protocol::SteadyTime time) { std::this_thread::sleep_until(time); }

protocol::Result<std::vector<core::AgentKind>> round_preset(std::size_t agent_count) {
    std::size_t supply_count = 0;
    if (agent_count == 4 || agent_count == 5) supply_count = 1;
    else if (agent_count == 7) supply_count = 2;
    else {
        return protocol::Result<std::vector<core::AgentKind>>::failure(
            {protocol::ErrorCode::CoreValidation,
             "no temporary round preset for this agent count; specify --types"});
    }
    std::vector<core::AgentKind> result(agent_count, core::AgentKind::Patrol);
    std::fill(result.end() - static_cast<std::ptrdiff_t>(supply_count), result.end(),
              core::AgentKind::Supply);
    return protocol::Result<std::vector<core::AgentKind>>::success(std::move(result));
}

protocol::Result<std::vector<core::AgentKind>> parse_kind_list(const std::string& text) {
    std::vector<core::AgentKind> result;
    std::istringstream input(text);
    std::string part;
    while (std::getline(input, part, ',')) {
        std::int32_t value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) {
            return protocol::Result<std::vector<core::AgentKind>>::failure(
                {protocol::ErrorCode::InvalidSchema, "type list must contain only 0 or 1"});
        }
        const auto kind = core::agent_kind_from_int(value);
        if (!kind) {
            return protocol::Result<std::vector<core::AgentKind>>::failure(
                {protocol::ErrorCode::InvalidSchema, "type list must contain only 0 or 1"});
        }
        result.push_back(*kind);
    }
    if (result.empty()) {
        return protocol::Result<std::vector<core::AgentKind>>::failure(
            {protocol::ErrorCode::InvalidSchema, "type list must not be empty"});
    }
    return protocol::Result<std::vector<core::AgentKind>>::success(std::move(result));
}

AutoCompetitionClient::AutoCompetitionClient(
    protocol::ProconApiClient& api, AutoClientConfig config, AppClock& clock,
    std::function<bool()> stop_requested, std::ostream& output,
    protocol::OperationLogger* logger, const optimizer::DailyDeadlineStages* deadline_stages)
    : api_(api), config_(std::move(config)), clock_(clock),
      stop_requested_(std::move(stop_requested)), output_(output),
      logger_(logger == nullptr ? &null_logger_ : logger), deadline_stages_(deadline_stages) {}

protocol::Result<core::MatchConfig> AutoCompetitionClient::fetch_setting(
    const std::string& phase, std::optional<protocol::SteadyTime> deadline) {
    auto backoff = std::chrono::milliseconds{500};
    std::size_t failures = 0;
    while (!stop_requested_()) {
        auto result = api_.get_setting(deadline, phase);
        if (result) return result;
        if (!retryable(result.error().code) || ++failures >= config_.maximum_get_attempts) return result;
        const auto wait_duration = result.error().retry_after_ms
            ? std::chrono::milliseconds{*result.error().retry_after_ms} : backoff;
        const auto target = clock_.now() + wait_duration;
        if (deadline && target > *deadline) {
            return protocol::Result<core::MatchConfig>::failure(
                {protocol::ErrorCode::DeadlineExceeded, "setting retry exceeds deadline"});
        }
        clock_.wait_until(target);
        backoff = std::min(backoff * 2, std::chrono::milliseconds{4000});
    }
    return protocol::Result<core::MatchConfig>::failure(
        {protocol::ErrorCode::Conflict, "stop requested"});
}

protocol::Result<core::DailyState> AutoCompetitionClient::fetch_state(
    const core::MatchConfig& config, std::optional<core::Quantity> current_day,
    std::chrono::system_clock::time_point wall_deadline) {
    auto backoff = std::chrono::milliseconds{500};
    std::size_t failures = 0;
    while (!stop_requested_() && clock_.wall_now() < wall_deadline) {
        auto result = api_.get_state(config, std::nullopt, "daily-state");
        if (result) {
            failures = 0;
            backoff = std::chrono::milliseconds{500};
            if (!current_day || result.value().day > *current_day) return result;
            if (result.value().day < *current_day) {
                return protocol::Result<core::DailyState>::failure(
                    {protocol::ErrorCode::Conflict, "server day moved backwards"});
            }
            const auto target = clock_.now() + backoff;
            if (clock_.wall_now() + backoff > wall_deadline) break;
            clock_.wait_until(target);
            backoff = std::min(backoff * 2, std::chrono::milliseconds{4000});
            continue;
        }
        if (result.error().code == protocol::ErrorCode::Auth || !retryable(result.error().code)) {
            return result;
        }
        if (result.error().code != protocol::ErrorCode::AccessTime &&
            ++failures >= config_.maximum_get_attempts) {
            return result;
        }
        const auto wait_duration = result.error().retry_after_ms
            ? std::chrono::milliseconds{*result.error().retry_after_ms} : backoff;
        const auto target = clock_.now() + wait_duration;
        if (clock_.wall_now() + wait_duration > wall_deadline) break;
        clock_.wait_until(target);
        backoff = std::min(backoff * 2, std::chrono::milliseconds{4000});
    }
    return protocol::Result<core::DailyState>::failure(
        {protocol::ErrorCode::DeadlineExceeded, "state polling deadline reached"});
}

bool AutoCompetitionClient::has_unknown_submission(const session::SessionSnapshot& snapshot) const {
    return snapshot.agent_kinds_unknown ||
           std::any_of(snapshot.submissions.begin(), snapshot.submissions.end(), [](const auto& item) {
        // Recovery is keyed to the persisted tri-state outcome, not to a
        // legacy classification name. A null value means POST started but its
        // outcome is not safely known.
        return !item.submission_attempted.has_value();
           });
}

void AutoCompetitionClient::print_kinds(const std::vector<core::AgentKind>& kinds) {
    output_ << "types=[";
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        if (index) output_ << ',';
        output_ << core::to_int(kinds[index]);
    }
    output_ << "] (official agent index order)\n";
}

void AutoCompetitionClient::print_day_summary(
    const core::MatchConfig& config, const core::DailyState& daily,
    const session::SubmissionRecord& record, const simulator::MatchProgress& progress) {
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        unix_time(daily.ends_at) - clock_.wall_now()).count();
    output_ << "day=" << daily.day << '/' << config.day_steps.size() - 1
            << " endsAt=" << daily.ends_at << " remainingSeconds=" << std::max<std::int64_t>(0, remaining)
            << " safetyMarginSeconds=" << config_.safety_margin.count() << '\n'
            << "actions=omitted localId=" << record.local_id;
    if (record.revision) output_ << " revision=" << *record.revision;
    output_ << " predictedBrands=" << record.simulation.brands.size()
            << " predictedBalls=" << record.simulation.total_balls
            << " matchBrands=" << progress.acquired_brands.size()
            << " matchBalls=" << progress.total_balls << '\n';
    if (record.planner) {
        const auto& planner = *record.planner;
        output_ << "  planningInput day=" << daily.day
                << " matchHash=" << planner.match_identity
                << " typeHash=" << planner.type_identity
                << " snapshotHash=" << planner.snapshot_identity
                << " agentStateHash=" << planner.agent_state_identity
                << " spotInventoryHash=" << planner.spot_inventory_identity
                << " nextStateHash=" << planner.next_state_identity
                << " readiness=" << planner.daily_readiness[0] << ','
                << planner.daily_readiness[1] << ',' << planner.daily_readiness[2]
                << ',' << planner.daily_readiness[3]
                << " officialScoreTieGroup=" << planner.official_score_tie_group_count
                << " reserveMs=" << planner.safety_reserve_milliseconds
                << " reason=" << planner.selection_reason << '\n';
    }
    for (std::size_t index = 0; index < record.simulation.end_agents.size(); ++index) {
        const auto& agent = record.simulation.end_agents[index];
        output_ << "  agent=" << index << " endPos=" << agent.position.value
                << " endFuel=" << agent.fuel << '\n';
    }
}

RunResult AutoCompetitionClient::run() {
    output_ << "mode=" << (config_.mode == RunMode::Execute ? "EXECUTE" : "DRY-RUN")
            << " transport=configured" << '\n';
    if (stop_requested_()) return {RunStatus::Stopped, "stop requested before startup"};
    std::error_code directory_error;
    std::filesystem::create_directories(config_.state_directory, directory_error);
    if (directory_error) return {RunStatus::Failed, "cannot create state directory"};

    std::optional<SessionDirectoryLock> lock;
    if (config_.mode == RunMode::Execute) {
        auto acquired = SessionDirectoryLock::acquire(config_.state_directory);
        if (!acquired) return {RunStatus::Failed, acquired.error().message};
        lock = acquired.take();
    }

    auto setting = fetch_setting("registration");
    if (!setting) return {stop_requested_() ? RunStatus::Stopped : RunStatus::Failed,
                          setting.error().message};
    output_ << "session=setting-received startsAt=" << setting.value().starts_at << '\n';
    if (config_.profile_set_version) {
        try {
            apply_v2_profile_set(config_, setting.value(), config_.profile_set_directory,
                                 *config_.profile_set_version);
        } catch (const std::exception& exception) {
            return {RunStatus::Failed, std::string{"profile-set dispatch rejected: "} + exception.what()};
        }
        output_ << "profile-dispatch=auto-v2 selected=" << config_.profile_id
                << " version=" << config_.profile_version
                << " size=" << config_.required_map_height << 'x' << config_.required_map_width
                << " agents=" << config_.required_agent_count << '\n';
    }
    if (config_.require_profile_target) {
        if (static_cast<std::size_t>(setting.value().map.height()) != config_.required_map_height
            || static_cast<std::size_t>(setting.value().map.width()) != config_.required_map_width
            || setting.value().initial_agent_positions.size() != config_.required_agent_count) {
            if (config_.profile_id == "16x16-one-supply-v1")
                return {RunStatus::Failed, "16x16 profile requires a 16x16 map and four agents"};
            return {RunStatus::Failed, "profile target size or agent count mismatch"};
        }
        if (config_.type_selector != TypeSelectorMode::Prematch) {
            return {RunStatus::Failed, "profile selector policy mismatch"};
        }
        std::size_t candidate_count = 0;
        if (!config_.type_selector_allowed_supply_counts.empty()) {
            for (const auto supply : config_.type_selector_allowed_supply_counts) {
                if (supply <= config_.required_agent_count) {
                    auto combinations = std::size_t{1};
                    for (std::size_t i = 1; i <= supply; ++i)
                        combinations = combinations * (config_.required_agent_count - supply + i) / i;
                    candidate_count += combinations;
                }
            }
        } else {
            candidate_count = config_.type_selector_min_supply == config_.type_selector_max_supply
                ? (config_.type_selector_min_supply == 1 ? config_.required_agent_count
                   : config_.type_selector_min_supply == 2
                       ? (config_.required_agent_count * (config_.required_agent_count - 1)) / 2 : 1)
                : 0;
        }
        output_ << "profile=" << config_.profile_id << " version=" << config_.profile_version << " type-selector=prematch minSupply="
                << config_.type_selector_min_supply << " maxSupply=" << config_.type_selector_max_supply
                << " selectorCandidates=" << candidate_count
                << " allowedSupplyCounts=";
        if (config_.type_selector_allowed_supply_counts.empty()) output_ << "range";
        else { output_ << "["; for (std::size_t i=0;i<config_.type_selector_allowed_supply_counts.size();++i) { if(i) output_ << ","; output_ << config_.type_selector_allowed_supply_counts[i]; } output_ << "]"; }
        output_ << " boundary=setting-derived-Day0 planner=greedy-refuel"
                   " selectorMs=" << config_.type_selector_budget.count()
                << " greedyMs=" << config_.planner_budget.count()
                << " refuelMs=" << config_.refuel_budget.count()
                << " seed=" << config_.planner_seed
                << " dailyPlanner=" << (config_.daily_deadline_policy ? "daily-improvement" : "greedy-refuel") << '\n';
    }
    const auto state_path = config_.state_directory / "session.json";
    session::SessionController initial(api_, setting.value(), nullptr, logger_);
    if (std::filesystem::exists(state_path)) {
        auto restored = initial.restore(state_path);
        if (!restored) return {RunStatus::RecoveryRequired, restored.error().message};
        if (has_unknown_submission(initial.snapshot())) {
            protocol::OperationLogEntry recovery_log;
            recovery_log.level = protocol::OperationLogEntry::Level::Warning;
            recovery_log.timestamp_utc = protocol::utc_timestamp();
            recovery_log.operation = "recovery-required";
            recovery_log.phase = "recovery";
            recovery_log.result = "saved POST outcome is unknown";
            recovery_log.response_classification = "unknown-post-outcome";
            recovery_log.submission_attempted = std::nullopt;
            logger_->write(recovery_log);
            return {RunStatus::RecoveryRequired, "saved session contains an unknown POST outcome"};
        }
    }
    if (!initial.bind_production_policy(config_.production_policy_identity))
        return {RunStatus::RecoveryRequired, "production profile/policy identity mismatch; recover with the original profile"};
    std::vector<core::AgentKind> selected_kinds;
    std::optional<session::TypeSelectionMetadata> selection_metadata;
    bool selected_by_profile_selector = false;
    if (initial.snapshot().submitted_agent_kinds) {
        selected_kinds = *initial.snapshot().submitted_agent_kinds;
        if (config_.explicit_kinds && *config_.explicit_kinds != selected_kinds) {
            return {RunStatus::RecoveryRequired,
                    "requested types differ from the saved official-order types"};
        }
        output_ << "type-selection=restored; selector will not run or resubmit\n";
    } else if (config_.explicit_kinds) {
        selected_kinds = *config_.explicit_kinds;
        output_ << "type-selection=explicit --types\n";
    } else if (config_.type_selector == TypeSelectorMode::Prematch) {
        if (setting.value().starts_at != 0) {
            return {RunStatus::Failed, "prematch type selector is only allowed before startsAt is fixed"};
        }
        if (config_.type_selector_budget.count() <= 0 || config_.type_submission_reserve.count() <= 0) {
            return {RunStatus::Failed, "type selector and POST reserve budgets must be positive"};
        }
        output_ << "type-selection=PREMATCH initial-state=setting-derived-Day0 "
                   "(configured positions/fuel; all roads smooth)\n";
        const auto initial_state = make_setting_day_zero(setting.value());
        const auto selector_started = clock_.now();
        const auto selector_deadline = selector_started + config_.type_selector_budget;
        planner::PreMatchTypeSelectorConfig selector_config;
        selector_config.seed = config_.planner_seed;
        selector_config.maximum_supply_agents = config_.type_selector_max_supply;
        selector_config.minimum_supply_agents = config_.type_selector_min_supply;
        selector_config.allowed_supply_counts = config_.type_selector_allowed_supply_counts;
        selector_config.maximum_candidates = config_.type_selector_max_candidates;
        selector_config.selector_budget = config_.type_selector_budget;
        selector_config.per_candidate_budget = config_.type_selector_budget;
        selector_config.minimum_refuel_budget_per_candidate = std::chrono::milliseconds{20};
        selector_config.safety_reserve = std::chrono::milliseconds{100};
        selector_config.optimizer_budget_per_candidate = std::chrono::milliseconds{150};
        const auto selection = planner::select_types_for_match(
            setting.value(), initial_state, {}, selector_config,
            selector_deadline, [&] { return clock_.now(); });
        if (!selection) return {RunStatus::Failed,
            "prematch type selection failed: " + selection.error().message};
        const auto& chosen = selection.value();
        selected_by_profile_selector = true;
        selected_kinds = chosen.selected.kinds;
        session::TypeSelectionMetadata metadata;
        metadata.seed = chosen.seed;
        metadata.initial_positions_hash = chosen.initial_positions_hash;
        for (const auto kind : chosen.selected.kinds) metadata.selected_kinds.push_back(core::to_int(kind));
        const auto chosen_score = std::find_if(chosen.evaluations.begin(), chosen.evaluations.end(),
            [&](const auto& item) { return item.candidate == chosen.selected; });
        if (chosen_score != chosen.evaluations.end()) {
            metadata.selected_official_score = {chosen_score->score.total_unique_brands,
                chosen_score->score.cumulative_daily_unique_brands, chosen_score->score.total_bowls};
            metadata.selected_method = chosen_score->method == planner::TypeEvaluationMethod::Optimized
                ? "optimized" : chosen_score->method == planner::TypeEvaluationMethod::Refuel
                ? "greedy-refuel" : chosen_score->method == planner::TypeEvaluationMethod::Greedy
                ? "greedy" : "all-wait-fallback";
        }
        metadata.maximum_supply_agents = config_.type_selector_max_supply;
        metadata.minimum_supply_agents = config_.type_selector_min_supply;
        metadata.allowed_supply_counts = config_.type_selector_allowed_supply_counts;
        metadata.total_candidates = chosen.total_candidates;
        metadata.total_by_supply_count = chosen.total_by_supply_count;
        metadata.evaluated_by_supply_count = chosen.evaluated_by_supply_count;
        metadata.budget_milliseconds = config_.type_selector_budget.count();
        metadata.post_reserve_milliseconds = config_.type_submission_reserve.count();
        metadata.unevaluated_candidates = chosen.unevaluated_candidates;
        metadata.confidence_limited = chosen.confidence_limited;
        metadata.termination = chosen.termination == planner::TypeSelectionTermination::Completed
            ? "completed" : chosen.termination == planner::TypeSelectionTermination::Deadline
            ? "deadline" : chosen.termination == planner::TypeSelectionTermination::EvaluationFailure
            ? "evaluation-failure" : "fallback";
        metadata.fallback_reason = chosen.fallback_reason;
        metadata.optimizer_status = chosen.optimizer_status;
        metadata.selection_warning = chosen.selection_warning;
        metadata.evaluated_candidates = chosen.evaluated_candidates;
        metadata.candidate_set_hash = chosen.candidate_set_hash;
        metadata.configured_budget_milliseconds = chosen.configured_budget_milliseconds;
        metadata.effective_budget_milliseconds = chosen.effective_budget_milliseconds;
        metadata.started_remaining_milliseconds = chosen.started_remaining_milliseconds;
        metadata.baseline_pass_remaining_milliseconds = chosen.baseline_pass_remaining_milliseconds;
        metadata.final_remaining_milliseconds = chosen.final_remaining_milliseconds;
        metadata.candidate_enumeration_microseconds = chosen.candidate_enumeration_microseconds;
        metadata.greedy_microseconds = chosen.greedy_microseconds;
        metadata.refuel_microseconds = chosen.refuel_microseconds;
        metadata.optimizer_microseconds = chosen.optimizer_microseconds;
        metadata.strict_simulator_timing_available = chosen.strict_simulator_timing_available;
        metadata.second_pass_attempted = chosen.second_pass_attempted;
        metadata.second_pass_top_k = chosen.second_pass_top_k;
        metadata.second_pass_reason = chosen.second_pass_reason;
        for (const auto& item : chosen.evaluations) {
            session::TypeCandidateRecord record;
            for (const auto kind : item.candidate.kinds) record.kinds.push_back(core::to_int(kind));
            record.official_score = {item.score.total_unique_brands,
                item.score.cumulative_daily_unique_brands, item.score.total_bowls};
            record.internal_tie_break = {item.tie_break.patrol_fuel_remaining,
                item.tie_break.total_travel_steps, item.tie_break.total_edges};
            record.elapsed_microseconds = item.elapsed.count();
            record.evaluation_method = item.method == planner::TypeEvaluationMethod::Optimized
                ? "optimized" : item.method == planner::TypeEvaluationMethod::Refuel
                ? "greedy-refuel" : item.method == planner::TypeEvaluationMethod::Greedy
                ? "greedy" : "all-wait-fallback";
            record.termination = item.termination;
            record.explanation = item.explanation;
            metadata.candidates.push_back(std::move(record));
        }
        selection_metadata = std::move(metadata);
        std::size_t evaluated_count = 0;
        for (const auto count : chosen.evaluated_by_supply_count) evaluated_count += count;
        const auto selected_evaluation = std::find_if(chosen.evaluations.begin(), chosen.evaluations.end(),
            [&](const auto& item) { return item.candidate == chosen.selected; });
        output_ << "type-selector seed=" << chosen.seed << " initialPositionsHash="
                << chosen.initial_positions_hash << " evaluatedCandidates=" << evaluated_count
                << " candidates=" << chosen.total_candidates
                << " maxSupply=" << config_.type_selector_max_supply
                << " unevaluated=" << chosen.unevaluated_candidates
                << " confidenceLimited=" << (chosen.confidence_limited ? "yes" : "no")
                << " budgetMs=" << config_.type_selector_budget.count()
                << " postReserveMs=" << config_.type_submission_reserve.count()
                << " elapsedUs=" << chosen.elapsed.count()
                << " effectiveBudgetMs=" << chosen.effective_budget_milliseconds
                << " startRemainingMs=" << chosen.started_remaining_milliseconds
                << " baselineRemainingMs=" << chosen.baseline_pass_remaining_milliseconds
                << " finalRemainingMs=" << chosen.final_remaining_milliseconds
                << " candidateSetHash=" << chosen.candidate_set_hash
                << " stageUs=enum:" << chosen.candidate_enumeration_microseconds
                << ",greedy:" << chosen.greedy_microseconds
                << ",refuel:" << chosen.refuel_microseconds
                << ",optimizer:" << chosen.optimizer_microseconds
                << ",simulator:unavailable"
                << " secondPass=" << (chosen.second_pass_attempted ? "yes" : "no")
                << " secondPassReason=" << chosen.second_pass_reason
                << " termination=" << selection_metadata->termination << '\n';
        output_ << "type-selector supply-counts=";
        for (std::size_t supplies = 0; supplies < chosen.total_by_supply_count.size(); ++supplies) {
            if (supplies) output_ << ',';
            output_ << supplies << ':' << chosen.evaluated_by_supply_count[supplies]
                    << '/' << chosen.total_by_supply_count[supplies];
        }
        output_ << " selected=[";
        for (std::size_t i = 0; i < chosen.selected.kinds.size(); ++i) {
            if (i) output_ << ',';
            output_ << core::to_int(chosen.selected.kinds[i]);
        }
        output_ << "] minSupply=" << config_.type_selector_min_supply << " budgetMs=" << config_.type_selector_budget.count()
                << " selectionMethod=" << chosen.selection_method
                << " optimizerStatus=" << chosen.optimizer_status << '\n';
        if (!chosen.fallback_reason.empty())
            output_ << "type-selector fallback=" << chosen.fallback_reason << '\n';
        if (selected_evaluation != chosen.evaluations.end()) {
            output_ << "type-selector-selected score=" << selected_evaluation->score.total_unique_brands
                    << ',' << selected_evaluation->score.cumulative_daily_unique_brands
                    << ',' << selected_evaluation->score.total_bowls
                    << " method=" << (selected_evaluation->method == planner::TypeEvaluationMethod::Optimized
                        ? "optimized" : selected_evaluation->method == planner::TypeEvaluationMethod::Refuel
                        ? "greedy-refuel" : selected_evaluation->method == planner::TypeEvaluationMethod::Greedy
                        ? "greedy" : "all-wait-fallback") << '\n';
        }
        for (const auto& item : chosen.evaluations) {
            output_ << "  candidate types=[";
            for (std::size_t i = 0; i < item.candidate.kinds.size(); ++i) {
                if (i) output_ << ',';
                output_ << core::to_int(item.candidate.kinds[i]);
            }
            output_ << "] score=" << item.score.total_unique_brands << ','
                    << item.score.cumulative_daily_unique_brands << ',' << item.score.total_bowls
                    << " method=" << (item.method == planner::TypeEvaluationMethod::Optimized
                        ? "optimized" : item.method == planner::TypeEvaluationMethod::Refuel
                        ? "greedy-refuel" : item.method == planner::TypeEvaluationMethod::Greedy
                        ? "greedy" : "wait-fallback")
                    << " termination=" << item.termination << '\n';
        }
        if (!chosen.selection_warning.empty())
            output_ << "type-selector-warning=" << chosen.selection_warning << '\n';
        protocol::OperationLogEntry selector_log;
        selector_log.timestamp_utc = protocol::utc_timestamp();
        selector_log.operation = "prematch-type-selection";
        selector_log.method = "LOCAL";
        selector_log.path = "Day0";
        selector_log.result = "types=";
        for (std::size_t i = 0; i < selected_kinds.size(); ++i) {
            if (i) selector_log.result.push_back(',');
            selector_log.result += std::to_string(core::to_int(selected_kinds[i]));
        }
        selector_log.result += ";evaluated=" + std::to_string(evaluated_count)
            + ";unevaluated=" + std::to_string(chosen.unevaluated_candidates)
            + ";candidateCount=" + std::to_string(chosen.total_candidates)
            + ";maxSupply=" + std::to_string(config_.type_selector_max_supply)
            + ";minSupply=" + std::to_string(config_.type_selector_min_supply)
            + ";evaluatedBySupply=";
        for (std::size_t supplies = 0; supplies < chosen.evaluated_by_supply_count.size(); ++supplies) {
            if (supplies) selector_log.result.push_back(',');
            selector_log.result += std::to_string(supplies) + ":"
                + std::to_string(chosen.evaluated_by_supply_count[supplies]);
        }
        if (selected_evaluation != chosen.evaluations.end()) {
            selector_log.result += ";score=" + std::to_string(selected_evaluation->score.total_unique_brands)
                + "," + std::to_string(selected_evaluation->score.cumulative_daily_unique_brands)
                + "," + std::to_string(selected_evaluation->score.total_bowls)
                + ";method=" + (selected_evaluation->method == planner::TypeEvaluationMethod::Optimized
                    ? "optimized" : selected_evaluation->method == planner::TypeEvaluationMethod::Refuel
                    ? "greedy-refuel" : selected_evaluation->method == planner::TypeEvaluationMethod::Greedy
                    ? "greedy" : "all-wait-fallback");
        }
        if (!chosen.fallback_reason.empty()) selector_log.result += ";fallbackReason=" + chosen.fallback_reason;
        selector_log.result += ";selectionMethod=" + chosen.selection_method
            + ";optimizerStatus=" + chosen.optimizer_status
            + ";confidenceLimited=" + (chosen.confidence_limited ? "yes" : "no")
            + ";seed=" + std::to_string(chosen.seed)
            + ";selectorBudgetMs=" + std::to_string(config_.type_selector_budget.count())
            + ";postReserveMs=" + std::to_string(config_.type_submission_reserve.count())
            + ";positionsHash=" + chosen.initial_positions_hash
            + ";candidateSetHash=" + chosen.candidate_set_hash
            + ";effectiveBudgetMs=" + std::to_string(chosen.effective_budget_milliseconds)
            + ";startRemainingMs=" + std::to_string(chosen.started_remaining_milliseconds)
            + ";baselineRemainingMs=" + std::to_string(chosen.baseline_pass_remaining_milliseconds)
            + ";finalRemainingMs=" + std::to_string(chosen.final_remaining_milliseconds)
            + ";stageUs=enum:" + std::to_string(chosen.candidate_enumeration_microseconds)
            + ",greedy:" + std::to_string(chosen.greedy_microseconds)
            + ",refuel:" + std::to_string(chosen.refuel_microseconds)
            + ",optimizer:" + std::to_string(chosen.optimizer_microseconds)
            + ",simulator:unavailable"
            + ";secondPass=" + (chosen.second_pass_attempted ? "yes" : "no")
            + ";secondPassReason=" + chosen.second_pass_reason;
        selector_log.state_transition = "types-selected-before-agent-post";
        logger_->write(selector_log);
    } else {
        auto preset = round_preset(setting.value().initial_agent_positions.size());
        if (!preset) return {RunStatus::Failed, preset.error().message};
        selected_kinds = preset.take();
        output_ << "type-selection=fixed-preset\n";
    }
    if (selected_kinds.size() != setting.value().initial_agent_positions.size())
        return {RunStatus::Failed, "type count does not match agents"};
    if (config_.require_profile_target && selected_by_profile_selector) {
        const auto supply_count = static_cast<std::size_t>(std::count(
            selected_kinds.begin(), selected_kinds.end(), core::AgentKind::Supply));
        if (!config_.type_selector_allowed_supply_counts.empty()) {
            if (std::find(config_.type_selector_allowed_supply_counts.begin(),
                          config_.type_selector_allowed_supply_counts.end(), supply_count)
                == config_.type_selector_allowed_supply_counts.end()) {
                return {RunStatus::Failed, "selected types violate profile supply allowlist"};
            }
        } else if (supply_count < config_.type_selector_min_supply
                   || supply_count > config_.type_selector_max_supply) {
            return {RunStatus::Failed, "selected types violate profile supply range"};
        }
    }
    print_kinds(selected_kinds);
    if (config_.mode == RunMode::Execute && !initial.snapshot().submitted_agent_kinds) {
        if (selection_metadata) initial.record_type_selection(*selection_metadata);
        const auto post_deadline = config_.type_selector == TypeSelectorMode::Prematch
            ? std::optional<protocol::SteadyTime>{clock_.now() + config_.type_submission_reserve}
            : std::nullopt;
        auto submitted = initial.submit_agent_kinds(selected_kinds, post_deadline);
        if (!submitted) {
            const auto saved_failure = initial.save(state_path);
            if (!saved_failure) return {RunStatus::Failed, saved_failure.error().message};
            return {submitted.error().submission_attempted != std::optional<bool>{false}
                        ? RunStatus::RecoveryRequired : RunStatus::Failed,
                    submitted.error().message};
        }
        auto saved = initial.save(state_path);
        if (!saved) return {RunStatus::Failed, saved.error().message};
        output_ << "session=agent-types-submitted\n";
    } else if (config_.mode == RunMode::DryRun) {
        output_ << "session=agent-types-dry-run body=omitted\n";
        if (config_.type_selector == TypeSelectorMode::Prematch
            && !(config_.profile_version == 2 && initial.snapshot().submitted_agent_kinds))
            return {RunStatus::Completed, "prematch type-selector dry-run completed without POST"};
    }

    // Once type registration is complete, wait for the first daily state on GET /
    // instead of re-fetching the already accepted match setting. fetch_state()
    // owns the 403/Retry-After/backoff/deadline policy for this phase.
    output_ << "session=waiting-for-match startsAt=" << setting.value().starts_at << '\n';

    session::SessionController competition(api_, setting.value(), nullptr, logger_);
    if (std::filesystem::exists(state_path)) {
        auto restored = competition.restore(state_path);
        if (!restored) return {RunStatus::RecoveryRequired, restored.error().message};
    }
    if (!competition.bind_production_policy(config_.production_policy_identity))
        return {RunStatus::RecoveryRequired, "production profile/policy identity mismatch"};
    std::optional<core::Quantity> observed_day = competition.snapshot().last_observed_day;
    bool reconcile_server_state = observed_day.has_value();
    std::set<core::Quantity> dry_processed;
    simulator::MatchProgress policy_dry_progress;
    // Only locally Simulator-validated DryRun receipts may seed a dry-run recovery.
    // They never become official accepted days or seed the execute path.
    if (config_.mode == RunMode::DryRun && config_.daily_deadline_policy) {
        std::map<core::Quantity, const session::SubmissionRecord*> summaries;
        for (const auto& receipt : competition.snapshot().submissions) {
            if (receipt.classification == session::SubmissionClassification::DryRun)
                summaries[receipt.day] = &receipt;
        }
        for (const auto& [day, receipt] : summaries) {
            if (day != static_cast<core::Quantity>(dry_processed.size()))
                return {RunStatus::RecoveryRequired, "non-contiguous dry-run simulation history"};
            const auto diagnostic = std::find_if(
                competition.snapshot().daily_planning_diagnostics.rbegin(),
                competition.snapshot().daily_planning_diagnostics.rend(), [&](const auto& r) {
                    return r.is_object() && r.contains("day") && r["day"] == day
                        && r.contains("termination") && r["termination"] == "completed";
                });
            if (diagnostic == competition.snapshot().daily_planning_diagnostics.rend()
                || !receipt->planner || !diagnostic->contains("snapshotHash")
                || !diagnostic->contains("startStateHash") || !diagnostic->contains("endStateHash")
                || !diagnostic->contains("types") || !diagnostic->contains("dryRunEndAgentsHash")
                || !diagnostic->contains("actionHash") || !diagnostic->contains("planHash"))
                return {RunStatus::RecoveryRequired, "incomplete dry-run recovery identity"};
            const auto actions = nlohmann::json::parse(receipt->action_json, nullptr, false);
            const auto encoded_types = nlohmann::json(selected_kinds);
            if (actions.is_discarded() || (*diagnostic)["types"] != encoded_types
                || (*diagnostic)["snapshotHash"] != receipt->planner->snapshot_identity
                || (*diagnostic)["startStateHash"] != receipt->planner->agent_state_identity
                || (*diagnostic)["endStateHash"] != receipt->planner->next_state_identity
                || (*diagnostic)["dryRunEndAgentsHash"] != agent_state_identity(
                    core::DailyState{0, day, receipt->simulation.end_agents, {}, {}})
                || (*diagnostic)["actionHash"] != stable_hash(actions.dump())
                || (*diagnostic)["planHash"] != stable_hash(nlohmann::json{
                    {"actions",actions},{"types",encoded_types},{"day",day}}.dump()))
                return {RunStatus::RecoveryRequired, "dry-run action/state/snapshot identity mismatch"};
            const auto& summary = receipt->simulation;
            dry_processed.insert(day);
            policy_dry_progress.acquired_brands.insert(summary.brands.begin(), summary.brands.end());
            policy_dry_progress.total_balls += summary.total_balls;
            policy_dry_progress.daily_distinct_brand_counts.push_back(
                static_cast<core::Quantity>(summary.brands.size()));
        }
    }
    const auto total_days = static_cast<core::Quantity>(setting.value().day_steps.size());
    const auto log_daily_failure = [&](const core::Quantity day, const std::string& phase,
                                       const std::string& reason) {
        std::string safe_reason = reason;
        for (auto& character : safe_reason) {
            if (character == '\n' || character == '\r') character = ' ';
        }
        if (safe_reason.size() > 240) safe_reason.resize(240);
        protocol::OperationLogEntry entry;
        entry.level = protocol::OperationLogEntry::Level::Warning;
        entry.timestamp_utc = protocol::utc_timestamp();
        entry.operation = "daily-replan";
        entry.method = "LOCAL";
        entry.path = "Day" + std::to_string(day);
        entry.day = day;
        entry.result = "termination=" + phase + ";failureReason=" + safe_reason;
        entry.state_transition = "safe-stop-no-plan-post";
        entry.endpoint = "/";
        entry.phase = "daily-submit";
        entry.stop_reason = phase + ":" + safe_reason;
        entry.response_classification = "safe-stop";
        entry.submission_attempted = false;
        logger_->write(entry);
    };

    while (!stop_requested_()) {
        const auto wait_deadline = clock_.wall_now() + std::chrono::minutes{10};
        auto daily = fetch_state(setting.value(), reconcile_server_state ? std::nullopt : observed_day,
                                 wait_deadline);
        if (!daily) {
            if (stop_requested_()) {
                if (config_.mode == RunMode::Execute) {
                    const auto saved_stop = competition.save(state_path);
                    if (!saved_stop) return {RunStatus::Failed, saved_stop.error().message};
                }
                protocol::OperationLogEntry log;
                log.timestamp_utc = protocol::utc_timestamp();
                log.operation = "automatic-client-exit";
                log.result = "signal-stop";
                logger_->write(log);
                return {RunStatus::Stopped, "stop requested"};
            }
            if (observed_day && *observed_day == total_days - 1 &&
                competition.current_day() && clock_.wall_now() >= unix_time(competition.current_day()->ends_at)) {
                if (config_.mode == RunMode::Execute) {
                    const auto saved_final = competition.save(state_path);
                    if (!saved_final) return {RunStatus::Failed, saved_final.error().message};
                }
                return {RunStatus::Completed, "final day deadline passed"};
            }
            return {RunStatus::Failed, daily.error().message};
        }
        reconcile_server_state = false;
        if (observed_day && daily.value().day > *observed_day + 1) {
            log_daily_failure(daily.value().day, "input_failure", "server skipped an unrecorded day");
            return {RunStatus::RecoveryRequired, "server skipped an unrecorded day"};
        }
        if (daily.value().day < 0 || daily.value().day >= total_days) {
            log_daily_failure(daily.value().day, "input_failure", "invalid day");
            return {RunStatus::RecoveryRequired, "server returned an invalid day"};
        }
        auto observed = competition.observe_day(daily.value());
        if (!observed) {
            log_daily_failure(daily.value().day, "input_failure", observed.error().message);
            return {RunStatus::RecoveryRequired, observed.error().message};
        }
        observed_day = daily.value().day;
        if (daily.value().day == 0 && std::any_of(daily.value().traffic.begin(),
                                                  daily.value().traffic.end(),
                                                  [](const auto& road) {
                                                      return road.status != core::RoadStatus::Smooth;
                                                  })) {
            log_daily_failure(daily.value().day, "input_failure", "Day0 road is not smooth");
            return {RunStatus::RecoveryRequired, "Day0 roads must be smooth"};
        }
        output_ << "daily-input day=" << daily.value().day
                << " typeHash=" << type_identity(selected_kinds)
                << " snapshotHash=" << daily_snapshot_identity(setting.value(), daily.value())
                << " agentStateHash=" << agent_state_identity(daily.value())
                << " spotInventoryHash=" << configured_spot_identity(setting.value())
                << " futureSnapshot=reject\n";

        const bool already_done = config_.mode == RunMode::Execute
            ? competition.snapshot().accepted_days.contains(daily.value().day)
            : dry_processed.contains(daily.value().day);
        if (!already_done) {
            if (config_.daily_deadline_policy) {
                if (config_.planner_mode != PlannerMode::DailyImprovement)
                    return {RunStatus::Failed, "daily deadline policy requires daily-improvement opt-in"};
                const auto size = static_cast<std::size_t>(setting.value().map.height());
                const auto policy = config_.profile_version == 2
                    ? optimizer::DailyDeadlinePolicy::for_production_size(size)
                    : optimizer::DailyDeadlinePolicy::for_size(size);
                if (!policy) return {RunStatus::Failed, "unsupported daily deadline policy size"};
                auto observed_deadline = optimizer::observed_daily_deadline(
                    daily.value().ends_at, clock_.wall_now(), clock_.now());
                constexpr auto planning_safety_reserve = std::chrono::seconds{10};
                const auto extra_reserve = std::max(std::chrono::milliseconds{0},
                    std::chrono::duration_cast<std::chrono::milliseconds>(planning_safety_reserve) - policy->reserve);
                if (observed_deadline) *observed_deadline -= planning_safety_reserve;
                optimizer::OptimizerConfig improvement_config;
                improvement_config.seed = config_.planner_seed;
                improvement_config.maximum_iterations = config_.optimizer_iterations;
                improvement_config.initial_temperature = config_.optimizer_initial_temperature;
                improvement_config.final_temperature = config_.optimizer_final_temperature;
                auto daily_stages = deadline_stages_ ? *deadline_stages_ : optimizer::DailyDeadlineStages{};
                if (!config_.lan_workers.empty()) {
                    daily_stages.worker = [&](const planner::PlannerInput& worker_input,
                        const planner::PlannerResult&, const planner::RefuelPlannerResult& baseline_refuel,
                        const simulator::DaySimulationResult& baseline_sim,
                        protocol::SteadyTime hard_deadline, std::chrono::milliseconds effective_timeout,
                        optimizer::OptimizerClock worker_now) {
                        static_cast<void>(worker_input);
                        optimizer::DailyDeadlineStages::WorkerResult result;
                        result.reason = "worker-unavailable";
                        const auto started = worker_now();
                        const auto previous = config_.mode == RunMode::DryRun ? policy_dry_progress : competition.progress();
                        const auto baseline_score = planner::official_score(previous, baseline_sim);
                        nlohmann::json request = {
                            {"requestId", daily_snapshot_identity(setting.value(), daily.value())},
                            {"evaluatorVersion", "daily-improvement-candidate-v1"},
                            {"day", daily.value().day}, {"size", setting.value().map.height()},
                            {"agentCount", daily.value().own_agents.size()},
                            {"typeIdentity", type_identity(selected_kinds)},
                            {"snapshotIdentity", daily_snapshot_identity(setting.value(), daily.value())},
                            {"mapIdentity", daily_snapshot_identity(setting.value(), daily.value())},
                            {"stateIdentity", agent_state_identity(daily.value())},
                            {"policyIdentity", config_.production_policy_identity},
                            {"workerBudgetMs", effective_timeout.count()},
                            {"futureSnapshotRead", false}, {"lookahead", 0}};
                        request["plannerInput"] = canonical_planner_input(
                            setting.value(), daily.value(), previous, selected_kinds,
                            config_.planner_seed, effective_timeout.count());
                        request["payloadHash"] = canonical_json_hash(request["plannerInput"]);
                        const char* secret = std::getenv(config_.lan_worker_secret_environment.c_str());
                        if (secret == nullptr || *secret == '\0') return result;
                        for (const auto& endpoint : config_.lan_workers) {
                            if (worker_now() + effective_timeout > hard_deadline) {
                                result.reason = "worker-timeout"; break;
                            }
                            const auto endpoint_digest = stable_hash(endpoint.host + ":" + std::to_string(endpoint.port));
                            const auto reply = request_lan_worker(endpoint, secret, request, effective_timeout);
                            if (!reply.success) { result.reason = reply.error.empty() ? "worker-failure" : reply.error; continue; }
                            try {
                                simulator::RawDayActionPlan raw;
                                for (const auto& row : reply.payload.at("actions")) raw.push_back(row.get<std::vector<std::int32_t>>());
                                const auto parsed = simulator::parse_action_plan(raw);
                                if (!parsed) { result.reason = "candidate-action-invalid"; continue; }
                                const simulator::DaySimulationInput sim_input{
                                    setting.value().map, setting.value().spots, setting.value().fuel_limit,
                                    setting.value().day_steps.at(static_cast<std::size_t>(daily.value().day)),
                                    daily.value().own_agents, daily.value().traffic};
                                const auto simulation = simulator::simulate_day(sim_input, parsed.value());
                                if (!simulation) { result.reason = "candidate-simulator-rejected"; continue; }
                                std::vector<std::vector<std::size_t>> visited(simulation.value().acquisitions.size());
                                for (std::size_t i = 0; i < visited.size(); ++i) visited[i] = simulation.value().acquisitions[i].spot_indices;
                                const auto candidate_score = planner::official_score(previous, simulation.value());
                                const auto candidate_readiness = planner::daily_readiness(setting.value(), daily.value(), simulation.value(), visited);
                                const auto expected_score = nlohmann::json::array({candidate_score.total_unique_brands, candidate_score.cumulative_daily_unique_brands, candidate_score.total_bowls});
                                const auto expected_readiness = nlohmann::json::array({candidate_readiness.uncollected_spot_reachability, candidate_readiness.fuel_reserve, candidate_readiness.patrol_dispersion, candidate_readiness.rendezvous_readiness});
                                const auto actions = reply.payload.at("actions");
                                const auto action_hash = canonical_json_hash(actions);
                                const auto plan_hash = canonical_json_hash({{"actions", actions}, {"types", selected_kinds}});
                                nlohmann::json end_agents = nlohmann::json::array();
                                for (const auto& agent : simulation.value().end_agents) end_agents.push_back({{"kind", core::to_int(agent.kind)}, {"position", agent.position.value}, {"fuel", agent.fuel}});
                                const auto end_hash = canonical_json_hash(end_agents);
                                nlohmann::json start_agents = nlohmann::json::array();
                                for (const auto& agent : daily.value().own_agents) start_agents.push_back({{"kind", core::to_int(agent.kind)}, {"position", agent.position.value}, {"fuel", agent.fuel}});
                                const auto start_hash = canonical_json_hash(start_agents);
                                nlohmann::json mismatches = nlohmann::json::array();
                                const auto compare = [&](const std::string& field, const nlohmann::json& expected, const nlohmann::json& actual) {
                                    if (expected != actual) mismatches.push_back({{"field", field}, {"expectedDigest", canonical_json_hash(expected)}, {"actualDigest", canonical_json_hash(actual)}});
                                };
                                compare("requestId", request.at("requestId"), reply.payload.value("requestId", nlohmann::json(nullptr)));
                                compare("inputHash", request.at("payloadHash"), reply.payload.value("inputHash", nlohmann::json(nullptr)));
                                compare("evaluatorVersion", request.at("evaluatorVersion"), reply.payload.value("evaluatorVersion", nlohmann::json(nullptr)));
                                compare("startStateHash", start_hash, reply.payload.value("startStateHash", nlohmann::json(nullptr)));
                                compare("actionHash", action_hash, reply.payload.value("actionHash", nlohmann::json(nullptr)));
                                compare("planHash", plan_hash, reply.payload.value("planHash", nlohmann::json(nullptr)));
                                compare("endStateHash", end_hash, reply.payload.value("endStateHash", nlohmann::json(nullptr)));
                                compare("score", expected_score, reply.payload.value("officialScore", nlohmann::json(nullptr)));
                                compare("readiness", expected_readiness, reply.payload.value("readiness", nlohmann::json(nullptr)));
                                compare("termination", "completed", reply.payload.value("termination", nlohmann::json(nullptr)));
                                if (!mismatches.empty()) {
                                    result.record["lanWorkerClaimDiagnostics"] = {{"workerEndpointDigest", endpoint_digest}, {"schemaVersion", reply.payload.value("protocolVersion", 0)}, {"evaluatorVersion", reply.payload.value("evaluatorVersion", "")}, {"requestIdDigest", canonical_json_hash(request.at("requestId"))}, {"fields", std::move(mismatches)}, {"mainRevalidationTermination", "completed"}, {"mainRevalidationFailureReason", ""}, {"fallback", "baseline-retained"}};
                                    result.reason = "candidate-claims-mismatch"; continue;
                                }
                                const bool better = planner::better_official_score(candidate_score, baseline_score);
                                const auto baseline_readiness = planner::daily_readiness(
                                    setting.value(), daily.value(), baseline_sim, baseline_refuel.visited_spots);
                                const bool tie_better = candidate_score == baseline_score
                                    && planner::better_daily_readiness(candidate_readiness, baseline_readiness);
                                result.record["score"] = expected_score;
                                result.record["readiness"] = expected_readiness;
                                result.record["actionHash"] = action_hash;
                                result.record["planHash"] = plan_hash;
                                result.record["endStateHash"] = end_hash;
                                if (better || tie_better) {
                                    result.plan = parsed.value(); result.simulation = simulation.value(); result.reason = "worker-candidate-adopted"; return result;
                                }
                                result.reason = "baseline-retained-lower-or-equal-candidate";
                            } catch (...) { result.reason = "candidate-response-schema-invalid"; }
                        }
                        static_cast<void>(started);
                        return result;
                    };
                }
                const auto size_worker_cap = setting.value().map.height() == 16 ? std::chrono::milliseconds{5000}
                    : setting.value().map.height() == 24 ? std::chrono::milliseconds{10000}
                    : std::chrono::milliseconds{15000};
                const auto configured_worker_timeout = config_.lan_worker_timeout.count() > 0
                    ? std::min(config_.lan_worker_timeout, size_worker_cap) : size_worker_cap;
                auto planned = optimizer::run_daily_deadline_policy(
                    {setting.value(), daily.value(), config_.mode == RunMode::DryRun ? policy_dry_progress : competition.progress()}, *policy, observed_deadline,
                    {config_.planner_candidate_limit, config_.planner_seed},
                    {config_.refuel_candidate_limit, config_.rendezvous_candidate_limit,
                     config_.maximum_refuels_per_patrol, config_.planner_seed},
                    improvement_config, [&] { return clock_.now(); }, daily_stages, true,
                    configured_worker_timeout);
                planned.record["additionalSafetyReserveMs"] = extra_reserve.count();
                planned.record["profileId"] = config_.profile_id;
                planned.record["profileVersion"] = config_.profile_version;
                planned.record["productionPolicyIdentity"] = config_.production_policy_identity;
                planned.record["observedEndsAt"] = daily.value().ends_at;
                planned.record["hardPlanningDeadline"] = daily.value().ends_at - planning_safety_reserve.count();
                if (config_.mode == RunMode::DryRun && planned.simulation)
                    planned.record["dryRunEndAgentsHash"] = agent_state_identity(
                        core::DailyState{0, daily.value().day, planned.simulation->end_agents, {}, {}});
                planned.record["configuredBudgets"] = {{"greedyMs",config_.planner_budget.count()},
                    {"refuelMs",config_.refuel_budget.count()}, {"optimizerMs",config_.optimizer_budget.count()}};
                planned.record["effectiveDailyBudgets"] = {{"baselineMaxMs",policy->baseline_max.count()},
                    {"improvementMaxMs",policy->improvement_max.count()}, {"reserveMs",policy->reserve.count()+extra_reserve.count()},
                    {"workerMaxMs", setting.value().map.height() == 16 ? 5000 : setting.value().map.height() == 24 ? 10000 : 15000},
                    {"hardPlanningDeadline","endsAt-minus-10-seconds"},
                    {"reserveMeaning","admission-and-fallback-guard-only"},
                    {"stageContract","shared-baseline-worker-optimizer-monotonic-deadline"}};
                if (false && !config_.lan_workers.empty() && planned.plan) {
                    const char* worker_secret = std::getenv(config_.lan_worker_secret_environment.c_str());
                    nlohmann::json worker_request = {
                        {"requestId", daily_snapshot_identity(setting.value(), daily.value())},
                        {"evaluatorVersion", "daily-improvement-candidate-v1"},
                        {"day", daily.value().day},
                        {"size", setting.value().map.height()},
                        {"agentCount", daily.value().own_agents.size()},
                        {"typeIdentity", type_identity(selected_kinds)},
                        {"snapshotIdentity", daily_snapshot_identity(setting.value(), daily.value())},
                        {"mapIdentity", daily_snapshot_identity(setting.value(), daily.value())},
                        {"stateIdentity", agent_state_identity(daily.value())},
                        {"policyIdentity", config_.production_policy_identity},
                        {"workerBudgetMs", policy->improvement_max.count()},
                        {"futureSnapshotRead", false}, {"lookahead", 0}
                    };
                    worker_request["startAgents"] = nlohmann::json::array();
                    for (const auto& agent : daily.value().own_agents) {
                        worker_request["startAgents"].push_back({
                            {"kind", core::to_int(agent.kind)}, {"position", agent.position.value},
                            {"fuel", agent.fuel}});
                    }
                    worker_request["traffic"] = nlohmann::json::array();
                    for (const auto& road : daily.value().traffic) {
                        worker_request["traffic"].push_back({
                            {"position", road.position.value}, {"status", core::to_int(road.status)}});
                    }
                    worker_request["plannerInput"] = canonical_planner_input(
                        setting.value(), daily.value(),
                        config_.mode == RunMode::DryRun ? policy_dry_progress : competition.progress(),
                        selected_kinds, config_.planner_seed, policy->improvement_max.count());
                    worker_request["payloadHash"] = canonical_json_hash(worker_request["plannerInput"]);
                    bool worker_accepted = false;
                    std::string worker_reason = "worker-unavailable";
                    if (worker_secret != nullptr && *worker_secret != '\0') {
                        for (const auto& endpoint : config_.lan_workers) {
                            const auto endpoint_digest = stable_hash(endpoint.host + ":" + std::to_string(endpoint.port));
                            const auto reply = request_lan_worker(endpoint, worker_secret, worker_request,
                                                                   config_.lan_worker_timeout);
                            if (reply.success && reply.payload.value("candidateValidated", false)
                                && reply.payload.value("requestId", "") == worker_request.at("requestId").get<std::string>()
                                && reply.payload.value("payloadHash", "") == worker_request.at("payloadHash").get<std::string>()) {
                                try {
                                    simulator::RawDayActionPlan raw;
                                    for (const auto& row : reply.payload.at("actions"))
                                        raw.push_back(row.get<std::vector<std::int32_t>>());
                                    const auto parsed = simulator::parse_action_plan(raw);
                                    if (!parsed) { worker_reason = "candidate-action-invalid"; continue; }
                                    const auto day_steps = setting.value().day_steps.at(
                                        static_cast<std::size_t>(daily.value().day));
                                    const simulator::DaySimulationInput sim_input{
                                        setting.value().map, setting.value().spots, setting.value().fuel_limit,
                                        day_steps, daily.value().own_agents, daily.value().traffic};
                                    const auto simulation = simulator::simulate_day(sim_input, parsed.value());
                                    if (!simulation) { worker_reason = "candidate-simulator-rejected"; continue; }
                                    std::vector<std::vector<std::size_t>> visited(simulation.value().acquisitions.size());
                                    for (std::size_t index = 0; index < visited.size(); ++index)
                                        visited[index] = simulation.value().acquisitions[index].spot_indices;
                                    const auto score = planner::official_score(
                                        config_.mode == RunMode::DryRun ? policy_dry_progress : competition.progress(),
                                        simulation.value());
                                    const auto readiness = planner::daily_readiness(
                                        setting.value(), daily.value(), simulation.value(), visited);
                                    nlohmann::json end_agents = nlohmann::json::array();
                                    for (const auto& agent : simulation.value().end_agents)
                                        end_agents.push_back({{"kind", core::to_int(agent.kind)},
                                                              {"position", agent.position.value}, {"fuel", agent.fuel}});
                                    const auto action_hash = canonical_json_hash(reply.payload.at("actions"));
                                    const auto plan_hash = canonical_json_hash({{"actions", reply.payload.at("actions")},
                                                                                {"types", selected_kinds}});
                                    const auto end_hash = canonical_json_hash(end_agents);
                                    nlohmann::json start_agents = nlohmann::json::array();
                                    for (const auto& agent : daily.value().own_agents)
                                        start_agents.push_back({{"kind", core::to_int(agent.kind)},
                                                                {"position", agent.position.value}, {"fuel", agent.fuel}});
                                    const auto start_hash = canonical_json_hash(start_agents);
                                    const auto claim_score = reply.payload.at("officialScore").get<std::vector<std::int64_t>>();
                                    const auto claim_readiness = reply.payload.at("readiness").get<std::vector<std::int64_t>>();
                                    const auto expected_score = nlohmann::json::array({
                                        score.total_unique_brands, score.cumulative_daily_unique_brands, score.total_bowls});
                                    const auto expected_readiness = nlohmann::json::array({
                                        readiness.uncollected_spot_reachability, readiness.fuel_reserve,
                                        readiness.patrol_dispersion, readiness.rendezvous_readiness});
                                    const auto expected_input_hash = worker_request.at("payloadHash").get<std::string>();
                                    const auto expected_evaluator = std::string{"daily-improvement-candidate-v1"};
                                    nlohmann::json mismatches = nlohmann::json::array();
                                    const auto compare_claim = [&](const std::string& field,
                                                                   const nlohmann::json& expected,
                                                                   const nlohmann::json& actual) {
                                        if (expected == actual) return;
                                        nlohmann::json item{{"field", field},
                                                            {"expectedDigest", canonical_json_hash(expected)},
                                                            {"actualDigest", canonical_json_hash(actual)}};
                                        if (expected.is_array() && actual.is_array()
                                            && expected.size() <= 16 && actual.size() <= 16
                                            && std::all_of(expected.begin(), expected.end(),
                                                [](const auto& value) { return value.is_number_integer(); })
                                            && std::all_of(actual.begin(), actual.end(),
                                                [](const auto& value) { return value.is_number_integer(); })) {
                                            item["expected"] = expected;
                                            item["actual"] = actual;
                                        }
                                        mismatches.push_back(std::move(item));
                                    };
                                    compare_claim("requestId", worker_request.at("requestId"),
                                                  reply.payload.value("requestId", nlohmann::json(nullptr)));
                                    compare_claim("inputHash", expected_input_hash,
                                                  reply.payload.value("inputHash", nlohmann::json(nullptr)));
                                    compare_claim("evaluatorVersion", expected_evaluator,
                                                  reply.payload.value("evaluatorVersion", nlohmann::json(nullptr)));
                                    compare_claim("startStateHash", start_hash,
                                                  reply.payload.value("startStateHash", nlohmann::json(nullptr)));
                                    compare_claim("actionHash", action_hash,
                                                  reply.payload.value("actionHash", nlohmann::json(nullptr)));
                                    compare_claim("planHash", plan_hash,
                                                  reply.payload.value("planHash", nlohmann::json(nullptr)));
                                    compare_claim("endStateHash", end_hash,
                                                  reply.payload.value("endStateHash", nlohmann::json(nullptr)));
                                    compare_claim("score", expected_score, claim_score);
                                    compare_claim("readiness", expected_readiness, claim_readiness);
                                    compare_claim("termination", "completed",
                                                  reply.payload.value("termination", nlohmann::json(nullptr)));
                                    if (!mismatches.empty()) {
                                        planned.record["lanWorkerClaimDiagnostics"] = {
                                            {"workerEndpointDigest", endpoint_digest},
                                            {"schemaVersion", reply.payload.value("protocolVersion", 0)},
                                            {"evaluatorVersion", reply.payload.value("evaluatorVersion", "")},
                                            {"requestIdDigest", canonical_json_hash(worker_request.at("requestId"))},
                                            {"fields", std::move(mismatches)},
                                            {"mainRevalidationTermination", "completed"},
                                            {"mainRevalidationFailureReason", ""},
                                            {"fallback", "baseline-retained"}};
                                        worker_reason = "candidate-claims-mismatch";
                                        continue;
                                    }
                                    const auto score_values = planned.record.value("score", std::vector<std::int64_t>{0, 0, 0});
                                    const auto readiness_values = planned.record.value("readiness", std::vector<std::int64_t>{0, 0, 0, 0});
                                    if (score_values.size() != 3 || readiness_values.size() != 4) {
                                        worker_reason = "baseline-claims-missing"; continue;
                                    }
                                    const auto baseline_score = planner::OfficialScore{
                                        score_values[0], score_values[1], score_values[2]};
                                    const auto baseline_readiness = planner::DailyReadiness{
                                        readiness_values[0], readiness_values[1], readiness_values[2], readiness_values[3], {}};
                                    const auto candidate_better = planner::better_official_score(score, baseline_score);
                                    const auto candidate_tie_better = score == baseline_score
                                        && planner::better_daily_readiness(readiness, baseline_readiness);
                                    if (candidate_better || candidate_tie_better) {
                                        planned.plan = parsed.value();
                                        planned.simulation = simulation.value();
                                        planned.record["lanWorkerCandidate"] = {
                                            {"actionHash", action_hash}, {"endStateHash", end_hash},
                                            {"adoptionReason", candidate_better ? "official-score-improved" : "readiness-tie-break"}};
                                        worker_accepted = true;
                                        worker_reason = "worker-candidate-adopted";
                                    } else worker_reason = "baseline-retained-lower-or-equal-candidate";
                                } catch (...) {
                                    worker_reason = "candidate-response-schema-invalid";
                                    planned.record["lanWorkerClaimDiagnostics"] = {
                                        {"workerEndpointDigest", endpoint_digest},
                                        {"schemaVersion", reply.payload.value("protocolVersion", 0)},
                                        {"evaluatorVersion", reply.payload.value("evaluatorVersion", "")},
                                        {"requestIdDigest", canonical_json_hash(worker_request.at("requestId"))},
                                        {"fields", nlohmann::json::array()},
                                        {"mainRevalidationTermination", "schema-invalid"},
                                        {"mainRevalidationFailureReason", "candidate response schema invalid"},
                                        {"fallback", "baseline-retained"}};
                                }
                                if (worker_accepted) break;
                            }
                            if (!reply.error.empty()) worker_reason = reply.error;
                        }
                    }
                    planned.record["lanWorker"] = {
                        {"requested", true}, {"accepted", worker_accepted},
                        {"fallback", !worker_accepted}, {"reason", worker_reason},
                        {"workerCount", config_.lan_workers.size()},
                        {"fallbackMode", worker_accepted ? "none" : "baseline-retained"}
                    };
                    if (!worker_accepted) output_ << "lan-worker baseline-retained reason=" << worker_reason << '\n';
                }
                planned.record["phase"] = "daily-submit";
                planned.record["submissionAttempted"] = false;
                planned.record["retryAfterMs"] = nullptr;
                planned.record["stopReason"] = "";
                planned.record["fallback"] = planned.plan ? "none" : "baseline-retained";
                if (!planned.plan) {
                    planned.record["plannedActionHash"] = nullptr;
                    planned.record["planHash"] = nullptr;
                    planned.record["startStateHash"] = agent_state_identity(daily.value());
                    planned.record["stopReason"] = planned.record.value("failureReason", "planner failure");
                    planned.record["submissionAttempted"] = false;
                }
                competition.record_daily_planning(planned.record);
                output_ << "daily-deadline " << planned.record.dump() << '\n';
                protocol::OperationLogEntry entry;
                entry.operation = "daily-deadline-policy"; entry.day = daily.value().day;
                entry.phase = "daily-submit";
                entry.submission_attempted = false;
                entry.stop_reason = planned.record.value("stopReason", "");
                entry.result = planned.record.dump(); logger_->write(entry);
                const auto saved = competition.save(state_path);
                if (!saved) return {RunStatus::Failed, saved.error().message};
                if (!planned.plan) return {RunStatus::RecoveryRequired,
                    planned.record.at("failureReason").get<std::string>()};
                std::optional<session::PlannerSubmissionMetadata> dry_metadata;
                if (config_.mode == RunMode::DryRun) {
                    dry_metadata.emplace();
                    dry_metadata->planner_kind = "daily-improvement-dry-run";
                    dry_metadata->snapshot_identity = planned.record.at("snapshotHash").get<std::string>();
                    dry_metadata->agent_state_identity = planned.record.at("startStateHash").get<std::string>();
                    dry_metadata->next_state_identity = planned.record.at("endStateHash").get<std::string>();
                    dry_metadata->selection_reason = planned.record.at("adoptionReason").get<std::string>();
                }
                // observed_deadline is already hardPlanningDeadline (endsAt - 10s).
                // Do not subtract policy.reserve again; it only gates starting improvement.
                const auto submission_deadline = *observed_deadline;
                if (submission_deadline <= clock_.now()) {
                    planned.record["stopReason"] = "deadline";
                    planned.record["fallback"] = "baseline-retained";
                    planned.record["submissionAttempted"] = false;
                    competition.record_daily_planning(planned.record);
                    const auto saved_stop = competition.save(state_path);
                    if (!saved_stop) return {RunStatus::Failed, saved_stop.error().message};
                    log_daily_failure(daily.value().day, "deadline", "submission deadline/reserve exceeded");
                    return {RunStatus::RecoveryRequired, "submission deadline/reserve exceeded"};
                }
                auto sent = competition.submit_plan(*planned.plan, config_.mode == RunMode::DryRun,
                    submission_deadline, dry_metadata);
                if (!sent) {
                    planned.record["stopReason"] = sent.error().message;
                    planned.record["fallback"] = "baseline-retained";
                    planned.record["submissionAttempted"] = sent.error().submission_attempted
                        ? nlohmann::json(*sent.error().submission_attempted)
                        : nlohmann::json(nullptr);
                    planned.record["retryAfterMs"] = sent.error().retry_after_ms
                        ? nlohmann::json(*sent.error().retry_after_ms) : nlohmann::json(nullptr);
                    competition.record_daily_planning(planned.record);
                    const auto saved_stop = competition.save(state_path);
                    if (!saved_stop) return {RunStatus::Failed, saved_stop.error().message};
                    log_daily_failure(daily.value().day, "transport-or-deadline", sent.error().message);
                    return {RunStatus::RecoveryRequired, sent.error().message};
                }
                planned.record["submissionAttempted"] = config_.mode == RunMode::DryRun
                    ? nlohmann::json(false) : nlohmann::json(true);
                competition.record_daily_planning(planned.record);
                if (config_.mode == RunMode::DryRun) {
                    dry_processed.insert(daily.value().day);
                    policy_dry_progress = simulator::accumulate_progress(policy_dry_progress, *planned.simulation);
                    const auto saved_dry = competition.save(state_path);
                    if (!saved_dry) return {RunStatus::Failed, saved_dry.error().message};
                } else {
                    const auto saved_submission = competition.save(state_path);
                    if (!saved_submission) return {RunStatus::Failed, saved_submission.error().message};
                }
                print_day_summary(setting.value(), daily.value(), sent.value(), competition.progress());
            } else {
            const auto deadline = competition.deadline(clock_.wall_now(), clock_.now(), config_.safety_margin);
            if (deadline.remaining(clock_.now()).count() <= 0) {
                log_daily_failure(daily.value().day, "deadline_exhausted", "safety deadline");
                return {RunStatus::RecoveryRequired, "day safety deadline already passed"};
            }
            const auto previous_progress = competition.progress();
            auto submitted = competition.submit_safe_wait(
                config_.mode == RunMode::DryRun, deadline.stop_at);
            if (!submitted) {
                log_daily_failure(daily.value().day, "simulation_or_transport_failure",
                                  submitted.error().message);
                if (config_.mode == RunMode::Execute) {
                    const auto saved_failure = competition.save(state_path);
                    if (!saved_failure) return {RunStatus::Failed, saved_failure.error().message};
                }
                return {submitted.error().submission_attempted != std::optional<bool>{false}
                            ? RunStatus::RecoveryRequired : RunStatus::Failed,
                        submitted.error().message};
            }
            if (config_.mode == RunMode::DryRun) dry_processed.insert(daily.value().day);
            else {
                auto saved = competition.save(state_path);
                if (!saved) return {RunStatus::Failed, saved.error().message};
            }
            print_day_summary(setting.value(), daily.value(), submitted.value(), competition.progress());

            if (config_.planner_mode != PlannerMode::Wait) {
                const auto planner_deadline = std::min(
                    deadline.stop_at, clock_.now() + config_.planner_budget);
                const auto planned = planner::make_greedy_plan(
                    {setting.value(), daily.value(), previous_progress},
                    {config_.planner_candidate_limit, config_.planner_seed},
                    planner_deadline,
                    [&] { return clock_.now(); });
                if (!planned) {
                    log_daily_failure(daily.value().day, "planner_failure", planned.error().message);
                    output_ << "planner=greedy fallback=yes message=" << planned.error().message << '\n';
                } else {
                    const auto& result = planned.value();
                    output_ << "planner=greedy baseline="
                            << result.baseline_score.total_unique_brands << ','
                            << result.baseline_score.cumulative_daily_unique_brands << ','
                            << result.baseline_score.total_bowls
                            << " greedy=" << result.score.total_unique_brands << ','
                            << result.score.cumulative_daily_unique_brands << ','
                            << result.score.total_bowls
                            << " improved="
                            << (planner::better_official_score(result.score, result.baseline_score)
                                    ? "yes" : "no")
                            << " candidates=" << result.evaluated_candidates
                            << " accepted=" << result.accepted_improvements
                            << " elapsedUs=" << result.elapsed.count()
                            << " termination=" << static_cast<int>(result.termination)
                            << " baselineRevision=";
                    if (submitted.value().revision) output_ << *submitted.value().revision;
                    else output_ << "none";
                    output_ << '\n';
                    output_ << "daily-replan day=" << daily.value().day
                            << " status=simulator-verified readiness="
                            << result.daily_readiness.uncollected_spot_reachability << ','
                            << result.daily_readiness.fuel_reserve << ','
                            << result.daily_readiness.patrol_dispersion << ','
                            << result.daily_readiness.rendezvous_readiness
                            << " tieGroup=" << result.official_score_tie_group_count
                            << " snapshotHash=" << daily_snapshot_identity(setting.value(), daily.value())
                            << " nextStateHash=" << agent_state_identity(daily.value()) << '\n';
                    for (std::size_t agent = 0; agent < result.visited_spots.size(); ++agent) {
                        output_ << "  agent=" << agent << " spots=";
                        for (const auto spot : result.visited_spots[agent]) output_ << spot << ',';
                        output_ << " objectives=";
                        for (const auto objective : result.route_objectives[agent]) {
                            output_ << (objective == pathfinding::RouteObjective::Fastest
                                            ? "fastest" : "fuel") << ',';
                        }
                        output_ << " travelSteps=" << result.agent_travel_steps[agent]
                                << " endFuel=" << result.simulation.end_agents[agent].fuel << '\n';
                    }
                    if (planner::better_official_score(result.score, result.baseline_score)) {
                        if (deadline.remaining(clock_.now()).count() <= 0) {
                            output_ << "planner-improvement=not-sent reason=deadline\n";
                        } else {
                            session::PlannerSubmissionMetadata metadata;
                            metadata.planner_kind = "greedy";
                            metadata.seed = config_.planner_seed;
                            metadata.candidate_limit = config_.planner_candidate_limit;
                            metadata.budget_milliseconds = config_.planner_budget.count();
                            metadata.official_score = {
                                result.score.total_unique_brands,
                                result.score.cumulative_daily_unique_brands,
                                result.score.total_bowls};
                            metadata.internal_tie_break = {
                                result.tie_break.patrol_fuel_remaining,
                                result.tie_break.total_travel_steps,
                                result.tie_break.total_edges};
                            add_daily_metadata(metadata, setting.value(), daily.value(), selected_kinds,
                                               result.simulation, config_.safety_margin,
                                               result.daily_readiness,
                                               "higher OfficialScore; daily readiness only after exact tie");
                            metadata.official_score_tie_group_count = result.official_score_tie_group_count;
                            metadata.visited_spots = result.visited_spots;
                            auto improved = competition.submit_plan(
                                result.plan,
                                config_.mode == RunMode::DryRun,
                                deadline.stop_at,
                                std::move(metadata));
                            if (!improved) {
                                if (config_.mode == RunMode::Execute) {
                                    const auto saved_failure = competition.save(state_path);
                                    if (!saved_failure) {
                                        return {RunStatus::Failed, saved_failure.error().message};
                                    }
                                }
                                return {improved.error().submission_attempted != std::optional<bool>{false}
                                            ? RunStatus::RecoveryRequired : RunStatus::Failed,
                                        improved.error().message};
                            }
                            output_ << "greedyRevision=";
                            if (improved.value().revision) output_ << *improved.value().revision;
                            else output_ << "dry-run";
                            output_ << '\n';
                            if (config_.mode == RunMode::Execute) {
                                auto saved_improvement = competition.save(state_path);
                                if (!saved_improvement) {
                                    return {RunStatus::Failed, saved_improvement.error().message};
                                }
                            }
                        }
                    }
                    if (config_.planner_mode == PlannerMode::GreedyRefuel
                        || config_.planner_mode == PlannerMode::Optimized
                        || config_.planner_mode == PlannerMode::DailyImprovement) {
                        const auto refuel_deadline = std::min(
                            deadline.stop_at, clock_.now() + config_.refuel_budget);
                        const auto refueled = planner::make_refuel_plan(
                            {setting.value(), daily.value(), previous_progress}, result,
                            {config_.refuel_candidate_limit,
                             config_.rendezvous_candidate_limit,
                             config_.maximum_refuels_per_patrol,
                             config_.planner_seed},
                            refuel_deadline, [&] { return clock_.now(); });
                        if (!refueled) {
                            log_daily_failure(daily.value().day, "planner_failure", refueled.error().message);
                            output_ << "planner=refuel fallback=yes message="
                                    << refueled.error().message << '\n';
                        } else {
                            const auto& refuel = refueled.value();
                            const bool refuel_improved = planner::better_official_score(
                                refuel.score, result.score);
                            output_ << "planner=refuel greedy="
                                    << result.score.total_unique_brands << ','
                                    << result.score.cumulative_daily_unique_brands << ','
                                    << result.score.total_bowls << " refuel="
                                    << refuel.score.total_unique_brands << ','
                                    << refuel.score.cumulative_daily_unique_brands << ','
                                    << refuel.score.total_bowls
                                    << " improved=" << (refuel_improved ? "yes" : "no")
                                    << " candidates=" << refuel.evaluated_candidates
                                    << " elapsedUs=" << refuel.elapsed.count()
                                    << " termination=" << static_cast<int>(refuel.termination) << '\n';
                            output_ << "daily-replan day=" << daily.value().day
                                    << " status=simulator-verified readiness="
                                    << refuel.daily_readiness.uncollected_spot_reachability << ','
                                    << refuel.daily_readiness.fuel_reserve << ','
                                    << refuel.daily_readiness.patrol_dispersion << ','
                                    << refuel.daily_readiness.rendezvous_readiness
                                    << " tieGroup=" << refuel.official_score_tie_group_count
                                    << " snapshotHash=" << daily_snapshot_identity(setting.value(), daily.value())
                                    << " nextStateHash=" << agent_state_identity(daily.value()) << '\n';
                            for (const auto& meeting : refuel.rendezvous) {
                                output_ << "  rendezvous patrol=" << meeting.patrol_agent
                                        << " supply=" << meeting.supply_agent
                                        << " step=" << meeting.step
                                        << " cell=" << meeting.cell.value
                                        << " fuel=" << meeting.fuel_before << "->"
                                        << meeting.fuel_after << '\n';
                            }
                            if (refuel_improved && deadline.remaining(clock_.now()).count() > 0) {
                                session::PlannerSubmissionMetadata metadata;
                                metadata.planner_kind = "greedy-refuel";
                                metadata.seed = config_.planner_seed;
                                metadata.candidate_limit = config_.refuel_candidate_limit;
                                metadata.budget_milliseconds = config_.refuel_budget.count();
                                metadata.official_score = {refuel.score.total_unique_brands,
                                    refuel.score.cumulative_daily_unique_brands,
                                    refuel.score.total_bowls};
                                add_daily_metadata(metadata, setting.value(), daily.value(), selected_kinds,
                                                   refuel.simulation, config_.safety_margin,
                                                   refuel.daily_readiness,
                                                   refuel_improved ? "higher OfficialScore" :
                                                   "OfficialScore tie; daily readiness tie-break evaluated");
                                metadata.official_score_tie_group_count = refuel.official_score_tie_group_count;
                                metadata.visited_spots = refuel.visited_spots;
                                for (const auto& meeting : refuel.rendezvous) {
                                    metadata.rendezvous.push_back({
                                        static_cast<std::int64_t>(meeting.patrol_agent),
                                        static_cast<std::int64_t>(meeting.supply_agent),
                                        meeting.cell.value, meeting.step,
                                        meeting.fuel_before, meeting.fuel_after});
                                }
                                for (const auto& supply : refuel.supply_schedules) {
                                    metadata.supply_plans.push_back({
                                        static_cast<std::int64_t>(supply.agent_index),
                                        supply.travel_steps, supply.wait_steps});
                                }
                                auto submitted_refuel = competition.submit_plan(refuel.plan,
                                    config_.mode == RunMode::DryRun, deadline.stop_at,
                                    std::move(metadata));
                                if (!submitted_refuel) {
                                    log_daily_failure(daily.value().day, "simulation_or_transport_failure",
                                                      submitted_refuel.error().message);
                                    if (config_.mode == RunMode::Execute) {
                                        static_cast<void>(competition.save(state_path));
                                    }
                                    return {submitted_refuel.error().code
                                                == protocol::ErrorCode::UnknownResponse
                                                ? RunStatus::RecoveryRequired : RunStatus::Failed,
                                            submitted_refuel.error().message};
                                }
                                output_ << "refuelRevision=";
                                if (submitted_refuel.value().revision) {
                                    output_ << *submitted_refuel.value().revision;
                                } else output_ << "dry-run";
                                output_ << '\n';
                                if (config_.mode == RunMode::Execute) {
                                    auto saved_refuel = competition.save(state_path);
                                    if (!saved_refuel) return {RunStatus::Failed,
                                                               saved_refuel.error().message};
                                }
                            }
                            if ((config_.planner_mode == PlannerMode::Optimized
                                 || config_.planner_mode == PlannerMode::DailyImprovement)
                                && deadline.remaining(clock_.now()).count() > 0) {
                                const auto optimizer_deadline = std::min(
                                    deadline.stop_at, clock_.now() + config_.optimizer_budget);
                                optimizer::OptimizerConfig optimizer_config;
                                optimizer_config.seed = config_.planner_seed;
                                optimizer_config.prefer_daily_readiness_on_tie =
                                    config_.planner_mode == PlannerMode::DailyImprovement;
                                optimizer_config.maximum_iterations = config_.optimizer_iterations;
                                optimizer_config.initial_temperature = config_.optimizer_initial_temperature;
                                optimizer_config.final_temperature = config_.optimizer_final_temperature;
                                const auto optimized = optimizer::optimize(
                                    {setting.value(), daily.value(), previous_progress},
                                    result, refuel, optimizer_config, optimizer_deadline,
                                    [&] { return clock_.now(); });
                                if (!optimized) {
                                    const auto failure_phase = optimized.error().code
                                        == planner::PlannerErrorCode::BaselineSimulationFailed
                                        ? "simulation_failure" : "planner_failure";
                                    log_daily_failure(daily.value().day, failure_phase, optimized.error().message);
                                    output_ << "planner="
                                            << (config_.planner_mode == PlannerMode::DailyImprovement
                                                ? "daily-improvement" : "optimizer")
                                            << " fallback=yes reason=baseline-retained termination="
                                            << failure_phase << " failureCount=1 message="
                                            << optimized.error().message << '\n';
                                } else {
                                    const auto& value = optimized.value();
                                    const bool use_refuel_baseline =
                                        config_.planner_mode == PlannerMode::DailyImprovement || refuel_improved;
                                    const auto adopted_score = use_refuel_baseline ? refuel.score : result.score;
                                    const auto adopted_readiness = use_refuel_baseline
                                        ? refuel.daily_readiness : result.daily_readiness;
                                    const auto decision = optimizer::evaluate_daily_improvement(
                                        adopted_score, adopted_readiness, value.score, value.readiness);
                                    const bool improved = decision.adopt;
                                    output_ << "planner="
                                            << (config_.planner_mode == PlannerMode::DailyImprovement
                                                ? "daily-improvement" : "optimizer")
                                            << " initial="
                                            << value.initial_score.total_unique_brands << ','
                                            << value.initial_score.cumulative_daily_unique_brands << ','
                                            << value.initial_score.total_bowls << " best="
                                            << value.score.total_unique_brands << ','
                                            << value.score.cumulative_daily_unique_brands << ','
                                            << value.score.total_bowls
                                            << " improved=" << (improved ? "yes" : "no")
                                            << " iterations=" << value.iterations
                                            << " generated=" << value.generated_candidates
                                            << " prefiltered=" << value.prefiltered_candidates
                                            << " valid=" << value.valid_candidates
                                            << " invalid=" << value.invalid_candidates
                                            << " simulatorRuns=" << value.simulator_runs
                                            << " accepted=" << value.accepted_candidates
                                            << " elapsedUs=" << value.elapsed.count()
                                            << " seed=" << value.seed
                                            << " baselineReadiness=" << adopted_readiness.uncollected_spot_reachability
                                            << ',' << adopted_readiness.fuel_reserve << ','
                                            << adopted_readiness.patrol_dispersion << ','
                                            << adopted_readiness.rendezvous_readiness
                                            << " finalReadiness=" << value.readiness.uncollected_spot_reachability
                                            << ',' << value.readiness.fuel_reserve << ','
                                            << value.readiness.patrol_dispersion << ','
                                            << value.readiness.rendezvous_readiness
                                            << " reason=" << (decision.reason ==
                                                optimizer::DailyImprovementDecisionReason::OfficialScoreImproved
                                                ? "official-score-improved"
                                                : decision.reason == optimizer::DailyImprovementDecisionReason::ReadinessTieBreak
                                                    ? "readiness-tie-break" : "baseline-retained")
                                            << " termination=" << static_cast<int>(value.termination) << '\n';
                                    if (improved && deadline.remaining(clock_.now()).count() > 0) {
                                        session::PlannerSubmissionMetadata metadata;
                                        metadata.planner_kind = config_.planner_mode == PlannerMode::DailyImprovement
                                            ? "daily-improvement" : "optimized";
                                        metadata.seed = value.seed;
                                        metadata.candidate_limit = config_.optimizer_iterations;
                                        metadata.budget_milliseconds = config_.optimizer_budget.count();
                                        metadata.official_score = {value.score.total_unique_brands,
                                            value.score.cumulative_daily_unique_brands,
                                            value.score.total_bowls};
                                        metadata.baseline_official_score = {adopted_score.total_unique_brands,
                                            adopted_score.cumulative_daily_unique_brands, adopted_score.total_bowls};
                                        metadata.baseline_daily_readiness = {
                                            adopted_readiness.uncollected_spot_reachability,
                                            adopted_readiness.fuel_reserve,
                                            adopted_readiness.patrol_dispersion,
                                            adopted_readiness.rendezvous_readiness};
                                        metadata.internal_tie_break = {
                                            value.tie_break.patrol_fuel_remaining,
                                            value.tie_break.total_travel_steps,
                                            value.tie_break.total_edges};
                                        metadata.visited_spots.resize(daily.value().own_agents.size());
                                        for (const auto& route : value.solution.patrol_routes)
                                            metadata.visited_spots[route.agent_index] = route.spot_indices;
                                        metadata.route_objectives.resize(daily.value().own_agents.size());
                                        for (const auto& route : value.solution.patrol_routes) {
                                            auto& encoded = metadata.route_objectives[route.agent_index];
                                            for (const auto objective : route.objectives)
                                                encoded.push_back(objective == pathfinding::RouteObjective::Fastest ? 0 : 1);
                                        }
                                        std::vector<std::vector<std::size_t>> optimized_visits = metadata.visited_spots;
                                        add_daily_metadata(metadata, setting.value(), daily.value(), selected_kinds,
                                                           value.simulation, config_.safety_margin,
                                                           planner::daily_readiness(setting.value(), daily.value(),
                                                                                    value.simulation, optimized_visits),
                                                           decision.reason == optimizer::DailyImprovementDecisionReason::OfficialScoreImproved
                                                               ? "official-score-improved" : "readiness-tie-break");
                                        metadata.official_score_tie_group_count = 1;
                                        metadata.optimizer_counts = {value.iterations,
                                            value.generated_candidates, value.prefiltered_candidates,
                                            value.simulator_runs, value.valid_candidates,
                                            value.accepted_candidates, value.improvements};
                                        for (const auto& stats : value.neighborhoods)
                                            metadata.neighborhood_statistics.push_back({stats.generated,
                                                stats.prefiltered, stats.simulated, stats.valid,
                                                stats.accepted, stats.improved});
                                        auto sent = competition.submit_plan(value.plan,
                                            config_.mode == RunMode::DryRun, deadline.stop_at,
                                            std::move(metadata));
                                        if (!sent) {
                                            if (config_.mode == RunMode::Execute)
                                                static_cast<void>(competition.save(state_path));
                                            return {sent.error().submission_attempted != std::optional<bool>{false}
                                                        ? RunStatus::RecoveryRequired : RunStatus::Failed,
                                                    sent.error().message};
                                        }
                                        output_ << "optimizedRevision=";
                                        if (sent.value().revision) output_ << *sent.value().revision;
                                        else output_ << "dry-run";
                                        output_ << '\n';
                                        if (config_.mode == RunMode::Execute) {
                                            auto saved_optimized = competition.save(state_path);
                                            if (!saved_optimized)
                                                return {RunStatus::Failed, saved_optimized.error().message};
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        } // Legacy planning path remains unchanged without policy opt-in.

        if (daily.value().day == total_days - 1) {
            const auto end = unix_time(daily.value().ends_at);
            if (clock_.wall_now() < end) {
                const auto duration = end - clock_.wall_now();
                clock_.wait_until(clock_.now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
            }
            if (config_.mode == RunMode::Execute) {
                auto saved = competition.save(state_path);
                if (!saved) return {RunStatus::Failed, saved.error().message};
            }
            return stop_requested_() ? RunResult{RunStatus::Stopped, "stop requested"}
                                          : RunResult{RunStatus::Completed, "all days completed"};
        }
    }
    if (config_.mode == RunMode::Execute) {
        const auto saved_stop = competition.save(state_path);
        if (!saved_stop) return {RunStatus::Failed, saved_stop.error().message};
    }
    return {RunStatus::Stopped, "stop requested"};
}

}  // namespace hexa_udon::app
