# sandbox/src — in-process sandbox core (nanobind .pyd, the only C++ target).
#
# Two build modes:
#   1. Direct CMake (standalone, e.g. via build.py):
#        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#        cmake --build build
#   2. Wheel (scikit-build-core, via pip wheel . / python -m build --wheel):
#        reads pyproject.toml at project root, uses this dir as cmake.source-dir
#
# Requires nanobind (pip install nanobind) and a Visual Studio toolchain.
# Uses nanobind split mode (BACKEND_MODULE) to produce a stable ABI (abi3)
# extension that works across Python 3.10+ without recompilation.
# The runtime 'nanobind-backend' package provides the backend module.
cmake_minimum_required(VERSION 3.20)
project(win_sandbox_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Python (SABIModule required for split mode on Windows) ---
find_package(Python COMPONENTS Interpreter Development.Module Development.SABIModule REQUIRED)

# --- Find nanobind (pip install nanobind provides the CMake config) ---
find_package(nanobind CONFIG REQUIRED)

set(CORE_SOURCES
  token.cpp
  acl.cpp
  sid.cpp
  job.cpp
  spawn.cpp
  instance.cpp
  module.cpp
)

# Split mode: the extension module loads the backend from 'nanobind-backend'
# at runtime. This produces a stable ABI (abi3) extension compatible with
# Python 3.10+ (floor set by wheel.py-api = "cp310" in pyproject.toml).
nanobind_add_module(win_sandbox_native
  BACKEND_MODULE nanobind_backend
  ${CORE_SOURCES}
)

target_link_libraries(win_sandbox_native PRIVATE advapi32 bcrypt shell32)
target_include_directories(win_sandbox_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
  target_compile_options(win_sandbox_native PRIVATE /W3 /utf-8 /permissive-)
endif()

# === Wheel 安装规则（供 scikit-build-core 打包时使用） ===
# 扩展模块安装到 win_sandbox/_native/，与 __init__.py 的加载路径对齐
install(TARGETS win_sandbox_native DESTINATION win_sandbox/_native)
# Python 包装文件（__init__.py / exceptions.py / helpers.py）安装到 win_sandbox/
install(DIRECTORY python/win_sandbox/ DESTINATION win_sandbox)