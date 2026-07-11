#include "bupt_net.h"

#if BUPT_NET_ENABLE_IPV6

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"

#if BUPT_NET_ENABLE_IPV6_PING
#include "freertos/semphr.h"
#include "ping/ping_sock.h"
#endif

#define IPV6_READY_BIT BIT0
#define IPV6_WAIT_MS 10000
#define IPV6_PING_COUNT 10
#define IPV6_PING_TIMEOUT_MS 2000

static esp_netif_t *s_netif;
static EventGroupHandle_t s_events;
static bool s_address_logged;

#if BUPT_NET_ENABLE_IPV6_PING
static SemaphoreHandle_t s_ping_done;
static uint32_t s_ping_sent;
static uint32_t s_ping_recv;

static void ping_success(esp_ping_handle_t handle, void *ctx)
{
    s_ping_recv++;
}

static void ping_timeout(esp_ping_handle_t handle, void *ctx) {}

static void ping_end(esp_ping_handle_t handle, void *ctx)
{
    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST,
                         &s_ping_sent, sizeof(s_ping_sent));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY,
                         &s_ping_recv, sizeof(s_ping_recv));
    if (s_ping_sent && s_ping_recv == s_ping_sent) {
        printf("[INFO   ] IPv6 ping: OK (0%% loss)\n");
    } else if (s_ping_sent) {
        uint32_t loss = ((s_ping_sent - s_ping_recv) * 100) / s_ping_sent;
        printf("[WARN   ] IPv6 ping: %lu%% loss (%lu/%lu)\n",
               (unsigned long)loss, (unsigned long)s_ping_recv,
               (unsigned long)s_ping_sent);
    } else {
        printf("[WARN   ] IPv6 ping: no reply\n");
    }
    if (s_ping_done) xSemaphoreGive(s_ping_done);
}

static void ping_check(void)
{
    ip_addr_t target;
    if (!ipaddr_aton(BUPT_NET_IPV6_PING_TARGET, &target)) {
        printf("[ERROR  ] Invalid IPv6 ping target\n");
        return;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = IPV6_PING_COUNT;
    config.timeout_ms = IPV6_PING_TIMEOUT_MS;
    config.task_stack_size = 2048;
    config.task_prio = 2;
    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_success,
        .on_ping_timeout = ping_timeout,
        .on_ping_end = ping_end,
    };

    s_ping_done = xSemaphoreCreateBinary();
    s_ping_sent = s_ping_recv = 0;
    esp_ping_handle_t ping = NULL;
    if (!s_ping_done || esp_ping_new_session(&config, &callbacks, &ping) != ESP_OK) {
        printf("[ERROR  ] Failed to start IPv6 ping\n");
        if (s_ping_done) vSemaphoreDelete(s_ping_done);
        s_ping_done = NULL;
        return;
    }

    esp_ping_start(ping);
    uint32_t wait_ms = IPV6_PING_COUNT * IPV6_PING_TIMEOUT_MS + 1000;
    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        printf("[WARN   ] IPv6 ping: timeout\n");
        esp_ping_stop(ping);
    }
    esp_ping_delete_session(ping);
    vSemaphoreDelete(s_ping_done);
    s_ping_done = NULL;
}
#endif

void bupt_ipv6_init(esp_netif_t *netif)
{
    s_netif = netif;
    s_events = xEventGroupCreate();
}

void bupt_ipv6_reset(void)
{
    if (s_events) xEventGroupClearBits(s_events, IPV6_READY_BIT);
}

void bupt_ipv6_on_got_ip(const ip_event_got_ip6_t *event)
{
    if (s_events && esp_netif_ip6_get_addr_type(&event->ip6_info.ip) ==
                    ESP_IP6_ADDR_IS_GLOBAL) {
        xEventGroupSetBits(s_events, IPV6_READY_BIT);
    }
}

void bupt_ipv6_start(void)
{
    if (s_netif) esp_netif_create_ip6_linklocal(s_netif);
}

void bupt_ipv6_wait(void)
{
    if (!s_netif || !s_events) return;
    EventBits_t bits = xEventGroupWaitBits(
        s_events, IPV6_READY_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(IPV6_WAIT_MS));
    if (!(bits & IPV6_READY_BIT)) {
        printf("[WARN   ] IPv6 ping: unavailable (SLAAC timeout)\n");
        return;
    }

    if (!s_address_logged) {
        esp_ip6_addr_t ip6;
        if (esp_netif_get_ip6_global(s_netif, &ip6) == ESP_OK) {
            printf("[INFO   ] IPv6 global: " IPV6STR "\n", IPV62STR(ip6));
            s_address_logged = true;
        }
    }
#if BUPT_NET_ENABLE_IPV6_PING
    ping_check();
#endif
}

#endif
