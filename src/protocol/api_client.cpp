#include "hexa_udon/protocol/api_client.hpp"

#include <algorithm>
#include <string_view>
#include <charconv>
#include <limits>

namespace hexa_udon::protocol {
namespace {

std::string join_url(std::string base, const std::string& path) {
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + (path.empty() || path.front() == '/' ? path : "/" + path);
}

std::optional<std::int64_t> retry_after_milliseconds(const HttpResponse& response) {
    for (const auto& [name, value] : response.headers) {
        if (name != "retry-after") continue;
        std::int64_t seconds = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && seconds >= 0
            && seconds <= std::numeric_limits<std::int64_t>::max() / 1000)
            return seconds * 1000;
        return std::nullopt;
    }
    return std::nullopt;
}

std::string response_classification(long status) {
    if (status >= 200 && status < 300) return "success";
    if (status == 403 || status == 429) return "rate-limited-or-not-ready";
    if (status >= 400 && status < 500) return "client-error";
    if (status >= 500) return "server-error";
    return "unexpected-status";
}

std::string operation_name(const std::string& path, const std::string& phase) {
    if (phase == "daily-submit") return "daily-submit";
    if (phase == "daily-state") return "daily-state";
    if (path == "/setting") return "setting";
    if (path == "/agent") return "agent";
    return "actions-or-state";
}

}  // namespace

ProconApiClient::ProconApiClient(HttpTransport& transport, ApiConfig config,
                                 RequestRateLimiter* limiter, OperationLogger* logger)
    : transport_(transport),
      config_(std::move(config)),
      owned_limiter_(config_.minimum_request_interval),
      limiter_(limiter == nullptr ? &owned_limiter_ : limiter),
      logger_(logger == nullptr ? &null_logger_ : logger) {}

Error ProconApiClient::redact(Error error) const {
    if (!config_.token.empty()) {
        std::size_t offset = 0;
        while ((offset = error.message.find(config_.token, offset)) != std::string::npos) {
            error.message.replace(offset, config_.token.size(), "[REDACTED]");
            offset += std::string_view{"[REDACTED]"}.size();
        }
    }
    return error;
}

Result<HttpResponse> ProconApiClient::request(
    HttpMethod method, const std::string& path, const std::string& body,
    std::optional<SteadyTime> deadline, const std::string& phase) {
    auto permission = limiter_->acquire(deadline);
    if (!permission) {
        OperationLogEntry entry;
        entry.level = OperationLogEntry::Level::Warning;
        entry.timestamp_utc = utc_timestamp();
        entry.operation = operation_name(path, phase);
        entry.method = method == HttpMethod::Get ? "GET" : "POST";
        entry.path = path;
        entry.endpoint = path;
        entry.phase = phase;
        entry.attempt = 1;
        entry.result = "not-sent";
        entry.response_classification = "rate-limit-stop";
        entry.stop_reason = permission.error().message;
        if (deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now()).count();
            entry.deadline_remaining_ms = std::max<std::int64_t>(0, remaining);
        }
        logger_->write(entry);
        auto error = permission.error();
        if (method == HttpMethod::Post) error.submission_attempted = false;
        return Result<HttpResponse>::failure(std::move(error));
    }
    HttpRequest request{
        method,
        join_url(config_.base_url, path),
        {"Accept: application/json", "Procon-Token: " + config_.token},
        body,
        config_.connect_timeout,
        config_.total_timeout,
        config_.max_response_bytes,
        config_.user_agent,
    };
    if (method == HttpMethod::Post) {
        request.headers.push_back("Content-Type: application/json");
    }
    const auto started = std::chrono::steady_clock::now();
    auto response = transport_.execute(request);
    OperationLogEntry entry;
    entry.timestamp_utc = utc_timestamp();
    entry.operation = operation_name(path, phase);
    entry.method = method == HttpMethod::Get ? "GET" : "POST";
    entry.path = path;
    entry.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    entry.result = response ? "http-response" : "transport-error";
    entry.endpoint = path;
    entry.phase = phase;
    entry.attempt = 1;
    entry.request_started_utc = entry.timestamp_utc;
    if (deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *deadline - std::chrono::steady_clock::now()).count();
        entry.deadline_remaining_ms = std::max<std::int64_t>(0, remaining);
    }
    if (response) {
        entry.http_status = response.value().status;
        entry.retry_after_ms = retry_after_milliseconds(response.value());
        entry.response_classification = response_classification(response.value().status);
        if (response.value().status >= 400) entry.level = OperationLogEntry::Level::Warning;
    } else {
        entry.level = OperationLogEntry::Level::Error;
        entry.response_classification = "transport-error";
    }
    if (method == HttpMethod::Post) {
        entry.submission_attempted = response && response.value().status >= 200
            && response.value().status < 300 ? std::optional<bool>{true} : std::nullopt;
    }
    // A successful HTTP response is not yet a successful application-level POST:
    // callers still have to validate the response body. They write the final
    // true/null outcome below, so empty/decode-failed responses cannot be logged
    // as a successful submission.
    if (!(method == HttpMethod::Post && response && response.value().status >= 200
          && response.value().status < 300)) {
        logger_->write(entry);
    }
    return response;
}

