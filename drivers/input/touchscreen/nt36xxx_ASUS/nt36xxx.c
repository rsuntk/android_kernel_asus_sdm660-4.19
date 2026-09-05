// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 *
 * $Revision: 47247 $
 * $Date: 2019-07-10 10:41:36 +0800 (Wed, 10 Jul 2019) $
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/input/mt.h>
#include <linux/proc_fs.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/init.h>

#include "nt36xxx.h"

struct nvt_ts_data *ts;
static uint8_t bTouchIsAwake;
static int __always_inline nvt_fb_notifier_callback(struct notifier_block *self,
						    unsigned long event,
						    void *data);

#if BOOT_UPDATE_FIRMWARE
static struct workqueue_struct *nvt_fwu_wq;
extern void Boot_Update_Firmware(struct work_struct *work);
#endif

#if WAKEUP_GESTURE
static struct wakeup_source *gesture_wakelock;
#define NVT_GESTURE_MODE "tpd_gesture"

static long gesture_mode = 0;

static ssize_t nvt_gesture_mode_get_proc(struct file *file, char __user *buffer,
					  size_t size, loff_t *ppos)
{
	char ptr[16];
	size_t len;

	len = scnprintf(ptr, sizeof(ptr), "%d\n", gesture_mode ? 1 : 0);
	return simple_read_from_buffer(buffer, size, ppos, ptr, len);
}

static ssize_t nvt_gesture_mode_set_proc(struct file *filp,
					  const char __user *buffer,
					  size_t count, loff_t *off)
{
	int ret = 0;
	long mode_val = 0;

	ret = kstrtol_from_user(buffer, count, 0, &mode_val);
	if (!ret) {
		if (mode_val == 0) {
			gesture_mode = 0;
		} else {
			gesture_mode = 0x1FF;
		}
	}

	return count;
}

static struct proc_dir_entry *nvt_gesture_mode_proc = NULL;
static const struct file_operations gesture_mode_proc_ops = {
	.owner = THIS_MODULE,
	.read = nvt_gesture_mode_get_proc,
	.write = nvt_gesture_mode_set_proc,
};

static const uint16_t gesture_key_array[] = {
	KEY_TP_GESTURE_C, // GESTURE_WORD_C
	KEY_TP_GESTURE_W, // GESTURE_WORD_W
	KEY_TP_GESTURE_V, // GESTURE_WORD_V
	KEY_WAKEUP, // GESTURE_DOUBLE_CLICK
	KEY_TP_GESTURE_Z, // GESTURE_WORD_Z
	KEY_TP_GESTURE_M, // GESTURE_WORD_M
	KEY_TP_GESTURE_O, // GESTURE_WORD_O
	KEY_TP_GESTURE_E, // GESTURE_WORD_E
	KEY_TP_GESTURE_S, // GESTURE_WORD_S
	KEY_TP_GESTURE_SWIPE_UP, // GESTURE_SLIDE_UP
	KEY_TP_GESTURE_SWIPE_DOWN, // GESTURE_SLIDE_DOWN
	KEY_TP_GESTURE_SWIPE_LEFT, // GESTURE_SLIDE_LEFT
	KEY_TP_GESTURE_SWIPE_RIGHT, // GESTURE_SLIDE_RIGHT
};

#define GESTURE_WORD_C 12
#define GESTURE_WORD_W 13
#define GESTURE_WORD_V 14
#define GESTURE_DOUBLE_CLICK 15
#define GESTURE_WORD_Z 16
#define GESTURE_WORD_M 17
#define GESTURE_WORD_O 18
#define GESTURE_WORD_e 19
#define GESTURE_WORD_S 20
#define GESTURE_SLIDE_UP 21
#define GESTURE_SLIDE_DOWN 22
#define GESTURE_SLIDE_LEFT 23
#define GESTURE_SLIDE_RIGHT 24
#define DATA_PROTOCOL 30
#define FUNCPAGE_GESTURE 1

