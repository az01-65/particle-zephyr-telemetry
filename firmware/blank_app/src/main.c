/*
 * blank_app - the minimal starting point for your own firmware on either
 * board. Blinks the red status LED (led1 - NOT led0, which is a known
 * "doesn't visibly light" pin documented elsewhere in this repo) and
 * prints a heartbeat over USB-CDC once a second, so you have a known-good
 * starting point that's easy to confirm is actually running before you
 * start replacing this file with your own project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(blank_app, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

int main(void)
{
	int err;
	uint32_t count = 0;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure LED (err %d)", err);
		return err;
	}

	LOG_INF("blank_app ready - this is the minimal starting point");

	while (1) {
		gpio_pin_toggle_dt(&led);
		LOG_INF("heartbeat %u", count++);
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
