# FVP Memory Ordering Bug - Test Case

This test demonstrates a memory ordering issue in ARM Fixed Virtual
Platform (FVP) version 11.29.27 that causes 118-145x performance
degradation in lockfree data structures.

## Quick Summary

**Issue:** FVP's `LDAR` instruction appears to not properly trigger cache
invalidation, causing excessive retries in lockfree algorithms.

**Results:**
- FVP without barriers: ~1,083,000 retries, ~35 seconds
- FVP with barriers: ~7,500 retries, ~1 second
- QEMU (reference): ~20,000 retries, ~1 second

## Getting the Code

This test is available at:
https://github.com/npitre/zephyr/tree/fvp_ordering_bug

```bash
# Clone the repository and branch
git clone https://github.com/npitre/zephyr.git -b fvp_ordering_bug
cd zephyr

# Or fetch the branch if you already have a Zephyr repository
git fetch https://github.com/npitre/zephyr.git fvp_ordering_bug
git checkout FETCH_HEAD
```

## Running the Test

```bash
# Build for ARMv9-A FVP (uses LSE atomics)
west build -b fvp_base_revc_2xaem/v9a/smp tests/arch/arm64/fvp_ordering_bug
west build -t run

# Build for ARMv8-A FVP (uses LDAXR/STLXR)
west build -b fvp_base_revc_2xaem/v8a/smp tests/arch/arm64/fvp_ordering_bug
west build -t run
```

**Toggle barriers:** Edit `src/main.c` lines 31-32:
- `BARRIER_BEFORE_READ`: 0 (shows bug) or 1 (shows mitigation)
- `BARRIER_AFTER_WRITE`: 0 (default) or 1

## Test Overview

The test implements a lockfree producer-consumer pattern:
- 2 producer threads + 1 consumer thread
- MPSC (multi-producer single-consumer) and SPSC queues
- 10,000 iterations per producer
- Self-contained atomic operations using inline assembly

The test measures retry counts to quantify cache coherency issues.

## Documentation

- **[BUG_REPORT.md](BUG_REPORT.md)** - Comprehensive technical report
  with all analysis and details
