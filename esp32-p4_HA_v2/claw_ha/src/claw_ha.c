#include "claw_ha/claw_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_core_llm.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_logical_device.h"
#include "ha_model/ha_properties.h"
#include "ha_model/ha_state.h"

/*
 * Claw как локальный потребитель Domain. Инструменты работают только с canonical
 * (entity_id/property/action); Zigbee/endpoint/cluster сюда не попадают.
 *
 * C2.2 bring-up: home_list / home_get / home_command (+ home_ping для selftest).
 * Mock-LLM остаётся до C2.3. Тайминги — чтобы локализовать задержки, без оптимизаций.
 */

static const char *TAG = "claw_ha";

static domain_t *s_domain;
static claw_core_handle_t s_core;

static sys_error_t claw_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_CLAW_HA, code);
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* --- home_ping (keep для selftest) --- */

static esp_err_t ping_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    snprintf(output, output_size, "{\"pong\":true}");
    return ESP_OK;
}

/* --- home_list: устройства + их entities (id как hex-строка, без потери 64 бит) --- */

#define LIST_DEV_MAX 32
#define LIST_ENT_MAX 64

typedef struct {
    ha_device_id_t id;
    ha_device_id_t parent;
    char name[HA_LOGICAL_DEVICE_NAME_MAX + 1];
} list_dev_t;

typedef struct {
    ha_entity_id_t id;
    ha_device_id_t device;
    char name[HA_ENTITY_NAME_MAX + 1];
} list_ent_t;

typedef struct {
    void *items;
    size_t count;
    size_t max;
} list_collect_t;

static bool collect_devices(const void *key, const void *record, void *ctx)
{
    list_collect_t *c = (list_collect_t *)ctx;
    if (c->count >= c->max) {
        return false;
    }
    list_dev_t *arr = (list_dev_t *)c->items;
    const ha_logical_device_key_t *k = (const ha_logical_device_key_t *)key;
    const ha_logical_device_record_t *r = (const ha_logical_device_record_t *)record;
    arr[c->count].id = k->id;
    arr[c->count].parent = r->parent_device_id;
    snprintf(arr[c->count].name, sizeof(arr[c->count].name), "%s", r->name);
    c->count++;
    return true;
}

static bool collect_entities(const void *key, const void *record, void *ctx)
{
    list_collect_t *c = (list_collect_t *)ctx;
    if (c->count >= c->max) {
        return false;
    }
    list_ent_t *arr = (list_ent_t *)c->items;
    const ha_entity_key_t *k = (const ha_entity_key_t *)key;
    const ha_entity_record_t *r = (const ha_entity_record_t *)record;
    arr[c->count].id = k->id;
    arr[c->count].device = r->device_id;
    snprintf(arr[c->count].name, sizeof(arr[c->count].name), "%s", r->name);
    c->count++;
    return true;
}