inline void nvt_ts_wakeup_gesture_report(uint8_t gesture_id, uint8_t *data)
{
	uint32_t keycode = 0;
	uint8_t func_type = data[2];
	uint8_t func_id = data[3];

	/* support fw specifal data protocol */
	if ((gesture_id == DATA_PROTOCOL) && (func_type == FUNCPAGE_GESTURE)) {
		gesture_id = func_id;
	} else if (gesture_id > DATA_PROTOCOL) {
		return;
	}

	switch (gesture_id) {
	case GESTURE_WORD_C:
		keycode = gesture_key_array[0];
		break;
	case GESTURE_WORD_W:
		keycode = gesture_key_array[1];
		break;
	case GESTURE_WORD_V:
		keycode = gesture_key_array[2];
		break;
	case GESTURE_DOUBLE_CLICK:
		keycode = gesture_key_array[3];
		break;
	case GESTURE_WORD_Z:
		keycode = gesture_key_array[4];
		break;
	case GESTURE_WORD_M:
		keycode = gesture_key_array[5];
		break;
	case GESTURE_WORD_O:
		keycode = gesture_key_array[6];
		break;
	case GESTURE_WORD_e:
		keycode = gesture_key_array[7];
		break;
	case GESTURE_WORD_S:
		keycode = gesture_key_array[8];
		break;
	case GESTURE_SLIDE_UP:
		keycode = gesture_key_array[9];
		break;
	case GESTURE_SLIDE_DOWN:
		keycode = gesture_key_array[10];
		break;
	case GESTURE_SLIDE_LEFT:
		keycode = gesture_key_array[11];
		break;
	case GESTURE_SLIDE_RIGHT:
		keycode = gesture_key_array[12];
		break;
	default:
		break;
	}

	if (keycode > 0) {
		input_report_key(ts->input_dev, keycode, 1);
		input_sync(ts->input_dev);
		input_report_key(ts->input_dev, keycode, 0);
		input_sync(ts->input_dev);
	}
}
#endif

#if NVT_POWER_SOURCE_CUST_EN
static int nvt_lcm_bias_power_init(struct nvt_ts_data *data)
{
	int ret = 0;

	data->lcm_lab = regulator_get(&data->client->dev, "lcm_lab");
	if (IS_ERR(data->lcm_lab)) {
		ret = PTR_ERR(data->lcm_lab);
		goto exit;
	}

	if (regulator_count_voltages(data->lcm_lab) > 0) {
		ret = regulator_set_voltage(data->lcm_lab, LCM_LAB_MIN_UV,
					    LCM_LAB_MAX_UV);
		if (ret)
			goto reg_lcm_lab_put;
	}

	data->lcm_ibb = regulator_get(&data->client->dev, "lcm_ibb");
	if (IS_ERR(data->lcm_ibb)) {
		ret = PTR_ERR(data->lcm_ibb);
		goto reg_set_lcm_lab_vtg;
	}

	if (regulator_count_voltages(data->lcm_ibb) > 0) {
		ret = regulator_set_voltage(data->lcm_ibb, LCM_IBB_MIN_UV,
					    LCM_IBB_MAX_UV);
		if (ret)
			goto reg_lcm_ibb_put;
	}

	return 0;

reg_lcm_ibb_put:
	regulator_put(data->lcm_ibb);
	data->lcm_ibb = NULL;
reg_set_lcm_lab_vtg:
	if (regulator_count_voltages(data->lcm_lab) > 0) {
		regulator_set_voltage(data->lcm_lab, 0, LCM_LAB_MAX_UV);
	}
reg_lcm_lab_put:
	regulator_put(data->lcm_lab);
	data->lcm_lab = NULL;
exit:
	return ret;
}

static int nvt_lcm_bias_power_deinit(struct nvt_ts_data *data)
{
	if (data->lcm_ibb != NULL) {
		if (regulator_count_voltages(data->lcm_ibb) > 0)
			regulator_set_voltage(data->lcm_ibb, 0, LCM_LAB_MAX_UV);

		regulator_put(data->lcm_ibb);
	}
	if (data->lcm_lab != NULL) {
		if (regulator_count_voltages(data->lcm_lab) > 0)
			regulator_set_voltage(data->lcm_lab, 0, LCM_LAB_MAX_UV);

		regulator_put(data->lcm_lab);
	}
	return 0;
}

