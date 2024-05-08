// SPDX-License-Identifier: GPL-3.0
// Author: Divyanshu-Modi <divyan.m05@gmail.com>

#define pr_fmt "main"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pr_log.h"

void app_main(void)
{
	pr_info(pr_fmt, "main_task");
}
