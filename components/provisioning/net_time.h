/*
 * net_time.h — one-shot SNTP clock sync over an already-connected network.
 *
 * WiFi bring-up (station connect + SoftAP captive-portal provisioning) is owned
 * by the `provisioning` component. Once we are online this syncs the system
 * clock, which the news screen needs for its 7-day query window and which the
 * chart uses for exchange-local time labels.
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