static esp_err_t home_list_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                   char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    static list_dev_t devs[LIST_DEV_MAX];
    static list_ent_t ents[LIST_ENT_MAX];

    const int64_t t0 = now_us();
    list_collect_t dc = {.items = devs, .count = 0, .max = LIST_DEV_MAX};
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, collect_devices, &dc);
    const int64_t t_dev = now_us();

    list_collect_t ec = {.items = ents, .count = 0, .max = LIST_ENT_MAX};
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_ENTITY, collect_entities, &ec);
    const int64_t t_ent = now_us();

    cJSON *root = cJSON_CreateObject();
    cJSON *jdevs = cJSON_AddArrayToObject(root, "devices");
    for (size_t i = 0; i < dc.count; i++) {
        cJSON *jd = cJSON_CreateObject();
        char idbuf[24];
        snprintf(idbuf, sizeof(idbuf), "0x%016llx", (unsigned long long)devs[i].id);
        cJSON_AddStringToObject(jd, "id", idbuf);
        cJSON_AddStringToObject(jd, "name", devs[i].name);
        cJSON *jents = cJSON_AddArrayToObject(jd, "entities");
        for (size_t j = 0; j < ec.count; j++) {
            if (ents[j].device != devs[i].id) {
                continue;
            }
            cJSON *je = cJSON_CreateObject();
            snprintf(idbuf, sizeof(idbuf), "0x%016llx", (unsigned long long)ents[j].id);
            cJSON_AddStringToObject(je, "id", idbuf);
            cJSON_AddStringToObject(je, "name", ents[j].name);
            cJSON_AddItemToArray(jents, je);
        }
        cJSON_AddItemToArray(jdevs, jd);
    }

    char *s = cJSON_PrintUnformatted(root);
    if (s != NULL && strlen(s) < output_size) {
        memcpy(output, s, strlen(s) + 1);
    } else {
        snprintf(output, output_size, "{\"error\":\"list too large\"}");
    }
    cJSON_free(s);
    cJSON_Delete(root);

    ESP_LOGW(TAG, "home_list: dev_iter=%lldus ent_iter=%lldus total=%lldus devs=%u ents=%u",
             (long long)(t_dev - t0), (long long)(t_ent - t_dev), (long long)(now_us() - t0),
             (unsigned)dc.count, (unsigned)ec.count);
    return ESP_OK;
}

/* --- home_get: canonical PROPERTY_STATE по (entity_id, property) --- */

static bool json_get_u64(const cJSON *root, const char *key, uint64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        char *end = NULL;
        const unsigned long long v = strtoull(item->valuestring, &end, 0);
        if (end == item->valuestring) {
            return false;
        }
        *out = (uint64_t)v;
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out = (uint64_t)item->valuedouble;
        return true;
    }
    return false;
}

static bool json_get_i32(const cJSON *root, const char *key, int32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        *out = (int32_t)item->valuedouble;
        return true;
    }
    return false;
}

static esp_err_t home_get_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                  char *output, size_t output_size)
{
    (void)ctx;
    uint64_t entity = 0;
    int32_t property = 0;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (root == NULL || !json_get_u64(root, "entity_id", &entity) ||
        !json_get_i32(root, "property", &property)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\":\"need entity_id and property\"}");
        return ESP_OK;
    }
    cJSON_Delete(root);

    const ha_state_key_t key = {.entity = entity, .property = (ha_property_id_t)property};
    ha_state_record_t rec = {0};

    const int64_t t0 = now_us();
    const sys_error_t err =
        domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key, &rec);
    const int64_t dt = now_us() - t0;

    if (sys_failed(err)) {
        snprintf(output, output_size, "{\"error\":\"state not found\",\"code\":%u}",
                 (unsigned)err.code);
        ESP_LOGW(TAG, "home_get: entity=0x%llx property=%d get=%lldus err=%u",
                 (unsigned long long)entity, (int)property, (long long)dt, (unsigned)err.code);
        return ESP_OK;
    }

    const ha_value_t *v = &rec.value;
    double num = 0.0;
    const bool has_num = ha_value_to_double(v, &num);
    snprintf(output, output_size,
             "{\"entity_id\":\"0x%016llx\",\"property\":%d,\"kind\":%u,\"value\":%.4f}",
             (unsigned long long)entity, (int)property, (unsigned)v->kind,
             has_num ? num : 0.0);
    ESP_LOGW(TAG, "home_get: entity=0x%llx property=%d get=%lldus kind=%u",
             (unsigned long long)entity, (int)property, (long long)dt, (unsigned)v->kind);
    return ESP_OK;
}

/* --- home_command: canonical команда → Domain (HA_CMD_PROPERTY) --- */

