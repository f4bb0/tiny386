#ifdef USE_LCD_EK79007

#include <stdio.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ek79007.h"
#include "esp_ldo_regulator.h"

#include "common.h"

static const char *TAG = "lcd";

#ifndef LCD_BL_ACTIVE_LEVEL
#define LCD_BL_ACTIVE_LEVEL 1
#endif

/*
 * Uncomment these lines if you need a custom initialization sequence.
 * The array must be static const and declared outside the function.
 */
/*
static const ek79007_lcd_init_cmd_t lcd_init_cmds[] = {
	{0xE0, (uint8_t[]){0x00}, 1, 0},
	{0xE1, (uint8_t[]){0x93}, 1, 0},
	{0xE2, (uint8_t[]){0x65}, 1, 0},
	{0xE3, (uint8_t[]){0xF8}, 1, 0},
};
*/

void pc_vga_step(void *o);

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	if (!globals.panel || !src) {
		return;
	}

	if (x_start < 0)
		x_start = 0;
	if (y_start < 0)
		y_start = 0;
	if (x_end > LCD_WIDTH)
		x_end = LCD_WIDTH;
	if (y_end > LCD_HEIGHT)
		y_end = LCD_HEIGHT;
	if (x_start >= x_end || y_start >= y_end) {
		return;
	}

	ESP_ERROR_CHECK(
		esp_lcd_panel_draw_bitmap(
			globals.panel,
			x_start, y_start,
			x_end, y_end,
			src));
}

static esp_err_t bsp_enable_dsi_phy_power(void)
{
	static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
	if (phy_pwr_chan) {
		return ESP_OK;
	}

	const esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = 3,
		.voltage_mv = 2500,
	};
	return esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
}

static esp_err_t bsp_enable_backlight(void)
{
#ifdef LCD_BL
	const gpio_config_t bl_cfg = {
		.pin_bit_mask = 1ULL << LCD_BL,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "configure LCD_BL");
	ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BL, LCD_BL_ACTIVE_LEVEL), TAG, "set LCD_BL level");
	ESP_LOGI(TAG, "Backlight enabled on GPIO %d", LCD_BL);
#else
	ESP_LOGW(TAG, "LCD_BL is not defined, skip backlight GPIO control");
#endif

	return ESP_OK;
}

static esp_err_t bsp_configure_panel_direction_pins(void)
{
	const gpio_config_t dir_cfg = {
		.pin_bit_mask = (1ULL << LCD_UPDN) | (1ULL << LCD_SHLR),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};

	ESP_RETURN_ON_ERROR(gpio_config(&dir_cfg), TAG, "configure LCD direction pins");
	ESP_RETURN_ON_ERROR(gpio_set_level(LCD_UPDN, 0), TAG, "set LCD_UPDN level");
	ESP_RETURN_ON_ERROR(gpio_set_level(LCD_SHLR, 0), TAG, "set LCD_SHLR level");

	return ESP_OK;
}

void vga_task(void *arg)
{
	ESP_ERROR_CHECK(bsp_configure_panel_direction_pins());

	(void)arg;

	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "vga runs on core %d\n", core_id);

	ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
	ESP_ERROR_CHECK(bsp_enable_dsi_phy_power());

	ESP_LOGI(TAG, "Initialize MIPI DSI bus");
	esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
	esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
	bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
	ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

	ESP_LOGI(TAG, "Install panel IO");
	esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
	esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

	ESP_LOGI(TAG, "Install EK79007 panel driver");
	esp_lcd_panel_handle_t panel_handle = NULL;
	esp_lcd_dpi_panel_config_t dpi_config = {
		.virtual_channel = 0,
		.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
		.dpi_clock_freq_mhz = 52,
		.num_fbs = 2,
		.video_timing = {
			.h_size = LCD_WIDTH,
			.v_size = LCD_HEIGHT,
			.hsync_pulse_width = 10,
			.hsync_back_porch = 160,
			.hsync_front_porch = 160,
			.vsync_pulse_width = 1,
			.vsync_back_porch = 23,
			.vsync_front_porch = 12,
		},
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
		.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
		.flags = { .use_dma2d = true },
#else
		.in_color_format = LCD_COLOR_FMT_RGB565,
#endif
	};

	ek79007_vendor_config_t vendor_config = {
		.init_cmds = NULL,
		.init_cmds_size = 0,
		.mipi_config = {
			.dsi_bus = mipi_dsi_bus,
			.dpi_config = &dpi_config,
			.lane_num = 2,
		},
	};

	const esp_lcd_panel_dev_config_t panel_config = {
		.reset_gpio_num = LCD_RST,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
		.flags = {
			.reset_active_high = 1,
		},
		.vendor_config = &vendor_config,
	};

	ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(mipi_dbi_io, &panel_config, &panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
	ESP_ERROR_CHECK(bsp_enable_backlight());

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
	ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel_handle));
	// esp_err_t dma2d_ret = esp_lcd_dpi_panel_enable_dma2d(panel_handle);
	// if (dma2d_ret != ESP_OK) {
	// 	ESP_LOGW(TAG, "DMA2D unavailable (%s), continuing without async memcpy", esp_err_to_name(dma2d_ret));
	// }
#endif
	//ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

	globals.panel = panel_handle;
	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);

	while (1) {
		pc_vga_step(globals.pc);
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

#endif
