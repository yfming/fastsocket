/*
 *  Bluetooth Wacom Tablet support
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2006-2007 Jiri Kosina
 *  Copyright (c) 2007 Paul Walmsley
 *  Copyright (c) 2008 Jiri Slaby <jirislaby@gmail.com>
 *  Copyright (c) 2006 Andrew Zabolotny <zap@homelink.ru>
 *  Copyright (c) 2009 Bastien Nocera <hadess@hadess.net>
 *  Copyright (c) 2011 Przemysaw Firszt <przemo@firszt.eu>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>

#include "hid-ids.h"

#define PAD_DEVICE_ID	0x0F

struct wacom_data {
	__u16 tool;
	__u16 butstate;
	__u32 id;
	__u32 serial;
	__u8 features;
	unsigned char high_speed;
};

static void wacom_set_features(struct hid_device *hdev)
{
	int ret;
	__u8 rep_data[2];

	/*set high speed, tablet mode*/
	rep_data[0] = 0x03;
	rep_data[1] = 0x20;
	ret = hdev->hid_output_raw_report(hdev, rep_data, 2,
				HID_FEATURE_REPORT);
	return;
}

static void wacom_poke(struct hid_device *hdev, u8 speed)
{
	struct wacom_data *wdata = hid_get_drvdata(hdev);
	int limit, ret;
	char rep_data[2];

	rep_data[0] = 0x03 ; rep_data[1] = 0x00;
	limit = 3;
	do {
		ret = hdev->hid_output_raw_report(hdev, rep_data, 2,
				HID_FEATURE_REPORT);
	} while (ret < 0 && limit-- > 0);

	if (ret >= 0) {
		if (speed == 0)
			rep_data[0] = 0x05;
		else
			rep_data[0] = 0x06;

		rep_data[1] = 0x00;
		limit = 3;
		do {
			ret = hdev->hid_output_raw_report(hdev, rep_data, 2,
					HID_FEATURE_REPORT);
		} while (ret < 0 && limit-- > 0);

		if (ret >= 0) {
			wdata->high_speed = speed;
			return;
		}
	}

	/*
	 * Note that if the raw queries fail, it's not a hard failure and it
	 * is safe to continue
	 */
	dev_warn(&hdev->dev, "failed to poke device, command %d, err %d\n",
				rep_data[0], ret);
	return;
}

static ssize_t wacom_show_speed(struct device *dev,
				struct device_attribute
				*attr, char *buf)
{
	struct wacom_data *wdata = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%i\n", wdata->high_speed);
}

static ssize_t wacom_store_speed(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	int new_speed;

	if (sscanf(buf, "%1d", &new_speed ) != 1)
		return -EINVAL;

	if (new_speed == 0 || new_speed == 1) {
		wacom_poke(hdev, new_speed);
		return strnlen(buf, PAGE_SIZE);
	} else
		return -EINVAL;
}

static DEVICE_ATTR(speed, S_IRUGO | S_IWUSR | S_IWGRP,
		wacom_show_speed, wacom_store_speed);

static int wacom_gr_parse_report(struct hid_device *hdev,
			struct wacom_data *wdata,
			struct input_dev *input, unsigned char *data)
{
	int tool, x, y, rw;

	tool = 0;
	/* Get X & Y positions */
	x = le16_to_cpu(*(__le16 *) &data[2]);
	y = le16_to_cpu(*(__le16 *) &data[4]);

	/* Get current tool identifier */
	if (data[1] & 0x90) { /* If pen is in the in/active area */
		switch ((data[1] >> 5) & 3) {
		case 0:	/* Pen */
			tool = BTN_TOOL_PEN;
			break;

		case 1: /* Rubber */
			tool = BTN_TOOL_RUBBER;
			break;

		case 2: /* Mouse with wheel */
		case 3: /* Mouse without wheel */
			tool = BTN_TOOL_MOUSE;
			break;
		}

		/* Reset tool if out of active tablet area */
		if (!(data[1] & 0x10))
			tool = 0;
	}

