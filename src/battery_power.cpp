#include "battery_power.h"

#include <Arduino.h>
#include <Wire.h>
#include <bq25896.h>
#include <bq25896_hal_esp_idf.h>
#include <bq27220.h>
#include <esp_log.h>

namespace {

constexpr char kTag[] = "battery";
constexpr uint8_t kBq27220Address = 0x55;
constexpr uint8_t kBq25896Address = 0x6B;
constexpr uint32_t kI2cFrequencyHz = 400000U;
constexpr uint32_t kChargerServicePeriodMs = 30000U;
constexpr uint32_t kBoostServicePeriodMs = 1000U;
constexpr uint16_t kLowBatteryVoltageMv = 3500U;
constexpr uint16_t kRecoveredBatteryVoltageMv = 3600U;
constexpr uint16_t kLowBatterySocPercent = 10U;
constexpr uint16_t kRecoveredBatterySocPercent = 12U;
// The standalone GameBoy USB host requires its own 5 V VBUS. Do not attempt
// boost at a marginal cell voltage or keep retrying a converter safety fault.
constexpr uint16_t kBoostMinBatteryMv = 3600U;
constexpr uint16_t kBoostStopBatteryMv = 3500U;
constexpr uint16_t kBoostMinSocPercent = 15U;
constexpr uint16_t kBoostStopSocPercent = 10U;
constexpr uint16_t kBoostOutputMv = 5000U;

struct BatteryProfile {
  uint16_t input_limit_ma;
  uint16_t capacity_mah;
  uint16_t charge_current_ma;
  uint16_t precharge_current_ma;
  uint16_t termination_current_ma;
  uint16_t charge_voltage_mv;
  uint16_t charge_termination_voltage_delta_mv;
  uint16_t system_min_voltage_mv;
  int16_t current_threshold_ma;
};

constexpr BatteryProfile kProfile = {1000, 1500, 512, 64, 64, 4208, 100, 3300, 20};

bool g_battery_init_attempted = false;
bool g_charger_found = false;
bool g_gauge_found = false;
bool g_charger_ready = false;
bool g_gauge_ready = false;
bool g_low_battery = false;
bool g_host_boost_active = false;
bool g_host_boost_fault_latched = false;
uint32_t g_last_charger_service_ms = 0;
bq25896_hal_esp_idf_ctx_t g_charger_hal = {};
bq25896_t g_charger = {};
BQ27220 g_gauge;

bool probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

i2c_master_bus_handle_t i2c_bus_handle() {
  return reinterpret_cast<i2c_master_bus_handle_t>(&Wire);
}

bool charger_call_ok(bq25896_err_t result) {
  return BQ25896_SUCCEEDED(result);
}

bool external_vbus(const bq25896_status_t &status) {
  // VBUS_GD can also report our OWN boost output. OTG is not external USB
  // power and must never be presented as charging or block BATFET shutdown.
  return status.vbus_status != BQ25896_VBUS_STATUS_OTG &&
      (status.vbus_good || status.power_good);
}

bool restore_charger_profile() {
  return charger_call_ok(bq25896_disable_otg(&g_charger)) &&
      charger_call_ok(bq25896_enable_battery_power_path(&g_charger)) &&
      charger_call_ok(bq25896_set_input_limit_ma(&g_charger, kProfile.input_limit_ma)) &&
      charger_call_ok(bq25896_set_charge_current_ma(&g_charger, kProfile.charge_current_ma)) &&
      charger_call_ok(bq25896_set_precharge_current_ma(&g_charger, kProfile.precharge_current_ma)) &&
      charger_call_ok(bq25896_set_termination_current_ma(&g_charger, kProfile.termination_current_ma)) &&
      charger_call_ok(bq25896_set_charge_voltage_mv(&g_charger, kProfile.charge_voltage_mv)) &&
      charger_call_ok(bq25896_set_system_min_voltage_mv(&g_charger, kProfile.system_min_voltage_mv)) &&
      charger_call_ok(bq25896_enable_charge(&g_charger));
}

bool configure_charger() {
  bq25896_config_t config = {};
  if (BQ25896_FAILED(bq25896_get_default_config(&config)) ||
      BQ25896_FAILED(bq25896_hal_esp_idf_get_default_ctx(&g_charger_hal))) return false;
  g_charger_hal.scl_speed_hz = kI2cFrequencyHz;
  g_charger_hal.timeout_ms = 100;
  if (BQ25896_FAILED(bq25896_hal_esp_idf_ctx_init(
          &g_charger_hal, i2c_bus_handle(), kBq25896Address))) return false;
  if (BQ25896_FAILED(bq25896_hal_esp_idf_make_hal(&g_charger_hal, &config.hal))) {
    (void)bq25896_hal_esp_idf_ctx_deinit(&g_charger_hal);
    return false;
  }
  config.i2c_addr_7bit = kBq25896Address;
  config.reset_registers_on_init = true;
  config.exit_hiz_on_init = true;
  config.adc_mode = BQ25896_ADC_MODE_CONTINUOUS;
  config.watchdog = BQ25896_WATCHDOG_DISABLED;
  if (BQ25896_FAILED(bq25896_init(&g_charger, &config))) {
    (void)bq25896_hal_esp_idf_ctx_deinit(&g_charger_hal);
    g_charger = {};
    return false;
  }
  if (!restore_charger_profile()) {
    (void)bq25896_hal_esp_idf_ctx_deinit(&g_charger_hal);
    g_charger = {};
    return false;
  }
  ESP_LOGI(kTag, "BQ25896 configured input=%u mA charge=%u mA pre=%u mA term=%u mA vreg=%u mV sysmin=%u mV watchdog=off",
           kProfile.input_limit_ma, kProfile.charge_current_ma,
           kProfile.precharge_current_ma, kProfile.termination_current_ma,
           kProfile.charge_voltage_mv, kProfile.system_min_voltage_mv);
  return true;
}

bool configure_gauge() {
  if (!g_gauge.begin(i2c_bus_handle(), kBq27220Address, kI2cFrequencyHz)) return false;
  if (!g_gauge.setDefaultCapacity(kProfile.capacity_mah) ||
      !g_gauge.setChargeParameters(kProfile.charge_current_ma,
          kProfile.charge_voltage_mv, kProfile.termination_current_ma,
          kProfile.charge_termination_voltage_delta_mv) || !g_gauge.init()) {
    g_gauge.end();
    return false;
  }
  ESP_LOGI(kTag, "BQ27220 configured capacity=%u mAh charge=%u mA/%u mV taper=%u mA",
      kProfile.capacity_mah, kProfile.charge_current_ma,
      kProfile.charge_voltage_mv, kProfile.termination_current_ma);
  return true;
}

bool charger_fault_active(const bq25896_fault_t &fault) {
  return fault.watchdog_fault || fault.boost_fault || fault.battery_fault ||
      fault.charge_fault != BQ25896_CHARGE_FAULT_NORMAL ||
      fault.ntc_fault != BQ25896_NTC_FAULT_NORMAL;
}

bool charger_config_matches_profile(const bq25896_charge_config_t &config) {
  return config.charge_enabled && !config.otg_enabled && !config.hiz_enabled &&
      !config.batfet_disabled &&
      config.charge_current_ma == kProfile.charge_current_ma &&
      config.precharge_current_ma == kProfile.precharge_current_ma &&
      config.termination_current_ma == kProfile.termination_current_ma &&
      config.charge_voltage_mv == kProfile.charge_voltage_mv &&
      config.sys_min_voltage_mv == kProfile.system_min_voltage_mv;
}

// BQ25896 REG0A[2:0] BOOST_LIM=000 selects USB-OTG's 500 mA limit.
// Change only that field, preserving the output voltage and converter flags.
bool set_safe_boost_current_limit() {
  uint8_t value = 0;
  if (BQ25896_FAILED(bq25896_hal_read(&g_charger.hal,
          g_charger.i2c_addr_7bit, BQ25896_REG_0A, &value, 1))) return false;
  value &= static_cast<uint8_t>(~BQ25896_REG0A_BOOST_LIM_MASK);
  if (BQ25896_FAILED(bq25896_hal_write(&g_charger.hal,
          g_charger.i2c_addr_7bit, BQ25896_REG_0A, &value, 1))) return false;
  uint8_t verified = 0xFF;
  return BQ25896_SUCCEEDED(bq25896_hal_read(&g_charger.hal,
          g_charger.i2c_addr_7bit, BQ25896_REG_0A, &verified, 1)) &&
      (verified & BQ25896_REG0A_BOOST_LIM_MASK) == 0;
}

bool battery_safe_for_boost(bool starting, uint16_t &mv, uint16_t &soc) {
  mv = 0;
  soc = 0;
  if (g_gauge_ready) {
    BQ27220Snapshot gauge = {};
    if (g_gauge.readSnapshot(&gauge)) {
      mv = gauge.voltage_mv;
      soc = gauge.soc;
      return mv >= (starting ? kBoostMinBatteryMv : kBoostStopBatteryMv) &&
          soc > (starting ? kBoostMinSocPercent : kBoostStopSocPercent);
    }
  }
  // If the gauge is unavailable, use the charger's battery ADC, not its
  // USB VBUS ADC (which measures our own 5 V output during host mode).
  bq25896_adc_t adc = {};
  if (!g_charger_ready || BQ25896_FAILED(bq25896_read_adc(&g_charger, &adc))) return false;
  mv = adc.battery_voltage_mv;
  return mv >= (starting ? kBoostMinBatteryMv : kBoostStopBatteryMv);
}

void stop_host_boost(const char *reason, bool latch_fault) {
  if (g_host_boost_active) ESP_LOGW(kTag, "USB VBUS boost stopped: %s", reason);
  g_host_boost_active = false;
  if (latch_fault) g_host_boost_fault_latched = true;
  if (g_charger_ready && !restore_charger_profile()) {
    ESP_LOGE(kTag, "failed to restore charging after USB VBUS boost stop");
  }
}

bool start_host_boost() {
  if (!g_charger_ready || g_host_boost_active || g_host_boost_fault_latched) return false;
  bq25896_status_t before = {};
  if (BQ25896_FAILED(bq25896_read_status(&g_charger, &before))) {
    ESP_LOGW(kTag, "USB VBUS boost deferred: charger status unavailable");
    return false;
  }
  if (external_vbus(before)) {
    ESP_LOGI(kTag, "USB VBUS boost deferred: external USB power detected");
    return false;
  }
  uint16_t voltage = 0, soc = 0;
  if (!battery_safe_for_boost(true, voltage, soc)) {
    ESP_LOGW(kTag, "USB VBUS boost deferred: battery unsafe or unreadable mv=%u soc=%u",
             voltage, soc);
    return false;
  }
  const bool programmed = charger_call_ok(bq25896_enable_battery_power_path(&g_charger)) &&
      charger_call_ok(bq25896_disable_charge(&g_charger)) &&
      charger_call_ok(bq25896_set_otg_voltage_mv(&g_charger, kBoostOutputMv)) &&
      set_safe_boost_current_limit() &&
      charger_call_ok(bq25896_enable_otg(&g_charger));
  if (!programmed) {
    ESP_LOGE(kTag, "USB VBUS boost programming failed; restoring charger");
    stop_host_boost("I2C configuration failure", true);
    return false;
  }
  // Datasheet: converter requires about 30 ms after OTG enable. Verify the
  // actual operational state, not merely that the I2C write was accepted.
  delay(60);
  bq25896_status_t after = {};
  bq25896_charge_config_t config = {};
  bq25896_fault_t fault = {};
  if (BQ25896_FAILED(bq25896_read_status(&g_charger, &after)) ||
      BQ25896_FAILED(bq25896_read_charge_config(&g_charger, &config)) ||
      BQ25896_FAILED(bq25896_read_fault(&g_charger, &fault)) ||
      !config.otg_enabled || after.vbus_status != BQ25896_VBUS_STATUS_OTG ||
      fault.boost_fault || fault.battery_fault || fault.ntc_fault != BQ25896_NTC_FAULT_NORMAL) {
    ESP_LOGE(kTag, "USB VBUS boost verification failed mode=%u reg03=0x%02x fault=0x%02x; disabling",
             unsigned(after.vbus_status), config.raw_reg03, fault.raw_reg0c);
    stop_host_boost("boost verification failed", true);
    return false;
  }
  g_host_boost_active = true;
  ESP_LOGI(kTag, "USB VBUS boost active: 5 V / 500 mA limit, BAT=%u mV SOC=%u%% mode=%u",
           voltage, soc, unsigned(after.vbus_status));
  return true;
}

void update_low_battery_status(PaperboyBatteryStatus &status) {
  if (status.usb_connected) {
    g_low_battery = false;
    status.low_battery = false;
    return;
  }
  const bool voltage_available = status.voltage_mv > 0U;
  const bool soc_available = status.gauge_read_ok;
  if (!voltage_available && !soc_available) {
    status.low_battery = false;
    return;
  }
  const bool voltage_low = voltage_available && status.voltage_mv < kLowBatteryVoltageMv;
  const bool soc_low = soc_available && status.soc_percent <= kLowBatterySocPercent;
  if (!g_low_battery) g_low_battery = voltage_low || soc_low;
  else if ((!voltage_available || status.voltage_mv >= kRecoveredBatteryVoltageMv) &&
           (!soc_available || status.soc_percent > kRecoveredBatterySocPercent)) g_low_battery = false;
  status.low_battery = g_low_battery;
}

}  // namespace

