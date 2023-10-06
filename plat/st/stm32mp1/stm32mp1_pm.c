/*
 * Copyright (c) 2015-2023, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>

#include <platform_def.h>

#include <arch_helpers.h>
#include <bl32/sp_min/platform_sp_min.h>
#include <common/debug.h>
#include <drivers/arm/gic_common.h>
#include <drivers/arm/gicv2.h>
#include <drivers/clk.h>
#include <drivers/delay_timer.h>
#include <drivers/st/stm32mp_reset.h>
#include <drivers/st/stm32mp1_rcc.h>
#include <dt-bindings/clock/stm32mp1-clks.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <plat/common/platform.h>

#include <stm32mp1_low_power.h>
#include <stm32mp1_power_config.h>

static uintptr_t stm32_sec_entrypoint;
static uint32_t cntfrq_core0;
static uintptr_t saved_entrypoint;

/*******************************************************************************
 * STM32MP1 handler called when a CPU is about to enter standby.
 * call by core 1 to enter in wfi
 ******************************************************************************/
static void stm32_cpu_standby(plat_local_state_t cpu_state)
{
	uint32_t interrupt = GIC_SPURIOUS_INTERRUPT;

	assert(cpu_state == ARM_LOCAL_STATE_RET);

	/*
	 * Enter standby state.
	 * Synchronize on memory accesses and instruction flow before the WFI
	 * instruction.
	 */
	dsb();
	isb();
	while (interrupt == GIC_SPURIOUS_INTERRUPT) {
		wfi();

		/* Acknoledge IT */
		interrupt = gicv2_acknowledge_interrupt();
		/* If Interrupt == 1022 it will be acknowledged by non secure */
		if ((interrupt != PENDING_G1_INTID) &&
		    (interrupt != GIC_SPURIOUS_INTERRUPT)) {
			gicv2_end_of_interrupt(interrupt);
		}
	}
}

/*******************************************************************************
 * STM32MP1 handler called when a power domain is about to be turned on. The
 * mpidr determines the CPU to be turned on.
 * call by core 0 to activate core 1
 ******************************************************************************/
static int stm32_pwr_domain_on(u_register_t mpidr)
{
	unsigned long current_cpu_mpidr = read_mpidr_el1();
	uintptr_t bkpr_core1_addr =
		tamp_bkpr(BOOT_API_CORE1_BRANCH_ADDRESS_TAMP_BCK_REG_IDX);
	uintptr_t bkpr_core1_magic =
		tamp_bkpr(BOOT_API_CORE1_MAGIC_NUMBER_TAMP_BCK_REG_IDX);

	if (stm32mp_is_single_core()) {
		return PSCI_E_INTERN_FAIL;
	}

	if (mpidr == current_cpu_mpidr) {
		return PSCI_E_INVALID_PARAMS;
	}

	/* Reset backup register content */
	mmio_write_32(bkpr_core1_magic, 0);

	/* Need to send additional IT 0 after individual core 1 reset */
	gicv2_raise_sgi(ARM_IRQ_NON_SEC_SGI_0, STM32MP_SECONDARY_CPU);

	/* Wait for this IT to be acknowledged by ROM code. */
	udelay(10);

	/* Only one valid entry point */
	if (stm32_sec_entrypoint != (uintptr_t)&sp_min_warm_entrypoint) {
		return PSCI_E_INVALID_ADDRESS;
	}

	clk_enable(RTCAPB);

	cntfrq_core0 = read_cntfrq_el0();

	/* Write entrypoint in backup RAM register */
	mmio_write_32(bkpr_core1_addr, stm32_sec_entrypoint);

	/* Write magic number in backup register */
	mmio_write_32(bkpr_core1_magic, BOOT_API_A7_CORE1_MAGIC_NUMBER);

	clk_disable(RTCAPB);

	/* Generate an IT to core 1 */
	gicv2_raise_sgi(ARM_IRQ_SEC_SGI_0, STM32MP_SECONDARY_CPU);

	return PSCI_E_SUCCESS;
}

/*******************************************************************************
 * STM32MP1 handler called when a power domain is about to be turned off. The
 * target_state encodes the power state that each level should transition to.
 ******************************************************************************/
static void stm32_pwr_domain_off(const psci_power_state_t *target_state)
{
	/* Nothing to do */
}

/*******************************************************************************
 * STM32MP1 handler called when a power domain is about to be suspended. The
 * target_state encodes the power state that each level should transition to.
 ******************************************************************************/
static void stm32_pwr_domain_suspend(const psci_power_state_t *target_state)
{
	uint32_t soc_mode = stm32mp1_get_lp_soc_mode(PSCI_MODE_SYSTEM_SUSPEND);

	stm32_enter_low_power(soc_mode, saved_entrypoint);
}

/*******************************************************************************
 * STM32MP1 handler called when a power domain has just been powered on after
 * being turned off earlier. The target_state encodes the low power state that
 * each level has woken up from.
 * call by core 1 just after wake up
 ******************************************************************************/
static void stm32_pwr_domain_on_finish(const psci_power_state_t *target_state)
{
	stm32mp_gic_pcpu_init();

	write_cntfrq_el0(cntfrq_core0);
}

/*******************************************************************************
 * STM32MP1 handler called when a power domain has just been powered on after
 * having been suspended earlier. The target_state encodes the low power state
 * that each level has woken up from.
 ******************************************************************************/
static void stm32_pwr_domain_suspend_finish(const psci_power_state_t
					    *target_state)
{
	/* Nothing to do, power domain is not disabled */
}

