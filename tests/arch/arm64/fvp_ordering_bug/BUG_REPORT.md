# FVP Memory Ordering Issue - Comprehensive Report

## Summary

ARM Fixed Virtual Platform (FVP) exhibits unexpected behavior with the
`LDAR` (Load-Acquire Register) instruction that results in significant
performance degradation in lockfree algorithms.

**Observed Impact:** 118-145x performance degradation in lockfree test
case

**Affected Platforms:** FVP Base RevC (both ARMv8-A and ARMv9-A SMP
configurations)

**FVP Version:** 11.29.27 (May 13 2025)

**Key Finding:** The issue affects both ARMv8.0-A (using LDAXR/STLXR
instructions) and ARMv8.1-A+ (using LSE atomics like LDADDAL/SWPAL).
Since the behavior is identical across different atomic instruction
implementations, this suggests the issue is in FVP's cache coherency
implementation or how `LDAR` triggers cache invalidation, rather than in
specific instruction emulation.

## Reproduction

### Minimal Test Case

A minimal reproducer is available at:
https://github.com/npitre/zephyr/tree/fvp_ordering_bug

Path: `tests/arch/arm64/fvp_ordering_bug/`

**Test Pattern:**
- 2 producer threads + 1 consumer thread
- Lockfree MPSC and SPSC queues using atomic operations
- 10,000 iterations per producer (40,000 total atomic operations)
- Self-contained atomic operations with explicit inline assembly

**Build and Run:**
```bash
# For ARMv9-A (uses LSE atomics LDADDAL/SWPAL)
west build -b fvp_base_revc_2xaem/v9a/smp tests/arch/arm64/fvp_ordering_bug
west build -t run

# For ARMv8-A (uses LDAXR/STLXR)
west build -b fvp_base_revc_2xaem/v8a/smp tests/arch/arm64/fvp_ordering_bug
west build -t run
```

**Configuration:** Edit `src/main.c` lines 31-32 to toggle barrier
usage.

Both platforms exhibit similar behavior, suggesting the issue is
architecture-independent.

## Performance Measurements

### Retry Statistics

The test measures "retry count" - how many times threads must re-attempt
lockfree operations when they cannot proceed. Retries occur when the
atomic values indicate it's not safe to proceed (e.g., queue appears
empty or full). This includes both:
- **Legitimate contention:** The queue state genuinely prevents the
  operation (expected behavior)
- **Cache coherency issues:** The queue has changed but the CPU observes
  a stale cached value

The baseline retry count on QEMU (~20,000) represents expected
legitimate contention. The dramatic increase on FVP (~1,083,000) and the
effectiveness of memory barriers (118x reduction) strongly suggest the
excess retries are due to cache coherency problems rather than normal
contention.

| Configuration | Retries | Retry/Op | Time |
|---------------|---------|----------|------|
| **FVP (no barriers)** | 1,083,452 | 27.09 | ~35s |
| **FVP (with DMB before reads)** | 9,144 | 0.19 | ~1s |
| **FVP (with both barriers)** | 7,485 | 0.19 | ~1s |
| **QEMU (reference)** | 20,243 | 0.51 | ~1s |

Adding explicit `DMB SY` instructions before atomic reads reduces retry
count by **118x** and brings execution time from 35 seconds to
approximately 1 second, matching QEMU performance.

### Barrier Effectiveness Analysis

We tested 4 barrier configurations to pinpoint the exact issue:

| AFTER_WRITE | BEFORE_READ | Retries | Improvement vs Baseline |
|-------------|-------------|---------|-------------------------|
| 0 | 0 | 1,083,452 | 1x (baseline) |
| 1 | 0 | 184,726 | 5.9x |
| 0 | 1 | **9,144** | **118x** ✅ |
| 1 | 1 | 7,485 | 145x |

**Critical Observation:** Barriers before read operations provide the
most significant improvement (118x). This strongly suggests the issue is
primarily with load-acquire operations rather than store-release
operations.

## Generated Code Analysis

The test uses architecture-appropriate ARM64 atomic instructions compiled
from inline assembly.

### ARMv8.1-A+ (LSE Atomics)

