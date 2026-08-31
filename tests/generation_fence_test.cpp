/**
 * @file tests/generation_fence_test.cpp
 * @brief Focused reconnect-generation fence transition tests.
 */

#include "lsp/transport/generation_fence.h"

#include <cstdint>
#include <iostream>

namespace {
  namespace transport = lumen::lsp::transport;

  /** @brief Return failure after printing the source line of a rejected test condition. */
  int fail(const int line) {
    std::cerr << "generation fence assertion failed at line " << line << '\n';
    return 1;
  }

#define PHOTON_REQUIRE(condition) \
  do { \
    if (!(condition)) { \
      return fail(__LINE__); \
    } \
  } while (false)

  /** @brief Prove definite ATTACH commit advances authority before explicit old-association retirement. */
  int test_successful_commit_and_retirement() {
    transport::reconnect_generation_fence fence;
    PHOTON_REQUIRE(fence.initialize(1, 10) == transport::generation_fence_result::accepted);
    auto snapshot = fence.snapshot();
    PHOTON_REQUIRE(snapshot.phase == transport::generation_fence_phase::authoritative);
    PHOTON_REQUIRE(snapshot.active_connection_generation == 1);
    PHOTON_REQUIRE(snapshot.active_input_generation == 10);
    PHOTON_REQUIRE(snapshot.input_authority_epoch == 1);
    PHOTON_REQUIRE(snapshot.connection_generation_high_watermark == 1);
    PHOTON_REQUIRE(fence.accepts_input_packet(1, 10, 1));

    PHOTON_REQUIRE(fence.begin_fresh_association(2) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.begin_fresh_association(3) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.mark_candidate_authenticated(3) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.mark_candidate_authenticated(2) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.commit_attach(2, 11) == transport::generation_fence_result::input_not_neutralized);
    PHOTON_REQUIRE(fence.neutralize_old_input(3) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.neutralize_old_input(2) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.accepts_connection_packet(1));
    PHOTON_REQUIRE(!fence.accepts_input_packet(1, 10, 1));
    PHOTON_REQUIRE(fence.commit_attach(3, 11) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.commit_attach(2, 10) == transport::generation_fence_result::invalid_generation);
    PHOTON_REQUIRE(fence.commit_attach(2, 11) == transport::generation_fence_result::accepted);

    snapshot = fence.snapshot();
    PHOTON_REQUIRE(snapshot.phase == transport::generation_fence_phase::attach_committed);
    PHOTON_REQUIRE(snapshot.active_connection_generation == 2);
    PHOTON_REQUIRE(snapshot.active_input_generation == 11);
    PHOTON_REQUIRE(snapshot.input_authority_epoch == 2);
    PHOTON_REQUIRE(snapshot.connection_generation_high_watermark == 2);
    PHOTON_REQUIRE(snapshot.candidate_connection_generation == 0);
    PHOTON_REQUIRE(snapshot.retiring_connection_generation == 1);
    PHOTON_REQUIRE(!snapshot.old_input_neutralized && !snapshot.baseline_required);
    PHOTON_REQUIRE(!fence.accepts_connection_packet(1));
    PHOTON_REQUIRE(!fence.accepts_input_packet(2, 11, 1));
    PHOTON_REQUIRE(fence.accepts_input_packet(2, 11, 2));
    PHOTON_REQUIRE(fence.cancel_candidate(2) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.reject_attach(2, 44) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.begin_fresh_association(3) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.retire_old_association(2) == transport::generation_fence_result::retiring_mismatch);
    PHOTON_REQUIRE(fence.retire_old_association(1) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.snapshot().phase == transport::generation_fence_phase::authoritative);
    PHOTON_REQUIRE(fence.retire_old_association(1) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.begin_fresh_association(3) == transport::generation_fence_result::accepted);
    return 0;
  }

