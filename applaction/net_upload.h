#ifndef __NET_UPLOAD_H
#define __NET_UPLOAD_H

#include "sms_queue.h"


int net_upload_init(void);


int net_upload_post(const sms_item_t *item);


void upload_task(void *argument);

#endif
