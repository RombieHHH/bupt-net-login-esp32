/*
 * Copyright (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * 认证流程参考 YouXam/bupt-net-login (https://github.com/YouXam/bupt-net-login)
 */

#pragma once

/* ================================================================
 *  bupt_net.h — 北邮校园网自动登录驱动 (ESP32-C3)
 *
 *  单文件驱动，内聚全部功能：WiFi 连接、HTTP 探测、认证、串口日志。
 *  对外暴露 bupt_net_init() 和 bupt_net_run() 两个函数。
 *
 *  认证流程：
 *   1. WiFi 连接 "BUPT-portal"（无密码 WIFI_AUTH_OPEN）
 *   2. GET generate_204 → 204 已登录 / 30x → 未登录
 *   3. GET Location URL → 拿 Set-Cookie
 *   4. 把 URL 中的 "index" 替换为 "login"
 *   5. POST login_url + Cookie + 凭据
 *   6. GET generate_204 → 期望 204 验证
 * ================================================================ */

/* ================================================================
 *  [1] 依赖 include
 * ================================================================ */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "driver/uart.h"
#include "ping/ping_sock.h"

/* ================================================================
 *  [2] 常量宏
 * ================================================================ */
#define PROBE_URL        "http://connect.rom.miui.com/generate_204?cmd=redirect&arubalp=12345"
#define WIFI_SSID        "BUPT-portal"
#define NVS_NAMESPACE    "bupt"
#define NVS_KEY_USER     "user"
#define NVS_KEY_PASS     "pass"
#define HTTP_TIMEOUT_MS  10000
#define WIFI_MAX_RETRY   5

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ================================================================
 *  [3] 日志宏 — [LEVEL][HH:MM:SS] 格式，输出到串口
 * ================================================================ */
static inline void _log(const char *level, const char *fmt, ...) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    printf("[%s][%s] ", level, time_buf);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

#define LOG_INFO(fmt, ...)   _log("INFO   ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   _log("WARN   ", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  _log("ERROR  ", fmt, ##__VA_ARGS__)

/* ---- 逐字符读取一行（UART 驱动，阻塞等待不会触发 task_wdt）---- */
static void read_line(char *buf, size_t len) {
    size_t i = 0;
    char c;
    while (i < len - 1) {
        /* 阻塞等待字符（portMAX_DELAY：阻塞态，不消耗 CPU，不触发 watchdog） */
        if (uart_read_bytes(UART_NUM_0, (uint8_t *)&c, 1, portMAX_DELAY) == 1) {
            if (c == '\r' || c == '\n') {
                printf("\r\n");
                fflush(stdout);
                break;
            }
            if (c == '\b' || c == 127) {
                if (i > 0) {
                    i--;
                    printf("\b \b");
                    fflush(stdout);
                }
                continue;
            }
            buf[i++] = c;
            printf("%c", c);
            fflush(stdout);
        }
    }
    buf[i] = '\0';
}

/* ================================================================
 *  [4] NVS 读写 — 凭据持久化存储
 * ================================================================ */