bool battery_begin() {
  if (g_battery_init_attempted) return g_charger_ready || g_gauge_ready;
  g_battery_init_attempted = true;
  g_charger_found = probe(kBq25896Address);
  g_gauge_found = probe(kBq27220Address);
  g_charger_ready = g_charger_found && configure_charger();
  g_gauge_ready = g_gauge_found && configure_gauge();
  ESP_LOGI(kTag, "battery management charger=%s gauge=%s",
           g_charger_ready ? "ready" : (g_charger_found ? "init-failed" : "missing"),
           g_gauge_ready ? "ready" : (g_gauge_found ? "init-failed" : "missing"));
  // The standalone firmware always runs the native USB host: power its
  // receiver before the host starts enumerating it in the console loop.
  if (g_charger_ready) (void)start_host_boost();
  return g_charger_ready || g_gauge_ready;
}

void battery_service() {
  if (!g_battery_init_attempted) (void)battery_begin();
  const uint32_t now = millis();
  const uint32_t interval = g_host_boost_active ? kBoostServicePeriodMs : kChargerServicePeriodMs;
  if ((now - g_last_charger_service_ms) < interval) return;
  g_last_charger_service_ms = now;
  if (!g_charger_ready) {
    g_charger_found = probe(kBq25896Address);
    if (g_charger_found) {
      g_charger_ready = configure_charger();
      ESP_LOGI(kTag, "BQ25896 retry %s", g_charger_ready ? "succeeded" : "failed");
      if (g_charger_ready) (void)start_host_boost();
    }
    return;
  }
  bq25896_fault_t fault = {};
  bq25896_charge_config_t config = {};
  bq25896_status_t status = {};
  if (BQ25896_FAILED(bq25896_read_fault(&g_charger, &fault)) ||
      BQ25896_FAILED(bq25896_read_charge_config(&g_charger, &config)) ||
      BQ25896_FAILED(bq25896_read_status(&g_charger, &status))) {
    ESP_LOGW(kTag, "charger service read failed; leaving safety state unchanged");
    return;
  }
  if (g_host_boost_active) {
    uint16_t mv = 0, soc = 0;
    const bool battery_safe = battery_safe_for_boost(false, mv, soc);
    if (fault.boost_fault || fault.battery_fault || fault.watchdog_fault ||
        fault.ntc_fault != BQ25896_NTC_FAULT_NORMAL || !battery_safe ||
        !config.otg_enabled || status.vbus_status != BQ25896_VBUS_STATUS_OTG) {
      ESP_LOGE(kTag, "USB VBUS protection fault=0x%02x reg03=0x%02x mode=%u BAT=%u SOC=%u safe=%u",
               fault.raw_reg0c, config.raw_reg03, unsigned(status.vbus_status),
               mv, soc, battery_safe ? 1U : 0U);
      // An external adapter might have caused an automatic role transition;
      // never attempt to restart boost against it or repeatedly hit OCP.
      stop_host_boost("fault, undervoltage, or host-mode loss", true);
    }
    return; // Never restore CHG_CONFIG while the USB host owns VBUS.
  }
  if (charger_fault_active(fault)) {
    ESP_LOGW(kTag, "charger safety fault raw=0x%02X charge=%u ntc=%u; not forcing charge",
             fault.raw_reg0c, unsigned(fault.charge_fault), unsigned(fault.ntc_fault));
    return;
  }
  if (!g_host_boost_fault_latched && !external_vbus(status)) {
    (void)start_host_boost();
    return;
  }
  if (!charger_config_matches_profile(config)) {
    const bool restored = restore_charger_profile();
    ESP_LOGW(kTag, "charger profile drift chg=%u otg=%u hiz=%u batfet=%u; restore=%s",
             config.charge_enabled ? 1U : 0U, config.otg_enabled ? 1U : 0U,
             config.hiz_enabled ? 1U : 0U, config.batfet_disabled ? 1U : 0U,
             restored ? "ok" : "failed");
  }
}