```assembly
local_atomic_get():
    ldar    x0, [x0]      # Load-Acquire Register
    ret

local_atomic_add():
    ldaddal x1, x1, [x0]  # Atomic Add with Acquire/Release
    ret

local_atomic_ptr_set():
    swpal   x1, x0, [x0]  # Swap with Acquire/Release
    ret
```

### ARMv8.0-A (Load-Exclusive/Store-Exclusive)

```assembly
local_atomic_get():
    ldar    x0, [x0]      # Load-Acquire Register
    ret

local_atomic_add():
1:  ldaxr   x0, [x2]      # Load-Acquire Exclusive
    add     x0, x0, x3
    stlxr   w1, x0, [x2]  # Store-Release Exclusive
    cbnz    w1, 1b
    sub     x0, x0, x3
    ret
```

Both instruction sets use `LDAR` for load-acquire operations. The
identical behavior across both variants suggests the issue is related to
`LDAR` and cache coherency rather than specific LSE instruction
implementations.

### With Mitigation Enabled

When `BARRIER_BEFORE_READ=1`, the code becomes:

```assembly
spsc_acquire:
    dmb     sy            # Explicit barrier
    bl      local_atomic_get    # Then LDAR
```

**Result:** `DMB SY + LDAR` works correctly, but `LDAR` alone does not
properly trigger cache invalidation.

## Expected vs Observed Behavior

### ARM Architecture Specification

Per ARM Architecture Reference Manual, `LDAR` (Load-Acquire Register) is
expected to:

1. Load the value from memory address
2. Provide acquire ordering (prior memory accesses complete first)
3. **Ensure cache coherency** - observe the latest value from any CPU

### Expected Behavior (QEMU Reference)

When CPU A writes to an atomic variable:
1. CPU A executes atomic store with release semantics
2. Value propagates to memory/interconnect
3. CPU B executes `LDAR` (load-acquire)
4. CPU B's cache coherency protocol ensures fresh value is loaded
5. CPU B observes CPU A's update promptly

**Result:** ~20,000 retries (legitimate contention due to small queue
size and concurrent access patterns), 1 second execution time

The retry count on QEMU represents the expected baseline for legitimate
contention - threads checking if they can proceed and finding the queue
temporarily full or empty due to concurrent operations by other threads.

### Observed Behavior on FVP

When CPU A writes to an atomic variable:
1. CPU A executes atomic store with release semantics
2. Value propagates to memory/interconnect
3. CPU B executes `LDAR` (load-acquire)
4. **CPU B appears to read from stale local cache**
5. CPU B does not observe CPU A's update for extended period
6. CPU B must retry operation many times before seeing updated value

**Result:** ~1,083,000 retries (53x more than QEMU), 35 second execution time

**Analysis:** The 53x increase in retries compared to QEMU's baseline
suggests that most of the excess ~1,060,000 retries are not due to
legitimate contention, but rather indicate that CPUs are repeatedly
observing values that should have been updated by other CPUs.

**Mitigation:** Explicit `DMB SY` before `LDAR` reduces retries to
~9,000 (close to QEMU's baseline) and execution time to ~1 second. This
dramatic improvement confirms that the excess retries on unmodified FVP
are indeed caused by cache coherency issues rather than increased
legitimate contention.

## Suggested Workaround

As a temporary workaround, adding explicit memory barriers before atomic
loads mitigates the issue:

```c
barrier_dmem_fence_full();  // DMB SY
value = atomic_get(&atomic_var);
```

This explicit barrier appears to trigger the cache invalidation that
would normally be expected from `LDAR`.

**Note:** This workaround should not be necessary on hardware that
correctly implements cache coherency for load-acquire operations.

## Test Environment

- **Platform:** FVP Base RevC 2xAEMv8A
- **FVP Version:** 11.29.27 (May 13 2025)
- **Configurations tested:**
  - `fvp_base_revc_2xaem/v8a/smp` (uses LDAXR/STLXR)
  - `fvp_base_revc_2xaem/v9a/smp` (uses LSE atomics)
- **QEMU Version (reference):** Used for comparison to validate expected
  behavior

## Reproducer Availability

A minimal reproducer is available at:
https://github.com/npitre/zephyr/tree/fvp_ordering_bug

Path: `tests/arch/arm64/fvp_ordering_bug/`
