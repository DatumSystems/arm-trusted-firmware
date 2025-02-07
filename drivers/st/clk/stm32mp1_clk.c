/*
 * Copyright (C) 2018-2021, STMicroelectronics - All Rights Reserved
 *
 * SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <libfdt.h>

#include <platform_def.h>

#include <arch.h>
#include <arch_helpers.h>
#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <drivers/st/stm32_timer.h>
#include <drivers/st/stm32mp_clkfunc.h>
#include <drivers/st/stm32mp1_clk.h>
#include <drivers/st/stm32mp1_rcc.h>
#include <dt-bindings/clock/stm32mp1-clksrc.h>
#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <lib/utils_def.h>
#include <plat/common/platform.h>

#define MAX_HSI_HZ		64000000
#define USB_PHY_48_MHZ		48000000

#define TIMEOUT_US_200MS	U(200000)
#define TIMEOUT_US_1S		U(1000000)

#define PLLRDY_TIMEOUT		TIMEOUT_US_200MS
#define CLKSRC_TIMEOUT		TIMEOUT_US_200MS
#define CLKDIV_TIMEOUT		TIMEOUT_US_200MS
#define HSIDIV_TIMEOUT		TIMEOUT_US_200MS
#define OSCRDY_TIMEOUT		TIMEOUT_US_1S

const char *stm32mp_osc_node_label[NB_OSC] = {
	[_LSI] = "clk-lsi",
	[_LSE] = "clk-lse",
	[_HSI] = "clk-hsi",
	[_HSE] = "clk-hse",
	[_CSI] = "clk-csi",
	[_I2S_CKIN] = "i2s_ckin",
};

/* PLL settings computation related definitions */
#define POST_DIVM_MIN	8000000
#define POST_DIVM_MAX	16000000
#define DIVM_MIN	0
#define DIVM_MAX	63
#define DIVN_MIN	24
#define DIVN_MAX	99
#define DIVP_MIN	0
#define DIVP_MAX	127
#define FRAC_MAX	8192
#define VCO_MIN		800000000
#define VCO_MAX		1600000000

enum stm32mp1_parent_id {
/* Oscillators are defined in enum stm32mp_osc_id */

/* Other parent source */
	_HSI_KER = NB_OSC,
	_HSE_KER,
	_HSE_KER_DIV2,
	_CSI_KER,
	_PLL1_P,
	_PLL1_Q,
	_PLL1_R,
	_PLL2_P,
	_PLL2_Q,
	_PLL2_R,
	_PLL3_P,
	_PLL3_Q,
	_PLL3_R,
	_PLL4_P,
	_PLL4_Q,
	_PLL4_R,
	_ACLK,
	_PCLK1,
	_PCLK2,
	_PCLK3,
	_PCLK4,
	_PCLK5,
	_HCLK6,
	_HCLK2,
	_CK_PER,
	_CK_MPU,
	_CK_MCU,
	_USB_PHY_48,
	_PARENT_NB,
	_UNKNOWN_ID = 0xff,
};

/* Lists only the parent clock we are interested in */
enum stm32mp1_parent_sel {
	_I2C12_SEL,
	_I2C35_SEL,
	_STGEN_SEL,
	_I2C46_SEL,
	_SPI6_SEL,
	_UART1_SEL,
	_RNG1_SEL,
	_UART6_SEL,
	_UART24_SEL,
	_UART35_SEL,
	_UART78_SEL,
	_SDMMC12_SEL,
	_SDMMC3_SEL,
	_QSPI_SEL,
	_FMC_SEL,
	_AXIS_SEL,
	_MCUS_SEL,
	_USBPHY_SEL,
	_USBO_SEL,
	_RTC_SEL,
	_MPU_SEL,
	_PER_SEL,
	_PARENT_SEL_NB,
	_UNKNOWN_SEL = 0xff,
};

/* State the parent clock ID straight related to a clock */
static const uint8_t parent_id_clock_id[_PARENT_NB] = {
	[_HSE] = CK_HSE,
	[_HSI] = CK_HSI,
	[_CSI] = CK_CSI,
	[_LSE] = CK_LSE,
	[_LSI] = CK_LSI,
	[_I2S_CKIN] = _UNKNOWN_ID,
	[_USB_PHY_48] = _UNKNOWN_ID,
	[_HSI_KER] = CK_HSI,
	[_HSE_KER] = CK_HSE,
	[_HSE_KER_DIV2] = CK_HSE_DIV2,
	[_CSI_KER] = CK_CSI,
	[_PLL1_P] = PLL1_P,
	[_PLL1_Q] = PLL1_Q,
	[_PLL1_R] = PLL1_R,
	[_PLL2_P] = PLL2_P,
	[_PLL2_Q] = PLL2_Q,
	[_PLL2_R] = PLL2_R,
	[_PLL3_P] = PLL3_P,
	[_PLL3_Q] = PLL3_Q,
	[_PLL3_R] = PLL3_R,
	[_PLL4_P] = PLL4_P,
	[_PLL4_Q] = PLL4_Q,
	[_PLL4_R] = PLL4_R,
	[_ACLK] = CK_AXI,
	[_PCLK1] = CK_AXI,
	[_PCLK2] = CK_AXI,
	[_PCLK3] = CK_AXI,
	[_PCLK4] = CK_AXI,
	[_PCLK5] = CK_AXI,
	[_CK_PER] = CK_PER,
	[_CK_MPU] = CK_MPU,
	[_CK_MCU] = CK_MCU,
};

static unsigned int clock_id2parent_id(unsigned long id)
{
	unsigned int n = 0;

	for (n = 0; n < ARRAY_SIZE(parent_id_clock_id); n++) {
		if (parent_id_clock_id[n] == id) {
			return n;
		}
	}

	return _UNKNOWN_ID;
}

enum stm32mp1_pll_id {
	_PLL1,
	_PLL2,
	_PLL3,
	_PLL4,
	_PLL_NB
};

enum stm32mp1_div_id {
	_DIV_P,
	_DIV_Q,
	_DIV_R,
	_DIV_NB,
};

enum stm32mp1_clksrc_id {
	CLKSRC_MPU,
	CLKSRC_AXI,
	CLKSRC_MCU,
	CLKSRC_PLL12,
	CLKSRC_PLL3,
	CLKSRC_PLL4,
	CLKSRC_RTC,
	CLKSRC_MCO1,
	CLKSRC_MCO2,
	CLKSRC_NB
};

enum stm32mp1_clkdiv_id {
	CLKDIV_MPU,
	CLKDIV_AXI,
	CLKDIV_MCU,
	CLKDIV_APB1,
	CLKDIV_APB2,
	CLKDIV_APB3,
	CLKDIV_APB4,
	CLKDIV_APB5,
	CLKDIV_RTC,
	CLKDIV_MCO1,
	CLKDIV_MCO2,
	CLKDIV_NB
};

enum stm32mp1_pllcfg {
	PLLCFG_M,
	PLLCFG_N,
	PLLCFG_P,
	PLLCFG_Q,
	PLLCFG_R,
	PLLCFG_O,
	PLLCFG_NB
};

enum stm32mp1_pllcsg {
	PLLCSG_MOD_PER,
	PLLCSG_INC_STEP,
	PLLCSG_SSCG_MODE,
	PLLCSG_NB
};

enum stm32mp1_plltype {
	PLL_800,
	PLL_1600,
	PLL_TYPE_NB
};

struct stm32mp1_pll {
	uint8_t refclk_min;
	uint8_t refclk_max;
	uint8_t divn_max;
};

struct stm32mp1_clk_gate {
	uint16_t offset;
	uint8_t bit;
	uint8_t index;
	uint8_t set_clr;
	uint8_t secure;
	uint8_t sel; /* Relates to enum stm32mp1_parent_sel */
	uint8_t fixed; /* Relates to enum stm32mp1_parent_id */
};

struct stm32mp1_clk_sel {
	uint16_t offset;
	uint8_t src;
	uint8_t msk;
	uint8_t nb_parent;
	const uint8_t *parent;
};

#define REFCLK_SIZE 4
struct stm32mp1_clk_pll {
	enum stm32mp1_plltype plltype;
	uint16_t rckxselr;
	uint16_t pllxcfgr1;
	uint16_t pllxcfgr2;
	uint16_t pllxfracr;
	uint16_t pllxcr;
	uint16_t pllxcsgr;
	enum stm32mp_osc_id refclk[REFCLK_SIZE];
};

/* Compact structure of 32bit cells, copied raw when suspending */
struct stm32mp1_pll_settings {
	uint32_t valid_id;
	uint32_t freq[PLAT_MAX_OPP_NB];
	uint32_t volt[PLAT_MAX_OPP_NB];
	uint32_t cfg[PLAT_MAX_OPP_NB][PLAT_MAX_PLLCFG_NB];
	uint32_t frac[PLAT_MAX_OPP_NB];
};

/* Clocks with selectable source and non set/clr register access */
#define _CLK_SELEC(sec, off, b, idx, s)			\
	{						\
		.offset = (off),			\
		.bit = (b),				\
		.index = (idx),				\
		.set_clr = 0,				\
		.secure = (sec),			\
		.sel = (s),				\
		.fixed = _UNKNOWN_ID,			\
	}

/* Clocks with fixed source and non set/clr register access */
#define _CLK_FIXED(sec, off, b, idx, f)			\
	{						\
		.offset = (off),			\
		.bit = (b),				\
		.index = (idx),				\
		.set_clr = 0,				\
		.secure = (sec),			\
		.sel = _UNKNOWN_SEL,			\
		.fixed = (f),				\
	}

/* Clocks with selectable source and set/clr register access */
#define _CLK_SC_SELEC(sec, off, b, idx, s)			\
	{						\
		.offset = (off),			\
		.bit = (b),				\
		.index = (idx),				\
		.set_clr = 1,				\
		.secure = (sec),			\
		.sel = (s),				\
		.fixed = _UNKNOWN_ID,			\
	}

/* Clocks with fixed source and set/clr register access */
#define _CLK_SC_FIXED(sec, off, b, idx, f)			\
	{						\
		.offset = (off),			\
		.bit = (b),				\
		.index = (idx),				\
		.set_clr = 1,				\
		.secure = (sec),			\
		.sel = _UNKNOWN_SEL,			\
		.fixed = (f),				\
	}

#define _CLK_PARENT_SEL(_label, _rcc_selr, _parents)		\
	[_ ## _label ## _SEL] = {				\
		.offset = _rcc_selr,				\
		.src = _rcc_selr ## _ ## _label ## SRC_SHIFT,	\
		.msk = (_rcc_selr ## _ ## _label ## SRC_MASK) >> \
			(_rcc_selr ## _ ## _label ## SRC_SHIFT), \
		.parent = (_parents),				\
		.nb_parent = ARRAY_SIZE(_parents)		\
	}

#define _CLK_PLL(idx, type, off1, off2, off3,		\
		 off4, off5, off6,			\
		 p1, p2, p3, p4)			\
	[(idx)] = {					\
		.plltype = (type),			\
		.rckxselr = (off1),			\
		.pllxcfgr1 = (off2),			\
		.pllxcfgr2 = (off3),			\
		.pllxfracr = (off4),			\
		.pllxcr = (off5),			\
		.pllxcsgr = (off6),			\
		.refclk[0] = (p1),			\
		.refclk[1] = (p2),			\
		.refclk[2] = (p3),			\
		.refclk[3] = (p4),			\
	}

#define NB_GATES	ARRAY_SIZE(stm32mp1_clk_gate)

#define SEC		1
#define N_S		0

static const struct stm32mp1_clk_gate stm32mp1_clk_gate[] = {
	_CLK_FIXED(SEC, RCC_DDRITFCR, 0, DDRC1, _ACLK),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 1, DDRC1LP, _ACLK),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 2, DDRC2, _ACLK),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 3, DDRC2LP, _ACLK),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 4, DDRPHYC, _PLL2_R),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 5, DDRPHYCLP, _PLL2_R),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 6, DDRCAPB, _PCLK4),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 7, DDRCAPBLP, _PCLK4),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 8, AXIDCG, _ACLK),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 9, DDRPHYCAPB, _PCLK4),
	_CLK_FIXED(SEC, RCC_DDRITFCR, 10, DDRPHYCAPBLP, _PCLK4),

#if defined(IMAGE_BL32)
	_CLK_SC_FIXED(N_S, RCC_MP_APB1ENSETR, 6, TIM12_K, _PCLK1),
#endif
#if defined(IMAGE_BL2)
	_CLK_SC_SELEC(N_S, RCC_MP_APB1ENSETR, 14, USART2_K, _UART24_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB1ENSETR, 15, USART3_K, _UART35_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB1ENSETR, 16, UART4_K, _UART24_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB1ENSETR, 17, UART5_K, _UART35_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB1ENSETR, 18, UART7_K, _UART78_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB1ENSETR, 19, UART8_K, _UART78_SEL),
#endif

#if defined(IMAGE_BL32)
	_CLK_SC_FIXED(N_S, RCC_MP_APB2ENSETR, 2, TIM15_K, _PCLK2),
#endif
#if defined(IMAGE_BL2)
	_CLK_SC_SELEC(N_S, RCC_MP_APB2ENSETR, 13, USART6_K, _UART6_SEL),
#endif

	_CLK_SC_FIXED(N_S, RCC_MP_APB3ENSETR, 11, SYSCFG, _UNKNOWN_ID),

#if defined(IMAGE_BL32)
	_CLK_SC_SELEC(N_S, RCC_MP_APB4ENSETR, 0, LTDC_PX, _UNKNOWN_SEL),
#endif
	_CLK_SC_SELEC(N_S, RCC_MP_APB4ENSETR, 8, DDRPERFM, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB4ENSETR, 15, IWDG2, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_APB4ENSETR, 16, USBPHY_K, _USBPHY_SEL),

	_CLK_SC_SELEC(SEC, RCC_MP_APB5ENSETR, 0, SPI6_K, _SPI6_SEL),
	_CLK_SC_SELEC(SEC, RCC_MP_APB5ENSETR, 2, I2C4_K, _I2C46_SEL),
	_CLK_SC_SELEC(SEC, RCC_MP_APB5ENSETR, 3, I2C6_K, _I2C46_SEL),
	_CLK_SC_SELEC(SEC, RCC_MP_APB5ENSETR, 4, USART1_K, _UART1_SEL),
	_CLK_SC_FIXED(SEC, RCC_MP_APB5ENSETR, 8, RTCAPB, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_APB5ENSETR, 11, TZC1, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_APB5ENSETR, 12, TZC2, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_APB5ENSETR, 13, TZPC, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_APB5ENSETR, 15, IWDG1, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_APB5ENSETR, 16, BSEC, _PCLK5),
	_CLK_SC_SELEC(SEC, RCC_MP_APB5ENSETR, 20, STGEN_K, _STGEN_SEL),

	_CLK_SELEC(SEC, RCC_BDCR, 20, RTC, _RTC_SEL),

#if defined(IMAGE_BL32)
	_CLK_SC_SELEC(N_S, RCC_MP_AHB2ENSETR, 0, DMA1, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB2ENSETR, 1, DMA2, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB2ENSETR, 8, USBO_K, _USBO_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB2ENSETR, 16, SDMMC3_K, _SDMMC3_SEL),
