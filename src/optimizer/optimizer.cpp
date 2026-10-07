#include "hexa_udon/optimizer/optimizer.hpp"
#include "hexa_udon/core/hex_distance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

namespace hexa_udon::optimizer {
namespace {

constexpr std::size_t neighborhood_count = static_cast<std::size_t>(Neighborhood::Count);

const char* neighborhood_name(const Neighborhood n) {
    switch (n) {
    case Neighborhood::SwapWithin: return "SwapWithin";
    case Neighborhood::RelocateWithin: return "RelocateWithin";
    case Neighborhood::AddSpot: return "AddSpot";
    case Neighborhood::AddUncollectedBrand: return "AddUncollectedBrand";
    case Neighborhood::RemoveSpot: return "RemoveSpot";
    case Neighborhood::ReplaceSameBrand: return "ReplaceSameBrand";
    case Neighborhood::MoveBetweenPatrols: return "MoveBetweenPatrols";
    case Neighborhood::SwapBetweenPatrols: return "SwapBetweenPatrols";
    case Neighborhood::ReverseSubsequence: return "ReverseSubsequence";
    case Neighborhood::SetFastest: return "SetFastest";
    case Neighborhood::SetFuelEfficient: return "SetFuelEfficient";
    case Neighborhood::MoveRendezvousCell: return "MoveRendezvousCell";
    case Neighborhood::AdjustRendezvousWait: return "AdjustRendezvousWait";
    case Neighborhood::ChangeSupply: return "ChangeSupply";
    case Neighborhood::SwapSupplyTasks: return "SwapSupplyTasks";
    case Neighborhood::MoveTaskBetweenSupplies: return "MoveTaskBetweenSupplies";
    case Neighborhood::RemoveRendezvous: return "RemoveRendezvous";
    case Neighborhood::AddRendezvous: return "AddRendezvous";
    case Neighborhood::ReplaceAndRelocate: return "ReplaceAndRelocate";
    case Neighborhood::Count: return "Count";
    }
    return "unknown";
}

std::string digest(const std::string& value) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : value) { h ^= c; h *= 1099511628211ULL; }
    std::ostringstream out;
    out << std::hex << h;
    return out.str();
}

std::string plan_digest(const simulator::DayActionPlan& plan) {
    std::ostringstream value;
    for (const auto& actions : plan) {
        value << '[';
        for (const auto& action : actions) {
            if (const auto* move = std::get_if<simulator::MoveAction>(&action))
                value << 'm' << static_cast<int>(move->direction);
            else value << 'w' << std::get<simulator::WaitAction>(action).steps;
            value << ',';
        }
        value << ']';
    }
    return digest(value.str());
}

std::string solution_digest(const StructuredSolution& solution) {
    std::ostringstream value;
    for (const auto& route : solution.patrol_routes) {
        value << route.agent_index << ':';
        for (std::size_t i = 0; i < route.spot_indices.size(); ++i)
            value << route.spot_indices[i] << '/' << static_cast<int>(route.objectives[i]) << ',';
        value << ';';
    }
    for (const auto& task : solution.rendezvous)
        value << task.patrol_agent << ',' << task.supply_agent << ',' << task.cell.value << ',' << task.wait_adjustment << ';';
    return digest(value.str());
}

std::set<core::Quantity> acquired_brands(const planner::PlannerInput& input) {
    return {input.previous_progress.acquired_brands.begin(), input.previous_progress.acquired_brands.end()};
}

template <class T> bool checked_add(const T a, const T b, T& result) {
    if (b > std::numeric_limits<T>::max() - a) return false;
    result = static_cast<T>(a + b); return true;
}
template <class T> bool checked_multiply(const T a, const T b, T& result) {
    if (a != 0 && b > std::numeric_limits<T>::max() / a) return false;
    result = static_cast<T>(a * b); return true;
}

std::size_t pick(std::mt19937_64& random, const std::size_t size) {
    return std::uniform_int_distribution<std::size_t>(0, size - 1)(random);
}

std::vector<std::size_t> supply_indices(const planner::PlannerInput& input) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < input.daily.own_agents.size(); ++i)
        if (input.daily.own_agents[i].kind == core::AgentKind::Supply) result.push_back(i);
    return result;
}

void normalize(PatrolRoutePlan& route) {
    route.objectives.resize(route.spot_indices.size(), pathfinding::RouteObjective::Fastest);
}

struct BuiltCandidate {
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    planner::OfficialScore score;
    planner::InternalTieBreak tie;
    planner::DailyReadiness readiness;
};

struct RouteOccurrence {
    core::CellIndex cell;
    std::int64_t step;
    std::size_t action_offset;
};