  /** @brief Prove definite rejection reopens the unchanged old input generation only after its exact baseline ACK. */
  int test_definite_rejection_rollback() {
    constexpr std::uint64_t required_baseline_id = 0x1122'3344'5566'7788ULL;
    transport::reconnect_generation_fence fence;
    PHOTON_REQUIRE(fence.initialize(7, 23) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.begin_fresh_association(8) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.mark_candidate_authenticated(8) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.neutralize_old_input(8) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.reject_attach(9, required_baseline_id) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.reject_attach(8, 0) == transport::generation_fence_result::baseline_mismatch);
    PHOTON_REQUIRE(fence.reject_attach(8, required_baseline_id) == transport::generation_fence_result::accepted);

    auto snapshot = fence.snapshot();
    PHOTON_REQUIRE(snapshot.phase == transport::generation_fence_phase::rollback_baseline_pending);
    PHOTON_REQUIRE(snapshot.active_connection_generation == 7);
    PHOTON_REQUIRE(snapshot.active_input_generation == 23);
    PHOTON_REQUIRE(snapshot.input_authority_epoch == 7);
    PHOTON_REQUIRE(snapshot.connection_generation_high_watermark == 8);
    PHOTON_REQUIRE(snapshot.candidate_connection_generation == 0);
    PHOTON_REQUIRE(snapshot.rollback_connection_generation == 8);
    PHOTON_REQUIRE(snapshot.required_rollback_baseline_id == required_baseline_id);
    PHOTON_REQUIRE(snapshot.old_input_neutralized && snapshot.baseline_required);
    PHOTON_REQUIRE(fence.accepts_connection_packet(7));
    PHOTON_REQUIRE(!fence.accepts_input_packet(7, 23, 7));
    PHOTON_REQUIRE(
      fence.acknowledge_old_neutral_baseline(6, 23, required_baseline_id) ==
      transport::generation_fence_result::active_authority_mismatch
    );
    PHOTON_REQUIRE(
      fence.acknowledge_old_neutral_baseline(7, 24, required_baseline_id) ==
      transport::generation_fence_result::active_authority_mismatch
    );
    PHOTON_REQUIRE(
      fence.acknowledge_old_neutral_baseline(7, 23, required_baseline_id - 1) ==
      transport::generation_fence_result::baseline_mismatch
    );
    PHOTON_REQUIRE(
      fence.acknowledge_old_neutral_baseline(7, 23, required_baseline_id) ==
      transport::generation_fence_result::accepted
    );

    snapshot = fence.snapshot();
    PHOTON_REQUIRE(snapshot.phase == transport::generation_fence_phase::authoritative);
    PHOTON_REQUIRE(snapshot.active_connection_generation == 7);
    PHOTON_REQUIRE(snapshot.active_input_generation == 23);
    PHOTON_REQUIRE(snapshot.input_authority_epoch == 8);
    PHOTON_REQUIRE(snapshot.connection_generation_high_watermark == 8);
    PHOTON_REQUIRE(snapshot.rollback_connection_generation == 0);
    PHOTON_REQUIRE(snapshot.required_rollback_baseline_id == 0);
    PHOTON_REQUIRE(!snapshot.old_input_neutralized && !snapshot.baseline_required);
    PHOTON_REQUIRE(!fence.accepts_input_packet(7, 23, 7));
    PHOTON_REQUIRE(fence.accepts_input_packet(7, 23, 8));
    PHOTON_REQUIRE(
      fence.acknowledge_old_neutral_baseline(7, 23, required_baseline_id) ==
      transport::generation_fence_result::wrong_phase
    );
    PHOTON_REQUIRE(fence.begin_fresh_association(8) == transport::generation_fence_result::invalid_generation);
    PHOTON_REQUIRE(fence.begin_fresh_association(9) == transport::generation_fence_result::accepted);
    return 0;
  }

  /** @brief Prove ambiguous ATTACH is terminal and never restores either association's authority. */
  int test_ambiguous_attach_is_terminal() {
    transport::reconnect_generation_fence fence;
    PHOTON_REQUIRE(fence.initialize(20, 30) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.begin_fresh_association(21) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.mark_candidate_authenticated(21) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.neutralize_old_input(21) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(
      fence.mark_attach_outcome_ambiguous(22) == transport::generation_fence_result::candidate_mismatch
    );
    PHOTON_REQUIRE(
      fence.mark_attach_outcome_ambiguous(21) == transport::generation_fence_result::accepted
    );

    const auto snapshot = fence.snapshot();
    PHOTON_REQUIRE(snapshot.phase == transport::generation_fence_phase::attach_outcome_ambiguous);
    PHOTON_REQUIRE(snapshot.active_connection_generation == 20);
    PHOTON_REQUIRE(snapshot.active_input_generation == 30);
    PHOTON_REQUIRE(snapshot.candidate_connection_generation == 21);
    PHOTON_REQUIRE(snapshot.old_input_neutralized);
    PHOTON_REQUIRE(!fence.accepts_connection_packet(20));
    PHOTON_REQUIRE(!fence.accepts_connection_packet(21));
    PHOTON_REQUIRE(!fence.accepts_input_packet(20, 30, 20));
    PHOTON_REQUIRE(fence.commit_attach(21, 31) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.reject_attach(21, 55) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.cancel_candidate(21) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.retire_old_association(20) == transport::generation_fence_result::wrong_phase);
    PHOTON_REQUIRE(fence.begin_fresh_association(22) == transport::generation_fence_result::wrong_phase);
    return 0;
  }

  /** @brief Prove canceled generations and late callbacks cannot mutate a later candidate. */
  int test_stale_candidate_callbacks_fail_closed() {
    transport::reconnect_generation_fence fence;
    PHOTON_REQUIRE(fence.initialize(1, 50) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.begin_fresh_association(2) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.cancel_candidate(3) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.cancel_candidate(2) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.accepts_input_packet(1, 50, 1));
    PHOTON_REQUIRE(fence.begin_fresh_association(2) == transport::generation_fence_result::invalid_generation);
    PHOTON_REQUIRE(fence.begin_fresh_association(3) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.mark_candidate_authenticated(2) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.mark_candidate_authenticated(3) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.neutralize_old_input(2) == transport::generation_fence_result::candidate_mismatch);
    PHOTON_REQUIRE(fence.neutralize_old_input(3) == transport::generation_fence_result::accepted);
    PHOTON_REQUIRE(fence.cancel_candidate(3) == transport::generation_fence_result::wrong_phase);
    return 0;
  }
}  // namespace

/** @brief Run focused generation-fence tests without an external test dependency. */
int main() {
  if (const auto result = test_successful_commit_and_retirement(); result != 0) {
    return result;
  }
  if (const auto result = test_definite_rejection_rollback(); result != 0) {
    return result;
  }
  if (const auto result = test_ambiguous_attach_is_terminal(); result != 0) {
    return result;
  }
  return test_stale_candidate_callbacks_fail_closed();
}
