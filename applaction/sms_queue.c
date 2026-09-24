#include "sms_queue.h"
#include "app.h"
#include "cm_fs.h"
#include "cm_os.h"
#include "cm_pm.h"
#include <string.h>

#define SMS_QUEUE_FILE  "smsq.dat"
#define SMS_QUEUE_MAGIC_V1 0x534D5331u  /* 旧格式：条目没有 type 字段 */
#define SMS_QUEUE_MAGIC    0x534D5332u  /* 当前格式：sms_item_t 多了 type */

typedef struct
{
    uint32_t   magic;
    uint32_t   head; 
    uint32_t   count; 
    uint32_t   seq;  
    sms_item_t items[SMS_QUEUE_MAX];
} sms_queue_blob_t;

/* 旧版本的落盘布局，只在读取历史 smsq.dat 时用 */
typedef struct
{
    uint32_t seq;
    char     sender[SMS_SENDER_MAX];
    char     timestamp[SMS_TIME_MAX];
    char     text[SMS_TEXT_MAX];
} sms_item_v1_t;

typedef struct
{
    uint32_t      magic;
    uint32_t      head;
    uint32_t      count;
    uint32_t      seq;
    sms_item_v1_t items[SMS_QUEUE_MAX];
} sms_queue_blob_v1_t;

/* 读文件时按最大的布局取字节，用 union 保证对齐 */
static union
{
    sms_queue_blob_t    cur;
    sms_queue_blob_v1_t v1;
} s_file;

static sms_queue_blob_t s_q;
static osMutexId_t      s_lock = NULL;
static osEventFlagsId_t s_evt = NULL;

#define SMS_QUEUE_EVT_ITEM (1u << 0)

static void queue_save(void)
{
    int32_t fd;

    cm_pm_work_lock();
    fd = cm_fs_open(SMS_QUEUE_FILE, CM_FS_WB);

    if (fd < 0)
    {
        u0_printf("[queue] save open failed: %d\r\n", (int)fd);
        cm_pm_work_unlock();
        return;
    }

    cm_fs_write(fd, &s_q, sizeof(s_q));
    cm_fs_close(fd);
    cm_pm_work_unlock();
}

static void queue_reset(void)
{
    memset(&s_q, 0, sizeof(s_q));
    s_q.magic = SMS_QUEUE_MAGIC;
}

/**
 * @brief 把旧格式（条目没有 type 字段）的队列转成当前格式
 *
 * @details 历史条目都当短信处理；升级前落盘的数据因此不会丢。
 */
static bool queue_import_v1(const sms_queue_blob_v1_t *old)
{
    int i;

    if (old->count > SMS_QUEUE_MAX || old->head >= SMS_QUEUE_MAX)
    {
        return false;
    }

    queue_reset();
    s_q.head  = old->head;
    s_q.count = old->count;
    s_q.seq   = old->seq;

    for (i = 0; i < SMS_QUEUE_MAX; i++)
    {
        s_q.items[i].type = SMS_ITEM_TYPE_SMS;
        s_q.items[i].seq  = old->items[i].seq;
        memcpy(s_q.items[i].sender, old->items[i].sender, SMS_SENDER_MAX);
        memcpy(s_q.items[i].timestamp, old->items[i].timestamp, SMS_TIME_MAX);
        memcpy(s_q.items[i].text, old->items[i].text, SMS_TEXT_MAX);
    }

    return true;
}

static void queue_load(void)
{
    int32_t fd;
    int32_t n;

    queue_reset();

    fd = cm_fs_open(SMS_QUEUE_FILE, CM_FS_RB);
    if (fd < 0)
    {
        return;
    }

    n = cm_fs_read(fd, &s_file, sizeof(s_file));
    cm_fs_close(fd);

    if (n == (int32_t)sizeof(sms_queue_blob_v1_t) &&
        s_file.v1.magic == SMS_QUEUE_MAGIC_V1)
    {
        if (!queue_import_v1(&s_file.v1))
        {
            u0_printf("[queue] persisted v1 data invalid, reset\r\n");
            return;
        }
        u0_printf("[queue] restored %u pending (v1)\r\n", (unsigned)s_q.count);
        return;
    }

    if (n != (int32_t)sizeof(sms_queue_blob_t) || s_file.cur.magic != SMS_QUEUE_MAGIC ||
        s_file.cur.count > SMS_QUEUE_MAX || s_file.cur.head >= SMS_QUEUE_MAX)
    {
        u0_printf("[queue] persisted data invalid (%d), reset\r\n", (int)n);
        return;
    }

    s_q = s_file.cur;
    u0_printf("[queue] restored %u pending\r\n", (unsigned)s_q.count);
}

void sms_queue_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = osMutexNew(NULL);
    }
    if (s_evt == NULL)
    {
        s_evt = osEventFlagsNew(NULL);
    }

    queue_load();
}

bool sms_queue_push(sms_item_t *item)
{
    bool     ok = false;
    uint32_t pos;

    if (item == NULL || s_lock == NULL)
    {
        return false;
    }

    osMutexAcquire(s_lock, osWaitForever);

    if (s_q.count < SMS_QUEUE_MAX)
    {
        pos = (s_q.head + s_q.count) % SMS_QUEUE_MAX;
        s_q.items[pos] = *item;
        s_q.seq++;
        s_q.items[pos].seq = s_q.seq;
        item->seq = s_q.seq;
        s_q.count++;
        ok = true;
    }

    if (ok)
    {
        queue_save();
    }

    osMutexRelease(s_lock);

    if (ok && s_evt != NULL)
    {
        osEventFlagsSet(s_evt, SMS_QUEUE_EVT_ITEM);
    }

    return ok;
}

bool sms_queue_peek(sms_item_t *out)
{
    bool ok = false;

    if (out == NULL || s_lock == NULL)
    {
        return false;
    }

    osMutexAcquire(s_lock, osWaitForever);

    if (s_q.count > 0)
    {
        *out = s_q.items[s_q.head];
        ok = true;
    }

    osMutexRelease(s_lock);
    return ok;
}

bool sms_queue_pop_head(void)
{
    bool ok = false;

    if (s_lock == NULL)
    {
        return false;
    }

    osMutexAcquire(s_lock, osWaitForever);

    if (s_q.count > 0)
    {
        s_q.head = (s_q.head + 1) % SMS_QUEUE_MAX;
        s_q.count--;
        ok = true;
    }

    if (ok)
    {
        queue_save();
    }

    osMutexRelease(s_lock);
    return ok;
}

uint32_t sms_queue_count(void)
{
    uint32_t n;

    if (s_lock == NULL)
    {
        return 0;
    }

    osMutexAcquire(s_lock, osWaitForever);
    n = s_q.count;
    osMutexRelease(s_lock);

    return n;
}

void sms_queue_wait(void)
{
    if (s_evt == NULL)
    {
        osDelayMs(1000);
        return;
    }

    (void)osEventFlagsWait(s_evt, SMS_QUEUE_EVT_ITEM, osFlagsWaitAny, osWaitForever);
}
