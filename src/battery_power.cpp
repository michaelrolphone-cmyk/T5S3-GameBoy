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
constexpr uint16_t kBoostMinBatteryMv = 3600U;
constexpr uint16_t kBoostStopBatteryMv = 3500U;
constexpr uint16_t kBoostMinSocPercent = 15U;
constexpr uint16_t kBoostStopSocPercent = 10U;

// Board-qualified RiscRTE v1.2.16 and board_power_t5s3_v2 PRs #101/#102.
// REG0A BOOSTV=1001 -> 5.126 V; BOOST_LIM=010 -> 1.2 A PMIC
// peak/overcurrent threshold. The logical 500 mA USB admission budget is
// separate: configuring 500 mA in this physical register failed on the board.
constexpr uint8_t kRegAdcControl = 0x02U;
constexpr uint8_t kRegPower = 0x03U;
constexpr uint8_t kRegBoost = 0x0AU;
constexpr uint8_t kRegStatus = 0x0BU;
constexpr uint8_t kRegFault = 0x0CU;
constexpr uint8_t kRegVbusAdc = 0x11U;
constexpr uint8_t kOtgEnable = 0x20U;
constexpr uint8_t kChargeEnable = 0x10U;
constexpr uint8_t kAdcContinuous = 0x40U;
constexpr uint8_t kVbusStatusMask = 0xE0U;
constexpr uint8_t kVbusOtg = 0xE0U;
constexpr uint8_t kBoostFault = 0x40U;
constexpr uint8_t kPowerGood = 0x04U;
constexpr uint8_t kVbusGood = 0x80U;
constexpr uint8_t kBoostConfig5126Mv1200Ma = 0x92U;
constexpr uint32_t kBoostSettleMs = 80U;
constexpr uint32_t kBoostStartupFaultWindowMs = 250U;
constexpr uint32_t kBoostRecoveryStableMs = 200U;
constexpr uint32_t kBoostStartupTimeoutMs = 1500U;
constexpr uint32_t kBoostShutdownTimeoutMs = 400U;

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
bool g_host_snapshot_valid = false;
uint8_t g_saved_power = 0;
uint8_t g_saved_boost = 0;
uint8_t g_saved_adc = 0;
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
bool charger_call_ok(bq25896_err_t result) { return BQ25896_SUCCEEDED(result); }