std::optional<BuiltCandidate> build_candidate(
    const planner::PlannerInput& input, const StructuredSolution& solution,
    pathfinding::Pathfinder& pathfinder, std::string& diagnostic) {
    const auto day = static_cast<std::size_t>(input.daily.day);
    const auto day_steps = input.match.day_steps[day];
    simulator::DayActionPlan plan(input.daily.own_agents.size());
    planner::InternalTieBreak tie;
    std::vector<std::int64_t> elapsed_by_agent(plan.size(), 0);
    std::vector<std::vector<RouteOccurrence>> occurrences(plan.size());
    std::vector<core::Quantity> assigned_counts(input.match.spots.size(), 0);
    for (const auto& route_plan : solution.patrol_routes) {
        if (route_plan.agent_index >= plan.size()
            || input.daily.own_agents[route_plan.agent_index].kind != core::AgentKind::Patrol
            || route_plan.objectives.size() != route_plan.spot_indices.size()) return std::nullopt;
        auto position = input.daily.own_agents[route_plan.agent_index].position;
        std::int64_t elapsed = 0;
        std::int64_t fuel = 0;
        occurrences[route_plan.agent_index].push_back({position, 0, 0});
        for (std::size_t leg = 0; leg < route_plan.spot_indices.size(); ++leg) {
            const auto spot = route_plan.spot_indices[leg];
            if (spot >= input.match.spots.size()
                || std::find(route_plan.spot_indices.begin(),
                             route_plan.spot_indices.begin() + static_cast<std::ptrdiff_t>(leg),
                             spot) != route_plan.spot_indices.begin() + static_cast<std::ptrdiff_t>(leg)
                || ++assigned_counts[spot] > input.match.spots[spot].max_stock) return std::nullopt;
            auto found = pathfinder.find_route(position, input.match.spots[spot].position,
                                               route_plan.objectives[leg]);
            if (!found) { diagnostic = found.error().message; return std::nullopt; }
            if (!found.value()) return std::nullopt;
            const auto& segment = *found.value();
            elapsed += segment.cost.travel_steps;
            fuel += segment.cost.patrol_fuel;
            if (elapsed > day_steps
                || (solution.rendezvous.empty()
                    && fuel > input.daily.own_agents[route_plan.agent_index].fuel))
                return std::nullopt;
            for (std::size_t edge = 0; edge < segment.directions.size(); ++edge) {
                plan[route_plan.agent_index].emplace_back(
                    simulator::MoveAction{segment.directions[edge]});
                auto one_edge = pathfinder.find_route(segment.cells[edge], segment.cells[edge + 1],
                                                       pathfinding::RouteObjective::Fastest);
                if (!one_edge || !one_edge.value()) return std::nullopt;
                const auto prior = occurrences[route_plan.agent_index].back().step;
                occurrences[route_plan.agent_index].push_back({segment.cells[edge + 1],
                    prior + one_edge.value()->cost.travel_steps,
                    plan[route_plan.agent_index].size()});
            }
            tie.total_edges += segment.cost.edge_count;
            position = segment.destination;
            tie.deterministic_order.push_back(static_cast<std::int32_t>(route_plan.agent_index));
            tie.deterministic_order.push_back(static_cast<std::int32_t>(spot));
        }
        elapsed_by_agent[route_plan.agent_index] = elapsed;
        tie.total_travel_steps += elapsed;
    }
    if (!solution.rendezvous.empty()) {
        std::vector<core::CellIndex> supply_position;
        supply_position.reserve(input.daily.own_agents.size());
        for (const auto& agent : input.daily.own_agents) supply_position.push_back(agent.position);
        std::vector<std::int64_t> patrol_delay(plan.size(), 0);
        std::vector<std::int64_t> patrol_last_occurrence(plan.size(), -1);
        std::vector<std::vector<std::size_t>> inserted_offsets(plan.size());
        for (const auto& task : solution.rendezvous) {
        if (task.patrol_agent >= plan.size() || task.supply_agent >= plan.size()
            || input.daily.own_agents[task.patrol_agent].kind != core::AgentKind::Patrol
            || input.daily.own_agents[task.supply_agent].kind != core::AgentKind::Supply)
            return std::nullopt;
        const auto occurrence = std::find_if(occurrences[task.patrol_agent].begin(),
            occurrences[task.patrol_agent].end(), [&task](const auto& item) { return item.cell == task.cell; });
        if (occurrence == occurrences[task.patrol_agent].end()
            || occurrence->step < patrol_last_occurrence[task.patrol_agent]) return std::nullopt;
        auto supply_route = pathfinder.find_route(supply_position[task.supply_agent],
            task.cell, pathfinding::RouteObjective::Fastest);
        if (!supply_route || !supply_route.value()) return std::nullopt;
        for (const auto direction : supply_route.value()->directions)
            plan[task.supply_agent].emplace_back(simulator::MoveAction{direction});
        const auto patrol_arrival = occurrence->step + patrol_delay[task.patrol_agent];
        const auto supply_arrival = elapsed_by_agent[task.supply_agent]
            + supply_route.value()->cost.travel_steps;
        auto meeting_step = std::max<std::int64_t>({1, patrol_arrival, supply_arrival});
        meeting_step = std::max<std::int64_t>(1, meeting_step + task.wait_adjustment);
        if (meeting_step < patrol_arrival || meeting_step < supply_arrival) return std::nullopt;
        const auto patrol_wait = meeting_step - patrol_arrival;
        if (patrol_wait > 0) {
            const auto prior_insertions = static_cast<std::size_t>(std::count_if(
                inserted_offsets[task.patrol_agent].begin(), inserted_offsets[task.patrol_agent].end(),
                [&occurrence](const auto offset) { return offset <= occurrence->action_offset; }));
            plan[task.patrol_agent].insert(
                plan[task.patrol_agent].begin()
                    + static_cast<std::ptrdiff_t>(occurrence->action_offset + prior_insertions),
                simulator::WaitAction{static_cast<core::Quantity>(patrol_wait)});
            elapsed_by_agent[task.patrol_agent] += patrol_wait;
            patrol_delay[task.patrol_agent] += patrol_wait;
            inserted_offsets[task.patrol_agent].push_back(occurrence->action_offset);
        }
        const auto supply_wait = meeting_step - supply_arrival;
        if (supply_wait > 0)
            plan[task.supply_agent].emplace_back(
                simulator::WaitAction{static_cast<core::Quantity>(supply_wait)});
        elapsed_by_agent[task.supply_agent] = meeting_step;
        supply_position[task.supply_agent] = task.cell;
        patrol_last_occurrence[task.patrol_agent] = occurrence->step;
        tie.total_travel_steps += supply_route.value()->cost.travel_steps;
        }
    }
    for (std::size_t agent = 0; agent < plan.size(); ++agent) {
        if (elapsed_by_agent[agent] > day_steps) return std::nullopt;
        const auto remaining = static_cast<std::int64_t>(day_steps) - elapsed_by_agent[agent];
        if (remaining > 0)
            plan[agent].emplace_back(simulator::WaitAction{static_cast<core::Quantity>(remaining)});
    }
    auto simulated = simulator::simulate_day({input.match.map, input.match.spots,
        input.match.fuel_limit, day_steps, input.daily.own_agents, input.daily.traffic}, plan,
        simulator::TraceMode::Enabled);
    if (!simulated) { diagnostic = simulated.error().message; return std::nullopt; }
    if (!solution.rendezvous.empty()) {
        const bool confirmed = std::all_of(solution.rendezvous.begin(), solution.rendezvous.end(),
            [&simulated](const auto& task) {
                return std::any_of(simulated.value().trace.begin(), simulated.value().trace.end(),
                    [&task](const auto& event) {
                return event.kind == simulator::EventKind::Refueled
                    && event.agent_index == task.patrol_agent && event.new_position == task.cell;
                    });
            });
        if (!confirmed) { diagnostic = "rendezvous was not confirmed by Simulator trace"; return std::nullopt; }
    }
    for (const auto& agent : simulated.value().end_agents)
        if (agent.kind == core::AgentKind::Patrol) tie.patrol_fuel_remaining += agent.fuel;
    auto result = BuiltCandidate{std::move(plan), std::move(simulated).value(), {}, std::move(tie), {}};
    result.score = planner::official_score(input.previous_progress, result.simulation);
    std::vector<std::vector<std::size_t>> visits(input.daily.own_agents.size());
    for (const auto& route : solution.patrol_routes) visits[route.agent_index] = route.spot_indices;
    result.readiness = planner::daily_readiness(input.match, input.daily, result.simulation, visits);
    return result;
}