static int nvt_lcm_power_source_ctrl(struct nvt_ts_data *data, int enable)
{
	int rc;

	if (data->lcm_lab != NULL && data->lcm_ibb != NULL) {
		if (enable) {
			if (atomic_inc_return(&data->lcm_lab_power) == 1) {
				rc = regulator_enable(data->lcm_lab);
				if (rc)
					atomic_dec(&data->lcm_lab_power);
			} else
				atomic_dec(&data->lcm_lab_power);

			if (atomic_inc_return(&data->lcm_ibb_power) == 1) {
				rc = regulator_enable(data->lcm_ibb);
				if (rc)
					atomic_dec(&data->lcm_ibb_power);
			} else
				atomic_dec(&data->lcm_ibb_power);
		} else {
			if (atomic_dec_return(&data->lcm_lab_power) == 0) {
				rc = regulator_disable(data->lcm_lab);
				if (rc)
					atomic_inc(&data->lcm_lab_power);
			} else
				atomic_inc(&data->lcm_lab_power);

			if (atomic_dec_return(&data->lcm_ibb_power) == 0) {
				rc = regulator_disable(data->lcm_ibb);
				if (rc)
					atomic_inc(&data->lcm_ibb_power);
			} else
				atomic_inc(&data->lcm_ibb_power);
		}
	}

	return 0;
}
#endif

static void __always_inline nvt_irq_enable(bool enable)
{
	if (enable) {
		if (!ts->irq_enabled) {
			enable_irq(ts->client->irq);
			enable_irq_wake(ts->client->irq);
			ts->irq_enabled = true;
		}
	} else {
		if (ts->irq_enabled) {
			disable_irq(ts->client->irq);
			disable_irq_wake(ts->client->irq);
			ts->irq_enabled = false;
		}
	}
}

inline int32_t CTP_I2C_READ(struct i2c_client *client, uint16_t address,
			    uint8_t *buf, uint16_t len)
{
	struct i2c_msg msgs[2];
	int32_t ret = -1;

	mutex_lock(&ts->xbuf_lock);

	msgs[0].flags = !I2C_M_RD;
	msgs[0].addr = address;
	msgs[0].len = 1;
	msgs[0].buf = &buf[0];

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr = address;
	msgs[1].len = len - 1;
	msgs[1].buf = ts->xbuf;

	ret = i2c_transfer(client->adapter, msgs, 2);
	memcpy(buf + 1, ts->xbuf, len - 1);

	mutex_unlock(&ts->xbuf_lock);

	return ret;
}

inline int32_t CTP_I2C_WRITE(struct i2c_client *client, uint16_t address,
			     uint8_t *buf, uint16_t len)
{
	struct i2c_msg msg;
	int32_t ret = -1;

	mutex_lock(&ts->xbuf_lock);

	msg.flags = !I2C_M_RD;
	msg.addr = address;
	msg.len = len;
	memcpy(ts->xbuf, buf, len);
	msg.buf = ts->xbuf;

	ret = i2c_transfer(client->adapter, &msg, 1);

	mutex_unlock(&ts->xbuf_lock);

	return ret;
}

inline int32_t nvt_set_page(uint16_t i2c_addr, uint32_t addr)
{
	uint8_t buf[4] = { 0 };

	buf[0] = 0xFF;
	buf[1] = (addr >> 16) & 0xFF;
	buf[2] = (addr >> 8) & 0xFF;

	return CTP_I2C_WRITE(ts->client, i2c_addr, buf, 3);
}

inline void nvt_sw_reset_idle(void)
{
	uint8_t buf[4] = { 0 };

	buf[0] = 0x00;
	buf[1] = 0xA5;
	CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

	msleep(15);
}

inline void nvt_bootloader_reset(void)
{
	uint8_t buf[8] = { 0 };

	buf[0] = 0x00;
	buf[1] = 0x69;
	CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

	msleep(35);
}

