#include "app.h"

osThreadId_t osThreadCreat(const char *name, osThreadFunc_t func, osPriority_t priority, uint32_t stacksize)
{
	osThreadAttr_t thread_cfg = {0};

	thread_cfg.name       = name;
	thread_cfg.priority   = priority;
	thread_cfg.stack_size = stacksize;

	if (osPriorityNormal > thread_cfg.priority)
	{
		thread_cfg.priority = osPriorityNormal;
	}

	return osThreadNew(func, NULL, (const osThreadAttr_t *)&thread_cfg);
}

osTimerId_t osTimerCreat(const char *name, osTimerFunc_t func, osTimerType_t type)
{
	osTimerAttr_t timer_attr = {0};

	timer_attr.name = name;

	return osTimerNew(func, type, NULL, &timer_attr);
}

void osDelayMs(uint32_t ms)
{
	osDelay(ms / 5);
}

uint32_t osMsToTicks(uint32_t ms)
{
	uint32_t ticks = (ms + 4) / 5;

	return (ticks == 0) ? 1 : ticks;
}
