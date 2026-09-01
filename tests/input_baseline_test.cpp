/**
 * @file tests/input_baseline_test.cpp
 * @brief Focused frozen input bodies, deterministic baseline, and sequence-history tests.
 */

#include "lsp/input_plane/state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>

namespace {
  namespace input = lumen::lsp::input_plane;

  /** @brief Return failure after printing the source line of a rejected test condition. */
  int fail(const int line) {
    std::cerr << "input baseline assertion failed at line " << line << '\n';
    return 1;
  }

#define PHOTON_REQUIRE(condition) \
  do { \
    if (!(condition)) { \
      return fail(__LINE__); \
    } \
  } while (false)

  /** @brief Test digest verifier binding one expected complete body and marker digest. */
  class exact_verifier final: public input::baseline_digest_verifier {
  public:
    /** @brief Construct an exact test verifier. */
    explicit exact_verifier(const std::span<const std::uint8_t> expected) noexcept:
        expected_(expected) {
    }

    /** @brief Accept only exact expected bytes and the test marker digest. */
    [[nodiscard]] bool verify(
      const std::span<const std::uint8_t> bytes,
      const std::array<std::uint8_t, 32> &digest
    ) const noexcept override {
      return digest[0] == 0x5a && std::ranges::equal(bytes, expected_);
    }

  private:
    std::span<const std::uint8_t> expected_ {};  ///< Exact expected complete body.
  };

  /** @brief Test verifier accepting any bytes carrying the marker digest. */
  class permissive_verifier final: public input::baseline_digest_verifier {
  public:
    /** @brief Accept the test marker digest. */
    [[nodiscard]] bool verify(
      const std::span<const std::uint8_t>,
      const std::array<std::uint8_t, 32> &digest
    ) const noexcept override {
      return digest[0] == 0x5a;
    }
  };

