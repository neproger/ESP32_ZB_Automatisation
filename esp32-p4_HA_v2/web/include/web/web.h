#pragma once

#include "domain/domain.h"
#include "sys/sys_error.h"

/*
 * Web service (docs/services/WEB.md): адаптер для браузера. HTTP-сервер + бинарный
 * WebSocket. Wi-Fi — station поверх ESP-Hosted (esp_wifi_remote).
 *
 * Поднимается из bootstrap; не блокирует: подключение к AP и старт сервера идут в
 * отдельной задаче. Система без Wi-Fi/Web остаётся рабочей.
 */
sys_error_t web_start(domain_t *domain);
