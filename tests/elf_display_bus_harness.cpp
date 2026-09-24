#include <cassert>
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK=0, GPIO_MODE_OUTPUT=1, GPIO_PULLUP_ENABLE=1, LCD_CLK_SRC_PLL160M=1;
constexpr int kDummyDcGpio=46, kWrGpio=4, kCsGpio=41, EPD_BUS_HZ=26600000;
constexpr int kDataGpios[8]={5,6,7,15,16,17,18,8};
constexpr size_t kDmaRowBytes=240;
struct gpio_config_t { uint64_t pin_bit_mask=0; int mode=0, pull_up_en=0; };
struct esp_lcd_i80_bus_config_t {
 int dc_gpio_num, wr_gpio_num, clk_src, data_gpio_nums[8], bus_width; size_t max_transfer_bytes;
};
struct esp_lcd_panel_io_i80_config_t {
 int cs_gpio_num, pclk_hz, trans_queue_depth; void (*on_color_trans_done)(); void *user_ctx;
 int lcd_cmd_bits, lcd_param_bits;
 struct {int dc_idle_level, dc_cmd_level, dc_dummy_level, dc_data_level;} dc_levels;
 struct {int cs_active_high, reverse_color_bits, swap_color_bytes, pclk_active_neg, pclk_idle_low;} flags;
};
static void *g_panel_io=nullptr, *g_i80_bus=nullptr;
static bool g_dma_done, lcd_connected, gpio_high;
static int bus_count;
static void dma_done_callback() {}
static bool init_control_gpios() {return true;}
#define ESP_LOGE(...) ((void)0)
static esp_err_t esp_lcd_new_i80_bus(const esp_lcd_i80_bus_config_t *cfg, void **out) {
 assert(cfg->dc_gpio_num==46); *out=&bus_count; ++bus_count;
 lcd_connected=true; gpio_high=false; return ESP_OK;
}
static esp_err_t esp_lcd_new_panel_io_i80(void *, const esp_lcd_panel_io_i80_config_t *cfg, void **out) {
 assert(cfg->dc_levels.dc_idle_level==1 && cfg->dc_levels.dc_data_level==1);
 *out=&bus_count; return ESP_OK;
}
static esp_err_t gpio_set_level(int pin, int high) {assert(pin==46); gpio_high=high; return ESP_OK;}
static esp_err_t gpio_config(const gpio_config_t *cfg) {
 assert(cfg->pin_bit_mask==(1ULL<<46) && cfg->mode==GPIO_MODE_OUTPUT);
 lcd_connected=false; return ESP_OK;
}
// STAGED_BUS
int main() {
 assert(init_panel_bus());
 // Provider ELF reads occur here, BEFORE any LCD transaction.
 assert(!lcd_connected && gpio_high && g_dma_done);
 assert(init_panel_bus() && bus_count==1); // setup reuses the reservation.
 assert(!lcd_connected && gpio_high);
}