inline int32_t nvt_clear_fw_status(void)
{
	uint8_t buf[8] = { 0 };

	nvt_set_page(I2C_FW_Address,
		     ts->mmap->EVENT_BUF_ADDR |
			     EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE);

	buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
	buf[1] = 0x00;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

	buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
	buf[1] = 0xFF;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

	return 0;
}

inline int32_t nvt_check_fw_status(void)
{
	uint8_t buf[8] = { 0 };

	nvt_set_page(I2C_FW_Address,
		     ts->mmap->EVENT_BUF_ADDR |
			     EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE);

	buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
	buf[1] = 0x00;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

	return 0;
}

inline int32_t nvt_check_fw_reset_state(RST_COMPLETE_STATE check_reset_state)
{
	uint8_t buf[8] = { 0 };
	int32_t ret = 0;

	buf[0] = EVENT_MAP_RESET_COMPLETE;
	buf[1] = 0x00;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 6);

	return ret;
}

inline int32_t nvt_read_pid(void)
{
	uint8_t buf[3] = { 0 };
	int32_t ret = 0;

	nvt_set_page(I2C_FW_Address,
		     ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_PROJECTID);

	buf[0] = EVENT_MAP_PROJECTID;
	buf[1] = 0x00;
	buf[2] = 0x00;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 3);

	ts->nvt_pid = (buf[2] << 8) + buf[1];

	return ret;
}

inline int32_t nvt_get_fw_info(void)
{
	uint8_t buf[64] = { 0 };
	uint32_t retry_count = 0;
	int32_t ret = 0;

info_retry:
	nvt_set_page(I2C_FW_Address,
		     ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_FWINFO);

	buf[0] = EVENT_MAP_FWINFO;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 17);
	ts->x_num = buf[3];
	ts->y_num = buf[4];
	ts->abs_x_max = (uint16_t)((buf[5] << 8) | buf[6]);
	ts->abs_y_max = (uint16_t)((buf[7] << 8) | buf[8]);

	if ((buf[1] + buf[2]) != 0xFF) {
		ts->x_num = 18;
		ts->y_num = 32;
		ts->abs_x_max = TOUCH_DEFAULT_MAX_WIDTH;
		ts->abs_y_max = TOUCH_DEFAULT_MAX_HEIGHT;

		if (retry_count < 3) {
			retry_count++;
			goto info_retry;
		} else
			ret = -1;
	} else
		ret = 0;

	nvt_read_pid();

	return ret;
}

static inline void nvt_parse_dt(struct device *dev)
{
#ifdef CONFIG_OF
	struct device_node *np = dev->of_node;

#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = of_get_named_gpio_flags(np, "novatek,reset-gpio", 0,
						 &ts->reset_flags);
#endif
	ts->irq_gpio = of_get_named_gpio_flags(np, "novatek,irq-gpio", 0,
					       &ts->irq_flags);
#else
#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = NVT_TOUCH_RST_PIN;
#endif
	ts->irq_gpio = NVT_TOUCH_INT_PIN;
#endif
}

static inline int nvt_gpio_config(struct nvt_ts_data *ts)
{
	int32_t ret = 0;

#if NVT_TOUCH_SUPPORT_HW_RST
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = gpio_request_one(ts->reset_gpio, GPIOF_OUT_INIT_HIGH,
				       "NVT-tp-rst");
		if (ret)
			goto err_request_reset_gpio;
	}
#endif

	if (gpio_is_valid(ts->irq_gpio)) {
		ret = gpio_request_one(ts->irq_gpio, GPIOF_IN, "NVT-int");
		if (ret)
			goto err_request_irq_gpio;
	}

	return ret;

err_request_irq_gpio:
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_free(ts->reset_gpio);
err_request_reset_gpio:
#endif
	return ret;
}

static inline void nvt_gpio_deconfig(struct nvt_ts_data *ts)
{
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
#if NVT_TOUCH_SUPPORT_HW_RST
	if (gpio_is_valid(ts->reset_gpio))
		gpio_free(ts->reset_gpio);
#endif
}

static inline uint8_t nvt_fw_recovery(uint8_t *point_data)
{
	uint8_t i = 0;
	uint8_t detected = true;

	for (i = 1; i < 7; i++) {
		if (point_data[i] != 0x77) {
			detected = false;
			break;
		}
	}

	return detected;
}