bool better(const BuiltCandidate& a, const BuiltCandidate& b, const bool prefer_daily_readiness_on_tie) {
    if (a.score != b.score) return planner::better_official_score(a.score, b.score);
    if (prefer_daily_readiness_on_tie) {
        if (planner::better_daily_readiness(a.readiness, b.readiness)) return true;
        if (planner::better_daily_readiness(b.readiness, a.readiness)) return false;
    }
    return planner::better_internal_tie_break(a.tie, b.tie);
}

double temperature(const OptimizerConfig& config, const std::size_t iteration) {
    if (config.maximum_iterations <= 1) return config.final_temperature;
    const double fraction = static_cast<double>(iteration)
        / static_cast<double>(config.maximum_iterations - 1);
    if (config.cooling == CoolingSchedule::Linear)
        return config.initial_temperature + (config.final_temperature - config.initial_temperature) * fraction;
    return config.initial_temperature * std::pow(config.final_temperature / config.initial_temperature, fraction);
}

ScoreBounds score_bounds(const planner::PlannerInput& input) {
    std::set<core::Quantity> brands(input.previous_progress.acquired_brands.begin(),
                                    input.previous_progress.acquired_brands.end());
    std::uint64_t stock = static_cast<std::uint64_t>(input.previous_progress.total_balls);
    for (const auto& spot : input.match.spots) { brands.insert(spot.brand); stock += static_cast<std::uint64_t>(spot.max_stock); }
    const auto previous_daily = std::accumulate(input.previous_progress.daily_distinct_brand_counts.begin(),
        input.previous_progress.daily_distinct_brand_counts.end(), std::uint64_t{0});
    return {static_cast<std::uint64_t>(brands.size()), previous_daily + brands.size(), stock};
}

}  // namespace

OrdinalResult::OrdinalResult(std::variant<std::uint64_t, OrdinalFailure> storage) : storage_(std::move(storage)) {}
OrdinalResult OrdinalResult::success(std::uint64_t value) { return OrdinalResult(value); }
OrdinalResult OrdinalResult::failure(OrdinalFailure error) { return OrdinalResult(std::move(error)); }
OrdinalResult::operator bool() const noexcept { return std::holds_alternative<std::uint64_t>(storage_); }
std::uint64_t OrdinalResult::value() const { return std::get<std::uint64_t>(storage_); }
const OrdinalFailure& OrdinalResult::error() const { return std::get<OrdinalFailure>(storage_); }

OrdinalResult score_ordinal(const planner::OfficialScore& score, const ScoreBounds& bounds) {
    if (score.total_unique_brands < 0 || score.cumulative_daily_unique_brands < 0 || score.total_bowls < 0)
        return OrdinalResult::failure({OrdinalError::NegativeScore, "negative score"});
    const auto first = static_cast<std::uint64_t>(score.total_unique_brands);
    const auto second = static_cast<std::uint64_t>(score.cumulative_daily_unique_brands);
    const auto third = static_cast<std::uint64_t>(score.total_bowls);
    if (first > bounds.total_unique_brands || second > bounds.cumulative_daily_unique_brands
        || third > bounds.total_bowls)
        return OrdinalResult::failure({OrdinalError::OutOfBounds, "score exceeds ordinal bounds"});
    if (bounds.cumulative_daily_unique_brands == std::numeric_limits<std::uint64_t>::max()
        || bounds.total_bowls == std::numeric_limits<std::uint64_t>::max())
        return OrdinalResult::failure({OrdinalError::Overflow, "score ordinal base overflow"});
    std::uint64_t value = 0, temporary = 0;
    if (!checked_multiply(first, bounds.cumulative_daily_unique_brands + 1, value)
        || !checked_add(value, second, value)
        || !checked_multiply(value, bounds.total_bowls + 1, temporary)
        || !checked_add(temporary, third, value))
        return OrdinalResult::failure({OrdinalError::Overflow, "score ordinal overflow"});
    return OrdinalResult::success(value);
}

