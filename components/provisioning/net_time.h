/*
 * net_time.h — one-shot SNTP clock sync over an already-connected network.
 *
 * WiFi bring-up (station connect + SoftAP captive-portal provisioning) is owned
 * by the `provisioning` component. Once we are online this syncs the system
 * clock, which is the only source the header clock has: there is no RTC on the
 * EE04, so until this succeeds the header reads `--:--`.
 *
 * Nothing else depends on it. Snapshot staleness is measured with
 * `esp_timer_get_time()` precisely so that a board which never reaches an NTP
 * server still ages its own data correctly.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Blocking SNTP sync against pool.ntp.org, bounded by timeout_ms. Safe to call
 * once after the network is up; logs and returns on timeout. */
void net_time_sync(int timeout_ms);

#ifdef __cplusplus
}
#endif
