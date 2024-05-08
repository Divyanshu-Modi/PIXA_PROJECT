// SPDX-License-Identifier: GPL-3.0
// Author: Divyanshu-Modi <divyan.m05@gmail.com>

#include "esp_log.h"

#pragma once

#define pr_info(pr_fmt, x, args...) ESP_LOGI(pr_fmt, x, ##args)
#define pr_err(pr_fmt, x, args...) ESP_LOGE(pr_fmt, x, ##args)
#define pr_warn(pr_fmt, x, args...) ESP_LOGW(pr_fmt, x, ##args)
#define pr_debug(pr_fmt, x, args...) ESP_LOGD(pr_fmt, x, ##args)
