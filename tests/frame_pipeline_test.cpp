#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <lsp/media/frame_pipeline.h>
#include <span>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<lumen::lsp::media::token_video_frame_reassembler>);
static_assert(!std::is_copy_assignable_v<lumen::lsp::media::token_video_frame_reassembler>);
static_assert(!std::is_move_constructible_v<lumen::lsp::media::token_video_frame_reassembler>);
static_assert(!std::is_move_assignable_v<lumen::lsp::media::token_video_frame_reassembler>);

namespace {
  namespace media = lumen::lsp::media;

  /** @brief Report one focused frame-pipeline test failure. */
  int require(const bool condition, const int line) {
    if (!condition) {
      std::fprintf(stderr, "Photon frame-pipeline test failed at line %d\n", line);
      return line;
    }
    return 0;
  }

  /** @brief Return from the active focused test when a condition fails. */
#define PHOTON_REQUIRE(condition) \
  do { \
    if (const auto failed = require(static_cast<bool>(condition), __LINE__); failed != 0) { \
      return failed; \
    } \
  } while (false)

  /** @brief Verify caller-slab retention, ordering, coalescing, and exact completion release. */
  int test_zero_copy_completion() {
    std::array<media::token_frame_slice, 8> slice_storage {};
    media::token_video_frame_reassembler reassembler(slice_storage, 64, 4);
    std::array<std::uint8_t, 32> retain_counts {};
    std::array<std::uint8_t, 32> release_counts {};
    const auto retain = [&retain_counts](const std::uint64_t token) {
      ++retain_counts[static_cast<std::size_t>(token)];
      return true;
    };
    const auto release = [&release_counts](const std::uint64_t token) {
      ++release_counts[static_cast<std::size_t>(token)];
    };
    constexpr media::frame_key key {.ssrc = 7, .extended_timestamp = 900};
    constexpr std::array<std::uint8_t, 2> middle {3, 4};
    constexpr std::array<std::uint8_t, 2> start {1, 2};
    constexpr std::array<std::uint8_t, 2> end {5, 6};

    PHOTON_REQUIRE(reassembler.ready());
    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = key, .frame_id = 44, .extended_sequence = 11, .deadline_us = 1'000, .slice_token = 2, .payload = middle},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = key, .frame_id = 44, .extended_sequence = 10, .deadline_us = 1'000, .slice_token = 1, .starts_frame = true, .payload = start},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    const auto duplicate = reassembler.add(
      {.key = key, .frame_id = 44, .extended_sequence = 11, .deadline_us = 1'000, .slice_token = 9, .payload = middle},
      100,
      retain,
      release
    );
    PHOTON_REQUIRE(duplicate.status == media::frame_reassembly_status::duplicate);
    PHOTON_REQUIRE(retain_counts[9] == 0U && release_counts[9] == 0U);

    const auto completed = reassembler.add(
      {.key = key, .frame_id = 44, .extended_sequence = 12, .deadline_us = 1'000, .slice_token = 3, .ends_frame = true, .marker = true, .payload = end},
      100,
      retain,
      release
    );
    PHOTON_REQUIRE(completed.status == media::frame_reassembly_status::completed && completed.frame);
    PHOTON_REQUIRE(completed.frame.slices.size() == 3U && completed.frame.total_size == 6U);
    PHOTON_REQUIRE(completed.frame.slices[0].extended_sequence == 10U);
    PHOTON_REQUIRE(completed.frame.slices[1].extended_sequence == 11U);
    PHOTON_REQUIRE(completed.frame.slices[2].extended_sequence == 12U);
    PHOTON_REQUIRE(completed.frame.slices[0].bytes.data() == start.data());
    PHOTON_REQUIRE(completed.frame.slices[1].bytes.data() == middle.data());
    PHOTON_REQUIRE(completed.frame.slices[2].bytes.data() == end.data());
    PHOTON_REQUIRE(retain_counts[1] == 1U && retain_counts[2] == 1U && retain_counts[3] == 1U);

    std::array<std::uint8_t, 6> coalesced {};
    const auto copied = reassembler.coalesce(completed.frame, coalesced);
    PHOTON_REQUIRE(copied && copied.bytes_written == coalesced.size());
    PHOTON_REQUIRE((coalesced == std::array<std::uint8_t, 6> {1, 2, 3, 4, 5, 6}));
    PHOTON_REQUIRE(reassembler.release(completed.frame, release));
    PHOTON_REQUIRE(release_counts[1] == 1U && release_counts[2] == 1U && release_counts[3] == 1U);
    PHOTON_REQUIRE(!reassembler.release(completed.frame, release));
    PHOTON_REQUIRE(
      reassembler.coalesce(completed.frame, coalesced).error == media::token_frame_coalesce_error::stale_frame
    );
    return 0;
  }

  /** @brief Verify expiry, conflict, eviction, failed retain, and teardown release lifetimes. */
  int test_zero_copy_failure_lifetimes() {
    std::array<media::token_frame_slice, 8> slice_storage {};
    media::token_video_frame_reassembler reassembler(slice_storage, 64, 4);
    std::array<std::uint8_t, 32> retain_counts {};
    std::array<std::uint8_t, 32> release_counts {};
    std::array<std::uint64_t, 32> retain_events {};
    std::array<std::uint64_t, 32> release_events {};
    std::uint64_t event = 0;
    const auto retain = [&retain_counts, &retain_events, &event](const std::uint64_t token) {
      ++retain_counts[static_cast<std::size_t>(token)];
      retain_events[static_cast<std::size_t>(token)] = ++event;
      return token != 10U;
    };
    const auto release = [&release_counts, &release_events, &event](const std::uint64_t token) {
      ++release_counts[static_cast<std::size_t>(token)];
      release_events[static_cast<std::size_t>(token)] = ++event;
    };
    constexpr std::array<std::uint8_t, 1> byte {1};
    constexpr std::array<std::uint8_t, 1> other {2};

    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 1, .extended_timestamp = 1}, .extended_sequence = 1, .deadline_us = 200, .slice_token = 4, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    PHOTON_REQUIRE(reassembler.expire(200, release) == 1U && release_counts[4] == 1U);

    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 2, .extended_timestamp = 2}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 8, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 2, .extended_timestamp = 2}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 9, .payload = other},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::malformed
    );
    PHOTON_REQUIRE(release_counts[8] == 0U && retain_counts[9] == 0U && release_counts[9] == 0U);
    PHOTON_REQUIRE(reassembler.incomplete_frames() == 1U);
    PHOTON_REQUIRE(reassembler.clear(release) == 1U && release_counts[8] == 1U);

    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 3, .extended_timestamp = 3}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 5, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 4, .extended_timestamp = 4}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 6, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    const auto evicted = reassembler.add(
      {.key = {.ssrc = 5, .extended_timestamp = 5}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 7, .payload = byte},
      100,
      retain,
      release
    );
    PHOTON_REQUIRE(evicted.status == media::frame_reassembly_status::evicted_and_stored);
    PHOTON_REQUIRE(evicted.evicted.has_value() && evicted.evicted->ssrc == 3U && release_counts[5] == 1U);
    PHOTON_REQUIRE(retain_events[7] < release_events[5]);
    PHOTON_REQUIRE(reassembler.clear(release) == 2U);
    PHOTON_REQUIRE(release_counts[6] == 1U && release_counts[7] == 1U);

    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 6, .extended_timestamp = 6}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 11, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 7, .extended_timestamp = 7}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 12, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::stored
    );
    PHOTON_REQUIRE(
      reassembler.add(
                   {.key = {.ssrc = 8, .extended_timestamp = 8}, .extended_sequence = 1, .deadline_us = 500, .slice_token = 10, .payload = byte},
                   100,
                   retain,
                   release
      )
        .status == media::frame_reassembly_status::resource_exhausted
    );
    PHOTON_REQUIRE(retain_counts[10] == 1U && release_counts[10] == 0U);
    PHOTON_REQUIRE(release_counts[11] == 0U && release_counts[12] == 0U);
    PHOTON_REQUIRE(reassembler.incomplete_frames() == 2U);
    PHOTON_REQUIRE(reassembler.clear(release) == 2U);
    PHOTON_REQUIRE(release_counts[11] == 1U && release_counts[12] == 1U);
    return 0;
  }
}  // namespace

/** @brief Run focused caller-slab frame-pipeline lifetime checks. */
int main() {
  if (const auto result = test_zero_copy_completion(); result != 0) {
    return result;
  }
  return test_zero_copy_failure_lifetimes();
}
