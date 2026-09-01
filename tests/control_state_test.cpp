/**
 * @file tests/control_state_test.cpp
 * @brief Focused LSPC reliability and retained-response tests.
 */

#include "lsp/core/control.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {
  namespace lsp = lumen::lsp;

  /** @brief Return failure after printing the source line of a rejected test condition. */
  int fail(const int line) {
    std::cerr << "control state assertion failed at line " << line << '\n';
    return 1;
  }

#define PHOTON_REQUIRE(condition) \
  do { \
    if (!(condition)) { \
      return fail(__LINE__); \
    } \
  } while (false)

  /** @brief Prove authenticated ACK timing updates RTO only for Karn-eligible frames. */
  int test_authenticated_ack_rtt() {
    lsp::control_rto local {lsp::control_path_profile::local};
    lsp::control_rto nonlocal {lsp::control_path_profile::nonlocal};
    PHOTON_REQUIRE(local.profile() == lsp::control_path_profile::local);
    PHOTON_REQUIRE(local.timeout_microseconds() == 20'000);
    PHOTON_REQUIRE(nonlocal.timeout_microseconds() == 100'000);

    lsp::outbound_control_window<8, 4> window;
    constexpr std::array<std::uint8_t, 2> first_frame {1, 2};
    constexpr std::array<std::uint8_t, 2> second_frame {3, 4};
    PHOTON_REQUIRE(window.reserve(1, first_frame, 100'000) == lsp::outbound_store_result::stored);
    PHOTON_REQUIRE(window.reserve(2, second_frame, 100'000) == lsp::outbound_store_result::stored);
    PHOTON_REQUIRE(window.due(30'000) == nullptr);
    PHOTON_REQUIRE(window.mark_initial_submitted(1, 1'000, local) == lsp::outbound_submission_result::submitted);
    PHOTON_REQUIRE(window.mark_initial_submitted(2, 2'000, local) == lsp::outbound_submission_result::submitted);

    const auto second_ack = window.acknowledge(2, 0, 12'000, local);
    PHOTON_REQUIRE(second_ack.status == lsp::outbound_ack_status::accepted);
    PHOTON_REQUIRE(second_ack.removed == 1 && second_ack.rto_updated);
    PHOTON_REQUIRE(second_ack.rtt_sample_microseconds == 10'000);
    PHOTON_REQUIRE(local.sampled());
    PHOTON_REQUIRE(local.smoothed_microseconds() == 10'000);
    PHOTON_REQUIRE(local.variation_microseconds() == 5'000);
    PHOTON_REQUIRE(local.timeout_microseconds() == 35'000);

    auto *retry = window.due(21'000);
    PHOTON_REQUIRE(retry != nullptr && retry->message_id == 1 && retry->karn_eligible);
    window.mark_retransmitted(*retry, 21'000, local);
    PHOTON_REQUIRE(!retry->karn_eligible && retry->first_sent_microseconds == 1'000);
    PHOTON_REQUIRE(retry->last_sent_microseconds == 21'000);
    const auto first_ack = window.acknowledge(1, 0, 30'000, local);
    PHOTON_REQUIRE(first_ack.status == lsp::outbound_ack_status::accepted);
    PHOTON_REQUIRE(first_ack.removed == 1 && !first_ack.rto_updated);
    PHOTON_REQUIRE(first_ack.rtt_sample_microseconds == 0);
    PHOTON_REQUIRE(local.timeout_microseconds() == 35'000);

    PHOTON_REQUIRE(window.reserve(3, first_frame, 100'000) == lsp::outbound_store_result::stored);
    PHOTON_REQUIRE(window.mark_initial_submitted(3, 40'000, local) == lsp::outbound_submission_result::submitted);
    const auto zero_time_ack = window.acknowledge(3, 0, 0, local);
    PHOTON_REQUIRE(zero_time_ack.status == lsp::outbound_ack_status::accepted);
    PHOTON_REQUIRE(zero_time_ack.removed == 1 && !zero_time_ack.rto_updated && window.size() == 0);
    PHOTON_REQUIRE(window.reserve(4, first_frame, 100'000) == lsp::outbound_store_result::stored);
    const auto unsent_ack = window.acknowledge(4, 0, 50'000, local);
    PHOTON_REQUIRE(unsent_ack.status == lsp::outbound_ack_status::no_match);
    PHOTON_REQUIRE(unsent_ack.removed == 0 && window.size() == 1);
    PHOTON_REQUIRE(window.mark_initial_submitted(4, 51'000, local) == lsp::outbound_submission_result::submitted);
    PHOTON_REQUIRE(window.acknowledge(4, 0, 52'000, local).removed == 1);
    return 0;
  }

  /** @brief Prove unexpired responses cause typed backpressure and retain semantic identity. */
  int test_response_retention_backpressure() {
    lsp::response_cache<8, 8, 2> cache;
    auto start_identity = lsp::response_semantic_identity {
      .message_type = 0x0200,
      .schema_version = 1,
      .generation = 7,
    };
    start_identity.semantic_id[0] = 11;
    start_identity.request_digest[0] = 0xa1;
    auto config_identity = lsp::response_semantic_identity {
      .message_type = 0x0300,
      .schema_version = 2,
      .generation = 9,
    };
    config_identity.semantic_id[0] = 12;
    config_identity.request_digest[0] = 0xa2;
    auto other_identity = lsp::response_semantic_identity {
      .message_type = 0x0201,
      .schema_version = 1,
      .generation = 7,
    };
    other_identity.semantic_id[0] = 11;
    other_identity.request_digest[0] = 0xa1;
    constexpr std::array<std::uint8_t, 2> first_request {1, 2};
    constexpr std::array<std::uint8_t, 2> first_response {3, 4};
    constexpr std::array<std::uint8_t, 2> second_request {5, 6};
    constexpr std::array<std::uint8_t, 2> second_response {7, 8};

    PHOTON_REQUIRE(
      cache.insert(
        100,
        {.identity = start_identity, .retention_deadline_microseconds = 100},
        first_request,
        first_response,
        10
      ) == lsp::response_cache_result::stored
    );
    PHOTON_REQUIRE(
      cache.insert(
        101,
        {.identity = config_identity, .retention_deadline_microseconds = 50},
        second_request,
        second_response,
        10
      ) == lsp::response_cache_result::stored
    );
    PHOTON_REQUIRE(
      cache.insert(
        102,
        {.identity = other_identity, .retention_deadline_microseconds = 200},
        first_request,
        first_response,
        20
      ) == lsp::response_cache_result::capacity_exhausted
    );
    PHOTON_REQUIRE(cache.size() == 2);

    const auto conflict = cache.lookup(100, other_identity, first_request, 20);
    PHOTON_REQUIRE(conflict.result == lsp::response_cache_result::conflicting_request);
    const auto hit = cache.lookup(100, start_identity, first_request, 99);
    PHOTON_REQUIRE(hit.result == lsp::response_cache_result::hit);
    PHOTON_REQUIRE(hit.metadata.identity == start_identity);
    PHOTON_REQUIRE(hit.metadata.retention_deadline_microseconds == 100);
    PHOTON_REQUIRE(hit.response.size() == 2 && hit.response[0] == 3 && hit.response[1] == 4);

    PHOTON_REQUIRE(cache.lookup(101, config_identity, second_request, 50).result == lsp::response_cache_result::miss);
    PHOTON_REQUIRE(cache.size() == 1);
    PHOTON_REQUIRE(
      cache.insert(
        102,
        {.identity = other_identity, .retention_deadline_microseconds = 200},
        first_request,
        first_response,
        50
      ) == lsp::response_cache_result::stored
    );
    PHOTON_REQUIRE(cache.expire(99) == 0);
    PHOTON_REQUIRE(cache.expire(100) == 1);
    PHOTON_REQUIRE(cache.lookup(100, start_identity, first_request, 100).result == lsp::response_cache_result::miss);
    PHOTON_REQUIRE(cache.size() == 1);

    constexpr lsp::response_semantic_identity invalid_identity {};
    PHOTON_REQUIRE(
      cache.insert(
        103,
        {.identity = invalid_identity, .retention_deadline_microseconds = 300},
        first_request,
        first_response,
        100
      ) == lsp::response_cache_result::invalid_semantic_identity
    );
    PHOTON_REQUIRE(
      cache.insert(
        103,
        {.identity = start_identity, .retention_deadline_microseconds = 100},
        first_request,
        first_response,
        100
      ) == lsp::response_cache_result::invalid_retention_deadline
    );
    return 0;
  }
}  // namespace

/** @brief Run focused LSPC state tests without an external dependency. */
int main() {
  if (const auto result = test_authenticated_ack_rtt(); result != 0) {
    return result;
  }
  return test_response_retention_backpressure();
}
