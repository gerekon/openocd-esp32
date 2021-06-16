/***************************************************************************
 *   ESP32-C3 specific flasher stub functions                              *
 *   Copyright (C) 2021 Espressif Systems Ltd.                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "sdkconfig.h"
#include "soc/rtc.h"
#include "soc/spi_mem_reg.h"
#include "soc/gpio_reg.h"
#include "rtc_clk_common.h"
#include "esp_app_trace_membufs_proto.h"
#include "stub_rom_chip.h"
#include "stub_flasher_int.h"
#include "stub_flasher_chip.h"
#include "stub_flasher.h"


#define ESP_APPTRACE_RISCV_BLOCK_LEN_MSK         0x7FFFUL
#define ESP_APPTRACE_RISCV_BLOCK_LEN(_l_)        ((_l_) & ESP_APPTRACE_RISCV_BLOCK_LEN_MSK)
#define ESP_APPTRACE_RISCV_BLOCK_LEN_GET(_v_)    ((_v_) & ESP_APPTRACE_RISCV_BLOCK_LEN_MSK)
#define ESP_APPTRACE_RISCV_BLOCK_ID_MSK          0x7FUL
#define ESP_APPTRACE_RISCV_BLOCK_ID(_id_)        (((_id_) & ESP_APPTRACE_RISCV_BLOCK_ID_MSK) << 15)
#define ESP_APPTRACE_RISCV_BLOCK_ID_GET(_v_)     (((_v_) >> 15) & ESP_APPTRACE_RISCV_BLOCK_ID_MSK)
#define ESP_APPTRACE_RISCV_HOST_DATA             (1 << 22)
#define ESP_APPTRACE_RISCV_HOST_CONNECT          (1 << 23)

/** RISCV memory host iface control block */
typedef struct {
	uint32_t ctrl;
	/* - Guard field. If this register is not zero then CPU is changing this struct and */
	/*   this guard field holds address of the instruction which application will execute when
	 * CPU finishes with those modifications. */
	uint32_t stat;
	esp_apptrace_mem_block_t *mem_blocks;
} esp_apptrace_riscv_ctrl_block_t;


static esp_apptrace_riscv_ctrl_block_t *s_apptrace_ctrl;
#if CONFIG_STUB_STACK_DATA_POOL_SIZE > 0
static uint8_t *s_stack_data_pool;
static size_t s_stack_data_pool_sz;
#endif

uint32_t g_stub_cpu_freq_hz = CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ * MHZ;


void vPortEnterCritical(void)
{
}

void vPortExitCritical(void)
{
}

static void esp32c3_flash_disable_cache(uint32_t *saved_state)
{
	uint32_t icache_state;
	icache_state = Cache_Suspend_ICache() << 16;
	saved_state[0] = icache_state;
}

static void esp32c3_flash_restore_cache(uint32_t *saved_state)
{
	Cache_Resume_ICache(saved_state[0] >> 16);
}

uint32_t stub_flash_get_id(void)
{
	uint32_t ret;

	STUB_LOGD("flash %x, cs %x, bs %x, ss %x, ps %x, sm %x\n",
		rom_spiflash_legacy_data->chip.device_id,
		rom_spiflash_legacy_data->chip.chip_size,
		rom_spiflash_legacy_data->chip.block_size,
		rom_spiflash_legacy_data->chip.sector_size,
		rom_spiflash_legacy_data->chip.page_size,
		rom_spiflash_legacy_data->chip.status_mask);
	WRITE_PERI_REG(PERIPHS_SPI_FLASH_C0, 0);/* clear regisrter */
	WRITE_PERI_REG(PERIPHS_SPI_FLASH_CMD, SPI_MEM_FLASH_RDID);
	while (READ_PERI_REG(PERIPHS_SPI_FLASH_CMD) != 0) ;
	ret = READ_PERI_REG(PERIPHS_SPI_FLASH_C0) & 0xffffff;
	STUB_LOGD("Flash ID read %x\n", ret);
	return ret >> 16;
}

