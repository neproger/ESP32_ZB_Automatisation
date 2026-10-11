/*
 * Step C1 spike host: минимальный P4-хост claw_core + claw_cap без esp-claw shell.
 *
 * Boot -> register mock LLM backend -> claw_cap (dummy group) -> claw_core ->
 * submit("ping") -> LLM(mock) делает tool_call home_ping -> dummy capability ->
 * финальный ответ. Heap-probe в ключевых точках.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_cap.h"
#include "claw_core.h"
#include "claw_core_llm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "spike";

/* --- heap probe (internal/DMA/PSRAM + largest block + min-ever internal) --- */

static void probe(const char *tag)
{
    ESP_LOGW(TAG, "PROBE %-20s internal=%u dma=%u largest=%u min_ever=%u psram=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* --- dummy capability: home_ping --- */

static esp_err_t ping_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    snprintf(output, output_size, "{\"pong\":true}");
    ESP_LOGI(TAG, "dummy capability home_ping executed");
    return ESP_OK;
}

static const claw_cap_descriptor_t PING_CAP = {
    .id = "home_ping",
    .name = "home_ping",
    .family = "home",
    .description = "Test tool: returns pong.",
    .kind = CLAW_CAP_KIND_CALLABLE,
    .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
    .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
    .execute = ping_execute,
};

static const claw_cap_group_t HOME_GROUP = {
    .group_id = "home",
    .plugin_name = "claw_ha_spike",
    .version = "0.1.0",
    .descriptors = &PING_CAP,
    .descriptor_count = 1,
};

/* --- mock LLM backend (без сети/TLS): 1-й вызов -> tool_call, 2-й -> финал --- */

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
        out_response->tool_calls[0].name = strdup("home_ping");
        out_response->tool_calls[0].arguments_json = strdup("{}");
        /* Assistant-сообщение с tool_calls — как отдаёт реальный backend. */
        out_response->raw_message_json = strdup(
            "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
            "{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"home_ping\",\"arguments\":\"{}\"}}]}");
        ESP_LOGI(TAG, "mock LLM -> tool_call home_ping");
    } else {
        out_response->text = strdup("pong ok");
        out_response->raw_message_json =
            strdup("{\"role\":\"assistant\",\"content\":\"pong ok\"}");
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
    .id = "spike-mock",
    .vtable = &MOCK_VTABLE,
    .defaults = {
        .auth_type = "bearer",
        .chat_path = "/chat/completions",
        .max_tokens_field = "max_tokens",
    },
};

void app_main(void)
{
    probe("boot");

    ESP_ERROR_CHECK(claw_core_llm_register_custom_backend(&MOCK_REG));

    ESP_ERROR_CHECK(claw_cap_init());
    ESP_ERROR_CHECK(claw_cap_register_group(&HOME_GROUP));
    const char *visible[] = {"home"};
    ESP_ERROR_CHECK(claw_cap_set_llm_visible_groups(visible, 1));
    ESP_ERROR_CHECK(claw_cap_start_all());
    probe("after cap init");

    claw_core_config_t cfg = {0};
    cfg.api_key = "spike";
    cfg.backend_type = "spike-mock";
    cfg.model = "mock-model";
    cfg.base_url = "http://127.0.0.1";
    cfg.auth_type = "bearer";
    cfg.max_tokens_field = "max_tokens";
    cfg.timeout_ms = 5000;
    cfg.max_tokens = 256;
    cfg.image_max_bytes = 0;
    cfg.supports_tools = true;
    cfg.supports_vision = false;
    cfg.system_prompt = "You are a test agent. Use the home_ping tool when asked to ping.";
    cfg.call_cap = claw_cap_call_from_core;
    cfg.task_stack_size = 16 * 1024;
    cfg.task_priority = 5;
    cfg.task_core = tskNO_AFFINITY;
    cfg.max_tool_iterations = 4;
    cfg.request_queue_len = 4;
    cfg.response_queue_len = 4;
    cfg.max_context_providers = 4;

    claw_core_handle_t core = NULL;
    esp_err_t err = claw_core_create(&cfg, &core);
    ESP_LOGI(TAG, "claw_core_create: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        probe("core create FAILED");
        return;
    }
    probe("after core create");

    (void)claw_core_add_context_provider(core, &claw_cap_root_agent_tools_provider);
    ESP_ERROR_CHECK(claw_core_start(core));
    probe("core idle");

    claw_core_request_t req = {
        .request_id = 1,
        .session_id = "s1",
        .user_text = "ping",
    };
    err = claw_core_submit(core, &req, 2000);
    ESP_LOGI(TAG, "submit: %s", esp_err_to_name(err));
    probe("after submit");

    claw_core_response_t resp = {0};
    err = claw_core_receive(core, &resp, 15000);
    ESP_LOGI(TAG, "receive: %s status=%d text=%s",
             esp_err_to_name(err), (int)resp.status, resp.text ? resp.text : "(null)");
    probe("after response");

    claw_core_response_free(&resp);
    probe("final");
    ESP_LOGI(TAG, "spike done");
}
