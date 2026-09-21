#ifndef __SMS_QUEUE_H
#define __SMS_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

#define SMS_QUEUE_MAX   16    
#define SMS_SENDER_MAX  24    
#define SMS_TIME_MAX    24  
#define SMS_TEXT_MAX    768   

typedef struct
{
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
