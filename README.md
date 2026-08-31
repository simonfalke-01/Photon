# Photon

Photon is the shared, portable C++23 implementation of the Lumen Streaming Protocol used by Lumen hosts and Umbra clients. **Photon is the repository and CMake package name; the protocol remains LSP/1 on the wire**, with the `lumen::lsp` C++ namespace.

The repository intentionally contains only the source-identical protocol core:

- LSPC framing and portable wire primitives
- input state, rate negotiation, and feedback machinery
- RTP/RTCP media packetization and frame-pipeline primitives
- pacing, congestion control, DPLPMTUD, repair, and reconnect-generation state

Product runtimes, platform adapters, C ABI wrappers, DTLS providers, wolfSSL, and libSRTP remain in their owning products. Photon has no external runtime dependency and does not rename or version the LSP wire protocol.

## Use with CMake

Photon is a header-only package. It requires a C++23 compiler and exports one target:

```cmake
find_package(Photon 1 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE Photon::LSP)
```

Public headers retain their source hierarchy:

```cpp
#include <lsp/core/core.h>
#include <lsp/input_plane/input_plane.h>
#include <lsp/media/media.h>
#include <lsp/transport/transport.h>
```

Configure, build, test, and install a standalone checkout with:

```sh
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
cmake --install cmake-build-release --prefix install
```

Set `PHOTON_BUILD_TESTS=OFF` when embedding Photon without its compile smoke test. It defaults to enabled only for a top-level build.

## Source synchronization and integrity

`source-manifest.txt` is the canonical ordered source list. `source-digest.sha256` uses the same path-and-file-digest aggregate format as the former Lumen/Umbra mirror check. Every normal build verifies that the manifest exactly describes `src/lsp` and that all source bytes match the recorded digest.

Verify it directly:

```sh
cmake -P cmake/VerifySourceDigest.cmake
```

After an intentional, reviewed core update, refresh the digest with:

```sh
cmake -DPHOTON_UPDATE_SOURCE_DIGEST=ON -P cmake/VerifySourceDigest.cmake
```

The authoritative protocol API remains in `namespace lumen::lsp`; moving the implementation to Photon does not create a `photon` wire name or compatibility alias.

## License

Photon is licensed under GPL-3.0. See [LICENSE](LICENSE).