void stub_flash_cache_flush(void)
{
	/* we do not know breakpoint program address here, so invalidate the
	 * whole ICache */
	Cache_Invalidate_ICache_All();
}

void stub_flash_state_prepare(struct stub_flash_state *state)
{
	uint32_t spiconfig = ets_efuse_get_spiconfig();
	uint32_t strapping = REG_READ(GPIO_STRAP_REG);
	/*  If GPIO1 (U0TXD) is pulled low and flash pin configuration is not set in efuse, assume
	 * HSPI flash mode (same as normal boot) */
	if (spiconfig == 0 && (strapping & 0x1c) == 0x08)
		spiconfig = 1;	/* HSPI flash mode */

	esp32c3_flash_disable_cache(state->cache_flags);
	esp_rom_spiflash_attach(spiconfig, 0);
}

void stub_flash_state_restore(struct stub_flash_state *state)
{
	esp32c3_flash_restore_cache(state->cache_flags);
}

#define RTC_PLL_FREQ_320M   320
#define RTC_PLL_FREQ_480M   480

rtc_xtal_freq_t stub_rtc_clk_xtal_freq_get(void)
{
	uint32_t xtal_freq_reg = READ_PERI_REG(RTC_XTAL_FREQ_REG);
	if (!clk_val_is_valid(xtal_freq_reg))
		return RTC_XTAL_FREQ_40M;
	return reg_val_to_clk_val(xtal_freq_reg);
}

/* Obviously we can call rtc_clk_cpu_freq_get_config() from esp-idf
But this call may cause undesired locks due to ets_printf or abort
*/
int stub_rtc_clk_cpu_freq_get_config(rtc_cpu_freq_config_t *out_config)
{
	rtc_cpu_freq_src_t source;
	uint32_t source_freq_mhz;
	uint32_t div;
	uint32_t freq_mhz;
	uint32_t soc_clk_sel = REG_GET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_SOC_CLK_SEL);
	switch (soc_clk_sel) {
		case DPORT_SOC_CLK_SEL_XTAL: {
			source = RTC_CPU_FREQ_SRC_XTAL;
			div = REG_GET_FIELD(SYSTEM_SYSCLK_CONF_REG, SYSTEM_PRE_DIV_CNT) + 1;
			source_freq_mhz = (uint32_t) stub_rtc_clk_xtal_freq_get();
			freq_mhz = source_freq_mhz / div;
		}
		break;
		case DPORT_SOC_CLK_SEL_PLL: {
			source = RTC_CPU_FREQ_SRC_PLL;
			uint32_t cpuperiod_sel = REG_GET_FIELD(SYSTEM_CPU_PER_CONF_REG,
				SYSTEM_CPUPERIOD_SEL);
			uint32_t pllfreq_sel = REG_GET_FIELD(SYSTEM_CPU_PER_CONF_REG,
				SYSTEM_PLL_FREQ_SEL);
			source_freq_mhz = (pllfreq_sel) ? RTC_PLL_FREQ_480M : RTC_PLL_FREQ_320M;
			if (cpuperiod_sel == DPORT_CPUPERIOD_SEL_80) {
				div = (source_freq_mhz == RTC_PLL_FREQ_480M) ? 6 : 4;
				freq_mhz = 80;
			} else if (cpuperiod_sel == DPORT_CPUPERIOD_SEL_160) {
				div = (source_freq_mhz == RTC_PLL_FREQ_480M) ? 3 : 2;
				div = 3;
				freq_mhz = 160;
			} else {
				/* unsupported frequency configuration */
				return -1;
			}
			break;
		}
		case DPORT_SOC_CLK_SEL_8M:
			source = RTC_CPU_FREQ_SRC_8M;
			source_freq_mhz = 8;
			div = 1;
			freq_mhz = source_freq_mhz;
			break;
		default:
			/* unsupported frequency configuration */
			return -2;
	}
	*out_config = (rtc_cpu_freq_config_t) {
		.source = source,
		.source_freq_mhz = source_freq_mhz,
		.div = div,
		.freq_mhz = freq_mhz
	};
	return 0;
}

