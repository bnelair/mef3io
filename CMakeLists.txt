# Top-level build for the mef3io Python extension (driven by scikit-build-core).
cmake_minimum_required(VERSION 3.26)

# Single source of truth for the version (shared with the wheel metadata via
# pyproject.toml's regex provider, and with C++ via mef3io/version.hpp).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MEF3IO_VERSION LIMIT_COUNT 1)
project(mef3io VERSION ${MEF3IO_VERSION} LANGUAGES CXX)

add_subdirectory(core)

# --- nanobind extension ---
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

# Locate nanobind's CMake config via the installed package.
execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE
  OUTPUT_VARIABLE NB_DIR)
list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_mef3io STABLE_ABI bindings/python/mef3io_ext.cpp)
target_link_libraries(_mef3io PRIVATE mef3io_core)

# Build identity, exposed as _mef3io.__build_id__ and folded into the
# warm-start snapshot's version stamp (mef3io/cache.py), so two builds that
# share a version string never share snapshots. The git commit when the source
# is a checkout, else the configure time (string(TIMESTAMP) honours
# SOURCE_DATE_EPOCH, so reproducible builds get a reproducible id). Taken at
# CONFIGURE time: an incremental rebuild keeps the old id, which is why the
# cache also folds in the binary's size and mtime.
set(MEF3IO_BUILD_ID "")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _mef3io_sha OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _mef3io_git_rc ERROR_QUIET)
  if(_mef3io_git_rc EQUAL 0 AND _mef3io_sha)
    set(MEF3IO_BUILD_ID "git-${_mef3io_sha}")
  endif()
endif()
if(NOT MEF3IO_BUILD_ID)
  string(TIMESTAMP _mef3io_ts "%Y%m%dT%H%M%SZ" UTC)
  set(MEF3IO_BUILD_ID "cfg-${_mef3io_ts}")
endif()
target_compile_definitions(_mef3io PRIVATE MEF3IO_BUILD_ID="${MEF3IO_BUILD_ID}")
target_compile_features(_mef3io PRIVATE cxx_std_20)

install(TARGETS _mef3io LIBRARY DESTINATION mef3io)