static esp_err_t load_creds(char *user, size_t user_len,
                            char *pass, size_t pass_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = user_len;
    err = nvs_get_str(handle, NVS_KEY_USER, user, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = pass_len;
    err = nvs_get_str(handle, NVS_KEY_PASS, pass, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t save_creds(const char *user, const char *pass) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, NVS_KEY_USER, user);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* ================================================================
 *  [5] WiFi 连接 — EventGroup 等待 GOT_IP
 * ================================================================ */
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry = 0;
static bool s_wifi_connected = false;
static esp_netif_t *s_netif = NULL;

static void log_ipv6(const char *msg, const esp_ip6_addr_t *ip6) {
    char buf[40];
    const uint8_t *b = (const uint8_t *)ip6->addr;
    snprintf(buf, sizeof(buf),
        "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
        "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
        (unsigned int)b[0], (unsigned int)b[1],
        (unsigned int)b[2], (unsigned int)b[3],
        (unsigned int)b[4], (unsigned int)b[5],
        (unsigned int)b[6], (unsigned int)b[7],
        (unsigned int)b[8], (unsigned int)b[9],
        (unsigned int)b[10], (unsigned int)b[11],
        (unsigned int)b[12], (unsigned int)b[13],
        (unsigned int)b[14], (unsigned int)b[15]);
    LOG_INFO("%s: %s", msg, buf);
}

/* ---- IPv6 连通性测试：ping 2400:3200::1（中国电信 IPv6 DNS） ---- */
static SemaphoreHandle_t s_ping_done = NULL;
static uint32_t s_ping_sent = 0, s_ping_recv = 0;

static void ping_on_success(esp_ping_handle_t h, void *ctx) {
    s_ping_recv++;
}

static void ping_on_timeout(esp_ping_handle_t h, void *ctx) {}

static void ping_on_end(esp_ping_handle_t h, void *ctx) {
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &s_ping_sent, sizeof(s_ping_sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &s_ping_recv, sizeof(s_ping_recv));
    if (s_ping_sent > 0 && s_ping_recv == s_ping_sent) {
        LOG_INFO("IPv6 ping: OK (0%% loss)");
    } else if (s_ping_sent > 0) {
        uint32_t loss = ((s_ping_sent - s_ping_recv) * 100) / s_ping_sent;
        LOG_WARN("IPv6 ping: %lu%% loss (%lu/%lu)",
                 (unsigned long)loss, (unsigned long)s_ping_recv, (unsigned long)s_ping_sent);
    } else {
        LOG_WARN("IPv6 ping: no reply");
    }
    if (s_ping_done) xSemaphoreGive(s_ping_done);
}

static void ping_ipv6_check(void) {
    ip_addr_t target_addr;
    ipaddr_aton("2400:3200::1", &target_addr);

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 10;
    ping_config.timeout_ms = 2000;
    ping_config.task_stack_size = 2048;
    ping_config.task_prio = 2;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
        .cb_args = NULL,
    };

    s_ping_done = xSemaphoreCreateBinary();
    s_ping_sent = 0;
    s_ping_recv = 0;
    esp_ping_handle_t ping;
    esp_ping_new_session(&ping_config, &cbs, &ping);
    esp_ping_start(ping);

    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        LOG_WARN("IPv6 ping: timeout");
        esp_ping_stop(ping);
    }
    esp_ping_delete_session(ping);
    vSemaphoreDelete(s_ping_done);
    s_ping_done = NULL;
}

/* ---- 每次探测循环中打印 IPv6 状态 ---- */
static void print_ipv6_status(void) {
    if (!s_netif) { LOG_WARN("IPv6: no netif"); return; }
    bool got = false;
    esp_ip6_addr_t ip6;

    if (esp_netif_get_ip6_global(s_netif, &ip6) == ESP_OK) {
        log_ipv6("IPv6 global", &ip6);
        got = true;
    }

    if (!got) {
        LOG_WARN("IPv6: no address assigned yet");
        return;
    }

    ping_ipv6_check();
}