/* this function has almost the same implementation for ESP32 and ESP32-S2
 * TODO: move to common file */
int stub_cpu_clock_configure(int cpu_freq_mhz)
{
	rtc_cpu_freq_config_t old_config;
	int ret = stub_rtc_clk_cpu_freq_get_config(&old_config);
	if (ret < 0) {
		/* this return value will avoid undesired restore requests for unsupported frequency
		 *configuration */
		old_config.freq_mhz = 0;
	}

#if STUB_LOG_LOCAL_LEVEL > STUB_LOG_NONE
	uart_tx_wait_idle(CONFIG_CONSOLE_UART_NUM);
#endif

	/* set to maximum possible value */
	if (cpu_freq_mhz == -1)
		cpu_freq_mhz = CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ;

	/* Set CPU to configured value. Keep other clocks unmodified. */
	if (cpu_freq_mhz > 0) {
		rtc_clk_config_t clk_cfg = RTC_CLK_CONFIG_DEFAULT();
		/* ESP32-S2 doesn't have XTAL_FREQ choice, always 40MHz.
		   So using default value is fine */
		clk_cfg.cpu_freq_mhz = cpu_freq_mhz;
		clk_cfg.slow_freq = rtc_clk_slow_freq_get();
		clk_cfg.fast_freq = rtc_clk_fast_freq_get();
		rtc_clk_init(clk_cfg);

		g_stub_cpu_freq_hz = cpu_freq_mhz * MHZ;
	}

	return old_config.freq_mhz;
}

#if STUB_LOG_LOCAL_LEVEL > STUB_LOG_NONE
void stub_uart_console_configure()
{
	extern bool g_uart_print;
	/* set the default parameter to UART module, but don't enable RX interrupt */
	uartAttach(NULL);
	/* first enable uart0 as printf channel */
	uint32_t clock = ets_get_apb_freq();
	ets_update_cpu_frequency(clock/1000000);

	Uart_Init(ets_efuse_get_uart_print_channel(), UART_CLK_FREQ_ROM);
	/* install to print later
	 * Non-Flash Boot can print
	 * Flash Boot can print when RTC_CNTL_STORE4_REG bit0 is 0 (can be 1 after deep sleep, software reset) and printf boot.
	 * print boot determined by GPIO and efuse, see ets_is_print_boot
	 */
	g_uart_print = true;
	ets_install_uart_printf();
}
#endif

uint32_t stub_esp_clk_cpu_freq(void)
{
	return (CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ * 1000000);
}

/* override apptrace control block advertising func, IDF's implementation issues syscall */
int esp_apptrace_advertise_ctrl_block(void *ctrl_block_addr)
{
	s_apptrace_ctrl = ctrl_block_addr;
	return 0;
}

#if CONFIG_STUB_STACK_DATA_POOL_SIZE > 0
void stub_stack_data_pool_init(uint8_t *data, size_t sz)
{
	STUB_LOGD("stack data pool %lu bytes @ 0x%x\n", sz, data);
	s_stack_data_pool = data;
	s_stack_data_pool_sz = sz;
}

void esp_apptrace_get_up_buffers(esp_apptrace_mem_block_t mem_blocks_cfg[2])
{
	/* use whole stack data pool for apptrace up buffers */
	mem_blocks_cfg[0].start = s_stack_data_pool;
	mem_blocks_cfg[0].sz = s_stack_data_pool_sz / 2;
	mem_blocks_cfg[1].start = s_stack_data_pool + mem_blocks_cfg[0].sz;
	mem_blocks_cfg[1].sz = mem_blocks_cfg[0].sz;
}
#endif

int stub_apptrace_prepare()
{
	/* imply that host is auto-connected */
	s_apptrace_ctrl->ctrl |= ESP_APPTRACE_RISCV_HOST_CONNECT;
	return ESP_STUB_ERR_OK;
}

