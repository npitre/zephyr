# SPDX-License-Identifier: Apache-2.0

# Add SVE support if enabled, or explicitly disable it for ARMv9-A.
#
# When GCC_M_CPU is set for an ARMv9-A core (e.g. cortex-a710), it already
# implies the correct architecture features including FP16 and SVE/noSVE.
# Adding an explicit -march=armv9-a can break with older assemblers
# (binutils < 2.42) that don't handle -march=armv9-a + FP16 scalar ops.
# So only set GCC_M_ARCH when GCC_M_CPU is not already defined.
if(CONFIG_ARM64_SVE)
  if(DEFINED GCC_M_ARCH)
    set(GCC_M_ARCH "${GCC_M_ARCH}+sve")
  elseif(NOT DEFINED GCC_M_CPU)
    set(GCC_M_ARCH "armv9-a+sve")
  endif()
elseif(CONFIG_ARMV9_A)
  # ARMv9-A includes SVE by default, so explicitly disable it when not configured
  if(DEFINED GCC_M_ARCH)
    set(GCC_M_ARCH "${GCC_M_ARCH}+nosve")
  elseif(NOT DEFINED GCC_M_CPU)
    set(GCC_M_ARCH "armv9-a+nosve")
  endif()
endif()

if(CONFIG_ARM64_RNG)
  if(DEFINED GCC_M_ARCH)
    set(GCC_M_ARCH "${GCC_M_ARCH}+rng")
  endif()
endif()

if(DEFINED GCC_M_CPU)
  list(APPEND TOOLCHAIN_C_FLAGS   -mcpu=${GCC_M_CPU})
  list(APPEND TOOLCHAIN_LD_FLAGS  -mcpu=${GCC_M_CPU})
endif()

if(DEFINED GCC_M_ARCH)
  list(APPEND TOOLCHAIN_C_FLAGS   -march=${GCC_M_ARCH})
  list(APPEND TOOLCHAIN_LD_FLAGS  -march=${GCC_M_ARCH})
endif()

if(DEFINED GCC_M_TUNE)
  list(APPEND TOOLCHAIN_C_FLAGS   -mtune=${GCC_M_TUNE})
  list(APPEND TOOLCHAIN_LD_FLAGS  -mtune=${GCC_M_TUNE})
endif()

list(APPEND TOOLCHAIN_C_FLAGS   -mabi=lp64)
list(APPEND TOOLCHAIN_LD_FLAGS  -mabi=lp64)

# Branch protection (PAC/BTI) compiler flags
if(CONFIG_ARM_PACBTI_STANDARD)
  list(APPEND TOOLCHAIN_C_FLAGS -mbranch-protection=standard)
elseif(CONFIG_ARM_PACBTI_PACRET)
  list(APPEND TOOLCHAIN_C_FLAGS -mbranch-protection=pac-ret)
elseif(CONFIG_ARM_PACBTI_PACRET_LEAF)
  list(APPEND TOOLCHAIN_C_FLAGS -mbranch-protection=pac-ret+leaf)
elseif(CONFIG_ARM_PACBTI_BTI)
  list(APPEND TOOLCHAIN_C_FLAGS -mbranch-protection=bti)
elseif(CONFIG_ARM_PACBTI_PACRET_BTI)
  list(APPEND TOOLCHAIN_C_FLAGS -mbranch-protection=pac-ret+bti)
elseif(CONFIG_ARM_PACBTI_PACRET_LEAF_BTI)
  list(APPEND TOOLCHAIN_C_FLAGS -mbranch-protection=pac-ret+leaf+bti)
endif()

set(LLEXT_REMOVE_FLAGS
  -fno-pic
  -fno-pie
  -ffunction-sections
  -fdata-sections
  -Os
)

list(APPEND LLEXT_EDK_REMOVE_FLAGS
  --sysroot=.*
  -fmacro-prefix-map=.*
  -g.*
)

list(APPEND LLEXT_EDK_APPEND_FLAGS
  -nodefaultlibs
)