/* ---- 等待并打印 IPv6 地址（最多等 10 秒，登录后调用） ---- */
static void wait_ipv6(void) {
    if (!s_netif) return;

    esp_ip6_addr_t ip6;

    /* 等待 global 地址（SLAAC 需要 RA，可能延迟几秒） */
    for (int i = 0; i < 20; i++) {
        if (esp_netif_get_ip6_global(s_netif, &ip6) == ESP_OK) {
            log_ipv6("IPv6 global", &ip6);
            ping_ipv6_check();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    LOG_WARN("No IPv6 global address within 10s (network may not support SLAAC)");
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev =
            (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        if (s_wifi_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry++;
            LOG_WARN("WiFi disconnected (reason=%d), retry %d/%d",
                     ev->reason, s_wifi_retry, WIFI_MAX_RETRY);
        } else {
            LOG_ERROR("WiFi failed after %d retries", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        LOG_INFO("Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        if (((const uint8_t *)event->ip6_info.ip.addr)[0] == 0x20) {
            log_ipv6("IPv6 global", &event->ip6_info.ip);
        }
    }
}

static void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6,
                               &wifi_event_handler, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);
}

static esp_err_t wifi_connect(void) {
    LOG_INFO("Connecting to %s...", WIFI_SSID);

    /* 清除旧的事件位 */
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_wifi_retry = 0;

    wifi_config_t wifi_config = { 0 };
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    /* 等待连接成功或失败 */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        /* 启用 IPv6（link-local 地址是 SLAAC 的必要前提） */
        if (s_netif) {
            esp_netif_create_ip6_linklocal(s_netif);
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ---- 响应头捕获上下文（用于 HTTP 事件回调） ---- */
typedef struct {
    char location[256];
    char set_cookie[256];
    int  status;
} http_resp_ctx_t;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    http_resp_ctx_t *ctx = (http_resp_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx && evt->header_key && evt->header_value) {
        LOG_INFO("  Header: %s = %s", evt->header_key, evt->header_value);
        if (strcasecmp(evt->header_key, "Location") == 0 && !ctx->location[0]) {
            strncpy(ctx->location, evt->header_value, sizeof(ctx->location) - 1);
        }
        if (strcasecmp(evt->header_key, "Set-Cookie") == 0 && !ctx->set_cookie[0]) {
            strncpy(ctx->set_cookie, evt->header_value, sizeof(ctx->set_cookie) - 1);
        }
    }
    return ESP_OK;
}

/* ---- 执行一次 HTTP 请求，返回响应状态码，ctx 带回响应头 ---- */
static int http_request(const char *url,
                        const char *method,
                        const char *cookie,
                        const char *body,
                        http_resp_ctx_t *ctx) {
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method ? (strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET) : HTTP_METHOD_GET,
        .max_redirection_count = 0,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = _http_event_handler,
        .user_data = ctx,
        .skip_cert_common_name_check = true,
    };

    memset(ctx, 0, sizeof(*ctx));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    if (cookie && strlen(cookie) > 0) {
        esp_http_client_set_header(client, "Cookie", cookie);
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type",
                                   "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ctx->status = status;

    if (err != ESP_OK) {
        LOG_ERROR("HTTP request failed: %s (status=%d)",
                  esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    return status;
}

/* ================================================================
 *  [6] HTTP 探测 — GET generate_204，返回三种状态
 *
 *  关键: max_redirection_count = 0，禁止自动跟 302
 * ================================================================ */
typedef enum {
    PROBE_LOGGED_IN,
    PROBE_NEED_LOGIN,
    PROBE_ERROR,
} probe_result_t;

static probe_result_t http_probe(char *auth_url, size_t auth_url_len) {
    LOG_INFO("Probing...");

    http_resp_ctx_t ctx;
    int status = http_request(PROBE_URL, "GET", NULL, NULL, &ctx);

    LOG_INFO("Probe status: %d", status);
    if (ctx.location[0]) LOG_INFO("Location: %s", ctx.location);

    if (status == 204) {
        return PROBE_LOGGED_IN;
    }

    if (status >= 300 && status < 400) {
        if (ctx.location[0] && strstr(ctx.location, "10.3.8")) {
            LOG_INFO("Auth server: %s", ctx.location);
            if (auth_url && auth_url_len > 0) {
                strncpy(auth_url, ctx.location, auth_url_len - 1);
                auth_url[auth_url_len - 1] = '\0';
            }
            return PROBE_NEED_LOGIN;
        }
    }

    return PROBE_ERROR;
}

/* ================================================================
 *  [7] HTTP 认证 — 三步：拿 Cookie → POST 登录 → 验证
 *
 *  返回 true 表示登录成功
 * ================================================================ */
static bool http_authenticate(const char *auth_url,
                              const char *user, const char *pass) {
    char session[128] = {0};
    char login_url[256] = {0};

    /* ---- Step 1/3: 获取 Cookie ---- */
    LOG_INFO("Step 1/3: Fetching cookie...");
    {
        http_resp_ctx_t ctx;
        int status = http_request(auth_url, "GET", NULL, NULL, &ctx);

        if (ctx.set_cookie[0]) {
            /* 截取第一个分号前的内容 */
            const char *semi = strchr(ctx.set_cookie, ';');
            size_t n = semi ? (size_t)(semi - ctx.set_cookie)
                            : strlen(ctx.set_cookie);
            memcpy(session, ctx.set_cookie, n);
            session[n] = '\0';
            LOG_INFO("Session: %s", session);
        } else {
            LOG_ERROR("No Set-Cookie in response (status=%d)", status);
            return false;
        }
    }

    /* ---- 构造 login URL：index → login ---- */
    strncpy(login_url, auth_url, sizeof(login_url) - 1);
    char *p = strstr(login_url, "index");
    if (p) {
        memcpy(p, "login", 5);  /* 原地替换 5 字节 */
    } else {
        LOG_ERROR("Cannot find 'index' in auth URL");
        return false;
    }

    /* ---- Step 2/3: POST 登录 ---- */
    LOG_INFO("Step 2/3: Posting login...");
    char body[128];
    snprintf(body, sizeof(body), "user=%s&pass=%s", user, pass);

    {
        http_resp_ctx_t ctx;
        int status = http_request(login_url, "POST", session, body, &ctx);
        LOG_INFO("POST returned %d", status);
    }

    /* ---- Step 3/3: 验证登录 ---- */
    LOG_INFO("Step 3/3: Verifying...");
    {
        http_resp_ctx_t ctx;
        int status = http_request(PROBE_URL, "GET", NULL, NULL, &ctx);

        if (status == 204) {
            LOG_INFO("Verify: 204 — LOGIN SUCCESS");
            return true;
        }
        LOG_ERROR("Verify: %d — LOGIN FAILED", status);
    }
    return false;
}

/* ================================================================
 *  [8] 对外接口
 * ================================================================ */

/**
 * @brief 初始化 NVS + WiFi + 串口
 *
 * 必须在使用其他功能前调用一次。
 */
void bupt_net_init(void) {
    /* 初始化 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 初始化 WiFi */
    wifi_init();
}

/**
 * @brief 执行一次登录检查（阻塞），支持保活循环
 *
 * @param user          学号，为 NULL 则从 NVS 读取或串口询问
 * @param pass          密码，为 NULL 则从 NVS 读取或串口询问
 * @param keepalive_sec 保活间隔（秒），0=单次执行，>0=循环保活
 */
void bupt_net_run(const char *user, const char *pass, int keepalive_sec) {
    char username[64] = {0};
    char password[64] = {0};

    /* ---- 获取凭据 ---- */
    if (user && pass) {
        /* 调用者直接传入 */
        strncpy(username, user, sizeof(username) - 1);
        strncpy(password, pass, sizeof(password) - 1);
    } else {
        /* 尝试从 NVS 读取 */
        esp_err_t err = load_creds(username, sizeof(username),
                                   password, sizeof(password));
        if (err != ESP_OK) {
            /* NVS 没有凭据，串口交互询问 */
            LOG_INFO("No credentials in NVS, please enter:");
            vTaskDelay(pdMS_TO_TICKS(200));

            /* 确保 UART 驱动已安装 — 使 uart_read_bytes 可用 */
            esp_err_t uart_err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
            if (uart_err == ESP_ERR_INVALID_STATE) {
                uart_err = ESP_OK;  /* 已安装，正常 */
            }
            uart_flush_input(UART_NUM_0);  /* 排空启动时的残留噪声 */

            /* 读取用户名 */
            printf("Username: ");
            fflush(stdout);
            read_line(username, sizeof(username));
            while (strlen(username) == 0) {
                printf("Username: ");
                fflush(stdout);
                read_line(username, sizeof(username));
            }

            /* 读取密码 */
            printf("Password: ");
            fflush(stdout);
            read_line(password, sizeof(password));
            while (strlen(password) == 0) {
                printf("Password: ");
                fflush(stdout);
                read_line(password, sizeof(password));
            }

            /* 确认并写入 NVS */
            char confirm[8];
            printf("Save to NVS? (y/n): ");
            fflush(stdout);
            do {
                read_line(confirm, sizeof(confirm));
            } while (strlen(confirm) == 0);

            if (confirm[0] == 'y' || confirm[0] == 'Y') {
                save_creds(username, password);
                LOG_INFO("Credentials saved to NVS");
            }
        } else {
            LOG_INFO("Loaded credentials from NVS");
        }
    }
    LOG_INFO("User: %s", username);

    /* ---- 首次连接 WiFi ---- */
    if (wifi_connect() != ESP_OK) {
        LOG_ERROR("Initial WiFi connection failed");
        return;
    }

    /* ---- 主循环 ---- */
    int fail_count = 0;
    while (1) {
        /* 检查 WiFi 状态，必要时重连 */
        if (!s_wifi_connected) {
            LOG_WARN("WiFi lost, reconnecting...");
            s_wifi_retry = 0;
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
            esp_wifi_connect();
            EventBits_t bits = xEventGroupWaitBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
            if (bits & WIFI_FAIL_BIT) {
                LOG_ERROR("Reconnection failed, retrying in 30s...");
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
        }

        /* 探测当前状态 */
        char auth_url[256] = {0};
        probe_result_t probe = http_probe(auth_url, sizeof(auth_url));

        if (probe == PROBE_LOGGED_IN) {
            LOG_INFO("Already logged in");
            print_ipv6_status();
            fail_count = 0;
            if (keepalive_sec <= 0) break;
            LOG_INFO("Sleeping %ds...", keepalive_sec);
            vTaskDelay(pdMS_TO_TICKS(keepalive_sec * 1000));
            continue;
        }

        if (probe == PROBE_NEED_LOGIN) {
            LOG_INFO("Status: 302 — need login");
            bool ok = http_authenticate(auth_url, username, password);
            if (ok) {
                /* 认证成功后才下发 IPv6 RA，等待获取 */
                LOG_INFO("Waiting for IPv6 SLAAC...");
                wait_ipv6();
                print_ipv6_status();
                fail_count = 0;
                if (keepalive_sec <= 0) break;
                LOG_INFO("Sleeping %ds...", keepalive_sec);
                vTaskDelay(pdMS_TO_TICKS(keepalive_sec * 1000));
                continue;
            }
            LOG_ERROR("Authentication failed");
        } else {
            LOG_ERROR("Probe error");
        }

        /* 失败处理 */
        fail_count++;
        if (keepalive_sec <= 0) break;

        int delay_s = (fail_count <= 3) ? 30 : 60;
        LOG_WARN("Retrying in %ds... (fail #%d)", delay_s, fail_count);
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
    }

    LOG_INFO("bupt_net_run finished");
}
