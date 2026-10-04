#include "hexa_udon/app/lan_worker.hpp"
#include "hexa_udon/optimizer/daily_deadline_policy.hpp"
#include "hexa_udon/protocol/json_codec.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <set>

namespace hexa_udon::app {
namespace {

constexpr int protocol_version = 1;

std::string hex_digest(std::string_view value) {
    // This is an identity/MAC token for the local protocol, not a password
    // transport. The shared secret is never placed in a JSON frame or log.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

bool write_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::optional<std::string> read_line(int fd, std::size_t maximum) {
    std::string result;
    result.reserve(std::min<std::size_t>(maximum, 4096));
    std::array<char, 1024> buffer{};
    while (result.size() <= maximum) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) return std::nullopt;
        result.append(buffer.data(), static_cast<std::size_t>(count));
        const auto end = result.find('\n');
        if (end != std::string::npos) {
            if (end > maximum) return std::nullopt;
            result.resize(end);
            return result;
        }
    }
    return std::nullopt;
}

bool wait_fd(int fd, bool writable, std::chrono::milliseconds timeout) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
    const auto result = ::select(fd + 1, writable ? nullptr : &set,
                                 writable ? &set : nullptr, nullptr, &tv);
    return result > 0 && FD_ISSET(fd, &set);
}

nlohmann::json response_failure(const std::string& reason) {
    return { {"protocolVersion", protocol_version}, {"success", false},
             {"termination", "worker-failure"}, {"failureReason", reason},
             {"futureSnapshotRead", false}, {"lookahead", 0},
             {"networkRequests", 0}, {"postCount", 0} };
}

struct DecodedInput {
    core::MatchConfig match;
    core::DailyState daily;
    simulator::MatchProgress progress;
    std::vector<core::AgentKind> types;
    std::uint64_t seed = 30013;
    std::chrono::milliseconds budget{0};
};

std::optional<DecodedInput> decode_input(const nlohmann::json& request, std::string& error) {
    try {
        const auto& input = request.at("plannerInput");
        static const std::set<std::string> allowed_input{
            "map", "startsAt", "daySeconds", "daySteps", "spots", "initialPositions",
            "fuelLimit", "players", "busyThreshold", "jammedThreshold", "daily", "progress",
            "types", "plannerSeed", "workerBudgetMs"};
        for (const auto& item : input.items())
            if (!allowed_input.contains(item.key())) { error = "unknown planner input field"; return std::nullopt; }
        const auto& map = input.at("map");
        std::vector<core::Terrain> cells;
        for (const auto& value : map.at("cells")) {
            const auto terrain = core::terrain_from_int(value.get<std::int32_t>());
            if (!terrain) { error = "invalid map terrain"; return std::nullopt; }
            cells.push_back(*terrain);
        }
        auto map_result = core::MapDefinition::create(map.at("height"), map.at("width"), std::move(cells));
        if (!map_result) { error = "invalid map"; return std::nullopt; }
        const auto starts_at = input.value("startsAt", 0LL);
        const auto day_seconds = input.at("daySeconds").get<std::vector<core::Quantity>>();
        const auto day_steps = input.at("daySteps").get<std::vector<core::Quantity>>();
        std::vector<core::Spot> spots;
        for (const auto& spot : input.at("spots")) spots.push_back({spot.at("brand"), {spot.at("position")}, spot.at("stock")});
        std::vector<core::CellIndex> initial_positions;
        for (const auto& position : input.at("initialPositions")) initial_positions.push_back({position.get<std::int32_t>()});
        const core::MatchConfig match{starts_at, day_seconds, day_steps, std::move(map_result).value(),
            std::move(spots), std::move(initial_positions), input.at("fuelLimit"), input.value("players", 1),
            input.value("busyThreshold", 1), input.value("jammedThreshold", 2)};
        core::DailyState daily{}; daily.ends_at = input.at("daily").at("endsAt"); daily.day = input.at("daily").at("day");
        for (const auto& agent : input.at("daily").at("agents")) {
            const auto kind = core::agent_kind_from_int(agent.at("kind"));
            if (!kind) { error = "invalid agent kind"; return std::nullopt; }
            daily.own_agents.push_back({*kind, {agent.at("position")}, agent.at("fuel")});
        }
        for (const auto& road : input.at("daily").at("traffic")) {
            const auto status = core::road_status_from_int(road.at("status"));
            if (!status) { error = "invalid road status"; return std::nullopt; }
            daily.traffic.push_back({{road.at("position")}, *status});
        }
        simulator::MatchProgress progress{};
        for (const auto& brand : input.at("progress").at("acquiredBrands")) progress.acquired_brands.insert(brand.get<core::Quantity>());
        progress.total_balls = input.at("progress").at("totalBalls");
        progress.daily_distinct_brand_counts = input.at("progress").at("dailyDistinctBrandCounts").get<std::vector<core::Quantity>>();
        std::vector<core::AgentKind> types;
        for (const auto& value : input.at("types")) {
            const auto kind = core::agent_kind_from_int(value.get<std::int32_t>());
            if (!kind) { error = "invalid type"; return std::nullopt; }
            types.push_back(*kind);
        }
        if (types.size() != daily.own_agents.size() || daily.own_agents.size() != match.initial_agent_positions.size()) {
            error = "agent count mismatch"; return std::nullopt;
        }
        const auto size = static_cast<std::size_t>(match.map.height());
        const auto supply = static_cast<std::size_t>(std::count(types.begin(), types.end(), core::AgentKind::Supply));
        if ((size != 16 && size != 24 && size != 32) || (size == 32 && supply != 1 && supply != 3)) {
            error = "type allowlist rejected"; return std::nullopt;
        }
        DecodedInput decoded{std::move(match), std::move(daily), std::move(progress), std::move(types),
                              input.at("plannerSeed"), std::chrono::milliseconds{input.at("workerBudgetMs").get<std::int64_t>()}};
        if (decoded.budget.count() <= 0 || decoded.budget > std::chrono::minutes{1}) { error = "invalid worker budget"; return std::nullopt; }
        if (!core::validate(decoded.match).empty() || !core::validate(decoded.daily, decoded.match).empty()) {
            error = "model validation failed"; return std::nullopt;
        }
        return decoded;
    } catch (...) { error = "planner input schema invalid"; return std::nullopt; }
}

