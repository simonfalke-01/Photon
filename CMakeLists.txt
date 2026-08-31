cmake_minimum_required(VERSION 3.23)

project(
  Photon
  VERSION 1.0.0
  DESCRIPTION "Shared C++23 implementation of the Lumen Streaming Protocol"
  HOMEPAGE_URL "https://github.com/simonfalke-01/Photon"
  LANGUAGES CXX
)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(PHOTON_BUILD_TESTS "Build Photon tests" ${PROJECT_IS_TOP_LEVEL})

set(PHOTON_LSP_HEADERS
  src/lsp/core/control.h
  src/lsp/core/core.h
  src/lsp/core/input.h
  src/lsp/core/packet.h
  src/lsp/core/rtp.h
  src/lsp/core/telemetry.h
  src/lsp/core/transport.h
  src/lsp/core/wire.h
  src/lsp/input_plane/feedback.h
  src/lsp/input_plane/input_plane.h
  src/lsp/input_plane/rate.h
  src/lsp/input_plane/state.h
  src/lsp/input_plane/wire.h
  src/lsp/media/audio.h
  src/lsp/media/av1.h
  src/lsp/media/common.h
  src/lsp/media/frame_pipeline.h
  src/lsp/media/h264.h
  src/lsp/media/hevc.h
  src/lsp/media/media.h
  src/lsp/transport/congestion.h
  src/lsp/transport/generation_fence.h
  src/lsp/transport/repair.h
  src/lsp/transport/rtcp.h
  src/lsp/transport/transport.h
)

add_library(Photon_LSP INTERFACE)
add_library(Photon::LSP ALIAS Photon_LSP)
set_target_properties(Photon_LSP PROPERTIES EXPORT_NAME LSP)

target_compile_features(Photon_LSP INTERFACE cxx_std_23)
target_include_directories(
  Photon_LSP
  INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Photon>"
)
target_sources(
  Photon_LSP
  INTERFACE
    FILE_SET HEADERS
      TYPE HEADERS
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
      FILES ${PHOTON_LSP_HEADERS}
)

add_custom_target(
  PhotonVerifySourceDigest ALL
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifySourceDigest.cmake"
  COMMENT "Verifying canonical Photon source manifest and digest"
  VERBATIM
)

if(PHOTON_BUILD_TESTS)
  include(CTest)
  add_executable(PhotonCompileSmoke tests/compile_smoke.cpp)
  target_link_libraries(PhotonCompileSmoke PRIVATE Photon::LSP)
  set_target_properties(PhotonCompileSmoke PROPERTIES CXX_EXTENSIONS OFF)
  add_test(NAME Photon.CompileSmoke COMMAND PhotonCompileSmoke)

  add_executable(PhotonRepairTests tests/repair_test.cpp)
  target_link_libraries(PhotonRepairTests PRIVATE Photon::LSP)
  set_target_properties(PhotonRepairTests PROPERTIES CXX_EXTENSIONS OFF)
  add_test(NAME Photon.Repair COMMAND PhotonRepairTests)
endif()

set(PHOTON_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Photon")

install(
  TARGETS Photon_LSP
  EXPORT PhotonTargets
  FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/Photon"
)
install(
  EXPORT PhotonTargets
  FILE PhotonTargets.cmake
  NAMESPACE Photon::
  DESTINATION "${PHOTON_INSTALL_CMAKEDIR}"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PhotonConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PhotonConfig.cmake"
  INSTALL_DESTINATION "${PHOTON_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PhotonConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhotonConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhotonConfigVersion.cmake"
  DESTINATION "${PHOTON_INSTALL_CMAKEDIR}"
)
install(FILES LICENSE README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/Photon")