bool battery_read_status(PaperboyBatteryStatus &status) {
  (void)battery_begin();
  status = {};
  status.gauge_found = g_gauge_found;
  status.charger_found = g_charger_found;
  status.configured_input_limit_ma = kProfile.input_limit_ma;
  status.configured_charge_current_ma = kProfile.charge_current_ma;
  status.configured_precharge_current_ma = kProfile.precharge_current_ma;
  status.configured_termination_current_ma = kProfile.termination_current_ma;
  status.configured_charge_voltage_mv = kProfile.charge_voltage_mv;
  status.configured_system_min_voltage_mv = kProfile.system_min_voltage_mv;
  if (g_charger_ready) {
    bq25896_status_t charger_status = {};
    bq25896_adc_t charger_adc = {};
    bq25896_charge_config_t charger_config = {};
    bq25896_fault_t charger_fault = {};
    const bool status_ok = BQ25896_SUCCEEDED(bq25896_read_status(&g_charger, &charger_status));
    const bool adc_ok = BQ25896_SUCCEEDED(bq25896_read_adc(&g_charger, &charger_adc));
    const bool config_ok = BQ25896_SUCCEEDED(bq25896_read_charge_config(&g_charger, &charger_config));
    status.charger_fault_read_ok = BQ25896_SUCCEEDED(bq25896_read_fault(&g_charger, &charger_fault));
    status.charger_read_ok = status_ok && adc_ok && config_ok;
    if (status_ok) {
      status.usb_connected = external_vbus(charger_status);
      status.charge_status = static_cast<uint8_t>(charger_status.charge_status);
      status.vbus_status = static_cast<uint8_t>(charger_status.vbus_status);
      status.active_input_limit_ma = charger_status.input_limit_ma;
      status.charge_done = charger_status.charge_status == BQ25896_CHARGE_STATUS_TERMINATION_DONE;
    }
    if (adc_ok) {
      if (!g_host_boost_active && (!status_ok ||
          charger_status.vbus_status != BQ25896_VBUS_STATUS_OTG))
        status.usb_connected = status.usb_connected || charger_adc.vbus_good;
      status.thermal_regulation_active = charger_adc.thermal_regulation_active;
      status.charger_adc_current_ma = charger_adc.charge_current_ma;
      status.charger_battery_voltage_mv = charger_adc.battery_voltage_mv;
      status.system_voltage_mv = charger_adc.system_voltage_mv;
      status.vbus_voltage_mv = charger_adc.vbus_voltage_mv;
    }
    if (config_ok) {
      status.charge_enabled = charger_config.charge_enabled;
      status.hiz_enabled = charger_config.hiz_enabled;
      status.batfet_disabled = charger_config.batfet_disabled;
      status.otg_enabled = charger_config.otg_enabled;
      status.configured_charge_current_ma = charger_config.charge_current_ma;
      status.configured_precharge_current_ma = charger_config.precharge_current_ma;
      status.configured_termination_current_ma = charger_config.termination_current_ma;
      status.configured_charge_voltage_mv = charger_config.charge_voltage_mv;
      status.configured_system_min_voltage_mv = charger_config.sys_min_voltage_mv;
    }
    if (status.charger_fault_read_ok) {
      status.watchdog_fault = charger_fault.watchdog_fault;
      status.boost_fault = charger_fault.boost_fault;
      status.battery_fault = charger_fault.battery_fault;
      status.charge_fault = static_cast<uint8_t>(charger_fault.charge_fault);
      status.ntc_fault = static_cast<uint8_t>(charger_fault.ntc_fault);
      status.fault_present = charger_fault_active(charger_fault);
    }
    status.charging = !status.otg_enabled && status.charge_enabled &&
        (status.charge_status == BQ25896_CHARGE_STATUS_PRECHARGE ||
         status.charge_status == BQ25896_CHARGE_STATUS_FAST_CHARGE);
    status.voltage_mv = status.charger_battery_voltage_mv;
  }
  if (g_gauge_ready) {
    BQ27220Snapshot gauge = {};
    status.gauge_read_ok = g_gauge.readSnapshot(&gauge);
    if (status.gauge_read_ok) {
      const bool inferred_vbus = status.usb_connected || (gauge.charging && !status.otg_enabled);
      const BQ27220State gauge_state = BQ27220::classifyState(
          &gauge, inferred_vbus, kProfile.current_threshold_ma);
      status.soc_percent = gauge.soc;
      status.voltage_mv = gauge.voltage_mv;
      status.current_ma = gauge.current_ma;
      status.average_current_ma = gauge.average_current_ma;
      status.remaining_capacity_mah = gauge.remaining_capacity_mah;
      status.full_capacity_mah = gauge.fcc_mah;
      status.health_percent = gauge.soh_percent;
      status.temperature_dk = gauge.temperature_dk;
      status.cycle_count = g_gauge.readRegU16(CommandCycleCount);
      status.charging = !status.otg_enabled && (status.charging || gauge_state == BQ27220StateCharge);
      status.charge_done = status.charge_done || gauge.full ||
          gauge.battery_status.reg.TCA || gauge.soc >= 100U;
      if (!status.charger_read_ok && !g_host_boost_active) status.usb_connected = inferred_vbus;
    }
  }
  update_low_battery_status(status);
  return status.gauge_read_ok || status.charger_read_ok;
}

BatteryShutdownResult battery_request_shutdown() {
  (void)battery_begin();
  if (!g_charger_ready) return BatteryShutdownResult::ChargerUnavailable;
  // Stop sourcing our own VBUS BEFORE deciding whether an external USB supply
  // prevents BATFET shutdown. Otherwise OTG's VBUS_GD looks like a charger.
  if (g_host_boost_active) {
    if (BQ25896_FAILED(bq25896_disable_otg(&g_charger)))
      return BatteryShutdownResult::IoError;
    g_host_boost_active = false;
    delay(40);
  }
  bq25896_status_t status = {};
  if (BQ25896_FAILED(bq25896_read_status(&g_charger, &status)))
    return BatteryShutdownResult::IoError;
  if (external_vbus(status)) return BatteryShutdownResult::UsbConnected;
  return BQ25896_SUCCEEDED(bq25896_shutdown(&g_charger))
      ? BatteryShutdownResult::PowerCutRequested
      : BatteryShutdownResult::IoError;
}
