#include "wh_knock.h"

#include <errno.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "wh_config.h"
#include "wh_trigger.h"

static const char *TAG = "wh_knock";

static TaskHandle_t s_task;
static volatile bool s_run;
static int s_listen_fd = -1;

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void knock_task(void *arg)
{
    (void)arg;
    const uint16_t port = (uint16_t)CONFIG_AA_KNOCK_PORT;

    while (s_run) {
        s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (s_listen_fd < 0) {
            ESP_LOGE(TAG, "socket failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int yes = 1;
        setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(TAG, "bind :%u failed errno=%d", (unsigned)port, errno);
            close_fd(&s_listen_fd);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (listen(s_listen_fd, 2) != 0) {
            ESP_LOGE(TAG, "listen failed errno=%d", errno);
            close_fd(&s_listen_fd);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Listening for TCP knock on :%u", (unsigned)port);

        while (s_run) {
            struct sockaddr_in peer = {0};
            socklen_t peer_len = sizeof(peer);
            int client = accept(s_listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (client < 0) {
                if (!s_run) {
                    break;
                }
                ESP_LOGW(TAG, "accept errno=%d", errno);
                break;
            }

            char ip[WH_IP_MAX];
            if (peer.sin_family == AF_INET) {
                if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) == NULL) {
                    ESP_LOGW(TAG, "Knock ignored — inet_ntop failed");
                    close(client);
                    continue;
                }
                ESP_LOGI(TAG, "Knock from %s", ip);
                close(client);
                wh_trigger_on_knock(ip);
            } else {
                ESP_LOGW(TAG, "Knock ignored — non-IPv4 peer");
                close(client);
            }
        }

        close_fd(&s_listen_fd);
    }

    ESP_LOGI(TAG, "Knock task exit");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wh_knock_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_run = true;
    BaseType_t ok = xTaskCreate(knock_task, "wh_knock", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        s_run = false;
        s_task = NULL;
        ESP_LOGE(TAG, "Failed to create knock task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
