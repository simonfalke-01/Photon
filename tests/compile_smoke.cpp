#include <array>
#include <cstdint>
#include <limits>
#include <lsp/core/core.h>
#include <lsp/input_plane/input_plane.h>
#include <lsp/media/media.h>
#include <lsp/transport/transport.h>
#include <span>

static_assert(lumen::lsp::core_api_version == 1);

namespace {
  using lumen::lsp::transport::lspv_coalescer;
  using lumen::lsp::transport::lspv_decoder_status;
  using lumen::lsp::transport::lspv_event;
  using lumen::lsp::transport::lspv_status;
  using lumen::lsp::transport::lspv_update_result;
  using lumen::lsp::transport::recovery_cause;
  using lumen::lsp::transport::recovery_controller;
  using lumen::lsp::transport::recovery_result;

  /** @brief Verify recovery epochs advance from the nonzero configured origin. */
  consteval bool recovery_epoch_seed_smoke() {
    recovery_controller controller {41};
    if (controller.snapshot().epoch != 41 ||
        controller.report_unrecoverable_damage(700, recovery_cause::decoder_unrecoverable) != recovery_result::accepted) {
      return false;
    }
    if (controller.snapshot().epoch != 42 || controller.mark_independent_frame_sent(800, 1'000, 2'000) != recovery_result::accepted ||
        controller.confirm_recovery_decoded_through(42, 799) != recovery_result::wrong_recovery_frame) {
      return false;
    }
    if (controller.confirm_recovery_decoded_through(42, 801) != recovery_result::accepted) {
      return false;
    }
    recovery_controller exhausted {std::numeric_limits<std::uint32_t>::max()};
    return exhausted.report_unrecoverable_damage(1, recovery_cause::decoder_unrecoverable) ==
           recovery_result::epoch_exhausted;
  }

  /** @brief Verify decoder-error LSPV evidence survives lossy capacity coalescing. */
  consteval bool lspv_error_latch_smoke() {
    lspv_coalescer coalescer;
    const lspv_status terminal {
      .session_generation = 2,
      .video_generation = 3,
      .recovery_epoch = 7,
      .largest_complete_frame_id = 100,
      .largest_decoded_frame_id = 90,
      .largest_metal_committed_frame_id = 80,
      .deadline_miss = true,
      .decoder_status = lspv_decoder_status::unrecoverable_error,
      .decode_submissions_in_flight = 1,
      .maximum_decode_submissions = 3,
      .render_mailbox_occupancy = 1,
    };
    if (coalescer.update(terminal, lspv_event::decode_completion) != lspv_update_result::accepted) {
      return false;
    }
    auto downgrade = terminal;
    downgrade.largest_complete_frame_id = 101;
    downgrade.largest_decoded_frame_id = 91;
    downgrade.deadline_miss = false;
    downgrade.decoder_status = lspv_decoder_status::recoverable_error;
    if (coalescer.update(downgrade, lspv_event::decode_completion) != lspv_update_result::accepted) {
      return false;
    }
    lspv_status emitted;
    lspv_event event {};
    if (!coalescer.take(0, emitted, event) ||
        emitted.largest_decoded_frame_id != 91 ||
        emitted.decoder_status != lspv_decoder_status::unrecoverable_error || !emitted.deadline_miss) {
      return false;
    }

    auto capacity = downgrade;
    capacity.deadline_miss = false;
    capacity.decoder_status = lspv_decoder_status::surface_exhausted;
    capacity.decode_submissions_in_flight = 2;
    if (coalescer.update(capacity, lspv_event::capacity_transition) != lspv_update_result::accepted ||
        !coalescer.take(500, emitted, event) ||
        emitted.decoder_status != lspv_decoder_status::unrecoverable_error || !emitted.deadline_miss ||
        emitted.decode_submissions_in_flight != 2 || event != lspv_event::capacity_transition) {
      return false;
    }

    capacity.decoder_status = lspv_decoder_status::ready;
    if (coalescer.update(capacity, lspv_event::decode_completion) != lspv_update_result::accepted ||
        !coalescer.take(1'000, emitted, event) ||
        emitted.decoder_status != lspv_decoder_status::unrecoverable_error || !emitted.deadline_miss) {
      return false;
    }

    capacity.recovery_epoch = 8;
    capacity.deadline_miss = false;
    if (coalescer.update(capacity, lspv_event::capacity_transition) != lspv_update_result::accepted ||
        !coalescer.take(1'500, emitted, event) || emitted.deadline_miss ||
        emitted.decoder_status != lspv_decoder_status::ready) {
      return false;
    }

    capacity.decoder_status = lspv_decoder_status::recoverable_error;
    if (coalescer.update(capacity, lspv_event::decode_completion) != lspv_update_result::accepted ||
        !coalescer.take(2'000, emitted, event) ||
        emitted.decoder_status != lspv_decoder_status::recoverable_error) {
      return false;
    }

    capacity.largest_complete_frame_id = 101;
    capacity.largest_decoded_frame_id = 101;
    capacity.largest_metal_committed_frame_id = 90;
    capacity.decoder_status = lspv_decoder_status::ready;
    if (coalescer.update(capacity, lspv_event::decode_completion) != lspv_update_result::accepted ||
        !coalescer.take(2'500, emitted, event) || emitted.deadline_miss ||
        emitted.decoder_status != lspv_decoder_status::ready) {
      return false;
    }

    capacity.recovery_epoch = 7;
    return coalescer.update(capacity, lspv_event::capacity_transition) == lspv_update_result::stale_generation;
  }

  /** @brief Verify a deadline-only update cannot be erased before coalesced transmission. */
  consteval bool lspv_deadline_latch_smoke() {
    lspv_coalescer coalescer;
    lspv_status status {
      .session_generation = 5,
      .video_generation = 6,
      .recovery_epoch = 9,
      .largest_complete_frame_id = 10,
      .largest_decoded_frame_id = 10,
      .largest_metal_committed_frame_id = 9,
      .deadline_miss = true,
      .decoder_status = lspv_decoder_status::ready,
    };
    if (coalescer.update(status, lspv_event::capacity_transition) != lspv_update_result::accepted) {
      return false;
    }
    status.deadline_miss = false;
    if (coalescer.update(status, lspv_event::capacity_transition) != lspv_update_result::accepted) {
      return false;
    }
    lspv_status emitted;
    lspv_event event {};
    if (!coalescer.take(0, emitted, event) || !emitted.deadline_miss) {
      return false;
    }
    status.largest_complete_frame_id = 11;
    status.largest_decoded_frame_id = 11;
    if (coalescer.update(status, lspv_event::decode_completion) != lspv_update_result::accepted ||
        !coalescer.take(500, emitted, event)) {
      return false;
    }
    return !emitted.deadline_miss && emitted.decoder_status == lspv_decoder_status::ready;
  }
}  // namespace

static_assert(recovery_epoch_seed_smoke());
static_assert(lspv_error_latch_smoke());
static_assert(lspv_deadline_latch_smoke());

int main() {
  constexpr std::array<std::uint8_t, 1> dtls_record {22};
  static_assert(lumen::lsp::classify_packet(std::span<const std::uint8_t> {dtls_record}) == lumen::lsp::packet_class::dtls);

  constexpr lumen::lsp::dplpmtud path {{.base_payload = 1'200, .fast_probe_payload = 1'400, .ceiling_payload = 1'472}};
  static_assert(path.valid());
  static_assert(path.current_payload() == 1'200);
  return 0;
}
