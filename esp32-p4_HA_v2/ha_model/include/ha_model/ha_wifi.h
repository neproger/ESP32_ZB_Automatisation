#pragma once

/*
 * Формы Wi-Fi provisioning (docs/services/WEB.md): у P4 нет своего радио, Wi-Fi живёт
 * на внешнем C3 (ESP-Hosted) и им владеет web/wifi-сервис. Display только читает Domain
 * и постит команды; формы — словарь, а не логика.
 *
 * Три таблицы (все RAM, кроме известных точек):
 *   - scan   : результат последнего скана, сервис очищает его после подключения;
 *   - known  : известные точки (ssid+password), персистентны; автоподключение при
 *              старте сканирует и выбирает известную с самым сильным сигналом;
 *   - status : текущее состояние подключения.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Синтетический uid Wi-Fi (вне диапазона Zigbee), ASCII "WIFI" в старших байтах. */
#define HA_WIFI_DEVICE_UID 0x5749464900000001ull

#define HA_WIFI_SSID_MAX     32
#define HA_WIFI_PASSWORD_MAX 64
#define HA_WIFI_SCAN_MAX     16
#define HA_WIFI_KNOWN_MAX    8

typedef enum {
    HA_WIFI_AUTH_OPEN = 0,
    HA_WIFI_AUTH_WPA = 1,
    HA_WIFI_AUTH_WPA2 = 2,
    HA_WIFI_AUTH_WPA3 = 3,
    HA_WIFI_AUTH_WPA2_ENTERPRISE = 4,
} ha_wifi_auth_t;

typedef enum {
    HA_WIFI_STATE_IDLE = 0,
    HA_WIFI_STATE_SCANNING = 1,
    HA_WIFI_STATE_CONNECTING = 2,
    HA_WIFI_STATE_CONNECTED = 3,
    HA_WIFI_STATE_ERROR = 4,
} ha_wifi_state_t;

/* sys_code_t последней ошибки подключения; 0 — нет ошибки. */
typedef struct {
    uint8_t count;
    uint8_t reserved[3];
    struct {
        char ssid[HA_WIFI_SSID_MAX];
        int8_t rssi;
        uint8_t auth; /* ha_wifi_auth_t */
        uint8_t reserved[2];
    } aps[HA_WIFI_SCAN_MAX];
} ha_wifi_scan_record_t;

typedef struct {
    uint32_t id;
} ha_wifi_known_key_t;

typedef struct {
    char ssid[HA_WIFI_SSID_MAX];
    char password[HA_WIFI_PASSWORD_MAX];
} ha_wifi_known_record_t;

typedef struct {
    uint8_t state;     /* ha_wifi_state_t */
    uint8_t connected; /* 1 — есть IP */
    int8_t rssi;
    uint8_t reserved;
    char ssid[HA_WIFI_SSID_MAX];
} ha_wifi_status_record_t;

/* Аргумент команды HA_CMD_WIFI_CONNECT. */
typedef struct {
    char ssid[HA_WIFI_SSID_MAX];
    char password[HA_WIFI_PASSWORD_MAX];
} ha_wifi_connect_args_t;

#ifdef __cplusplus
static_assert(offsetof(ha_wifi_scan_record_t, aps) == 4, "wifi scan: layout");
static_assert(sizeof(ha_wifi_scan_record_t) == 4 + HA_WIFI_SCAN_MAX * 36, "wifi scan: size");
static_assert(sizeof(ha_wifi_known_record_t) == 96, "wifi known: size");
static_assert(sizeof(ha_wifi_status_record_t) == 36, "wifi status: size");
static_assert(sizeof(ha_wifi_connect_args_t) == 96, "wifi connect args: size");
#else
_Static_assert(offsetof(ha_wifi_scan_record_t, aps) == 4, "wifi scan: layout");
_Static_assert(sizeof(ha_wifi_scan_record_t) == 4 + HA_WIFI_SCAN_MAX * 36, "wifi scan: size");
_Static_assert(sizeof(ha_wifi_known_record_t) == 96, "wifi known: size");
_Static_assert(sizeof(ha_wifi_status_record_t) == 36, "wifi status: size");
_Static_assert(sizeof(ha_wifi_connect_args_t) == 96, "wifi connect args: size");
#endif

#ifdef __cplusplus
}
#endif