static inline void nvt_ts_worker(struct work_struct *work)
{
	struct nvt_ts_data *ts = container_of(work, struct nvt_ts_data, irq_work);
	int32_t ret;
	int32_t i;
	int32_t finger_cnt = 0;
	uint8_t point_data[POINT_DATA_LEN + 1] = { 0 };
	uint8_t input_id;
	uint8_t press_id[TOUCH_MAX_FINGER_NUM] = { 0 };
	uint32_t position;
	uint32_t input_x;
	uint32_t input_y;

	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	sched_setscheduler(current, SCHED_RR, &param);

	mutex_lock(&ts->lock);

	ret = CTP_I2C_READ(ts->client, I2C_FW_Address, point_data,
			   POINT_DATA_LEN + 1);
	if (unlikely(ret < 0))
		goto XFER_ERROR;

	if (nvt_fw_recovery(point_data))
		goto XFER_ERROR;

#if WAKEUP_GESTURE
	if (unlikely(bTouchIsAwake == 0)) {
		input_id = (uint8_t)(point_data[1] >> 3);
		nvt_ts_wakeup_gesture_report(input_id, point_data);
		nvt_irq_enable(true);
		goto XFER_ERROR;
	}
#endif

	for (i = 0; i < ts->max_touch_num; i++) {
		position = 1 + 6 * i;
		input_id = (uint8_t)(point_data[position] >> 3);

		if ((input_id == 0) || (input_id > ts->max_touch_num))
			continue;

		if (likely(((point_data[position] & 0x07) == 0x01) ||
			   ((point_data[position] & 0x07) == 0x02))) {
			input_x = (uint32_t)(point_data[position + 1] << 4) +
				  (uint32_t)(point_data[position + 3] >> 4);
			input_y = (uint32_t)(point_data[position + 2] << 4) +
				  (uint32_t)(point_data[position + 3] & 0x0F);

			press_id[input_id - 1] = 1;
			input_mt_slot(ts->input_dev, input_id - 1);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);

			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, input_x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, input_y);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, TOUCH_FORCE_NUM);

			finger_cnt++;
		}
	}

	for (i = 0; i < ts->max_touch_num; i++) {
		if (likely(press_id[i] != 1)) {
			input_mt_slot(ts->input_dev, i);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
		}
	}

	input_report_key(ts->input_dev, BTN_TOUCH, (finger_cnt > 0));
	input_sync(ts->input_dev);

XFER_ERROR:
	mutex_unlock(&ts->lock);
}

static inline irqreturn_t nvt_ts_work_func(int irq, void *data)
{
	struct nvt_ts_data *ts = data;

#if WAKEUP_GESTURE
	if (unlikely(bTouchIsAwake == 0))
		__pm_wakeup_event(gesture_wakelock, msecs_to_jiffies(5000));
#endif

	queue_work(ts->coord_workqueue, &ts->irq_work);

	return IRQ_HANDLED;
}

inline void nvt_stop_crc_reboot(void)
{
	uint8_t buf[8] = { 0 };
	int32_t retry = 0;

	nvt_set_page(I2C_BLDR_Address, 0x1F64E);

	buf[0] = 0x4E;
	CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 4);

	if ((buf[1] == 0xFC) ||
	    ((buf[1] == 0xFF) && (buf[2] == 0xFF) && (buf[3] == 0xFF))) {
		for (retry = 5; retry > 0; retry--) {
			buf[0] = 0x00;
			buf[1] = 0xA5;
			CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

			buf[0] = 0x00;
			buf[1] = 0xA5;
			CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);
			msleep(1);

			nvt_set_page(I2C_BLDR_Address, 0x3F135);

			buf[0] = 0x35;
			buf[1] = 0xA5;
			CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 2);

			nvt_set_page(I2C_BLDR_Address, 0x3F135);

			buf[0] = 0x35;
			buf[1] = 0x00;
			CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 2);

			if (buf[1] == 0xA5)
				break;
		}
	}

	return;
}