#endif

	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 0, GPIOA, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 1, GPIOB, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 2, GPIOC, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 3, GPIOD, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 4, GPIOE, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 5, GPIOF, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 6, GPIOG, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 7, GPIOH, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 8, GPIOI, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 9, GPIOJ, _UNKNOWN_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB4ENSETR, 10, GPIOK, _UNKNOWN_SEL),

	_CLK_SC_FIXED(SEC, RCC_MP_AHB5ENSETR, 0, GPIOZ, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_AHB5ENSETR, 4, CRYP1, _PCLK5),
	_CLK_SC_FIXED(SEC, RCC_MP_AHB5ENSETR, 5, HASH1, _PCLK5),
	_CLK_SC_SELEC(SEC, RCC_MP_AHB5ENSETR, 6, RNG1_K, _RNG1_SEL),
	_CLK_SC_FIXED(SEC, RCC_MP_AHB5ENSETR, 8, BKPSRAM, _PCLK5),

#if defined(IMAGE_BL32)
	_CLK_SC_FIXED(SEC, RCC_MP_TZAHB6ENSETR, 0, MDMA, _ACLK),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB6ENSETR, 5, GPU, _UNKNOWN_SEL),
	_CLK_SC_FIXED(N_S, RCC_MP_AHB6ENSETR, 10, ETHMAC, _ACLK),
#endif
#if defined(IMAGE_BL2)
	_CLK_SC_SELEC(N_S, RCC_MP_AHB6ENSETR, 12, FMC_K, _FMC_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB6ENSETR, 14, QSPI_K, _QSPI_SEL),
#endif
	_CLK_SC_SELEC(N_S, RCC_MP_AHB6ENSETR, 16, SDMMC1_K, _SDMMC12_SEL),
	_CLK_SC_SELEC(N_S, RCC_MP_AHB6ENSETR, 17, SDMMC2_K, _SDMMC12_SEL),
#if defined(IMAGE_BL32)
	_CLK_SC_SELEC(N_S, RCC_MP_AHB6ENSETR, 24, USBH, _UNKNOWN_SEL),
#endif

	_CLK_SELEC(N_S, RCC_DBGCFGR, 8, CK_DBG, _UNKNOWN_SEL),
};

static const uint8_t i2c12_parents[] = {
	_PCLK1, _PLL4_R, _HSI_KER, _CSI_KER
};

static const uint8_t i2c35_parents[] = {
	_PCLK1, _PLL4_R, _HSI_KER, _CSI_KER
};

static const uint8_t stgen_parents[] = {
	_HSI_KER, _HSE_KER
};

static const uint8_t i2c46_parents[] = {
	_PCLK5, _PLL3_Q, _HSI_KER, _CSI_KER
};

static const uint8_t spi6_parents[] = {
	_PCLK5, _PLL4_Q, _HSI_KER, _CSI_KER, _HSE_KER, _PLL3_Q
};

static const uint8_t usart1_parents[] = {
	_PCLK5, _PLL3_Q, _HSI_KER, _CSI_KER, _PLL4_Q, _HSE_KER
};

static const uint8_t rng1_parents[] = {
	_CSI, _PLL4_R, _LSE, _LSI
};

static const uint8_t uart6_parents[] = {
	_PCLK2, _PLL4_Q, _HSI_KER, _CSI_KER, _HSE_KER
};

static const uint8_t uart234578_parents[] = {
	_PCLK1, _PLL4_Q, _HSI_KER, _CSI_KER, _HSE_KER
};

static const uint8_t sdmmc12_parents[] = {
	_HCLK6, _PLL3_R, _PLL4_P, _HSI_KER
};

static const uint8_t sdmmc3_parents[] = {
	_HCLK2, _PLL3_R, _PLL4_P, _HSI_KER
};

static const uint8_t qspi_parents[] = {
	_ACLK, _PLL3_R, _PLL4_P, _CK_PER
};

static const uint8_t fmc_parents[] = {
	_ACLK, _PLL3_R, _PLL4_P, _CK_PER
};

static const uint8_t axiss_parents[] = {
	_HSI, _HSE, _PLL2_P
};

static const uint8_t mcuss_parents[] = {
	_HSI, _HSE, _CSI, _PLL3_P
};

static const uint8_t usbphy_parents[] = {
	_HSE_KER, _PLL4_R, _HSE_KER_DIV2
};

static const uint8_t usbo_parents[] = {
	_PLL4_R, _USB_PHY_48
};

static const uint8_t rtc_parents[] = {
	_UNKNOWN_ID, _LSE, _LSI, _HSE
};

static const uint8_t mpu_parents[] = {
	_HSI, _HSE, _PLL1_P, _PLL1_P /* specific div */
};

static const uint8_t per_parents[] = {
	_HSI, _HSE, _CSI,
};

static const struct stm32mp1_clk_sel stm32mp1_clk_sel[_PARENT_SEL_NB] = {
	_CLK_PARENT_SEL(I2C12, RCC_I2C12CKSELR, i2c12_parents),
	_CLK_PARENT_SEL(I2C35, RCC_I2C35CKSELR, i2c35_parents),
	_CLK_PARENT_SEL(STGEN, RCC_STGENCKSELR, stgen_parents),
	_CLK_PARENT_SEL(I2C46, RCC_I2C46CKSELR, i2c46_parents),
	_CLK_PARENT_SEL(SPI6, RCC_SPI6CKSELR, spi6_parents),
	_CLK_PARENT_SEL(UART1, RCC_UART1CKSELR, usart1_parents),
	_CLK_PARENT_SEL(RNG1, RCC_RNG1CKSELR, rng1_parents),
	_CLK_PARENT_SEL(RTC, RCC_BDCR, rtc_parents),
	_CLK_PARENT_SEL(MPU, RCC_MPCKSELR, mpu_parents),
	_CLK_PARENT_SEL(PER, RCC_CPERCKSELR, per_parents),
	_CLK_PARENT_SEL(UART6, RCC_UART6CKSELR, uart6_parents),
	_CLK_PARENT_SEL(UART24, RCC_UART24CKSELR, uart234578_parents),
	_CLK_PARENT_SEL(UART35, RCC_UART35CKSELR, uart234578_parents),
	_CLK_PARENT_SEL(UART78, RCC_UART78CKSELR, uart234578_parents),
	_CLK_PARENT_SEL(SDMMC12, RCC_SDMMC12CKSELR, sdmmc12_parents),
	_CLK_PARENT_SEL(SDMMC3, RCC_SDMMC3CKSELR, sdmmc3_parents),
	_CLK_PARENT_SEL(QSPI, RCC_QSPICKSELR, qspi_parents),
	_CLK_PARENT_SEL(FMC, RCC_FMCCKSELR, fmc_parents),
	_CLK_PARENT_SEL(AXIS, RCC_ASSCKSELR, axiss_parents),
	_CLK_PARENT_SEL(MCUS, RCC_MSSCKSELR, mcuss_parents),
	_CLK_PARENT_SEL(USBPHY, RCC_USBCKSELR, usbphy_parents),
	_CLK_PARENT_SEL(USBO, RCC_USBCKSELR, usbo_parents),
};

/* Define characteristic of PLL according type */
static const struct stm32mp1_pll stm32mp1_pll[PLL_TYPE_NB] = {
	[PLL_800] = {
		.refclk_min = 4,
		.refclk_max = 16,
		.divn_max = 99,
	},
	[PLL_1600] = {
		.refclk_min = 8,
		.refclk_max = 16,
		.divn_max = 199,
	},
};

/* PLLNCFGR2 register divider by output */
static const uint8_t pllncfgr2[_DIV_NB] = {
	[_DIV_P] = RCC_PLLNCFGR2_DIVP_SHIFT,
	[_DIV_Q] = RCC_PLLNCFGR2_DIVQ_SHIFT,
	[_DIV_R] = RCC_PLLNCFGR2_DIVR_SHIFT,
};

static const struct stm32mp1_clk_pll stm32mp1_clk_pll[_PLL_NB] = {
	_CLK_PLL(_PLL1, PLL_1600,
		 RCC_RCK12SELR, RCC_PLL1CFGR1, RCC_PLL1CFGR2,
		 RCC_PLL1FRACR, RCC_PLL1CR, RCC_PLL1CSGR,
		 _HSI, _HSE, _UNKNOWN_OSC_ID, _UNKNOWN_OSC_ID),
	_CLK_PLL(_PLL2, PLL_1600,
		 RCC_RCK12SELR, RCC_PLL2CFGR1, RCC_PLL2CFGR2,
		 RCC_PLL2FRACR, RCC_PLL2CR, RCC_PLL2CSGR,
		 _HSI, _HSE, _UNKNOWN_OSC_ID, _UNKNOWN_OSC_ID),
	_CLK_PLL(_PLL3, PLL_800,
		 RCC_RCK3SELR, RCC_PLL3CFGR1, RCC_PLL3CFGR2,
		 RCC_PLL3FRACR, RCC_PLL3CR, RCC_PLL3CSGR,
		 _HSI, _HSE, _CSI, _UNKNOWN_OSC_ID),
	_CLK_PLL(_PLL4, PLL_800,
		 RCC_RCK4SELR, RCC_PLL4CFGR1, RCC_PLL4CFGR2,
		 RCC_PLL4FRACR, RCC_PLL4CR, RCC_PLL4CSGR,
		 _HSI, _HSE, _CSI, _I2S_CKIN),
};

/* Prescaler table lookups for clock computation */
/* div = /1 /2 /4 /8 / 16 /64 /128 /512 */
static const uint8_t stm32mp1_mcu_div[16] = {
	0, 1, 2, 3, 4, 6, 7, 8, 9, 9, 9, 9, 9, 9, 9, 9
};

/* div = /1 /2 /4 /8 /16 : same divider for PMU and APBX */
#define stm32mp1_mpu_div stm32mp1_mpu_apbx_div
#define stm32mp1_apbx_div stm32mp1_mpu_apbx_div
static const uint8_t stm32mp1_mpu_apbx_div[8] = {
	0, 1, 2, 3, 4, 4, 4, 4
};

/* div = /1 /2 /3 /4 */
static const uint8_t stm32mp1_axi_div[8] = {
	1, 2, 3, 4, 4, 4, 4, 4
};

#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
static const char * const stm32mp1_clk_parent_name[_PARENT_NB] __unused = {
	[_HSI] = "HSI",
	[_HSE] = "HSE",
	[_CSI] = "CSI",
	[_LSI] = "LSI",
	[_LSE] = "LSE",
	[_I2S_CKIN] = "I2S_CKIN",
	[_HSI_KER] = "HSI_KER",
	[_HSE_KER] = "HSE_KER",
	[_HSE_KER_DIV2] = "HSE_KER_DIV2",
	[_CSI_KER] = "CSI_KER",
	[_PLL1_P] = "PLL1_P",
	[_PLL1_Q] = "PLL1_Q",
	[_PLL1_R] = "PLL1_R",
	[_PLL2_P] = "PLL2_P",
	[_PLL2_Q] = "PLL2_Q",
	[_PLL2_R] = "PLL2_R",
	[_PLL3_P] = "PLL3_P",
	[_PLL3_Q] = "PLL3_Q",
	[_PLL3_R] = "PLL3_R",
	[_PLL4_P] = "PLL4_P",
	[_PLL4_Q] = "PLL4_Q",
	[_PLL4_R] = "PLL4_R",
	[_ACLK] = "ACLK",
	[_PCLK1] = "PCLK1",
	[_PCLK2] = "PCLK2",
	[_PCLK3] = "PCLK3",
	[_PCLK4] = "PCLK4",
	[_PCLK5] = "PCLK5",
	[_HCLK6] = "KCLK6",
	[_HCLK2] = "HCLK2",
	[_CK_PER] = "CK_PER",
	[_CK_MPU] = "CK_MPU",
	[_CK_MCU] = "CK_MCU",
	[_USB_PHY_48] = "USB_PHY_48",
};

static const char *
const stm32mp1_clk_parent_sel_name[_PARENT_SEL_NB] __unused = {
	[_I2C12_SEL] = "I2C12",
	[_I2C35_SEL] = "I2C35",
	[_STGEN_SEL] = "STGEN",
	[_I2C46_SEL] = "I2C46",
	[_SPI6_SEL] = "SPI6",
	[_UART1_SEL] = "USART1",
	[_RNG1_SEL] = "RNG1",
	[_UART6_SEL] = "UART6",
	[_UART24_SEL] = "UART24",
	[_UART35_SEL] = "UART35",
	[_UART78_SEL] = "UART78",
	[_SDMMC12_SEL] = "SDMMC12",
	[_SDMMC3_SEL] = "SDMMC3",
	[_QSPI_SEL] = "QSPI",
	[_FMC_SEL] = "FMC",
	[_AXIS_SEL] = "AXISS",
	[_MCUS_SEL] = "MCUSS",
	[_USBPHY_SEL] = "USBPHY",
	[_USBO_SEL] = "USBO",
};
#endif

/* RCC clock device driver private */
static unsigned long stm32mp1_osc[NB_OSC];
static struct spinlock reg_lock;
static unsigned int gate_refcounts[NB_GATES];
static struct spinlock refcount_lock;
static struct stm32mp1_pll_settings pll1_settings;
static uint32_t current_opp_khz;
static uint32_t pll3cr;
static uint32_t pll4cr;
static uint32_t mssckselr;
static uint32_t mcudivr;

static const struct stm32mp1_clk_gate *gate_ref(unsigned int idx)
{
	return &stm32mp1_clk_gate[idx];
}

static bool gate_is_non_secure(const struct stm32mp1_clk_gate *gate)
{
	return gate->secure == N_S;
}

static const struct stm32mp1_clk_sel *clk_sel_ref(unsigned int idx)
{
	return &stm32mp1_clk_sel[idx];
}

static const struct stm32mp1_clk_pll *pll_ref(unsigned int idx)
{
	return &stm32mp1_clk_pll[idx];
}

static void stm32mp1_clk_lock(struct spinlock *lock)
{
	if (stm32mp_lock_available()) {
		/* Assume interrupts are masked */
		spin_lock(lock);
	}
}

static void stm32mp1_clk_unlock(struct spinlock *lock)
{
	if (stm32mp_lock_available()) {
		spin_unlock(lock);
	}
}

bool stm32mp1_rcc_is_secure(void)
{
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t mask = RCC_TZCR_TZEN;

	return (mmio_read_32(rcc_base + RCC_TZCR) & mask) == mask;
}

bool stm32mp1_rcc_is_mckprot(void)
{
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t mask = RCC_TZCR_TZEN | RCC_TZCR_MCKPROT;

	return (mmio_read_32(rcc_base + RCC_TZCR) & mask) == mask;
}

void stm32mp1_clk_rcc_regs_lock(void)
{
	stm32mp1_clk_lock(&reg_lock);
}

void stm32mp1_clk_rcc_regs_unlock(void)
{
	stm32mp1_clk_unlock(&reg_lock);
}

