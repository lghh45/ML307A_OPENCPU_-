#include "app.h"
#include "sms_queue.h"
#include "sms_recv.h"
#include "net_upload.h"
#include "cm_pm.h"

static void pm_enter_callback(void)
{
	/* 进入低功耗时不能打印或执行耗时操作 */
}

static void pm_exit_callback(void)
{
	/* 退出低功耗时不能打印或执行耗时操作 */
}

/**
 * @brief OpenCPU 用户程序入口
 *
 */
int cm_opencpu_entry(char *param)
{
	(void)param;

	uart_open(CM_UART_DEV_0, CM_UART_BAUDRATE_115200, u0_callback);
	u0_printf("\r\n===== SMS forwarder boot =====\r\n");

	sms_queue_init();
	u0_printf("pending in queue: %u\r\n", (unsigned)sms_queue_count());

	{
		cm_pm_cfg_t pm_cfg = {pm_enter_callback, pm_exit_callback};

		cm_pm_init(pm_cfg);
	}

	cm_pm_work_unlock();

	osThreadCreat("sms_recv", sms_recv_task, APP_TASK_PRIO, APP_SMS_STACK_SIZE);
	osThreadCreat("sms_upload", upload_task, APP_TASK_PRIO, APP_UP_STACK_SIZE);

	return 0;
}
