/*
 * Copyright (c) 2025 BayLibre SAS
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal ExecuTorch + Ethos-U scaffolding.
 *
 * This initial version verifies that ExecuTorch compiles and links on the
 * Corstone-1000-A320 (Cortex-A320 + Ethos-U85) target.  It initializes
 * the ExecuTorch PAL and prints a status banner.  Actual model inference
 * is added in the next commit.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include <executorch/runtime/platform/platform.h>

int main(void)
{
	printf("ExecuTorch + Ethos-U85 on Corstone-1000-A320\n");

	et_pal_init();
	printf("ExecuTorch PAL initialized\n");

	printf("Scaffolding OK — ready for model inference\n");
	return 0;
}