static esp_err_t home_command_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                      char *output, size_t output_size)
{
    (void)ctx;
    uint64_t entity = 0;
    int32_t property = 0;
    int32_t action = 0;
    const cJSON *jvalue = NULL;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (root == NULL || !json_get_u64(root, "entity_id", &entity) ||
        !json_get_i32(root, "property", &property) || !json_get_i32(root, "action", &action)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\":\"need entity_id, property, action\"}");
        return ESP_OK;
    }
    jvalue = cJSON_GetObjectItemCaseSensitive(root, "value");

    ha_property_command_t cmd = {0};
    cmd.entity = entity;
    cmd.property = (ha_property_id_t)property;
    cmd.action = (uint8_t)action;
    if (cJSON_IsBool(jvalue)) {
        cmd.value.kind = HA_COMMAND_VALUE_SCALAR;
        cmd.value.value.scalar.kind = HA_VALUE_BOOL;
        cmd.value.value.scalar.value.b = cJSON_IsTrue(jvalue);
    } else if (cJSON_IsNumber(jvalue)) {
        cmd.value.kind = HA_COMMAND_VALUE_SCALAR;
        cmd.value.value.scalar.kind = HA_VALUE_FLOAT;
        cmd.value.value.scalar.value.f32 = (float)jvalue->valuedouble;
    } else {
        cmd.value.kind = HA_COMMAND_VALUE_NONE;
    }
    cJSON_Delete(root);

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const int64_t t0 = now_us();
    const sys_error_t err =
        domain_post(s_domain, HA_CMD_PROPERTY, &cmd, sizeof(cmd), NULL, &meta);
    const int64_t dt = now_us() - t0;

    snprintf(output, output_size, "{\"posted\":%s,\"code\":%u}",
             sys_failed(err) ? "false" : "true", (unsigned)err.code);
    ESP_LOGW(TAG, "home_command: entity=0x%llx property=%d action=%d post=%lldus err=%u",
             (unsigned long long)entity, (int)property, (int)action, (long long)dt,
             (unsigned)err.code);
    return ESP_OK;
}

/* --- capability group --- */

static const claw_cap_descriptor_t HOME_CAPS[] = {
    {
        .id = "home_ping",
        .name = "home_ping",
        .family = "home",
        .description = "Test tool: returns pong.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = ping_execute,
    },
    {
        .id = "home_list",
        .name = "home_list",
        .family = "home",
        .description = "List devices and their entities (id as 0x hex string).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = home_list_execute,
    },
    {
        .id = "home_get",
        .name = "home_get",
        .family = "home",
        .description = "Read a property state of an entity.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"entity_id\":{\"type\":\"string\"},\"property\":{\"type\":\"integer\"}},"
            "\"required\":[\"entity_id\",\"property\"]}",
        .execute = home_get_execute,
    },
    {
        .id = "home_command",
        .name = "home_command",
        .family = "home",
        .description = "Send a canonical command to an entity property "
                       "(action: 1 ON, 2 OFF, 3 TOGGLE, 4 SET).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"entity_id\":{\"type\":\"string\"},\"property\":{\"type\":\"integer\"},"
            "\"action\":{\"type\":\"integer\"},\"value\":{}},"
            "\"required\":[\"entity_id\",\"property\",\"action\"]}",
        .execute = home_command_execute,
    },
};

static const claw_cap_group_t HOME_GROUP = {
    .group_id = "home",
    .plugin_name = "claw_ha",
    .version = "0.1.0",
    .descriptors = HOME_CAPS,
    .descriptor_count = sizeof(HOME_CAPS) / sizeof(HOME_CAPS[0]),
};

/* --- mock LLM backend (до C2.3) --- */

#if !CONFIG_HA_CLAW_USE_REAL_LLM

static size_t s_chat_calls;

static esp_err_t mock_init(const claw_llm_runtime_config_t *config,
                           const claw_llm_model_profile_t *profile,
                           void **out_backend_ctx, char **out_error_message)
{
    (void)config;
    (void)profile;
    (void)out_error_message;
    *out_backend_ctx = NULL;
    return ESP_OK;
}

