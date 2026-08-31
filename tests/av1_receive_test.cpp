/**
 * @file tests/av1_receive_test.cpp
 * @brief Focused allocation-free AOM AV1 RTP receive and round-trip tests.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <lsp/core/rtp.h>
#include <lsp/media/av1.h>
#include <span>
#include <vector>

namespace {
  namespace media = lumen::lsp::media;

  /**
   * @brief Report one focused test failure.
   *
   * @param condition Required condition.
   * @param line Source line naming the failed condition.
   * @return Zero on success or the failing source line.
   */
  int require(const bool condition, const int line) {
    if (!condition) {
      std::fprintf(stderr, "Photon AV1 receive test failed at line %d\n", line);
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

  /**
   * @brief Build deterministic extension-free AV1 packetization configuration.
   *
   * @return Valid LSP video packetization configuration.
   */
  media::video_packetization_config video_config() {
    return {
      .stream = {
        .payload_type = 96,
        .first_sequence_number = 65'530,
        .timestamp = 0x0102'0304,
        .ssrc = 0x1122'3344,
      },
      .path = {
        .family = media::ip_family::ipv4,
        .udp_payload_size = 1'200,
      },
    };
  }

  /**
   * @brief Return an extension-free packet's borrowed AV1 RTP payload.
   *
   * @param packet Complete plaintext RTP packet.
   * @return Borrowed bytes following the fixed RTP header.
   */
  std::span<const std::uint8_t> packet_payload(const media::rtp_packet &packet) {
    const auto header = lumen::lsp::parse_rtp_fixed_header(packet.bytes);
    if (!header || header.header.extension) {
      return {};
    }
    return std::span<const std::uint8_t> {packet.bytes}.subspan(header.bytes_consumed);
  }

  /**
   * @brief Append one minimal LEB128 value to test-owned expected bytes.
   *
   * @param output Destination vector.
   * @param value Value to encode.
   */
  void append_leb128(std::vector<std::uint8_t> &output, std::size_t value) {
    do {
      auto byte = static_cast<std::uint8_t>(value & 0x7fU);
      value >>= 7U;
      if (value != 0) {
        byte = static_cast<std::uint8_t>(byte | 0x80U);
      }
      output.push_back(byte);
    } while (value != 0);
  }

  /**
   * @brief Build expected canonical low-overhead bytes from normalized source OBUs.
   *
   * @param obus Complete normalized source OBU elements.
   * @return Concatenated OBUs with internal minimal size fields.
   */
  std::vector<std::uint8_t> canonical_bytes(
    const std::span<const std::span<const std::uint8_t>> obus
  ) {
    std::vector<std::uint8_t> output;
    for (const auto obu : obus) {
      const auto header_size = (obu[0] & 0x04U) != 0 ? 2U : 1U;
      output.push_back(static_cast<std::uint8_t>(obu[0] | 0x02U));
      if (header_size == 2) {
        output.push_back(obu[1]);
      }
      append_leb128(output, obu.size() - header_size);
      output.insert(output.end(), obu.begin() + static_cast<std::ptrdiff_t>(header_size), obu.end());
    }
    return output;
  }

  /** @brief Verify exact packetizer-to-depacketizer reconstruction across aggregation and fragmentation. */
  int test_exact_packetizer_round_trip() {
    const std::array<std::uint8_t, 2> sequence_header {0x08, 0xaa};
    const std::array<std::uint8_t, 3> metadata {0x28, 0xbb, 0xcc};
    std::vector<std::uint8_t> large_frame(2'600, 0x5a);
    large_frame[0] = 0x30;
    const std::array<std::uint8_t, 3> tile_group {0x20, 0xdd, 0xee};
    const std::array<std::uint8_t, 2> padding {0x78, 0xff};
    const std::array<std::span<const std::uint8_t>, 5> obus {
      sequence_header,
      metadata,
      large_frame,
      tile_group,
      padding,
    };
    const auto packetized = media::packetize_av1(obus, true, video_config());
    PHOTON_REQUIRE(packetized);
    PHOTON_REQUIRE(packetized.packets.size() >= 4);

    std::array<std::uint8_t, 4'096> storage {};
    media::av1_depacketizer receiver(storage);
    for (const auto &packet : packetized.packets) {
      const auto rtp = lumen::lsp::parse_rtp_fixed_header(packet.bytes);
      PHOTON_REQUIRE(rtp);
      const auto received = receiver.push_packet(
        rtp.header.sequence_number,
        rtp.header.timestamp,
        rtp.header.marker,
        packet_payload(packet)
      );
      PHOTON_REQUIRE(received);
    }
    PHOTON_REQUIRE(receiver.complete());
    const auto expected = canonical_bytes(obus);
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), expected));
    return 0;
  }

  /** @brief Verify `W=0`, `W=1...3`, mixed fragment boundaries, and ignored OBU types. */
  int test_aggregation_fragments_and_omission() {
    std::array<std::uint8_t, 128> storage {};
    media::av1_depacketizer receiver(storage);

    constexpr std::array<std::uint8_t, 12> explicit_lengths {
      0x00,
      0x02,
      0x08,
      0x11,
      0x02,
      0x18,
      0x22,
      0x02,
      0x20,
      0x33,
      0x01,
      0x78,
    };
    auto result = receiver.push_packet(1, 9'000, true, explicit_lengths);
    PHOTON_REQUIRE(result && result.temporal_unit_complete);
    constexpr std::array<std::uint8_t, 11> explicit_expected {
      0x0a,
      0x01,
      0x11,
      0x1a,
      0x01,
      0x22,
      0x22,
      0x01,
      0x33,
      0x7a,
      0x00,
    };
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), explicit_expected));

    receiver.reset();
    constexpr std::array<std::uint8_t, 3> first_fragment {0x50, 0x18, 0xaa};
    constexpr std::array<std::uint8_t, 5> completed_and_full {0xa0, 0x01, 0xbb, 0x30, 0xcc};
    PHOTON_REQUIRE(receiver.push_packet(20, 9'000, false, first_fragment));
    PHOTON_REQUIRE(receiver.fragment_pending());
    result = receiver.push_packet(21, 9'000, true, completed_and_full);
    PHOTON_REQUIRE(result && result.temporal_unit_complete);
    constexpr std::array<std::uint8_t, 7> fragment_expected {
      0x1a,
      0x02,
      0xaa,
      0xbb,
      0x32,
      0x01,
      0xcc,
    };
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), fragment_expected));

    receiver.reset();
    constexpr std::array<std::uint8_t, 7> ignored_and_frame {
      0x30,
      0x01,
      0x10,
      0x01,
      0x40,
      0x18,
      0xdd,
    };
    result = receiver.push_packet(30, 9'000, true, ignored_and_frame);
    PHOTON_REQUIRE(result && result.bytes_written == 3);
    constexpr std::array<std::uint8_t, 3> ignored_expected {0x1a, 0x01, 0xdd};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), ignored_expected));

    receiver.reset();
    constexpr std::array<std::uint8_t, 2> ignored_fragment_start {0x50, 0x14};
    constexpr std::array<std::uint8_t, 3> ignored_fragment_end {0x90, 0x08, 0xee};
    PHOTON_REQUIRE(receiver.push_packet(40, 9'000, false, ignored_fragment_start));
    result = receiver.push_packet(41, 9'000, true, ignored_fragment_end);
    PHOTON_REQUIRE(result && result.temporal_unit_complete && result.bytes_written == 0);
    PHOTON_REQUIRE(receiver.bytes().empty());
    return 0;
  }

  /** @brief Verify relaxed wire LEB128 and canonicalization of internally sized OBUs. */
  int test_leb_and_internal_size_canonicalization() {
    std::array<std::uint8_t, 128> storage {};
    media::av1_depacketizer receiver(storage);

    constexpr std::array<std::uint8_t, 10> eight_byte_element_length {
      0x00,
      0x81,
      0x80,
      0x80,
      0x80,
      0x80,
      0x80,
      0x80,
      0x00,
      0x18,
    };
    auto result = receiver.push_packet(1, 9'000, true, eight_byte_element_length);
    PHOTON_REQUIRE(result && result.packet_consumed && result.temporal_unit_complete);
    constexpr std::array<std::uint8_t, 2> zero_payload_obu {0x1a, 0x00};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), zero_payload_obu));

    receiver.reset();
    constexpr std::array<std::uint8_t, 6> internally_sized {
      0x10,
      0x1a,
      0x82,
      0x00,
      0xaa,
      0xbb,
    };
    result = receiver.push_packet(2, 9'000, true, internally_sized);
    PHOTON_REQUIRE(result);
    constexpr std::array<std::uint8_t, 4> canonical {0x1a, 0x02, 0xaa, 0xbb};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), canonical));

    receiver.reset();
    constexpr std::array<std::uint8_t, 3> sized_fragment_start {0x50, 0x1a, 0x82};
    constexpr std::array<std::uint8_t, 4> sized_fragment_end {0x90, 0x00, 0xaa, 0xbb};
    PHOTON_REQUIRE(receiver.push_packet(3, 9'000, false, sized_fragment_start));
    result = receiver.push_packet(4, 9'000, true, sized_fragment_end);
    PHOTON_REQUIRE(result && result.temporal_unit_complete);
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), canonical));

    std::array<std::uint8_t, 4> exact_storage {};
    media::av1_depacketizer exact_receiver(exact_storage);
    PHOTON_REQUIRE(exact_receiver.push_packet(5, 9'000, false, sized_fragment_start));
    result = exact_receiver.push_packet(6, 9'000, true, sized_fragment_end);
    PHOTON_REQUIRE(result && result.temporal_unit_complete);
    PHOTON_REQUIRE(std::ranges::equal(exact_receiver.bytes(), canonical));
    return 0;
  }

  /** @brief Verify all extended packet elements and continuations use one layer identifier. */
  int test_packet_local_layer_ids() {
    std::array<std::uint8_t, 128> storage {};
    media::av1_depacketizer receiver(storage);

    constexpr std::array<std::uint8_t, 8> matching {
      0x20,
      0x03,
      0x1c,
      0x08,
      0xaa,
      0x24,
      0x08,
      0xbb,
    };
    auto result = receiver.push_packet(1, 9'000, true, matching);
    PHOTON_REQUIRE(result);
    constexpr std::array<std::uint8_t, 8> matching_expected {
      0x1e,
      0x08,
      0x01,
      0xaa,
      0x26,
      0x08,
      0x01,
      0xbb,
    };
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), matching_expected));

    receiver.reset();
    constexpr std::array<std::uint8_t, 8> mismatched {
      0x20,
      0x03,
      0x1c,
      0x08,
      0xaa,
      0x24,
      0x10,
      0xbb,
    };
    PHOTON_REQUIRE(
      receiver.push_packet(2, 9'000, true, mismatched).error ==
      media::av1_depacketization_error::inconsistent_layer_ids
    );

    receiver.reset();
    constexpr std::array<std::uint8_t, 4> fragment_start {0x50, 0x1c, 0x08, 0xaa};
    constexpr std::array<std::uint8_t, 6> continuation_mismatch {
      0xa0,
      0x01,
      0xbb,
      0x24,
      0x10,
      0xcc,
    };
    PHOTON_REQUIRE(receiver.push_packet(3, 9'000, false, fragment_start));
    PHOTON_REQUIRE(
      receiver.push_packet(4, 9'000, true, continuation_mismatch).error ==
      media::av1_depacketization_error::inconsistent_layer_ids
    );

    receiver.reset();
    constexpr std::array<std::uint8_t, 6> split_extension_start {
      0x60,
      0x03,
      0x1c,
      0x08,
      0xaa,
      0x24,
    };
    constexpr std::array<std::uint8_t, 3> split_extension_mismatch {0x90, 0x10, 0xbb};
    PHOTON_REQUIRE(receiver.push_packet(5, 9'000, false, split_extension_start));
    PHOTON_REQUIRE(
      receiver.push_packet(6, 9'000, true, split_extension_mismatch).error ==
      media::av1_depacketization_error::inconsistent_layer_ids
    );
    return 0;
  }

  /** @brief Verify sender filtering preserves carried OBU order and reports an empty filtered unit. */
  int test_sender_ignored_obu_filtering() {
    constexpr std::array<std::uint8_t, 1> temporal_delimiter {0x10};
    constexpr std::array<std::uint8_t, 2> sequence_header {0x08, 0xaa};
    constexpr std::array<std::uint8_t, 1> tile_list {0x40};
    constexpr std::array<std::uint8_t, 2> frame {0x18, 0xbb};
    const std::array<std::span<const std::uint8_t>, 4> source {
      temporal_delimiter,
      sequence_header,
      tile_list,
      frame,
    };
    const auto packetized = media::packetize_av1(source, true, video_config());
    PHOTON_REQUIRE(packetized && packetized.packets.size() == 1);

    std::array<std::uint8_t, 64> storage {};
    media::av1_depacketizer receiver(storage);
    const auto rtp = lumen::lsp::parse_rtp_fixed_header(packetized.packets.front().bytes);
    PHOTON_REQUIRE(rtp);
    PHOTON_REQUIRE(receiver.push_packet(
      rtp.header.sequence_number,
      rtp.header.timestamp,
      rtp.header.marker,
      packet_payload(packetized.packets.front())
    ));
    const std::array<std::span<const std::uint8_t>, 2> carried {sequence_header, frame};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), canonical_bytes(carried)));

    const std::array<std::span<const std::uint8_t>, 2> ignored_only {
      temporal_delimiter,
      tile_list,
    };
    const auto empty = media::packetize_av1(ignored_only, false, video_config());
    PHOTON_REQUIRE(empty.error == media::packetization_error::empty_access_unit);
    PHOTON_REQUIRE(empty.packets.empty());
    return 0;
  }

  /** @brief Verify malformed payloads fail closed and never expose partial decoder bytes. */
  int test_malformed_payloads_and_capacity() {
    const auto fails_as = [](
                            const std::span<const std::uint8_t> payload,
                            const media::av1_depacketization_error expected
                          ) {
      std::array<std::uint8_t, 32> storage {};
      media::av1_depacketizer receiver(storage);
      const auto result = receiver.push_packet(1, 9'000, true, payload);
      return result.error == expected && receiver.bytes().empty() &&
             receiver.push_packet(2, 9'000, true, payload).error == media::av1_depacketization_error::failed_state;
    };

    PHOTON_REQUIRE(fails_as({}, media::av1_depacketization_error::payload_too_short));
    constexpr std::array<std::uint8_t, 2> reserved {0x01, 0x18};
    PHOTON_REQUIRE(fails_as(reserved, media::av1_depacketization_error::reserved_header_bits));
    constexpr std::array<std::uint8_t, 2> empty_element {0x00, 0x00};
    PHOTON_REQUIRE(fails_as(empty_element, media::av1_depacketization_error::empty_obu_element));
    constexpr std::array<std::uint8_t, 3> truncated {0x00, 0x05, 0x18};
    PHOTON_REQUIRE(fails_as(truncated, media::av1_depacketization_error::truncated_obu_element));
    constexpr std::array<std::uint8_t, 3> missing_w_element {0x20, 0x01, 0x18};
    PHOTON_REQUIRE(fails_as(missing_w_element, media::av1_depacketization_error::empty_obu_element));
    constexpr std::array<std::uint8_t, 4> size_mismatch {0x10, 0x1a, 0x02, 0xaa};
    PHOTON_REQUIRE(fails_as(size_mismatch, media::av1_depacketization_error::invalid_obu_size));
    constexpr std::array<std::uint8_t, 6> length_above_uint32 {
      0x00,
      0x80,
      0x80,
      0x80,
      0x80,
      0x10,
    };
    PHOTON_REQUIRE(fails_as(length_above_uint32, media::av1_depacketization_error::malformed_length));
    constexpr std::array<std::uint8_t, 2> forbidden_type {0x10, 0x48};
    PHOTON_REQUIRE(fails_as(forbidden_type, media::av1_depacketization_error::invalid_obu_header));
    constexpr std::array<std::uint8_t, 2> unexpected_continuation {0x90, 0xaa};
    PHOTON_REQUIRE(fails_as(unexpected_continuation, media::av1_depacketization_error::unexpected_continuation));
    constexpr std::array<std::uint8_t, 2> marker_fragment {0x50, 0x18};
    PHOTON_REQUIRE(fails_as(marker_fragment, media::av1_depacketization_error::marker_on_incomplete_obu));

    std::array<std::uint8_t, 2> tiny_storage {};
    media::av1_depacketizer tiny(tiny_storage);
    constexpr std::array<std::uint8_t, 3> complete_obu {0x10, 0x18, 0xaa};
    PHOTON_REQUIRE(
      tiny.push_packet(1, 9'000, true, complete_obu).error == media::av1_depacketization_error::output_too_small
    );
    PHOTON_REQUIRE(tiny.bytes().empty());
    return 0;
  }

  /** @brief Verify loss/reorder boundaries, continuation requirements, sequence wrap, and `N`. */
  int test_packet_order_and_boundaries() {
    constexpr std::array<std::uint8_t, 2> fragment_start {0x50, 0x18};
    constexpr std::array<std::uint8_t, 2> fragment_end {0x90, 0xaa};
    constexpr std::array<std::uint8_t, 2> complete {0x10, 0x18};

    std::array<std::uint8_t, 32> storage {};
    media::av1_depacketizer receiver(storage);
    PHOTON_REQUIRE(receiver.push_packet(100, 9'000, false, fragment_start));
    PHOTON_REQUIRE(
      receiver.push_packet(102, 9'000, true, fragment_end).error == media::av1_depacketization_error::sequence_discontinuity
    );
    PHOTON_REQUIRE(receiver.bytes().empty());

    receiver.reset();
    PHOTON_REQUIRE(receiver.push_packet(200, 9'000, false, fragment_start));
    PHOTON_REQUIRE(
      receiver.push_packet(201, 9'000, true, complete).error == media::av1_depacketization_error::missing_continuation
    );

    receiver.reset();
    PHOTON_REQUIRE(receiver.push_packet(65'535, 9'000, false, fragment_start));
    const auto wrapped = receiver.push_packet(0, 9'000, true, fragment_end);
    PHOTON_REQUIRE(wrapped && wrapped.temporal_unit_complete);
    constexpr std::array<std::uint8_t, 3> wrapped_expected {0x1a, 0x01, 0xaa};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), wrapped_expected));

    receiver.reset();
    constexpr std::array<std::uint8_t, 2> first_packet {0x10, 0x18};
    constexpr std::array<std::uint8_t, 2> late_new_sequence {0x18, 0x18};
    PHOTON_REQUIRE(receiver.push_packet(7, 9'000, false, first_packet));
    PHOTON_REQUIRE(
      receiver.push_packet(8, 9'000, true, late_new_sequence).error ==
      media::av1_depacketization_error::invalid_new_sequence_boundary
    );

    receiver.reset();
    constexpr std::array<std::uint8_t, 2> first_new_sequence {0x18, 0x18};
    const auto new_sequence = receiver.push_packet(9, 9'000, true, first_new_sequence);
    PHOTON_REQUIRE(new_sequence && new_sequence.starts_coded_video_sequence);
    PHOTON_REQUIRE(
      receiver.push_packet(10, 9'000, true, complete).error == media::av1_depacketization_error::packet_after_temporal_unit
    );

    receiver.reset();
    constexpr std::array<std::uint8_t, 3> first_without_marker {0x10, 0x18, 0xaa};
    constexpr std::array<std::uint8_t, 3> next_timestamp {0x10, 0x18, 0xbb};
    const auto first = receiver.push_packet(11, 9'000, false, first_without_marker);
    PHOTON_REQUIRE(first && first.packet_consumed && !first.temporal_unit_complete);
    const auto boundary = receiver.push_packet(12, 12'000, true, next_timestamp);
    PHOTON_REQUIRE(boundary && !boundary.packet_consumed && boundary.temporal_unit_complete);
    constexpr std::array<std::uint8_t, 3> first_expected {0x1a, 0x01, 0xaa};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), first_expected));
    receiver.reset();
    const auto retried = receiver.push_packet(12, 12'000, true, next_timestamp);
    PHOTON_REQUIRE(retried && retried.packet_consumed && retried.temporal_unit_complete);
    constexpr std::array<std::uint8_t, 3> next_expected {0x1a, 0x01, 0xbb};
    PHOTON_REQUIRE(std::ranges::equal(receiver.bytes(), next_expected));

    receiver.reset();
    PHOTON_REQUIRE(receiver.push_packet(20, 9'000, false, first_without_marker));
    PHOTON_REQUIRE(
      receiver.push_packet(22, 12'000, true, next_timestamp).error ==
      media::av1_depacketization_error::sequence_discontinuity
    );
    PHOTON_REQUIRE(receiver.bytes().empty());

    receiver.reset();
    PHOTON_REQUIRE(receiver.push_packet(20, 9'000, false, first_without_marker));
    PHOTON_REQUIRE(
      receiver.push_packet(20, 12'000, true, next_timestamp).error ==
      media::av1_depacketization_error::sequence_discontinuity
    );

    receiver.reset();
    PHOTON_REQUIRE(receiver.push_packet(13, 9'000, false, fragment_start));
    PHOTON_REQUIRE(
      receiver.push_packet(14, 12'000, true, next_timestamp).error ==
      media::av1_depacketization_error::missing_continuation
    );
    return 0;
  }
}  // namespace

/** @brief Run focused Photon AV1 receive tests. */
int main() {
  if (const auto status = test_exact_packetizer_round_trip(); status != 0) {
    return status;
  }
  if (const auto status = test_aggregation_fragments_and_omission(); status != 0) {
    return status;
  }
  if (const auto status = test_leb_and_internal_size_canonicalization(); status != 0) {
    return status;
  }
  if (const auto status = test_packet_local_layer_ids(); status != 0) {
    return status;
  }
  if (const auto status = test_sender_ignored_obu_filtering(); status != 0) {
    return status;
  }
  if (const auto status = test_malformed_payloads_and_capacity(); status != 0) {
    return status;
  }
  return test_packet_order_and_boundaries();
}
