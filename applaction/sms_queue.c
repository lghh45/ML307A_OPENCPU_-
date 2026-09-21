#include "sms_queue.h"
#include "app.h"
#include "cm_fs.h"
#include "cm_os.h"
#include "cm_pm.h"
#include <string.h>

#define SMS_QUEUE_FILE  "smsq.dat"
#define SMS_QUEUE_MAGIC 0x534D5331u 

typedef struct
{
    uint32_t   magic;
    uint32_t   head; 
    uint32_t   count; 
    uint32_t   seq;  
    sms_item_t items[SMS_QUEUE_MAX];
} sms_queue_blob_t;

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

static void queue_load(void)
{
    int32_t fd;
    int32_t n;

    memset(&s_q, 0, sizeof(s_q));
    s_q.magic = SMS_QUEUE_MAGIC;

    fd = cm_fs_open(SMS_QUEUE_FILE, CM_FS_RB);
    if (fd < 0)
    {
        return;
    }

    n = cm_fs_read(fd, &s_q, sizeof(s_q));
    cm_fs_close(fd);

    if (n != (int32_t)sizeof(s_q) || s_q.magic != SMS_QUEUE_MAGIC ||
        s_q.count > SMS_QUEUE_MAX || s_q.head >= SMS_QUEUE_MAX)
    {
        u0_printf("[queue] persisted data invalid (%d), reset\r\n", (int)n);
        memset(&s_q, 0, sizeof(s_q));
        s_q.magic = SMS_QUEUE_MAGIC;
        return;
    }

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
