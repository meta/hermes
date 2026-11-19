#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Builds Hermes Apple frameworks for all platforms (iOS, macOS, tvOS, visionOS,
# Mac Catalyst) and combines them into a single universal xcframework.
#
# Usage:
#   ./build-apple-framework.sh                   # build all platforms + universal xcframework
#   ./build-apple-framework.sh <platform>        # build a single platform (e.g. macosx, iphoneos)
#   ./build-apple-framework.sh build_framework   # combine already-built slices into xcframework
#   ./build-apple-framework.sh build_hermesc     # build host hermesc compiler
#   ./build-apple-framework.sh prepare_dest_root # assemble destroot from pre-built slices

if [ "$CI" ]; then
  set -x
fi
set -e

CURR_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

IMPORT_HERMESC_PATH=${HERMES_OVERRIDE_HERMESC_PATH:-$PWD/build_host_hermesc/ImportHostCompilers.cmake}
BUILD_TYPE=${BUILD_TYPE:-Debug}

HERMES_PATH="$CURR_SCRIPT_DIR/.."
HERMES_COMPILER_PACKAGE_PATH="$HERMES_PATH/npm/hermes-compiler"

NUM_CORES=$(sysctl -n hw.ncpu)

PLATFORMS=("macosx" "iphoneos" "iphonesimulator" "catalyst" "xros" "xrsimulator" "appletvos" "appletvsimulator")

if [[ -z "$JSI_PATH" ]]; then
  JSI_PATH="$HERMES_PATH/API/jsi"
fi

function use_env_var {
  if [[ -n "$1" ]]; then
    echo "$1"
  else
    echo "error: Missing $2 environment variable"
    exit 1
  fi
}

function get_ios_deployment_target {
  use_env_var "${IOS_DEPLOYMENT_TARGET}" "IOS_DEPLOYMENT_TARGET"
}

function get_visionos_deployment_target {
  use_env_var "${XROS_DEPLOYMENT_TARGET}" "XROS_DEPLOYMENT_TARGET"
}

function get_mac_deployment_target {
  use_env_var "${MAC_DEPLOYMENT_TARGET}" "MAC_DEPLOYMENT_TARGET"
}

function get_release_version {
  local package_json_path="$HERMES_COMPILER_PACKAGE_PATH/package.json"
  if [[ -f "$package_json_path" ]]; then
    local version
    version=$(node -e "console.log(JSON.parse(require('fs').readFileSync('$package_json_path', 'utf8')).version)" 2>/dev/null)
    if [[ -n "$version" ]]; then
      echo "$version"
      return
    else
      echo >&2 "Error: Failed to read version from $package_json_path"
      exit 1
    fi
  else
    echo >&2 "Error: Package file not found at $package_json_path"
    exit 1
  fi
}

# Given a specific target, retrieve the right architecture for it
# $1 the target you want to build. Allowed values: iphoneos, iphonesimulator, catalyst, macosx, xros, xrsimulator, appletvos, appletvsimulator
function get_architecture {
    if [[ $1 == "iphoneos" || $1 == "xros" || $1 == "appletvos" ]]; then
      echo "arm64"
    elif [[ $1 == "iphonesimulator" || $1 == "xrsimulator" || $1 == "appletvsimulator" || $1 == "catalyst" || $1 == "macosx" ]]; then
      echo "x86_64;arm64"
    else
      echo "Error: unknown architecture passed $1"
      exit 1
    fi
}

function get_deployment_target {
    if [[ $1 == "xros" || $1 == "xrsimulator" ]]; then
      echo "$(get_visionos_deployment_target)"
    elif [[ $1 == "macosx" ]]; then
      echo "$(get_mac_deployment_target)"
    else # tvOS and iOS use the same deployment target
      echo "$(get_ios_deployment_target)"
    fi
}

# Build host hermes compiler for internal bytecode
function build_host_hermesc {
  echo "Building hermesc"
  pushd "$HERMES_PATH" > /dev/null || exit 1
    cmake -S . -B build_host_hermesc -DJSI_DIR="$JSI_PATH" -DCMAKE_BUILD_TYPE=Release
    cmake --build ./build_host_hermesc --target hermesc -j "${NUM_CORES}"
  popd > /dev/null || exit 1
}

function build_host_hermesc_if_needed {
  if [[ ! -f "$IMPORT_HERMESC_PATH" ]]; then
    build_host_hermesc
  else
    echo "[HermesC] Skipping! Found an existent hermesc already at: $IMPORT_HERMESC_PATH"
  fi
}