	/* If tool changed, notify input subsystem */
	if (wdata->tool != tool) {
		if (wdata->tool) {
			/* Completely reset old tool state */
			if (wdata->tool == BTN_TOOL_MOUSE) {
				input_report_key(input, BTN_LEFT, 0);
				input_report_key(input, BTN_RIGHT, 0);
				input_report_key(input, BTN_MIDDLE, 0);
				input_report_abs(input, ABS_DISTANCE,
						input->absmax[ABS_DISTANCE]);
			} else {
				input_report_key(input, BTN_TOUCH, 0);
				input_report_key(input, BTN_STYLUS, 0);
				input_report_key(input, BTN_STYLUS2, 0);
				input_report_abs(input, ABS_PRESSURE, 0);
			}
			input_report_key(input, wdata->tool, 0);
			input_sync(input);
		}
		wdata->tool = tool;
		if (tool)
			input_report_key(input, tool, 1);
	}

	if (tool) {
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);

		switch ((data[1] >> 5) & 3) {
		case 2: /* Mouse with wheel */
			input_report_key(input, BTN_MIDDLE, data[1] & 0x04);
			rw = (data[6] & 0x01) ? -1 :
				(data[6] & 0x02) ? 1 : 0;
			input_report_rel(input, REL_WHEEL, rw);
			/* fall through */

		case 3: /* Mouse without wheel */
			input_report_key(input, BTN_LEFT, data[1] & 0x01);
			input_report_key(input, BTN_RIGHT, data[1] & 0x02);
			/* Compute distance between mouse and tablet */
			rw = 44 - (data[6] >> 2);
			if (rw < 0)
				rw = 0;
			else if (rw > 31)
				rw = 31;
			input_report_abs(input, ABS_DISTANCE, rw);
			break;

		default:
			input_report_abs(input, ABS_PRESSURE,
					data[6] | (((__u16) (data[1] & 0x08)) << 5));
			input_report_key(input, BTN_TOUCH, data[1] & 0x01);
			input_report_key(input, BTN_STYLUS, data[1] & 0x02);
			input_report_key(input, BTN_STYLUS2, (tool == BTN_TOOL_PEN) && data[1] & 0x04);
			break;
		}

		input_sync(input);
	}

	/* Report the state of the two buttons at the top of the tablet
	 * as two extra fingerpad keys (buttons 4 & 5). */
	rw = data[7] & 0x03;
	if (rw != wdata->butstate) {
		wdata->butstate = rw;
		input_report_key(input, BTN_0, rw & 0x02);
		input_report_key(input, BTN_1, rw & 0x01);
		input_report_key(input, BTN_TOOL_FINGER, 0xf0);
		input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
		input_sync(input);
	}

	return 1;
}

static void wacom_i4_parse_button_report(struct wacom_data *wdata,
			struct input_dev *input, unsigned char *data)
{
	__u16 new_butstate;

	new_butstate = (data[3] << 1) | (data[2] & 0x01);
	if (new_butstate != wdata->butstate) {
		wdata->butstate = new_butstate;
		input_report_key(input, BTN_0, new_butstate & 0x001);
		input_report_key(input, BTN_1, new_butstate & 0x002);
		input_report_key(input, BTN_2, new_butstate & 0x004);
		input_report_key(input, BTN_3, new_butstate & 0x008);
		input_report_key(input, BTN_4, new_butstate & 0x010);
		input_report_key(input, BTN_5, new_butstate & 0x020);
		input_report_key(input, BTN_6, new_butstate & 0x040);
		input_report_key(input, BTN_7, new_butstate & 0x080);
		input_report_key(input, BTN_8, new_butstate & 0x100);
		input_report_key(input, BTN_TOOL_FINGER, 1);
		input_report_abs(input, ABS_MISC, PAD_DEVICE_ID);
		input_event(input, EV_MSC, MSC_SERIAL, 0xffffffff);
		input_sync(input);
	}
}

static void wacom_i4_parse_pen_report(struct wacom_data *wdata,
			struct input_dev *input, unsigned char *data)
{
	__u16 x, y, pressure;
	__u8 distance;

