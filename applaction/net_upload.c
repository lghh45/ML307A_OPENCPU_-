#include "net_upload.h"
#include "app.h"
#include "cm_http.h"
#include "cm_ssl.h"
#include "cm_mem.h"
#include "cm_sys.h"
#include "cm_sim.h"
#include "cm_modem.h"
#include "cm_rtc.h"
#include "cm_pm.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>


#define UPLOAD_URL      "http://127.0.0.0:1" //服务器地址
#define UPLOAD_PATH     "/api/sms/report"
#define UPLOAD_USE_SSL  0 

/* ===== 超时与重试 ===== */
#define UPLOAD_SSL_ID       2
#define UPLOAD_CONN_TIMEOUT 30 
#define UPLOAD_RSP_TIMEOUT  30 
#define UPLOAD_RETRY_MIN_MS 2000
#define UPLOAD_RETRY_MAX_MS 60000
#define PDP_CID             1


#define NET_POLL_MIN_MS     2000
#define NET_POLL_MAX_MS     30000
#define NET_LOG_PERIOD_MS   30000

static cm_httpclient_handle_t s_client = NULL;
static char                   s_imei[CM_IMEI_LEN] = {0};
static char                   s_imsi[CM_IMSI_LEN + 1] = {0};
static char                   s_body[2048];

static bool net_ready(void)
{
    bool ready;

    cm_pm_work_lock();
    ready = (cm_modem_get_pdp_state(PDP_CID) == 1);
    cm_pm_work_unlock();

    return ready;
}

/**
等待网络成功
 */
static void net_wait_ready(void)
{
    uint32_t interval  = NET_POLL_MIN_MS;
    uint32_t since_log = 0;

    u0_printf("[up] waiting for network...\r\n");

    while (!net_ready())
    {
        osDelayMs(interval);
        since_log += interval;

        if (interval < NET_POLL_MAX_MS)
        {
            interval *= 2;
            if (interval > NET_POLL_MAX_MS)
            {
                interval = NET_POLL_MAX_MS;
            }
        }

        if (since_log >= NET_LOG_PERIOD_MS)
        {
            bool sim_ready;

            since_log = 0;
            cm_pm_work_lock();
            sim_ready = (cm_modem_get_cpin() == 0);
            cm_pm_work_unlock();
            u0_printf("[up] network not ready, sim=%s\r\n",
                      sim_ready ? "ready" : "not ready");
        }
    }

    u0_printf("[up] network ready\r\n");
}