nlohmann::json score_json(const planner::OfficialScore& score) {
    return {score.total_unique_brands, score.cumulative_daily_unique_brands, score.total_bowls};
}

std::string value_hash(const nlohmann::json& value) { return hex_digest(value.dump()); }

nlohmann::json agents_json(const std::vector<core::AgentState>& agents) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& agent : agents) {
        result.push_back({{"kind", core::to_int(agent.kind)},
                          {"position", agent.position.value}, {"fuel", agent.fuel}});
    }
    return result;
}

}  // namespace

std::optional<LanWorkerEndpoint> parse_lan_worker_endpoint(const std::string& value) {
    const auto separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) return std::nullopt;
    try {
        const auto port = std::stoul(value.substr(separator + 1));
        if (port == 0 || port > 65535) return std::nullopt;
        LanWorkerEndpoint endpoint{value.substr(0, separator), static_cast<unsigned short>(port)};
        if (!is_allowed_worker_address(endpoint.host)) return std::nullopt;
        return endpoint;
    } catch (...) {
        return std::nullopt;
    }
}

bool is_allowed_worker_address(const std::string& host) {
    if (host == "localhost" || host == "127.0.0.1" || host == "::1") return true;
    in_addr address{};
    if (::inet_pton(AF_INET, host.c_str(), &address) != 1) return false;
    const auto value = ntohl(address.s_addr);
    return (value >> 24U) == 10U || (value >> 20U) == 0xAC1U
        || (value >> 16U) == 0xC0A8U;
}

std::string worker_auth_digest(const std::string& secret, const nlohmann::json& request) {
    nlohmann::json canonical = request;
    canonical.erase("auth");
    return hex_digest(secret + "\n" + canonical.dump());
}