# Utility function to configure an Apple framework
function configure_apple_framework {
  local enable_debugger cmake_build_type xcode_15_flags xcode_major_version

  if [[ $BUILD_TYPE == "Debug" ]]; then
    enable_debugger="true"
  else
    enable_debugger="false"
  fi
  if [[ $BUILD_TYPE == "Debug" ]]; then
    # JS developers aren't VM developers.
    # Therefore we're passing as build type Release, to provide a faster build.
    cmake_build_type="Release"
  else
    cmake_build_type="MinSizeRel"
  fi

  xcode_15_flags=""
  xcode_major_version=$(xcodebuild -version | grep -oE '[0-9]*' | head -n 1)
  if [[ $xcode_major_version -ge 15 ]]; then
    xcode_15_flags="LINKER:-ld_classic"
  fi

  # For catalyst, we need to set the target triple to use the macabi environment.
  # The architecture in -target is overridden by CMake's -arch flags, so we can use
  # any architecture here (arm64). CMake will add -arch x86_64 and -arch arm64 which
  # correctly override just the architecture portion while preserving ios-macabi.
  boost_context_flag=""
  shared_clang_flags=""
  if [[ $1 == "catalyst" ]]; then
    boost_context_flag="-DHERMES_ALLOW_BOOST_CONTEXT=0"
    shared_clang_flags="-target arm64-apple-ios$3-macabi -isystem ${CMAKE_OSX_SYSROOT}/System/iOSSupport/usr/include"
  fi

  pushd "$HERMES_PATH" > /dev/null || exit 1
    cmake -S . -B "build_$1" \
      -DHERMES_EXTRA_LINKER_FLAGS="$xcode_15_flags" \
      -DHERMES_APPLE_TARGET_PLATFORM:STRING="$1" \
      -DCMAKE_OSX_ARCHITECTURES:STRING="$2" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET:STRING="$3" \
      -DHERMES_ENABLE_DEBUGGER:BOOLEAN="$enable_debugger" \
      -DHERMES_ENABLE_INTL:BOOLEAN=true \
      -DHERMES_ENABLE_LIBFUZZER:BOOLEAN=false \
      -DHERMES_ENABLE_FUZZILLI:BOOLEAN=false \
      -DHERMES_ENABLE_TEST_SUITE:BOOLEAN=false \
      -DHERMES_ENABLE_BITCODE:BOOLEAN=false \
      -DHERMES_BUILD_APPLE_FRAMEWORK:BOOLEAN=true \
      -DHERMES_BUILD_SHARED_JSI:BOOLEAN=false \
      -DCMAKE_CXX_FLAGS:STRING="-gdwarf $shared_clang_flags" \
      -DCMAKE_C_FLAGS:STRING="-gdwarf $shared_clang_flags" \
      -DIMPORT_HOST_COMPILERS:PATH="$IMPORT_HERMESC_PATH" \
      -DJSI_DIR="$JSI_PATH" \
      -DHERMES_RELEASE_VERSION="$(get_release_version)" \
      -DCMAKE_BUILD_TYPE="$cmake_build_type" \
      $boost_context_flag
    popd > /dev/null || exit 1
}

function generate_dSYM {
    TARGET_PLATFORM="$1"

    DSYM_PATH="$PWD/build_$TARGET_PLATFORM/lib/hermesvm.framework.dSYM"
    xcrun dsymutil "$PWD/build_$TARGET_PLATFORM/lib/hermesvm.framework/hermesvm" --out "$DSYM_PATH"
    mkdir -p "$PWD/destroot/Library/Frameworks/$TARGET_PLATFORM"
    cp -R "$DSYM_PATH" "$PWD/destroot/Library/Frameworks/$TARGET_PLATFORM"
}