	switch (data[1]) {
	case 0x80: /* Out of proximity report */
		input_report_key(input, BTN_TOUCH, 0);
		input_report_abs(input, ABS_PRESSURE, 0);
		input_report_key(input, wdata->tool, 0);
		input_report_abs(input, ABS_MISC, 0);
		input_event(input, EV_MSC, MSC_SERIAL, wdata->serial);
		wdata->tool = 0;
		input_sync(input);
		break;
	case 0xC2: /* Tool report */
		wdata->id = ((data[2] << 4) | (data[3] >> 4) |
			((data[7] & 0x0f) << 20) |
			((data[8] & 0xf0) << 12));
		wdata->serial = ((data[3] & 0x0f) << 28) +
				(data[4] << 20) + (data[5] << 12) +
				(data[6] << 4) + (data[7] >> 4);

		switch (wdata->id) {
		case 0x100802:
			wdata->tool = BTN_TOOL_PEN;
			break;
		case 0x10080A:
			wdata->tool = BTN_TOOL_RUBBER;
			break;
		}
		break;
	default: /* Position/pressure report */
		x = data[2] << 9 | data[3] << 1 | ((data[9] & 0x02) >> 1);
		y = data[4] << 9 | data[5] << 1 | (data[9] & 0x01);
		pressure = (data[6] << 3) | ((data[7] & 0xC0) >> 5)
			| (data[1] & 0x01);
		distance = (data[9] >> 2) & 0x3f;

		input_report_key(input, BTN_TOUCH, pressure > 1);

		input_report_key(input, BTN_STYLUS, data[1] & 0x02);
		input_report_key(input, BTN_STYLUS2, data[1] & 0x04);
		input_report_key(input, wdata->tool, 1);
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
		input_report_abs(input, ABS_DISTANCE, distance);
		input_report_abs(input, ABS_PRESSURE, pressure);
		input_report_abs(input, ABS_MISC, wdata->id);
		input_event(input, EV_MSC, MSC_SERIAL, wdata->serial);
		input_sync(input);
		break;
	}

	return;
}

static void wacom_i4_parse_report(struct hid_device *hdev,
			struct wacom_data *wdata,
			struct input_dev *input, unsigned char *data)
{
	switch (data[0]) {
	case 0x00: /* Empty report */
		break;
	case 0x02: /* Pen report */
		wacom_i4_parse_pen_report(wdata, input, data);
		break;
	case 0x03: /* Features Report */
		wdata->features = data[2];
		break;
	case 0x0C: /* Button report */
		wacom_i4_parse_button_report(wdata, input, data);
		break;
	default:
		dev_err(&(hdev)->dev, "Unknown report: %d,%d\n", data[0], data[1]);
		break;
	}
}

static int wacom_raw_event(struct hid_device *hdev, struct hid_report *report,
		u8 *raw_data, int size)
{
	struct wacom_data *wdata = hid_get_drvdata(hdev);
	struct hid_input *hidinput;
	struct input_dev *input;
	unsigned char *data = (unsigned char *) raw_data;
	int i;

	if (!(hdev->claimed & HID_CLAIMED_INPUT))
		return 0;

	hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
	input = hidinput->input;

	/* Check if this is a tablet report */
	if (data[0] != 0x03)
		return 0;

	switch (hdev->product) {
	case USB_DEVICE_ID_WACOM_GRAPHIRE_BLUETOOTH:
		return wacom_gr_parse_report(hdev, wdata, input, data);
		break;
	case USB_DEVICE_ID_WACOM_INTUOS4_BLUETOOTH:
		i = 1;

		switch (data[0]) {
		case 0x04:
			wacom_i4_parse_report(hdev, wdata, input, data + i);
			i += 10;
			/* fall through */
		case 0x03:
			wacom_i4_parse_report(hdev, wdata, input, data + i);
			i += 10;
			wacom_i4_parse_report(hdev, wdata, input, data + i);
			break;
		default:
			dev_err(&(hdev)->dev, "Unknown report: %d,%d size:%d\n",
					data[0], data[1], size);
			return 0;
		}
	}
	return 1;
}