static unsigned int get_id_from_rcc_bit(unsigned int offset, unsigned int bit)
{
	unsigned int idx;

	for (idx = 0U; idx < NB_GATES; idx++) {
		const struct stm32mp1_clk_gate *gate = gate_ref(idx);

		if ((offset == gate->offset) && (bit == gate->bit)) {
			return gate->index;
		}

		if ((gate->set_clr != 0U) &&
		    (offset == (gate->offset + RCC_MP_ENCLRR_OFFSET)) &&
		    (bit == gate->bit)) {
			return gate->index;
		}
	}

	/* Currently only supported gated clocks */
	return ~0U;
}

static unsigned long stm32mp1_clk_get_fixed(enum stm32mp_osc_id idx)
{
	if (idx >= NB_OSC) {
		return 0;
	}

	return stm32mp1_osc[idx];
}

static int stm32mp1_clk_get_gated_id(unsigned long id)
{
	unsigned int i;

	for (i = 0U; i < NB_GATES; i++) {
		if (gate_ref(i)->index == id) {
			return i;
		}
	}

	ERROR("%s: clk id %d not found\n", __func__, (uint32_t)id);

	return -EINVAL;
}

static enum stm32mp1_parent_sel stm32mp1_clk_get_sel(int i)
{
	return (enum stm32mp1_parent_sel)(gate_ref(i)->sel);
}

static enum stm32mp1_parent_id stm32mp1_clk_get_fixed_parent(int i)
{
	return (enum stm32mp1_parent_id)(gate_ref(i)->fixed);
}

static int stm32mp1_clk_get_parent(unsigned long id)
{
	const struct stm32mp1_clk_sel *sel;
	uint32_t p_sel;
	int i;
	enum stm32mp1_parent_id p;
	enum stm32mp1_parent_sel s;
	uintptr_t rcc_base = stm32mp_rcc_base();

	/* Few non gateable clock have a static parent ID, find them */
	i = (int)clock_id2parent_id(id);
	if (i != _UNKNOWN_ID)
		return i;

	i = stm32mp1_clk_get_gated_id(id);
	if (i < 0) {
		panic();
	}

	p = stm32mp1_clk_get_fixed_parent(i);
	if (p < _PARENT_NB) {
		return (int)p;
	}

	s = stm32mp1_clk_get_sel(i);
	if (s == _UNKNOWN_SEL) {
		return -EINVAL;
	}
	if (s >= _PARENT_SEL_NB) {
		panic();
	}

	sel = clk_sel_ref(s);
	p_sel = (mmio_read_32(rcc_base + sel->offset) &
		 (sel->msk << sel->src)) >> sel->src;
	if (p_sel < sel->nb_parent) {
#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
		VERBOSE("%s: %s clock is the parent %s of clk id %ld\n",
			__func__,
			stm32mp1_clk_parent_name[sel->parent[p_sel]],
			stm32mp1_clk_parent_sel_name[s], id);
#endif
		return (int)sel->parent[p_sel];
	}

	return -EINVAL;
}

static unsigned long stm32mp1_pll_get_fref(const struct stm32mp1_clk_pll *pll)
{
	uint32_t selr = mmio_read_32(stm32mp_rcc_base() + pll->rckxselr);
	uint32_t src = selr & RCC_SELR_REFCLK_SRC_MASK;

	return stm32mp1_clk_get_fixed(pll->refclk[src]);
}

/*
 * pll_get_fvco() : return the VCO or (VCO / 2) frequency for the requested PLL
 * - PLL1 & PLL2 => return VCO / 2 with Fpll_y_ck = FVCO / 2 * (DIVy + 1)
 * - PLL3 & PLL4 => return VCO     with Fpll_y_ck = FVCO / (DIVy + 1)
 * => in all cases Fpll_y_ck = pll_get_fvco() / (DIVy + 1)
 */
static unsigned long stm32mp1_pll_get_fvco(const struct stm32mp1_clk_pll *pll)
{
	unsigned long refclk, fvco;
	uint32_t cfgr1, fracr, divm, divn;
	uintptr_t rcc_base = stm32mp_rcc_base();

	cfgr1 = mmio_read_32(rcc_base + pll->pllxcfgr1);
	fracr = mmio_read_32(rcc_base + pll->pllxfracr);

	divm = (cfgr1 & (RCC_PLLNCFGR1_DIVM_MASK)) >> RCC_PLLNCFGR1_DIVM_SHIFT;
	divn = cfgr1 & RCC_PLLNCFGR1_DIVN_MASK;

	refclk = stm32mp1_pll_get_fref(pll);

	/*
	 * With FRACV :
	 *   Fvco = Fck_ref * ((DIVN + 1) + FRACV / 2^13) / (DIVM + 1)
	 * Without FRACV
	 *   Fvco = Fck_ref * ((DIVN + 1) / (DIVM + 1)
	 */
	if ((fracr & RCC_PLLNFRACR_FRACLE) != 0U) {
		uint32_t fracv = (fracr & RCC_PLLNFRACR_FRACV_MASK) >>
				 RCC_PLLNFRACR_FRACV_SHIFT;
		unsigned long long numerator, denominator;

		numerator = (((unsigned long long)divn + 1U) << 13) + fracv;
		numerator = refclk * numerator;
		denominator = ((unsigned long long)divm + 1U) << 13;
		fvco = (unsigned long)(numerator / denominator);
	} else {
		fvco = (unsigned long)(refclk * (divn + 1U) / (divm + 1U));
	}

	return fvco;
}

static unsigned long stm32mp1_read_pll_freq(enum stm32mp1_pll_id pll_id,
					    enum stm32mp1_div_id div_id)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	unsigned long dfout;
	uint32_t cfgr2, divy;

	if (div_id >= _DIV_NB) {
		return 0;
	}

	cfgr2 = mmio_read_32(stm32mp_rcc_base() + pll->pllxcfgr2);
	divy = (cfgr2 >> pllncfgr2[div_id]) & RCC_PLLNCFGR2_DIVX_MASK;

	dfout = stm32mp1_pll_get_fvco(pll) / (divy + 1U);

	return dfout;
}

static unsigned long get_clock_rate(int p)
{
	uint32_t reg, clkdiv;
	unsigned long clock = 0;
	uintptr_t rcc_base = stm32mp_rcc_base();

	switch (p) {
	case _CK_MPU:
	/* MPU sub system */
		reg = mmio_read_32(rcc_base + RCC_MPCKSELR);
		switch (reg & RCC_SELR_SRC_MASK) {
		case RCC_MPCKSELR_HSI:
			clock = stm32mp1_clk_get_fixed(_HSI);
			break;
		case RCC_MPCKSELR_HSE:
			clock = stm32mp1_clk_get_fixed(_HSE);
			break;
		case RCC_MPCKSELR_PLL:
			clock = stm32mp1_read_pll_freq(_PLL1, _DIV_P);
			break;
		case RCC_MPCKSELR_PLL_MPUDIV:
			clock = stm32mp1_read_pll_freq(_PLL1, _DIV_P);

			reg = mmio_read_32(rcc_base + RCC_MPCKDIVR);
			clkdiv = reg & RCC_MPUDIV_MASK;
			clock >>= stm32mp1_mpu_div[clkdiv];
			break;
		default:
			break;
		}
		break;
	/* AXI sub system */
	case _ACLK:
	case _HCLK2:
	case _HCLK6:
	case _PCLK4:
	case _PCLK5:
		reg = mmio_read_32(rcc_base + RCC_ASSCKSELR);
		switch (reg & RCC_SELR_SRC_MASK) {
		case RCC_ASSCKSELR_HSI:
			clock = stm32mp1_clk_get_fixed(_HSI);
			break;
		case RCC_ASSCKSELR_HSE:
			clock = stm32mp1_clk_get_fixed(_HSE);
			break;
		case RCC_ASSCKSELR_PLL:
			clock = stm32mp1_read_pll_freq(_PLL2, _DIV_P);
			break;
		default:
			break;
		}

		/* System clock divider */
		reg = mmio_read_32(rcc_base + RCC_AXIDIVR);
		clock /= stm32mp1_axi_div[reg & RCC_AXIDIV_MASK];

		switch (p) {
		case _PCLK4:
			reg = mmio_read_32(rcc_base + RCC_APB4DIVR);
			clock >>= stm32mp1_apbx_div[reg & RCC_APBXDIV_MASK];
			break;
		case _PCLK5:
			reg = mmio_read_32(rcc_base + RCC_APB5DIVR);
			clock >>= stm32mp1_apbx_div[reg & RCC_APBXDIV_MASK];
			break;
		default:
			break;
		}
		break;
	/* MCU sub system */
	case _CK_MCU:
	case _PCLK1:
	case _PCLK2:
	case _PCLK3:
		reg = mmio_read_32(rcc_base + RCC_MSSCKSELR);
		switch (reg & RCC_SELR_SRC_MASK) {
		case RCC_MSSCKSELR_HSI:
			clock = stm32mp1_clk_get_fixed(_HSI);
			break;
		case RCC_MSSCKSELR_HSE:
			clock = stm32mp1_clk_get_fixed(_HSE);
			break;
		case RCC_MSSCKSELR_CSI:
			clock = stm32mp1_clk_get_fixed(_CSI);
			break;
		case RCC_MSSCKSELR_PLL:
			clock = stm32mp1_read_pll_freq(_PLL3, _DIV_P);
			break;
		default:
			break;
		}

		/* MCU clock divider */
		reg = mmio_read_32(rcc_base + RCC_MCUDIVR);
		clock >>= stm32mp1_mcu_div[reg & RCC_MCUDIV_MASK];

		switch (p) {
		case _PCLK1:
			reg = mmio_read_32(rcc_base + RCC_APB1DIVR);
			clock >>= stm32mp1_apbx_div[reg & RCC_APBXDIV_MASK];
			break;
		case _PCLK2:
			reg = mmio_read_32(rcc_base + RCC_APB2DIVR);
			clock >>= stm32mp1_apbx_div[reg & RCC_APBXDIV_MASK];
			break;
		case _PCLK3:
			reg = mmio_read_32(rcc_base + RCC_APB3DIVR);
			clock >>= stm32mp1_apbx_div[reg & RCC_APBXDIV_MASK];
			break;
		case _CK_MCU:
		default:
			break;
		}
		break;
	case _CK_PER:
		reg = mmio_read_32(rcc_base + RCC_CPERCKSELR);
		switch (reg & RCC_SELR_SRC_MASK) {
		case RCC_CPERCKSELR_HSI:
			clock = stm32mp1_clk_get_fixed(_HSI);
			break;
		case RCC_CPERCKSELR_HSE:
			clock = stm32mp1_clk_get_fixed(_HSE);
			break;
		case RCC_CPERCKSELR_CSI:
			clock = stm32mp1_clk_get_fixed(_CSI);
			break;
		default:
			break;
		}
		break;
	case _HSI:
	case _HSI_KER:
		clock = stm32mp1_clk_get_fixed(_HSI);
		break;
	case _CSI:
	case _CSI_KER:
		clock = stm32mp1_clk_get_fixed(_CSI);
		break;
	case _HSE:
	case _HSE_KER:
		clock = stm32mp1_clk_get_fixed(_HSE);
		break;
	case _HSE_KER_DIV2:
		clock = stm32mp1_clk_get_fixed(_HSE) >> 1;
		break;
	case _LSI:
		clock = stm32mp1_clk_get_fixed(_LSI);
		break;
	case _LSE:
		clock = stm32mp1_clk_get_fixed(_LSE);
		break;
	/* PLL */
	case _PLL1_P:
		clock = stm32mp1_read_pll_freq(_PLL1, _DIV_P);
		break;
	case _PLL1_Q:
		clock = stm32mp1_read_pll_freq(_PLL1, _DIV_Q);
		break;
	case _PLL1_R:
		clock = stm32mp1_read_pll_freq(_PLL1, _DIV_R);
		break;
	case _PLL2_P:
		clock = stm32mp1_read_pll_freq(_PLL2, _DIV_P);
		break;
	case _PLL2_Q:
		clock = stm32mp1_read_pll_freq(_PLL2, _DIV_Q);
		break;
	case _PLL2_R:
		clock = stm32mp1_read_pll_freq(_PLL2, _DIV_R);
		break;
	case _PLL3_P:
		clock = stm32mp1_read_pll_freq(_PLL3, _DIV_P);
		break;
	case _PLL3_Q:
		clock = stm32mp1_read_pll_freq(_PLL3, _DIV_Q);
		break;
	case _PLL3_R:
		clock = stm32mp1_read_pll_freq(_PLL3, _DIV_R);
		break;
	case _PLL4_P:
		clock = stm32mp1_read_pll_freq(_PLL4, _DIV_P);
		break;
	case _PLL4_Q:
		clock = stm32mp1_read_pll_freq(_PLL4, _DIV_Q);
		break;
	case _PLL4_R:
		clock = stm32mp1_read_pll_freq(_PLL4, _DIV_R);
		break;
	/* Other */
	case _USB_PHY_48:
		clock = USB_PHY_48_MHZ;
		break;
	default:
		break;
	}

	return clock;
}

static void __clk_enable(struct stm32mp1_clk_gate const *gate)
{
	uintptr_t rcc_base = stm32mp_rcc_base();

	VERBOSE("Enable clock %d\n", gate->index);

	if (gate->set_clr != 0U) {
		mmio_write_32(rcc_base + gate->offset, BIT(gate->bit));
	} else {
		stm32mp_mmio_setbits_32_shregs(rcc_base + gate->offset,
					       BIT(gate->bit));
	}
}

static void __clk_disable(struct stm32mp1_clk_gate const *gate)
{
	uintptr_t rcc_base = stm32mp_rcc_base();

	VERBOSE("Disable clock %d\n", gate->index);

	if (gate->set_clr != 0U) {
		mmio_write_32(rcc_base + gate->offset + RCC_MP_ENCLRR_OFFSET,
			      BIT(gate->bit));
	} else {
		stm32mp_mmio_clrbits_32_shregs(rcc_base + gate->offset,
					       BIT(gate->bit));
	}
}

static bool __clk_is_enabled(struct stm32mp1_clk_gate const *gate)
{
	uintptr_t rcc_base = stm32mp_rcc_base();

	return mmio_read_32(rcc_base + gate->offset) & BIT(gate->bit);
}

/* Oscillators and PLLs are not gated at runtime */
static bool clock_is_always_on(unsigned long id)
{
	CASSERT((CK_HSE == 0) &&
		((CK_HSE + 1) == CK_CSI) &&
		((CK_HSE + 2) == CK_LSI) &&
		((CK_HSE + 3) == CK_LSE) &&
		((CK_HSE + 4) == CK_HSI) &&
		((CK_HSE + 5) == CK_HSE_DIV2) &&
		((PLL1_P + 1) == PLL1_Q) &&
		((PLL1_P + 2) == PLL1_R) &&
		((PLL1_P + 3) == PLL2_P) &&
		((PLL1_P + 4) == PLL2_Q) &&
		((PLL1_P + 5) == PLL2_R) &&
		((PLL1_P + 6) == PLL3_P) &&
		((PLL1_P + 7) == PLL3_Q) &&
		((PLL1_P + 8) == PLL3_R),
		assert_osc_and_pll_ids_are_contiguous);

	if ((id <= CK_HSE_DIV2) || ((id >= PLL1_P) && (id <= PLL3_R)))
		return true;

	switch (id) {
	case CK_AXI:
	case CK_MPU:
	case CK_MCU:
	case RTC:
		return true;
	default:
		return false;
	}
}

