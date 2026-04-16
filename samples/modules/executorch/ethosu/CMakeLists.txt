# Copyright (c) 2026 BayLibre SAS
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)

set(CMAKE_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# --- Model embedding ---
# With sysbuild, pass -Dethosu_ET_PTE_FILE_PATH=/path/to/model.pte
# Without sysbuild, pass -DET_PTE_FILE_PATH=/path/to/model.pte
# The sysbuild cache (SYSBUILD_NAME, image-prefixed variables) is only
# available after find_package(Zephyr).
if(TARGET sysbuild_cache)
  get_property(_sb_pte TARGET sysbuild_cache
               PROPERTY "${SYSBUILD_NAME}_ET_PTE_FILE_PATH")
endif()
if(_sb_pte)
  set(ET_PTE_FILE_PATH "${_sb_pte}"
      CACHE FILEPATH "Path to the ExecuTorch .pte model to embed" FORCE)
else()
  set(ET_PTE_FILE_PATH
      "${CMAKE_CURRENT_SOURCE_DIR}/src/models/add_arm_delegate_ethos-u85-256.pte"
      CACHE FILEPATH "Path to the ExecuTorch .pte model to embed")
endif()

# Resolve relative paths against the source directory.
if(ET_PTE_FILE_PATH AND NOT IS_ABSOLUTE "${ET_PTE_FILE_PATH}")
  get_filename_component(ET_PTE_FILE_PATH "${ET_PTE_FILE_PATH}" ABSOLUTE
                         BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(ET_PTE_FILE_PATH "${ET_PTE_FILE_PATH}"
      CACHE FILEPATH "Path to the ExecuTorch .pte model to embed" FORCE)
endif()

if(NOT ET_PTE_FILE_PATH OR NOT EXISTS "${ET_PTE_FILE_PATH}")
  message(FATAL_ERROR
    "Model not found at ET_PTE_FILE_PATH: ${ET_PTE_FILE_PATH}\n"
    "  west build -- -Dethosu_ET_PTE_FILE_PATH=/path/to/model.pte")
endif()

# --- Selective operator build ---
# Introspect the .pte to discover which portable ops are needed (if any).
# Fully-delegated models produce an empty list, which is fine.
# ZEPHYR_EXECUTORCH_MODULE_DIR is set by Zephyr's module discovery.
execute_process(
  COMMAND python
    "${ZEPHYR_EXECUTORCH_MODULE_DIR}/codegen/tools/gen_oplist.py"
    --model_file_path=${ET_PTE_FILE_PATH}
    --output_path=${CMAKE_CURRENT_BINARY_DIR}/temp.yaml
  OUTPUT_VARIABLE CMD_RESULT
)

if(CMD_RESULT MATCHES "aten::" OR CMD_RESULT MATCHES "dim_order_ops::")
  set(_GEN_PORTABLE_OPS ON)
  set(EXECUTORCH_SELECT_OPS_MODEL "${ET_PTE_FILE_PATH}"
      CACHE STRING "" FORCE)
else()
  set(_GEN_PORTABLE_OPS OFF)
  set(EXECUTORCH_SELECT_OPS_MODEL "")
endif()
project(executorch_ethosu)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

target_compile_options(app PRIVATE
  -Wall -Wno-switch -Wno-float-conversion -Wno-double-promotion)

# --- ExecuTorch module directory ---
if(NOT DEFINED EXECUTORCH_DIR)
  if(DEFINED ZEPHYR_EXECUTORCH_MODULE_DIR)
    set(EXECUTORCH_DIR ${ZEPHYR_EXECUTORCH_MODULE_DIR})
  else()
    message(FATAL_ERROR "ExecuTorch module not found.")
  endif()
endif()

set(EXECUTORCH_ROOT ${EXECUTORCH_DIR})

# --- Selective operator codegen ---
if(NOT TARGET portable_kernels)
  set(EXECUTORCH_PORTABLE_BUILD_KERNELS_ONLY ON)
  add_subdirectory(
    ${EXECUTORCH_DIR}/kernels/portable
    ${CMAKE_CURRENT_BINARY_DIR}/executorch/kernels/portable)
  unset(EXECUTORCH_PORTABLE_BUILD_KERNELS_ONLY)
endif()

set(EXECUTORCH_OPS_LIB "")
if(_GEN_PORTABLE_OPS)
  include(${EXECUTORCH_DIR}/tools/cmake/Utils.cmake)
  include(${EXECUTORCH_DIR}/tools/cmake/Codegen.cmake)
  if(NOT DEFINED EXECUTORCH_ENABLE_DTYPE_SELECTIVE_BUILD)
    set(EXECUTORCH_ENABLE_DTYPE_SELECTIVE_BUILD "")
  endif()
  gen_selected_ops(
    LIB_NAME "cpu_portable_ops_lib"
    OPS_SCHEMA_YAML ""
    ROOT_OPS ""
    INCLUDE_ALL_OPS ""
    OPS_FROM_MODEL "${EXECUTORCH_SELECT_OPS_MODEL}"
    DTYPE_SELECTIVE_BUILD "${EXECUTORCH_ENABLE_DTYPE_SELECTIVE_BUILD}")
  generate_bindings_for_kernels(
    LIB_NAME "cpu_portable_ops_lib"
    FUNCTIONS_YAML ${EXECUTORCH_DIR}/kernels/portable/functions.yaml
    DTYPE_SELECTIVE_BUILD "${EXECUTORCH_ENABLE_DTYPE_SELECTIVE_BUILD}")
  gen_operators_lib(
    LIB_NAME "cpu_portable_ops_lib"
    KERNEL_LIBS portable_kernels
    DEPS executorch
    DTYPE_SELECTIVE_BUILD "${EXECUTORCH_ENABLE_DTYPE_SELECTIVE_BUILD}")
  set(EXECUTORCH_OPS_LIB "cpu_portable_ops_lib")
endif()

# --- cortex_m quantize/dequantize ops for Cortex-A ---
# Models exported with -q embed cortex_m::quantize_per_tensor and
# cortex_m::dequantize_per_tensor ops at the delegation boundary.
# The C++ kernels are architecture-agnostic (Helium SIMD is optional),
# so we build just these two ops without pulling in the full cortex_m
# backend (which depends on CMSIS-NN / Cortex-M).
if(NOT TARGET cortex_m_qdq_kernels)
  include(${EXECUTORCH_DIR}/tools/cmake/Utils.cmake)
  include(${EXECUTORCH_DIR}/tools/cmake/Codegen.cmake)

  set(_qdq_yaml ${CMAKE_CURRENT_SOURCE_DIR}/src/cortex_m_qdq_ops.yaml)
  gen_selected_ops(LIB_NAME "cortex_m_qdq_ops_lib" OPS_SCHEMA_YAML "${_qdq_yaml}")
  generate_bindings_for_kernels(
    LIB_NAME "cortex_m_qdq_ops_lib" CUSTOM_OPS_YAML "${_qdq_yaml}")

  add_library(cortex_m_qdq_kernels
    ${EXECUTORCH_DIR}/backends/cortex_m/ops/op_quantize_per_tensor.cpp
    ${EXECUTORCH_DIR}/backends/cortex_m/ops/op_dequantize_per_tensor.cpp)
  target_link_libraries(cortex_m_qdq_kernels PRIVATE executorch kernels_util_all_deps)
  target_include_directories(cortex_m_qdq_kernels PRIVATE
    ${EXECUTORCH_DIR}/..
    ${EXECUTORCH_DIR}/runtime/core/portable_type/c10)

  gen_operators_lib(
    LIB_NAME "cortex_m_qdq_ops_lib"
    KERNEL_LIBS cortex_m_qdq_kernels
    DEPS executorch)
endif()

# --- Application sources ---
target_sources(app PRIVATE
  src/main.cpp
  ${EXECUTORCH_DIR}/examples/arm/executor_runner/arm_memory_allocator.cpp)

# --- Generate model_pte.h from the .pte binary ---
set(_model_pte_header ${CMAKE_CURRENT_BINARY_DIR}/model_pte.h)
add_custom_command(
  OUTPUT ${_model_pte_header}
  COMMAND ${Python3_EXECUTABLE}
    ${EXECUTORCH_DIR}/examples/arm/executor_runner/pte_to_header.py
    --pte ${ET_PTE_FILE_PATH}
    --outdir ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${ET_PTE_FILE_PATH}
          ${EXECUTORCH_DIR}/examples/arm/executor_runner/pte_to_header.py
  COMMENT "Converting ${ET_PTE_FILE_PATH} to model_pte.h")
add_custom_target(gen_model_header DEPENDS ${_model_pte_header})
add_dependencies(app gen_model_header)

# --- Allocator pool sizes (configurable via Kconfig) ---
if(DEFINED CONFIG_EXECUTORCH_METHOD_ALLOCATOR_POOL_SIZE)
  target_compile_definitions(app PRIVATE
    ET_ARM_METHOD_ALLOCATOR_POOL_SIZE=${CONFIG_EXECUTORCH_METHOD_ALLOCATOR_POOL_SIZE})
endif()
if(DEFINED CONFIG_EXECUTORCH_TEMP_ALLOCATOR_POOL_SIZE)
  target_compile_definitions(app PRIVATE
    ET_ARM_BAREMETAL_SCRATCH_TEMP_ALLOCATOR_POOL_SIZE=${CONFIG_EXECUTORCH_TEMP_ALLOCATOR_POOL_SIZE})
endif()

# --- Linking ---
target_link_libraries(app PRIVATE libexecutorch)
if(EXECUTORCH_OPS_LIB)
  target_link_libraries(app PRIVATE ${EXECUTORCH_OPS_LIB})
endif()
if(TARGET cortex_m_qdq_ops_lib)
  target_link_libraries(app PRIVATE cortex_m_qdq_ops_lib)
endif()
if(TARGET executorch_delegate_ethos_u)
  target_link_libraries(app PRIVATE executorch_delegate_ethos_u)
endif()
if(TARGET ethosu_core_driver)
  target_link_libraries(app PRIVATE ethosu_core_driver)
endif()
if(TARGET xnnpack_backend)
  target_link_libraries(app PRIVATE xnnpack_backend)
  # Force the linker to pull in the XnnpackBackend object file so its
  # static registration constructor runs.  Without this, the linker
  # discards the unused object and the backend is never registered.
  target_link_options(app PRIVATE
    "LINKER:--undefined=_ZN10executorch8backends14XnnpackBackendC1Ev")
endif()

target_include_directories(app PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
