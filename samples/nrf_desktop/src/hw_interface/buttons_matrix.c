/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr/types.h>

#include <kernel.h>
#include <spinlock.h>
#include <soc.h>
#include <device.h>
#include <gpio.h>

#include "event_manager.h"
#include "button_event.h"
#include "power_event.h"

#define MODULE buttons
#include "module_state_event.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_DESKTOP_BUTTONS_LOG_LEVEL);

#define SCAN_INTERVAL CONFIG_DESKTOP_BUTTONS_MATRIX_SCAN_INTERVAL

enum state {
	STATE_IDLE,
	STATE_ACTIVE,
	STATE_SCANNING,
	STATE_SUSPENDING
};


static const u8_t col_pin[] = { 2, 21, 20, 19};
static const u8_t row_pin[] = {29, 31, 22, 24};

static struct device *gpio_dev;
static struct gpio_callback gpio_cb;
static struct k_delayed_work matrix_scan;
static enum state state;
static struct k_spinlock lock;


static void matrix_scan_fn(struct k_work *work);


static int set_cols(u32_t mask)
{
	for (size_t i = 0; i < ARRAY_SIZE(col_pin); i++) {
		u32_t val = (mask & (1 << i)) ? (1) : (0);

		if (gpio_pin_write(gpio_dev, col_pin[i], val)) {
			LOG_ERR("cannot set pin");

			return -EFAULT;
		}
	}

	return 0;
}

static int get_rows(u32_t *mask)
{
	for (size_t i = 0; i < ARRAY_SIZE(col_pin); i++) {
		u32_t val;

		if (gpio_pin_read(gpio_dev, row_pin[i], &val)) {
			LOG_ERR("cannot get pin");
			return -EFAULT;
		}

		(*mask) |= (val << i);
	}

	return 0;
}

static int set_trig_mode(int trig_mode)
{
	__ASSERT_NO_MSG((trig_mode == GPIO_INT_EDGE) ||
			(trig_mode == GPIO_INT_LEVEL));

	int flags = GPIO_PUD_PULL_DOWN | GPIO_DIR_IN | GPIO_INT |
		    GPIO_INT_ACTIVE_HIGH;
	flags |= trig_mode;

	int err = 0;

	for (size_t i = 0; (i < ARRAY_SIZE(row_pin)) && !err; i++) {
		err = gpio_pin_configure(gpio_dev, row_pin[i], flags);
	}

	return err;
}

static int callback_ctrl(bool enable)
{
	int err = 0;

	/* This must be done with irqs disabled to avoid pin callback
	 * being fired before others are still not activated.
	 */
	for (size_t i = 0; (i < ARRAY_SIZE(row_pin)) && !err; i++) {
		if (enable) {
			err = gpio_pin_enable_callback(gpio_dev, row_pin[i]);
		} else {
			err = gpio_pin_disable_callback(gpio_dev, row_pin[i]);
		}
	}

	return err;
}

static int suspend_nolock(void)
{
	int err = -EBUSY;

	switch (state) {
	case STATE_SCANNING:
		state = STATE_SUSPENDING;
		break;

	case STATE_SUSPENDING:
		/* Waiting for scanning to stop */
		break;

	case STATE_ACTIVE:
		state = STATE_IDLE;

		/* Leaving deep sleep requires level interrupt */
		err = set_trig_mode(GPIO_INT_LEVEL);
		if (!err) {
			err = callback_ctrl(true);
		}
		break;

	case STATE_IDLE:
		err = -EALREADY;
		break;

	default:
		__ASSERT_NO_MSG(false);
		break;
	}

	return err;
}

static int suspend(void)
{
	int err;

	k_spinlock_key_t key = k_spin_lock(&lock);
	err = suspend_nolock();
	k_spin_unlock(&lock, key);

	return err;
}

static void resume(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (state != STATE_IDLE) {
		/* Already activated. */
		k_spin_unlock(&lock, key);
		return;
	}

	int err = callback_ctrl(false);
	if (err) {
		LOG_ERR("cannot disable callbacks");
	} else {
		err = set_trig_mode(GPIO_INT_EDGE);
		if (err) {
			LOG_ERR("cannot set trig mode");
		} else {
			state = STATE_SCANNING;
		}
	}

	/* GPIO callback is disabled - it is safe to unlock */
	k_spin_unlock(&lock, key);

	if (err) {
		module_set_state(MODULE_STATE_ERROR);
	} else {
		matrix_scan_fn(NULL);

		module_set_state(MODULE_STATE_READY);
	}
}

