#include "sms_recv.h"
#include "sms_queue.h"
#include "sms_pdu.h"
#include "app.h"
#include "cm_virt_at.h"
#include "cm_os.h"
#include "cm_modem.h"
#include "cm_modem_info.h"
#include "cm_pm.h"
#include "cm_rtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define AT_BUF_SIZE     1024
#define AT_TIMEOUT_MS   10000

/* 调试开关：打开后
 *   1) 把所有主动上报（URC）原文打到日志
 *   2) 每 SMS_SCAN_PERIOD_MS 把短信存储里的内容列一遍
 * 用来区分"URC 没上报"和"短信根本没到模块"，定位完可以改成 0。
 */
#define SMS_DEBUG_SCAN      0
#define SMS_POLL_PERIOD_MS  30000
#define SMS_SCAN_PERIOD_MS  SMS_POLL_PERIOD_MS

#define SMS_CONCAT_MAX_GROUPS  5
#define SMS_CONCAT_MAX_PARTS   10
#define SMS_PART_TEXT_MAX      256
#define SMS_CONCAT_TIMEOUT_MS  30000

/* eDRX：让模组按约定周期休眠，每个周期只在 PTW 窗口内监听寻呼，
 * 空闲电流明显低于默认 DRX，代价是收短信的延迟约为周期的一半。
 * 20.48 s 对应 LTE 的 eDRX 值 "0010"，实际周期由网络在 Attach / TAU
 * 时批准，可用 AT+CEDRXS? 和 +CEDRXP 核对。
 */
#define EDRX_CFG_ENABLE  1
#define EDRX_MODE        1       /* 1 仅开启，2 开启并上报 +CEDRXP */
#define EDRX_ACT_TYPE    4       /* 4 = E-UTRAN(LTE) */
#define EDRX_VALUE       "0010"  /* LTE: 0010 = 20.48 s */

/* 来电检测：只上报"有没有人来电 + 主叫号码"，不接听。
 * 该固件的 AT 层没有 ATA/CHUP，能用的只有 RING 和 +CLIP 两条 URC：
 * 一通电话会先来 RING，+CLIP 带号码，之后每 3~5 秒重复 RING，
 * 所以首声 RING 先开一个收集窗口，把两者凑齐后再入队。
 */
#define CALL_REPORT_ENABLE   1
#define CALL_CLIP_WINDOW_MS  2000   /* 首声 RING 之后等 +CLIP 的收集窗口 */
#define CALL_DEDUP_MS        60000  /* 同一通电话重复响铃的忽略时长 */
#define CALL_CLIP_BUF_SIZE   96

typedef struct
{
    bool     used;
    int      ref_number;
    int      total_parts;
    int      received_parts;
    uint32_t first_tick;
    char     sender[SMS_SENDER_MAX];
    char     timestamp[SMS_TIME_MAX];
    bool     part_valid[SMS_CONCAT_MAX_PARTS];
    char     parts[SMS_CONCAT_MAX_PARTS][SMS_PART_TEXT_MAX];
} sms_concat_t;

/** 一次 AT 事务的上下文 */
typedef struct
{
    volatile int done;
    volatile int result; /* 0 OK / 1 CME / 2 参数错 / 3 超时 / 4 系统错 */
    uint32_t     len;
    char         buf[AT_BUF_SIZE];
} at_ctx_t;

static at_ctx_t        s_at;
static char            s_resp[AT_BUF_SIZE]; 
static osSemaphoreId_t s_at_done = NULL;
static osMutexId_t     s_at_lock = NULL;

static osEventFlagsId_t s_evt = NULL;

/* +CMT 下整条短信就在 URC 里，先拷出来交给任务线程处理。
 * URC 回调里不做耗时动作，只拷一份、置个标志。
 */
static char         s_cmt_buf[AT_BUF_SIZE];
static volatile int s_cmt_pending = 0;
#define EVT_SMS_CMT   (1u << 0)
#define EVT_CONCAT_TIMEOUT (1u << 1)

