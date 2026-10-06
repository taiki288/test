#include "hexa_udon/protocol/request_id_digest.hpp"
#include "hexa_udon/app/lan_worker.hpp"

#include <cassert>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace {
void require(bool condition) {
    if (!condition) std::abort();
}
}

int main() {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    const auto deadline = start + std::chrono::milliseconds{1000};
    // The connect, write, and read phases all use the same absolute deadline,
    // so each phase receives only the time still available at its start.
    require(hexa_udon::app::remaining_worker_timeout(deadline,
        start).count() == 1000);
    require(hexa_udon::app::remaining_worker_timeout(deadline,
        start + std::chrono::milliseconds{250}).count() == 750);
    require(hexa_udon::app::remaining_worker_timeout(deadline,
        start + std::chrono::milliseconds{750}).count() == 250);
    require(hexa_udon::app::remaining_worker_timeout(deadline,
        deadline).count() == 0);
    require(hexa_udon::app::remaining_worker_timeout(deadline,
        deadline + std::chrono::milliseconds{1}).count() == 0);

    const auto planning_started = start + std::chrono::milliseconds{100};
    const auto reply_deadline = hexa_udon::app::worker_reply_deadline(
        planning_started, std::chrono::milliseconds{5900},
        std::chrono::milliseconds{100});
    require(hexa_udon::app::remaining_worker_timeout(
        reply_deadline, start + std::chrono::milliseconds{5900}).count() == 200);
    require(hexa_udon::app::remaining_worker_timeout(
        reply_deadline, reply_deadline).count() == 0);
    require(hexa_udon::app::classify_worker_failure("worker connection failed", false)
        == "connect-failure");
    require(hexa_udon::app::classify_worker_failure("worker write timeout", true)
        == "write-timeout");
    require(hexa_udon::app::classify_worker_failure("worker read timeout", true)
        == "read-timeout");
    require(hexa_udon::app::classify_worker_failure("worker reply eof", false)
        == "reply-eof");
    require(hexa_udon::app::classify_worker_failure("authentication-or-protocol-mismatch", false)
        == "auth-protocol-mismatch");
    require(hexa_udon::app::classify_worker_failure("worker reply malformed", false)
        == "frame-decode-failure");

    using hexa_udon::protocol::request_id_digest_or_missing;
    const auto make_reply = [](int index, const char* termination,
                               const char* strict, const char* adoption) {
        return nlohmann::json{{"requestIdDigest", "same-request-digest"},
                              {"workerIndex", index}, {"workerCount", 2},
                              {"plannerSeed", 30013 + index}, {"termination", termination},
                              {"workerTermination", termination},
                              {"workerBestCandidateAtLocalDeadline", termination[0] == 'd'},
                              {"workerBestCandidateStrictVerified", termination[0] == 'd'},
                              {"strictRevalidation", strict}, {"adoption", adoption}};
    };
    const nlohmann::json request = {{"requestId", "same-request"}, {"workerCount", 2}};
    const auto sent = request_id_digest_or_missing(request.at("requestId"));
    const auto received = request_id_digest_or_missing(request.at("requestId"));
    require(sent == received);

    const auto success0 = make_reply(0, "completed", "passed", "adopted");
    const auto success1 = make_reply(1, "completed", "passed", "adopted");
    require(success0.at("requestIdDigest") == success1.at("requestIdDigest"));
    require(success0.at("workerIndex") == 0 && success1.at("workerIndex") == 1);
    require(success0.at("workerCount") == 2 && success1.at("workerCount") == 2);
    require(success0.at("plannerSeed") != success1.at("plannerSeed"));
    require(success0.at("strictRevalidation") == "passed");
    require(success0.at("adoption") == "adopted");

    // A worker-local deadline claim is independent from the main PC's
    // shared-deadline result and is not a claim-mismatch by itself.
    require(success0.at("workerTermination") == "completed");
    require(success0.at("workerBestCandidateAtLocalDeadline") == false);
    require(success0.at("workerBestCandidateStrictVerified") == false);
    const auto local_deadline = make_reply(0, "deadline_exhausted_best_available", "passed", "baseline-retained");
    require(local_deadline.at("workerBestCandidateAtLocalDeadline") == true);
    require(local_deadline.at("workerBestCandidateStrictVerified") == true);

    const auto timeout = make_reply(1, "timeout", "not-evaluated", "baseline-retained");
    require(timeout.at("termination") == "timeout");
    require(timeout.at("adoption") == "baseline-retained");
    require(success0.at("adoption") == "adopted");

    const nlohmann::json mismatch = {
        {"workerIndex", 1}, {"workerCount", 2}, {"claimMismatchFields", {"mapIdentity"}},
        {"rejectionReason", "claim-mismatch"}, {"adoption", "baseline-retained"}};
    require(mismatch.at("claimMismatchFields").at(0) == "mapIdentity");
    require(mismatch.at("rejectionReason") == "claim-mismatch");

    const nlohmann::json strict_failure = {
        {"strictRevalidation", "failed"}, {"comparison", "not-evaluated"},
        {"rejectionReason", "strict-revalidation-failed"},
        {"adoption", "baseline-retained"}};
    require(strict_failure.at("comparison") == "not-evaluated");
    require(strict_failure.at("adoption") == "baseline-retained");
    require(request_id_digest_or_missing(nlohmann::json::object()) == "missing");

    // Old replies without the new diagnostic fields remain decodable.
    const nlohmann::json old_reply = {{"termination", "completed"},
                                      {"bestCandidateAtDeadline", false},
                                      {"bestCandidateStrictVerified", false}};
    require(old_reply.value("workerTermination", old_reply.value("termination", "")) == "completed");
    require(old_reply.value("workerBestCandidateAtLocalDeadline",
        old_reply.value("bestCandidateAtDeadline", false)) == false);
}