static inline int8_t nvt_ts_check_chip_ver_trim(void)
{
	uint8_t buf[8] = { 0 };
	int32_t retry = 0;
	int32_t list = 0;
	int32_t i = 0;
	int32_t found_nvt_chip = 0;
	int32_t ret = -1;

	nvt_bootloader_reset();

	for (retry = 5; retry > 0; retry--) {
		nvt_sw_reset_idle();

		buf[0] = 0x00;
		buf[1] = 0x35;
		CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);
		msleep(10);

		nvt_set_page(I2C_BLDR_Address, 0x1F64E);

		buf[0] = 0x4E;
		buf[1] = 0x00;
		buf[2] = 0x00;
		buf[3] = 0x00;
		buf[4] = 0x00;
		buf[5] = 0x00;
		buf[6] = 0x00;
		CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 7);

		if ((buf[1] == 0xFC) || ((buf[1] == 0xFF) && (buf[2] == 0xFF) && (buf[3] == 0xFF))) {
			nvt_stop_crc_reboot();
			continue;
		}

		for (list = 0; list < (sizeof(trim_id_table) / sizeof(struct nvt_ts_trim_id_table)); list++) {
			found_nvt_chip = 0;

			for (i = 0; i < NVT_ID_BYTE_MAX; i++) {
				if (trim_id_table[list].mask[i]) {
					if (buf[i + 1] != trim_id_table[list].id[i])
						break;
				}
			}

			if (i == NVT_ID_BYTE_MAX)
				found_nvt_chip = 1;

			if (found_nvt_chip) {
				ts->mmap = trim_id_table[list].mmap;
				ts->carrier_system = trim_id_table[list].hwinfo->carrier_system;
				ret = 0;
				goto out;
			} else {
				ts->mmap = NULL;
				ret = -1;
			}
		}

		msleep(10);
	}

out:
	return ret;
}

static inline int32_t nvt_ts_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	int32_t ret = 0;

	ts = kmalloc(sizeof(struct nvt_ts_data), GFP_KERNEL);
	if (ts == NULL)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	nvt_parse_dt(&client->dev);

#if NVT_POWER_SOURCE_CUST_EN
	atomic_set(&ts->lcm_lab_power, 0);
	atomic_set(&ts->lcm_ibb_power, 0);

	ret = nvt_lcm_bias_power_init(ts);
	if (ret)
		goto err_power_resource_init_fail;

	nvt_lcm_power_source_ctrl(ts, 1);
#endif

	ret = nvt_gpio_config(ts);
	if (ret)
		goto err_gpio_config_failed;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	mutex_init(&ts->lock);
	mutex_init(&ts->xbuf_lock);

	msleep(10);

	ret = nvt_ts_check_chip_ver_trim();
	if (ret) {
		ret = -EINVAL;
		goto err_chipvertrim_failed;
	}

	nvt_bootloader_reset();
	nvt_check_fw_reset_state(RESET_STATE_INIT);
	nvt_get_fw_info();

	ts->coord_workqueue = alloc_workqueue("nvt_ts_workqueue", WQ_HIGHPRI, 0);
	if (!ts->coord_workqueue) {
		ret = -ENOMEM;
		goto err_create_nvt_ts_workqueue_failed;
	}
	INIT_WORK(&ts->irq_work, nvt_ts_worker);

	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	ts->max_touch_num = TOUCH_MAX_FINGER_NUM;
	ts->int_trigger_type = INT_TRIGGER_TYPE;

	ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	ts->input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

	input_mt_init_slots(ts->input_dev, ts->max_touch_num, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, TOUCH_FORCE_NUM,
			     0, 0);

#if TOUCH_MAX_FINGER_NUM > 1
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->abs_x_max,
			     0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->abs_y_max,
			     0, 0);
#endif

#if WAKEUP_GESTURE
	{
		int i;
		for (i = 0; i < ARRAY_SIZE(gesture_key_array); i++) {
			input_set_capability(ts->input_dev, EV_KEY, gesture_key_array[i]);
		}
		gesture_wakelock = wakeup_source_register(NULL, "poll-wake-lock");
	}