static void __stm32mp1_clk_enable(unsigned long id, bool with_refcnt)
{
	const struct stm32mp1_clk_gate *gate;
	int i;

	if (clock_is_always_on(id)) {
		return;
	}

	i = stm32mp1_clk_get_gated_id(id);
	if (i < 0) {
		ERROR("Clock %d can't be enabled\n", (uint32_t)id);
		panic();
	}

	gate = gate_ref(i);

	if (!with_refcnt) {
		__clk_enable(gate);
		return;
	}

#if defined(IMAGE_BL32)
	if (gate_is_non_secure(gate)) {
		/* Enable non-secure clock w/o any refcounting */
		__clk_enable(gate);
		return;
	}
#endif

	stm32mp1_clk_lock(&refcount_lock);

	if (gate_refcounts[i] == 0) {
		__clk_enable(gate);
	}

	gate_refcounts[i]++;
	if (gate_refcounts[i] == UINT_MAX) {
		panic();
	}

	stm32mp1_clk_unlock(&refcount_lock);
}

static void __stm32mp1_clk_disable(unsigned long id, bool with_refcnt)
{
	const struct stm32mp1_clk_gate *gate;
	int i;

	if (clock_is_always_on(id)) {
		return;
	}

	i = stm32mp1_clk_get_gated_id(id);
	if (i < 0) {
		ERROR("Clock %d can't be disabled\n", (uint32_t)id);
		panic();
	}

	gate = gate_ref(i);

	if (!with_refcnt) {
		__clk_disable(gate);
		return;
	}

#if defined(IMAGE_BL32)
	if (gate_is_non_secure(gate)) {
		/* Don't disable non-secure clocks */
		return;
	}
#endif

	stm32mp1_clk_lock(&refcount_lock);

	if (gate_refcounts[i] == 0) {
		panic();
	}
	gate_refcounts[i]--;

	if (gate_refcounts[i] == 0) {
		__clk_disable(gate);
	}

	stm32mp1_clk_unlock(&refcount_lock);
}

void stm32mp_clk_enable(unsigned long id)
{
	__stm32mp1_clk_enable(id, true);
}

void stm32mp_clk_disable(unsigned long id)
{
	__stm32mp1_clk_disable(id, true);
}

void stm32mp1_clk_force_enable(unsigned long id)
{
	__stm32mp1_clk_enable(id, false);
}

void stm32mp1_clk_force_disable(unsigned long id)
{
	__stm32mp1_clk_disable(id, false);
}

bool stm32mp_clk_is_enabled(unsigned long id)
{
	int i;

	if (clock_is_always_on(id)) {
		return true;
	}

	i = stm32mp1_clk_get_gated_id(id);
	if (i < 0) {
		panic();
	}

	return __clk_is_enabled(gate_ref(i));
}

unsigned long stm32mp_clk_get_rate(unsigned long id)
{
	int p = stm32mp1_clk_get_parent(id);

	if (p < 0) {
		return 0;
	}

	return get_clock_rate(p);
}

static void stm32mp1_ls_osc_set(bool enable, uint32_t offset, uint32_t mask_on)
{
	uintptr_t address = stm32mp_rcc_base() + offset;

	if (enable) {
		mmio_setbits_32(address, mask_on);
	} else {
		mmio_clrbits_32(address, mask_on);
	}
}

static void stm32mp1_hs_ocs_set(bool enable, uint32_t mask_on)
{
	uint32_t offset = enable ? RCC_OCENSETR : RCC_OCENCLRR;
	uintptr_t address = stm32mp_rcc_base() + offset;

	mmio_write_32(address, mask_on);
}

