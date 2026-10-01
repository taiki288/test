#include "hexa_udon/protocol/api_client.hpp"

#include <nlohmann/json.hpp>

#include <string_view>

namespace hexa_udon::protocol {
namespace {

std::string join_url(std::string base, const std::string& path) {
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + (path.empty() || path.front() == '/' ? path : "/" + path);
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
    std::optional<SteadyTime> deadline) {
    auto permission = limiter_->acquire(deadline);
    if (!permission) {
        return Result<HttpResponse>::failure(permission.error());
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
    entry.operation = path == "/setting" ? "setting" : path == "/agent" ? "agent" : "actions-or-state";
    entry.method = method == HttpMethod::Get ? "GET" : "POST";
    entry.path = path;
    entry.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    entry.result = response ? "http-response" : "transport-error";
    if (response) {
        entry.http_status = response.value().status;
        if (response.value().status >= 400) entry.level = OperationLogEntry::Level::Warning;
    } else {
        entry.level = OperationLogEntry::Level::Error;
    }
    logger_->write(entry);
    return response;
}

Error ProconApiClient::http_error(const HttpResponse& response) const {
    const ErrorCode code = response.status == 401   ? ErrorCode::Auth
                           : response.status == 403 ? ErrorCode::AccessTime
                           : response.status >= 500 ? ErrorCode::Http5xx
                                                    : ErrorCode::Http4xx;
    std::string message = "HTTP " + std::to_string(response.status);
    try {
        const auto json = nlohmann::json::parse(response.body);
        if (json.contains("message") && json["message"].is_string()) {
            message += ": " + json["message"].get<std::string>().substr(0, 256);
        }
    } catch (...) {
        // Error bodies are optional and untrusted. The status remains authoritative.
    }
    return redact({code, message});
}

Result<core::MatchConfig> ProconApiClient::get_setting() {
    auto response = request(HttpMethod::Get, "/setting");
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
    const std::vector<core::AgentKind>& kinds, std::optional<SteadyTime> deadline) {
    auto body = encode_agent_kinds(kinds);
    if (!body) {
        return Result<bool>::failure(body.error());
    }
    std::lock_guard lock(post_mutex_);
    auto response = request(HttpMethod::Post, "/agent", body.value(), deadline);
    if (!response) {
        if (response.error().code == ErrorCode::TransferTimeout ||
            response.error().code == ErrorCode::Disconnected) {
            return Result<bool>::failure({ErrorCode::UnknownResponse, "POST /agent outcome is unknown"});
        }
        return Result<bool>::failure(redact(response.error()));
    }
    if (response.value().status != 200) {
        return Result<bool>::failure(http_error(response.value()));
    }
    return Result<bool>::success(true);
}

Result<core::DailyState> ProconApiClient::get_state(const core::MatchConfig& config) {
    auto response = request(HttpMethod::Get, "/");
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
    const simulator::DayActionPlan& plan, std::optional<SteadyTime> deadline) {
    auto body = encode_actions(plan);
    if (!body) {
        return Result<std::int32_t>::failure(body.error());
    }
    std::lock_guard lock(post_mutex_);
    auto response = request(HttpMethod::Post, "/", body.value(), deadline);
    if (!response) {
        const auto code = response.error().code;
        if (code == ErrorCode::TransferTimeout || code == ErrorCode::Disconnected) {
            return Result<std::int32_t>::failure(
                {ErrorCode::UnknownResponse, "POST outcome is unknown"});
        }
        return Result<std::int32_t>::failure(redact(response.error()));
    }
    if (response.value().status != 200) {
        return Result<std::int32_t>::failure(http_error(response.value()));
    }
    if (response.value().body.empty()) {
        return Result<std::int32_t>::failure(
            {ErrorCode::EmptyBody, "empty revision response"});
    }
    return decode_revision(response.value().body);
}

}  // namespace hexa_udon::protocol
