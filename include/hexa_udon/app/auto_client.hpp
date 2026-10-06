#pragma once

#include "hexa_udon/protocol/api_client.hpp"
#include "hexa_udon/planner/greedy_planner.hpp"
#include "hexa_udon/session/polling.hpp"
#include "hexa_udon/session/session.hpp"
#include "hexa_udon/optimizer/daily_deadline_policy.hpp"
#include "hexa_udon/app/lan_worker.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace hexa_udon::app {

enum class RunMode { DryRun, Execute };
enum class RunStatus { Completed, Stopped, RecoveryRequired, Failed };
enum class PlannerMode { Wait, Greedy, GreedyRefuel, Optimized, DailyImprovement };
enum class TypeSelectorMode { Fixed, Prematch };

struct AutoClientConfig {
    std::string base_url;
    RunMode mode = RunMode::DryRun;
    std::filesystem::path state_directory;
    std::optional<std::vector<core::AgentKind>> explicit_kinds;
    bool use_round_preset = true;
    std::chrono::milliseconds polling_interval{750};
    std::chrono::seconds safety_margin{5};
    std::size_t maximum_get_attempts = 8;
    PlannerMode planner_mode = PlannerMode::Wait;
    bool daily_deadline_policy = false;
    std::chrono::milliseconds planner_budget{1500};
    std::size_t planner_candidate_limit = 2000;
    std::uint64_t planner_seed = 30013;
    std::chrono::milliseconds refuel_budget{2000};
    std::size_t refuel_candidate_limit = 2000;
    std::size_t rendezvous_candidate_limit = 4000;
    std::size_t maximum_refuels_per_patrol = 2;
    std::chrono::milliseconds optimizer_budget{5000};
    std::size_t optimizer_iterations = 10000;
    TypeSelectorMode type_selector = TypeSelectorMode::Fixed;
    std::size_t type_selector_max_supply = 1;
    std::size_t type_selector_min_supply = 0;
    std::vector<std::size_t> type_selector_allowed_supply_counts;
    std::chrono::milliseconds type_selector_budget{3000};
    std::size_t type_selector_max_candidates = 35;
    std::chrono::milliseconds type_submission_reserve{6000};
    double optimizer_initial_temperature = 8.0;
    double optimizer_final_temperature = 0.05;
    bool require_16x16_profile = false;
    bool require_profile_target = false;
    std::string profile_id;
    int profile_version = 1;
    std::optional<std::string> profile_set_version;
    std::filesystem::path profile_set_directory{"config/profiles"};
    nlohmann::json production_policy_identity = nullptr;
    std::size_t required_map_height = 0;
    std::size_t required_map_width = 0;
    std::size_t required_agent_count = 0;
    std::vector<LanWorkerEndpoint> lan_workers;
    std::string lan_worker_secret_environment;
    // Zero selects the size-specific Phase 50 cap; a positive value is an explicit override.
    std::chrono::milliseconds lan_worker_timeout{0};
};

struct RunResult {
    RunStatus status;
    std::string message;
};

class AppClock : public session::PollClock {
public:
    [[nodiscard]] virtual std::chrono::system_clock::time_point wall_now() const = 0;
};

class SystemAppClock final : public AppClock {
public:
    [[nodiscard]] protocol::SteadyTime now() const override;
    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const override;
    void wait_until(protocol::SteadyTime time) override;
};

class SessionDirectoryLock {
public:
    SessionDirectoryLock() = default;
    ~SessionDirectoryLock();
    SessionDirectoryLock(const SessionDirectoryLock&) = delete;
    SessionDirectoryLock& operator=(const SessionDirectoryLock&) = delete;
    SessionDirectoryLock(SessionDirectoryLock&& other) noexcept;
    SessionDirectoryLock& operator=(SessionDirectoryLock&& other) noexcept;

    [[nodiscard]] static protocol::Result<SessionDirectoryLock> acquire(
        const std::filesystem::path& directory);
    [[nodiscard]] bool owns_lock() const noexcept;

private:
    explicit SessionDirectoryLock(int descriptor);
    int descriptor_ = -1;
};

[[nodiscard]] protocol::Result<std::vector<core::AgentKind>> round_preset(
    std::size_t agent_count);
[[nodiscard]] protocol::Result<std::vector<core::AgentKind>> parse_kind_list(
    const std::string& text);

class AutoCompetitionClient {
public:
    AutoCompetitionClient(protocol::ProconApiClient& api, AutoClientConfig config,
                          AppClock& clock, std::function<bool()> stop_requested,
                          std::ostream& output,
                          protocol::OperationLogger* logger = nullptr,
                          const optimizer::DailyDeadlineStages* deadline_stages = nullptr);

    [[nodiscard]] RunResult run();

private:
    [[nodiscard]] protocol::Result<core::MatchConfig> fetch_setting(
        const std::string& phase = "registration",
        std::optional<protocol::SteadyTime> deadline = std::nullopt);
    [[nodiscard]] protocol::Result<core::DailyState> fetch_state(
        const core::MatchConfig& config,
        std::optional<core::Quantity> current_day,
        std::chrono::system_clock::time_point wall_deadline);
    [[nodiscard]] bool has_unknown_submission(const session::SessionSnapshot& snapshot) const;
    void print_kinds(const std::vector<core::AgentKind>& kinds);
    void print_day_summary(const core::MatchConfig& config, const core::DailyState& daily,
                           const session::SubmissionRecord& record,
                           const simulator::MatchProgress& progress);

    protocol::ProconApiClient& api_;
    AutoClientConfig config_;
    AppClock& clock_;
    std::function<bool()> stop_requested_;
    std::ostream& output_;
    protocol::NullOperationLogger null_logger_;
    protocol::OperationLogger* logger_;
    const optimizer::DailyDeadlineStages* deadline_stages_;
};

}  // namespace hexa_udon::app