static int stm32mp1_osc_wait(bool enable, uint32_t offset, uint32_t mask_rdy)
{
	uint64_t timeout;
	uint32_t mask_test;
	uintptr_t address = stm32mp_rcc_base() + offset;

	if (enable) {
		mask_test = mask_rdy;
	} else {
		mask_test = 0;
	}

	timeout = timeout_init_us(OSCRDY_TIMEOUT);
	while ((mmio_read_32(address) & mask_rdy) != mask_test) {
		if (timeout_elapsed(timeout)) {
			ERROR("OSC %x @ %lx timeout for enable=%d : 0x%x\n",
			      mask_rdy, address, enable, mmio_read_32(address));
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static void stm32mp1_lse_enable(bool bypass, bool digbyp, uint32_t lsedrv)
{
	uint32_t value;
	uintptr_t rcc_base = stm32mp_rcc_base();

	if (digbyp) {
		mmio_setbits_32(rcc_base + RCC_BDCR, RCC_BDCR_DIGBYP);
	}

	if (bypass || digbyp) {
		mmio_setbits_32(rcc_base + RCC_BDCR, RCC_BDCR_LSEBYP);
	}

	/*
	 * Warning: not recommended to switch directly from "high drive"
	 * to "medium low drive", and vice-versa.
	 */
	value = (mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_LSEDRV_MASK) >>
		RCC_BDCR_LSEDRV_SHIFT;

	while (value != lsedrv) {
		if (value > lsedrv) {
			value--;
		} else {
			value++;
		}

		mmio_clrsetbits_32(rcc_base + RCC_BDCR,
				   RCC_BDCR_LSEDRV_MASK,
				   value << RCC_BDCR_LSEDRV_SHIFT);
	}

	stm32mp1_ls_osc_set(true, RCC_BDCR, RCC_BDCR_LSEON);
}

static void stm32mp1_lse_wait(void)
{
	if (stm32mp1_osc_wait(true, RCC_BDCR, RCC_BDCR_LSERDY) != 0) {
		VERBOSE("%s: failed\n", __func__);
	}
}

static void stm32mp1_lsi_set(bool enable)
{
	stm32mp1_ls_osc_set(enable, RCC_RDLSICR, RCC_RDLSICR_LSION);

	if (stm32mp1_osc_wait(enable, RCC_RDLSICR, RCC_RDLSICR_LSIRDY) != 0) {
		VERBOSE("%s: failed\n", __func__);
	}
}

static void stm32mp1_hse_enable(bool bypass, bool digbyp, bool css)
{
	uintptr_t rcc_base = stm32mp_rcc_base();

	if (digbyp) {
		mmio_write_32(rcc_base + RCC_OCENSETR, RCC_OCENR_DIGBYP);
	}

	if (bypass || digbyp) {
		mmio_write_32(rcc_base + RCC_OCENSETR, RCC_OCENR_HSEBYP);
	}

	stm32mp1_hs_ocs_set(true, RCC_OCENR_HSEON);
	if (stm32mp1_osc_wait(true, RCC_OCRDYR, RCC_OCRDYR_HSERDY) != 0) {
		VERBOSE("%s: failed\n", __func__);
	}

	if (css) {
		mmio_write_32(rcc_base + RCC_OCENSETR, RCC_OCENR_HSECSSON);
	}

#if defined(STM32MP_USB) || defined(STM32MP_UART)
	if ((mmio_read_32(rcc_base + RCC_OCENSETR) & RCC_OCENR_HSEBYP) &&
	    (!(digbyp || bypass))) {
		panic();
	}
#endif
}

static void stm32mp1_csi_set(bool enable)
{
	stm32mp1_hs_ocs_set(enable, RCC_OCENR_CSION);
	if (stm32mp1_osc_wait(enable, RCC_OCRDYR, RCC_OCRDYR_CSIRDY) != 0) {
		VERBOSE("%s: failed\n", __func__);
	}
}

static void stm32mp1_hsi_set(bool enable)
{
	stm32mp1_hs_ocs_set(enable, RCC_OCENR_HSION);
	if (stm32mp1_osc_wait(enable, RCC_OCRDYR, RCC_OCRDYR_HSIRDY) != 0) {
		VERBOSE("%s: failed\n", __func__);
	}
}

static int stm32mp1_set_hsidiv(uint8_t hsidiv)
{
	uint64_t timeout;
	uintptr_t rcc_base = stm32mp_rcc_base();
	uintptr_t address = rcc_base + RCC_OCRDYR;

	mmio_clrsetbits_32(rcc_base + RCC_HSICFGR,
			   RCC_HSICFGR_HSIDIV_MASK,
			   RCC_HSICFGR_HSIDIV_MASK & (uint32_t)hsidiv);

	timeout = timeout_init_us(HSIDIV_TIMEOUT);
	while ((mmio_read_32(address) & RCC_OCRDYR_HSIDIVRDY) == 0U) {
		if (timeout_elapsed(timeout)) {
			ERROR("HSIDIV failed @ 0x%lx: 0x%x\n",
			      address, mmio_read_32(address));
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int stm32mp1_hsidiv(unsigned long hsifreq)
{
	uint8_t hsidiv;
	uint32_t hsidivfreq = MAX_HSI_HZ;

	for (hsidiv = 0; hsidiv < 4U; hsidiv++) {
		if (hsidivfreq == hsifreq) {
			break;
		}

		hsidivfreq /= 2U;
	}

	if (hsidiv == 4U) {
		ERROR("Invalid clk-hsi frequency\n");
		return -1;
	}

	if (hsidiv != 0U) {
		return stm32mp1_set_hsidiv(hsidiv);
	}

	return 0;
}

static bool stm32mp1_check_pll_conf(enum stm32mp1_pll_id pll_id,
				    unsigned int clksrc,
				    uint32_t *pllcfg, int plloff)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t rcc_base = stm32mp_rcc_base();
	uintptr_t pllxcr = rcc_base + pll->pllxcr;
	enum stm32mp1_plltype type = pll->plltype;
	uintptr_t clksrc_address = rcc_base + (clksrc >> 4);
	unsigned long refclk;
	uint32_t ifrge = 0U;
	uint32_t src, value, fracv;

	/* Check PLL output */
	if (mmio_read_32(pllxcr) != RCC_PLLNCR_PLLON) {
		return false;
	}

	/* Check current clksrc */
	src = mmio_read_32(clksrc_address) & RCC_SELR_SRC_MASK;
	if (src != (clksrc & RCC_SELR_SRC_MASK)) {
		return false;
	}

	/* Check Div */
	src = mmio_read_32(rcc_base + pll->rckxselr) & RCC_SELR_REFCLK_SRC_MASK;

	refclk = stm32mp1_clk_get_fixed(pll->refclk[src]) /
		 (pllcfg[PLLCFG_M] + 1U);

	if ((refclk < (stm32mp1_pll[type].refclk_min * 1000000U)) ||
	    (refclk > (stm32mp1_pll[type].refclk_max * 1000000U))) {
		return false;
	}

	if ((type == PLL_800) && (refclk >= 8000000U)) {
		ifrge = 1U;
	}

	value = (pllcfg[PLLCFG_N] << RCC_PLLNCFGR1_DIVN_SHIFT) &
		RCC_PLLNCFGR1_DIVN_MASK;
	value |= (pllcfg[PLLCFG_M] << RCC_PLLNCFGR1_DIVM_SHIFT) &
		 RCC_PLLNCFGR1_DIVM_MASK;
	value |= (ifrge << RCC_PLLNCFGR1_IFRGE_SHIFT) &
		 RCC_PLLNCFGR1_IFRGE_MASK;
	if (mmio_read_32(rcc_base + pll->pllxcfgr1) != value) {
		return false;
	}

	/* Fractional configuration */
	fracv = fdt_read_uint32_default(plloff, "frac", 0);

	value = fracv << RCC_PLLNFRACR_FRACV_SHIFT;
	value |= RCC_PLLNFRACR_FRACLE;
	if (mmio_read_32(rcc_base + pll->pllxfracr) != value) {
		return false;
	}

	/* Output config */
	value = (pllcfg[PLLCFG_P] << RCC_PLLNCFGR2_DIVP_SHIFT) &
		RCC_PLLNCFGR2_DIVP_MASK;
	value |= (pllcfg[PLLCFG_Q] << RCC_PLLNCFGR2_DIVQ_SHIFT) &
		 RCC_PLLNCFGR2_DIVQ_MASK;
	value |= (pllcfg[PLLCFG_R] << RCC_PLLNCFGR2_DIVR_SHIFT) &
		 RCC_PLLNCFGR2_DIVR_MASK;
	if (mmio_read_32(rcc_base + pll->pllxcfgr2) != value) {
		return false;
	}

	return true;
}

static void stm32mp1_pll_start(enum stm32mp1_pll_id pll_id)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t pllxcr = stm32mp_rcc_base() + pll->pllxcr;

	/* Preserve RCC_PLLNCR_SSCG_CTRL value */
	mmio_clrsetbits_32(pllxcr,
			   RCC_PLLNCR_DIVPEN | RCC_PLLNCR_DIVQEN |
			   RCC_PLLNCR_DIVREN,
			   RCC_PLLNCR_PLLON);
}

static int stm32mp1_pll_output(enum stm32mp1_pll_id pll_id, uint32_t output)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t pllxcr = stm32mp_rcc_base() + pll->pllxcr;
	uint64_t timeout = timeout_init_us(PLLRDY_TIMEOUT);

	/* Wait PLL lock */
	while ((mmio_read_32(pllxcr) & RCC_PLLNCR_PLLRDY) == 0U) {
		if (timeout_elapsed(timeout)) {
			ERROR("PLL%d start failed @ 0x%lx: 0x%x\n",
			      pll_id, pllxcr, mmio_read_32(pllxcr));
			return -ETIMEDOUT;
		}
	}

	/* Start the requested output */
	mmio_setbits_32(pllxcr, output << RCC_PLLNCR_DIVEN_SHIFT);

	return 0;
}

static int stm32mp1_pll_stop(enum stm32mp1_pll_id pll_id)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t pllxcr = stm32mp_rcc_base() + pll->pllxcr;
	uint64_t timeout;

	/* Stop all output */
	mmio_clrbits_32(pllxcr, RCC_PLLNCR_DIVPEN | RCC_PLLNCR_DIVQEN |
			RCC_PLLNCR_DIVREN);

	/* Stop PLL */
	mmio_clrbits_32(pllxcr, RCC_PLLNCR_PLLON);

	timeout = timeout_init_us(PLLRDY_TIMEOUT);
	/* Wait PLL stopped */
	while ((mmio_read_32(pllxcr) & RCC_PLLNCR_PLLRDY) != 0U) {
		if (timeout_elapsed(timeout)) {
			ERROR("PLL%d stop failed @ 0x%lx: 0x%x\n",
			      pll_id, pllxcr, mmio_read_32(pllxcr));
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static uint32_t stm32mp1_pll_compute_pllxcfgr2(uint32_t *pllcfg)
{
	uint32_t value;

	value = (pllcfg[PLLCFG_P] << RCC_PLLNCFGR2_DIVP_SHIFT) &
		RCC_PLLNCFGR2_DIVP_MASK;
	value |= (pllcfg[PLLCFG_Q] << RCC_PLLNCFGR2_DIVQ_SHIFT) &
		 RCC_PLLNCFGR2_DIVQ_MASK;
	value |= (pllcfg[PLLCFG_R] << RCC_PLLNCFGR2_DIVR_SHIFT) &
		 RCC_PLLNCFGR2_DIVR_MASK;

	return value;
}

static void stm32mp1_pll_config_output(enum stm32mp1_pll_id pll_id,
				       uint32_t *pllcfg)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t value;

	value = stm32mp1_pll_compute_pllxcfgr2(pllcfg);

	mmio_write_32(rcc_base + pll->pllxcfgr2, value);
}

static int stm32mp1_pll_compute_pllxcfgr1(const struct stm32mp1_clk_pll *pll,
					  uint32_t *pllcfg, uint32_t *cfgr1)
{
	uintptr_t rcc_base = stm32mp_rcc_base();
	enum stm32mp1_plltype type = pll->plltype;
	unsigned long refclk;
	uint32_t ifrge = 0;
	uint32_t src;

	src = mmio_read_32(rcc_base + pll->rckxselr) &
	      RCC_SELR_REFCLK_SRC_MASK;

	refclk = stm32mp1_clk_get_fixed(pll->refclk[src]) /
		 (pllcfg[PLLCFG_M] + 1U);

	if ((refclk < (stm32mp1_pll[type].refclk_min * 1000000U)) ||
	    (refclk > (stm32mp1_pll[type].refclk_max * 1000000U))) {
		return -EINVAL;
	}

	if ((type == PLL_800) && (refclk >= 8000000U)) {
		ifrge = 1U;
	}

	*cfgr1 = (pllcfg[PLLCFG_N] << RCC_PLLNCFGR1_DIVN_SHIFT) &
		 RCC_PLLNCFGR1_DIVN_MASK;
	*cfgr1 |= (pllcfg[PLLCFG_M] << RCC_PLLNCFGR1_DIVM_SHIFT) &
		  RCC_PLLNCFGR1_DIVM_MASK;
	*cfgr1 |= (ifrge << RCC_PLLNCFGR1_IFRGE_SHIFT) &
		  RCC_PLLNCFGR1_IFRGE_MASK;

	return 0;
}

static int stm32mp1_pll_config(enum stm32mp1_pll_id pll_id,
			       uint32_t *pllcfg, uint32_t fracv)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t value;
	int ret;

	ret = stm32mp1_pll_compute_pllxcfgr1(pll, pllcfg, &value);
	if (ret != 0) {
		return ret;
	}

	mmio_write_32(rcc_base + pll->pllxcfgr1, value);

	/* Fractional configuration */
	value = 0;
	mmio_write_32(rcc_base + pll->pllxfracr, value);

	/*  Frac must be enabled only once its configuration is loaded */
	value = fracv << RCC_PLLNFRACR_FRACV_SHIFT;
	mmio_write_32(rcc_base + pll->pllxfracr, value);
	mmio_setbits_32(rcc_base + pll->pllxfracr, RCC_PLLNFRACR_FRACLE);

	stm32mp1_pll_config_output(pll_id, pllcfg);

	return 0;
}

static void stm32mp1_pll_csg(enum stm32mp1_pll_id pll_id, uint32_t *csg)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uint32_t pllxcsg = 0;

	pllxcsg |= (csg[PLLCSG_MOD_PER] << RCC_PLLNCSGR_MOD_PER_SHIFT) &
		    RCC_PLLNCSGR_MOD_PER_MASK;

	pllxcsg |= (csg[PLLCSG_INC_STEP] << RCC_PLLNCSGR_INC_STEP_SHIFT) &
		    RCC_PLLNCSGR_INC_STEP_MASK;

	pllxcsg |= (csg[PLLCSG_SSCG_MODE] << RCC_PLLNCSGR_SSCG_MODE_SHIFT) &
		    RCC_PLLNCSGR_SSCG_MODE_MASK;

	mmio_write_32(stm32mp_rcc_base() + pll->pllxcsgr, pllxcsg);

	mmio_setbits_32(stm32mp_rcc_base() + pll->pllxcr,
			RCC_PLLNCR_SSCG_CTRL);
}

static int stm32mp1_set_clksrc(unsigned int clksrc)
{
	uintptr_t clksrc_address = stm32mp_rcc_base() + (clksrc >> 4);
	uint64_t timeout;

	mmio_clrsetbits_32(clksrc_address, RCC_SELR_SRC_MASK,
			   clksrc & RCC_SELR_SRC_MASK);

	timeout = timeout_init_us(CLKSRC_TIMEOUT);
	while ((mmio_read_32(clksrc_address) & RCC_SELR_SRCRDY) == 0U) {
		if (timeout_elapsed(timeout)) {
			ERROR("CLKSRC %x start failed @ 0x%lx: 0x%x\n", clksrc,
			      clksrc_address, mmio_read_32(clksrc_address));
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int stm32mp1_set_clkdiv(unsigned int clkdiv, uintptr_t address)
{
	uint64_t timeout;

	mmio_clrsetbits_32(address, RCC_DIVR_DIV_MASK,
			   clkdiv & RCC_DIVR_DIV_MASK);

	timeout = timeout_init_us(CLKDIV_TIMEOUT);
	while ((mmio_read_32(address) & RCC_DIVR_DIVRDY) == 0U) {
		if (timeout_elapsed(timeout)) {
			ERROR("CLKDIV %x start failed @ 0x%lx: 0x%x\n",
			      clkdiv, address, mmio_read_32(address));
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static void stm32mp1_mco_csg(uint32_t clksrc, uint32_t clkdiv)
{
	uintptr_t clksrc_address = stm32mp_rcc_base() + (clksrc >> 4);

	/*
	 * Binding clksrc :
	 *      bit15-4 offset
	 *      bit3:   disable
	 *      bit2-0: MCOSEL[2:0]
	 */
	if ((clksrc & 0x8U) != 0U) {
		mmio_clrbits_32(clksrc_address, RCC_MCOCFG_MCOON);
	} else {
		mmio_clrsetbits_32(clksrc_address,
				   RCC_MCOCFG_MCOSRC_MASK,
				   clksrc & RCC_MCOCFG_MCOSRC_MASK);
		mmio_clrsetbits_32(clksrc_address,
				   RCC_MCOCFG_MCODIV_MASK,
				   clkdiv << RCC_MCOCFG_MCODIV_SHIFT);
		mmio_setbits_32(clksrc_address, RCC_MCOCFG_MCOON);
	}
}

static void stm32mp1_set_rtcsrc(unsigned int clksrc, bool lse_css)
{
	uintptr_t address = stm32mp_rcc_base() + RCC_BDCR;

	if (((mmio_read_32(address) & RCC_BDCR_RTCCKEN) == 0U) ||
	    (clksrc != (uint32_t)CLK_RTC_DISABLED)) {
		mmio_clrsetbits_32(address,
				   RCC_BDCR_RTCSRC_MASK,
				   (clksrc & RCC_SELR_SRC_MASK) <<
				   RCC_BDCR_RTCSRC_SHIFT);

		mmio_setbits_32(address, RCC_BDCR_RTCCKEN);
	}

	if (lse_css) {
		mmio_setbits_32(address, RCC_BDCR_LSECSSON);
	}
}

unsigned long stm32mp_clk_timer_get_rate(unsigned long id)
{
	unsigned long parent_rate;
	uint32_t prescaler, timpre;
	uintptr_t rcc_base = stm32mp_rcc_base();

	parent_rate = stm32mp_clk_get_rate(id);

	if (id < TIM1_K) {
		prescaler = mmio_read_32(rcc_base + RCC_APB1DIVR) &
			    RCC_APBXDIV_MASK;
		timpre = mmio_read_32(rcc_base + RCC_TIMG1PRER) &
			 RCC_TIMGXPRER_TIMGXPRE;
	} else {
		prescaler = mmio_read_32(rcc_base + RCC_APB2DIVR) &
			    RCC_APBXDIV_MASK;
		timpre = mmio_read_32(rcc_base + RCC_TIMG2PRER) &
			 RCC_TIMGXPRER_TIMGXPRE;
	}

	if (!prescaler) {
		return parent_rate;
	}

	return parent_rate * (timpre + 1) * 2;
}

/*******************************************************************************
 * This function determines the number of needed RTC calendar read operations
 * to get consistent values (1 or 2 depending on clock frequencies).
 * If APB1 frequency is lower than 7 times the RTC one, the software has to
 * read the calendar time and date registers twice.
 * Returns true if read twice is needed, false else.
 ******************************************************************************/
bool stm32mp1_rtc_get_read_twice(void)
{
	unsigned long apb1_freq;
	uint32_t rtc_freq;
	uint32_t apb1_div;
	uintptr_t rcc_base = stm32mp_rcc_base();

	switch ((mmio_read_32(rcc_base + RCC_BDCR) &
		 RCC_BDCR_RTCSRC_MASK) >> RCC_BDCR_RTCSRC_SHIFT) {
	case 1:
		rtc_freq = stm32mp_clk_get_rate(CK_LSE);
		break;
	case 2:
		rtc_freq = stm32mp_clk_get_rate(CK_LSI);
		break;
	case 3:
		rtc_freq = stm32mp_clk_get_rate(CK_HSE);
		rtc_freq /= (mmio_read_32(rcc_base + RCC_RTCDIVR) &
			     RCC_DIVR_DIV_MASK) + 1U;
		break;
	default:
		panic();
	}

	apb1_div = mmio_read_32(rcc_base + RCC_APB1DIVR) & RCC_APBXDIV_MASK;
	apb1_freq = stm32mp_clk_get_rate(CK_MCU) >> apb1_div;

	return apb1_freq < (rtc_freq * 7U);
}

static void stm32mp1_pkcs_config(uint32_t pkcs)
{
	uintptr_t address = stm32mp_rcc_base() + ((pkcs >> 4) & 0xFFFU);
	uint32_t value = pkcs & 0xFU;
	uint32_t mask = 0xFU;

	if ((pkcs & BIT(31)) != 0U) {
		mask <<= 4;
		value <<= 4;
	}

	mmio_clrsetbits_32(address, mask, value);
}

static bool clk_pll1_settings_are_valid(void)
{
	return pll1_settings.valid_id == PLL1_SETTINGS_VALID_ID;
}

int stm32mp1_round_opp_khz(uint32_t *freq_khz)
{
	unsigned int i;
	uint32_t round_opp = 0U;

	if (!clk_pll1_settings_are_valid()) {
		/*
		 * No OPP table in DT, or an error occurred during PLL1
		 * settings computation, system can only work on current
		 * operating point, so return current CPU frequency.
		 */
		*freq_khz = current_opp_khz;

		return 0;
	}

	for (i = 0; i < PLAT_MAX_OPP_NB; i++) {
		if ((pll1_settings.freq[i] <= *freq_khz) &&
		    (pll1_settings.freq[i] > round_opp)) {
			round_opp = pll1_settings.freq[i];
		}
	}

	*freq_khz = round_opp;

	return 0;
}

/*
 * Check if PLL1 can be configured on the fly.
 * @result (-1) => config on the fly is not possible.
 *         (0)  => config on the fly is possible.
 *         (+1) => same parameters, no need to reconfigure.
 * Return value is 0 if no error.
 */
static int stm32mp1_is_pll_config_on_the_fly(enum stm32mp1_pll_id pll_id,
					     uint32_t *pllcfg, uint32_t fracv,
					     int *result)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t fracr;
	uint32_t value;
	int ret;

	ret = stm32mp1_pll_compute_pllxcfgr1(pll, pllcfg, &value);
	if (ret != 0) {
		return ret;
	}

	if (mmio_read_32(rcc_base + pll->pllxcfgr1) != value) {
		/* Different DIVN/DIVM, can't config on the fly */
		*result = -1;
		return 0;
	}

	*result = true;

	fracr = fracv << RCC_PLLNFRACR_FRACV_SHIFT;
	fracr |= RCC_PLLNFRACR_FRACLE;
	value = stm32mp1_pll_compute_pllxcfgr2(pllcfg);

	if ((mmio_read_32(rcc_base + pll->pllxfracr) == fracr) &&
	    (mmio_read_32(rcc_base + pll->pllxcfgr2) == value)) {
		/* Same parameters, no need to config */
		*result = 1;
	} else {
		*result = 0;
	}

	return 0;
}

static int stm32mp1_get_mpu_div(uint32_t freq_khz)
{
	unsigned long freq_pll1_p;
	unsigned long div;

	freq_pll1_p = get_clock_rate(_PLL1_P) / 1000UL;
	if ((freq_pll1_p % freq_khz) != 0U) {
		return -1;
	}

	div = freq_pll1_p / freq_khz;

	switch (div) {
	case 1UL:
	case 2UL:
	case 4UL:
	case 8UL:
	case 16UL:
		return __builtin_ffs(div) - 1;
	default:
		return -1;
	}
}

static int stm32mp1_pll1_config_from_opp_khz(uint32_t freq_khz)
{
	unsigned int i;
	int ret;
	int div;
	int config_on_the_fly = -1;

	for (i = 0; i < PLAT_MAX_OPP_NB; i++) {
		if (pll1_settings.freq[i] == freq_khz) {
			break;
		}
	}

	if (i == PLAT_MAX_OPP_NB) {
		return -ENXIO;
	}

	div = stm32mp1_get_mpu_div(freq_khz);

	switch (div) {
	case -1:
		break;
	case 0:
		return stm32mp1_set_clksrc(CLK_MPU_PLL1P);
	default:
		ret = stm32mp1_set_clkdiv(div, stm32mp_rcc_base() +
					  RCC_MPCKDIVR);
		if (ret == 0) {
			ret = stm32mp1_set_clksrc(CLK_MPU_PLL1P_DIV);
		}
		return ret;
	}

	ret = stm32mp1_is_pll_config_on_the_fly(_PLL1, &pll1_settings.cfg[i][0],
						pll1_settings.frac[i],
						&config_on_the_fly);
	if (ret != 0) {
		return ret;
	}

	if (config_on_the_fly == 1) {
		/*  No need to reconfigure, setup already OK */
		return 0;
	}

	if (config_on_the_fly == -1) {
		/* Switch to HSI and stop PLL1 before reconfiguration */
		ret = stm32mp1_set_clksrc(CLK_MPU_HSI);
		if (ret != 0) {
			return ret;
		}

		ret = stm32mp1_pll_stop(_PLL1);
		if (ret != 0) {
			return ret;
		}
	}

	ret = stm32mp1_pll_config(_PLL1, &pll1_settings.cfg[i][0],
				  pll1_settings.frac[i]);
	if (ret != 0) {
		return ret;
	}

	if (config_on_the_fly == -1) {
		/* Start PLL1 and switch back to after reconfiguration */
		stm32mp1_pll_start(_PLL1);

		ret = stm32mp1_pll_output(_PLL1,
					  pll1_settings.cfg[i][PLLCFG_O]);
		if (ret != 0) {
			return ret;
		}

		ret = stm32mp1_set_clksrc(CLK_MPU_PLL1P);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

int stm32mp1_set_opp_khz(uint32_t freq_khz)
{
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t mpu_src;

	if (freq_khz == current_opp_khz) {
		/* OPP already set, nothing to do */
		return 0;
	}

	if (!clk_pll1_settings_are_valid()) {
		/*
		 * No OPP table in DT or an error occurred during PLL1
		 * settings computation, system can only work on current
		 * operating point so return error.
		 */
		return -EACCES;
	}

	/* Check that PLL1 is MPU clock source */
	mpu_src = mmio_read_32(rcc_base + RCC_MPCKSELR) & RCC_SELR_SRC_MASK;
	if ((mpu_src != RCC_MPCKSELR_PLL) &&
	    (mpu_src != RCC_MPCKSELR_PLL_MPUDIV)) {
		return -EPERM;
	}

	if (stm32mp1_pll1_config_from_opp_khz(freq_khz) != 0) {
		/* Restore original value */
		if (stm32mp1_pll1_config_from_opp_khz(current_opp_khz) != 0) {
			ERROR("No CPU operating point can be set\n");
			panic();
		}

		return -EIO;
	}

	current_opp_khz = freq_khz;

	return 0;
}

static int clk_get_pll_settings_from_dt(int plloff, unsigned int *pllcfg,
					uint32_t *fracv, uint32_t *csg,
					bool *csg_set)
{
	int ret;

	ret = fdt_read_uint32_array(plloff, "cfg", pllcfg, (uint32_t)PLLCFG_NB);
	if (ret < 0) {
		return -FDT_ERR_NOTFOUND;
	}

	*fracv = fdt_read_uint32_default(plloff, "frac", 0);

	ret = fdt_read_uint32_array(plloff, "csg", csg, (uint32_t)PLLCSG_NB);

	*csg_set = (ret == 0);

	if (ret == -FDT_ERR_NOTFOUND) {
		ret = 0;
	}

	return ret;
}

static int clk_compute_pll1_settings(unsigned long input_freq,
				     uint32_t freq_khz,
				     uint32_t *pllcfg, uint32_t *fracv)
{
	unsigned long long output_freq = freq_khz * 1000U;
	unsigned long long freq;
	unsigned long long vco;
	int divm;
	int divn;
	int divp;
	int frac;
	int i;
	unsigned int diff;
	unsigned int best_diff = UINT_MAX;

	/* Following parameters have always the same value */
	pllcfg[PLLCFG_Q] = 0;
	pllcfg[PLLCFG_R] = 0;
	pllcfg[PLLCFG_O] = PQR(1, 0, 0);

	for (divm = DIVM_MAX; divm >= DIVM_MIN; divm--)	{
		unsigned long post_divm = input_freq /
					  (unsigned long)(divm + 1);

		if ((post_divm < POST_DIVM_MIN) ||
		    (post_divm > POST_DIVM_MAX)) {
			continue;
		}

		for (divp = DIVP_MIN; divp <= DIVP_MAX; divp++) {

			freq = output_freq * (divm + 1) * (divp + 1);

			divn = (int)((freq / input_freq) - 1);
			if ((divn < DIVN_MIN) || (divn > DIVN_MAX)) {
				continue;
			}

			frac = (int)(((freq * FRAC_MAX) / input_freq) -
				     ((divn + 1) * FRAC_MAX));

			/* 2 loops to refine the fractional part */
			for (i = 2; i != 0; i--) {
				if (frac > FRAC_MAX) {
					break;
				}

				vco = (post_divm * (divn + 1)) +
				      ((post_divm * (unsigned long long)frac) /
				       FRAC_MAX);

				if ((vco < (VCO_MIN / 2)) ||
				    (vco > (VCO_MAX / 2))) {
					frac++;
					continue;
				}

				freq = vco / (divp + 1);
				if (output_freq < freq) {
					diff = (unsigned int)(freq -
							      output_freq);
				} else {
					diff = (unsigned int)(output_freq -
							      freq);
				}

				if (diff < best_diff)  {
					pllcfg[PLLCFG_M] = divm;
					pllcfg[PLLCFG_N] = divn;
					pllcfg[PLLCFG_P] = divp;
					*fracv = frac;

					if (diff == 0) {
						return 0;
					}

					best_diff = diff;
				}

				frac++;
			}
		}
	}

	if (best_diff == UINT_MAX) {
		return -1;
	}

	return 0;
}

static int clk_get_pll1_settings(uint32_t clksrc, uint32_t freq_khz,
				 uint32_t *pllcfg, uint32_t *fracv)
{
	unsigned int i;

	assert(pllcfg != NULL);
	assert(fracv != NULL);

	for (i = 0; i < PLAT_MAX_OPP_NB; i++) {
		if (pll1_settings.freq[i] == freq_khz) {
			break;
		}
	}

	if (((i == PLAT_MAX_OPP_NB) && (pll1_settings.valid_id == 0U)) ||
	    ((i < PLAT_MAX_OPP_NB) &&
	     (pll1_settings.cfg[i][PLLCFG_O] == 0U))) {
		unsigned long input_freq;

		/*
		 * Either PLL1 settings structure is completely empty,
		 * or these settings are not yet computed: do it.
		 */
		switch (clksrc) {
		case CLK_PLL12_HSI:
			input_freq = stm32mp_clk_get_rate(CK_HSI);
			break;
		case CLK_PLL12_HSE:
			input_freq = stm32mp_clk_get_rate(CK_HSE);
			break;
		default:
			panic();
		}

		return clk_compute_pll1_settings(input_freq, freq_khz, pllcfg,
						 fracv);
	}

	if ((i < PLAT_MAX_OPP_NB) &&
	    (pll1_settings.cfg[i][PLLCFG_O] != 0U)) {
		/*
		 * Index is in range and PLL1 settings are computed:
		 * use content to answer to the request.
		 */
		memcpy(pllcfg, &pll1_settings.cfg[i][0],
		       sizeof(uint32_t) * PLAT_MAX_PLLCFG_NB);
		*fracv = pll1_settings.frac[i];

		return 0;
	}

	return -1;
}

int stm32mp1_clk_get_maxfreq_opp(uint32_t *freq_khz,
				 uint32_t *voltage_mv)
{
	unsigned int i;
	uint32_t freq = 0U;
	uint32_t voltage = 0U;

	assert(freq_khz != NULL);
	assert(voltage_mv != NULL);

	if (!clk_pll1_settings_are_valid()) {
		return -1;
	}

	for (i = 0; i < PLAT_MAX_OPP_NB; i++) {
		if (pll1_settings.freq[i] > freq) {
			freq = pll1_settings.freq[i];
			voltage = pll1_settings.volt[i];
		}
	}

	if ((freq == 0U) || (voltage == 0U)) {
		return -1;
	}

	*freq_khz = freq;
	*voltage_mv = voltage;

	return 0;
}

static int clk_save_current_pll1_settings(uint32_t buck1_voltage)
{
	const struct stm32mp1_clk_pll *pll = pll_ref(_PLL1);
	uint32_t rcc_base = stm32mp_rcc_base();
	uint32_t freq;
	unsigned int i;

	freq = udiv_round_nearest(stm32mp_clk_get_rate(CK_MPU), 1000L);

	for (i = 0; i < PLAT_MAX_OPP_NB; i++) {
		if (pll1_settings.freq[i] == freq) {
			break;
		}
	}

	if ((i == PLAT_MAX_OPP_NB) ||
	    ((pll1_settings.volt[i] != buck1_voltage) &&
	     (buck1_voltage != 0U))) {
		return -1;
	}

	pll1_settings.cfg[i][PLLCFG_M] =
		(mmio_read_32(rcc_base + pll->pllxcfgr1) &
		 RCC_PLLNCFGR1_DIVM_MASK) >> RCC_PLLNCFGR1_DIVM_SHIFT;

	pll1_settings.cfg[i][PLLCFG_N] =
		(mmio_read_32(rcc_base + pll->pllxcfgr1) &
		 RCC_PLLNCFGR1_DIVN_MASK) >> RCC_PLLNCFGR1_DIVN_SHIFT;

	pll1_settings.cfg[i][PLLCFG_P] =
		(mmio_read_32(rcc_base + pll->pllxcfgr2) &
		 RCC_PLLNCFGR2_DIVP_MASK) >> RCC_PLLNCFGR2_DIVP_SHIFT;

	pll1_settings.cfg[i][PLLCFG_Q] =
		(mmio_read_32(rcc_base + pll->pllxcfgr2) &
		 RCC_PLLNCFGR2_DIVQ_MASK) >> RCC_PLLNCFGR2_DIVQ_SHIFT;

	pll1_settings.cfg[i][PLLCFG_R] =
		(mmio_read_32(rcc_base + pll->pllxcfgr2) &
		 RCC_PLLNCFGR2_DIVR_MASK) >> RCC_PLLNCFGR2_DIVR_SHIFT;

	pll1_settings.cfg[i][PLLCFG_O] =
		mmio_read_32(rcc_base + pll->pllxcr) >>
		RCC_PLLNCR_DIVEN_SHIFT;

	pll1_settings.frac[i] =
		(mmio_read_32(rcc_base + pll->pllxfracr) &
		 RCC_PLLNFRACR_FRACV_MASK) >> RCC_PLLNFRACR_FRACV_SHIFT;

	return i;
}

static uint32_t stm32mp1_clk_get_pll1_current_clksrc(void)
{
	uint32_t value;
	const struct stm32mp1_clk_pll *pll = pll_ref(_PLL1);
	uint32_t rcc_base = stm32mp_rcc_base();

	value = mmio_read_32(rcc_base + pll->rckxselr);

	switch (value & RCC_SELR_REFCLK_SRC_MASK) {
	case 0:
		return CLK_PLL12_HSI;
	case 1:
		return CLK_PLL12_HSE;
	default:
		panic();
	}
}

int stm32mp1_clk_compute_all_pll1_settings(uint32_t buck1_voltage)
{
	int i;
	int ret;
	int index;
	uint32_t count = PLAT_MAX_OPP_NB;
	uint32_t clksrc;

	ret = dt_get_all_opp_freqvolt(&count, pll1_settings.freq,
				      pll1_settings.volt);
	switch (ret) {
	case 0:
		break;
	case -FDT_ERR_NOTFOUND:
		VERBOSE("Cannot find OPP table in DT, use default settings.\n");
		return 0;
	default:
		ERROR("Inconsistent OPP settings found in DT, ignored.\n");
		return 0;
	}

	index = clk_save_current_pll1_settings(buck1_voltage);

	clksrc = stm32mp1_clk_get_pll1_current_clksrc();

	for (i = 0; i < (int)count; i++) {
		if (i == index) {
			continue;
		}

		ret = clk_get_pll1_settings(clksrc, pll1_settings.freq[i],
					    &pll1_settings.cfg[i][0],
					    &pll1_settings.frac[i]);
		if (ret != 0) {
			return ret;
		}
	}

	pll1_settings.valid_id = PLL1_SETTINGS_VALID_ID;

	return 0;
}

void stm32mp1_clk_lp_save_opp_pll1_settings(uint8_t *data, size_t size)
{
	if (size != sizeof(pll1_settings) || !clk_pll1_settings_are_valid()) {
		panic();
	}

	memcpy(data, &pll1_settings, size);
}

void stm32mp1_clk_lp_load_opp_pll1_settings(uint8_t *data, size_t size)
{
	if (size != sizeof(pll1_settings)) {
		panic();
	}

	memcpy(&pll1_settings, data, size);
}

int stm32mp1_clk_init(uint32_t pll1_freq_khz)
{
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t pllfracv[_PLL_NB];
	uint32_t pllcsg[_PLL_NB][PLLCSG_NB];
	unsigned int clksrc[CLKSRC_NB];
	unsigned int clkdiv[CLKDIV_NB];
	unsigned int pllcfg[_PLL_NB][PLLCFG_NB];
	int plloff[_PLL_NB];
	int ret, len;
	enum stm32mp1_pll_id i;
	bool pllcsg_set[_PLL_NB];
	bool pllcfg_valid[_PLL_NB];
	bool lse_css = false;
	bool pll3_preserve = false;
	bool pll4_preserve = false;
	bool pll4_bootrom = false;
	const fdt32_t *pkcs_cell;
	int stgen_p = stm32mp1_clk_get_parent((int)STGEN_K);
	int usbphy_p = stm32mp1_clk_get_parent((int)USBPHY_K);

	/* Check status field to disable security */
	if (!fdt_get_rcc_secure_status()) {
		mmio_write_32(rcc_base + RCC_TZCR, 0);
	}

	ret = fdt_rcc_read_uint32_array("st,clksrc", clksrc,
					(uint32_t)CLKSRC_NB);
	if (ret < 0) {
		return -FDT_ERR_NOTFOUND;
	}

	ret = fdt_rcc_read_uint32_array("st,clkdiv", clkdiv,
					(uint32_t)CLKDIV_NB);
	if (ret < 0) {
		return -FDT_ERR_NOTFOUND;
	}

	for (i = (enum stm32mp1_pll_id)0; i < _PLL_NB; i++) {
		char name[12];

		snprintf(name, sizeof(name), "st,pll@%d", i);
		plloff[i] = fdt_rcc_subnode_offset(name);

		pllcfg_valid[i] = fdt_check_node(plloff[i]);
		if (pllcfg_valid[i]) {
			ret = clk_get_pll_settings_from_dt(plloff[i], pllcfg[i],
							   &pllfracv[i],
							   pllcsg[i],
							   &pllcsg_set[i]);
			if (ret != 0) {
				return ret;
			}

			continue;
		}

		if ((i == _PLL1) && (pll1_freq_khz != 0U)) {
			ret = clk_get_pll1_settings(clksrc[CLKSRC_PLL12],
						    pll1_freq_khz,
						    pllcfg[i], &pllfracv[i]);
			if (ret != 0) {
				return ret;
			}

			pllcfg_valid[i] = true;
		}
	}

	stm32mp1_mco_csg(clksrc[CLKSRC_MCO1], clkdiv[CLKDIV_MCO1]);
	stm32mp1_mco_csg(clksrc[CLKSRC_MCO2], clkdiv[CLKDIV_MCO2]);

	/*
	 * Switch ON oscillator found in device-tree.
	 * Note: HSI already ON after BootROM stage.
	 */
	if (stm32mp1_osc[_LSI] != 0U) {
		stm32mp1_lsi_set(true);
	}
	if (stm32mp1_osc[_LSE] != 0U) {
		bool bypass, digbyp;
		uint32_t lsedrv;

		bypass = fdt_osc_read_bool(_LSE, "st,bypass");
		digbyp = fdt_osc_read_bool(_LSE, "st,digbypass");
		lse_css = fdt_osc_read_bool(_LSE, "st,css");
		lsedrv = fdt_osc_read_uint32_default(_LSE, "st,drive",
						     LSEDRV_MEDIUM_HIGH);
		stm32mp1_lse_enable(bypass, digbyp, lsedrv);
	}
	if (stm32mp1_osc[_HSE] != 0U) {
		bool bypass, digbyp, css;

		bypass = fdt_osc_read_bool(_HSE, "st,bypass");
		digbyp = fdt_osc_read_bool(_HSE, "st,digbypass");
		css = fdt_osc_read_bool(_HSE, "st,css");
		stm32mp1_hse_enable(bypass, digbyp, css);
	}
	/*
	 * CSI is mandatory for automatic I/O compensation (SYSCFG_CMPCR)
	 * => switch on CSI even if node is not present in device tree
	 */
	stm32mp1_csi_set(true);

	/* Come back to HSI */
	ret = stm32mp1_set_clksrc(CLK_MPU_HSI);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clksrc(CLK_AXI_HSI);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clksrc(CLK_MCU_HSI);
	if (ret != 0) {
		return ret;
	}

	if ((mmio_read_32(rcc_base + RCC_MP_RSTSCLRR) &
	     RCC_MP_RSTSCLRR_MPUP0RSTF) != 0) {
		pll3_preserve = stm32mp1_check_pll_conf(_PLL3,
							clksrc[CLKSRC_PLL3],
							pllcfg[_PLL3],
							plloff[_PLL3]);
		pll4_preserve = stm32mp1_check_pll_conf(_PLL4,
							clksrc[CLKSRC_PLL4],
							pllcfg[_PLL4],
							plloff[_PLL4]);
	}
	/* Don't initialize PLL4, when used by BOOTROM */
	if ((get_boot_device() == BOOT_DEVICE_USB) &&
	    ((stgen_p == (int)_PLL4_R) || (usbphy_p == (int)_PLL4_R))) {
		pll4_bootrom = true;
		pll4_preserve = true;
	}

	for (i = (enum stm32mp1_pll_id)0; i < _PLL_NB; i++) {
		if (((i == _PLL3) && pll3_preserve) ||
		    ((i == _PLL4) && pll4_preserve)) {
			continue;
		}

		ret = stm32mp1_pll_stop(i);
		if (ret != 0) {
			return ret;
		}
	}

	/* Configure HSIDIV */
	if (stm32mp1_osc[_HSI] != 0U) {
		ret = stm32mp1_hsidiv(stm32mp1_osc[_HSI]);
		if (ret != 0) {
			return ret;
		}

		stm32mp_stgen_config(stm32mp_clk_get_rate(STGEN_K));
	}

	/* Select DIV */
	/* No ready bit when MPUSRC != CLK_MPU_PLL1P_DIV, MPUDIV is disabled */
	mmio_write_32(rcc_base + RCC_MPCKDIVR,
		      clkdiv[CLKDIV_MPU] & RCC_DIVR_DIV_MASK);
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_AXI], rcc_base + RCC_AXIDIVR);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_APB4], rcc_base + RCC_APB4DIVR);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_APB5], rcc_base + RCC_APB5DIVR);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_MCU], rcc_base + RCC_MCUDIVR);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_APB1], rcc_base + RCC_APB1DIVR);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_APB2], rcc_base + RCC_APB2DIVR);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clkdiv(clkdiv[CLKDIV_APB3], rcc_base + RCC_APB3DIVR);
	if (ret != 0) {
		return ret;
	}

	/* No ready bit for RTC */
	mmio_write_32(rcc_base + RCC_RTCDIVR,
		      clkdiv[CLKDIV_RTC] & RCC_DIVR_DIV_MASK);

	/* Configure PLLs source */
	ret = stm32mp1_set_clksrc(clksrc[CLKSRC_PLL12]);
	if (ret != 0) {
		return ret;
	}

	if (!pll3_preserve) {
		ret = stm32mp1_set_clksrc(clksrc[CLKSRC_PLL3]);
		if (ret != 0) {
			return ret;
		}
	}

	if (!pll4_preserve) {
		ret = stm32mp1_set_clksrc(clksrc[CLKSRC_PLL4]);
		if (ret != 0) {
			return ret;
		}
	}

	/* Configure and start PLLs */
	for (i = (enum stm32mp1_pll_id)0; i < _PLL_NB; i++) {
		if (((i == _PLL3) && pll3_preserve) ||
		    ((i == _PLL4) && pll4_preserve && !pll4_bootrom)) {
			continue;
		}

		if (!pllcfg_valid[i]) {
			continue;
		}

		if ((i == _PLL4) && pll4_bootrom) {
			/* Set output divider if not done by the Bootrom */
			stm32mp1_pll_config_output(i, pllcfg[i]);
			continue;
		}

		ret = stm32mp1_pll_config(i, pllcfg[i], pllfracv[i]);
		if (ret != 0) {
			return ret;
		}

		if (pllcsg_set[i]) {
			stm32mp1_pll_csg(i, pllcsg[i]);
		}

		stm32mp1_pll_start(i);
	}
	/* Wait and start PLLs ouptut when ready */
	for (i = (enum stm32mp1_pll_id)0; i < _PLL_NB; i++) {
		if (!pllcfg_valid[i]) {
			continue;
		}

		ret = stm32mp1_pll_output(i, pllcfg[i][PLLCFG_O]);
		if (ret != 0) {
			return ret;
		}
	}
	/* Wait LSE ready before to use it */
	if (stm32mp1_osc[_LSE] != 0U) {
		stm32mp1_lse_wait();
	}

	/* Configure with expected clock source */
	ret = stm32mp1_set_clksrc(clksrc[CLKSRC_MPU]);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clksrc(clksrc[CLKSRC_AXI]);
	if (ret != 0) {
		return ret;
	}
	ret = stm32mp1_set_clksrc(clksrc[CLKSRC_MCU]);
	if (ret != 0) {
		return ret;
	}
	stm32mp1_set_rtcsrc(clksrc[CLKSRC_RTC], lse_css);

	/* Configure PKCK */
	pkcs_cell = fdt_rcc_read_prop("st,pkcs", &len);
	if (pkcs_cell != NULL) {
		bool ckper_disabled = false;
		uint32_t j;
		uint32_t usbreg_bootrom = 0U;

		if (pll4_bootrom) {
			usbreg_bootrom = mmio_read_32(rcc_base + RCC_USBCKSELR);
		}

		for (j = 0; j < ((uint32_t)len / sizeof(uint32_t)); j++) {
			uint32_t pkcs = fdt32_to_cpu(pkcs_cell[j]);

			if (pkcs == (uint32_t)CLK_CKPER_DISABLED) {
				ckper_disabled = true;
				continue;
			}
			stm32mp1_pkcs_config(pkcs);
		}

		/*
		 * CKPER is source for some peripheral clocks
		 * (FMC-NAND / QPSI-NOR) and switching source is allowed
		 * only if previous clock is still ON
		 * => deactivated CKPER only after switching clock
		 */
		if (ckper_disabled) {
			stm32mp1_pkcs_config(CLK_CKPER_DISABLED);
		}

		if (pll4_bootrom) {
			uint32_t usbreg_value, usbreg_mask;
			const struct stm32mp1_clk_sel *sel;

			sel = clk_sel_ref(_USBPHY_SEL);
			usbreg_mask = (uint32_t)sel->msk << sel->src;
			sel = clk_sel_ref(_USBO_SEL);
			usbreg_mask |= (uint32_t)sel->msk << sel->src;

			usbreg_value = mmio_read_32(rcc_base + RCC_USBCKSELR) &
				       usbreg_mask;
			usbreg_bootrom &= usbreg_mask;
			if (usbreg_bootrom != usbreg_value) {
				VERBOSE("forbidden new USB clk path\n");
				VERBOSE("vs bootrom on USB boot\n");
				return -FDT_ERR_BADVALUE;
			}
		}
	}

	/* Switch OFF HSI if not found in device-tree */
	if (stm32mp1_osc[_HSI] == 0U) {
		stm32mp1_hsi_set(false);
	}

	stm32mp_stgen_config(stm32mp_clk_get_rate(STGEN_K));

	/* Software Self-Refresh mode (SSR) during DDR initilialization */
	mmio_clrsetbits_32(rcc_base + RCC_DDRITFCR,
			   RCC_DDRITFCR_DDRCKMOD_MASK,
			   RCC_DDRITFCR_DDRCKMOD_SSR <<
			   RCC_DDRITFCR_DDRCKMOD_SHIFT);

	return 0;
}

