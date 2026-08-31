#include <lsp/core/core.h>
#include <lsp/input_plane/input_plane.h>
#include <lsp/media/media.h>
#include <lsp/transport/transport.h>

#include <array>
#include <cstdint>
#include <span>

static_assert(lumen::lsp::core_api_version == 1);

int main() {
  constexpr std::array<std::uint8_t, 1> dtls_record {22};
  static_assert(lumen::lsp::classify_packet(std::span<const std::uint8_t> {dtls_record}) == lumen::lsp::packet_class::dtls);

  constexpr lumen::lsp::dplpmtud path {{.base_payload = 1'200, .fast_probe_payload = 1'400, .ceiling_payload = 1'472}};
  static_assert(path.valid());
  static_assert(path.current_payload() == 1'200);
  return 0;
}