static int wacom_input_mapped(struct hid_device *hdev, struct hid_input *hi,
	struct hid_field *field, struct hid_usage *usage, unsigned long **bit,
								int *max)
{
	struct input_dev *input = hi->input;

	/* Basics */
	input->evbit[0] |= BIT(EV_KEY) | BIT(EV_ABS) | BIT(EV_REL);

	__set_bit(REL_WHEEL, input->relbit);

	__set_bit(BTN_TOOL_PEN, input->keybit);
	__set_bit(BTN_TOUCH, input->keybit);
	__set_bit(BTN_STYLUS, input->keybit);
	__set_bit(BTN_STYLUS2, input->keybit);
	__set_bit(BTN_LEFT, input->keybit);
	__set_bit(BTN_RIGHT, input->keybit);
	__set_bit(BTN_MIDDLE, input->keybit);

	/* Pad */
	input->evbit[0] |= BIT(EV_MSC);

	__set_bit(MSC_SERIAL, input->mscbit);

	__set_bit(BTN_0, input->keybit);
	__set_bit(BTN_1, input->keybit);
	__set_bit(BTN_TOOL_FINGER, input->keybit);

	/* Distance, rubber and mouse */
	__set_bit(BTN_TOOL_RUBBER, input->keybit);
	__set_bit(BTN_TOOL_MOUSE, input->keybit);

	switch (hdev->product) {
	case USB_DEVICE_ID_WACOM_GRAPHIRE_BLUETOOTH:
		input_set_abs_params(input, ABS_X, 0, 16704, 4, 0);
		input_set_abs_params(input, ABS_Y, 0, 12064, 4, 0);
		input_set_abs_params(input, ABS_PRESSURE, 0, 511, 0, 0);
		input_set_abs_params(input, ABS_DISTANCE, 0, 32, 0, 0);
		break;
	case USB_DEVICE_ID_WACOM_INTUOS4_BLUETOOTH:
		__set_bit(ABS_MISC, input->absbit);
		__set_bit(BTN_2, input->keybit);
		__set_bit(BTN_3, input->keybit);
		__set_bit(BTN_4, input->keybit);
		__set_bit(BTN_5, input->keybit);
		__set_bit(BTN_6, input->keybit);
		__set_bit(BTN_7, input->keybit);
		__set_bit(BTN_8, input->keybit);
		input_set_abs_params(input, ABS_X, 0, 40640, 4, 0);
		input_set_abs_params(input, ABS_Y, 0, 25400, 4, 0);
		input_set_abs_params(input, ABS_PRESSURE, 0, 2047, 0, 0);
		input_set_abs_params(input, ABS_DISTANCE, 0, 63, 0, 0);
		break;
	}

	return 0;
}

static int wacom_probe(struct hid_device *hdev,
		const struct hid_device_id *id)
{
	struct wacom_data *wdata;
	int ret;

	wdata = kzalloc(sizeof(*wdata), GFP_KERNEL);
	if (wdata == NULL) {
		dev_err(&hdev->dev, "can't alloc wacom descriptor\n");
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, wdata);

	/* Parse the HID report now */
	ret = hid_parse(hdev);
	if (ret) {
		dev_err(&hdev->dev, "parse failed\n");
		goto err_free;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		dev_err(&hdev->dev, "hw start failed\n");
		goto err_free;
	}

	ret = device_create_file(&hdev->dev, &dev_attr_speed);
	if (ret)
		dev_warn(&hdev->dev,
			"can't create sysfs speed attribute err: %d\n", ret);

	switch (hdev->product) {
	case USB_DEVICE_ID_WACOM_GRAPHIRE_BLUETOOTH:
		/* Set Wacom mode 2 with high reporting speed */
		wacom_poke(hdev, 1);
		break;
	case USB_DEVICE_ID_WACOM_INTUOS4_BLUETOOTH:
		wdata->features = 0;
		wacom_set_features(hdev);
		break;
	}

	return 0;
err_free:
	kfree(wdata);
	return ret;
}

static void wacom_remove(struct hid_device *hdev)
{
	device_remove_file(&hdev->dev, &dev_attr_speed);
	hid_hw_stop(hdev);
	kfree(hid_get_drvdata(hdev));
}

static const struct hid_device_id wacom_devices[] = {
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE_BLUETOOTH) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS4_BLUETOOTH) },

	{ }
};
MODULE_DEVICE_TABLE(hid, wacom_devices);

static struct hid_driver wacom_driver = {
	.name = "wacom",
	.id_table = wacom_devices,
	.probe = wacom_probe,
	.remove = wacom_remove,
	.raw_event = wacom_raw_event,
	.input_mapped = wacom_input_mapped,
};

static int __init wacom_init(void)
{
	int ret;

	ret = hid_register_driver(&wacom_driver);
	if (ret)
		printk(KERN_ERR "can't register wacom driver\n");
	printk(KERN_ERR "wacom driver registered\n");
	return ret;
}

static void __exit wacom_exit(void)
{
	hid_unregister_driver(&wacom_driver);
}

module_init(wacom_init);
module_exit(wacom_exit);
MODULE_LICENSE("GPL");