static void stm32mp1_osc_clk_init(const char *name,
				  enum stm32mp_osc_id index)
{
	uint32_t frequency;

	if (fdt_osc_read_freq(name, &frequency) == 0) {
		stm32mp1_osc[index] = frequency;
	}
}

static void stm32mp1_osc_init(void)
{
	enum stm32mp_osc_id i;

	for (i = (enum stm32mp_osc_id)0 ; i < NB_OSC; i++) {
		stm32mp1_osc_clk_init(stm32mp_osc_node_label[i], i);
	}
}

/*
 * Lookup platform clock from enable bit location in RCC registers.
 * Return a valid clock ID on success, return ~0 on error.
 */
unsigned long stm32mp1_clk_rcc2id(unsigned int offset, unsigned int bit)
{
	return get_id_from_rcc_bit(offset, bit);
}

#ifdef IMAGE_BL32
/*
 * Get the parent ID of the target parent clock, for tagging as secure
 * shared clock dependencies.
 */
static int get_parent_id_parent(unsigned int parent_id)
{
	enum stm32mp1_parent_sel s = _UNKNOWN_SEL;
	enum stm32mp1_pll_id pll_id;
	uint32_t p_sel;

	switch (parent_id) {
	case _ACLK:
	case _PCLK4:
	case _PCLK5:
		s = _AXIS_SEL;
		break;
	case _PLL1_P:
	case _PLL1_Q:
	case _PLL1_R:
		pll_id = _PLL1;
		break;
	case _PLL2_P:
	case _PLL2_Q:
	case _PLL2_R:
		pll_id = _PLL2;
		break;
	case _PLL3_P:
	case _PLL3_Q:
	case _PLL3_R:
		pll_id = _PLL3;
		break;
	case _PLL4_P:
	case _PLL4_Q:
	case _PLL4_R:
		pll_id = _PLL4;
		break;
	case _PCLK1:
	case _PCLK2:
	case _HCLK2:
	case _HCLK6:
	case _CK_PER:
	case _CK_MPU:
	case _CK_MCU:
	case _USB_PHY_48:
		/* We do not expected to access these */
		panic();
		break;
	default:
		/* Other parents have no parent */
		return -1;
	}

	if (s != _UNKNOWN_SEL) {
		const struct stm32mp1_clk_sel *sel = clk_sel_ref(s);
		uintptr_t rcc_base = stm32mp_rcc_base();

		p_sel = (mmio_read_32(rcc_base + sel->offset) >> sel->src) &
			sel->msk;

		if (p_sel < sel->nb_parent) {
			return (int)sel->parent[p_sel];
		}
	} else {
		const struct stm32mp1_clk_pll *pll = pll_ref(pll_id);
		uintptr_t rcc_base = stm32mp_rcc_base();

		p_sel = mmio_read_32(rcc_base + pll->rckxselr) &
			RCC_SELR_REFCLK_SRC_MASK;

		if (pll->refclk[p_sel] != _UNKNOWN_OSC_ID) {
			return (int)pll->refclk[p_sel];
		}
	}

#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
	VERBOSE("No parent selected for %s\n",
		stm32mp1_clk_parent_name[parent_id]);
#endif

	return -1;
}