#if CALL_REPORT_ENABLE
static char         s_clip_buf[CALL_CLIP_BUF_SIZE];
static volatile int s_clip_pending = 0;
static volatile int s_ring_pending = 0;
static uint32_t     s_clip_tick = 0;
static bool         s_call_collecting = false;
static uint32_t     s_call_window_tick = 0;
static uint32_t     s_call_last_tick = 0;
static bool         s_call_reported = false;
#define EVT_CALL_RING  (1u << 2)
#define EVT_CALL_CLIP  (1u << 3)
#define EVT_WAIT_MASK  (EVT_SMS_CMT | EVT_CONCAT_TIMEOUT | EVT_CALL_RING | EVT_CALL_CLIP)
#else
#define EVT_WAIT_MASK  (EVT_SMS_CMT | EVT_CONCAT_TIMEOUT)
#endif

static sms_concat_t s_concat[SMS_CONCAT_MAX_GROUPS];
static osTimerId_t  s_concat_timer = NULL;

static void concat_timer_cb(void *argument)
{
    (void)argument;

    if (s_evt != NULL)
    {
        osEventFlagsSet(s_evt, EVT_CONCAT_TIMEOUT);
    }
}

/* --------------------------------------------------------------------------
 * AT 收发
 * ------------------------------------------------------------------------ */

static void at_resp_cb(void *param)
{
    s_at.result = atoi((char *)param);

    while (s_at.len < sizeof(s_at.buf) - 1)
    {
        int32_t n = cm_virt_at_get((unsigned char *)s_at.buf + s_at.len,
                                   (int)(sizeof(s_at.buf) - 1 - s_at.len));
        if (n <= 0)
        {
            break;
        }
        s_at.len += (uint32_t)n;
    }
    s_at.buf[s_at.len] = '\0';

    cm_virt_at_deinit();
    s_at.done = 1;
    osSemaphoreRelease(s_at_done);
}

/**
 * @brief 执行一条 AT 指令并取回响应
 *
 * @param cmd      AT 指令，不含结尾的 CRLF
 * @param out      响应缓冲，可为 NULL
 * @param out_size 响应缓冲长度
 *
 * @return 0 指令返回 OK，其他失败
 *
 * @details cm_virt_at.h 注意事项 3 提到，响应过长时需要多次发指令逐次读取。
 *          若实测发现 +CMT PDU 被截断，需要在这里补重发续读的逻辑。
 */
static int at_exec(const char *cmd, char *out, uint32_t out_size)
{
    char line[96];
    int  ret = -1;

    if (s_at_lock == NULL || s_at_done == NULL)
    {
        return -1;
    }

    if (osMutexAcquire(s_at_lock, osMsToTicks(AT_TIMEOUT_MS)) != osOK)
    {
        return -1;
    }

    cm_pm_work_lock();

    s_at.len    = 0;
    s_at.done   = 0;
    s_at.result = -1;
    s_at.buf[0] = '\0';

    snprintf(line, sizeof(line), "%s\r\n", cmd);

    if (cm_virt_at_init(at_resp_cb) != 0)
    {
        u0_printf("[at] init failed: %s\r\n", cmd);
    }
    else if (cm_virt_at_send((uint8_t *)line, (int32_t)strlen(line)) < 0)
    {
        u0_printf("[at] send failed: %s\r\n", cmd);
        cm_virt_at_deinit();
    }
    else if (osSemaphoreAcquire(s_at_done, osMsToTicks(AT_TIMEOUT_MS)) != osOK)
    {
        u0_printf("[at] timeout: %s\r\n", cmd);
        cm_virt_at_deinit();
    }
    else
    {
        /* 原样返回底层结果码，便于区分失败原因：
         * 0 OK / 1 CME错误 / 2 参数无效 / 3 超时 / 4 系统错误
         * 负数表示连指令都没发出去（init/send/互斥或等应答失败）
         */
        ret = s_at.result;

        if (out != NULL && out_size > 0)
        {
            uint32_t n = (s_at.len < out_size - 1) ? s_at.len : (out_size - 1);
            memcpy(out, s_at.buf, n);
            out[n] = '\0';
        }
    }

    osMutexRelease(s_at_lock);
    cm_pm_work_unlock();
    return ret;
}