# Utility function to build an Apple framework for a single platform
# $1: platform, $2: architectures, $3: deployment target
function build_apple_framework {
  # Only build host HermesC if no file found at $IMPORT_HERMESC_PATH
  build_host_hermesc_if_needed

  # Confirm ImportHermesc.cmake is now available.
  [ ! -f "$IMPORT_HERMESC_PATH" ] &&
  echo "Host hermesc is required to build apple frameworks!"

  echo "Building $BUILD_TYPE framework for $1 with architectures: $2"
  configure_apple_framework "$1" "$2" "$3"

  pushd "$HERMES_PATH" > /dev/null || exit 1
    mkdir -p "destroot/Library/Frameworks/$1"
    cmake --build "./build_$1" --target hermesvm node-api -j "${NUM_CORES}"
    cp -R "./build_$1"/lib/hermesvm.framework* "destroot/Library/Frameworks/$1"
    cp -R "./build_$1"/lib/node-api.framework "destroot/Library/Frameworks/$1"
    generate_dSYM "$1"

    # In a MacOS build, also produce the hermes and hermesc CLI tools.
    if [[ $1 == macosx ]]; then
      cmake --build "./build_$1" --target hermesc hermesvm -j "${NUM_CORES}"
      mkdir -p destroot/bin
      cp "./build_$1/bin"/* "destroot/bin"
    fi

    # Copy over Hermes and JSI API headers.
    mkdir -p destroot/include/hermes/Public
    cp public/hermes/Public/*.h destroot/include/hermes/Public

    mkdir -p destroot/include/hermes
    cp API/hermes/*.h destroot/include/hermes

    mkdir -p destroot/include/hermes/cdp
    cp API/hermes/cdp/*.h destroot/include/hermes/cdp

    mkdir -p destroot/include/hermes/napi
    cp include/hermes/napi/*.h destroot/include/hermes/napi

    mkdir -p destroot/include/jsi
    cp "$JSI_PATH"/jsi/*.h destroot/include/jsi
  popd > /dev/null || exit 1
}

function prepare_dest_root_for_ci {
  mkdir -p  "destroot/bin"
  for platform in "${PLATFORMS[@]}"; do
    mkdir -p "destroot/Library/Frameworks/$platform"
    cp -R "./build_$platform/lib/hermesvm.framework"* "destroot/Library/Frameworks/$platform"
    cp -R "./build_$platform/lib/node-api.framework" "destroot/Library/Frameworks/$platform"
  done

  cp "./build_macosx/bin/"* "destroot/bin"

  # Copy over Hermes and JSI API headers.
  mkdir -p destroot/include/hermes/Public
  cp public/hermes/Public/*.h destroot/include/hermes/Public

  mkdir -p destroot/include/hermes
  cp API/hermes/*.h destroot/include/hermes

  mkdir -p destroot/include/hermes/cdp
  cp API/hermes/cdp/*.h destroot/include/hermes/cdp

  mkdir -p destroot/include/hermes/napi
  cp include/hermes/napi/*.h destroot/include/hermes/napi

  mkdir -p destroot/include/jsi
  cp "$JSI_PATH"/jsi/*.h destroot/include/jsi
}

# Accepts an array of frameworks and will place all of
# the architectures into an universal folder and then remove
# the merged frameworks from destroot
function create_universal_framework {
  pushd "$HERMES_PATH/destroot/Library/Frameworks" > /dev/null || exit 1

  local platforms=("$@")
  local failed=""

  echo "Creating universal framework for platforms: ${platforms[*]}"

  mkdir -p universal
  for framework in hermesvm node-api; do
    local args=""
    for platform in "${platforms[@]}"; do
      args+="-framework ${platform}/${framework}.framework "
    done
    # shellcheck disable=SC2086
    xcodebuild -create-xcframework $args -output "universal/${framework}.xcframework" || failed=1
  done

  if [[ -z $failed ]]
  then
    # # Remove the thin hermesvm.frameworks that are now part of the universal
    # XCFramework
    for platform in "${platforms[@]}"; do
      rm -r "$platform"
    done
  fi

  popd > /dev/null || exit 1
}

# Build a single framework for a given platform
# $1 is the target to build (e.g. iphoneos, macosx, etc.)
function build_framework {
  if [ ! -d destroot/Library/Frameworks/universal/hermesvm.xcframework ]; then
    deployment_target=$(get_deployment_target "$1")
    architecture=$(get_architecture "$1")
    build_apple_framework "$1" "$architecture" "$deployment_target"
  else
    echo "Skipping; Clean \"destroot\" to rebuild".
  fi
}

# Combine already-built slices into a universal xcframework
function build_universal_framework {
    if [ ! -d destroot/Library/Frameworks/universal/hermesvm.xcframework ]; then
        create_universal_framework "${PLATFORMS[@]}"
    else
        echo "Skipping; Clean \"destroot\" to rebuild".
    fi
}

# Build all platforms sequentially then combine into a universal xcframework
function create_framework {
    if [ ! -d destroot/Library/Frameworks/universal/hermesvm.xcframework ]; then
        for platform in "${PLATFORMS[@]}"; do
            build_framework "$platform"
        done
        build_universal_framework
    else
        echo "Skipping; Clean \"destroot\" to rebuild".
    fi
}

# --- Entry point ---
if [[ -z $1 ]]; then
  create_framework
elif [[ $1 == "build_framework" ]]; then
  build_universal_framework
elif [[ $1 == "build_hermesc" ]]; then
  build_host_hermesc_if_needed
elif [[ $1 == "prepare_dest_root" ]]; then
  prepare_dest_root_for_ci
else
  build_framework "$1"
fi