LanWorkerReply request_lan_worker(const LanWorkerEndpoint& endpoint,
                                  const std::string& secret,
                                  const nlohmann::json& request,
                                  std::chrono::milliseconds timeout,
                                  std::size_t maximum_frame_bytes) {
    if (secret.empty()) return {false, {}, "worker secret is empty"};
    if (request.dump().size() > maximum_frame_bytes) return {false, {}, "request is oversized"};
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    const auto service = std::to_string(endpoint.port);
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &resolved) != 0) {
        return {false, {}, "worker address resolution failed"};
    }
    int fd = -1;
    for (auto* item = resolved; item != nullptr; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) continue;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const auto connected = ::connect(fd, item->ai_addr, item->ai_addrlen) == 0
            || (errno == EINPROGRESS && wait_fd(fd, true, timeout));
        if (connected) {
            ::fcntl(fd, F_SETFL, flags);
            timeval tv{};
            tv.tv_sec = static_cast<long>(timeout.count() / 1000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            break;
        }
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(resolved);
    if (fd < 0) return {false, {}, "worker connection failed"};
    nlohmann::json frame = request;
    frame["protocolVersion"] = protocol_version;
    frame["auth"] = worker_auth_digest(secret, frame);
    const auto encoded = frame.dump() + "\n";
    if (!wait_fd(fd, true, timeout) || !write_all(fd, encoded)) {
        ::close(fd); return {false, {}, "worker write timeout"};
    }
    if (!wait_fd(fd, false, timeout)) { ::close(fd); return {false, {}, "worker reply timeout"}; }
    const auto line = read_line(fd, maximum_frame_bytes);
    ::close(fd);
    if (!line) return {false, {}, "worker reply missing or oversized"};
    try {
        const auto payload = nlohmann::json::parse(*line);
        if (payload.value("protocolVersion", 0) != protocol_version) return {false, {}, "worker protocol mismatch"};
        return {payload.value("success", false), payload, payload.value("failureReason", "")};
    } catch (...) { return {false, {}, "worker reply malformed"}; }
}