#endif

	snprintf(ts->phys, sizeof(ts->phys), "input/ts");
	ts->input_dev->name = NVT_TS_NAME;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_I2C;

	ret = input_register_device(ts->input_dev);
	if (ret)
		goto err_input_register_device_failed;

	//---set int-pin & request irq---
	client->irq = gpio_to_irq(ts->irq_gpio);
	if (client->irq) {
		ret = request_threaded_irq(client->irq, NULL, nvt_ts_work_func,
				ts->int_trigger_type | IRQF_ONESHOT, NVT_I2C_NAME, ts);
		if (ret != 0) 
			goto err_int_request_failed;

		disable_irq_nosync(client->irq);
		ts->irq_enabled = false;
	}

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 1);
#endif

#if BOOT_UPDATE_FIRMWARE
	nvt_fwu_wq = alloc_workqueue("nvt_fwu_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!nvt_fwu_wq) {
		ret = -ENOMEM;
		goto err_create_nvt_fwu_wq_failed;
	}
	INIT_DELAYED_WORK(&ts->nvt_fwu_work, Boot_Update_Firmware);
	queue_delayed_work(nvt_fwu_wq, &ts->nvt_fwu_work,
			   msecs_to_jiffies(14000));
#endif

#if WAKEUP_GESTURE
	nvt_gesture_mode_proc = proc_create(NVT_GESTURE_MODE, 0666, NULL,
					    &gesture_mode_proc_ops);
#endif

	ts->fb_notif.notifier_call = nvt_fb_notifier_callback;
	ret = fb_register_client(&ts->fb_notif);
	if (ret)
		goto err_register_fb_notif_failed;

	bTouchIsAwake = 1;
	nvt_irq_enable(true);

	return 0;

err_register_fb_notif_failed:
err_create_nvt_ts_workqueue_failed:
	if (ts->coord_workqueue)
		destroy_workqueue(ts->coord_workqueue);
#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
err_create_nvt_fwu_wq_failed:
#endif
#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 0);
#endif
	free_irq(client->irq, ts);
err_int_request_failed:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
err_input_register_device_failed:
	if (ts->input_dev) {
		input_free_device(ts->input_dev);
		ts->input_dev = NULL;
	}
err_input_dev_alloc_failed:
err_chipvertrim_failed:
	mutex_destroy(&ts->xbuf_lock);
	mutex_destroy(&ts->lock);
err_check_functionality_failed:
	nvt_gpio_deconfig(ts);
err_gpio_config_failed:
#if NVT_POWER_SOURCE_CUST_EN
	nvt_lcm_power_source_ctrl(ts, 0);
	nvt_lcm_bias_power_deinit(ts);
err_power_resource_init_fail:
#endif
	i2c_set_clientdata(client, NULL);
	if (ts) {
		kfree(ts);
		ts = NULL;
	}
	return ret;
}

static inline int32_t nvt_ts_remove(struct i2c_client *client)
{
	if (ts->coord_workqueue)
		destroy_workqueue(ts->coord_workqueue);

	fb_unregister_client(&ts->fb_notif);

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
#endif

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 0);
#endif

	nvt_irq_enable(false);
	free_irq(client->irq, ts);

#if NVT_POWER_SOURCE_CUST_EN
	nvt_lcm_bias_power_deinit(ts);
#endif

	mutex_destroy(&ts->xbuf_lock);
	mutex_destroy(&ts->lock);

	nvt_gpio_deconfig(ts);

	if (ts->input_dev) {
		input_unregister_device(ts->input_dev);
		ts->input_dev = NULL;
	}

	i2c_set_clientdata(client, NULL);

	if (ts) {
		kfree(ts);
		ts = NULL;
	}

	return 0;
}

static inline void nvt_ts_shutdown(struct i2c_client *client)
{
	nvt_irq_enable(false);
	fb_unregister_client(&ts->fb_notif);

#if NVT_POWER_SOURCE_CUST_EN
	nvt_lcm_power_source_ctrl(ts, 0);
#endif

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
#endif

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 0);
#endif
}