int64_t esp_timer_get_time(void)
{
	/* this function is used by apptrace code to implement timeouts.
	   unfortunately esp32c3 does not support CPU cycle counter, so we have two options:
	   1) Use some HW timer. It can be hard, because we need to ensure that it is initialized and possibly restore its state.
	   2) Emulate timer by incrementing some var on every call.
	          Stub flasher uses ESP_APPTRACE_TMO_INFINITE only, so this function won't be called by apptrace at all. */
	return 0;
}

uint64_t stub_get_time(void)
{
	/* this function is used for perf measurements only.
	   unfortunately esp32c3 does not support CPU cycle counter and usage of HW timer is problematic */
	return 0;
}

esp_rom_spiflash_result_t esp_rom_spiflash_erase_area(uint32_t start_addr, uint32_t area_len)
{
	int32_t total_sector_num;
	int32_t head_sector_num;
	uint32_t sector_no;
	uint32_t sector_num_per_block;

	/* set read mode to Fastmode ,not QDIO mode for erase
	 *
	 * TODO: this is probably a bug as it doesn't re-enable QIO mode, not serious as this
	 * function is not used in IDF.
	 * esp_rom_spiflash_config_readmode(ESP_ROM_SPIFLASH_SLOWRD_MODE); */

	/* check if area is oversize of flash */
	if ((start_addr + area_len) > rom_spiflash_legacy_data->chip.chip_size)
		return ESP_ROM_SPIFLASH_RESULT_ERR;

	/* start_addr is aligned as sector boundary */
	if (0 != (start_addr % rom_spiflash_legacy_data->chip.sector_size))
		return ESP_ROM_SPIFLASH_RESULT_ERR;

	/* Unlock flash to enable erase */
	if (ESP_ROM_SPIFLASH_RESULT_OK != esp_rom_spiflash_unlock(/*&rom_spiflash_legacy_data->chip*/))
		return ESP_ROM_SPIFLASH_RESULT_ERR;

	sector_no = start_addr / rom_spiflash_legacy_data->chip.sector_size;
	sector_num_per_block = rom_spiflash_legacy_data->chip.block_size /
		rom_spiflash_legacy_data->chip.sector_size;
	total_sector_num =
		(0 ==
		(area_len %
			rom_spiflash_legacy_data->chip.sector_size)) ? area_len /
		rom_spiflash_legacy_data->chip.sector_size :
		1 + (area_len / rom_spiflash_legacy_data->chip.sector_size);

	/* check if erase area reach over block boundary */
	head_sector_num = sector_num_per_block - (sector_no % sector_num_per_block);

	head_sector_num =
		(head_sector_num >= total_sector_num) ? total_sector_num : head_sector_num;

	/* JJJ, BUG of 6.0 erase
	 * middle part of area is aligned by blocks */
	total_sector_num -= head_sector_num;

	/* head part of area is erased */
	while (0 != head_sector_num) {
		if (ESP_ROM_SPIFLASH_RESULT_OK != esp_rom_spiflash_erase_sector(sector_no))
			return ESP_ROM_SPIFLASH_RESULT_ERR;
		sector_no++;
		head_sector_num--;
	}
	while (total_sector_num > sector_num_per_block) {
		if (ESP_ROM_SPIFLASH_RESULT_OK !=
			esp_rom_spiflash_erase_block(sector_no / sector_num_per_block))
			return ESP_ROM_SPIFLASH_RESULT_ERR;
		sector_no += sector_num_per_block;
		total_sector_num -= sector_num_per_block;
	}

	/* tail part of area burn */
	while (0 < total_sector_num) {
		if (ESP_ROM_SPIFLASH_RESULT_OK != esp_rom_spiflash_erase_sector(sector_no))
			return ESP_ROM_SPIFLASH_RESULT_ERR;
		sector_no++;
		total_sector_num--;
	}

	return ESP_ROM_SPIFLASH_RESULT_OK;
}

bool esp_cpu_in_ocd_debug_mode(void)
{
	return true;
}

int stub_flash_read_buff(uint32_t addr, void *buffer, uint32_t size)
{
	return esp_rom_spiflash_read(addr, buffer, size);
}