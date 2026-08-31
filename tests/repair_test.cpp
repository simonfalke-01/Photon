#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <lsp/transport/repair.h>
#include <lsp/transport/rtcp.h>
#include <span>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<lumen::lsp::transport::rtx_retention>);
static_assert(!std::is_copy_assignable_v<lumen::lsp::transport::rtx_retention>);
static_assert(!std::is_move_constructible_v<lumen::lsp::transport::rtx_retention>);
static_assert(!std::is_move_assignable_v<lumen::lsp::transport::rtx_retention>);

namespace {
  namespace transport = lumen::lsp::transport;

  /**
   * @brief Report one focused test failure.
   *
   * @param condition Required condition.
   * @param line Source line naming the failed condition.
   * @return Zero on success or the failing source line.
   */
  int require(const bool condition, const int line) {
    if (!condition) {
      std::fprintf(stderr, "Photon repair test failed at line %d\n", line);
      return line;
    }
    return 0;
  }

  /** @brief Return immediately from a focused test when a condition fails. */
#define PHOTON_REQUIRE(condition) \
  do { \
    if (const auto failed = require(static_cast<bool>(condition), __LINE__); failed != 0) { \
      return failed; \
    } \
  } while (false)

  /** @brief Verify RFC 8627 tail/header recovery and all LSP mask sizes. */
  int test_flexfec_wire_and_recovery() {
    constexpr std::uint32_t primary_ssrc = 0x1122'3344U;
    constexpr std::uint32_t repair_ssrc = 0x5566'7788U;
    constexpr std::array<std::uint8_t, 15> first {
      0x80,
      0xe0,
      0xff,
      0xff,
      0x00,
      0x00,
      0x00,
      0x64,
      0x11,
      0x22,
      0x33,
      0x44,
      1,
      2,
      3,
    };
    constexpr std::array<std::uint8_t, 17> missing {
      0x80,
      0x60,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x65,
      0x11,
      0x22,
      0x33,
      0x44,
      4,
      5,
      6,
      7,
      8,
    };
    constexpr std::array<std::uint8_t, 16> third {
      0x80,
      0xe0,
      0x00,
      0x01,
      0x00,
      0x00,
      0x00,
      0x66,
      0x11,
      0x22,
      0x33,
      0x44,
      9,
      10,
      11,
      12,
    };

    std::array<std::uint8_t, 32> parity_storage {};
    transport::flexfec_group group(parity_storage);
    PHOTON_REQUIRE(group.begin(7, primary_ssrc, repair_ssrc) == transport::flexfec_result::accepted);
    PHOTON_REQUIRE(group.add_plaintext_source(7, primary_ssrc, 65'535, first) == transport::flexfec_result::accepted);
    PHOTON_REQUIRE(group.add_plaintext_source(7, primary_ssrc, 0, missing) == transport::flexfec_result::accepted);
    PHOTON_REQUIRE(group.add_plaintext_source(7, primary_ssrc, 1, third) == transport::flexfec_result::accepted);

    transport::flexfec_parity_view parity;
    PHOTON_REQUIRE(group.seal_plaintext_repair(parity) == transport::flexfec_result::accepted);
    PHOTON_REQUIRE(parity.length_recovery == (3U ^ 5U ^ 4U));
    PHOTON_REQUIRE(parity.parity.size() == 5U);
    PHOTON_REQUIRE(parity.parity[0] == static_cast<std::uint8_t>(1U ^ 4U ^ 9U));

    std::array<std::uint8_t, 128> repair_packet {};
    const auto written = transport::write_flexfec_repair(
      parity,
      {.payload_type = 122, .sequence_number = 42, .timestamp = 900},
      repair_packet
    );
    PHOTON_REQUIRE(written);
    PHOTON_REQUIRE(group.mark_repair_plaintext_constructed() == transport::flexfec_result::accepted);
    const auto parsed = transport::parse_flexfec_repair(
      std::span<const std::uint8_t> {repair_packet}.first(written.bytes_written)
    );
    PHOTON_REQUIRE(parsed);
    PHOTON_REQUIRE(parsed.fec_header_bytes == 12U);
    PHOTON_REQUIRE(parsed.repair.source_mask == 0b111U);
    PHOTON_REQUIRE(parsed.repair.primary_ssrc == primary_ssrc);
    PHOTON_REQUIRE(parsed.repair.repair_ssrc == repair_ssrc);

    const std::array<std::span<const std::uint8_t>, 2> received {first, third};
    std::array<std::uint8_t, 64> recovered {};
    const auto decoded = transport::recover_flexfec_source(parsed.repair, 0, received, recovered);
    PHOTON_REQUIRE(decoded);
    PHOTON_REQUIRE(decoded.bytes_written == missing.size());
    PHOTON_REQUIRE(std::equal(missing.begin(), missing.end(), recovered.begin()));

    transport::flexfec_parity_view extended_mask {
      .frame_id = 8,
      .primary_ssrc = primary_ssrc,
      .repair_ssrc = repair_ssrc,
      .source_mask = (std::uint64_t {1} << 45U) | 1U,
    };
    auto extended_written = transport::write_flexfec_repair(
      extended_mask,
      {.payload_type = 122, .sequence_number = 43, .timestamp = 901},
      repair_packet
    );
    PHOTON_REQUIRE(extended_written);
    PHOTON_REQUIRE(transport::parse_flexfec_repair(std::span<const std::uint8_t> {repair_packet}.first(extended_written.bytes_written)).fec_header_bytes == 16U);

    extended_mask.source_mask = (std::uint64_t {1} << 63U) | 1U;
    extended_written = transport::write_flexfec_repair(
      extended_mask,
      {.payload_type = 122, .sequence_number = 44, .timestamp = 902},
      repair_packet
    );
    PHOTON_REQUIRE(extended_written);
    const auto largest_mask = transport::parse_flexfec_repair(
      std::span<const std::uint8_t> {repair_packet}.first(extended_written.bytes_written)
    );
    PHOTON_REQUIRE(largest_mask);
    PHOTON_REQUIRE(largest_mask.fec_header_bytes == 24U);
    PHOTON_REQUIRE(largest_mask.repair.source_mask == extended_mask.source_mask);
    return 0;
  }

  /** @brief Verify LSP dynamic payload-type boundaries at construction and recovered admission. */
  int test_dynamic_payload_type_boundaries() {
    transport::flexfec_parity_view parity {
      .frame_id = 1,
      .primary_ssrc = 10,
      .repair_ssrc = 20,
      .source_mask = 1,
    };
    std::array<std::uint8_t, 64> packet {};
    PHOTON_REQUIRE(
      transport::write_flexfec_repair(
        parity,
        {.payload_type = 95, .sequence_number = 1, .timestamp = 1},
        packet
      )
        .error == transport::flexfec_wire_error::invalid_payload_type
    );
    const auto minimum_dynamic = transport::write_flexfec_repair(
      parity,
      {.payload_type = 96, .sequence_number = 1, .timestamp = 1},
      packet
    );
    PHOTON_REQUIRE(minimum_dynamic);
    packet[1] = 95;
    const auto permissive_parse = transport::parse_flexfec_repair(
      std::span<const std::uint8_t> {packet}.first(minimum_dynamic.bytes_written)
    );
    PHOTON_REQUIRE(permissive_parse && permissive_parse.repair.repair_payload_type == 95U);
    PHOTON_REQUIRE(
      transport::write_flexfec_repair(
        parity,
        {.payload_type = 127, .sequence_number = 2, .timestamp = 2},
        packet
      )
    );
    PHOTON_REQUIRE(
      transport::write_flexfec_repair(
        parity,
        {.payload_type = 128, .sequence_number = 3, .timestamp = 3},
        packet
      )
        .error == transport::flexfec_wire_error::invalid_payload_type
    );

    const auto admit = [](const std::uint8_t payload_type) {
      return transport::recovered_packet_admission::authorize({
        .repair_authenticated_and_decrypted = true,
        .all_contributing_sources_authenticated_and_decrypted = true,
        .expected_source_ssrc = 10,
        .recovered_source_ssrc = 10,
        .expected_generation = 2,
        .recovered_generation = 2,
        .expected_payload_type = payload_type,
        .recovered_payload_type = payload_type,
        .recovered_packet_bytes = 100,
        .maximum_packet_bytes = 1'400,
        .now_microseconds = 1,
        .frame_deadline_microseconds = 2,
      });
    };
    PHOTON_REQUIRE(admit(95).error == transport::recovered_packet_error::invalid_payload_type);
    PHOTON_REQUIRE(admit(96));
    PHOTON_REQUIRE(admit(127));
    PHOTON_REQUIRE(admit(128).error == transport::recovered_packet_error::invalid_payload_type);
    return 0;
  }

  /** @brief Verify shared replay admission and bounded gap confirmation. */
  int test_replay_and_gap_tracking() {
    transport::source_replay_window replay;
    PHOTON_REQUIRE(replay.reset(17, 3));
    PHOTON_REQUIRE(replay.admit(17, 3, 65'535) == transport::source_replay_result::accepted_new_highest);
    PHOTON_REQUIRE(replay.admit(17, 3, 0) == transport::source_replay_result::accepted_new_highest);
    PHOTON_REQUIRE(replay.admit(17, 3, 65'534) == transport::source_replay_result::accepted_reordered);
    PHOTON_REQUIRE(replay.admit(17, 3, 65'534) == transport::source_replay_result::duplicate);
    PHOTON_REQUIRE(replay.admit(18, 3, 1) == transport::source_replay_result::wrong_source);
    for (std::uint16_t sequence = 1; sequence <= 128; ++sequence) {
      PHOTON_REQUIRE(replay.admit(17, 3, sequence) == transport::source_replay_result::accepted_new_highest);
    }
    PHOTON_REQUIRE(replay.admit(17, 3, 0) == transport::source_replay_result::too_old);

    transport::video_gap_tracker<4> gaps;
    PHOTON_REQUIRE(gaps.reset(17, 3, 100));
    PHOTON_REQUIRE(gaps.observe_source(17, 3, 100) == transport::video_gap_result::accepted);
    PHOTON_REQUIRE(gaps.record_gap(17, 3, 101, 50, 2'000, 1'000) == transport::video_gap_result::accepted);
    PHOTON_REQUIRE(gaps.observe_source(17, 3, 102) == transport::video_gap_result::accepted);
    std::array<transport::video_gap_nack_candidate, 4> candidates {};
    constexpr transport::rtx_deadline_estimate estimate {
      .reverse_feedback_microseconds = 10,
      .retransmission_serialization_microseconds = 10,
      .forward_delivery_microseconds = 10,
      .decode_safety_reserve_microseconds = 10,
    };
    PHOTON_REQUIRE(gaps.pending_nacks(1'050, estimate, candidates).count == 0U);
    PHOTON_REQUIRE(gaps.observe_source(17, 3, 103) == transport::video_gap_result::accepted);
    const auto due = gaps.pending_nacks(1'051, estimate, candidates);
    PHOTON_REQUIRE(due && due.count == 1U && candidates[0].sequence_number == 101U);
    constexpr std::array<std::uint16_t, 1> submitted {101};
    PHOTON_REQUIRE(gaps.mark_nacks_sent(submitted) == 1U);
    PHOTON_REQUIRE(gaps.pending_nacks(1'052, estimate, candidates).count == 0U);
    PHOTON_REQUIRE(gaps.observe_source(17, 3, 101) == transport::video_gap_result::resolved);
    PHOTON_REQUIRE(gaps.size() == 0U);

    PHOTON_REQUIRE(gaps.record_gap(17, 3, 104, 51, 2'000, 1'100) == transport::video_gap_result::accepted);
    PHOTON_REQUIRE(gaps.pending_nacks(1'199, estimate, candidates).count == 0U);
    PHOTON_REQUIRE(gaps.pending_nacks(1'200, estimate, candidates).count == 1U);
    PHOTON_REQUIRE(gaps.erase(104));

    PHOTON_REQUIRE(gaps.observe_source(17, 3, 200) == transport::video_gap_result::accepted);
    PHOTON_REQUIRE(gaps.record_gap(17, 3, 199, 52, 3'000, 1'300) == transport::video_gap_result::accepted);
    PHOTON_REQUIRE(gaps.pending_nacks(1'300, estimate, candidates).count == 0U);
    PHOTON_REQUIRE(gaps.observe_source(17, 3, 201) == transport::video_gap_result::accepted);
    PHOTON_REQUIRE(gaps.pending_nacks(1'300, estimate, candidates).count == 1U);
    PHOTON_REQUIRE(gaps.erase(199));
    return 0;
  }

  /** @brief Verify feedback parsers and bounded duplicate-free NACK expansion. */
  int test_rtcp_feedback_parsers() {
    transport::generic_nack<2> nack {
      .sender_ssrc = 1,
      .media_ssrc = 2,
      .pairs = {{{.packet_id = 65'535, .lost_packet_bitmask = 0b11}, {.packet_id = 0, .lost_packet_bitmask = 0b1}}},
      .pair_count = 2,
    };
    std::array<std::uint8_t, 64> packet {};
    const auto nack_written = transport::write_generic_nack(nack, packet);
    PHOTON_REQUIRE(nack_written);
    const auto nack_packet = transport::parse_rtcp_packet(
      std::span<const std::uint8_t> {packet}.first(nack_written.bytes_written)
    );
    transport::generic_nack<2> parsed_nack;
    PHOTON_REQUIRE(transport::parse_generic_nack(nack_packet, parsed_nack) == transport::rtcp_feedback_parse_error::none);
    std::array<std::uint16_t, 3> sequences {};
    const auto expanded = transport::expand_generic_nack(parsed_nack, sequences);
    PHOTON_REQUIRE(expanded && expanded.sequence_count == 3U);
    PHOTON_REQUIRE((sequences == std::array<std::uint16_t, 3> {65'535, 0, 1}));
    std::array<std::uint16_t, 2> short_sequences {};
    const auto short_expansion = transport::expand_generic_nack(parsed_nack, short_sequences);
    PHOTON_REQUIRE(short_expansion.error == transport::generic_nack_expansion_error::destination_too_small);
    PHOTON_REQUIRE(short_expansion.sequence_count == 0U && short_expansion.required == 3U);

    const auto pli_written = transport::write_picture_loss_indication({.sender_ssrc = 3, .media_ssrc = 4}, packet);
    PHOTON_REQUIRE(pli_written);
    transport::picture_loss_indication pli;
    PHOTON_REQUIRE(
      transport::parse_picture_loss_indication(
        transport::parse_rtcp_packet(std::span<const std::uint8_t> {packet}.first(pli_written.bytes_written)),
        pli
      ) == transport::rtcp_feedback_parse_error::none
    );
    PHOTON_REQUIRE(pli.sender_ssrc == 3U && pli.media_ssrc == 4U);

    transport::full_intra_request<2> fir {
      .sender_ssrc = 5,
      .entries = {{{.media_sender_ssrc = 6, .sequence_number = 7}, {.media_sender_ssrc = 8, .sequence_number = 9}}},
      .entry_count = 2,
    };
    const auto fir_written = transport::write_full_intra_request(fir, packet);
    PHOTON_REQUIRE(fir_written);
    transport::full_intra_request<2> parsed_fir;
    PHOTON_REQUIRE(
      transport::parse_full_intra_request(
        transport::parse_rtcp_packet(std::span<const std::uint8_t> {packet}.first(fir_written.bytes_written)),
        parsed_fir
      ) == transport::rtcp_feedback_parse_error::none
    );
    PHOTON_REQUIRE(parsed_fir.sender_ssrc == 5U && parsed_fir.entry_count == 2U);
    PHOTON_REQUIRE(parsed_fir.entries[1].media_sender_ssrc == 8U && parsed_fir.entries[1].sequence_number == 9U);
    return 0;
  }

  /** @brief Verify transactional O(1) RTX retention, wrap shadowing, and exactly-once release. */
  int test_rtx_reconstruction_tokens() {
    static std::array<transport::rtx_retained_packet, transport::required_rtx_retention_slots> slots {};
    transport::rtx_retention retention(slots);
    std::array<std::uint8_t, 4'096> release_counts {};
    const auto release = [&release_counts](const std::uint64_t token) {
      if (token < release_counts.size()) {
        ++release_counts[static_cast<std::size_t>(token)];
      }
    };
    const auto descriptor = [](const std::uint64_t frame_id) {
      return transport::rtx_frame_descriptor {
        .frame_id = frame_id,
        .frame_deadline_microseconds = 10'000,
        .now_microseconds = 1'000,
        .retention_origin_microseconds = 1'000,
        .measured_rtt_microseconds = 5'000,
        .source_ssrc = 7,
        .video_generation = 9,
      };
    };
    PHOTON_REQUIRE(retention.valid());

    const auto speculative = retention.begin_frame(descriptor(10), release);
    PHOTON_REQUIRE(speculative);
    PHOTON_REQUIRE(
      retention.retain(
        speculative.handle,
        {.reconstruction_token = 10, .source_sequence_number = 65'535, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(!retention.find_eligible(9, 7, 65'535, 1'100, {}).has_value());
    const auto aborted = retention.abort_frame(speculative.handle, release);
    PHOTON_REQUIRE(aborted && aborted.tokens_released == 1U && release_counts[10] == 1U);
    PHOTON_REQUIRE(
      retention.abort_frame(speculative.handle, release).status == transport::rtx_frame_result::stale_handle
    );
    PHOTON_REQUIRE(release_counts[10] == 1U);

    const auto wrapped = retention.begin_frame(descriptor(20), release);
    PHOTON_REQUIRE(wrapped);
    PHOTON_REQUIRE(
      retention.retain(
        wrapped.handle,
        {.reconstruction_token = 20, .source_sequence_number = 65'535, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(
      retention.retain(
        wrapped.handle,
        {.reconstruction_token = 21, .source_sequence_number = 0, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(retention.commit_frame(wrapped.handle, 1'000) == transport::rtx_frame_result::accepted);

    const auto newer = retention.begin_frame(descriptor(21), release);
    PHOTON_REQUIRE(newer);
    PHOTON_REQUIRE(
      retention.retain(
        newer.handle,
        {.reconstruction_token = 30, .source_sequence_number = 0, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(retention.commit_frame(newer.handle, 1'000) == transport::rtx_frame_result::accepted);
    const auto newest_zero = retention.find_eligible(9, 7, 0, 1'100, {});
    PHOTON_REQUIRE(newest_zero.has_value() && newest_zero->reconstruction_token == 30U);
    PHOTON_REQUIRE(retention.erase(9, 7, 0, release));
    PHOTON_REQUIRE(release_counts[30] == 1U);
    PHOTON_REQUIRE(!retention.find_eligible(9, 7, 0, 1'100, {}).has_value());
    const auto old_wrap = retention.find_eligible(9, 7, 65'535, 1'100, {});
    PHOTON_REQUIRE(old_wrap.has_value() && old_wrap->reconstruction_token == 20U);

    const auto failed_newer = retention.begin_frame(descriptor(22), release);
    PHOTON_REQUIRE(failed_newer && failed_newer.tokens_released == 2U);
    PHOTON_REQUIRE(release_counts[20] == 1U && release_counts[21] == 1U);
    PHOTON_REQUIRE(
      retention.retain(
        failed_newer.handle,
        {.reconstruction_token = 40, .source_sequence_number = 0, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(
      retention.retain(
        failed_newer.handle,
        {.reconstruction_token = 41, .source_sequence_number = 0, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::duplicate_packet
    );
    const auto failed_abort = retention.abort_frame(failed_newer.handle, release);
    PHOTON_REQUIRE(failed_abort && failed_abort.tokens_released == 1U && release_counts[40] == 1U);
    PHOTON_REQUIRE(release_counts[41] == 0U);
    PHOTON_REQUIRE(!retention.find_eligible(9, 7, 0, 1'100, {}).has_value());
    PHOTON_REQUIRE(retention.clear(release).frames_released == 1U);

    const auto live_before_expired_begin = retention.begin_frame(descriptor(70), release);
    PHOTON_REQUIRE(live_before_expired_begin);
    PHOTON_REQUIRE(
      retention.retain(
        live_before_expired_begin.handle,
        {.reconstruction_token = 70, .source_sequence_number = 70, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(
      retention.commit_frame(live_before_expired_begin.handle, 1'000) == transport::rtx_frame_result::accepted
    );
    auto already_expired = descriptor(71);
    already_expired.now_microseconds = 1'100;
    already_expired.measured_rtt_microseconds = 100;
    const auto rejected_expired = retention.begin_frame(already_expired, release);
    PHOTON_REQUIRE(rejected_expired.status == transport::rtx_frame_result::expired);
    PHOTON_REQUIRE(rejected_expired.tokens_released == 0U && release_counts[70] == 0U);
    PHOTON_REQUIRE(retention.retained_frame_count() == 1U && retention.retained_packets() == 1U);
    const auto still_live = retention.find_eligible(9, 7, 70, 1'100, {});
    PHOTON_REQUIRE(still_live.has_value() && still_live->reconstruction_token == 70U);
    PHOTON_REQUIRE(retention.clear(release).tokens_released == 1U && release_counts[70] == 1U);

    auto expiring_descriptor = descriptor(50);
    expiring_descriptor.measured_rtt_microseconds = 100;
    const auto expiring = retention.begin_frame(expiring_descriptor, release);
    PHOTON_REQUIRE(expiring);
    PHOTON_REQUIRE(
      retention.retain(
        expiring.handle,
        {.reconstruction_token = 50, .source_sequence_number = 50, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(retention.commit_frame(expiring.handle, 1'000) == transport::rtx_frame_result::accepted);
    auto after_expiry = descriptor(51);
    after_expiry.now_microseconds = 1'100;
    const auto boundary_pruned = retention.begin_frame(after_expiry, release);
    PHOTON_REQUIRE(boundary_pruned && boundary_pruned.tokens_released == 1U && release_counts[50] == 1U);
    PHOTON_REQUIRE(retention.abort_frame(boundary_pruned.handle, release).tokens_released == 0U);

    auto delayed_descriptor = descriptor(55);
    delayed_descriptor.measured_rtt_microseconds = 100;
    const auto delayed = retention.begin_frame(delayed_descriptor, release);
    PHOTON_REQUIRE(delayed);
    PHOTON_REQUIRE(
      retention.retain(
        delayed.handle,
        {.reconstruction_token = 55, .source_sequence_number = 55, .source_packet_bytes = 1'200}
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(retention.commit_frame(delayed.handle, 1'100) == transport::rtx_frame_result::expired);
    PHOTON_REQUIRE(retention.abort_frame(delayed.handle, release).tokens_released == 1U);
    PHOTON_REQUIRE(release_counts[55] == 1U);

    const auto capacity = retention.begin_frame(descriptor(60), release);
    PHOTON_REQUIRE(capacity);
    PHOTON_REQUIRE(
      retention.retain(
        capacity.handle,
        {
          .reconstruction_token = 60,
          .source_sequence_number = 60,
          .source_packet_bytes = transport::maximum_rtx_retained_bytes,
        }
      ) == transport::rtx_retention_result::retained
    );
    PHOTON_REQUIRE(
      retention.retain(
        capacity.handle,
        {.reconstruction_token = 61, .source_sequence_number = 61, .source_packet_bytes = 1}
      ) == transport::rtx_retention_result::byte_capacity_exceeded
    );
    PHOTON_REQUIRE(retention.abort_frame(capacity.handle, release).tokens_released == 1U);
    PHOTON_REQUIRE(release_counts[60] == 1U && release_counts[61] == 0U);

    const auto performance = retention.begin_frame(descriptor(100), release);
    PHOTON_REQUIRE(performance);
    retention.reset_operation_counts();
    constexpr auto retained_for_count = std::size_t {1'024};
    for (std::size_t index = 0; index < retained_for_count; ++index) {
      PHOTON_REQUIRE(
        retention.retain(
          performance.handle,
          {
            .reconstruction_token = 1'000U + index,
            .source_sequence_number = static_cast<std::uint16_t>(index),
            .source_packet_bytes = 1'200,
          }
        ) == transport::rtx_retention_result::retained
      );
    }
    auto counts = retention.operation_counts();
    PHOTON_REQUIRE(counts.retain_slot_probes == retained_for_count && counts.boundary_slot_visits == 0U);
    PHOTON_REQUIRE(retention.commit_frame(performance.handle, 1'000) == transport::rtx_frame_result::accepted);
    retention.reset_operation_counts();
    PHOTON_REQUIRE(retention.find_eligible(9, 7, 500, 1'100, {}).has_value());
    counts = retention.operation_counts();
    PHOTON_REQUIRE(counts.lookup_lane_probes == 2U && counts.lookup_slot_probes == 1U);
    PHOTON_REQUIRE(counts.boundary_slot_visits == 0U);
    const auto cleared = retention.clear(release);
    PHOTON_REQUIRE(cleared.tokens_released == retained_for_count);
    PHOTON_REQUIRE(retention.operation_counts().boundary_slot_visits == retained_for_count);
    PHOTON_REQUIRE(release_counts[1'500] == 1U);
    PHOTON_REQUIRE(retention.retained_bytes() == 0U && retention.retained_packets() == 0U);
    return 0;
  }
}  // namespace

/** @brief Run focused allocation-free repair primitive checks. */
int main() {
  if (const auto result = test_flexfec_wire_and_recovery(); result != 0) {
    return result;
  }
  if (const auto result = test_dynamic_payload_type_boundaries(); result != 0) {
    return result;
  }
  if (const auto result = test_replay_and_gap_tracking(); result != 0) {
    return result;
  }
  if (const auto result = test_rtcp_feedback_parsers(); result != 0) {
    return result;
  }
  return test_rtx_reconstruction_tokens();
}
