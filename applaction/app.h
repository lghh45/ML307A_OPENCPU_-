#ifndef __APP_H
#define __APP_H

#include "includes.h"
#include "uart.h"

#define APP_TASK_PRIO        osPriorityNormal
#define APP_SMS_STACK_SIZE   (1024 * 4)
#define APP_UP_STACK_SIZE    (1024 * 4)

//创建线程
osThreadId_t osThreadCreat(const char *name, osThreadFunc_t func, osPriority_t priority, uint32_t stacksize);

//创建定时器
osTimerId_t osTimerCreat(const char *name, osTimerFunc_t func, osTimerType_t type);


void osDelayMs(uint32_t ms);

uint32_t osMsToTicks(uint32_t ms);

#endif /* __APP_H */