static void secure_parent_clocks(unsigned long parent_id)
{
	int grandparent_id;

	switch (parent_id) {
	case _PLL3_P:
	case _PLL3_Q:
	case _PLL3_R:
		stm32mp_register_secure_periph(STM32MP1_SHRES_PLL3);
		break;

	/* These clocks are always secure when RCC is secure */
	case _ACLK:
	case _HCLK2:
	case _HCLK6:
	case _PCLK4:
	case _PCLK5:
	case _PLL1_P:
	case _PLL1_Q:
	case _PLL1_R:
	case _PLL2_P:
	case _PLL2_Q:
	case _PLL2_R:
	case _HSI:
	case _HSI_KER:
	case _LSI:
	case _CSI:
	case _CSI_KER:
	case _HSE:
	case _HSE_KER:
	case _HSE_KER_DIV2:
	case _LSE:
		break;

	default:
#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
		VERBOSE("Cannot secure parent clock %s\n",
			stm32mp1_clk_parent_name[parent_id]);
#endif
		panic();
	}

	grandparent_id = get_parent_id_parent(parent_id);
	if (grandparent_id >= 0) {
		secure_parent_clocks(grandparent_id);
	}
}

void stm32mp1_register_clock_parents_secure(unsigned long clock_id)
{
	int parent_id;

	if (!stm32mp1_rcc_is_secure()) {
		return;
	}

	switch (clock_id) {
	case PLL1:
	case PLL2:
		/* PLL1/PLL2 are always secure: nothing to do */
		return;
	case PLL3:
		stm32mp_register_secure_periph(STM32MP1_SHRES_PLL3);
		return;
	case PLL4:
		ERROR("PLL4 cannot be secured\n");
		panic();
		break;
	default:
		/* Others are expected gateable clock */
		parent_id = stm32mp1_clk_get_parent(clock_id);
		break;
	}

	if (parent_id < 0) {
		INFO("No parent for clock %lu\n", clock_id);
		return;
	}

	secure_parent_clocks(parent_id);
}
#else
void stm32mp1_register_clock_parents_secure(unsigned long clock_id)
{
}
#endif /* IMAGE_BL32 */

/*
 * Sequence to save/restore the non-secure configuration.
 * Restoring clocks and muxes need IPs to run on kernel clock
 * hence on configuration is restored at resume, kernel clock
 * should be disable: this mandates secure access.
 *
 * backup_mux*_cfg for the clock muxes.
 * backup_clock_sc_cfg for the set/clear clock gating registers
 * backup_clock_cfg for the regular full write registers
 */

struct backup_mux_cfg {
	uint16_t offset;
	uint8_t value;
	uint8_t bit_len;
};

#define MUXCFG(_offset, _bit_len) \
	{ .offset = (_offset), .bit_len = (_bit_len) }

