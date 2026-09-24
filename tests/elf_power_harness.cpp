#include <cassert>
#define PAPERBOY_RISCRTE_ELF 1
#define BQ25896_FAILED(v) ((v)!=0)
#define BQ25896_SUCCEEDED(v) ((v)==0)
struct Hal {int marker;};
struct Config {Hal hal; int i2c_addr_7bit;};
using bq25896_config_t=Config;
struct Device {Hal hal; int i2c_addr_7bit; bool is_initialized;};
struct Context {int scl_speed_hz, timeout_ms;};
struct bq25896_status_t {};
static Device g_charger;
static Context g_charger_hal;
static bool g_charger_ready, read_fail;
constexpr int kI2cFrequencyHz=400000, kBq25896Address=0x6b;
static int reads;
static void *i2c_bus_handle() {return &g_charger;}
static int bq25896_get_default_config(Config *c) {*c={};return 0;}
static int bq25896_hal_esp_idf_get_default_ctx(Context *c) {*c={};return 0;}
static int bq25896_hal_esp_idf_ctx_init(Context *, void *, int) {return 0;}
static int bq25896_hal_esp_idf_ctx_deinit(Context *) {return 0;}
static int bq25896_hal_esp_idf_make_hal(Context *, Hal *h) {h->marker=123; return 0;}
static int bq25896_read_status(Device *d, bq25896_status_t *) {
 assert(d->hal.marker==123 && d->i2c_addr_7bit==0x6b && d->is_initialized);
 ++reads; return read_fail ? -1 : 0;
}
// No register-writing API is provided: accidental charger/OTG control fails compilation.
// ELF_POWER_FUNCTIONS
int main() {
 assert(configure_charger() && g_charger_ready && reads==1);
 battery_service(); assert(!start_host_boost()); assert(reads==1);
 read_fail=true;
 assert(!configure_charger() && !g_charger_ready && reads==2);
}