/* --------------------------------------------------------------------------
 * 主动上报
 * ------------------------------------------------------------------------ */

/**
 * @brief 把可能带换行的原始数据打成一行，方便看日志
 *
 * @param tag 前缀
 * @param s   原始数据
 */
static void log_text(const char *tag, const char *s)
{
    char     buf[512];
    uint32_t k = 0;

    if (s == NULL)
    {
        return;
    }

    while (*s != '\0' && k < sizeof(buf) - 1)
    {
        char c = *s++;

        buf[k++] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    buf[k] = '\0';

    u0_printf("%s%s\r\n", tag, buf);
}

static void urc_cb(char *urc)
{
    size_t len;

    if (urc == NULL)
    {
        return;
    }

    log_text("[urc] ", urc);

#if CALL_REPORT_ENABLE
    /* 来电的两条 URC：+CLIP 带主叫号码，RING 表示有来电。
     * 这里只拷贝和置位，解析和入队交给 sms_recv_task。
     * 用 strstr 而不是 strncmp，是因为底层可能把几行 URC 一起交上来。
     */
    if (strstr(urc, "+CLIP:") != NULL)
    {
        size_t clip_len = strlen(urc);

        if (clip_len >= sizeof(s_clip_buf))
        {
            clip_len = sizeof(s_clip_buf) - 1;
        }
        memcpy(s_clip_buf, urc, clip_len);
        s_clip_buf[clip_len] = '\0';
        s_clip_tick    = osKernelGetTickCount();
        s_clip_pending = 1;

        if (s_evt != NULL)
        {
            osEventFlagsSet(s_evt, EVT_CALL_CLIP);
        }
    }

    if (strstr(urc, "RING") != NULL)
    {
        s_ring_pending = 1;

        if (s_evt != NULL)
        {
            osEventFlagsSet(s_evt, EVT_CALL_RING);
        }
    }
#endif

    if (strstr(urc, "+CMT:") == NULL)
    {
        return;
    }

    len = strlen(urc);
    if (len >= sizeof(s_cmt_buf))
    {
        len = sizeof(s_cmt_buf) - 1;
    }
    memcpy(s_cmt_buf, urc, len);
    s_cmt_buf[len] = '\0';
    s_cmt_pending = 1;

    if (s_evt != NULL)
    {
        osEventFlagsSet(s_evt, EVT_SMS_CMT);
    }
}

/* --------------------------------------------------------------------------
 * 长短信缓存与入队
 * ------------------------------------------------------------------------ */

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;

    if (dst_size == 0)
    {
        return;
    }

    while (src[i] != '\0' && i < dst_size - 1)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void text_append(char *dst, size_t dst_size, const char *src)
{
    size_t used;
    size_t space;

    if (dst_size == 0)
    {
        return;
    }

    used = strlen(dst);
    if (used >= dst_size - 1)
    {
        return;
    }

    space = dst_size - 1 - used;
    while (space > 0 && *src != '\0')
    {
        dst[used++] = *src++;
        space--;
    }
    dst[used] = '\0';
}

static void concat_clear(sms_concat_t *group)
{
    memset(group, 0, sizeof(*group));
}

static sms_concat_t *concat_find_or_create(int ref_number, const char *sender,
                                           const char *timestamp, int total_parts)
{
    sms_concat_t *slot = NULL;
    int           i;

    for (i = 0; i < SMS_CONCAT_MAX_GROUPS; i++)
    {
        if (s_concat[i].used &&
            s_concat[i].ref_number == ref_number &&
            strcmp(s_concat[i].sender, sender) == 0)
        {
            return &s_concat[i];
        }
    }

    for (i = 0; i < SMS_CONCAT_MAX_GROUPS; i++)
    {
        if (!s_concat[i].used)
        {
            slot = &s_concat[i];
            break;
        }
    }

    if (slot == NULL)
    {
        slot = &s_concat[0];
        for (i = 1; i < SMS_CONCAT_MAX_GROUPS; i++)
        {
            if (s_concat[i].first_tick < slot->first_tick)
            {
                slot = &s_concat[i];
            }
        }
        u0_printf("[sms] concat cache full, drop oldest ref=%d\r\n", slot->ref_number);
    }

    concat_clear(slot);
    slot->used           = true;
    slot->ref_number     = ref_number;
    slot->total_parts    = (total_parts > SMS_CONCAT_MAX_PARTS) ?
                           SMS_CONCAT_MAX_PARTS : total_parts;
    slot->received_parts = 0;
    slot->first_tick     = osKernelGetTickCount();
    copy_str(slot->sender, sizeof(slot->sender), sender);
    copy_str(slot->timestamp, sizeof(slot->timestamp), timestamp);

    return slot;
}

static void concat_store_part(sms_concat_t *group, int part_number, const char *text)
{
    int index = part_number - 1;

    if (index < 0 || index >= SMS_CONCAT_MAX_PARTS)
    {
        u0_printf("[sms] concat part out of range: %d\r\n", part_number);
        return;
    }

    if (group->part_valid[index])
    {
        u0_printf("[sms] concat duplicate part %d/%d, skip\r\n",
                  part_number, group->total_parts);
        return;
    }

    copy_str(group->parts[index], sizeof(group->parts[index]), text);
    group->part_valid[index] = true;
    group->received_parts++;
}

static void concat_assemble(const sms_concat_t *group, char *out, size_t out_size)
{
    int i;

    if (out_size == 0)
    {
        return;
    }
    out[0] = '\0';

    for (i = 0; i < group->total_parts; i++)
    {
        if (i < SMS_CONCAT_MAX_PARTS && group->part_valid[i])
        {
            text_append(out, out_size, group->parts[i]);
        }
        else
        {
            char mark[32];

            snprintf(mark, sizeof(mark), "[缺失分段%d]", i + 1);
            text_append(out, out_size, mark);
        }
    }
}

static void sms_push_item(sms_item_t *item, const char *reason)
{
    if (!sms_queue_push(item))
    {
        u0_printf("[sms] queue full, %s dropped\r\n", reason);
        return;
    }

    u0_printf("[sms] queued seq=%u sender=%s\r\n",
              (unsigned)item->seq, item->sender);
}

static void concat_timer_rearm(void)
{
    uint32_t now;
    uint32_t timeout_ticks;
    uint32_t earliest = 0;
    bool     found = false;
    int      i;

    if (s_concat_timer == NULL)
    {
        return;
    }

    now = osKernelGetTickCount();
    timeout_ticks = osMsToTicks(SMS_CONCAT_TIMEOUT_MS);

    for (i = 0; i < SMS_CONCAT_MAX_GROUPS; i++)
    {
        uint32_t elapsed;
        uint32_t remaining;

        if (!s_concat[i].used)
        {
            continue;
        }

        elapsed = (uint32_t)(now - s_concat[i].first_tick);
        remaining = (elapsed >= timeout_ticks) ? 1 : (timeout_ticks - elapsed);

        if (!found || remaining < earliest)
        {
            earliest = remaining;
            found = true;
        }
    }

    if (!found)
    {
        osTimerStop(s_concat_timer);
        return;
    }

    osTimerStart(s_concat_timer, earliest);
}

static void handle_decoded_sms(sms_item_t *item, int ref_number,
                               int part_number, int total_parts)
{
    sms_concat_t *group;

    if (item == NULL)
    {
        return;
    }

    if (total_parts <= 1 || part_number <= 0)
    {
        sms_push_item(item, "single");
        return;
    }

    group = concat_find_or_create(ref_number, item->sender,
                                  item->timestamp, total_parts);
    concat_store_part(group, part_number, item->text);

    u0_printf("[sms] concat %d/%d ref=%d\r\n",
              group->received_parts, group->total_parts, group->ref_number);

    if (group->received_parts >= group->total_parts)
    {
        concat_assemble(group, item->text, sizeof(item->text));
        sms_push_item(item, "concat");
        concat_clear(group);
    }

    concat_timer_rearm();
}

static void concat_expire(void)
{
    uint32_t now = osKernelGetTickCount();
    uint32_t timeout = osMsToTicks(SMS_CONCAT_TIMEOUT_MS);
    int      i;

    for (i = 0; i < SMS_CONCAT_MAX_GROUPS; i++)
    {
        sms_concat_t *group = &s_concat[i];

        if (!group->used)
        {
            continue;
        }
        if ((uint32_t)(now - group->first_tick) < timeout)
        {
            continue;
        }

        {
            sms_item_t item;

            memset(&item, 0, sizeof(item));
            copy_str(item.sender, sizeof(item.sender), group->sender);
            copy_str(item.timestamp, sizeof(item.timestamp), group->timestamp);
            concat_assemble(group, item.text, sizeof(item.text));

            u0_printf("[sms] concat timeout ref=%d %d/%d\r\n",
                      group->ref_number, group->received_parts, group->total_parts);
            sms_push_item(&item, "concat timeout");
        }

        concat_clear(group);
    }
}

/* --------------------------------------------------------------------------
 * +CMT PDU 处理
 * ------------------------------------------------------------------------ */

static void sms_handle_cmt(const char *raw)
{
    sms_item_t item;
    int        ref_number = 0;
    int        part_number = 1;
    int        total_parts = 1;

    if (raw == NULL)
    {
        return;
    }

    memset(&item, 0, sizeof(item));

    if (!sms_pdu_extract(raw, "+CMT:", s_resp, sizeof(s_resp)))
    {
        u0_printf("[sms] CMT PDU extract failed\r\n");
        log_text("[sms] raw: ", raw);
        return;
    }

    if (!sms_pdu_decode(s_resp, &item, &ref_number, &part_number, &total_parts))
    {
        u0_printf("[sms] CMT PDU decode failed\r\n");
        log_text("[sms] pdu: ", s_resp);
        return;
    }

    u0_printf("[sms] CMT sender=%s part=%d/%d ref=%d len=%u\r\n",
              item.sender, part_number, total_parts, ref_number,
              (unsigned)strlen(item.text));

    handle_decoded_sms(&item, ref_number, part_number, total_parts);
}

#if CALL_REPORT_ENABLE
/* --------------------------------------------------------------------------
 * 来电检测
 * ------------------------------------------------------------------------ */

/**
 * @brief 从 +CLIP URC 里取出主叫号码
 *
 * @details 形如 +CLIP: "+8613800138000",145,,,,0，号码在引号里；
 *          主叫隐藏号码时引号内为空，按未知号码处理。
 */
static void call_parse_clip(const char *urc, char *out, size_t out_size)
{
    const char *p;
    size_t      n = 0;

    if (out == NULL || out_size == 0)
    {
        return;
    }
    out[0] = '\0';

    if (urc == NULL)
    {
        return;
    }

    p = strchr(urc, '"');
    if (p == NULL)
    {
        return;
    }

    for (p = p + 1; *p != '\0' && *p != '"' && n < out_size - 1; p++)
    {
        out[n++] = *p;
    }
    out[n] = '\0';
}

static void call_push_item(const char *number)
{
    sms_item_t item;

    memset(&item, 0, sizeof(item));
    item.type = SMS_ITEM_TYPE_CALL;
    copy_str(item.sender, sizeof(item.sender), (number != NULL) ? number : "");
    /* 来电没有 SCTS，存检测时刻的 epoch 秒，避免离线排队后时间失真 */
    snprintf(item.timestamp, sizeof(item.timestamp), "%llu",
             (unsigned long long)cm_rtc_get_current_time());
    copy_str(item.text, sizeof(item.text), "incoming call");

    if (!sms_queue_push(&item))
    {
        u0_printf("[call] queue full, dropped\r\n");
        return;
    }

    u0_printf("[call] queued seq=%u from=%s\r\n", (unsigned)item.seq,
              (item.sender[0] != '\0') ? item.sender : "unknown");
}

/* 收集窗口还剩多少 tick 要等，已经到期返回 1 */
static uint32_t call_window_ticks(void)
{
    uint32_t window  = osMsToTicks(CALL_CLIP_WINDOW_MS);
    uint32_t elapsed = (uint32_t)(osKernelGetTickCount() - s_call_window_tick);

    return (elapsed >= window) ? 1 : (window - elapsed);
}

/**
 * @brief 收到一声 RING：判断这是不是新的一通电话
 */
static void call_handle_ring(void)
{
    uint32_t now = osKernelGetTickCount();

    if (s_call_collecting)
    {
        return; /* 同一通电话的重复响铃 */
    }

    if (s_call_reported &&
        (uint32_t)(now - s_call_last_tick) < osMsToTicks(CALL_DEDUP_MS))
    {
        return; /* 去重期内，按同一通电话处理 */
    }

    s_call_collecting  = true;
    s_call_window_tick = now;

    /* 号码比 RING 早到时留着的 +CLIP，超过一个窗口就丢掉 */
    if (s_clip_pending &&
        (uint32_t)(now - s_clip_tick) > osMsToTicks(CALL_CLIP_WINDOW_MS))
    {
        s_clip_pending = 0;
    }
}

/**
 * @brief 收集窗口到期：把这一通电话入队
 */
static void call_handle_window(void)
{
    char number[SMS_SENDER_MAX];

    if ((uint32_t)(osKernelGetTickCount() - s_call_window_tick) <
        osMsToTicks(CALL_CLIP_WINDOW_MS))
    {
        return; /* 还在等 +CLIP */
    }

    number[0] = '\0';
    if (s_clip_pending)
    {
        call_parse_clip(s_clip_buf, number, sizeof(number));
    }

    s_call_collecting = false;
    s_clip_pending    = 0;
    s_call_reported   = true;
    s_call_last_tick  = osKernelGetTickCount();

    call_push_item(number);
}
#endif

/* --------------------------------------------------------------------------
 * 线程
 * ------------------------------------------------------------------------ */

#if SMS_DEBUG_SCAN
/**
 * @brief 打印当前信号强度
 *
 * @details CSQ 的 rssi 取值 0~31，越大越好，99 表示未知。
 *          小于 10 基本等于没信号，这种情况下收不到短信很正常。
 */
static void log_csq(void)
{
    char rssi[8] = {0};
    char ber[8] = {0};
    int  rc;

    cm_pm_work_lock();
    rc = cm_modem_get_csq(rssi, ber);
    cm_pm_work_unlock();

    if (rc == 0)
    {
        u0_printf("[sms] CSQ: rssi=%s ber=%s\r\n", rssi, ber);
    }
    else
    {
        u0_printf("[sms] CSQ query failed\r\n");
    }
}

/**
 * @brief 打印 LTE 的真实无线指标和服务小区
 *
 * @details CSQ 是映射出来的粗指标，RSRP/RSRQ/SNR 才是 LTE 的原始测量值。
 *          服务小区的 MCC/MNC 能确认模组到底驻留在哪个网上。
 */
static void log_radio(void)
{
    cm_radio_info_t ri;
    cm_cell_info_t  ci;
    int32_t         rc;
    int32_t         n;

    memset(&ri, 0, sizeof(ri));
    cm_pm_work_lock();
    rc = cm_modem_get_radio_info(&ri);
    cm_pm_work_unlock();

    if (rc == 0)
    {
        u0_printf("[sms] RADIO: rat=%u rsrp=%u rsrq=%u rssi=%u rxlev=%u\r\n",
                  (unsigned)ri.rat, (unsigned)ri.rsrp, (unsigned)ri.rsrq,
                  (unsigned)ri.rssi, (unsigned)ri.rxlev);
    }
    else
    {
        u0_printf("[sms] RADIO query failed\r\n");
    }

    memset(&ci, 0, sizeof(ci));
    cm_pm_work_lock();
    n = cm_modem_info_cell(&ci, 1);
    cm_pm_work_unlock();

    if (n > 0)
    {
        u0_printf("[sms] CELL: mcc=%s mnc=%s rsrp=%u rsrq=%u snr=%u\r\n",
                  (const char *)ci.mcc, (const char *)ci.mnc,
                  (unsigned)ci.rsrp, (unsigned)ci.rsrq, (unsigned)ci.snr);
    }
    else
    {
        u0_printf("[sms] CELL query failed (%d)\r\n", (int)n);
    }
}

/**
 * @brief 调试用：把短信存储里现有的内容列出来
 *
 * @details 用来区分"URC 没上报"和"短信根本没到模块"这两件事。
 */
static void sms_debug_scan(void)
{
    log_csq();

    if (at_exec("AT+CMGL=4", s_resp, sizeof(s_resp)) == 0)
    {
        log_text("[sms] CMGL: ", s_resp);
    }
    else
    {
        u0_printf("[sms] CMGL failed\r\n");
    }
}

/**
 * @brief 开机一次性探针：确认卡和网络到底支不支持收短信
 *
 * @details 收不到短信时，这三个查询能把原因缩小到很小的范围。
 */
static void sms_debug_probe(void)
{
    log_radio();

    /* +CSMS: <service>,<mt>,<mo>,<bm>，其中 mt=1 表示网络支持"收短信"。
     * 这是收不到短信时第一个要看的：mt=0 说明这张卡/这个网络根本不给发。
     */
    if (at_exec("AT+CSMS?", s_resp, sizeof(s_resp)) == 0)
    {
        log_text("[sms] CSMS?: ", s_resp);
    }
    else
    {
        u0_printf("[sms] CSMS? failed\r\n");
    }

    if (at_exec("AT+CPMS?", s_resp, sizeof(s_resp)) == 0)
    {
        log_text("[sms] CPMS?: ", s_resp);
    }
    else
    {
        u0_printf("[sms] CPMS? failed\r\n");
    }

    if (at_exec("AT+CNMI?", s_resp, sizeof(s_resp)) == 0)
    {
        log_text("[sms] CNMI?: ", s_resp);
    }
    else
    {
        u0_printf("[sms] CNMI? failed\r\n");
    }

    // /* 本机号码：+CNUM: ,"<号码>",<type>。
    //  * 物联网卡经常没往卡里写这个字段，读不到是正常的，但读到就能确认号码。
    //  */
    // if (at_exec("AT+CNUM", s_resp, sizeof(s_resp)) == 0)
    // {
    //     log_text("[sms] CNUM: ", s_resp);
    // }
    // else
    // {
    //     u0_printf("[sms] CNUM failed\r\n");
    // }

    /* CS 域注册状态：+CREG: <n>,<stat>，stat=1 或 5 表示已注册。
     * 收短信依赖这个状态，只注册上 PS 域（能上网）不代表一定能收短信。
     */
    // if (at_exec("AT+CREG?", s_resp, sizeof(s_resp)) == 0)
    // {
    //     log_text("[sms] CREG?: ", s_resp);
    // }
    // else
    // {
    //     u0_printf("[sms] CREG? failed\r\n");
    // }
}

#endif

#if EDRX_CFG_ENABLE
/**
 * @brief 配置模组 eDRX 周期
 *
 * @details 3GPP 规定 <Requested_eDRX_value> 是带引号的字符串（如 "0010"），
 *          个别固件也接受不带引号的写法，这里失败后回退一次。
 *          命令下发的是"请求值"，最终周期由网络在 Attach / TAU 时决定。
 */
static void edrx_config(void)
{
    char cmd[40];
    int  rc;

    snprintf(cmd, sizeof(cmd), "AT+CEDRXS=%d,%d,\"%s\"",
             EDRX_MODE, EDRX_ACT_TYPE, EDRX_VALUE);
    rc = at_exec(cmd, s_resp, sizeof(s_resp));

    if (rc != 0)
    {
        snprintf(cmd, sizeof(cmd), "AT+CEDRXS=%d,%d,%s",
                 EDRX_MODE, EDRX_ACT_TYPE, EDRX_VALUE);
        rc = at_exec(cmd, s_resp, sizeof(s_resp));
    }

    u0_printf("[sms] eDRX value %s -> %d\r\n", EDRX_VALUE, rc);
}
#endif

void sms_recv_task(void *argument)
{
#if SMS_DEBUG_SCAN
    uint32_t last_scan_tick;
#endif
    int      rc        = 0;

    (void)argument;

    u0_printf("[sms] task start\r\n");

    s_evt     = osEventFlagsNew(NULL);
    s_at_done = osSemaphoreNew(1, 0, NULL);
    s_at_lock = osMutexNew(NULL);
    s_concat_timer = osTimerNew(concat_timer_cb, osTimerOnce, NULL, NULL);
    if (s_evt == NULL || s_at_done == NULL || s_at_lock == NULL ||
        s_concat_timer == NULL)
    {
        u0_printf("[sms] sync object create failed\r\n");
        return;
    }

    cm_virt_at_urc_reg(urc_cb);

#if EDRX_CFG_ENABLE
    edrx_config();
#endif

    rc = at_exec("AT+CMGF=0", s_resp, sizeof(s_resp));
    u0_printf("[sms] CMGF=0   -> %d\r\n", rc);
    if (rc != 0)
    {
        log_text("[sms]   raw: ", s_resp);
    }

    rc = at_exec("AT+CNMI=2,2,0,0,0", s_resp, sizeof(s_resp));
    u0_printf("[sms] CNMI=2,2(+CMT) -> %d\r\n", rc);
    if (rc != 0)
    {
        log_text("[sms]   raw: ", s_resp);
    }

#if CALL_REPORT_ENABLE
    /* 打开主叫号码显示，来电的 +CLIP URC 里才会带号码 */
    rc = at_exec("AT+CLIP=1", s_resp, sizeof(s_resp));
    u0_printf("[call] CLIP=1   -> %d\r\n", rc);
    if (rc != 0)
    {
        log_text("[call]   raw: ", s_resp);
    }
#endif

#if SMS_DEBUG_SCAN
    sms_debug_probe(); 
    sms_debug_scan();  
    last_scan_tick = osKernelGetTickCount();
#endif

    for (;;)
    {
        uint32_t wait_ticks = osWaitForever;

#if CALL_REPORT_ENABLE
        if (s_call_collecting)
        {
            wait_ticks = call_window_ticks();
        }
#endif

        osEventFlagsWait(s_evt, EVT_WAIT_MASK, osFlagsWaitAny, wait_ticks);

        if (s_cmt_pending)
        {
            s_cmt_pending = 0;
            sms_handle_cmt(s_cmt_buf);
        }

#if CALL_REPORT_ENABLE
        if (s_ring_pending)
        {
            s_ring_pending = 0;
            call_handle_ring();
        }

        if (s_call_collecting)
        {
            call_handle_window();
        }
#endif

        concat_expire();
        concat_timer_rearm();

#if SMS_DEBUG_SCAN
        {
            uint32_t now = osKernelGetTickCount();

            if ((uint32_t)(now - last_scan_tick) >= osMsToTicks(SMS_SCAN_PERIOD_MS))
            {
                last_scan_tick = now;
                sms_debug_scan();
            }
        }
#endif
    }
}