static struct backup_mux_cfg backup_mux0_cfg[] = {
	MUXCFG(RCC_SDMMC12CKSELR, 3),
	MUXCFG(RCC_SPI2S23CKSELR, 3),
	MUXCFG(RCC_SPI45CKSELR, 3),
	MUXCFG(RCC_I2C12CKSELR, 3),
	MUXCFG(RCC_I2C35CKSELR, 3),
	MUXCFG(RCC_LPTIM23CKSELR, 3),
	MUXCFG(RCC_LPTIM45CKSELR, 3),
	MUXCFG(RCC_UART24CKSELR, 3),
	MUXCFG(RCC_UART35CKSELR, 3),
	MUXCFG(RCC_UART78CKSELR, 3),
	MUXCFG(RCC_SAI1CKSELR, 3),
	MUXCFG(RCC_ETHCKSELR, 2),
	MUXCFG(RCC_I2C46CKSELR, 3),
	MUXCFG(RCC_RNG2CKSELR, 2),
	MUXCFG(RCC_SDMMC3CKSELR, 3),
	MUXCFG(RCC_FMCCKSELR, 2),
	MUXCFG(RCC_QSPICKSELR, 2),
	MUXCFG(RCC_USBCKSELR, 2),
	MUXCFG(RCC_SPDIFCKSELR, 2),
	MUXCFG(RCC_SPI2S1CKSELR, 3),
	MUXCFG(RCC_CECCKSELR, 2),
	MUXCFG(RCC_LPTIM1CKSELR, 3),
	MUXCFG(RCC_UART6CKSELR, 3),
	MUXCFG(RCC_FDCANCKSELR, 2),
	MUXCFG(RCC_SAI2CKSELR, 3),
	MUXCFG(RCC_SAI3CKSELR,  3),
	MUXCFG(RCC_SAI4CKSELR, 3),
	MUXCFG(RCC_ADCCKSELR, 2),
	MUXCFG(RCC_DSICKSELR, 1),
	MUXCFG(RCC_CPERCKSELR, 2),
	MUXCFG(RCC_RNG1CKSELR, 2),
	MUXCFG(RCC_STGENCKSELR, 2),
	MUXCFG(RCC_UART1CKSELR, 3),
	MUXCFG(RCC_SPI6CKSELR, 3),
};

static struct backup_mux_cfg backup_mux4_cfg[] = {
	MUXCFG(RCC_USBCKSELR, 1),
};

static void backup_mux_cfg(void)
{
	uintptr_t base = stm32mp_rcc_base();
	struct backup_mux_cfg *cfg;
	size_t i;

	cfg = backup_mux0_cfg;
	for (i = 0U; i < ARRAY_SIZE(backup_mux0_cfg); i++) {
		cfg[i].value = mmio_read_32(base + cfg[i].offset) &
			       GENMASK_32(cfg[i].bit_len - 1U, 0U);
	}

	cfg = backup_mux4_cfg;
	for (i = 0U; i < ARRAY_SIZE(backup_mux4_cfg); i++) {
		cfg[i].value = mmio_read_32(base + cfg[i].offset) &
			       GENMASK_32(4U + cfg[i].bit_len - 1U, 4U);
	}
}

static void restore_mux_cfg(void)
{
	uintptr_t base = stm32mp_rcc_base();
	struct backup_mux_cfg *cfg;
	size_t i;

	cfg = backup_mux0_cfg;
	for (i = 0U; i < ARRAY_SIZE(backup_mux0_cfg); i++) {
		uint32_t mask = GENMASK_32(cfg[i].bit_len - 1U, 0U);
		uint32_t value = cfg[i].value & mask;

		mmio_clrsetbits_32(base + cfg[i].offset, mask, value);
	}

	cfg = backup_mux4_cfg;
	for (i = 0U; i < ARRAY_SIZE(backup_mux4_cfg); i++) {
		uint32_t mask = GENMASK_32(4U + cfg[i].bit_len - 1U, 4U);
		uint32_t value = cfg[i].value & mask;

		mmio_clrsetbits_32(base + cfg[i].offset, mask, value);
	}
}

/* Structure is used for set/clear registers and for regular registers */
struct backup_clock_cfg {
	uint32_t offset;
	uint32_t value;
};

static struct backup_clock_cfg backup_clock_sc_cfg[] = {
	{ .offset = RCC_MP_APB1ENSETR },
	{ .offset = RCC_MP_APB2ENSETR },
	{ .offset = RCC_MP_APB3ENSETR },
	{ .offset = RCC_MP_APB4ENSETR },
	{ .offset = RCC_MP_APB5ENSETR },
	{ .offset = RCC_MP_AHB2ENSETR },
	{ .offset = RCC_MP_AHB3ENSETR },
	{ .offset = RCC_MP_AHB4ENSETR },
	{ .offset = RCC_MP_AHB5ENSETR },
	{ .offset = RCC_MP_AHB6ENSETR },
	{ .offset = RCC_MP_MLAHBENSETR },
};

static struct backup_clock_cfg backup_clock_cfg[] = {
	{ .offset = RCC_MCO1CFGR },
	{ .offset = RCC_MCO2CFGR },
	{ .offset = RCC_PLL3CR },
	{ .offset = RCC_PLL4CR },
	{ .offset = RCC_PLL4CFGR2 },
	{ .offset = RCC_MCUDIVR },
	{ .offset = RCC_MSSCKSELR },
};

static void backup_sc_cfg(void)
{
	struct backup_clock_cfg *cfg = backup_clock_sc_cfg;
	size_t count = ARRAY_SIZE(backup_clock_sc_cfg);
	uintptr_t base = stm32mp_rcc_base();
	size_t i;

	for (i = 0U; i < count; i++) {
		cfg[i].value = mmio_read_32(base + cfg[i].offset);
	}
}

static void restore_sc_cfg(void)
{
	struct backup_clock_cfg *cfg = backup_clock_sc_cfg;
	size_t count = ARRAY_SIZE(backup_clock_sc_cfg);
	uintptr_t base = stm32mp_rcc_base();
	size_t i;

	for (i = 0U; i < count; i++) {
		mmio_write_32(base + cfg[i].offset, cfg[i].value);
		mmio_write_32(base + cfg[i].offset + RCC_MP_ENCLRR_OFFSET,
			      ~cfg[i].value);
	}
}

static void backup_regular_cfg(void)
{
	struct backup_clock_cfg *cfg = backup_clock_cfg;
	size_t count = ARRAY_SIZE(backup_clock_cfg);
	uintptr_t base = stm32mp_rcc_base();
	size_t i;

	for (i = 0U; i < count; i++) {
		cfg[i].value = mmio_read_32(base + cfg[i].offset);
	}
}

static void restore_regular_cfg(void)
{
	struct backup_clock_cfg *cfg = backup_clock_cfg;
	size_t count = ARRAY_SIZE(backup_clock_cfg);
	uintptr_t base = stm32mp_rcc_base();
	size_t i;

	for (i = 0U; i < count; i++) {
		mmio_write_32(base + cfg[i].offset, cfg[i].value);
	}
}

static void disable_kernel_clocks(void)
{
	const uint32_t ker_mask = RCC_OCENR_HSIKERON |
				  RCC_OCENR_CSIKERON |
				  RCC_OCENR_HSEKERON;

	/* Disable all ck_xxx_ker clocks */
	mmio_write_32(stm32mp_rcc_base() + RCC_OCENCLRR, ker_mask);
}

static void enable_kernel_clocks(void)
{
	uintptr_t rcc_base = stm32mp_rcc_base();
	uint32_t reg;
	const uint32_t ker_mask = RCC_OCENR_HSIKERON |
				  RCC_OCENR_CSIKERON |
				  RCC_OCENR_HSEKERON;

	/* Enable ck_xxx_ker clocks if ck_xxx was on */
	reg = mmio_read_32(rcc_base + RCC_OCENSETR) << 1U;
	mmio_write_32(rcc_base + RCC_OCENSETR, reg & ker_mask);
}

static void clear_rcc_reset_status(void)
{
	/* Clear reset status fields */
	mmio_write_32(stm32mp_rcc_base() + RCC_MP_RSTSCLRR, 0U);
}

void save_clock_pm_context(void)
{
	size_t offset = 0U;

	stm32mp1_pm_save_clock_cfg(offset,
				   (uint8_t *)backup_mux0_cfg,
				   sizeof(backup_mux0_cfg));
	offset += sizeof(backup_mux0_cfg);

	stm32mp1_pm_save_clock_cfg(offset,
				   (uint8_t *)backup_mux4_cfg,
				   sizeof(backup_mux4_cfg));
	offset += sizeof(backup_mux4_cfg);

	stm32mp1_pm_save_clock_cfg(offset,
				   (uint8_t *)backup_clock_sc_cfg,
				   sizeof(backup_clock_sc_cfg));
	offset += sizeof(backup_clock_sc_cfg);

	stm32mp1_pm_save_clock_cfg(offset,
				   (uint8_t *)backup_clock_cfg,
				   sizeof(backup_clock_cfg));
	offset += sizeof(backup_clock_cfg);

	stm32mp1_pm_save_clock_cfg(offset,
				   (uint8_t *)gate_refcounts,
				   sizeof(gate_refcounts));
}

void restore_clock_pm_context(void)
{
	size_t offset = 0U;

	stm32mp1_pm_restore_clock_cfg(offset,
				      (uint8_t *)backup_mux0_cfg,
				      sizeof(backup_mux0_cfg));
	offset += sizeof(backup_mux0_cfg);

	stm32mp1_pm_restore_clock_cfg(offset,
				      (uint8_t *)backup_mux4_cfg,
				      sizeof(backup_mux4_cfg));
	offset += sizeof(backup_mux4_cfg);

	stm32mp1_pm_restore_clock_cfg(offset,
				      (uint8_t *)backup_clock_sc_cfg,
				      sizeof(backup_clock_sc_cfg));
	offset += sizeof(backup_clock_sc_cfg);

	stm32mp1_pm_restore_clock_cfg(offset,
				      (uint8_t *)backup_clock_cfg,
				      sizeof(backup_clock_cfg));
	offset += sizeof(backup_clock_cfg);

	stm32mp1_pm_restore_clock_cfg(offset,
				      (uint8_t *)gate_refcounts,
				      sizeof(gate_refcounts));
}

void stm32mp1_clock_suspend(void)
{
	backup_regular_cfg();
	backup_sc_cfg();
	backup_mux_cfg();
	clear_rcc_reset_status();
}

void stm32mp1_clock_resume(void)
{
	unsigned int idx;

	restore_mux_cfg();
	restore_sc_cfg();
	restore_regular_cfg();

	/* Sync secure and shared clocks physical state on functional state */
	for (idx = 0U; idx < NB_GATES; idx++) {
		struct stm32mp1_clk_gate const *gate = gate_ref(idx);

		if (clock_is_always_on(gate->index)) {
			continue;
		}

		if (gate_is_non_secure(gate)) {
			continue;
		}

		if (gate_refcounts[idx] != 0U) {
			VERBOSE("Resume clock %d enable\n", gate->index);
			__clk_enable(gate);
		} else {
			VERBOSE("Resume clock %d disable\n", gate->index);
			__clk_disable(gate);
		}
	}

	disable_kernel_clocks();
}

void stm32mp1_clock_stopmode_save(void)
{
	uintptr_t rcc_base = stm32mp_rcc_base();

	/* Save registers not restored after STOP mode */
	pll3cr = mmio_read_32(rcc_base + RCC_PLL3CR);
	pll4cr = mmio_read_32(rcc_base + RCC_PLL4CR);
	mssckselr = mmio_read_32(rcc_base + RCC_MSSCKSELR);
	mcudivr = mmio_read_32(rcc_base + RCC_MCUDIVR) & RCC_MCUDIV_MASK;
	enable_kernel_clocks();
}

static bool pll_is_running(uint32_t pll_offset)
{
	uintptr_t pll_cr = stm32mp_rcc_base() + pll_offset;

	return (mmio_read_32(pll_cr) & RCC_PLLNCR_PLLON) != 0U;
}

static bool pll_was_running(uint32_t saved_value)
{
	return (saved_value & RCC_PLLNCR_PLLON) != 0U;
}

int stm32mp1_clock_stopmode_resume(void)
{
	int res;
	uintptr_t rcc_base = stm32mp_rcc_base();

	if (pll_was_running(pll4cr) && !pll_is_running(RCC_PLL4CR)) {
		stm32mp1_pll_start(_PLL4);
	}

	if (pll_was_running(pll3cr)) {
		if (!pll_is_running(RCC_PLL3CR)) {
			stm32mp1_pll_start(_PLL3);
		}

		res = stm32mp1_pll_output(_PLL3,
					  pll3cr >> RCC_PLLNCR_DIVEN_SHIFT);
		if (res != 0) {
			return res;
		}
	}

	if (pll_was_running(pll4cr)) {
		res = stm32mp1_pll_output(_PLL4,
					  pll4cr >> RCC_PLLNCR_DIVEN_SHIFT);
		if (res != 0) {
			return res;
		}
	}

	/* Restore MCU clock src after PLL3 RDY */
	mmio_write_32(rcc_base + RCC_MSSCKSELR, mssckselr);

	/* Restore MCUDIV */
	res = stm32mp1_set_clkdiv(mcudivr, rcc_base + RCC_MCUDIVR);
	if (res != 0) {
		return res;
	}

	disable_kernel_clocks();

	return 0;
}

/* Sync secure clock refcount after all drivers probe/inits,  */
void stm32mp1_dump_clocks_state(void)
{
#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
	unsigned int idx;

	/* Dump clocks state */
	for (idx = 0U; idx < NB_GATES; idx++) {
		const struct stm32mp1_clk_gate *gate = gate_ref(idx);
		unsigned long __unused clock_id = gate->index;
		unsigned int __unused refcnt = gate_refcounts[idx];
		int __unused p = stm32mp1_clk_get_parent(clock_id);

		VERBOSE("stm32mp1 clk %lu %sabled (refcnt %d) (parent %d %s)\n",
			clock_id, __clk_is_enabled(gate) ? "en" : "dis",
			refcnt, p,
			p < 0 ? "n.a" : stm32mp1_clk_parent_sel_name[p]);
	}
#endif
}

static void sync_earlyboot_clocks_state(void)
{
	unsigned int n;

	for (n = 0U; n < ARRAY_SIZE(stm32mp1_clk_gate); n++) {
		const struct stm32mp1_clk_gate *gate = gate_ref(n);

		if (!gate_is_non_secure(gate))
			stm32mp1_register_clock_parents_secure(gate->index);
	}

	/*
	 * Register secure clock parents and init a refcount for
	 * secure only resources that are not registered from a driver probe.
	 * - DDR controller and phy clocks.
	 * - TZC400, ETZPC and STGEN clocks.
	 * - RTCAPB clocks on multi-core
	 */
	stm32mp_clk_enable(AXIDCG);

	stm32mp_clk_enable(DDRC1);
	stm32mp_clk_enable(DDRC1LP);
	stm32mp_clk_enable(DDRC2);
	stm32mp_clk_enable(DDRC2LP);
	stm32mp_clk_enable(DDRCAPB);
	stm32mp_clk_enable(DDRPHYC);
	stm32mp_clk_enable(DDRPHYCLP);
	stm32mp_clk_enable(DDRPHYCAPB);
	stm32mp_clk_enable(DDRPHYCAPBLP);

	stm32mp_clk_enable(TZPC);
	stm32mp_clk_enable(TZC1);
	stm32mp_clk_enable(TZC2);
	stm32mp_clk_enable(STGEN_K);

	stm32mp_clk_enable(RTCAPB);
}

int stm32mp1_clk_probe(void)
{
	unsigned long freq_khz;

	assert(PLLCFG_NB == PLAT_MAX_PLLCFG_NB);

	stm32mp1_osc_init();

	sync_earlyboot_clocks_state();

	/* Save current CPU operating point value */
	freq_khz = udiv_round_nearest(stm32mp_clk_get_rate(CK_MPU), 1000UL);
	if (freq_khz > (unsigned long)UINT32_MAX) {
		panic();
	}

	current_opp_khz = (uint32_t)freq_khz;

	return 0;
}