int run_lan_worker(const LanWorkerConfig& config, const std::function<bool()>& stop_requested,
                   std::ostream& output) {
    if (!is_allowed_worker_address(config.listen.host) || config.listen.port == 0) {
        output << "worker-error=address-not-private\n"; return 2;
    }
    const char* secret = std::getenv(config.secret_environment.c_str());
    if (secret == nullptr || *secret == '\0') { output << "worker-error=secret-missing\n"; return 2; }
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { output << "worker-error=socket\n"; return 1; }
    int reuse = 1; ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config.listen.port);
    const auto bind_result = config.listen.host == "localhost"
        ? (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1)
        : (::inet_pton(AF_INET, config.listen.host.c_str(), &address.sin_addr) == 1);
    if (!bind_result
        || ::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
        || ::listen(server, 4) != 0) { ::close(server); output << "worker-error=listen\n"; return 1; }
    output << "worker=ready address=" << config.listen.host << ':' << config.listen.port << "\n";
    while (!stop_requested()) {
        if (!wait_fd(server, false, std::chrono::milliseconds{100})) continue;
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) continue;
        const auto line = read_line(client, config.maximum_frame_bytes);
        nlohmann::json reply;
        if (!line) reply = response_failure("malformed-or-oversized-request");
        else try {
            const auto request = nlohmann::json::parse(*line);
            static const std::set<std::string> allowed_request{
                "protocolVersion", "requestId", "auth", "evaluatorVersion", "day", "size",
                "agentCount", "typeIdentity", "snapshotIdentity", "mapIdentity", "stateIdentity",
                "policyIdentity", "workerBudgetMs", "futureSnapshotRead", "lookahead", "startAgents",
                "traffic", "plannerInput", "payloadHash"};
            bool unknown = false;
            for (const auto& item : request.items()) unknown = unknown || !allowed_request.contains(item.key());
            const auto supplied = request.value("auth", "");
            if (request.value("protocolVersion", 0) != protocol_version
                || supplied != worker_auth_digest(secret, request) || unknown) {
                reply = response_failure("authentication-or-protocol-mismatch");
            } else if (request.value("futureSnapshotRead", false)
                       || request.value("lookahead", 0) != 0) {
                reply = response_failure("future-snapshot-forbidden");
            } else {
                std::string decode_error;
                const auto decoded = decode_input(request, decode_error);
                if (!decoded) {
                    reply = response_failure(decode_error);
                } else {
                    if (request.value("day", -1) != decoded->daily.day
                        || request.value("size", 0) != decoded->match.map.height()
                        || request.value("agentCount", 0) != static_cast<int>(decoded->daily.own_agents.size())
                        || request.value("payloadHash", "") != value_hash(request.at("plannerInput"))
                        || request.value("futureSnapshotRead", true)
                        || request.value("lookahead", 1) != 0) {
                        reply = response_failure("planner-input-identity-mismatch");
                        const auto encoded = reply.dump() + "\n";
                        static_cast<void>(write_all(client, encoded));
                        ::close(client);
                        continue;
                    }
                    const auto started = std::chrono::steady_clock::now();
                    const planner::PlannerInput planner_input{decoded->match, decoded->daily, decoded->progress};
                    const auto deadline = started + decoded->budget;
                    planner::PlannerConfig planner_config{2000, decoded->seed};
                    auto baseline = planner::make_greedy_plan(planner_input, planner_config, deadline);
                    if (!baseline) {
                        reply = response_failure("baseline-planner-failure");
                    } else {
                        planner::RefuelPlannerConfig refuel_config{};
                        refuel_config.seed = decoded->seed;
                        auto refuel = planner::make_refuel_plan(planner_input, baseline.value(), refuel_config, deadline);
                        if (!refuel) {
                            reply = response_failure("refuel-planner-failure");
                        } else {
                            optimizer::OptimizerConfig optimizer_config{};
                            optimizer_config.seed = decoded->seed;
                            optimizer_config.prefer_daily_readiness_on_tie = true;
                            auto optimized = optimizer::optimize(planner_input, baseline.value(), refuel.value(),
                                                                 optimizer_config, deadline);
                            if (!optimized) {
                                reply = response_failure("optimizer-failure");
                            } else {
                                const auto encoded = protocol::encode_actions(optimized.value().plan);
                                if (!encoded) {
                                    reply = response_failure("action-encoding-failure");
                                } else {
                                    const auto actions = nlohmann::json::parse(encoded.value());
                                    const auto readiness = optimized.value().readiness;
                                    const nlohmann::json end_state = agents_json(
                                        optimized.value().simulation.end_agents);
                                    const nlohmann::json start_state = agents_json(decoded->daily.own_agents);
                                    const auto action_hash = value_hash(actions);
                                    const auto plan_hash = value_hash({{"actions", actions}, {"types", decoded->types}});
                                    reply = {{"protocolVersion", protocol_version}, {"success", true},
                                             {"requestId", request.value("requestId", "")},
                                             {"evaluatorVersion", request.value("evaluatorVersion", "")},
                                             {"payloadHash", value_hash(request.at("plannerInput"))},
                                             {"inputHash", value_hash(request.at("plannerInput"))},
                                             {"startStateHash", value_hash(start_state)},
                                             {"candidateValidated", true}, {"actions", actions},
                                             {"actionHash", action_hash}, {"planHash", plan_hash},
                                             {"endState", end_state}, {"endStateHash", value_hash(end_state)},
                                             {"officialScore", score_json(optimized.value().score)},
                                             {"readiness", {readiness.uncollected_spot_reachability,
                                                readiness.fuel_reserve, readiness.patrol_dispersion,
                                                readiness.rendezvous_readiness}},
                                             {"evaluatedCandidates", optimized.value().generated_candidates},
                                             {"validCandidates", optimized.value().valid_candidates},
                                             {"acceptedCandidates", optimized.value().accepted_candidates},
                                             {"runtimeUs", optimized.value().elapsed.count()},
                                             {"termination", "completed"}, {"failureReason", ""},
                                             {"futureSnapshotRead", false}, {"lookahead", 0},
                                             {"networkRequests", 0}, {"postCount", 0}};
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) { reply = response_failure("malformed-json"); }
        const auto encoded = reply.dump() + "\n";
        static_cast<void>(write_all(client, encoded));
        ::close(client);
    }
    ::close(server);
    output << "worker=stopped\n";
    return 0;
}

}  // namespace hexa_udon::app
