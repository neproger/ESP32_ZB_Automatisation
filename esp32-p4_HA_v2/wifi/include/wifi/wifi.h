#pragma once

/*
 * Wi-Fi service (docs/services/WIFI.md): владеет радио на внешнем C3 (ESP-Hosted) и
 * provisioning. Поднимает стек, при старте подключает сильнейшую известную точку,
 * исполняет команды скана/подключения, заполняет wifi_scan/known/status в Domain.
 */

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

sys_error_t wifi_start(domain_t *domain);

#ifdef __cplusplus
}
#endif