static int32_t __always_inline nvt_ts_suspend(struct device *dev)
{
	struct nvt_ts_data *data __maybe_unused;
	uint8_t buf[4] = { 0 };
	uint32_t i = 0;

	if (!bTouchIsAwake)
		return 0;

#if NVT_POWER_SOURCE_CUST_EN
	data = dev_get_drvdata(dev);
#endif

	mutex_lock(&ts->lock);
	bTouchIsAwake = 0;

#if WAKEUP_GESTURE
	if (((gesture_mode & 0x100) == 0) || ((gesture_mode & 0x0FF) == 0)) {
		nvt_irq_enable(false);
		buf[0] = EVENT_MAP_HOST_CMD;
		buf[1] = 0x11;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

		// Force deep sleep mode
		nvt_set_page(I2C_FW_Address, 0x11a50);
		buf[0] = 0x11a50 & 0xff;
		buf[1] = 0x11;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);
#if NVT_POWER_SOURCE_CUST_EN
		nvt_lcm_power_source_ctrl(data, 0);
#endif
	} else {
		buf[0] = EVENT_MAP_HOST_CMD;
		buf[1] = 0x13;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);
		nvt_irq_enable(true);
	}
#else
	nvt_irq_enable(false);
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x11;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

	nvt_set_page(I2C_FW_Address, 0x11a50);
	buf[0] = 0x11a50 & 0xff;
	buf[1] = 0x11;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);
#endif

	mutex_unlock(&ts->lock);

	for (i = 0; i < ts->max_touch_num; i++) {
		input_mt_slot(ts->input_dev, i);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
	}

	input_report_key(ts->input_dev, BTN_TOUCH, 0);
	input_sync(ts->input_dev);

	msleep(50);
	return 0;
}

static int32_t __always_inline nvt_ts_resume(struct device *dev)
{
#if NVT_POWER_SOURCE_CUST_EN
	struct nvt_ts_data *data = dev_get_drvdata(dev);
	nvt_lcm_power_source_ctrl(data, 1);
#endif

	if (bTouchIsAwake)
		return 0;

	mutex_lock(&ts->lock);

#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_set_value(ts->reset_gpio, 1);
#endif

	nvt_bootloader_reset();
	nvt_check_fw_reset_state(RESET_STATE_REK);

#if WAKEUP_GESTURE
	if (((gesture_mode & 0x100) == 0) || ((gesture_mode & 0x0FF) == 0)) {
		nvt_irq_enable(true);
	}
#endif

	bTouchIsAwake = 1;
	mutex_unlock(&ts->lock);
	return 0;
}

static int __always_inline nvt_fb_notifier_callback(struct notifier_block *self,
						    unsigned long event,
						    void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct nvt_ts_data *ts = container_of(self, struct nvt_ts_data, fb_notif);

	if (evdata && evdata->data && event == FB_EARLY_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_POWERDOWN)
			nvt_ts_suspend(&ts->client->dev);
	} else if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK)
			nvt_ts_resume(&ts->client->dev);
	}

	return 0;
}

static const struct i2c_device_id nvt_ts_id[] = {
	{ NVT_I2C_NAME, 0 },
	{},
};

#ifdef CONFIG_OF
static struct of_device_id nvt_match_table[] = {
	{ .compatible = "novatek,NVT-ts" },
	{},
};
#endif

static struct i2c_driver nvt_i2c_driver = {
	.probe		= nvt_ts_probe,
	.remove		= nvt_ts_remove,
	.shutdown	= nvt_ts_shutdown,
	.id_table	= nvt_ts_id,
	.driver = {
		.name	= NVT_I2C_NAME,
		.owner	= THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = nvt_match_table,
#endif
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static inline int32_t __init nvt_driver_init(void)
{
	int32_t ret = 0;

	ret = i2c_add_driver(&nvt_i2c_driver);
	if (ret)
		goto err_driver;

err_driver:
	return ret;
}
module_init(nvt_driver_init);

static inline void __exit nvt_driver_exit(void)
{
	i2c_del_driver(&nvt_i2c_driver);
}
module_exit(nvt_driver_exit);

MODULE_DESCRIPTION("Novatek Touchscreen Driver");
MODULE_LICENSE("GPL");