  /** @brief Serialize all frozen state bodies and one neutral deterministic baseline. */
  int test_frozen_bodies_and_atomic_baseline() {
    std::array<std::uint8_t, input::core_state_payload_size> core_bytes {};
    const input::core_state_payload core {
      .neutral = true,
      .keyboard_instance_generation = 1,
      .physical_ordinal = 1,
      .text_barrier_edge_id = 5,
    };
    PHOTON_REQUIRE(input::serialize_core_state_payload(core, core_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(input::parse_core_state_payload(core_bytes).value == core);

    std::array<std::uint8_t, input::pointer_payload_size> pointer_bytes {};
    const input::pointer_payload pointer {
      .instance_generation = 2,
      .mode = input::pointer_flag::absolute,
      .represented_reports = 1,
      .physical_ordinal = 2,
      .cumulative_vertical_scroll = 10,
      .absolute_x = 0x4000'0000,
      .absolute_y = 0x8000'0000,
    };
    PHOTON_REQUIRE(input::serialize_pointer_payload(pointer, pointer_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(input::parse_absolute_pointer_payload(pointer_bytes).value == pointer);

    std::array<std::uint8_t, input::controller_state_payload_size> controller_bytes {};
    const input::controller_state_payload controller {
      .prefix = {
        .device = input::device_type::controller,
        .represented_reports = 1,
        .device_id = 5,
        .instance_generation = 3,
        .record_length = input::controller_state_payload_size,
        .physical_ordinal = 3,
      },
    };
    PHOTON_REQUIRE(input::serialize_controller_state_payload(controller, controller_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(input::parse_controller_state_payload(controller_bytes).value == controller);
    auto gapped_touchpad = controller;
    gapped_touchpad.touchpad[1] = {.contact_id = 1, .active = true, .x = 1, .y = 2, .pressure = 3};
    PHOTON_REQUIRE(
      input::serialize_controller_state_payload(gapped_touchpad, controller_bytes) ==
      input::wire_error::invalid_controller_state
    );
    auto unsorted_touchpad = controller;
    unsorted_touchpad.touchpad[0] = {.contact_id = 2, .active = true};
    unsorted_touchpad.touchpad[1] = {.contact_id = 1, .active = true};
    PHOTON_REQUIRE(
      input::serialize_controller_state_payload(unsorted_touchpad, controller_bytes) ==
      input::wire_error::invalid_controller_state
    );
    PHOTON_REQUIRE(input::serialize_controller_state_payload(controller, controller_bytes) == input::wire_error::none);

    std::array<std::uint8_t, input::controller_sensor_payload_size> gyro_bytes {};
    const input::controller_sensor_payload gyro {
      .prefix = {
        .device = input::device_type::controller,
        .represented_reports = 1,
        .device_id = 5,
        .instance_generation = 3,
        .record_length = input::controller_sensor_payload_size,
        .physical_ordinal = 4,
      },
      .sensor = input::sensor_type::gyroscope,
    };
    PHOTON_REQUIRE(input::serialize_controller_sensor_payload(gyro, gyro_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(input::parse_controller_sensor_payload(gyro_bytes).value == gyro);

    std::array<std::uint8_t, input::controller_sensor_payload_size> accelerometer_bytes {};
    auto accelerometer = gyro;
    accelerometer.prefix.physical_ordinal = 5;
    accelerometer.sensor = input::sensor_type::accelerometer;
    PHOTON_REQUIRE(
      input::serialize_controller_sensor_payload(accelerometer, accelerometer_bytes) == input::wire_error::none
    );

    std::array<std::uint8_t, input::touch_state_header_size> touch_bytes {};
    const input::touch_state_view touch {
      .prefix = {
        .device = input::device_type::touch,
        .represented_reports = 1,
        .device_id = 6,
        .instance_generation = 4,
        .record_length = input::touch_state_header_size,
        .physical_ordinal = 6,
      },
    };
    PHOTON_REQUIRE(input::serialize_touch_state_payload(touch, touch_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(input::parse_touch_state_payload(touch_bytes).contact_count == 0);
    std::array<input::touch_contact_payload, 2> contacts {{
      {.contact_id = 2, .active = true, .x = 20, .y = 30, .physical_ordinal = 6},
      {.contact_id = 1, .active = true, .x = 10, .y = 15, .physical_ordinal = 5},
    }};
    std::array<std::uint8_t, input::touch_state_header_size + 2 * input::touch_contact_size> contact_bytes {};
    auto active_touch = touch;
    active_touch.prefix.record_length = static_cast<std::uint16_t>(contact_bytes.size());
    active_touch.contacts = contacts;
    PHOTON_REQUIRE(input::serialize_touch_state_payload(active_touch, contact_bytes) == input::wire_error::invalid_touch_state);
    std::ranges::reverse(contacts);
    active_touch.contacts = contacts;
    PHOTON_REQUIRE(input::serialize_touch_state_payload(active_touch, contact_bytes) == input::wire_error::none);
    const auto parsed_contacts = input::parse_touch_state_payload(contact_bytes);
    PHOTON_REQUIRE(parsed_contacts && parsed_contacts.contact(0).value.contact_id == 1);
    PHOTON_REQUIRE(parsed_contacts.contact(1).value.contact_id == 2);

    std::array<std::uint8_t, input::pen_state_payload_size> pen_bytes {};
    const input::pen_state_payload pen {
      .prefix = {
        .device = input::device_type::pen,
        .represented_reports = 1,
        .device_id = 7,
        .instance_generation = 5,
        .record_length = input::pen_state_payload_size,
        .physical_ordinal = 7,
      },
    };
    PHOTON_REQUIRE(input::serialize_pen_state_payload(pen, pen_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(input::parse_pen_state_payload(pen_bytes).value == pen);
    auto nonneutral_pen = pen;
    nonneutral_pen.x = 1;
    std::array<std::uint8_t, input::pen_state_payload_size> nonneutral_pen_bytes {};
    PHOTON_REQUIRE(input::serialize_pen_state_payload(nonneutral_pen, nonneutral_pen_bytes) == input::wire_error::none);

    std::array<std::array<std::uint8_t, 4>, 4> lifecycle_bytes {};
    constexpr std::array<input::device_type, 4> lifecycle_types {
      input::device_type::pointer,
      input::device_type::controller,
      input::device_type::touch,
      input::device_type::pen,
    };
    for (std::size_t index = 0; index < lifecycle_types.size(); ++index) {
      PHOTON_REQUIRE(
        input::serialize_device_lifecycle_payload(
          {.device = lifecycle_types[index], .presence = input::device_presence::active},
          lifecycle_bytes[index]
        ) == input::wire_error::none
      );
    }

    const std::array<input::input_baseline_record_view, 11> records {{
      {
        .header = {.kind = input::baseline_record_kind::core, .payload_length = core_bytes.size(), .instance_generation = 1, .state_sequence = 1, .physical_ordinal = 1},
        .payload = core_bytes,
      },
      {
        .header = {.kind = input::baseline_record_kind::device_lifecycle, .subtype = static_cast<std::uint8_t>(input::device_type::pointer), .payload_length = 4, .device_id = 1, .instance_generation = 2, .state_sequence = 1, .physical_ordinal = 2},
        .payload = lifecycle_bytes[0],
      },
      {
        .header = {.kind = input::baseline_record_kind::device_lifecycle, .subtype = static_cast<std::uint8_t>(input::device_type::controller), .payload_length = 4, .device_id = 5, .instance_generation = 3, .state_sequence = 1, .physical_ordinal = 3},
        .payload = lifecycle_bytes[1],
      },
      {
        .header = {.kind = input::baseline_record_kind::device_lifecycle, .subtype = static_cast<std::uint8_t>(input::device_type::touch), .payload_length = 4, .device_id = 6, .instance_generation = 4, .state_sequence = 1, .physical_ordinal = 6},
        .payload = lifecycle_bytes[2],
      },
      {
        .header = {.kind = input::baseline_record_kind::device_lifecycle, .subtype = static_cast<std::uint8_t>(input::device_type::pen), .payload_length = 4, .device_id = 7, .instance_generation = 5, .state_sequence = 1, .physical_ordinal = 7},
        .payload = lifecycle_bytes[3],
      },
      {
        .header = {.kind = input::baseline_record_kind::pointer, .payload_length = pointer_bytes.size(), .device_id = 1, .instance_generation = 2, .state_sequence = 1, .physical_ordinal = 2},
        .payload = pointer_bytes,
      },
      {
        .header = {.kind = input::baseline_record_kind::controller, .payload_length = controller_bytes.size(), .device_id = 5, .instance_generation = 3, .state_sequence = 1, .physical_ordinal = 3},
        .payload = controller_bytes,
      },
      {
        .header = {.kind = input::baseline_record_kind::controller_sensor, .subtype = static_cast<std::uint8_t>(input::sensor_type::gyroscope), .payload_length = gyro_bytes.size(), .device_id = 5, .instance_generation = 3, .state_sequence = 1, .physical_ordinal = 4},
        .payload = gyro_bytes,
      },
      {
        .header = {.kind = input::baseline_record_kind::controller_sensor, .subtype = static_cast<std::uint8_t>(input::sensor_type::accelerometer), .payload_length = accelerometer_bytes.size(), .device_id = 5, .instance_generation = 3, .state_sequence = 1, .physical_ordinal = 5},
        .payload = accelerometer_bytes,
      },
      {
        .header = {.kind = input::baseline_record_kind::touch, .payload_length = touch_bytes.size(), .device_id = 6, .instance_generation = 4, .state_sequence = 1, .physical_ordinal = 6},
        .payload = touch_bytes,
      },
      {
        .header = {.kind = input::baseline_record_kind::pen, .payload_length = pen_bytes.size(), .device_id = 7, .instance_generation = 5, .state_sequence = 1, .physical_ordinal = 7},
        .payload = pen_bytes,
      },
    }};

    std::array<std::uint8_t, 2'048> encoded_storage {};
    const auto encoded = input::serialize_input_baseline(
      {.neutral = true, .edge_watermark = 5, .text_barrier_edge_id = 5, .records = records},
      encoded_storage
    );
    PHOTON_REQUIRE(encoded);
    const auto baseline = input::parse_input_baseline(std::span(encoded_storage).first(encoded.bytes_written));
    PHOTON_REQUIRE(baseline && baseline.neutral && baseline.record_count == records.size());
    PHOTON_REQUIRE(baseline.record(0).header.kind == input::baseline_record_kind::core);
    PHOTON_REQUIRE(baseline.record(10).header.kind == input::baseline_record_kind::pen);
    PHOTON_REQUIRE(baseline.record(11).payload.empty());
    std::array<std::uint8_t, input::input_baseline_record_header_size - 1> short_header {};
    PHOTON_REQUIRE(
      input::serialize_input_baseline_record_header(records[0].header, short_header) == input::wire_error::output_too_small
    );

    input::latest_state_table<16, 128> installed_state;
    PHOTON_REQUIRE(installed_state.install_baseline(baseline, 9) == input::baseline_install_result::installed);
    input::pointer_receiver installed_pointer;
    PHOTON_REQUIRE(installed_pointer.install_baseline_record(9, baseline, 5));
    auto pointer_update = pointer;
    pointer_update.physical_ordinal = 3;
    std::array<std::uint8_t, input::pointer_payload_size> pointer_update_bytes {};
    PHOTON_REQUIRE(
      input::serialize_pointer_payload(pointer_update, pointer_update_bytes) == input::wire_error::none
    );
    const input::common_header regressed_pointer_header {
      .input_generation = 9,
      .kind = input::packet_kind::pointer_motion,
      .state_sequence = 2,
      .sample_time_us = 20,
      .object_id = 1,
      .edge_watermark = 4,
    };
    std::array<std::uint8_t, input::common_header_size + input::pointer_payload_size> pointer_packet_bytes {};
    PHOTON_REQUIRE(
      input::serialize_packet(regressed_pointer_header, pointer_update_bytes, pointer_packet_bytes) ==
      input::wire_error::none
    );
    PHOTON_REQUIRE(
      installed_pointer.admit(input::parse_packet(pointer_packet_bytes, 9), 5) == input::pointer_receive_result::stale
    );
    input::pen_receiver installed_pen;
    PHOTON_REQUIRE(installed_pen.install_baseline_record(9, baseline, 10));
    auto nonneutral_records = records;
    nonneutral_records[10].payload = nonneutral_pen_bytes;
    PHOTON_REQUIRE(
      !input::serialize_input_baseline(
        {.neutral = true, .edge_watermark = 5, .text_barrier_edge_id = 5, .records = nonneutral_records},
        encoded_storage
      )
    );

    std::array<std::uint8_t, 4> removed_pointer {};
    PHOTON_REQUIRE(
      input::serialize_device_lifecycle_payload(
        {.device = input::device_type::pointer, .presence = input::device_presence::removed},
        removed_pointer
      ) == input::wire_error::none
    );
    std::array<input::input_baseline_record_view, 2> removed_records {{records[0], records[1]}};
    removed_records[1].payload = removed_pointer;
    std::array<std::uint8_t, 256> removed_storage {};
    PHOTON_REQUIRE(
      input::serialize_input_baseline(
        {.neutral = true, .edge_watermark = 5, .text_barrier_edge_id = 5, .records = removed_records},
        removed_storage
      )
    );
    removed_records[1].payload = lifecycle_bytes[0];
    PHOTON_REQUIRE(
      !input::serialize_input_baseline(
        {.neutral = true, .edge_watermark = 5, .text_barrier_edge_id = 5, .records = removed_records},
        removed_storage
      )
    );

    std::array<std::uint8_t, 32> digest {};
    digest[0] = 0x5a;
    const auto complete_bytes = std::span<const std::uint8_t>(encoded_storage).first(encoded.bytes_written);
    const auto split = encoded.bytes_written / 2U;
    const input::baseline_part first_part {
      .part_index = 0,
      .part_count = 2,
      .total_length = static_cast<std::uint32_t>(encoded.bytes_written),
      .part_offset = 0,
      .part_length = static_cast<std::uint32_t>(split),
      .digest = digest,
      .bytes = complete_bytes.first(split),
    };
    const input::baseline_part second_part {
      .part_index = 1,
      .part_count = 2,
      .total_length = static_cast<std::uint32_t>(encoded.bytes_written),
      .part_offset = static_cast<std::uint32_t>(split),
      .part_length = static_cast<std::uint32_t>(encoded.bytes_written - split),
      .digest = digest,
      .bytes = complete_bytes.subspan(split),
    };
    const input::common_header baseline_header {
      .input_generation = 9,
      .kind = input::packet_kind::baseline_part,
      .state_sequence = 1,
      .sample_time_us = 10,
      .object_id = 0x1234,
      .edge_watermark = 5,
    };
    input::baseline_receiver receiver;
    PHOTON_REQUIRE(receiver.add(baseline_header, second_part) == input::baseline_result::accepted);
    PHOTON_REQUIRE(receiver.add(baseline_header, first_part) == input::baseline_result::accepted);
    PHOTON_REQUIRE(receiver.complete() && receiver.committed_bytes().empty());
    const exact_verifier verifier {complete_bytes};
    PHOTON_REQUIRE(receiver.commit(verifier) == input::baseline_result::committed);
    PHOTON_REQUIRE(receiver.committed_baseline().record_count == records.size());
    auto conflicting_part = first_part;
    std::array<std::uint8_t, 2'048> conflicting_storage {};
    std::copy(first_part.bytes.begin(), first_part.bytes.end(), conflicting_storage.begin());
    conflicting_storage[0] ^= 1U;
    conflicting_part.bytes = std::span<const std::uint8_t>(conflicting_storage).first(split);
    PHOTON_REQUIRE(receiver.add(baseline_header, conflicting_part) == input::baseline_result::conflict);

    std::array<std::uint8_t, 1> invalid_body {0};
    input::baseline_receiver invalid_receiver;
    const input::baseline_part invalid_part {
      .part_index = 0,
      .part_count = 1,
      .total_length = 1,
      .part_offset = 0,
      .part_length = 1,
      .digest = digest,
      .bytes = invalid_body,
    };
    PHOTON_REQUIRE(invalid_receiver.add(baseline_header, invalid_part) == input::baseline_result::accepted);
    const permissive_verifier permissive;
    PHOTON_REQUIRE(invalid_receiver.commit(permissive) == input::baseline_result::invalid_body);
    return 0;
  }

  /** @brief Prove drained replaceable state stays stale and pen application respects edge barriers. */
  int test_latest_sequence_and_pen_receiver() {
    input::latest_sequence_table<2> strict_sequences;
    const input::supersession_key strict_pen_key {
      .input_generation = 9,
      .kind = input::packet_kind::device_state,
      .device = input::device_type::pen,
      .device_id = 7,
      .instance_generation = 5,
    };
    PHOTON_REQUIRE(strict_sequences.observe(strict_pen_key, 1, 10, 0) == input::latest_sequence_result::advanced);
    PHOTON_REQUIRE(strict_sequences.observe(strict_pen_key, 2, 10, 0) == input::latest_sequence_result::stale);

    input::core_state_payload core {
      .keyboard_instance_generation = 8,
      .physical_ordinal = 100,
      .text_barrier_edge_id = 4,
      .pointer_buttons = 1,
    };
    core.key_bitmap[1] = 1;
    std::array<std::uint8_t, input::core_state_payload_size> core_bytes {};
    PHOTON_REQUIRE(input::serialize_core_state_payload(core, core_bytes) == input::wire_error::none);
    input::common_header core_header {
      .input_generation = 9,
      .kind = input::packet_kind::core_state,
      .state_sequence = 2,
      .sample_time_us = 100,
      .edge_watermark = 5,
    };
    std::array<std::uint8_t, input::common_header_size + input::core_state_payload_size> packet_bytes {};
    PHOTON_REQUIRE(input::serialize_packet(core_header, core_bytes, packet_bytes) == input::wire_error::none);
    auto packet = input::parse_packet(packet_bytes, 9);
    input::latest_state_table<4, 128> latest;
    PHOTON_REQUIRE(latest.put(packet) == input::latest_state_result::stored);
    PHOTON_REQUIRE(latest.drain_ready(5, [](const auto &, const auto &, const auto) {
      return true;
    }) == 1);
    PHOTON_REQUIRE(latest.put(packet) == input::latest_state_result::stale);
    core_header.state_sequence = 3;
    PHOTON_REQUIRE(input::serialize_packet(core_header, core_bytes, packet_bytes) == input::wire_error::none);
    packet = input::parse_packet(packet_bytes, 9);
    PHOTON_REQUIRE(latest.put(packet) == input::latest_state_result::stored);
    input::supersession_key core_key;
    PHOTON_REQUIRE(input::make_supersession_key(packet, core_key) == input::supersession_key_result::success);
    const auto latest_snapshot = latest.latest_sequence(core_key);
    PHOTON_REQUIRE(
      latest_snapshot.found && latest_snapshot.state_sequence == 3 && latest_snapshot.physical_ordinal == 100 &&
      latest_snapshot.edge_watermark == 5
    );
    core_header.state_sequence = 4;
    core_header.edge_watermark = 4;
    PHOTON_REQUIRE(input::serialize_packet(core_header, core_bytes, packet_bytes) == input::wire_error::none);
    PHOTON_REQUIRE(latest.put(input::parse_packet(packet_bytes, 9)) == input::latest_state_result::stale);

    PHOTON_REQUIRE(
      latest.observe_lifecycle(input::device_type::pen, 7, 5, input::device_presence::active) ==
      input::lifecycle_generation_result::advanced
    );

    const input::pen_state_payload baseline_pen {
      .prefix = {
        .device = input::device_type::pen,
        .represented_reports = 1,
        .device_id = 7,
        .instance_generation = 5,
        .record_length = input::pen_state_payload_size,
        .physical_ordinal = 10,
      },
    };
    input::pen_receiver pen_receiver;
    PHOTON_REQUIRE(pen_receiver.reset(9, 7, baseline_pen, 1, 5));
    auto active_pen = baseline_pen;
    active_pen.prefix.physical_ordinal = 11;
    active_pen.in_range = true;
    active_pen.contact = true;
    active_pen.contact_id = 1;
    active_pen.x = 100;
    active_pen.y = 200;
    active_pen.pressure = 300;
    std::array<std::uint8_t, input::pen_state_payload_size> pen_bytes {};
    PHOTON_REQUIRE(input::serialize_pen_state_payload(active_pen, pen_bytes) == input::wire_error::none);
    const input::common_header pen_header {
      .input_generation = 9,
      .kind = input::packet_kind::device_state,
      .state_sequence = 2,
      .sample_time_us = 110,
      .object_id = input::make_device_object_id(input::device_type::pen, 7),
      .edge_watermark = 5,
    };
    std::array<std::uint8_t, input::common_header_size + input::pen_state_payload_size> pen_packet_bytes {};
    PHOTON_REQUIRE(input::serialize_packet(pen_header, pen_bytes, pen_packet_bytes) == input::wire_error::none);
    auto regressed_pen_header = pen_header;
    regressed_pen_header.edge_watermark = 4;
    PHOTON_REQUIRE(
      input::serialize_packet(regressed_pen_header, pen_bytes, pen_packet_bytes) == input::wire_error::none
    );
    PHOTON_REQUIRE(
      pen_receiver.admit(input::parse_packet(pen_packet_bytes, 9), 5) == input::pen_receive_result::stale
    );
    PHOTON_REQUIRE(input::serialize_packet(pen_header, pen_bytes, pen_packet_bytes) == input::wire_error::none);
    const auto valid_pen_packet = input::parse_packet(pen_packet_bytes, 9);
    PHOTON_REQUIRE(pen_receiver.admit(valid_pen_packet, 4) == input::pen_receive_result::waiting_for_edges);
    PHOTON_REQUIRE(
      pen_receiver.apply_ready(4, [](const auto &) {
        return true;
      }) == input::pen_receive_result::waiting_for_edges
    );
    bool applied = false;
    PHOTON_REQUIRE(
      pen_receiver.apply_ready(5, [&applied](const auto &value) {
        applied = value.contact && value.x == 100 && value.y == 200;
        return applied;
      }) == input::pen_receive_result::applied
    );
    PHOTON_REQUIRE(applied && pen_receiver.applied_state() == active_pen);
    PHOTON_REQUIRE(pen_receiver.admit(valid_pen_packet, 5) == input::pen_receive_result::stale);
    input::latest_state_table<4, 128> gated;
    PHOTON_REQUIRE(gated.put(valid_pen_packet) == input::latest_state_result::inactive_lifecycle);
    PHOTON_REQUIRE(
      gated.observe_lifecycle(input::device_type::pen, 7, 5, input::device_presence::active) ==
      input::lifecycle_generation_result::advanced
    );
    PHOTON_REQUIRE(gated.put(valid_pen_packet) == input::latest_state_result::stored);
    PHOTON_REQUIRE(
      gated.observe_lifecycle(input::device_type::pen, 7, 5, input::device_presence::removed) ==
      input::lifecycle_generation_result::advanced
    );
    PHOTON_REQUIRE(gated.put(valid_pen_packet) == input::latest_state_result::inactive_lifecycle);
    PHOTON_REQUIRE(
      latest.observe_lifecycle(input::device_type::pen, 7, 5, input::device_presence::removed) ==
      input::lifecycle_generation_result::advanced
    );
    PHOTON_REQUIRE(
      latest.observe_lifecycle(input::device_type::pen, 7, 6, input::device_presence::active) ==
      input::lifecycle_generation_result::advanced
    );
    input::device_lifecycle_table<1> hotplug;
    for (std::uint32_t generation = 1; generation <= 100; ++generation) {
      PHOTON_REQUIRE(
        hotplug.observe(input::device_type::controller, 1, generation, input::device_presence::active) ==
        input::lifecycle_generation_result::advanced
      );
      PHOTON_REQUIRE(
        hotplug.observe(input::device_type::controller, 1, generation, input::device_presence::removed) ==
        input::lifecycle_generation_result::advanced
      );
    }
    return 0;
  }
}  // namespace

/** @brief Run focused frozen input-state tests without an external dependency. */
int main() {
  if (const auto result = test_frozen_bodies_and_atomic_baseline(); result != 0) {
    return result;
  }
  return test_latest_sequence_and_pen_receiver();
}