StructuredSolution solution_from_baseline(const planner::PlannerInput& input,
    const planner::PlannerResult& greedy, const planner::RefuelPlannerResult& refuel) {
    StructuredSolution result;
    for (std::size_t agent = 0; agent < input.daily.own_agents.size(); ++agent) {
        if (input.daily.own_agents[agent].kind != core::AgentKind::Patrol) continue;
        PatrolRoutePlan route{agent, refuel.visited_spots[agent], {}};
        if (agent < greedy.route_objectives.size()) route.objectives = greedy.route_objectives[agent];
        normalize(route);
        result.patrol_routes.push_back(std::move(route));
    }
    for (const auto& meeting : refuel.rendezvous)
        result.rendezvous.push_back({meeting.patrol_agent, meeting.supply_agent, meeting.cell, 0});
    return result;
}

bool apply_neighborhood(StructuredSolution& s, const Neighborhood n,
    const planner::PlannerInput& input, std::mt19937_64& random,
    std::string* mutation_kind, std::string* fallback_reason) {
    if (s.patrol_routes.empty()) return false;
    if (mutation_kind != nullptr) *mutation_kind = neighborhood_name(n);
    if (fallback_reason != nullptr) *fallback_reason = {};
    auto& a = s.patrol_routes[pick(random, s.patrol_routes.size())];
    normalize(a);
    const auto stock_value_spot = [&](const std::vector<std::size_t>& candidates) {
        const auto origin = a.spot_indices.empty()
            ? input.daily.own_agents[a.agent_index].position
            : input.match.spots[a.spot_indices.back()].position;
        return *std::max_element(candidates.begin(), candidates.end(), [&](const auto left, const auto right) {
            const auto left_key = std::tuple{input.match.spots[left].max_stock,
                -core::hex_distance(input.match.map, origin, input.match.spots[left].position),
                -static_cast<std::int64_t>(left)};
            const auto right_key = std::tuple{input.match.spots[right].max_stock,
                -core::hex_distance(input.match.map, origin, input.match.spots[right].position),
                -static_cast<std::int64_t>(right)};
            return left_key < right_key;
        });
    };
    const auto meeting_cells = [&](const PatrolRoutePlan& route) {
        std::vector<core::CellIndex> cells{input.daily.own_agents[route.agent_index].position};
        for (const auto spot : route.spot_indices)
            if (spot < input.match.spots.size()) cells.push_back(input.match.spots[spot].position);
        return cells;
    };
    switch (n) {
    case Neighborhood::SwapWithin: {
        if (a.spot_indices.size() < 2) return false;
        auto i = pick(random, a.spot_indices.size());
        auto j = pick(random, a.spot_indices.size());
        if (i == j) j = (j + 1) % a.spot_indices.size();
        std::swap(a.spot_indices[i], a.spot_indices[j]);
        std::swap(a.objectives[i], a.objectives[j]);
        return true;
    }
    case Neighborhood::RelocateWithin: {
        if (a.spot_indices.size() < 2) return false;
        const auto from = pick(random, a.spot_indices.size());
        auto to = pick(random, a.spot_indices.size());
        if (from == to) to = (to + 1) % a.spot_indices.size();
        const auto spot = a.spot_indices[from];
        const auto objective = a.objectives[from];
        a.spot_indices.erase(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(from));
        a.objectives.erase(a.objectives.begin() + static_cast<std::ptrdiff_t>(from));
        a.spot_indices.insert(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(to), spot);
        a.objectives.insert(a.objectives.begin() + static_cast<std::ptrdiff_t>(to), objective);
        return true;
    }
    case Neighborhood::AddSpot: {
        std::set<std::size_t> used;
        for (const auto& route : s.patrol_routes) used.insert(route.spot_indices.begin(), route.spot_indices.end());
        std::vector<std::size_t> unused;
        for (std::size_t i = 0; i < input.match.spots.size(); ++i) if (!used.contains(i)) unused.push_back(i);
        if (unused.empty()) return false;
        const auto spot = stock_value_spot(unused);
        const auto at = pick(random, a.spot_indices.size() + 1);
        a.spot_indices.insert(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(at), spot);
        a.objectives.insert(a.objectives.begin() + static_cast<std::ptrdiff_t>(at), pathfinding::RouteObjective::Fastest);
        return true;
    }
    case Neighborhood::AddUncollectedBrand: {
        const auto acquired = acquired_brands(input);
        std::set<std::size_t> used;
        std::set<core::Quantity> planned_brands;
        for (const auto& route : s.patrol_routes)
            for (const auto spot : route.spot_indices) {
                if (spot >= input.match.spots.size()) continue;
                used.insert(spot);
                planned_brands.insert(input.match.spots[spot].brand);
            }
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < input.match.spots.size(); ++i)
            if (!used.contains(i) && !acquired.contains(input.match.spots[i].brand)
                && !planned_brands.contains(input.match.spots[i].brand))
                candidates.push_back(i);
        const bool fallback_to_add_spot = candidates.empty();
        if (fallback_to_add_spot) {
            // Preserve the old search surface when no uncollected brand is available.
            std::vector<std::size_t> unused;
            for (std::size_t i = 0; i < input.match.spots.size(); ++i)
                if (!used.contains(i)) unused.push_back(i);
            if (unused.empty()) return false;
            candidates = std::move(unused);
            if (fallback_reason != nullptr) *fallback_reason = "no-uncollected-brand";
        }
        const auto spot = stock_value_spot(candidates);
        const bool can_insert = !a.spot_indices.empty();
        const auto mutation_choice = random() % 3U;
        const bool replace = can_insert && mutation_choice == 0U;
        const bool insert_nearby = can_insert && mutation_choice == 1U;
        if (replace) {
            // A replacement keeps route size bounded while directing an existing leg
            // toward a brand absent from the current progress snapshot.
            const auto at = pick(random, a.spot_indices.size());
            a.spot_indices[at] = spot;
            if (mutation_kind != nullptr) *mutation_kind = "AddUncollectedBrand.route-replace";
        } else if (insert_nearby) {
            // One bounded insertion is chosen near the closest existing route spot.
            // The strict candidate build below is the fuel/step/refuel feasibility gate.
            std::size_t insertion = a.spot_indices.size();
            std::int64_t best_distance = std::numeric_limits<std::int64_t>::max();
            std::size_t nearest_spot = std::numeric_limits<std::size_t>::max();
            for (std::size_t i = 0; i < a.spot_indices.size(); ++i) {
                const auto route_spot = a.spot_indices[i];
                if (route_spot >= input.match.spots.size()) continue;
                const auto distance = core::hex_distance(input.match.map,
                    input.match.spots[route_spot].position, input.match.spots[spot].position);
                if (distance < best_distance
                    || (distance == best_distance && route_spot < nearest_spot)) {
                    best_distance = distance;
                    nearest_spot = route_spot;
                    insertion = i + 1;
                }
            }
            a.spot_indices.insert(a.spot_indices.begin()
                + static_cast<std::ptrdiff_t>(insertion), spot);
            a.objectives.insert(a.objectives.begin()
                + static_cast<std::ptrdiff_t>(insertion), pathfinding::RouteObjective::Fastest);
            if (mutation_kind != nullptr) *mutation_kind = "AddUncollectedBrand.route-insert-nearby";
        } else {
            const auto at = a.spot_indices.size();
            a.spot_indices.insert(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(at), spot);
            a.objectives.insert(a.objectives.begin() + static_cast<std::ptrdiff_t>(at), pathfinding::RouteObjective::Fastest);
            if (mutation_kind != nullptr) *mutation_kind = "AddUncollectedBrand.route-append";
        }
        if (fallback_to_add_spot && mutation_kind != nullptr) *mutation_kind = "AddSpot";
        return true;
    }
    case Neighborhood::RemoveSpot: {
        if (a.spot_indices.empty()) return false;
        const auto i = pick(random, a.spot_indices.size());
        a.spot_indices.erase(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(i));
        a.objectives.erase(a.objectives.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    case Neighborhood::ReplaceSameBrand: {
        if (a.spot_indices.empty()) return false;
        const auto i = pick(random, a.spot_indices.size());
        std::vector<std::size_t> same;
        for (std::size_t x = 0; x < input.match.spots.size(); ++x)
            if (x != a.spot_indices[i] && input.match.spots[x].brand == input.match.spots[a.spot_indices[i]].brand) same.push_back(x);
        if (same.empty()) return false;
        const auto before = i == 0 ? input.daily.own_agents[a.agent_index].position
                                   : input.match.spots[a.spot_indices[i - 1]].position;
        const auto after = i + 1 < a.spot_indices.size()
            ? std::optional<core::CellIndex>{input.match.spots[a.spot_indices[i + 1]].position}
            : std::nullopt;
        a.spot_indices[i] = *std::max_element(same.begin(), same.end(), [&](const auto left, const auto right) {
            const auto detour = [&](const auto spot) {
                auto distance = core::hex_distance(input.match.map, before, input.match.spots[spot].position);
                if (after) distance += core::hex_distance(input.match.map, input.match.spots[spot].position, *after);
                return distance;
            };
            return std::tuple{input.match.spots[left].max_stock, -detour(left), -static_cast<std::int64_t>(left)}
                < std::tuple{input.match.spots[right].max_stock, -detour(right), -static_cast<std::int64_t>(right)};
        });
        return true;
    }
    case Neighborhood::MoveBetweenPatrols:
    case Neighborhood::SwapBetweenPatrols: {
        if (s.patrol_routes.size() < 2 || a.spot_indices.empty()) return false;
        auto j = pick(random, s.patrol_routes.size());
        if (&s.patrol_routes[j] == &a) j = (j + 1) % s.patrol_routes.size();
        auto& b = s.patrol_routes[j]; normalize(b);
        if (n == Neighborhood::SwapBetweenPatrols) {
            if (b.spot_indices.empty()) return false;
            const auto i = pick(random, a.spot_indices.size()); const auto k = pick(random, b.spot_indices.size());
            std::swap(a.spot_indices[i], b.spot_indices[k]); std::swap(a.objectives[i], b.objectives[k]);
        } else {
            const auto i = pick(random, a.spot_indices.size());
            b.spot_indices.push_back(a.spot_indices[i]); b.objectives.push_back(a.objectives[i]);
            a.spot_indices.erase(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(i));
            a.objectives.erase(a.objectives.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return true;
    }
    case Neighborhood::ReverseSubsequence: {
        if (a.spot_indices.size() < 2) return false;
        auto i = pick(random, a.spot_indices.size()); auto j = pick(random, a.spot_indices.size());
        if (i > j) std::swap(i, j);
        if (i == j) return false;
        std::reverse(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(i), a.spot_indices.begin() + static_cast<std::ptrdiff_t>(j + 1));
        std::reverse(a.objectives.begin() + static_cast<std::ptrdiff_t>(i), a.objectives.begin() + static_cast<std::ptrdiff_t>(j + 1));
        return true;
    }
    case Neighborhood::SetFastest:
    case Neighborhood::SetFuelEfficient: {
        if (a.objectives.empty()) return false;
        const auto i = pick(random, a.objectives.size());
        const auto objective = n == Neighborhood::SetFastest ? pathfinding::RouteObjective::Fastest : pathfinding::RouteObjective::FuelEfficient;
        if (a.objectives[i] == objective) return false;
        a.objectives[i] = objective;
        return true;
    }
    case Neighborhood::MoveRendezvousCell: {
        if (s.rendezvous.empty()) return false;
        auto& task = s.rendezvous[pick(random, s.rendezvous.size())];
        const auto route = std::find_if(s.patrol_routes.begin(), s.patrol_routes.end(),
            [&task](const auto& item) { return item.agent_index == task.patrol_agent; });
        if (route == s.patrol_routes.end()) return false;
        auto cells = meeting_cells(*route);
        cells.erase(std::remove(cells.begin(), cells.end(), task.cell), cells.end());
        if (cells.empty()) return false;
        task.cell = cells[pick(random, cells.size())];
        return true;
    }
    case Neighborhood::AdjustRendezvousWait: {
        if (s.rendezvous.empty()) return false;
        auto& task = s.rendezvous[pick(random, s.rendezvous.size())]; task.wait_adjustment += (random() & 1U) ? 1 : -1;
        return true;
    }
    case Neighborhood::ChangeSupply: case Neighborhood::MoveTaskBetweenSupplies: {
        if (s.rendezvous.empty()) return false;
        const auto supplies = supply_indices(input); if (supplies.size() < 2) return false;
        auto& task = s.rendezvous[pick(random, s.rendezvous.size())]; const auto old = task.supply_agent;
        task.supply_agent = supplies[pick(random, supplies.size())]; return task.supply_agent != old;
    }
    case Neighborhood::SwapSupplyTasks: {
        if (s.rendezvous.size() < 2) return false;
        auto i = pick(random, s.rendezvous.size()); auto j = pick(random, s.rendezvous.size());
        if (i == j) j = (j + 1) % s.rendezvous.size();
        std::swap(s.rendezvous[i], s.rendezvous[j]);
        return true;
    }
    case Neighborhood::RemoveRendezvous:
        if (s.rendezvous.empty()) return false;
        s.rendezvous.erase(s.rendezvous.begin() + static_cast<std::ptrdiff_t>(pick(random, s.rendezvous.size()))); return true;
    case Neighborhood::AddRendezvous: {
        const auto supplies = supply_indices(input);
        const auto cells = meeting_cells(a);
        if (supplies.empty() || cells.empty()) return false;
        s.rendezvous.push_back(
            {a.agent_index, supplies[pick(random, supplies.size())], cells[pick(random, cells.size())], 0});
        return true;
    }
    case Neighborhood::ReplaceAndRelocate: {
        if (a.spot_indices.empty()) return false;
        std::set<std::size_t> used;
        for (const auto& route : s.patrol_routes)
            used.insert(route.spot_indices.begin(), route.spot_indices.end());
        std::vector<std::size_t> unused;
        for (std::size_t spot = 0; spot < input.match.spots.size(); ++spot)
            if (!used.contains(spot)) unused.push_back(spot);
        if (unused.empty()) return false;
        const auto from = pick(random, a.spot_indices.size());
        a.spot_indices[from] = unused[pick(random, unused.size())];
        a.objectives[from] = pathfinding::RouteObjective::Fastest;
        if (a.spot_indices.size() > 1) {
            const auto spot = a.spot_indices[from];
            const auto objective = a.objectives[from];
            a.spot_indices.erase(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(from));
            a.objectives.erase(a.objectives.begin() + static_cast<std::ptrdiff_t>(from));
            const auto to = pick(random, a.spot_indices.size() + 1);
            a.spot_indices.insert(a.spot_indices.begin() + static_cast<std::ptrdiff_t>(to), spot);
            a.objectives.insert(a.objectives.begin() + static_cast<std::ptrdiff_t>(to), objective);
        }
        return true;
    }
    case Neighborhood::Count: return false;
    }
    return false;
}

OptimizerOutcome::OptimizerOutcome(std::variant<OptimizerResult, planner::PlannerError> storage):storage_(std::move(storage)){}
OptimizerOutcome OptimizerOutcome::success(OptimizerResult result){return OptimizerOutcome(std::move(result));}
OptimizerOutcome OptimizerOutcome::failure(planner::PlannerError error){return OptimizerOutcome(std::move(error));}
bool OptimizerOutcome::has_value() const noexcept{return std::holds_alternative<OptimizerResult>(storage_);}
OptimizerOutcome::operator bool() const noexcept{return has_value();}
const OptimizerResult& OptimizerOutcome::value() const&{return std::get<OptimizerResult>(storage_);}
OptimizerResult&& OptimizerOutcome::value() &&{return std::get<OptimizerResult>(std::move(storage_));}
const planner::PlannerError& OptimizerOutcome::error() const&{return std::get<planner::PlannerError>(storage_);}

DailyImprovementDecision evaluate_daily_improvement(
    const planner::OfficialScore& baseline_score,
    const planner::DailyReadiness& baseline_readiness,
    const planner::OfficialScore& candidate_score,
    const planner::DailyReadiness& candidate_readiness) noexcept {
    if (planner::better_official_score(candidate_score, baseline_score))
        return {true, DailyImprovementDecisionReason::OfficialScoreImproved};
    if (candidate_score == baseline_score
        && planner::better_daily_readiness(candidate_readiness, baseline_readiness))
        return {true, DailyImprovementDecisionReason::ReadinessTieBreak};
    return {};
}

OptimizerOutcome optimize(const planner::PlannerInput& input, const planner::PlannerResult& greedy,
    const planner::RefuelPlannerResult& baseline, const OptimizerConfig& config,
    const std::chrono::steady_clock::time_point deadline, OptimizerClock now) {
    const auto started=now();
    if(config.initial_temperature<=0.0||config.final_temperature<=0.0
       ||!std::isfinite(config.initial_temperature)||!std::isfinite(config.final_temperature))
        return OptimizerOutcome::failure({planner::PlannerErrorCode::InvalidInput,"invalid optimizer temperature"});
    auto pathfinder_result=pathfinding::Pathfinder::create(input.match.map,input.daily.traffic);
    if(!pathfinder_result)return OptimizerOutcome::failure({planner::PlannerErrorCode::InvalidInput,pathfinder_result.error().message});
    auto pathfinder=std::move(pathfinder_result).value();
    StructuredSolution current_solution=solution_from_baseline(input,greedy,baseline);
    StructuredSolution best_solution=current_solution;
    BuiltCandidate current{baseline.plan,baseline.simulation,baseline.score,{},baseline.daily_readiness};
    for(const auto& agent:baseline.simulation.end_agents)if(agent.kind==core::AgentKind::Patrol)current.tie.patrol_fuel_remaining+=agent.fuel;
    BuiltCandidate best=current;
    OptimizerResult result; result.initial_score=baseline.score;
    result.initial_readiness=baseline.daily_readiness;
    result.seed=config.seed;
    std::mt19937_64 random(config.seed);
    std::string initial_diagnostic;
    if (auto rebuilt = build_candidate(input, current_solution, pathfinder, initial_diagnostic)) {
        current = std::move(*rebuilt);
        best = current;
        result.initial_readiness = current.readiness;
    } else {
        current_solution.rendezvous.clear();
    }
    std::array<double,neighborhood_count> weights=config.neighborhood_weights;
    if(std::all_of(weights.begin(),weights.end(),[](double x){return x<=0.0;}))weights.fill(1.0);
    for(auto& x:weights)if(!std::isfinite(x)||x<0.0)x=0.0;
    std::discrete_distribution<std::size_t> select(weights.begin(),weights.end());
    const auto bounds=score_bounds(input); std::size_t invalid_run=0;
    bool deadline_hit = false;
    bool best_is_candidate = false;
    std::string best_neighborhood_kind;
    std::chrono::steady_clock::time_point best_evaluated_at = started;
    result.termination=config.maximum_iterations==0?OptimizerTermination::IterationLimit:OptimizerTermination::Completed;
    for(std::size_t iteration=0;iteration<config.maximum_iterations;++iteration){
        if(now()>=deadline){result.termination=OptimizerTermination::Deadline;deadline_hit=true;break;}
        result.iterations=iteration+1; const auto neighborhood=static_cast<Neighborhood>(select(random));
        auto& stats=result.neighborhoods[static_cast<std::size_t>(neighborhood)]; ++stats.generated; ++result.generated_candidates;
        auto proposed=current_solution;
        std::string mutation_kind;
        std::string fallback_reason;
        if(!apply_neighborhood(proposed,neighborhood,input,random,&mutation_kind,&fallback_reason)){++stats.prefiltered;++result.prefiltered_candidates;continue;}
        if(now()>=deadline){result.termination=OptimizerTermination::Deadline;deadline_hit=true;break;}
        std::string diagnostic; auto candidate=build_candidate(input,proposed,pathfinder,diagnostic); ++result.simulator_runs; ++stats.simulated;
        // A candidate whose strict build completed after the deadline is not a
        // deadline best. Keep the previously verified best instead.
        if (now() >= deadline) {
            result.termination = OptimizerTermination::Deadline;
            deadline_hit = true;
            break;
        }
        if(!candidate){++result.invalid_candidates;++invalid_run;result.diagnostic=std::move(diagnostic);if(invalid_run>=config.maximum_consecutive_invalid){result.termination=OptimizerTermination::InvalidLimit;break;}continue;}
        invalid_run=0;++result.valid_candidates;++stats.valid;
        const bool official_improved = planner::better_official_score(candidate->score, current.score);
        const bool official_lower = planner::better_official_score(current.score, candidate->score);
        bool accept = better(*candidate, current, config.prefer_daily_readiness_on_tie);
        std::string rejection_reason;
        if (accept) {
            const bool readiness_changed = planner::better_daily_readiness(candidate->readiness, current.readiness)
                || planner::better_daily_readiness(current.readiness, candidate->readiness);
            rejection_reason = official_improved ? "official-score-improved"
                : (readiness_changed ? "readiness-improved" : "deterministic-tie-break");
        } else if (official_lower) {
            const auto candidate_ordinal=score_ordinal(candidate->score,bounds);
            const auto current_ordinal=score_ordinal(current.score,bounds);
            if(candidate_ordinal&&current_ordinal){
                const auto delta=static_cast<long double>(candidate_ordinal.value())-static_cast<long double>(current_ordinal.value());
                const auto temp=std::max(config.final_temperature,temperature(config,iteration));
                const auto probability=std::exp(static_cast<double>(delta/static_cast<long double>(temp)));
                accept=std::uniform_real_distribution<double>(0.0,1.0)(random)<probability;
            }
            rejection_reason = accept ? "annealed-lower-official-score" : "lower-official-score-rejected";
        } else {
            // OfficialScore equality never enters annealing (delta would be zero).
            // Readiness and the deterministic internal tie-break are the only tie paths.
            rejection_reason = "official-score-tie-not-better";
        }
        if (result.candidate_diagnostics.size() < 64) {
            const auto acquired = acquired_brands(input);
            std::set<core::Quantity> all_brands;
            for (const auto& spot : input.match.spots) all_brands.insert(spot.brand);
            std::size_t newly_acquired = 0;
            for (const auto brand : candidate->simulation.distinct_brands)
                if (!acquired.contains(brand)) ++newly_acquired;
            CandidateDiagnostic trace;
            trace.neighborhood_kind = mutation_kind.empty() ? neighborhood_name(neighborhood) : mutation_kind;
            trace.fallback_reason = fallback_reason;
            trace.acquired_brand_count = candidate->score.total_unique_brands;
            trace.newly_acquired_brand_count = static_cast<std::int64_t>(newly_acquired);
            trace.uncollected_brand_count = static_cast<std::int64_t>(
                std::count_if(all_brands.begin(), all_brands.end(), [&acquired](const auto brand) {
                    return !acquired.contains(brand);
                }));
            trace.official_score_delta = {candidate->score.total_unique_brands - current.score.total_unique_brands,
                candidate->score.cumulative_daily_unique_brands - current.score.cumulative_daily_unique_brands,
                candidate->score.total_bowls - current.score.total_bowls};
            trace.daily_readiness_delta = {candidate->readiness.uncollected_spot_reachability - current.readiness.uncollected_spot_reachability,
                candidate->readiness.fuel_reserve - current.readiness.fuel_reserve,
                candidate->readiness.patrol_dispersion - current.readiness.patrol_dispersion,
                candidate->readiness.rendezvous_readiness - current.readiness.rendezvous_readiness, {}};
            trace.accepted = accept;
            trace.rejection_reason = accept ? "" : rejection_reason;
            trace.action_hash = plan_digest(candidate->plan);
            trace.plan_hash = solution_digest(proposed);
            trace.candidate_hash = digest(trace.action_hash + ":" + trace.plan_hash);
            result.candidate_diagnostics.push_back(std::move(trace));
        }
        if(accept){current_solution=std::move(proposed);current=std::move(*candidate);++result.accepted_candidates;++stats.accepted;if(better(current,best,config.prefer_daily_readiness_on_tie)){
                best=current;best_solution=current_solution;++result.improvements;++stats.improved;
                best_is_candidate=true;
                best_neighborhood_kind=mutation_kind.empty() ? neighborhood_name(neighborhood) : mutation_kind;
                best_evaluated_at=now();
            }}
    }
    std::optional<simulator::DaySimulationResult> final_check;
    if (!deadline_hit && now() < deadline) {
        auto checked=simulator::simulate_day({input.match.map,input.match.spots,input.match.fuel_limit,
            input.match.day_steps[static_cast<std::size_t>(input.daily.day)],input.daily.own_agents,input.daily.traffic},best.plan,simulator::TraceMode::Enabled);
        ++result.simulator_runs;
        if(!checked)return OptimizerOutcome::failure({planner::PlannerErrorCode::BaselineSimulationFailed,"optimizer final verification failed: "+checked.error().message});
        final_check=std::move(checked).value();
    }
    const bool fallback_to_baseline = planner::better_official_score(baseline.score, best.score)
        || (config.prefer_daily_readiness_on_tie && best.score == baseline.score
            && planner::better_daily_readiness(baseline.daily_readiness, best.readiness));
    if (fallback_to_baseline) {
        best = {baseline.plan, baseline.simulation, baseline.score, {}, baseline.daily_readiness};
        best_solution = solution_from_baseline(input, greedy, baseline);
        result.termination = OptimizerTermination::Fallback;
    }
    result.plan=std::move(best.plan);
    result.simulation = fallback_to_baseline ? baseline.simulation : (final_check ? std::move(final_check).value() : best.simulation);
    result.score=planner::official_score(input.previous_progress,result.simulation);result.tie_break=best.tie;
    std::vector<std::vector<std::size_t>> final_visits(input.daily.own_agents.size());
    for (const auto& route : best_solution.patrol_routes) final_visits[route.agent_index] = route.spot_indices;
    result.readiness=planner::daily_readiness(input.match,input.daily,result.simulation,final_visits);
    result.solution=std::move(best_solution);result.rendezvous=baseline.rendezvous;result.supply_schedules=baseline.supply_schedules;result.elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now()-started);
    result.best_candidate_at_deadline = deadline_hit && best_is_candidate && !fallback_to_baseline;
    result.best_candidate_strict_verified = best_is_candidate && !fallback_to_baseline;
    if (result.best_candidate_strict_verified) {
        result.best_candidate_action_hash = plan_digest(result.plan);
        result.best_candidate_plan_hash = solution_digest(result.solution);
        result.best_candidate_end_state_hash = digest(result.best_candidate_action_hash + ":" + std::to_string(result.simulation.total_balls));
        result.best_candidate_neighborhood_kind = best_neighborhood_kind;
        result.best_candidate_evaluated_at_us = std::chrono::duration_cast<std::chrono::microseconds>(best_evaluated_at-started).count();
    }
    return OptimizerOutcome::success(std::move(result));
}

}  // namespace hexa_udon::optimizer