void ProconApiClient::log_post_response(const std::string& path,
                                        const HttpResponse& response,
                                        const std::string& phase,
                                        std::optional<bool> attempted,
                                        const std::string& result) noexcept {
    OperationLogEntry entry;
    entry.timestamp_utc = utc_timestamp();
    entry.operation = operation_name(path, phase);
    entry.method = "POST";
    entry.path = path;
    entry.elapsed = response.elapsed;
    entry.result = result;
    entry.endpoint = path;
    entry.phase = phase;
    entry.attempt = 1;
    entry.request_started_utc = entry.timestamp_utc;
    entry.http_status = response.status;
    entry.retry_after_ms = retry_after_milliseconds(response);
    entry.response_classification = response_classification(response.status);
    entry.submission_attempted = attempted;
    if (response.status >= 400) entry.level = OperationLogEntry::Level::Warning;
    logger_->write(entry);
}

Error ProconApiClient::http_error(const HttpResponse& response) const {
    const ErrorCode code = response.status == 401   ? ErrorCode::Auth
                           : response.status == 403 ? ErrorCode::AccessTime
                           : response.status >= 500 ? ErrorCode::Http5xx
                                                    : ErrorCode::Http4xx;
    // Keep response bodies out of diagnostics. HTTP status and safe headers are enough
    // for retry classification; the body remains solely an internal decode input.
    return redact({code, "HTTP " + std::to_string(response.status), retry_after_milliseconds(response)});
}

Result<core::MatchConfig> ProconApiClient::get_setting(
    std::optional<SteadyTime> deadline, const std::string& phase) {
    auto response = request(HttpMethod::Get, "/setting", {}, deadline, phase);
    if (!response) {
        return Result<core::MatchConfig>::failure(redact(response.error()));
    }
    if (response.value().status != 200) {
        return Result<core::MatchConfig>::failure(http_error(response.value()));
    }
    if (response.value().body.empty()) {
        return Result<core::MatchConfig>::failure(
            {ErrorCode::EmptyBody, "empty setting response"});
    }
    return decode_match_setting(response.value().body);
}

Result<bool> ProconApiClient::post_agent_kinds(
    const std::vector<core::AgentKind>& kinds, std::optional<SteadyTime> deadline,
    const std::string& phase) {
    auto body = encode_agent_kinds(kinds);
    if (!body) {
        auto error = body.error();
        error.submission_attempted = false;
        return Result<bool>::failure(std::move(error));
    }
    std::lock_guard lock(post_mutex_);
    auto response = request(HttpMethod::Post, "/agent", body.value(), deadline, phase);
    if (!response) {
        if (response.error().code == ErrorCode::TransferTimeout ||
            response.error().code == ErrorCode::Disconnected) {
            return Result<bool>::failure({ErrorCode::UnknownResponse, "POST /agent outcome is unknown"});
        }
        return Result<bool>::failure(redact(response.error()));
    }
    if (response.value().status != 200) {
        auto error = http_error(response.value());
        error.submission_attempted = std::nullopt;
        return Result<bool>::failure(std::move(error));
    }
    log_post_response("/agent", response.value(), phase, true, "accepted");
    return Result<bool>::success(true);
}

Result<core::DailyState> ProconApiClient::get_state(
    const core::MatchConfig& config, std::optional<SteadyTime> deadline,
    const std::string& phase) {
    auto response = request(HttpMethod::Get, "/", {}, deadline, phase);
    if (!response) {
        return Result<core::DailyState>::failure(redact(response.error()));
    }
    if (response.value().status != 200) {
        return Result<core::DailyState>::failure(http_error(response.value()));
    }
    if (response.value().body.empty()) {
        return Result<core::DailyState>::failure({ErrorCode::EmptyBody, "empty state response"});
    }
    return decode_match_state(response.value().body, config);
}

Result<std::int32_t> ProconApiClient::post_actions(
    const simulator::DayActionPlan& plan, std::optional<SteadyTime> deadline,
    const std::string& phase) {
    auto body = encode_actions(plan);
    if (!body) {
        auto error = body.error();
        error.submission_attempted = false;
        return Result<std::int32_t>::failure(std::move(error));
    }
    std::lock_guard lock(post_mutex_);
    auto response = request(HttpMethod::Post, "/", body.value(), deadline, phase);
    if (!response) {
        const auto code = response.error().code;
        if (code == ErrorCode::TransferTimeout || code == ErrorCode::Disconnected) {
            return Result<std::int32_t>::failure(
                {ErrorCode::UnknownResponse, "POST outcome is unknown", std::nullopt, std::nullopt});
        }
        return Result<std::int32_t>::failure(redact(response.error()));
    }
    if (response.value().status != 200) {
        auto error = http_error(response.value());
        error.submission_attempted = std::nullopt;
        return Result<std::int32_t>::failure(std::move(error));
    }
    if (response.value().body.empty()) {
        log_post_response("/", response.value(), phase, std::nullopt, "empty-response");
        return Result<std::int32_t>::failure(
            {ErrorCode::EmptyBody, "empty revision response", std::nullopt, std::nullopt});
    }
    auto revision = decode_revision(response.value().body);
    if (!revision) {
        auto error = revision.error();
        error.submission_attempted = std::nullopt;
        log_post_response("/", response.value(), phase, std::nullopt, "decode-error");
        return Result<std::int32_t>::failure(std::move(error));
    }
    log_post_response("/", response.value(), phase, true, "accepted");
    return revision;
}

}  // namespace hexa_udon::protocol
