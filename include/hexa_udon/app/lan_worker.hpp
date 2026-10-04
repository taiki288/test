#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace hexa_udon::app {

struct LanWorkerEndpoint {
    std::string host;
    unsigned short port = 0;
};

struct LanWorkerConfig {
    LanWorkerEndpoint listen;
    std::string secret_environment;
    std::size_t maximum_frame_bytes = 262144;
};

struct LanWorkerReply {
    bool success = false;
    nlohmann::json payload;
    std::string error;
};

[[nodiscard]] std::optional<LanWorkerEndpoint> parse_lan_worker_endpoint(
    const std::string& value);
[[nodiscard]] bool is_allowed_worker_address(const std::string& host);
[[nodiscard]] std::string worker_auth_digest(const std::string& secret,
                                              const nlohmann::json& request);

[[nodiscard]] LanWorkerReply request_lan_worker(
    const LanWorkerEndpoint& endpoint,
    const std::string& secret,
    const nlohmann::json& request,
    std::chrono::milliseconds timeout,
    std::size_t maximum_frame_bytes = 262144);

int run_lan_worker(const LanWorkerConfig& config,
                   const std::function<bool()>& stop_requested,
                   std::ostream& output);

}  // namespace hexa_udon::app
