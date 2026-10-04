#include "hexa_udon/app/production_profile.hpp"
#include "hexa_udon/core/type_candidates.hpp"
#include "hexa_udon/optimizer/daily_deadline_policy.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
namespace hexa_udon::app {
namespace {
bool validate_32_profile(const nlohmann::json& supplied) {
    if (supplied.value("profileId", "") != "32x32-one-or-three-supply-v1") return false;
    std::vector<std::vector<int>> expected_values;
    for (const auto supply : {std::size_t{1}, std::size_t{3}}) {
        for (const auto& kinds : core::complete_type_arrays(7, supply, supply)) {
            std::vector<int> row;
            for (const auto kind : kinds) row.push_back(core::to_int(kind));
            expected_values.push_back(std::move(row));
        }
    }
    std::sort(expected_values.begin(), expected_values.end());
    const nlohmann::json expected_candidates = expected_values;
    const auto target = nlohmann::json{{"height",32},{"width",32},{"agents",7}};
    const auto boundary = supplied.value("informationBoundary", nlohmann::json::object());
    const auto parameters = supplied.value("parameters", nlohmann::json::object());
    if (supplied.value("schemaVersion", 0) != 1 || supplied.value("profileVersion", 0) != 1
        || supplied.value("status", "") != "human-approved-provisional"
        || supplied.value("target", nlohmann::json::object()) != target
        || supplied.value("type-selector", "") != "prematch"
        || supplied.value("minSupply", 0) != 1 || supplied.value("maxSupply", 0) != 3
        || supplied.value("allowedSupplyCounts", std::vector<std::size_t>{}) != std::vector<std::size_t>{1,3}
        || supplied.value("typeCandidateCount", 0) != 42
        || supplied.value("typeCandidates", nlohmann::json::array()) != expected_candidates
        || supplied.value("plannerMode", "") != "greedy-refuel"
        || supplied.value("seed", 0) != 30013
        || supplied.value("selectorBudgetMs", 0) <= 0
        || supplied.value("typeSubmissionReserveMs", 0) <= 0
        || boundary.value("futureSnapshotsAllowed", true)
        || boundary.value("benchmarkMapsAllowed", true)
        || parameters.value("greedyBudgetMs", 0) != 100
        || parameters.value("refuelBudgetMs", 0) != 150
        || parameters.value("greedyCandidates", 0) != 500
        || parameters.value("refuelCandidates", 0) != 500
        || parameters.value("rendezvousCandidates", 0) != 1000
        || parameters.value("maximumRefuelsPerPatrol", 0) != 2) return false;
    const std::vector<std::string> ids{"type-policy-32-pilot-01", "type-policy-32-training-01",
        "type-policy-32-validation-02", "type-policy-32-holdout-02"};
    const std::vector<std::vector<int>> wdl{{3,0,0}, {24,1,3}, {25,1,2}, {24,0,4}};
    const std::vector<std::vector<std::string>> hashes{
        {"b4a949413ca814aa1788e41eb684ff7f47d0b9672b7d3b0a61bbf57ce99c93ea", "6a50b4b54820537b2387d5cc224725b2b6b6ef81725edb1af5b701baa6e746a8", "b02a125520e3727d6fb989b03bce1c77e0cfd16d2cced01865865d81cb687052", "0a1b3a8dbfd31d0689a8ee1b67074da6c7e2bb5467d2f509fccee557143dc228"},
        {"a6b6bc9134b5bf9fe7d2ff8524d67e5fd033035b0ace4a8320739904c488d2fb", "3f9053ba761b29fe9e8a3d29f7e7f16683f26db3fc86cbf328469d0e8a8382c9", "027e321e1c0a585ef5e8f014b73c880407f4551368d946001e1e1a236f2c9ff1", "0a1b3a8dbfd31d0689a8ee1b67074da6c7e2bb5467d2f509fccee557143dc228"},
        {"6c9c84ba65984facfd3602fc2c5338b72169f5a8e7bb40b280866e0d4c529860", "266701582f85e1ff0399c3908093ddff4d280b8d5cffe1de8e145e51d7eeae56", "446fdc85ee8fe3efaa77e6f823f34a29193586175b3d39ed703cafcf9daea74e", "477c735168ee1dd511c42db47e0b1c49d34b7227e5dcd2397a9426652917d18e"},
        {"adc8342ca063b8067a9836851d8a68e3ded0a9a1691b34eedc12838c653f9c4a", "67db326974178789ccb7259f54141d4fd2b3b8abfc2a33f0906284ceca4bdd66", "c75656fffcdbcf8efe1f56ab1c4528916da90d3fc5380afefed4f61176049efa", "477c735168ee1dd511c42db47e0b1c49d34b7227e5dcd2397a9426652917d18e"}};
    const auto evidence = supplied.value("evidence", nlohmann::json::array());
    if (!evidence.is_array() || evidence.size() != ids.size()) return false;
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        if (evidence[i].value("outputId", "") != ids[i]
            || evidence[i].value("threeVsOneWDL", std::vector<int>{}) != wdl[i]
            || evidence[i].value("trialFailures", -1) != 0
            || evidence[i].value("artifactErrors", -1) != 0
            || evidence[i].value("configHash", "") != hashes[i][0]
            || evidence[i].value("catalogHash", "") != hashes[i][1]
            || evidence[i].value("mapIdentityHash", "") != hashes[i][2]
            || evidence[i].value("typeCandidatesHash", "") != hashes[i][3]
            || evidence[i].value("fixedParameterHash", "") != "41c9b1eb88be227a10238424df8800c590aa76373edd0d576841733620053974"
            || evidence[i].value("policyVersion", 0) != 1 || evidence[i].value("generatorVersion", 0) != 2) return false;
    }
    return supplied.value("excludedInvalidOutputs", nlohmann::json::array())
        == nlohmann::json{"type-policy-32-validation-01", "type-policy-32-holdout-01"};
}

bool same_schema_types(const nlohmann::json& supplied, const nlohmann::json& approved) {
    // Parsed positive integers may be unsigned; both remain integral JSON numbers.
    if (supplied.is_number_integer() && approved.is_number_integer()) return true;
    if (supplied.type() != approved.type()) return false;
    if (approved.is_object()) {
        for (auto it = approved.begin(); it != approved.end(); ++it) {
            if (!supplied.contains(it.key()) || !same_schema_types(supplied.at(it.key()), it.value()))
                return false;
        }
    } else if (approved.is_array()) {
        if (supplied.size() != approved.size()) return false;
        for (std::size_t index = 0; index < approved.size(); ++index)
            if (!same_schema_types(supplied[index], approved[index])) return false;
    }
    return true;
}
// Reviewed policy registry: semantic equality locks schema, budgets and evidence.
constexpr const char* approved_profile = R"approved({
  "schemaVersion": 1,
  "profileId": "16x16-one-supply-v1",
  "profileVersion": 1,
  "status": "human-approved-provisional",
  "target": {
    "height": 16,
    "width": 16,
    "agents": 4
  },
  "type-selector": "prematch",
  "minSupply": 1,
  "maxSupply": 1,
  "typeCandidates": [
    [
      0,
      0,
      0,
      1
    ],
    [
      0,
      0,
      1,
      0
    ],
    [
      0,
      1,
      0,
      0
    ],
    [
      1,
      0,
      0,
      0
    ]
  ],
  "informationBoundary": {
    "selectorInput": "setting-derived-Day0",
    "futureSnapshotsAllowed": false,
    "benchmarkMapsAllowed": false,
    "day1Plus": "offline-research-only"
  },
  "plannerMode": "greedy-refuel",
  "selectorBudgetMs": 3000,
  "seed": 30013,
  "connectTimeoutMilliseconds": 2000,
  "totalTimeoutMilliseconds": 5000,
  "pollingMilliseconds": 750,
  "safetyMarginSeconds": 5,
  "budgetSource": "config/profiles/round-4-candidate.json",
  "parameters": {
    "greedyBudgetMs": 100,
    "refuelBudgetMs": 150,
    "optimizerBudgetMs": 250,
    "greedyCandidates": 500,
    "refuelCandidates": 500,
    "rendezvousCandidates": 1000,
    "maximumRefuelsPerPatrol": 2,
    "optimizerIterations": 10000,
    "optimizerInitialTemperature": 8.0,
    "optimizerFinalTemperature": 0.05
  },
  "decision": {
    "authority": "phase-20-explicit-human-instruction",
    "comparison": "within-map-OfficialScore-lexicographic",
    "crossSplitAggregation": false,
    "supplyIndex": "select-on-current-Day0",
    "twoSupply": "excluded-by-training",
    "limitations": [
      "holdout-four-losses-and-one-tie",
      "not-applicable-to-24x24-or-32x32"
    ]
  },
  "evidence": [
    {
      "split": "training",
      "outputId": "type-policy-review-01",
      "oneVsZeroWDL": [
        20,
        0,
        8
      ],
      "trialFailures": 0,
      "configHash": "350a511b4c05f3c019b197676693203f9630060e5a11aee80a79310becbfe88b",
      "catalogHash": "44b984263bd3269296f2cedac5aef77bcd7402214b11c0e56ca8f4145e88f5a2",
      "mapIdentityHash": "3ff0a53a26b62a983b20bac4a791c1641aa121ebb0fee823c590e05e011798df",
      "typeCandidatesHash": "819ee4f5be2ff603597d8eb2463eefce0350c539b964455e55bade9729e06d87",
      "policyVersion": 1,
      "fixedParameterHash": "41c9b1eb88be227a10238424df8800c590aa76373edd0d576841733620053974",
      "generatorVersion": 2
    },
    {
      "split": "validation",
      "outputId": "type-policy-validation-01",
      "oneVsZeroWDL": [
        26,
        0,
        2
      ],
      "trialFailures": 0,
      "configHash": "e38b086811443f2b24eb61266d4534adc31acd708ed9e45a89639ca7a0b32add",
      "catalogHash": "2954248a3fe32940c153a8b4797cd6c540800bacdad62ad35660cb35df37bb72",
      "mapIdentityHash": "0fb72d38b6f0b8ad611bd840ebfa6f85c3eb91b42c46292d12cd74d930b9db13",
      "typeCandidatesHash": "b79be32cb43a91f17040745bd1b19d204ac4d45337ffae4ab056bdafd7a4b7a6",
      "policyVersion": 1,
      "fixedParameterHash": "41c9b1eb88be227a10238424df8800c590aa76373edd0d576841733620053974",
      "generatorVersion": 2
    },
    {
      "split": "holdout",
      "outputId": "type-policy-holdout-01",
      "oneVsZeroWDL": [
        23,
        1,
        4
      ],
      "trialFailures": 0,
      "configHash": "e4ea8ece6d07ac907041fad4bd40df025a42e131ef5471c3991a769b783996ea",
      "catalogHash": "d57bcb9ae3eb7123152da53485fc3e1b29cdc29f88fcfb19478e7fda7b234ed0",
      "mapIdentityHash": "17873fce3977f7cb69992cd69ae72b9c379cf3c29fa37eaa9699bebcffb59556",
      "typeCandidatesHash": "b79be32cb43a91f17040745bd1b19d204ac4d45337ffae4ab056bdafd7a4b7a6",
      "policyVersion": 1,
      "fixedParameterHash": "41c9b1eb88be227a10238424df8800c590aa76373edd0d576841733620053974",
      "generatorVersion": 2
    }
  ]
}
)approved";
}
static std::string profile_digest(const nlohmann::json& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value.dump()) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}
static void validate_profile_json(const nlohmann::json& supplied) {
    if (supplied.value("profileVersion", 0) == 2) {
        if (!supplied.at("profileVersion").is_number_integer())
            throw std::runtime_error("profileVersion must be an integer");
        const auto id = supplied.value("profileId", "");
        const std::vector<std::string> ids{"16x16-one-supply-v2", "24x24-two-supply-v2", "32x32-one-or-three-supply-v2"};
        const std::vector<std::string> digests{"bbebc8c4ad5379b5", "ff3664585e5c50e7", "7060bc11f88a2ed3"};
        const auto found = std::find(ids.begin(), ids.end(), id);
        if (found == ids.end()) throw std::runtime_error("unapproved v2 profile identity");
        const auto index = static_cast<std::size_t>(found - ids.begin());
        const auto policy = optimizer::DailyDeadlinePolicy::for_production_size(index == 0 ? 16 : index == 1 ? 24 : 32);
        const nlohmann::json approval{{"authority","phase-42-explicit-human-instruction"},
            {"scope","observed-current-day-only"},{"baselineFirst",true},{"defaultChanged",false}};
        if (supplied.at("plannerMode") != "daily-improvement"
            || supplied.at("dailyDeadlinePolicy") != policy->identity()
            || !same_schema_types(supplied.at("dailyDeadlinePolicy"), policy->identity())
            || supplied.at("dailyPolicyApproval") != approval
            || !same_schema_types(supplied.at("dailyPolicyApproval"), approval))
            throw std::runtime_error("unapproved daily deadline policy");
        auto inherited = supplied;
        inherited.erase("dailyDeadlinePolicy"); inherited.erase("dailyPolicyApproval");
        inherited["profileId"] = id.substr(0, id.size()-1) + "1";
        inherited["profileVersion"] = 1; inherited["plannerMode"] = "greedy-refuel";
        if (profile_digest(inherited) != digests[index])
            throw std::runtime_error("v2 inherited profile identity mismatch");
        validate_profile_json(inherited);
        return;
    }
    if (supplied.value("profileId", "") == "32x32-one-or-three-supply-v1") {
        if (!validate_32_profile(supplied))
            throw std::runtime_error("profile schema/identity differs from human-approved 32x32-one-or-three-supply-v1");
        return;
    }
    const auto approved = nlohmann::json::parse(approved_profile);
    if (supplied == approved && same_schema_types(supplied, approved)) return;
    if (supplied.value("profileId", "") != "24x24-two-supply-v1")
        throw std::runtime_error("profile schema/identity differs from an approved profile");
    const auto candidates = supplied.value("typeCandidates", nlohmann::json::array());
    const auto expected = nlohmann::json::parse(R"([[0,0,0,1,1],[0,0,1,0,1],[0,0,1,1,0],[0,1,0,0,1],[0,1,0,1,0],[0,1,1,0,0],[1,0,0,0,1],[1,0,0,1,0],[1,0,1,0,0],[1,1,0,0,0]])");
    if (supplied.value("schemaVersion", 0) != 1 || supplied.value("profileVersion", 0) != 1
        || supplied.value("status", "") != "human-approved-provisional"
        || supplied.value("target", nlohmann::json::object()) != nlohmann::json{{"height",24},{"width",24},{"agents",5}}
        || supplied.value("type-selector", "") != "prematch"
        || supplied.value("minSupply", 0) != 2 || supplied.value("maxSupply", 0) != 2
        || candidates != expected || !same_schema_types(candidates, expected)
        || supplied.value("plannerMode", "") != "greedy-refuel"
        || supplied.value("selectorBudgetMs", 0) != 3000 || supplied.value("seed", 0) != 30013
        || supplied.value("connectTimeoutMilliseconds", 0) != 2000
        || supplied.value("totalTimeoutMilliseconds", 0) != 5000
        || supplied.value("pollingMilliseconds", 0) != 750
        || supplied.value("safetyMarginSeconds", 0) != 5
        || supplied.value("budgetSource", "") != "config/profiles/round-5-candidate.json"
        || supplied.value("typeCandidateCount", 0) != 10
        || supplied.value("informationBoundary", nlohmann::json::object()).value("futureSnapshotsAllowed", true)
        || supplied.value("informationBoundary", nlohmann::json::object()).value("benchmarkMapsAllowed", true)
        || supplied.value("typeSubmissionReserveMs", 0) != 5250)
        throw std::runtime_error("profile schema/identity differs from human-approved 24x24-two-supply-v1");
    const auto& parameters = supplied.at("parameters");
    if (parameters.value("greedyBudgetMs", 0) != 100 || parameters.value("refuelBudgetMs", 0) != 150
        || parameters.value("optimizerBudgetMs", 0) != 250 || parameters.value("greedyCandidates", 0) != 500
        || parameters.value("refuelCandidates", 0) != 500 || parameters.value("rendezvousCandidates", 0) != 1000
        || parameters.value("maximumRefuelsPerPatrol", 0) != 2 || parameters.value("optimizerIterations", 0) != 10000
        || parameters.value("optimizerInitialTemperature", 0.0) != 8.0
        || parameters.value("optimizerFinalTemperature", 0.0) != 0.05)
        throw std::runtime_error("24x24 profile planner parameter mismatch");
    const auto evidence = supplied.value("evidence", nlohmann::json::array());
    if (!evidence.is_array() || evidence.size() != 4) throw std::runtime_error("24x24 profile evidence is incomplete");
    const std::vector<std::string> ids{"type-policy-24-pilot-01", "type-policy-24-training-01", "type-policy-24-validation-02", "type-policy-24-holdout-02"};
    const std::vector<std::vector<int>> wdl{{3,0,0},{26,0,2},{26,1,1},{26,0,2}};
    const std::vector<std::vector<std::string>> hashes{
        {"34dc8d21e7a014abcae1d27ca8f2dd43d856a898537c58fec520cf2c622fe0cd", "2f53f0998039a33485c9cb5e540f7dc99f12787af722f8210746fb56b922abbf", "8edfe2da6f689e13fd1e8a6197a981177aa157af0a846b1426d1417e35a89a36", "02756452636dd6834c6edb7a2fe3dc813188faecd3bdfe526a13ea8e4176f71c"},
        {"1e8e3ab1950366ddb650d5dd0bd2035771d1e34787a37536a941d25b2259dfd5", "f6909d3fc577a5640c43f0661207426c00bf949f3c8b93ddc11fba35cf4f7c32", "70cd1c4118139606e333f336c3050ea213f466a2d21c4afb9004b5d0202ef485", "02756452636dd6834c6edb7a2fe3dc813188faecd3bdfe526a13ea8e4176f71c"},
        {"72282861cf0604c62df7b2e9a0c522afb74cc7088ac3d10cd57bb52044c6da38", "fa862ba825ca98aa1435e8f8946b81ea5dceb810775655c035a82d86be5be6c0", "37fdbef5835ad8f047f9b11e2ce7269b1d0599d572d365a661764fc9f7e7ecd3", "88f5a7f0f5e724841d4486c1519766175ce300f1bb8e1ab5bc4b07b4666863af"},
        {"60d49f5eae9945242b1e13deb55735c1a7a7e6b0549d745c90b2ac4d44140fb1", "40e69446d08dc834f11299c15e89ae5cc286eb6d38625e36f3a3f09357fa22d3", "1ed16731d72825ceed5b5865451ae066430e540d90e120d671ecc9e43e34d721", "88f5a7f0f5e724841d4486c1519766175ce300f1bb8e1ab5bc4b07b4666863af"}
    };
    const std::string fixed_parameter_hash = "41c9b1eb88be227a10238424df8800c590aa76373edd0d576841733620053974";
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        if (evidence[i].value("outputId", "") != ids[i] || evidence[i].value("trialFailures", -1) != 0
            || evidence[i].value("twoVsOneWDL", std::vector<int>{}) != wdl[i]
            || evidence[i].value("configHash", "") != hashes[i][0]
            || evidence[i].value("catalogHash", "") != hashes[i][1]
            || evidence[i].value("mapIdentityHash", "") != hashes[i][2]
            || evidence[i].value("typeCandidatesHash", "") != hashes[i][3]
            || evidence[i].value("fixedParameterHash", "") != fixed_parameter_hash
            || evidence[i].value("policyVersion", 0) != 1
            || evidence[i].value("generatorVersion", 0) != 2)
            throw std::runtime_error("24x24 profile evidence identity mismatch");
    }
    if (supplied.value("excludedInvalidOutputs", nlohmann::json::array())
        != nlohmann::json{"type-policy-24-validation-01", "type-policy-24-holdout-01"})
        throw std::runtime_error("invalid 24x24 evidence exclusion mismatch");
}
void validate_production_profile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input || std::filesystem::file_size(path) > 65536)
        throw std::runtime_error("profile missing or too large");
    validate_profile_json(nlohmann::json::parse(input));
}
void apply_production_profile(AutoClientConfig& config, const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input || std::filesystem::file_size(path) > 65536)
        throw std::runtime_error("profile missing or too large");
    const auto profile = nlohmann::json::parse(input);
    validate_profile_json(profile); // Apply exactly the bytes validated in this read.
    const auto& parameters = profile.at("parameters");
    config.require_16x16_profile = profile.at("target").at("height") == 16;
    config.profile_version = profile.at("profileVersion").get<int>();
    config.production_policy_identity = nullptr;
    config.daily_deadline_policy = config.profile_version == 2;
    if (config.daily_deadline_policy)
        config.production_policy_identity = {{"profileId",profile.at("profileId")},
            {"profileDigest",profile_digest(profile)}, {"policy",profile.at("dailyDeadlinePolicy")}};
    config.require_profile_target = true;
    config.profile_id = profile.at("profileId").get<std::string>();
    config.required_map_height = profile.at("target").at("height").get<std::size_t>();
    config.required_map_width = profile.at("target").at("width").get<std::size_t>();
    config.required_agent_count = profile.at("target").at("agents").get<std::size_t>();
    config.type_selector = TypeSelectorMode::Prematch;
    config.type_selector_min_supply = profile.at("minSupply").get<std::size_t>();
    config.type_selector_max_supply = profile.at("maxSupply").get<std::size_t>();
    config.type_selector_allowed_supply_counts = profile.value("allowedSupplyCounts", std::vector<std::size_t>{});
    config.type_selector_budget = std::chrono::milliseconds{profile.at("selectorBudgetMs").get<int>()};
    config.type_selector_max_candidates = profile.value("typeCandidateCount", config.required_agent_count);
    config.planner_mode = config.daily_deadline_policy ? PlannerMode::DailyImprovement : PlannerMode::GreedyRefuel;
    config.planner_seed = profile.at("seed").get<std::uint64_t>();
    config.planner_budget = std::chrono::milliseconds{parameters.at("greedyBudgetMs").get<int>()};
    config.planner_candidate_limit = parameters.at("greedyCandidates").get<std::size_t>();
    config.refuel_budget = std::chrono::milliseconds{parameters.at("refuelBudgetMs").get<int>()};
    config.refuel_candidate_limit = parameters.at("refuelCandidates").get<std::size_t>();
    config.rendezvous_candidate_limit = parameters.at("rendezvousCandidates").get<std::size_t>();
    config.maximum_refuels_per_patrol = parameters.at("maximumRefuelsPerPatrol").get<std::size_t>();
    config.optimizer_budget = std::chrono::milliseconds{parameters.at("optimizerBudgetMs").get<int>()};
    config.optimizer_iterations = parameters.at("optimizerIterations").get<std::size_t>();
    config.optimizer_initial_temperature = parameters.at("optimizerInitialTemperature").get<double>();
    config.optimizer_final_temperature = parameters.at("optimizerFinalTemperature").get<double>();
    config.polling_interval = std::chrono::milliseconds{profile.at("pollingMilliseconds").get<int>()};
    config.safety_margin = std::chrono::seconds{profile.at("safetyMarginSeconds").get<int>()};
    if (profile.contains("typeSubmissionReserveMs"))
        config.type_submission_reserve = std::chrono::milliseconds{profile.at("typeSubmissionReserveMs").get<int>()};
}
void apply_16x16_production_profile(AutoClientConfig& config) {
    const auto profile = nlohmann::json::parse(approved_profile);
    const auto& parameters = profile.at("parameters");
    config.require_16x16_profile = true;
    config.require_profile_target = true;
    config.profile_id = profile.at("profileId").get<std::string>();
    config.required_map_height = 16;
    config.required_map_width = 16;
    config.required_agent_count = 4;
    config.type_selector = TypeSelectorMode::Prematch;
    config.type_selector_min_supply = profile.at("minSupply").get<std::size_t>();
    config.type_selector_max_supply = profile.at("maxSupply").get<std::size_t>();
    config.type_selector_budget = std::chrono::milliseconds{profile.at("selectorBudgetMs").get<int>()};
    config.planner_mode = PlannerMode::GreedyRefuel;
    config.planner_seed = profile.at("seed").get<std::uint64_t>();
    config.planner_budget = std::chrono::milliseconds{parameters.at("greedyBudgetMs").get<int>()};
    config.planner_candidate_limit = parameters.at("greedyCandidates").get<std::size_t>();
    config.refuel_budget = std::chrono::milliseconds{parameters.at("refuelBudgetMs").get<int>()};
    config.refuel_candidate_limit = parameters.at("refuelCandidates").get<std::size_t>();
    config.rendezvous_candidate_limit = parameters.at("rendezvousCandidates").get<std::size_t>();
    config.maximum_refuels_per_patrol = parameters.at("maximumRefuelsPerPatrol").get<std::size_t>();
    config.optimizer_budget = std::chrono::milliseconds{parameters.at("optimizerBudgetMs").get<int>()};
    config.optimizer_iterations = parameters.at("optimizerIterations").get<std::size_t>();
    config.optimizer_initial_temperature = parameters.at("optimizerInitialTemperature").get<double>();
    config.optimizer_final_temperature = parameters.at("optimizerFinalTemperature").get<double>();
    config.polling_interval = std::chrono::milliseconds{profile.at("pollingMilliseconds").get<int>()};
    config.safety_margin = std::chrono::seconds{profile.at("safetyMarginSeconds").get<int>()};
}
}