static esp_err_t mock_chat(void *backend_ctx, const claw_llm_model_profile_t *profile,
                           const claw_llm_chat_request_t *request,
                           claw_llm_response_t *out_response, char **out_error_message)
{
    (void)backend_ctx;
    (void)profile;
    (void)request;
    (void)out_error_message;
    memset(out_response, 0, sizeof(*out_response));

    if (s_chat_calls++ == 0) {
        out_response->tool_calls = calloc(1, sizeof(claw_llm_tool_call_t));
        if (out_response->tool_calls == NULL) {
            return ESP_ERR_NO_MEM;
        }
        out_response->tool_call_count = 1;
        out_response->tool_calls[0].id = strdup("call_1");
        out_response->tool_calls[0].name = strdup("home_list");
        out_response->tool_calls[0].arguments_json = strdup("{}");
        out_response->raw_message_json = strdup(
            "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
            "{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"home_list\",\"arguments\":\"{}\"}}]}");
        ESP_LOGI(TAG, "mock LLM -> tool_call home_list");
    } else {
        out_response->text = strdup("ok");
        out_response->raw_message_json = strdup("{\"role\":\"assistant\",\"content\":\"ok\"}");
        ESP_LOGI(TAG, "mock LLM -> final text");
    }
    return ESP_OK;
}

static esp_err_t mock_infer_media(void *backend_ctx, const claw_llm_model_profile_t *profile,
                                  const claw_llm_media_request_t *request, char **out_text,
                                  char **out_error_message)
{
    (void)backend_ctx;
    (void)profile;
    (void)request;
    (void)out_text;
    (void)out_error_message;
    return ESP_ERR_NOT_SUPPORTED;
}

static void mock_deinit(void *backend_ctx)
{
    (void)backend_ctx;
}

static const claw_llm_backend_vtable_t MOCK_VTABLE = {
    .init = mock_init,
    .chat = mock_chat,
    .infer_media = mock_infer_media,
    .deinit = mock_deinit,
};

static const claw_llm_custom_backend_registration_t MOCK_REG = {
    .id = "ha-mock",
    .vtable = &MOCK_VTABLE,
    .defaults = {
        .auth_type = "bearer",
        .chat_path = "/chat/completions",
        .max_tokens_field = "max_tokens",
    },
};

#endif /* !CONFIG_HA_CLAW_USE_REAL_LLM */

#if CONFIG_HA_CLAW_SELFTEST
static bool count_entity_cb(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (*(int *)ctx)++;
    return true;
}

