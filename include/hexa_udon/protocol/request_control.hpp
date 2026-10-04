#pragma once

#include "hexa_udon/protocol/result.hpp"

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hexa_udon::protocol {

using SteadyTime = std::chrono::steady_clock::time_point;

class RequestRateLimiter {
public:
    using Now = std::function<SteadyTime()>;
    using SleepUntil = std::function<void(SteadyTime)>;

    explicit RequestRateLimiter(
        std::chrono::milliseconds minimum_interval = std::chrono::milliseconds{250},
        Now now = [] { return std::chrono::steady_clock::now(); },
        SleepUntil sleep_until = [](SteadyTime time) { std::this_thread::sleep_until(time); });

    [[nodiscard]] Result<bool> acquire(std::optional<SteadyTime> deadline = std::nullopt);

private:
    std::chrono::milliseconds minimum_interval_;
    Now now_;
    SleepUntil sleep_until_;
    std::mutex mutex_;
    std::optional<SteadyTime> next_allowed_;
};

struct OperationLogEntry {
    enum class Level { Info, Warning, Error };
    Level level = Level::Info;
    std::string timestamp_utc;
    std::string operation;
    std::string method;
    std::string path;
    std::optional<std::int32_t> day;
    std::optional<std::uint64_t> local_submission_id;
    std::optional<long> http_status;
    std::optional<std::int32_t> revision;
    std::chrono::milliseconds elapsed{0};
    std::string result;
    std::string state_transition;
    std::string endpoint;
    std::string phase;
    std::size_t attempt = 0;
    std::string request_started_utc;
    std::string next_retry_utc;
    std::string backoff_reason;
    std::optional<std::int64_t> retry_after_ms;
    std::optional<std::int64_t> deadline_remaining_ms;
    std::string stop_reason;
    std::string response_classification;
    std::optional<bool> submission_attempted;
};

class OperationLogger {
public:
    virtual ~OperationLogger() = default;
    virtual void write(const OperationLogEntry& entry) noexcept = 0;
};

class NullOperationLogger final : public OperationLogger {
public:
    void write(const OperationLogEntry&) noexcept override {}
};

class FileOperationLogger final : public OperationLogger {
public:
    explicit FileOperationLogger(
        std::filesystem::path path,
        OperationLogEntry::Level minimum_level = OperationLogEntry::Level::Info);
    void write(const OperationLogEntry& entry) noexcept override;

private:
    std::filesystem::path path_;
    OperationLogEntry::Level minimum_level_;
    std::mutex mutex_;
};

[[nodiscard]] std::string utc_timestamp();

}  // namespace hexa_udon::protocol
