#pragma once

/*
 * Системный сервис (docs/services/SYSTEM.md): точное время (SNTP) и, позже, погода.
 *
 * Держит синтетическое системное устройство в Domain (device/endpoint/state/location),
 * поэтому Automation и Web видят его как обычное. Сеть берётся у Web-сервиса: задача
 * ждёт появления IP на STA-интерфейсе, сама Wi-Fi не поднимает.
 */

#include "domain/domain.h"
#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

sys_error_t system_start(domain_t *domain);

#ifdef __cplusplus
}
#endif