bool read_reg(uint8_t reg, uint8_t &value) {
  return g_charger_ready && charger_call_ok(bq25896_hal_read(
      &g_charger.hal, g_charger.i2c_addr_7bit, reg, &value, 1));
}
bool write_reg(uint8_t reg, uint8_t value) {
  return g_charger_ready && charger_call_ok(bq25896_hal_write(
      &g_charger.hal, g_charger.i2c_addr_7bit, reg, &value, 1));
}
// TI REG0C: first read contains fault history, second consecutive read
// contains the live state. Historical inrush is not a persistent fault.
bool read_fault_pair(uint8_t &latched, uint8_t &live) {
  return read_reg(kRegFault, latched) && read_reg(kRegFault, live);
}
bool external_vbus(const bq25896_status_t &status) {
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
  g_charger_ready = true;
  if (!restore_charger_profile()) {
    g_charger_ready = false;
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
  bq25896_adc_t adc = {};
  if (!g_charger_ready || BQ25896_FAILED(bq25896_read_adc(&g_charger, &adc))) return false;
  mv = adc.battery_voltage_mv;
  return mv >= (starting ? kBoostMinBatteryMv : kBoostStopBatteryMv);
}

void report_boost_failure(const char *reason, uint32_t begun) {
  uint8_t power = 0xFF, status = 0xFF, adc = 0xFF, prev = 0xFF, live = 0xFF;
  uint8_t boost = 0xFF, battery = 0xFF, adc_control = 0xFF;
  (void)read_reg(kRegPower, power);
  (void)read_reg(kRegStatus, status);
  (void)read_reg(kRegVbusAdc, adc);
  (void)read_fault_pair(prev, live);
  (void)read_reg(kRegBoost, boost);
  (void)read_reg(0x0EU, battery);
  (void)read_reg(kRegAdcControl, adc_control);
  ESP_LOGE(kTag, "USB VBUS failure=%s p=%02x s=%02x v=%02x prev=%02x now=%02x bat=%02x cfg=%02x conv=%02x ms=%lu",
           reason, power, status, adc, prev, live, battery, boost, adc_control,
           static_cast<unsigned long>(millis() - begun));
}

// Never restore charging while sourcing cannot be proven off. A failed write
// or readback retains the active source marker and blocks BATFET shutdown.
bool stop_host_boost(const char *reason, bool latch_fault) {
  if (latch_fault) g_host_boost_fault_latched = true;
  if (!g_host_boost_active) return true;
  if (!g_charger_ready || !g_host_snapshot_valid ||
      !write_reg(kRegPower, static_cast<uint8_t>(g_saved_power & ~(kOtgEnable | kChargeEnable)))) {
    ESP_LOGE(kTag, "USB VBUS unsafe rollback: OTG disable failed (%s)", reason);
    g_host_boost_fault_latched = true;
    return false;
  }
  const uint32_t begun = millis();
  bool off = false;
  while (millis() - begun < kBoostShutdownTimeoutMs) {
    uint8_t power = 0, status = 0;
    if (!read_reg(kRegPower, power) || !read_reg(kRegStatus, status)) break;
    if (!(power & kOtgEnable) && (status & kVbusStatusMask) != kVbusOtg) {
      off = true;
      break;
    }
    delay(10);
  }
  if (!off || !write_reg(kRegBoost, g_saved_boost) ||
      !write_reg(kRegAdcControl, g_saved_adc) || !restore_charger_profile()) {
    ESP_LOGE(kTag, "USB VBUS unsafe rollback: rail-off or profile restore not verified (%s)", reason);
    g_host_boost_fault_latched = true;
    return false;
  }
  uint8_t power = 0, status = 0, boost = 0, adc = 0;
  if (!read_reg(kRegPower, power) || !read_reg(kRegStatus, status) ||
      !read_reg(kRegBoost, boost) || !read_reg(kRegAdcControl, adc) ||
      (power & kOtgEnable) || (status & kVbusStatusMask) == kVbusOtg ||
      boost != g_saved_boost || (adc & kAdcContinuous) != (g_saved_adc & kAdcContinuous)) {
    ESP_LOGE(kTag, "USB VBUS unsafe rollback: readback failure (%s)", reason);
    g_host_boost_fault_latched = true;
    return false;
  }
  g_host_boost_active = false;
  g_host_snapshot_valid = false;
  ESP_LOGI(kTag, "USB VBUS source released and charger restored: %s", reason);
  return true;
}

bool preflight_host_boost() {
  uint8_t status = 0, vbus = 0, power = 0, prev = 0, live = 0;
  if (!read_reg(kRegStatus, status) || !read_reg(kRegVbusAdc, vbus) ||
      !read_reg(kRegPower, power) || !read_fault_pair(prev, live)) {
    ESP_LOGE(kTag, "USB VBUS preflight I2C read failure");
    return false;
  }
  if ((status & (kVbusStatusMask | kPowerGood)) ||
      (vbus & kVbusGood) || (power & kOtgEnable)) {
    ESP_LOGW(kTag, "USB VBUS preflight conflict s=%02x v=%02x p=%02x", status, vbus, power);
    return false;
  }
  if (live & (kBoostFault | 0x08U | 0x07U)) {
    ESP_LOGE(kTag, "USB VBUS preflight live fault prev=%02x now=%02x", prev, live);
    return false;
  }
  if (prev & kBoostFault)
    ESP_LOGW(kTag, "USB VBUS historical fault cleared prev=%02x now=%02x", prev, live);
  return true;
}

bool verify_host_boost(uint32_t begun) {
  bool startup_transient = false;
  uint32_t clean_since = 0;
  for (;;) {
    uint8_t power = 0, status = 0, adc = 0, prev = 0, live = 0;
    if (!read_reg(kRegPower, power) || !read_reg(kRegStatus, status) ||
        !read_reg(kRegVbusAdc, adc) || !read_fault_pair(prev, live)) {
      report_boost_failure("boost-read", begun);
      return false;
    }
    const uint32_t elapsed = millis() - begun;
    if (live & (kBoostFault | 0x08U | 0x07U)) {
      report_boost_failure("boost-live-fault", begun);
      return false;
    }
    if (!(power & kOtgEnable)) {
      report_boost_failure("boost-disabled", begun);
      return false;
    }
    if (prev & kBoostFault) {
      if (startup_transient || elapsed > kBoostStartupFaultWindowMs ||
          (status & kVbusStatusMask) != kVbusOtg) {
        report_boost_failure("boost-transient-repeat", begun);
        return false;
      }
      startup_transient = true;
      clean_since = millis();
      ESP_LOGW(kTag, "USB VBUS startup transient observed prev=%02x now=%02x ms=%lu",
               prev, live, static_cast<unsigned long>(elapsed));
    }
    if (startup_transient && (status & kVbusStatusMask) != kVbusOtg) {
      report_boost_failure("boost-unstable", begun);
      return false;
    }
    // REG11 ADC may take ~1 second to refresh. REG11 VBUS_GD is an input
    // indicator, not proof against OTG: verify OTG and ADC >=18 (~4.4 V).
    if ((status & kVbusStatusMask) == kVbusOtg && (adc & 0x7FU) >= 18U &&
        (!startup_transient || millis() - clean_since >= kBoostRecoveryStableMs)) {
      ESP_LOGI(kTag, "USB VBUS source verified cfg=0x%02x output_adc=%02x inrush=%u ms=%lu",
               kBoostConfig5126Mv1200Ma, adc, startup_transient ? 1U : 0U,
               static_cast<unsigned long>(millis() - begun));
      return true;
    }
    if (elapsed >= kBoostStartupTimeoutMs) {
      report_boost_failure("boost-timeout", begun);
      return false;
    }
    delay(10);
  }
}

bool start_host_boost() {
  if (!g_charger_ready || g_host_boost_active || g_host_boost_fault_latched) return false;
  if (!preflight_host_boost()) return false;
  uint16_t voltage = 0, soc = 0;
  if (!battery_safe_for_boost(true, voltage, soc)) {
    ESP_LOGW(kTag, "USB VBUS boost deferred: battery unsafe or unreadable mv=%u soc=%u", voltage, soc);
    return false;
  }
  if (!read_reg(kRegPower, g_saved_power) || !read_reg(kRegBoost, g_saved_boost) ||
      !read_reg(kRegAdcControl, g_saved_adc)) {
    ESP_LOGE(kTag, "USB VBUS snapshot read failed");
    return false;
  }
  g_host_snapshot_valid = true;
  // Mark active before writes: a failed I2C write may have partially applied.
  g_host_boost_active = true;
  const uint8_t boost = static_cast<uint8_t>((g_saved_boost & 0x08U) | kBoostConfig5126Mv1200Ma);
  const bool wrote = write_reg(kRegBoost, boost) &&
      write_reg(kRegAdcControl, static_cast<uint8_t>(g_saved_adc | kAdcContinuous)) &&
      write_reg(kRegPower, static_cast<uint8_t>((g_saved_power & ~kChargeEnable) | kOtgEnable));
  if (!wrote) {
    report_boost_failure("boost-config-write", millis());
    (void)stop_host_boost("startup I2C error", true);
    return false;
  }
  const uint32_t begun = millis();
  delay(kBoostSettleMs);
  if (!verify_host_boost(begun)) {
    (void)stop_host_boost("startup verification failed", true);
    return false;
  }
  ESP_LOGI(kTag, "USB VBUS boost active: 5126 mV, 1200 mA PMIC peak; BAT=%u mV SOC=%u%% cfg=0x%02x",
           voltage, soc, boost);
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
} // namespace

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
  uint8_t prev = 0, live = 0;
  bq25896_charge_config_t config = {};
  bq25896_status_t status = {};
  if (!read_fault_pair(prev, live) ||
      BQ25896_FAILED(bq25896_read_charge_config(&g_charger, &config)) ||
      BQ25896_FAILED(bq25896_read_status(&g_charger, &status))) {
    ESP_LOGW(kTag, "charger service read failed; leaving safety state unchanged");
    return;
  }
  if (g_host_boost_active) {
    uint16_t mv = 0, soc = 0;
    const bool battery_safe = battery_safe_for_boost(false, mv, soc);
    if ((live & (kBoostFault | 0x08U | 0x07U | 0x80U)) || !battery_safe ||
        !config.otg_enabled || status.vbus_status != BQ25896_VBUS_STATUS_OTG) {
      ESP_LOGE(kTag, "USB VBUS protection prev=0x%02x now=0x%02x reg03=0x%02x mode=%u BAT=%u SOC=%u safe=%u",
               prev, live, config.raw_reg03, unsigned(status.vbus_status),
               mv, soc, battery_safe ? 1U : 0U);
      (void)stop_host_boost("fault, undervoltage, or host-mode loss", true);
    }
    return; // Never restore charging while USB host owns VBUS.
  }
  if (g_host_boost_fault_latched && g_host_snapshot_valid) return;
  if (!g_host_boost_fault_latched && !external_vbus(status)) {
    (void)start_host_boost();
    return;
  }
  if ((live & (kBoostFault | 0x08U | 0x07U | 0x80U | 0x30U)) != 0) {
    ESP_LOGW(kTag, "charger live safety fault now=0x%02x; no charge restore", live);
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
  if (g_host_boost_active && !stop_host_boost("power-off", false))
    return BatteryShutdownResult::IoError;
  bq25896_status_t status = {};
  if (BQ25896_FAILED(bq25896_read_status(&g_charger, &status)))
    return BatteryShutdownResult::IoError;
  if (external_vbus(status)) return BatteryShutdownResult::UsbConnected;
  return BQ25896_SUCCEEDED(bq25896_shutdown(&g_charger))
      ? BatteryShutdownResult::PowerCutRequested
      : BatteryShutdownResult::IoError;
}
