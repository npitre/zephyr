/*
 * Copyright (c) 2020 Stephanos Ioannidis <root@stephanos.io>
 * Copyright (c) 2018 Xilinx, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_ttcps

#include <zephyr/arch/cpu.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/sys/clock.h>
#include <soc.h>
#include <zephyr/drivers/timer/system_timer.h>
#include "xlnx_psttc_timer_priv.h"

#define TIMER_IRQ		DT_INST_IRQN(0)
#define TIMER_BASE_ADDR		DT_INST_REG_ADDR(0)
#define TIMER_CLOCK_FREQUECY	DT_INST_PROP(0, clock_frequency)

#if defined(CONFIG_TEST)
const int32_t z_sys_timer_irq_for_test = DT_IRQN(DT_INST(0, xlnx_ttcps));
#endif

BUILD_ASSERT(TIMER_CLOCK_FREQUECY ==
			CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	     "Configured system timer frequency does not match the TTC "
	     "clock frequency in the device tree");

/*
 * Free-running 32-bit counter with a MATCH_0 register that interrupts only
 * when the counter steps onto it: a match value already behind the counter is
 * missed until the counter wraps. That is the COMPARE_EXACT backend, and the
 * core writes MATCH_0 through its verify loop.
 */
#define TIMER_CORE_BACKEND_COMPARE_EXACT
#define TIMER_CORE_COUNTER_WIDTH 32

static inline uint32_t timer_driver_cycle_get(void)
{
	return sys_read32(TIMER_BASE_ADDR + XTTCPS_COUNT_VALUE_OFFSET);
}

static inline void timer_driver_set_compare(uint32_t cycles)
{
	sys_write32(cycles, TIMER_BASE_ADDR + XTTCPS_MATCH_0_OFFSET);
}

#include "system_timer_generic.h"

static void ttc_isr(const void *arg)
{
	ARG_UNUSED(arg);

	/* Acknowledge interrupt: the status register clears on read */
	(void)sys_read32(TIMER_BASE_ADDR + XTTCPS_ISR_OFFSET);

	timer_core_announce();
}

static int sys_clock_driver_init(void)
{
	uint32_t reg_val;

	/* Stop timer */
	sys_write32(XTTCPS_CNT_CNTRL_DIS_MASK,
		    TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	/* Initialise timer registers */
	sys_write32(XTTCPS_CNT_CNTRL_RESET_VALUE,
		    TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_CLK_CNTRL_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_INTERVAL_VAL_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_MATCH_0_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_MATCH_1_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_MATCH_2_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_IER_OFFSET);
	sys_write32(XTTCPS_IXR_ALL_MASK, TIMER_BASE_ADDR + XTTCPS_ISR_OFFSET);

	/* Reset counter value */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	reg_val |= XTTCPS_CNT_CNTRL_RST_MASK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	/* Set match mode */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	reg_val |= XTTCPS_CNT_CNTRL_MATCH_MASK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	/* Connect timer interrupt */
	IRQ_CONNECT(TIMER_IRQ, 0, ttc_isr, 0, 0);

	/* Enable timer interrupt */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_IER_OFFSET);
	reg_val |= XTTCPS_IXR_MATCH_0_MASK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_IER_OFFSET);

	/* Start timer */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	reg_val &= (~XTTCPS_CNT_CNTRL_DIS_MASK);
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	timer_core_init();
	irq_enable(TIMER_IRQ);

	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2,
	 CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
