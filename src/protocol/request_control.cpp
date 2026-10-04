#include "hexa_udon/protocol/request_control.hpp"

#include <nlohmann/json.hpp>

#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace hexa_udon::protocol {

RequestRateLimiter::RequestRateLimiter(
    std::chrono::milliseconds minimum_interval, Now now, SleepUntil sleep_until)
    : minimum_interval_(minimum_interval), now_(std::move(now)), sleep_until_(std::move(sleep_until)) {}

Result<bool> RequestRateLimiter::acquire(std::optional<SteadyTime> deadline) {
    std::lock_guard lock(mutex_);
    auto current = now_();
    const auto send_at = next_allowed_ && *next_allowed_ > current ? *next_allowed_ : current;
    if (deadline && send_at > *deadline) {
        return Result<bool>::failure({ErrorCode::DeadlineExceeded, "rate limit wait exceeds deadline"});
    }
    if (send_at > current) {
        sleep_until_(send_at);
        current = now_();
        if (deadline && current > *deadline) {
            return Result<bool>::failure({ErrorCode::DeadlineExceeded, "deadline passed while rate limited"});
        }
    }
    next_allowed_ = current + minimum_interval_;
    return Result<bool>::success(true);
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

FileOperationLogger::FileOperationLogger(
    std::filesystem::path path, OperationLogEntry::Level minimum_level)
    : path_(std::move(path)), minimum_level_(minimum_level) {}

void FileOperationLogger::write(const OperationLogEntry& entry) noexcept {
    try {
        if (entry.level < minimum_level_) return;
        std::lock_guard lock(mutex_);
        nlohmann::json json{{"time", entry.timestamp_utc},
                            {"level", static_cast<int>(entry.level)},
                            {"operation", entry.operation},
                            {"method", entry.method},
                            {"path", entry.path},
                            {"elapsedMs", entry.elapsed.count()},
                            {"result", entry.result},
                            {"stateTransition", entry.state_transition}};
        json["endpoint"] = entry.endpoint;
        json["phase"] = entry.phase;
        json["attempt"] = entry.attempt;
        json["requestStartedAt"] = entry.request_started_utc.empty()
                                         ? nlohmann::json(nullptr)
                                         : nlohmann::json(entry.request_started_utc);
        json["nextRetryAt"] = entry.next_retry_utc.empty()
                                   ? nlohmann::json(nullptr)
                                   : nlohmann::json(entry.next_retry_utc);
        json["backoffReason"] = entry.backoff_reason;
        json["retryAfterMs"] = entry.retry_after_ms
                                    ? nlohmann::json(*entry.retry_after_ms)
                                    : nlohmann::json(nullptr);
        json["deadlineRemainingMs"] = entry.deadline_remaining_ms
                                           ? nlohmann::json(*entry.deadline_remaining_ms)
                                           : nlohmann::json(nullptr);
        json["stopReason"] = entry.stop_reason;
        json["responseClassification"] = entry.response_classification;
        json["submissionAttempted"] = entry.submission_attempted
                                            ? nlohmann::json(*entry.submission_attempted)
                                            : nlohmann::json(nullptr);
        json["day"] = entry.day ? nlohmann::json(*entry.day) : nlohmann::json(nullptr);
        json["localSubmissionId"] = entry.local_submission_id
                                        ? nlohmann::json(*entry.local_submission_id)
                                        : nlohmann::json(nullptr);
        json["httpStatus"] = entry.http_status ? nlohmann::json(*entry.http_status)
                                                : nlohmann::json(nullptr);
        json["revision"] = entry.revision ? nlohmann::json(*entry.revision)
                                           : nlohmann::json(nullptr);
        std::ofstream output(path_, std::ios::app);
        if (output) {
            output << json.dump() << '\n';
            output.flush();
        }
    } catch (...) {
        // Logging must never stop competition processing.
    }
}

}  // namespace hexa_udon::protocol