static void claw_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Ждём, пока adapter наполнит Domain (Zigbee-интервью идёт после boot). */
    for (int i = 0; i < 45; i++) {
        int n = 0;
        domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_ENTITY, count_entity_cb, &n);
        if (n > 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#if CONFIG_HA_CLAW_USE_REAL_LLM
    /* TLS требует валидного времени и поднятой сети (IP). */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 90; i++) {
        esp_netif_ip_info_t ip = {0};
        const bool has_ip = sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
        if (has_ip && time(NULL) > 1700000000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    claw_core_request_t req = {
        .request_id = 1,
        .session_id = "selftest",
        .user_text = "Turn on the light (entity named 'ESP32C6-DISPLAY - 2'). "
                     "Use home_list to find its entity id, then home_command to switch it on.",
    };
    ESP_LOGW(TAG, "selftest time=%lld (epoch)", (long long)time(NULL));
    esp_err_t err = claw_core_submit(s_core, &req, 2000);
    ESP_LOGI(TAG, "selftest submit: %s", esp_err_to_name(err));

    claw_core_response_t resp = {0};
    const int64_t t0 = now_us();
    err = claw_core_receive(s_core, &resp, 60000);
    ESP_LOGW(TAG, "selftest receive: %s status=%d e2e=%lldus text=%s",
             esp_err_to_name(err), (int)resp.status, (long long)(now_us() - t0),
             resp.text ? resp.text : "(null)");
    claw_core_response_free(&resp);
    vTaskDelete(NULL);
}
#endif

sys_error_t claw_ha_start(domain_t *domain)
{
    s_domain = domain;

#if !CONFIG_HA_CLAW_ENABLE
    ESP_LOGI(TAG, "claw disabled by config");
    return SYS_OK;
#else
    ESP_LOGI(TAG, "task priorities: claw=%d automation=5 dispatcher=6 zigbee=5 web=4 wifi=4 system=3",
             5);

#if CONFIG_HA_CLAW_USE_REAL_LLM
    esp_log_level_set("llm_http", ESP_LOG_DEBUG);
    esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
#endif

    esp_err_t err;
#if CONFIG_HA_CLAW_USE_REAL_LLM
    if (strlen(CONFIG_HA_CLAW_LLM_API_KEY) == 0) {
        ESP_LOGE(TAG, "real LLM enabled but HA_CLAW_LLM_API_KEY is empty");
        return claw_fail(SYS_CODE_INVALID_ARG);
    }
#else
    err = claw_core_llm_register_custom_backend(&MOCK_REG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register mock backend failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_INVALID_STATE);
    }
#endif

    if ((err = claw_cap_init()) != ESP_OK) {
        ESP_LOGE(TAG, "claw_cap_init failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_NO_MEM);
    }
    if ((err = claw_cap_register_group(&HOME_GROUP)) != ESP_OK) {
        ESP_LOGE(TAG, "register home group failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_INVALID_STATE);
    }
    const char *visible[] = {"home"};
    if ((err = claw_cap_set_llm_visible_groups(visible, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set llm visible groups failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_INVALID_STATE);
    }
    if ((err = claw_cap_start_all()) != ESP_OK) {
        ESP_LOGE(TAG, "claw_cap_start_all failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_INVALID_STATE);
    }

    claw_core_config_t cfg = {0};
#if CONFIG_HA_CLAW_USE_REAL_LLM
    cfg.api_key = CONFIG_HA_CLAW_LLM_API_KEY;
    cfg.backend_type = CONFIG_HA_CLAW_LLM_BACKEND;
    cfg.model = CONFIG_HA_CLAW_LLM_MODEL;
    cfg.base_url = CONFIG_HA_CLAW_LLM_BASE_URL;
    cfg.auth_type = "bearer";
    cfg.max_tokens_field = "max_tokens";
    cfg.timeout_ms = 30000;
    cfg.max_tokens = 512;
#else
    cfg.api_key = "mock";
    cfg.backend_type = "ha-mock";
    cfg.model = "mock-model";
    cfg.base_url = "http://127.0.0.1";
    cfg.auth_type = "bearer";
    cfg.max_tokens_field = "max_tokens";
    cfg.timeout_ms = 5000;
    cfg.max_tokens = 256;
#endif
    cfg.supports_tools = true;
    cfg.supports_vision = false;
    cfg.system_prompt = "You are the HA agent. Use home_list/home_get/home_command.";
    cfg.call_cap = claw_cap_call_from_core;
    cfg.task_stack_size = 16 * 1024;
    cfg.task_priority = 5;
    cfg.task_core = tskNO_AFFINITY;
    cfg.max_tool_iterations = 4;
    cfg.request_queue_len = 4;
    cfg.response_queue_len = 4;
    cfg.max_context_providers = 4;

    if ((err = claw_core_create(&cfg, &s_core)) != ESP_OK) {
        ESP_LOGE(TAG, "claw_core_create failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_NO_MEM);
    }
    (void)claw_core_add_context_provider(s_core, &claw_cap_root_agent_tools_provider);
    if ((err = claw_core_start(s_core)) != ESP_OK) {
        ESP_LOGE(TAG, "claw_core_start failed: %s", esp_err_to_name(err));
        return claw_fail(SYS_CODE_INVALID_STATE);
    }

    ESP_LOGI(TAG, "claw started (mock backend, tools: list/get/command)");

#if CONFIG_HA_CLAW_SELFTEST
    if (xTaskCreate(claw_selftest_task, "claw_selftest", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "selftest task not created");
    }
#endif
    return SYS_OK;
#endif
}
