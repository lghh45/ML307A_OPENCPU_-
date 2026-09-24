#ifndef __SMS_QUEUE_H
#define __SMS_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

/* 队列条目类型：短信正文 / 来电记录。
 * 字段按类型复用：sender 是短信发件号码或来电主叫号码（未知时为空串），
 * timestamp 短信存 SCTS 字符串、来电存检测时刻的 epoch 秒。
 */
#define SMS_ITEM_TYPE_SMS   0
#define SMS_ITEM_TYPE_CALL  1

#define SMS_QUEUE_MAX   16    
#define SMS_SENDER_MAX  24    
#define SMS_TIME_MAX    24  
#define SMS_TEXT_MAX    768   

typedef struct
{
    uint8_t  type;                    /* SMS_ITEM_TYPE_xxx */
    uint32_t seq;                    
    char     sender[SMS_SENDER_MAX]; 
    char     timestamp[SMS_TIME_MAX]; 
    char     text[SMS_TEXT_MAX];      
} sms_item_t;


void sms_queue_init(void);

bool sms_queue_push(sms_item_t *item);


bool sms_queue_peek(sms_item_t *out);


bool sms_queue_pop_head(void);


uint32_t sms_queue_count(void);


void sms_queue_wait(void);

#endif 
