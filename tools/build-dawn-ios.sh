#!/bin/sh
# Builds Dawn's static library for iPhone from tools/dawn-src (with patches/dawn-ios-bc.patch applied), the way Dawn's own CI
# does, but with a deployment target new enough for BC texture support (iOS 16.4+).
set -eu
cd "$(dirname "$0")/dawn-src"
cmake -G Ninja -S . -B out/host -DDAWN_FETCH_DEPENDENCIES=ON \
  -DDAWN_BUILD_PROTOBUF=ON -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_NODE=OFF -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_METAL=OFF \
  -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_NULL=OFF \
  -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF -DDAWN_USE_GLFW=OFF -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_DOCS=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_WGSL_READER=OFF -DTINT_BUILD_WGSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_IR_BINARY=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/host --target protoc
cmake -S . -B out/ios_arm64 -G Ninja -DDAWN_MOBILE_BUILD=apple -C .github/workflows/dawn-ci.cmake \
  -DCMAKE_TOOLCHAIN_FILE=build-tools/apple.toolchain.cmake \
  -DPLATFORM=OS64 -DDEPLOYMENT_TARGET=17.0 -DENABLE_BITCODE=OFF -DENABLE_ARC=OFF -DENABLE_VISIBILITY=OFF \
  -DCMAKE_BUILD_TYPE=Release -DPROTOC_EXECUTABLE="$PWD/out/host/protoc"
ninja -C out/ios_arm64
ls -la out/ios_arm64/src/dawn/native/libwebgpu_dawn.a