static void matrix_scan_fn(struct k_work *work)
{
	if (IS_ENABLED(CONFIG_ASSERT)) {
		/* Validate state */
		k_spinlock_key_t key = k_spin_lock(&lock);
		__ASSERT_NO_MSG((state == STATE_SCANNING) ||
				(state == STATE_SUSPENDING));
		k_spin_unlock(&lock, key);
	}

	/* Get current state */
	u32_t cur_state[ARRAY_SIZE(col_pin)] = {0};

	for (size_t i = 0; i < ARRAY_SIZE(col_pin); i++) {

		int err = set_cols(1 << i);

		if (!err) {
			err = get_rows(&cur_state[i]);
		}

		if (err) {
			LOG_ERR("cannot scan matrix");
			goto error;
		}
	}

	/* Emit event for any key state change */

	static u32_t old_state[ARRAY_SIZE(col_pin)];
	bool any_pressed = false;

	for (size_t i = 0; i < ARRAY_SIZE(col_pin); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(row_pin); j++) {
			bool is_pressed = (cur_state[i] & (1 << j));
			bool was_pressed = (old_state[i] & (1 << j));

			if (is_pressed != was_pressed) {
				struct button_event *event = new_button_event();

				event->key_id = (i << 8) | (j & 0xFF);
				event->pressed = is_pressed;
				EVENT_SUBMIT(event);
			}

			any_pressed = any_pressed || is_pressed;
		}
	}

	memcpy(old_state, cur_state, sizeof(old_state));

	if (any_pressed) {
		/* Avoid draining current between scans */
		if (set_cols(0x00)) {
			LOG_ERR("cannot set neutral state");
			goto error;
		}

		/* Schedule next scan */
		k_delayed_work_submit(&matrix_scan, SCAN_INTERVAL);
	} else {
		/* If no button is pressed module can switch to callbacks */

		/* Prepare to wait for a callback */
		if (set_cols(0xFF)) {
			LOG_ERR("cannot set neutral state");
			goto error;
		}

		/* Make sure that mode is set before callbacks are enabled */
		int err = 0;

		k_spinlock_key_t key = k_spin_lock(&lock);
		switch (state) {
		case STATE_SCANNING:
			state = STATE_ACTIVE;
			err = callback_ctrl(true);
			break;

		case STATE_SUSPENDING:
			state = STATE_ACTIVE;
			err = suspend_nolock();
			if (!err) {
				module_set_state(MODULE_STATE_STANDBY);
			}
			__ASSERT_NO_MSG((err != -EBUSY) && (err != -EALREADY));
			break;

		default:
			__ASSERT_NO_MSG(false);
			break;
		}
		k_spin_unlock(&lock, key);

		if (err) {
			LOG_ERR("cannot enable callbacks");
			goto error;
		}
	}

	return;

error:
	module_set_state(MODULE_STATE_ERROR);
}

void button_pressed(struct device *gpio_dev, struct gpio_callback *cb,
		    u32_t pins)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	/* Disable GPIO interrupt */
	for (size_t i = 0; i < ARRAY_SIZE(row_pin); i++) {
		int err = gpio_pin_disable_callback(gpio_dev, row_pin[i]);
		if (err) {
			LOG_ERR("cannot disable callbacks");
		}
	}

	switch (state) {
	case STATE_IDLE:;
		struct wake_up_event *event = new_wake_up_event();
		EVENT_SUBMIT(event);
		break;

	case STATE_ACTIVE:
		state = STATE_SCANNING;
		k_delayed_work_submit(&matrix_scan, 0);
		break;

	case STATE_SCANNING:
	case STATE_SUSPENDING:
	default:
		/* Invalid state */
		__ASSERT_NO_MSG(false);
		break;
	}

	k_spin_unlock(&lock, key);
}

static void init_fn(void)
{
	/* Setup GPIO configuration */
	gpio_dev = device_get_binding(DT_GPIO_P0_DEV_NAME);
	if (!gpio_dev) {
		LOG_ERR("cannot get GPIO device binding");
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(col_pin); i++) {
		int err = gpio_pin_configure(gpio_dev, col_pin[i],
				GPIO_DIR_OUT);

		if (err) {
			LOG_ERR("cannot configure cols");
			goto error;
		}
	}

	int err = set_trig_mode(GPIO_INT_EDGE);
	if (err) {
		LOG_ERR("cannot set interrupt mode");
		goto error;
	}

	u32_t pin_mask = 0;
	for (size_t i = 0; i < ARRAY_SIZE(row_pin); i++) {
		/* Module starts in scanning mode and will switch to
		 * callback mode if no button is pressed.
		 */
		err = gpio_pin_disable_callback(gpio_dev, row_pin[i]);
		if (err) {
			LOG_ERR("cannot configure rows");
			goto error;
		}

		pin_mask |= BIT(row_pin[i]);
	}

	gpio_init_callback(&gpio_cb, button_pressed, pin_mask);
	err = gpio_add_callback(gpio_dev, &gpio_cb);
	if (err) {
		LOG_ERR("cannot add callback");
		goto error;
	}

	module_set_state(MODULE_STATE_READY);

	/* Perform initial scan */
	state = STATE_SCANNING;

	matrix_scan_fn(NULL);

	return;

error:
	module_set_state(MODULE_STATE_ERROR);
}

static bool event_handler(const struct event_header *eh)
{
	if (is_module_state_event(eh)) {
		struct module_state_event *event = cast_module_state_event(eh);

		if (check_state(event, MODULE_ID(main), MODULE_STATE_READY)) {
			static bool initialized;

			__ASSERT_NO_MSG(!initialized);
			initialized = true;

			k_delayed_work_init(&matrix_scan, matrix_scan_fn);

			init_fn();

			return false;
		}

		return false;
	}

	if (is_wake_up_event(eh)) {
		resume();

		return false;
	}

	if (is_power_down_event(eh)) {
		int err = suspend();

		if (!err) {
			module_set_state(MODULE_STATE_STANDBY);
			return false;
		} else if (err == -EALREADY) {
			/* Already suspended */
			return false;
		} else if (err == -EBUSY) {
			/* Cannot suspend while scanning */
			return true;
		}

		LOG_ERR("error while suspending");
		module_set_state(MODULE_STATE_ERROR);
		return true;
	}

	/* If event is unhandled, unsubscribe. */
	__ASSERT_NO_MSG(false);

	return false;
}
EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, module_state_event);
EVENT_SUBSCRIBE_EARLY(MODULE, power_down_event);
EVENT_SUBSCRIBE(MODULE, wake_up_event);
