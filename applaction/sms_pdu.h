#ifndef __SMS_PDU_H
#define __SMS_PDU_H

#include <stdbool.h>
#include <stddef.h>
#include "sms_queue.h"

bool sms_pdu_extract(const char *resp, const char *tag, char *out, size_t out_size);

bool sms_pdu_decode(const char *pdu_hex, sms_item_t *item,
                    int *ref_number, int *part_number, int *total_parts);

#endif
