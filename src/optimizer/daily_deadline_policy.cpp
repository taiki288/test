#include "hexa_udon/optimizer/daily_deadline_policy.hpp"
#include <algorithm>
#include <cctype>
#include <future>
#include <iomanip>
#include <sstream>
namespace hexa_udon::optimizer {
namespace {
using Json = nlohmann::json;
using Time = std::chrono::steady_clock::time_point;
using Ms = std::chrono::milliseconds;
std::string hash(const Json& value) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : value.dump()) { h ^= c; h *= 1099511628211ULL; }
    std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(16) << h; return out.str();
}
Json agents(const std::vector<core::AgentState>& values) {
    Json out = Json::array();
    for (const auto& a : values) out.push_back({{"kind",static_cast<int>(a.kind)}, {"position",a.position.value},{"fuel",a.fuel}});
    return out;
}
Json progress(const simulator::MatchProgress& p) {
    return {{"acquiredBrands",p.acquired_brands},{"totalBalls",p.total_balls},{"dailyDistinctBrandCounts",p.daily_distinct_brand_counts}};
}
Json score(const planner::OfficialScore& s) { return Json::array({s.total_unique_brands,s.cumulative_daily_unique_brands,s.total_bowls}); }
Json readiness(const planner::DailyReadiness& r) { return Json::array({r.uncollected_spot_reachability,r.fuel_reserve,r.patrol_dispersion,r.rendezvous_readiness}); }
Json candidate_diagnostic(const CandidateDiagnostic& d) {
    return {{"neighborhoodKind",d.neighborhood_kind},
        {"fallbackReason",d.fallback_reason},
        {"acquiredBrandCount",d.acquired_brand_count},
        {"newlyAcquiredBrandCount",d.newly_acquired_brand_count},
        {"uncollectedBrandCount",d.uncollected_brand_count},
        {"officialScoreDelta",score(d.official_score_delta)},
        {"dailyReadinessDelta",readiness(d.daily_readiness_delta)},
        {"accepted",d.accepted},{"rejectionReason",d.rejection_reason},
        {"candidateHash",d.candidate_hash},{"actionHash",d.action_hash},{"planHash",d.plan_hash}};
}
Json actions(const simulator::DayActionPlan& plan) {
    Json out=Json::array();
    for (const auto& a : plan) { Json row=Json::array(); for (const auto& v : a) {
        if (const auto* m=std::get_if<simulator::MoveAction>(&v)) row.push_back(static_cast<int>(m->direction));
        else row.push_back(-std::get<simulator::WaitAction>(v).steps);
    } out.push_back(row); } return out;
}
}
std::optional<DailyDeadlinePolicy> DailyDeadlinePolicy::for_size(std::size_t size) {
    if(size==16) return DailyDeadlinePolicy{16,Ms{1000},Ms{10000},Ms{8000},Ms{250}};
    if(size==24) return DailyDeadlinePolicy{24,Ms{2000},Ms{20000},Ms{8000},Ms{250}};
    if(size==32) return DailyDeadlinePolicy{32,Ms{5000},Ms{30000},Ms{10000},Ms{250}};
    return std::nullopt;
}
std::optional<DailyDeadlinePolicy> DailyDeadlinePolicy::for_measurement(
    std::size_t size, std::chrono::milliseconds baseline, std::chrono::milliseconds improvement) {
    auto locked = for_size(size);
    if (!locked || baseline.count() <= 0 || improvement.count() <= 0 || improvement.count() != 10000) return std::nullopt;
    locked->baseline_max = baseline;
    locked->improvement_max = improvement;
    return locked;
}
std::optional<DailyDeadlinePolicy> DailyDeadlinePolicy::for_production_size(std::size_t size) {
    auto policy = for_size(size);
    if (policy) policy->production_approved = true;
    return policy;
}
Json DailyDeadlinePolicy::identity() const {
    return {{"version",version},{"size",size},{"baselineMaxMs",baseline_max.count()},
        {"improvementMaxMs",improvement_max.count()},{"reserveMs",reserve.count()},
        {"minimumImprovementMs",minimum_improvement.count()},{"provisional",!production_approved}};
}
std::optional<Time> observed_daily_deadline(core::UnixTimestamp ends_at,
    std::chrono::system_clock::time_point wall_now, Time steady_now) {
    if (ends_at<=0) return std::nullopt;
    const auto wall_seconds=std::chrono::duration_cast<std::chrono::seconds>(wall_now.time_since_epoch()).count();
    // Reject unrepresentable or implausibly distant timestamps before duration conversion.
    if(ends_at<=wall_seconds || ends_at-wall_seconds>86400) return std::nullopt;
    const auto remaining=std::chrono::system_clock::time_point{std::chrono::seconds{ends_at}}-wall_now;
    return steady_now+std::chrono::duration_cast<Time::duration>(remaining);
}
DailyDeadlineResult run_daily_deadline_policy(const planner::PlannerInput& in,
    const DailyDeadlinePolicy& policy, std::optional<Time> deadline,
    const planner::PlannerConfig& gc, const planner::RefuelPlannerConfig& rc,
    const OptimizerConfig& oc, OptimizerClock now, const DailyDeadlineStages& stages,
    bool run_improvement, std::chrono::milliseconds worker_configured_timeout) {
    DailyDeadlineResult result;
    auto& r=result.record;
    const auto started=now();
    const auto remaining=[&] { return deadline ? std::max<std::int64_t>(0,std::chrono::duration_cast<Ms>(*deadline-now()).count()) : 0; };
    const auto micros=[&](Time t) {return std::chrono::duration_cast<std::chrono::microseconds>(now()-t).count();};
    Json traffic=Json::array(), terrain=Json::array(), types=Json::array(), spots=Json::array();
    for(const auto& road:in.daily.traffic) traffic.push_back({{"cell",road.position.value},{"status",static_cast<int>(road.status)}});
    for(std::int32_t i=0;i<in.match.map.cell_count();++i) terrain.push_back(static_cast<int>(*in.match.map.terrain_at({static_cast<std::int32_t>(i)})));
    for(const auto& a:in.daily.own_agents) types.push_back(static_cast<int>(a.kind));
    for(const auto& s:in.match.spots) spots.push_back({{"position",s.position.value},{"brand",s.brand},{"stock",s.max_stock}});
    const auto start_state=Json{{"agents",agents(in.daily.own_agents)},{"progress",progress(in.previous_progress)}};
    const auto snapshot=Json{{"day",in.daily.day},{"terrain",terrain},{"traffic",traffic},{"spots",spots}};
    const auto input_hash=hash(Json{{"snapshot",snapshot},{"state",start_state},{"types",types},
        {"greedySeed",gc.seed},{"refuelSeed",rc.seed},{"greedyCandidates",gc.maximum_candidate_evaluations},
        {"fuelLimit",in.match.fuel_limit},{"daySteps",in.match.day_steps},
        {"baselineMaxMs",policy.baseline_max.count()},
        {"refuelCandidates",rc.maximum_candidate_evaluations},{"rendezvousCandidates",rc.maximum_rendezvous_candidates},
        {"maximumRefuels",rc.maximum_refuels_per_patrol}});
    r={{"schemaVersion",1},{"policy",policy.identity()},{"day",in.daily.day},{"types",types},
       {"snapshotHash",hash(snapshot)},{"startStateHash",hash(start_state)},{"progressHash",hash(progress(in.previous_progress))},
       {"baselineInputHash",input_hash},{"remainingBudgetMs",remaining()},{"baselineRemainingMs",nullptr},
       {"hardPlanningDeadline",nullptr},{"effectiveStopPoint","shared-hard-planning-deadline"},
       {"reserveMeaning","optimizer-admission-and-fallback-guard-only"},{"dayStartRemainingMs",remaining()},
       {"baselineGreedyUs",0},{"baselineRefuelUs",0},{"baselineSimulatorUs",0},{"baselineWallUs",0},
       {"plannerSeed",gc.seed},{"improvementSeed",oc.seed},{"baselineCandidateLimit",gc.maximum_candidate_evaluations},
       {"refuelCandidateLimit",rc.maximum_candidate_evaluations},{"baselineEvaluatedCandidates",0},
       {"improvementSimulatorUs",0},{"baselineTermination","not-started"},{"baselineFailureReason",""},{"baselineActionHash",nullptr},
       {"baselinePlanHash",nullptr},{"baselineEndStateHash",nullptr},{"baselineScore",nullptr},{"baselineReadiness",nullptr},
       {"baselineParity",{{"scope","same-day-start-state"},{"inputHash",input_hash},{"startStateHash",hash(start_state)},
           {"normalBudgetMs",policy.baseline_max.count()},{"status","not-verified"}}},
       {"improvementStarted",false},{"improvementUs",0},{"improvementStartRemainingMs",nullptr},{"improvementEndRemainingMs",nullptr},
       {"workerConfiguredTimeoutMs",worker_configured_timeout.count()},{"workerEffectiveTimeoutMs",0},
       {"workerElapsedMs",0},{"remainingBeforeOptimizerMs",nullptr},{"effectiveImprovementBudgetMs",0},
       {"workerFallbackReason",""},{"workerCandidateReturned",false},
       {"workerFinalAdoptionPending",false},
       {"improvementTermination","not-started"},{"improvementFailureReason",""},
       {"evaluatedCandidates",0},{"validCandidates",0},{"acceptedCandidates",0},
       {"candidateActionHash",nullptr},{"candidatePlanHash",nullptr},{"candidateEndStateHash",nullptr},
       {"bestCandidateAtDeadline",false},{"bestCandidateActionHash",nullptr},{"bestCandidatePlanHash",nullptr},
       {"bestCandidateEndStateHash",nullptr},{"bestCandidateOfficialScore",nullptr},{"bestCandidateReadiness",nullptr},
       {"bestCandidateNeighborhoodKind",nullptr},{"bestCandidateEvaluatedAtUs",nullptr},
       {"bestCandidateStrictVerified",false},
       {"actionHash",nullptr},{"planHash",nullptr},{"endStateHash",nullptr},{"score",nullptr},{"readiness",nullptr},
       {"termination","input_failure"},{"failureReason",""},{"adoptionReason","no-validated-baseline"},
       {"futureSnapshotRead",false},{"lookaheadRequestCount",0},{"waitFallbackGenerated",false}};
    const auto fail=[&](const std::string& termination,const std::string& reason) {
        r["termination"]=termination; r["failureReason"]=reason;
        r["baselineTermination"]=termination; r["baselineFailureReason"]=reason;
        r["baselineWallUs"]=micros(started); r["baselineRemainingMs"]=remaining();
        r["finalRemainingMs"]=remaining(); r["totalWallUs"]=micros(started); return result;
    };
    if(!core::validate(in.match).empty() || !core::validate(in.daily,in.match).empty())
        return fail("input_failure","invalid current-day input");
    const auto locked=policy.production_approved ? DailyDeadlinePolicy::for_production_size(policy.size)
        : DailyDeadlinePolicy::for_size(policy.size);
    const bool measurement_override = !policy.production_approved && locked
        && policy.size == locked->size && policy.reserve == locked->reserve
        && policy.minimum_improvement == locked->minimum_improvement
        && policy.improvement_max == Ms{10000}
        && (policy.baseline_max == Ms{1000} || policy.baseline_max == Ms{2000} || policy.baseline_max == Ms{5000});
    if(!locked || (locked->identity()!=policy.identity() && !measurement_override)
        || static_cast<std::size_t>(in.match.map.height())!=policy.size || static_cast<std::size_t>(in.match.map.width())!=policy.size)
        return fail("input_failure","policy identity or size mismatch");
    const auto expected=policy.size==16?4U:policy.size==24?5U:7U;
    const auto supply=std::count(types.begin(),types.end(),1);
    if(types.size()!=expected || (policy.size==16 && supply!=1) || (policy.size==24 && supply!=2)
        || (policy.size==32 && supply!=1 && supply!=3)) return fail("input_failure","type allowlist mismatch");
    if(in.daily.day<0 || static_cast<std::size_t>(in.daily.day)>=in.match.day_steps.size()
        || in.previous_progress.daily_distinct_brand_counts.size()!=static_cast<std::size_t>(in.daily.day))
        return fail("input_failure","day/progress identity mismatch");
    if(in.daily.day==0 && std::any_of(in.daily.traffic.begin(),in.daily.traffic.end(),[](const auto& x){return x.status!=core::RoadStatus::Smooth;}))
        return fail("input_failure","Day0 roads must all be smooth");
    if(!deadline || remaining()<=0) return fail("deadline_exhausted","missing or expired daily deadline");
    // The caller supplies hardPlanningDeadline = endsAt - communication reserve.
    // policy.reserve is a start/admission guard, not another wall-clock subtraction.
    const auto stop=*deadline;
    constexpr Ms reply_grace{100};
    r["replyGraceMs"] = reply_grace.count();
    const auto baseline_start=now();
    const auto baseline_deadline=std::min(stop,baseline_start+policy.baseline_max);
    r["baselineCreatedAfterStartUs"]=micros(started);
    r["baselineDeadlineRemainingMs"]=std::chrono::duration_cast<Ms>(baseline_deadline-baseline_start).count();
    auto stage_start=now(); auto greedy=stages.greedy(in,gc,baseline_deadline,now);
    r["baselineGreedyUs"]=micros(stage_start);
    if(!greedy) return fail("planner_failure","greedy planner failed");
    if(now()>=baseline_deadline || greedy.value().termination==planner::PlannerTermination::Deadline || greedy.value().termination==planner::PlannerTermination::Fallback)
        return fail("deadline_exhausted","greedy did not complete within baseline deadline");
    stage_start=now(); auto refuel=stages.refuel(in,greedy.value(),rc,baseline_deadline,now);
    r["baselineRefuelUs"]=micros(stage_start);
    if(!refuel) return fail("planner_failure","refuel planner failed");
    if(now()>=baseline_deadline || refuel.value().termination==planner::RefuelTermination::Deadline || refuel.value().termination==planner::RefuelTermination::Fallback)
        return fail("deadline_exhausted","refuel did not complete within baseline deadline");
    const simulator::DaySimulationInput sim_input{in.match.map,in.match.spots,in.match.fuel_limit,
        in.match.day_steps[static_cast<std::size_t>(in.daily.day)],in.daily.own_agents,in.daily.traffic};
    stage_start=now(); auto verified=stages.simulate(sim_input,refuel.value().plan,simulator::TraceMode::Disabled);
    r["baselineSimulatorUs"]=micros(stage_start);
    if(!verified) return fail("simulation_failure","baseline strict Simulator failed");
    if(now()>=baseline_deadline) return fail("deadline_exhausted","baseline verification exceeded deadline");
    r["baselineEvaluatedCandidates"]=greedy.value().evaluated_candidates+refuel.value().evaluated_candidates;
    const auto bs=planner::official_score(in.previous_progress,verified.value());
    const auto br=planner::daily_readiness(in.match,in.daily,verified.value(),refuel.value().visited_spots);
    const auto record_plan=[&](const simulator::DayActionPlan& p,const simulator::DaySimulationResult& s) {
        auto a=actions(p); return Json{{"actionHash",hash(a)},
            {"planHash",hash(Json{{"actions",a},{"types",types},{"day",in.daily.day}})},
            {"endStateHash",hash(Json{{"agents",agents(s.end_agents)},{"remainingStock",s.remaining_stock},
                {"progress",progress(simulator::accumulate_progress(in.previous_progress,s))}})}};
    };
    const auto base=record_plan(refuel.value().plan,verified.value());
    for(const auto& key:{"ActionHash","PlanHash","EndStateHash"}) {
        std::string k=key; k[0]=static_cast<char>(std::tolower(k[0])); r[std::string("baseline")+key]=base[k];
    }
    r["baselineScore"]=score(bs); r["baselineReadiness"]=readiness(br);
    r["baselineTermination"]="completed"; r["baselineWallUs"]=micros(baseline_start);
    r["baselineRemainingMs"]=remaining(); r["baselineParity"]["status"]="same-input-baseline-verified";
    result.plan=refuel.value().plan; result.simulation=verified.value();
    auto final_score=bs; auto final_readiness=br;
    r["candidateSource"]="baseline";
    r["adoptionReason"]="baseline-retained";

    std::future<OptimizerOutcome> improvement_future;
    bool improvement_started = false;
    Time improvement_start{};
    Time improvement_deadline = stop;
    r["remainingBeforeOptimizerMs"] = remaining();
    const auto remaining_for_improvement = remaining();
    const auto stage_remaining = std::max<std::int64_t>(
        0, remaining_for_improvement - reply_grace.count());
    const auto configured_improvement_cap = policy.size == 32
        ? std::max<std::int64_t>(0, stage_remaining - policy.minimum_improvement.count())
        : policy.improvement_max.count();
    const auto improvement_budget = std::min<std::int64_t>(configured_improvement_cap,
        stage_remaining);
    r["effectiveImprovementBudgetMs"] = improvement_budget;
    if (run_improvement && remaining() > policy.minimum_improvement.count()
        && improvement_budget > 0) {
        improvement_start = now();
        improvement_deadline = std::min(stop - reply_grace,
            improvement_start + Ms{improvement_budget});
        auto config = oc;
        config.prefer_daily_readiness_on_tie = true;
        r["improvementCreatedAfterStartUs"] = micros(started);
        r["improvementStarted"] = true;
        r["improvementStartRemainingMs"] = remaining();
        r["improvementDeadlineRemainingMs"] = std::chrono::duration_cast<Ms>(
            improvement_deadline - improvement_start).count();
        improvement_started = true;
        improvement_future = std::async(std::launch::async,
            [&, config, improvement_deadline] {
                return stages.improve(in, greedy.value(), refuel.value(), config,
                    improvement_deadline, now);
            });
    }
    if (stages.worker && worker_configured_timeout.count() > 0) {
        const auto worker_started = now();
        const auto worker_remaining = remaining();
        const auto worker_stage_remaining = std::max<std::int64_t>(
            0, worker_remaining - reply_grace.count());
        const auto effective = std::max<std::int64_t>(0, std::min<std::int64_t>(
            worker_configured_timeout.count(), worker_stage_remaining));
        r["workerEffectiveTimeoutMs"] = effective;
        r["workerStarted"] = effective > 0;
        r["workerStartRemainingMs"] = worker_remaining;
        if (effective > 0) {
            auto worker = stages.worker(in, greedy.value(), refuel.value(), verified.value(),
                *deadline, Ms{effective}, now);
            r["workerElapsedMs"] = micros(worker_started) / 1000;
            r["workerEndRemainingMs"] = remaining();
            r["worker"] = worker.record;
            r["workerFallbackReason"] = worker.reason;
            if (worker.plan && worker.simulation && now() < *deadline) {
                result.plan = std::move(worker.plan);
                result.simulation = std::move(worker.simulation);
                // The policy runner returns a strict candidate for App-level
                // claim validation and score/readiness comparison. It does not
                // make the final worker adoption decision.
                r["workerCandidateReturned"] = true;
                r["workerFinalAdoptionPending"] = true;
                r["candidateSource"] = "worker";
                r["adoptionReason"] = "worker-candidate-for-app-review";
                if (worker.record.contains("score") && worker.record["score"].is_array()
                    && worker.record["score"].size() == 3) {
                    final_score = {worker.record["score"][0], worker.record["score"][1], worker.record["score"][2]};
                }
                if (worker.record.contains("readiness") && worker.record["readiness"].is_array()
                    && worker.record["readiness"].size() == 4) {
                    final_readiness = {worker.record["readiness"][0], worker.record["readiness"][1],
                        worker.record["readiness"][2], worker.record["readiness"][3], {}};
                }
            }
        } else {
            r["workerStarted"] = false;
            r["workerFallbackReason"] = "reserve-exhausted";
        }
    } else {
        r["workerStarted"] = false;
        r["workerFallbackReason"] = "worker-not-configured";
    }
    if (improvement_started) {
        auto improved = improvement_future.get();
        r["improvementUs"] = micros(improvement_start);
        r["improvementEndRemainingMs"] = remaining();
        if(!improved) {r["improvementTermination"]=now()>=improvement_deadline?"deadline_exhausted":"planner_failure";r["improvementFailureReason"]="optimizer failed; baseline retained";}
        else {
            const auto& c=improved.value(); r["evaluatedCandidates"]=c.generated_candidates; r["validCandidates"]=c.valid_candidates; r["acceptedCandidates"]=c.accepted_candidates;
            r["candidateDiagnostics"] = Json::array();
            for (const auto& diagnostic : c.candidate_diagnostics)
                r["candidateDiagnostics"].push_back(candidate_diagnostic(diagnostic));
            const bool deadline_result = now()>=improvement_deadline || c.termination==OptimizerTermination::Deadline;
            const bool deadline_best = c.termination==OptimizerTermination::Deadline
                && c.best_candidate_at_deadline && c.best_candidate_strict_verified;
            if (deadline_best) {
                const auto cp=record_plan(c.plan,c.simulation);
                const auto cs=planner::official_score(in.previous_progress,c.simulation);
                std::vector<std::vector<std::size_t>> visits(types.size());
                bool valid_routes=true;
                for(const auto& route:c.solution.patrol_routes) {
                    if(route.agent_index>=visits.size()) {valid_routes=false;break;}
                    visits[route.agent_index]=route.spot_indices;
                }
                const auto cr=planner::daily_readiness(in.match,in.daily,c.simulation,visits);
                r["bestCandidateAtDeadline"]=true;
                r["bestCandidateActionHash"]=cp["actionHash"];
                r["bestCandidatePlanHash"]=cp["planHash"];
                r["bestCandidateEndStateHash"]=cp["endStateHash"];
                r["bestCandidateOfficialScore"]=score(cs);
                r["bestCandidateReadiness"]=readiness(cr);
                r["bestCandidateNeighborhoodKind"]=c.best_candidate_neighborhood_kind;
                r["bestCandidateEvaluatedAtUs"]=c.best_candidate_evaluated_at_us;
                r["bestCandidateStrictVerified"]=true;
                r["candidateActionHash"]=cp["actionHash"];
                r["candidatePlanHash"]=cp["planHash"];
                r["candidateEndStateHash"]=cp["endStateHash"];
                const auto decision=valid_routes ? evaluate_daily_improvement(bs,br,cs,cr) : DailyImprovementDecision{};
                r["improvementTermination"]="deadline_exhausted_best_available";
                r["improvementFailureReason"]="";
                if(valid_routes && now()<stop && decision.adopt) {
                    result.plan=c.plan; result.simulation=c.simulation; final_score=cs; final_readiness=cr;
                    r["candidateSource"]="main";
                    r["adoptionReason"]=decision.reason==DailyImprovementDecisionReason::OfficialScoreImproved
                        ? "official-score-improved" : "readiness-tie-break";
                } else {
                    r["adoptionReason"]="baseline-retained";
                }
            } else if(deadline_result || c.termination==OptimizerTermination::Fallback) {
                r["improvementTermination"]="deadline_exhausted";r["improvementFailureReason"]="optimizer deadline/fallback; baseline retained";
            } else {
                const auto candidate_started=now();
                auto candidate=stages.simulate(sim_input,c.plan,simulator::TraceMode::Disabled);
                r["improvementSimulatorUs"]=micros(candidate_started);
                if(!candidate) {r["improvementTermination"]="simulation_failure";r["improvementFailureReason"]="candidate strict Simulator failed";}
                else {
                    const auto cp=record_plan(c.plan,candidate.value());
                    r["candidateActionHash"]=cp["actionHash"];r["candidatePlanHash"]=cp["planHash"];r["candidateEndStateHash"]=cp["endStateHash"];
                    const auto cs=planner::official_score(in.previous_progress,candidate.value());
                    std::vector<std::vector<std::size_t>> visits(types.size());
                    bool valid_routes=true;
                    for(const auto& route:c.solution.patrol_routes) {
                        if(route.agent_index>=visits.size()) {valid_routes=false;break;}
                        visits[route.agent_index]=route.spot_indices;
                    }
                    const auto cr=planner::daily_readiness(in.match,in.daily,candidate.value(),visits);
                    const auto decision=evaluate_daily_improvement(bs,br,cs,cr);
                    r["improvementTermination"]=now()<improvement_deadline?"completed":"deadline_exhausted";
                    if(valid_routes && now()<improvement_deadline && decision.adopt) {result.plan=c.plan;result.simulation=candidate.value();final_score=cs;final_readiness=cr;
                        r["candidateSource"]="main";
                        r["adoptionReason"]=decision.reason==DailyImprovementDecisionReason::OfficialScoreImproved?"official-score-improved":"readiness-tie-break";}
                }
            }
        }
        r["improvementUs"] = micros(improvement_start);
        r["improvementEndRemainingMs"] = remaining();
    } else {
        r["improvementTermination"]="not-started";
        r["improvementFailureReason"]=run_improvement?"insufficient remaining time for minimum improvement":"explicitly disabled";
    }
    if (r.value("candidateSource", "baseline") == "worker"
        && r.value("adoptionReason", "") == "worker-candidate-for-app-review") {
        r["adoptionReason"] = r.value("workerAdoptionReason", "official-score-improved");
    }
    const auto final=record_plan(*result.plan,*result.simulation);
    for(const auto& key:{"actionHash","planHash","endStateHash"}) r[key]=final[key];
    r["score"]=score(final_score);r["readiness"]=readiness(final_readiness);
    Json predicted_brands=Json::array();
    for (const auto brand : result.simulation->distinct_brands) predicted_brands.push_back(brand);
    r["predictedBrands"]=predicted_brands;
    r["predictedBalls"]=result.simulation->total_balls;
    r["termination"]="completed";r["finalRemainingMs"]=remaining();r["totalWallUs"]=micros(started);
    if(now()>=stop) {result.plan.reset();result.simulation.reset();r["termination"]="deadline_exhausted";r["failureReason"]="hard planning deadline reached; do not submit";}
    return result;
}
} // namespace hexa_udon::optimizer