int net_upload_init(void)
{
    cm_httpclient_ret_code_e ret;
    cm_httpclient_cfg_t      cfg;

    if (s_imei[0] == '\0')
    {
        if (cm_sys_get_imei(s_imei) != 0)
        {
            u0_printf("[up] get imei failed\r\n");
        }
    }

    if (s_imsi[0] == '\0')
    {
        if (cm_sim_get_imsi(s_imsi) != 0)
        {
            u0_printf("[up] get imsi failed\r\n");
        }
    }

    if (s_client != NULL)
    {
        return 0;
    }

    cm_pm_work_lock();

    ret = cm_httpclient_create((const uint8_t *)UPLOAD_URL, NULL, &s_client);
    if (ret != CM_HTTP_RET_CODE_OK || s_client == NULL)
    {
        u0_printf("[up] create client failed: %d\r\n", (int)ret);
        s_client = NULL;
        cm_pm_work_unlock();
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.ssl_enable   = UPLOAD_USE_SSL;
    cfg.ssl_id       = UPLOAD_SSL_ID;
    cfg.cid          = PDP_CID;
    cfg.conn_timeout = UPLOAD_CONN_TIMEOUT;
    cfg.rsp_timeout  = UPLOAD_RSP_TIMEOUT;
    cfg.dns_priority = 1; /* v4 优先 */

    ret = cm_httpclient_set_cfg(s_client, cfg);
    if (ret != CM_HTTP_RET_CODE_OK)
    {
        u0_printf("[up] set cfg failed: %d\r\n", (int)ret);
        cm_httpclient_delete(s_client);
        s_client = NULL;
        cm_pm_work_unlock();
        return -1;
    }

#if UPLOAD_USE_SSL
    {
        int verify = 0;
        cm_ssl_setopt(UPLOAD_SSL_ID, CM_SSL_PARAM_VERIFY, &verify);
    }
#endif

    cm_pm_work_unlock();
    u0_printf("[up] client ready, imei=%s imsi=%s\r\n", s_imei, s_imsi);
    return 0;
}

int net_upload_post(const sms_item_t *item)
{
    cJSON                        *root = NULL;
    char                         *body = NULL;
    char                          msg_id[48];
    uint32_t                      body_len;
    cm_httpclient_sync_param_t    param;
    cm_httpclient_sync_response_t resp;
    cm_httpclient_ret_code_e      ret;
    int                           rc = -1;

    if (item == NULL || s_client == NULL || !net_ready())
    {
        return -1;
    }

    snprintf(msg_id, sizeof(msg_id), "%s-%08u", s_imei, (unsigned)item->seq);

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return -1;
    }
    cJSON_AddStringToObject(root, "msg_id", msg_id);
    cJSON_AddStringToObject(root, "imei", s_imei);
    cJSON_AddStringToObject(root, "imsi", s_imsi);
    cJSON_AddStringToObject(root, "sender", item->sender);
    cJSON_AddStringToObject(root, "scts", item->timestamp);
    cJSON_AddNumberToObject(root, "ts", (double)cm_rtc_get_current_time());
    cJSON_AddStringToObject(root, "content", item->text);

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        return -1;
    }

    body_len = (uint32_t)strlen(body);
    if (body_len >= sizeof(s_body))
    {
        u0_printf("[up] json too long: %u\r\n", (unsigned)body_len);
        cm_free(body);
        return -1;
    }
    memcpy(s_body, body, body_len + 1);
    cm_free(body);

    memset(&param, 0, sizeof(param));
    param.method         = HTTPCLIENT_REQUEST_POST;
    param.path           = (const uint8_t *)UPLOAD_PATH;
    param.content_length = body_len;
    param.content        = (uint8_t *)s_body;

    memset(&resp, 0, sizeof(resp));
    cm_pm_work_lock();
    ret = cm_httpclient_sync_request(s_client, param, &resp);

    if (ret == CM_HTTP_RET_CODE_OK && resp.response_code == 200)
    {
        rc = 0;
    }
    else
    {
        u0_printf("[up] post failed: ret=%d http=%u\r\n", (int)ret, (unsigned)resp.response_code);
    }

    if (resp.response_content != NULL && resp.response_content_len > 0)
    {
        u0_printf("[up] resp: %.*s\r\n", (int)resp.response_content_len,
                  (const char *)resp.response_content);
    }

    cm_httpclient_sync_free_data(s_client);
    cm_pm_work_unlock();
    return rc;
}

void upload_task(void *argument)
{
    sms_item_t item;
    uint32_t   backoff_ms = UPLOAD_RETRY_MIN_MS;

    (void)argument;

    u0_printf("[up] task start\r\n");

    net_wait_ready();
    net_upload_init();

    for (;;)
    {
        memset(&item, 0, sizeof(item));

        if (!sms_queue_peek(&item))
        {
            backoff_ms = UPLOAD_RETRY_MIN_MS;
            sms_queue_wait();
            continue;
        }

        if (s_client == NULL)
        {
            net_upload_init();
            if (s_client == NULL)
            {
                osDelayMs(UPLOAD_RETRY_MIN_MS);
                continue;
            }
        }

        if (net_upload_post(&item) == 0)
        {
            sms_queue_pop_head();
            backoff_ms = UPLOAD_RETRY_MIN_MS;
            u0_printf("[up] ok, %u left\r\n", (unsigned)sms_queue_count());
        }
        else
        {
            u0_printf("[up] failed, retry in %u ms\r\n", (unsigned)backoff_ms);
            osDelayMs(backoff_ms);
            backoff_ms = (backoff_ms < UPLOAD_RETRY_MAX_MS) ? (backoff_ms * 2) : UPLOAD_RETRY_MAX_MS;
        }
    }
}