/*******************************************************************************
 * STM32MP1 handler called when a core tries to power itself down. If this
 * call is made by core 0, it is a return from stop mode. In this case, we
 * should restore previous context and jump to secure entrypoint.
 ******************************************************************************/
static void __dead2 stm32_pwr_domain_pwr_down_wfi(const psci_power_state_t
						  *target_state)
{
	if (MPIDR_AFFLVL0_VAL(read_mpidr_el1()) == STM32MP_PRIMARY_CPU) {
		void (*warm_entrypoint)(void) =
			(void (*)(void))stm32_sec_entrypoint;

		disable_mmu_icache_secure();

		stm32_pwr_down_wfi(stm32_is_cstop_done(),
				   stm32mp1_get_lp_soc_mode(PSCI_MODE_SYSTEM_SUSPEND));

		stm32_exit_cstop();

		warm_entrypoint();
	}

	mmio_write_32(stm32mp_rcc_base() + RCC_MP_GRSTCSETR,
		      RCC_MP_GRSTCSETR_MPUP1RST);

	/*
	 * Synchronize on memory accesses and instruction flow before
	 * auto-reset from the WFI instruction.
	 */
	dsb();
	isb();
	wfi();

	/* This shouldn't be reached */
	panic();
}

static void __dead2 stm32_system_off(void)
{
	uint32_t soc_mode = stm32mp1_get_lp_soc_mode(PSCI_MODE_SYSTEM_OFF);

	if (!stm32mp_is_single_core()) {
		/* Prepare Core 1 reset */
		mmio_setbits_32(stm32mp_rcc_base() + RCC_MP_GRSTCSETR,
				RCC_MP_GRSTCSETR_MPUP1RST);
		/* Send IT to core 1 to put itself in WFI */
		gicv2_raise_sgi(ARM_IRQ_SEC_SGI_1, STM32MP_SECONDARY_CPU);
	}

	stm32_enter_low_power(soc_mode, 0);

	stm32_pwr_down_wfi(true, soc_mode);

	/* This shouldn't be reached */
	panic();
}

static void __dead2 stm32_system_reset(void)
{
	stm32mp_system_reset();
}

static int stm32_validate_power_state(unsigned int power_state,
				      psci_power_state_t *req_state)
{
	int pstate = psci_get_pstate_type(power_state);

	if (pstate != 0) {
		return PSCI_E_INVALID_PARAMS;
	}

	if (psci_get_pstate_pwrlvl(power_state)) {
		return PSCI_E_INVALID_PARAMS;
	}

	if (psci_get_pstate_id(power_state)) {
		return PSCI_E_INVALID_PARAMS;
	}

	req_state->pwr_domain_state[0] = ARM_LOCAL_STATE_RET;
	req_state->pwr_domain_state[1] = ARM_LOCAL_STATE_RUN;

	return PSCI_E_SUCCESS;
}

static int stm32_validate_ns_entrypoint(uintptr_t entrypoint)
{
	/* The non-secure entry point must be in DDR */
	if (entrypoint < STM32MP_DDR_BASE) {
		return PSCI_E_INVALID_ADDRESS;
	}

	saved_entrypoint = entrypoint;

	return PSCI_E_SUCCESS;
}

static int stm32_node_hw_state(u_register_t target_cpu,
			       unsigned int power_level)
{
	/*
	 * The format of 'power_level' is implementation-defined, but 0 must
	 * mean a CPU. Only allow level 0.
	 */
	if (power_level != MPIDR_AFFLVL0) {
		return PSCI_E_INVALID_PARAMS;
	}

	/*
	 * From psci view the CPU 0 is always ON,
	 * CPU 1 can be SUSPEND or RUNNING.
	 * Therefore do not manage POWER OFF state and always return HW_ON.
	 */

	return (int)HW_ON;
}

static void stm32_get_sys_suspend_power_state(psci_power_state_t *req_state)
{
	req_state->pwr_domain_state[0] = ARM_LOCAL_STATE_OFF;
	req_state->pwr_domain_state[1] = ARM_LOCAL_STATE_OFF;
}

/*******************************************************************************
 * Export the platform handlers. The ARM Standard platform layer will take care
 * of registering the handlers with PSCI.
 ******************************************************************************/
static const plat_psci_ops_t stm32_psci_ops = {
	.cpu_standby = stm32_cpu_standby,
	.pwr_domain_on = stm32_pwr_domain_on,
	.pwr_domain_off = stm32_pwr_domain_off,
	.pwr_domain_suspend = stm32_pwr_domain_suspend,
	.pwr_domain_on_finish = stm32_pwr_domain_on_finish,
	.pwr_domain_suspend_finish = stm32_pwr_domain_suspend_finish,
	.pwr_domain_pwr_down_wfi = stm32_pwr_domain_pwr_down_wfi,
	.system_off = stm32_system_off,
	.system_reset = stm32_system_reset,
	.validate_power_state = stm32_validate_power_state,
	.validate_ns_entrypoint = stm32_validate_ns_entrypoint,
	.get_node_hw_state = stm32_node_hw_state,
	.get_sys_suspend_power_state = stm32_get_sys_suspend_power_state,
};

/*******************************************************************************
 * Export the platform specific power ops.
 ******************************************************************************/
int plat_setup_psci_ops(uintptr_t sec_entrypoint,
			const plat_psci_ops_t **psci_ops)
{
	stm32_sec_entrypoint = sec_entrypoint;
	*psci_ops = &stm32_psci_ops;

	return 0;
}
