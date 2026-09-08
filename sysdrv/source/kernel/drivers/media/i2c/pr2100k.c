// SPDX-License-Identifier: GPL-2.0
/*
 * Pixelplus PR2100K 2-Channel AHD to MIPI CSI-2 Video Decoder Driver
 *
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sched.h>
#include <linux/kthread.h>

#define DRIVER_VERSION			KERNEL_VERSION(1, 0, 0)
#define PR2100K_NAME			"pr2100k"

#define PR2100K_XVCLK_FREQ		27000000
#define PR2100K_LINK_FREQ_1188M		594000000UL
#define PR2100K_LINK_FREQ_594M		297000000UL
#define PR2100K_LANES			2
#define PR2100K_BITS_PER_SAMPLE		8

/* PR2100K preliminary datasheet Rev 0.7, common page register map. */
#define PR2100K_CHIP_ID_H_REG		0xFC
#define PR2100K_CHIP_ID_L_REG		0xFD
#define PR2100K_REV_ID_REG		0xFE
#define PR2100K_OPT_ID_REG		0xFB
#define PR2100K_CHIP_ID_H_VAL		0x21
#define PR2100K_CHIP_ID_L_VAL		0x00

#define PR2100K_PAGE_REG		0xFF
#define PR2100K_PAGE_COMMON		0x00
#define PR2100K_PAGE_CH0		0x01
#define PR2100K_PAGE_CH1		0x02
#define PR2100K_PAGE_MIPI_CTRL		0x05
#define PR2100K_PAGE_MIPI_DPHY		0x06

#define PR2100K_DET_VIDEO_BIT		BIT(3)
#define PR2100K_LOCK_STD_BIT		BIT(7)

#define PR2100K_MAX_CHANNELS		2
#define PR2100K_ACTIVE_DUAL		PR2100K_MAX_CHANNELS

enum pr2100k_format_id {
	PR2100K_FMT_1080P_25 = 0,
	PR2100K_FMT_1080P_30,
	PR2100K_FMT_720P_25,
	PR2100K_FMT_720P_30,
	PR2100K_FMT_UNKNOWN,
};

struct regval {
	u8 page;
	u8 addr;
	u8 val;
};

struct pr2100k_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 mipi_freq_idx;
	u32 bpp;
	u32 vc[PAD_MAX];
	const struct regval *reg_list;
	u32 reg_list_size;
};

struct pr2100k_channel {
	bool signal_present;
	bool locked;
	enum pr2100k_format_id fmt_id;
	u8 standard;
	u32 width;
	u32 height;
	u32 fps;
};

struct pr2100k {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator	*dovdd;
	struct regulator	*avdd;
	struct regulator	*dvdd;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct mutex		mutex;

	bool			power_on;
	bool			streaming;
	u8			stream_users;
	u16			chip_id;
	const struct pr2100k_mode *cur_mode;

	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;

	struct pr2100k_channel	channels[PR2100K_MAX_CHANNELS];
	u8			active_channel;
	struct task_struct	*detect_thread;
};

#define to_pr2100k(sd) container_of(sd, struct pr2100k, subdev)

#include "pr2100k_vendor_regs.inc"
#include "pr2100k_ch1_regs.inc"

/*
 * The validated vendor baseline programs CH0 for HDA 1080p25.  A 720p25
 * source needs matching decoder active timing and CSI-2 packet geometry;
 * changing only the advertised V4L2 size leaves the decoder forced to 1080p
 * and produces a continuous RKCIF size-error flood.  These values follow the
 * PR2100K Rev 0.7 register definitions and were verified on BECAM-2 with
 * DET=0x8a, LOCK=0xf8 and 100 consecutive 1280x720 UYVY frames at 25 fps.
 */
static const struct regval pr2100k_720p25_overrides[] = {
	{0x06, 0x04, 0x10}, /* MIPI off while changing geometry */
	{0x00, 0x2a, 0x19}, /* CH0 VDELAY: preserve all 720 active lines */
	{0x01, 0x12, 0x45}, /* HACTIVE=1280, VACTIVE=720 MSBs */
	{0x01, 0x14, 0x00},
	{0x01, 0x15, 0x19}, /* 720p vertical active start */
	{0x01, 0x16, 0xd0},
	{0x01, 0x1d, 0x05}, /* horizontal scaler active=1280 */
	{0x01, 0x1f, 0x00},
	{0x01, 0xbe, 0x05}, /* VOUT sync HACTIVE=1280 */
	{0x01, 0xbf, 0x00},
	{0x01, 0xc2, 0x02}, /* VOUT 720p25, VACTIVE=720 */
	{0x01, 0xc3, 0xd0},
	{0x01, 0xcb, 0x05}, /* VIN sync HACTIVE=1280 */
	{0x01, 0xcc, 0x00},
	{0x01, 0xcf, 0x02}, /* VIN 720p25, VACTIVE=720 */
	{0x01, 0xd0, 0xd0},
	{0x05, 0x20, 0x8e}, /* enable CH0, HD buffer */
	{0x05, 0x21, 0x05}, /* MTX HSIZE=1280 */
	{0x05, 0x22, 0x00},
	{0x05, 0x23, 0x02}, /* MTX VSIZE=720 */
	{0x05, 0x24, 0xd0},
	{0x06, 0x36, 0x0a}, /* CSI-2 UYVY line payload=2560 bytes */
	{0x06, 0x37, 0x00},
	{0x06, 0x04, 0x50}, /* MIPI on, continuous clock */
};

/* CH1 is a separate decoder on page 2 and emits CSI-2 virtual channel 1. */
static const struct regval pr2100k_ch1_720p25_overrides[] = {
	{0x06, 0x04, 0x10}, /* MIPI off while changing geometry */
	{0x02, 0x12, 0x45}, /* HACTIVE=1280, VACTIVE=720 MSBs */
	{0x02, 0x14, 0x00},
	{0x02, 0x15, 0x19},
	{0x02, 0x16, 0xd0},
	{0x02, 0x1d, 0x05},
	{0x02, 0x1f, 0x00},
	{0x02, 0xbe, 0x05},
	{0x02, 0xbf, 0x00},
	{0x02, 0xc2, 0x02},
	{0x02, 0xc3, 0xd0},
	{0x02, 0xcb, 0x05},
	{0x02, 0xcc, 0x00},
	{0x02, 0xcf, 0x02},
	{0x02, 0xd0, 0xd0},
	{0x05, 0x10, 0x81}, /* MTX path, 2 lanes, pass decoder data unmasked */
	{0x05, 0x20, 0x0e}, /* disable CH0 matrix output */
	{0x05, 0x30, 0x9e}, /* enable CH1, HD buffer */
	{0x05, 0x31, 0x05},
	{0x05, 0x32, 0x00},
	{0x05, 0x33, 0x02},
	{0x05, 0x34, 0xd0},
	{0x06, 0x38, 0x0a}, /* VC1 UYVY line payload=2560 bytes */
	{0x06, 0x39, 0x00},
	{0x06, 0x04, 0x50},
};

static const struct regval pr2100k_ch1_1080p25_output[] = {
	{0x06, 0x04, 0x10},
	{0x05, 0x10, 0x81}, /* MTX path, 2 lanes, pass decoder data unmasked */
	{0x05, 0x20, 0x08}, /* disable CH0 matrix output */
	{0x05, 0x30, 0x98},
	{0x06, 0x38, 0x0f},
	{0x06, 0x39, 0x00},
	{0x06, 0x04, 0x50},
};

/*
 * Dual output keeps both CSI-2 virtual channels enabled.  Unlike the
 * single-channel arrays above, neither array toggles MIPI at its end: MIPI is
 * enabled only after both matrix paths have their matching geometry.
 */
static const struct regval pr2100k_ch0_1080p25_dual_output[] = {
	{0x05, 0x10, 0x81}, /* MTX path, 2 lanes, no VBLK/no-video masking */
	{0x05, 0x20, 0x88}, /* enable CH0, HD buffer */
	{0x06, 0x36, 0x0f},
	{0x06, 0x37, 0x00},
};

static const struct regval pr2100k_ch1_1080p25_dual_output[] = {
	{0x05, 0x10, 0x81}, /* MTX path, 2 lanes, no VBLK/no-video masking */
	{0x05, 0x30, 0x98}, /* enable CH1, HD buffer */
	{0x06, 0x38, 0x0f},
	{0x06, 0x39, 0x00},
};

static const struct regval pr2100k_ch1_720p25_dual_output[] = {
	{0x02, 0x12, 0x45},
	{0x02, 0x14, 0x00},
	{0x02, 0x15, 0x19},
	{0x02, 0x16, 0xd0},
	{0x02, 0x1d, 0x05},
	{0x02, 0x1f, 0x00},
	{0x02, 0xbe, 0x05},
	{0x02, 0xbf, 0x00},
	{0x02, 0xc2, 0x02},
	{0x02, 0xc3, 0xd0},
	{0x02, 0xcb, 0x05},
	{0x02, 0xcc, 0x00},
	{0x02, 0xcf, 0x02},
	{0x02, 0xd0, 0xd0},
	{0x05, 0x10, 0x81},
	{0x05, 0x30, 0x9e}, /* enable CH1, 1280x720 matrix */
	{0x05, 0x31, 0x05},
	{0x05, 0x32, 0x00},
	{0x05, 0x33, 0x02},
	{0x05, 0x34, 0xd0},
	{0x06, 0x38, 0x0a},
	{0x06, 0x39, 0x00},
};

static const struct pr2100k_mode supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_UYVY8_2X8,
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 250000,
		},
		.mipi_freq_idx = 0,
		.bpp = 16,
		.vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
		.vc[PAD1] = V4L2_MBUS_CSI2_CHANNEL_1,
		.reg_list = pr2100k_vendor_regs,
		.reg_list_size = ARRAY_SIZE(pr2100k_vendor_regs),
	},
	{
		.bus_fmt = MEDIA_BUS_FMT_UYVY8_2X8,
		.width = 1280,
		.height = 720,
		.max_fps = {
			.numerator = 10000,
			.denominator = 250000,
		},
		.mipi_freq_idx = 0,
		.bpp = 16,
		.vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
		.vc[PAD1] = V4L2_MBUS_CSI2_CHANNEL_1,
		.reg_list = pr2100k_vendor_regs,
		.reg_list_size = ARRAY_SIZE(pr2100k_vendor_regs),
	}
};

static const s64 link_freq_items[] = {
	PR2100K_LINK_FREQ_1188M,
	PR2100K_LINK_FREQ_594M,
};

static int pr2100k_write_reg(struct i2c_client *client, u8 page, u8 reg, u8 val)
{
	struct i2c_msg msg[2];
	u8 page_buf[2] = {PR2100K_PAGE_REG, page};
	u8 data_buf[2] = {reg, val};
	int ret;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = page_buf;
	msg[0].len = 2;

	msg[1].addr = client->addr;
	msg[1].flags = client->flags;
	msg[1].buf = data_buf;
	msg[1].len = 2;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret == 2)
		return 0;

	if (ret >= 0)
		ret = -EIO;

	dev_err(&client->dev, "PR2100K write page 0x%02x reg 0x%02x failed: %d\n",
		page, reg, ret);
	return ret;
}

static int pr2100k_read_reg_count(struct i2c_client *client, u8 page, u8 reg,
				  u8 *val, int *message_count)
{
	struct i2c_msg msg[3];
	u8 page_buf[2] = {PR2100K_PAGE_REG, page};
	u8 reg_buf[1] = {reg};
	int ret;

	*val = 0;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = page_buf;
	msg[0].len = 2;

	msg[1].addr = client->addr;
	msg[1].flags = client->flags;
	msg[1].buf = reg_buf;
	msg[1].len = 1;

	msg[2].addr = client->addr;
	msg[2].flags = client->flags | I2C_M_RD;
	msg[2].buf = val;
	msg[2].len = 1;

	ret = i2c_transfer(client->adapter, msg, 3);
	if (message_count)
		*message_count = ret;
	if (ret == 3)
		return 0;

	if (ret >= 0)
		ret = -EIO;

	dev_err(&client->dev, "PR2100K read page 0x%02x reg 0x%02x failed: %d\n",
		page, reg, ret);
	return ret;
}

static int pr2100k_read_reg(struct i2c_client *client, u8 page, u8 reg, u8 *val)
{
	return pr2100k_read_reg_count(client, page, reg, val, NULL);
}

static int pr2100k_write_array(struct i2c_client *client,
			       const struct regval *regs, u32 count)
{
	u32 i;
	int ret;

	for (i = 0; i < count; ++i) {
		ret = pr2100k_write_reg(client, regs[i].page, regs[i].addr, regs[i].val);
		if (ret)
			return ret;
		usleep_range(200, 300);
	}
	return 0;
}

static int pr2100k_get_channel_status(struct pr2100k *pr2100k, u8 ch,
				      bool *signal_present, bool *locked,
				      enum pr2100k_format_id *fmt_id, u8 *standard)
{
	u8 det_val = 0, lock_val = 0;
	u8 det_reg = ch ? 0x20 : 0x00;
	int ret;

	/* Page 0 contains the real per-channel detection and lock status. */
	ret = pr2100k_read_reg(pr2100k->client, PR2100K_PAGE_COMMON,
				 det_reg, &det_val);
	if (ret)
		return ret;

	ret = pr2100k_read_reg(pr2100k->client, PR2100K_PAGE_COMMON,
				 det_reg + 1, &lock_val);
	if (ret)
		return ret;

	*signal_present = !!(det_val & PR2100K_DET_VIDEO_BIT);
	*locked = *signal_present &&
		  (lock_val & PR2100K_LOCK_STD_BIT);
	*standard = (det_val >> 6) & 0x03;
	*fmt_id = PR2100K_FMT_UNKNOWN;
	if ((det_val & 0x07) == 0x03)
		*fmt_id = (det_val & 0x10) ? PR2100K_FMT_1080P_30 :
						 PR2100K_FMT_1080P_25;
	else if ((det_val & 0x07) == 0x02)
		*fmt_id = (det_val & 0x10) ? PR2100K_FMT_720P_30 :
						 PR2100K_FMT_720P_25;

	return 0;
}

static void pr2100k_update_channel_state(struct pr2100k_channel *channel,
					 bool signal_present, bool locked,
					 enum pr2100k_format_id fmt_id, u8 standard)
{
	channel->signal_present = signal_present;
	channel->locked = locked;
	channel->fmt_id = fmt_id;
	channel->standard = standard;
	channel->width = 0;
	channel->height = 0;
	channel->fps = 0;

	/* A detector hint without LOCK is not a valid camera input format. */
	if (!locked)
		return;
	if (fmt_id == PR2100K_FMT_1080P_25 || fmt_id == PR2100K_FMT_1080P_30) {
		channel->width = 1920;
		channel->height = 1080;
		channel->fps = fmt_id == PR2100K_FMT_1080P_30 ? 30 : 25;
	} else if (fmt_id == PR2100K_FMT_720P_25 || fmt_id == PR2100K_FMT_720P_30) {
		channel->width = 1280;
		channel->height = 720;
		channel->fps = fmt_id == PR2100K_FMT_720P_30 ? 30 : 25;
	}
}

static u8 pr2100k_manual_format(u8 standard,
				enum pr2100k_format_id format)
{
	u8 value = (standard & 0x03) << 6;

	if (format == PR2100K_FMT_1080P_30 ||
	    format == PR2100K_FMT_720P_30)
		value |= BIT(4);
	if (format == PR2100K_FMT_1080P_25 ||
	    format == PR2100K_FMT_1080P_30)
		value |= 0x03;
	else
		value |= 0x02;

	return value;
}

static bool pr2100k_is_720p(enum pr2100k_format_id format)
{
	return format == PR2100K_FMT_720P_25 ||
	       format == PR2100K_FMT_720P_30;
}

static int pr2100k_apply_dual_output(struct pr2100k *pr2100k)
{
	int ret;

	/* MIPI is held off by the caller while both VC geometries are updated. */
	if (pr2100k_is_720p(pr2100k->channels[0].fmt_id))
		ret = pr2100k_write_array(pr2100k->client,
					  pr2100k_720p25_overrides,
					  ARRAY_SIZE(pr2100k_720p25_overrides) - 1);
	else
		ret = pr2100k_write_array(pr2100k->client,
					  pr2100k_ch0_1080p25_dual_output,
					  ARRAY_SIZE(pr2100k_ch0_1080p25_dual_output));
	if (ret)
		return ret;

	if (pr2100k_is_720p(pr2100k->channels[1].fmt_id))
		ret = pr2100k_write_array(pr2100k->client,
					  pr2100k_ch1_720p25_dual_output,
					  ARRAY_SIZE(pr2100k_ch1_720p25_dual_output));
	else
		ret = pr2100k_write_array(pr2100k->client,
					  pr2100k_ch1_1080p25_dual_output,
					  ARRAY_SIZE(pr2100k_ch1_1080p25_dual_output));
	if (ret)
		return ret;

	return pr2100k_write_reg(pr2100k->client,
				 PR2100K_PAGE_MIPI_DPHY, 0x04, 0x50);
}

static int pr2100k_detect_thread_fn(void *data)
{
	struct pr2100k *pr2100k = data;
	bool locked;
	bool signal_present;
	enum pr2100k_format_id fmt_id;
	u8 standard;
	int ch;

	while (!kthread_should_stop()) {
		mutex_lock(&pr2100k->mutex);
		if (pr2100k->power_on) {
			for (ch = 0; ch < PR2100K_MAX_CHANNELS; ++ch) {
				if (pr2100k_get_channel_status(pr2100k, ch,
							 &signal_present, &locked,
							 &fmt_id, &standard) == 0) {
					if (pr2100k->channels[ch].locked != locked ||
					    pr2100k->channels[ch].signal_present != signal_present ||
					    pr2100k->channels[ch].fmt_id != fmt_id) {
						pr2100k_update_channel_state(&pr2100k->channels[ch],
									 signal_present, locked,
									 fmt_id, standard);
						dev_info(&pr2100k->client->dev,
							 "[PR2100K] ch=%d signal=%s format=%s\n",
							 ch, locked ? "LOCK" : "LOSS",
							 fmt_id == PR2100K_FMT_1080P_25 ? "1080P25" :
							 fmt_id == PR2100K_FMT_1080P_30 ? "1080P30" :
							 fmt_id == PR2100K_FMT_720P_25 ? "720P25" : "720P30");
					}
				}
			}
		}
		mutex_unlock(&pr2100k->mutex);
		msleep(500);
	}
	return 0;
}

static int pr2100k_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct pr2100k *pr2100k = to_pr2100k(sd);
	const struct pr2100k_mode *mode;

	mutex_lock(&pr2100k->mutex);
	mode = pr2100k->cur_mode;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&pr2100k->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
		if (fmt->pad < PAD_MAX && fmt->pad >= PAD0)
			fmt->reserved[0] = mode->vc[fmt->pad];
		else
			fmt->reserved[0] = mode->vc[PAD0];
	}
	mutex_unlock(&pr2100k->mutex);
	return 0;
}

static int pr2100k_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
{
	struct pr2100k *pr2100k = to_pr2100k(sd);
	const struct pr2100k_mode *mode = &supported_modes[0];
	u64 pixel_rate;

	mutex_lock(&pr2100k->mutex);

	if (fmt->format.width <= 1280 && fmt->format.height <= 720)
		mode = &supported_modes[1];

	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&pr2100k->mutex);
		return -ENOTTY;
#endif
	} else {
		pr2100k->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(pr2100k->link_freq, mode->mipi_freq_idx);
		pixel_rate = (u32)link_freq_items[mode->mipi_freq_idx] / mode->bpp * 2 * PR2100K_LANES;
		__v4l2_ctrl_s_ctrl_int64(pr2100k->pixel_rate, pixel_rate);
	}

	mutex_unlock(&pr2100k->mutex);
	return 0;
}

static int pr2100k_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct pr2100k *pr2100k = to_pr2100k(sd);

	if (code->index != 0)
		return -EINVAL;
	code->code = pr2100k->cur_mode->bus_fmt;
	return 0;
}

static int pr2100k_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				   struct v4l2_mbus_config *config)
{
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = V4L2_MBUS_CSI2_2_LANE |
			V4L2_MBUS_CSI2_CHANNEL_0 |
			V4L2_MBUS_CSI2_CHANNEL_1 |
			V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	return 0;
}

static int pr2100k_s_stream(struct v4l2_subdev *sd, int on)
{
	struct pr2100k *pr2100k = to_pr2100k(sd);
	const bool dual = pr2100k->active_channel == PR2100K_ACTIVE_DUAL;
	u8 first_channel = dual ? 0 : pr2100k->active_channel;
	u8 last_channel = dual ? PR2100K_MAX_CHANNELS : first_channel + 1;
	bool ready = false;
	u8 channel;
	int attempt;
	int ret = 0;

	mutex_lock(&pr2100k->mutex);
	if (on && pr2100k->streaming) {
		++pr2100k->stream_users;
		dev_info(&pr2100k->client->dev,
			 "[PR2100K] stream_user=ADD users=%u mode=%s\n",
			 pr2100k->stream_users, dual ? "DUAL" : "SINGLE");
		mutex_unlock(&pr2100k->mutex);
		return 0;
	}
	if (!on && !pr2100k->streaming) {
		mutex_unlock(&pr2100k->mutex);
		return 0;
	}
	if (!on && pr2100k->stream_users > 1) {
		--pr2100k->stream_users;
		dev_info(&pr2100k->client->dev,
			 "[PR2100K] stream_user=REMOVE users=%u mode=%s\n",
			 pr2100k->stream_users, dual ? "DUAL" : "SINGLE");
		mutex_unlock(&pr2100k->mutex);
		return 0;
	}

	if (on) {
		ret = pr2100k_write_array(pr2100k->client,
					  pr2100k_vendor_regs,
					  ARRAY_SIZE(pr2100k_vendor_regs));
		if (ret) {
			dev_err(&pr2100k->client->dev, "[PR2100K] Failed to start stream: %d\n", ret);
			mutex_unlock(&pr2100k->mutex);
			return ret;
		}
		ret = pr2100k_write_array(pr2100k->client,
					  pr2100k_ch1_decoder_regs,
					  ARRAY_SIZE(pr2100k_ch1_decoder_regs));
		if (ret) {
			dev_err(&pr2100k->client->dev,
				"[PR2100K] Failed to initialize CH1 decoder: %d\n", ret);
			mutex_unlock(&pr2100k->mutex);
			return ret;
		}

		/* Keep CSI quiet while the decoder determines the input profile. */
		ret = pr2100k_write_reg(pr2100k->client,
					 PR2100K_PAGE_MIPI_DPHY, 0x04, 0x10);
		if (ret) {
			dev_err(&pr2100k->client->dev,
				"[PR2100K] Failed to pause MIPI for format detection: %d\n",
				ret);
			mutex_unlock(&pr2100k->mutex);
			return ret;
		}

		/*
		 * The vendor baseline initially probes 1080p25.  A real 720p source
		 * still reports DET_IFMT_RES=720p even though LOCK_STD cannot assert
		 * against the forced 1080p profile.  Poll long enough for a 1080p
		 * source to lock; switch immediately when the detector reports 720p.
		 * This runs on every stream start, so changing between 1080p25 and
		 * 720p25 requires no firmware or source-code change.
		 */
		for (attempt = 0; attempt < 20; ++attempt) {
			ready = true;
			msleep(100);
			for (channel = first_channel; channel < last_channel; ++channel) {
				bool signal_present = false;
				bool detected_lock = false;
				enum pr2100k_format_id detected_format = PR2100K_FMT_UNKNOWN;
				u8 detected_standard = 0;

				ret = pr2100k_get_channel_status(pr2100k, channel,
								 &signal_present, &detected_lock,
								 &detected_format,
								 &detected_standard);
				if (ret) {
					dev_err(&pr2100k->client->dev,
						"[PR2100K] Failed to probe CH%u format: %d\n",
						channel, ret);
					mutex_unlock(&pr2100k->mutex);
					return ret;
				}
				pr2100k_update_channel_state(&pr2100k->channels[channel],
							     signal_present, detected_lock,
							     detected_format, detected_standard);
				ready = ready && detected_lock &&
					detected_format != PR2100K_FMT_UNKNOWN;
			}
			if (ready)
				break;
		}

		if (!ready) {
			dev_err(&pr2100k->client->dev,
				"[PR2100K] %s input lock timeout ch0=%ux%u@%u lock=%u ch1=%ux%u@%u lock=%u\n",
				dual ? "dual" : "single",
				pr2100k->channels[0].width, pr2100k->channels[0].height,
				pr2100k->channels[0].fps, pr2100k->channels[0].locked,
				pr2100k->channels[1].width, pr2100k->channels[1].height,
				pr2100k->channels[1].fps, pr2100k->channels[1].locked);
			mutex_unlock(&pr2100k->mutex);
			return -ENOLINK;
		}
		for (channel = first_channel; channel < last_channel; ++channel) {
			u8 man_ifmt = pr2100k_manual_format(pr2100k->channels[channel].standard,
							       pr2100k->channels[channel].fmt_id);

			ret = pr2100k_write_reg(pr2100k->client, PR2100K_PAGE_COMMON,
						channel ? 0x30 : 0x10, man_ifmt);
			if (ret) {
				dev_err(&pr2100k->client->dev,
					"[PR2100K] Failed to select CH%u detected profile: %d\n",
					channel, ret);
				mutex_unlock(&pr2100k->mutex);
				return ret;
			}
			dev_info(&pr2100k->client->dev,
				 "[PR2100K] ch=%u detected_standard=%u man_ifmt=0x%02x\n",
				 channel, pr2100k->channels[channel].standard, man_ifmt);
		}

		if (dual) {
			ret = pr2100k_apply_dual_output(pr2100k);
		} else if (pr2100k_is_720p(pr2100k->channels[first_channel].fmt_id)) {
			pr2100k->cur_mode = &supported_modes[1];
			if (first_channel == 1)
				ret = pr2100k_write_array(pr2100k->client,
						  pr2100k_ch1_720p25_overrides,
						  ARRAY_SIZE(pr2100k_ch1_720p25_overrides));
			else
				ret = pr2100k_write_array(pr2100k->client,
						  pr2100k_720p25_overrides,
						  ARRAY_SIZE(pr2100k_720p25_overrides));
			if (ret) {
				dev_err(&pr2100k->client->dev,
					"[PR2100K] Failed to apply 720p25 overrides: %d\n",
					ret);
				mutex_unlock(&pr2100k->mutex);
				return ret;
			}
		} else if (first_channel == 1) {
			pr2100k->cur_mode = &supported_modes[0];
			ret = pr2100k_write_array(pr2100k->client,
						  pr2100k_ch1_1080p25_output,
						  ARRAY_SIZE(pr2100k_ch1_1080p25_output));
			if (ret) {
				dev_err(&pr2100k->client->dev,
					"[PR2100K] Failed to enable CH1 1080p25 MIPI: %d\n", ret);
				mutex_unlock(&pr2100k->mutex);
				return ret;
			}
		} else {
			pr2100k->cur_mode = &supported_modes[0];
			ret = pr2100k_write_reg(pr2100k->client,
						 PR2100K_PAGE_MIPI_DPHY, 0x04, 0x50);
			if (ret) {
				dev_err(&pr2100k->client->dev,
					"[PR2100K] Failed to enable 1080p25 MIPI: %d\n",
					ret);
				mutex_unlock(&pr2100k->mutex);
				return ret;
			}
		}
		if (ret) {
			dev_err(&pr2100k->client->dev,
				"[PR2100K] Failed to configure %s output: %d\n",
				dual ? "dual" : "single", ret);
			mutex_unlock(&pr2100k->mutex);
			return ret;
		}
		pr2100k->streaming = true;
		pr2100k->stream_users = 1;
		dev_info(&pr2100k->client->dev,
			 "[PR2100K] mode=%s vc0=%ux%u@%u vc1=%ux%u@%u mipi_lanes=2 stream=ON\n",
			 dual ? "DUAL" : "SINGLE", pr2100k->channels[0].width,
			 pr2100k->channels[0].height, pr2100k->channels[0].fps,
			 pr2100k->channels[1].width, pr2100k->channels[1].height,
			 pr2100k->channels[1].fps);
	} else {
		/* Page 6 register 0x04 bit 6 is the real MIPI enable. */
		ret = pr2100k_write_reg(pr2100k->client,
					  PR2100K_PAGE_MIPI_DPHY, 0x04, 0x10);
		if (ret) {
			dev_err(&pr2100k->client->dev,
				"[PR2100K] Failed to stop stream: %d\n", ret);
			mutex_unlock(&pr2100k->mutex);
			return ret;
		}
		pr2100k->streaming = false;
		pr2100k->stream_users = 0;
		dev_info(&pr2100k->client->dev, "[PR2100K] stream=OFF\n");
	}

	mutex_unlock(&pr2100k->mutex);
	return 0;
}

static int pr2100k_get_module_inf(struct pr2100k *pr2100k, struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, pr2100k->module_name ? pr2100k->module_name : "PR2100K-AHD", sizeof(inf->base.sensor));
	strlcpy(inf->base.module, pr2100k->module_name ? pr2100k->module_name : "PR2100K", sizeof(inf->base.module));
	strlcpy(inf->base.lens, pr2100k->len_name ? pr2100k->len_name : "default", sizeof(inf->base.lens));
	return 0;
}

static int pr2100k_get_vc_fmt_inf(struct pr2100k *pr2100k, struct rkmodule_vc_fmt_info *inf)
{
	int i;

	memset(inf, 0, sizeof(*inf));
	for (i = 0; i < PR2100K_MAX_CHANNELS; ++i) {
		inf->width[i] = pr2100k->channels[i].width ? pr2100k->channels[i].width : 1920;
		inf->height[i] = pr2100k->channels[i].height ? pr2100k->channels[i].height : 1080;
		inf->fps[i] = pr2100k->channels[i].fps ? pr2100k->channels[i].fps : 25;
	}
	return 0;
}

static int pr2100k_get_vc_hotplug_inf(struct pr2100k *pr2100k, struct rkmodule_vc_hotplug_info *inf)
{
	u8 status = 0;
	if (pr2100k->channels[0].locked)
		status |= BIT(0);
	if (pr2100k->channels[1].locked)
		status |= BIT(1);
	inf->detect_status = status;
	return 0;
}

static int pr2100k_get_channel_inf(struct pr2100k *pr2100k, struct rkmodule_channel_info *ch_info)
{
	u32 index = ch_info->index;
	const struct pr2100k_channel *channel;
	if (index >= PR2100K_MAX_CHANNELS)
		return -EINVAL;
	channel = &pr2100k->channels[index];

	ch_info->vc = (index == 0) ? V4L2_MBUS_CSI2_CHANNEL_0 : V4L2_MBUS_CSI2_CHANNEL_1;
	/* RKCIF requests this ioctl per stream ID; do not leak the other VC size. */
	ch_info->width = channel->width ? channel->width : pr2100k->cur_mode->width;
	ch_info->height = channel->height ? channel->height : pr2100k->cur_mode->height;
	ch_info->bus_fmt = pr2100k->cur_mode->bus_fmt;
	ch_info->data_type = 0x1E; /* YUV422 8-bit */
	ch_info->data_bit = 8;
	return 0;
}

static int pr2100k_s_stream(struct v4l2_subdev *sd, int on);

static long pr2100k_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct pr2100k *pr2100k = to_pr2100k(sd);
	long ret = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		ret = pr2100k_get_module_inf(pr2100k, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_GET_VC_FMT_INFO:
		ret = pr2100k_get_vc_fmt_inf(pr2100k, (struct rkmodule_vc_fmt_info *)arg);
		break;
	case RKMODULE_GET_VC_HOTPLUG_INFO:
		ret = pr2100k_get_vc_hotplug_inf(pr2100k, (struct rkmodule_vc_hotplug_info *)arg);
		break;
	case RKMODULE_GET_CHANNEL_INFO:
		ret = pr2100k_get_channel_inf(pr2100k, (struct rkmodule_channel_info *)arg);
		break;
	case RKMODULE_GET_START_STREAM_SEQ:
		*(int *)arg = RKMODULE_START_STREAM_FRONT;
		break;
	/*
	 * The RV1106 RKCIF pipeline starts its terminal sensor through this
	 * Rockchip ioctl rather than the video s_stream callback.  Route it through
	 * the same stateful path so dynamic input detection and the dual VC setup
	 * are applied before CIF starts DMA.
	 */
	case RKMODULE_SET_QUICK_STREAM:
		if (!arg)
			ret = -EINVAL;
		else
			ret = pr2100k_s_stream(sd, *(int *)arg);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long pr2100k_compat_ioctl32(struct v4l2_subdev *sd, unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_vc_fmt_info *vc_fmt;
	struct rkmodule_vc_hotplug_info *vc_hp;
	struct rkmodule_channel_info *ch_info;
	long ret;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf)
			return -ENOMEM;
		ret = pr2100k_ioctl(sd, cmd, inf);
		if (!ret && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		return ret;
	case RKMODULE_GET_VC_FMT_INFO:
		vc_fmt = kzalloc(sizeof(*vc_fmt), GFP_KERNEL);
		if (!vc_fmt)
			return -ENOMEM;
		ret = pr2100k_ioctl(sd, cmd, vc_fmt);
		if (!ret && copy_to_user(up, vc_fmt, sizeof(*vc_fmt)))
			ret = -EFAULT;
		kfree(vc_fmt);
		return ret;
	case RKMODULE_GET_VC_HOTPLUG_INFO:
		vc_hp = kzalloc(sizeof(*vc_hp), GFP_KERNEL);
		if (!vc_hp)
			return -ENOMEM;
		ret = pr2100k_ioctl(sd, cmd, vc_hp);
		if (!ret && copy_to_user(up, vc_hp, sizeof(*vc_hp)))
			ret = -EFAULT;
		kfree(vc_hp);
		return ret;
	case RKMODULE_GET_CHANNEL_INFO:
		ch_info = kzalloc(sizeof(*ch_info), GFP_KERNEL);
		if (!ch_info)
			return -ENOMEM;
		if (copy_from_user(ch_info, up, sizeof(*ch_info))) {
			kfree(ch_info);
			return -EFAULT;
		}
		ret = pr2100k_ioctl(sd, cmd, ch_info);
		if (!ret && copy_to_user(up, ch_info, sizeof(*ch_info)))
			ret = -EFAULT;
		kfree(ch_info);
		return ret;
	default:
		return pr2100k_ioctl(sd, cmd, (void *)arg);
	}
}
#endif

static const struct v4l2_subdev_core_ops pr2100k_core_ops = {
	.ioctl = pr2100k_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = pr2100k_compat_ioctl32,
#endif
};

static int pr2100k_g_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct pr2100k *pr2100k = to_pr2100k(sd);

	if (!fi)
		return -EINVAL;

	mutex_lock(&pr2100k->mutex);
	fi->interval = pr2100k->cur_mode->max_fps;
	mutex_unlock(&pr2100k->mutex);

	return 0;
}

static const struct v4l2_subdev_video_ops pr2100k_video_ops = {
	.s_stream = pr2100k_s_stream,
	.g_frame_interval = pr2100k_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops pr2100k_pad_ops = {
	.enum_mbus_code = pr2100k_enum_mbus_code,
	.get_fmt = pr2100k_get_fmt,
	.set_fmt = pr2100k_set_fmt,
	.get_mbus_config = pr2100k_get_mbus_config,
};

static const struct v4l2_subdev_ops pr2100k_subdev_ops = {
	.core = &pr2100k_core_ops,
	.video = &pr2100k_video_ops,
	.pad = &pr2100k_pad_ops,
};

static int pr2100k_power_on(struct pr2100k *pr2100k)
{
	int ret;

	ret = regulator_enable(pr2100k->dovdd);
	if (ret)
		return ret;
	ret = regulator_enable(pr2100k->avdd);
	if (ret)
		goto disable_dovdd;
	ret = regulator_enable(pr2100k->dvdd);
	if (ret)
		goto disable_avdd;

	if (clk_get_rate(pr2100k->xvclk) != PR2100K_XVCLK_FREQ) {
		dev_err(&pr2100k->client->dev,
			"[PR2100K_PWR] xvclk rate=%lu expected=%u\n",
			clk_get_rate(pr2100k->xvclk), PR2100K_XVCLK_FREQ);
		ret = -EINVAL;
		goto disable_dvdd;
	}
	ret = clk_prepare_enable(pr2100k->xvclk);
	if (ret) {
		dev_err(&pr2100k->client->dev,
			"[PR2100K_PWR] xvclk enable failed: %d\n", ret);
		goto disable_dvdd;
	}
	dev_info(&pr2100k->client->dev,
		 "[PR2100K_PWR] power=ON rails=ENABLED external_crystal_declared_hz=%u\n",
		 PR2100K_XVCLK_FREQ);

	gpiod_set_value_cansleep(pr2100k->pwdn_gpio, 0);
	dev_info(&pr2100k->client->dev,
		 "[PR2100K_PWR] pwdn=RELEASED delay_ms=5\n");
	usleep_range(5000, 6000);

	gpiod_set_value_cansleep(pr2100k->reset_gpio, 1);
	dev_info(&pr2100k->client->dev,
		 "[PR2100K_PWR] reset=ASSERTED delay_ms=10\n");
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(pr2100k->reset_gpio, 0);
	dev_info(&pr2100k->client->dev,
		 "[PR2100K_PWR] reset=RELEASED delay_ms=20\n");
	usleep_range(20000, 21000);

	pr2100k->power_on = true;
	return 0;

disable_dvdd:
	regulator_disable(pr2100k->dvdd);
disable_avdd:
	regulator_disable(pr2100k->avdd);
disable_dovdd:
	regulator_disable(pr2100k->dovdd);
	return ret;
}

static void pr2100k_power_off(struct pr2100k *pr2100k)
{
	gpiod_set_value_cansleep(pr2100k->reset_gpio, 1);
	gpiod_set_value_cansleep(pr2100k->pwdn_gpio, 1);
	clk_disable_unprepare(pr2100k->xvclk);
	regulator_disable(pr2100k->dvdd);
	regulator_disable(pr2100k->avdd);
	regulator_disable(pr2100k->dovdd);
	pr2100k->power_on = false;
	dev_info(&pr2100k->client->dev,
		 "[PR2100K_PWR] power=OFF pwdn=ASSERTED reset=ASSERTED\n");
}

static int pr2100k_check_chip_id(struct pr2100k *pr2100k)
{
	u8 page_readback = 0, opt_id = 0, id_h = 0, id_l = 0, rev_id = 0;
	int ret, message_count;

#define PR2100K_PROBE_READ(_reg, _value) do { \
	message_count = 0; \
	ret = pr2100k_read_reg_count(pr2100k->client, PR2100K_PAGE_COMMON, \
				       (_reg), &(_value), &message_count); \
	dev_info(&pr2100k->client->dev, \
		 "[PR2100K_I2C] WRITE page_select reg=0x%02x value=0x%02x combined_transfer_ret=%d expected_messages=3\n", \
		 PR2100K_PAGE_REG, PR2100K_PAGE_COMMON, message_count); \
	dev_info(&pr2100k->client->dev, \
		 "[PR2100K_I2C] READ page=0x%02x reg=0x%02x value=0x%02x ret=%d\n", \
		 PR2100K_PAGE_COMMON, (_reg), (_value), ret); \
} while (0)

	PR2100K_PROBE_READ(PR2100K_PAGE_REG, page_readback);
	if (ret)
		goto read_failed;
	if ((page_readback & 0x07) != PR2100K_PAGE_COMMON) {
		dev_err(&pr2100k->client->dev,
			"[PR2100K] probe=FAIL reason=PAGE_SELECT readback=0x%02x expected_page=0x00\n",
			page_readback);
		ret = -EIO;
		goto out;
	}

	PR2100K_PROBE_READ(PR2100K_OPT_ID_REG, opt_id);
	if (ret)
		goto read_failed;

	PR2100K_PROBE_READ(PR2100K_CHIP_ID_H_REG, id_h);
	if (ret)
		goto read_failed;

	PR2100K_PROBE_READ(PR2100K_CHIP_ID_L_REG, id_l);
	if (ret)
		goto read_failed;

	PR2100K_PROBE_READ(PR2100K_REV_ID_REG, rev_id);
	if (ret)
		goto read_failed;

	if (id_h != PR2100K_CHIP_ID_H_VAL || id_l != PR2100K_CHIP_ID_L_VAL) {
		dev_err(&pr2100k->client->dev,
			"[PR2100K] probe=FAIL chip_id=0x%02x%02x expected=0x2100\n",
			id_h, id_l);
		ret = -ENODEV;
		goto out;
	}
	pr2100k->chip_id = ((u16)id_h << 8) | id_l;

	dev_info(&pr2100k->client->dev,
		 "[PR2100K] probe=PASS chip_id=0x%02x%02x opt_id=0x%02x rev_id=0x%02x i2c_bus=%d addr=0x%02x\n",
		 id_h, id_l, opt_id, rev_id, pr2100k->client->adapter->nr,
		 pr2100k->client->addr);
	ret = 0;
	goto out;

read_failed:
	dev_err(&pr2100k->client->dev,
		"[PR2100K] probe=FAIL reason=%s ret=%d\n",
		ret == -ENXIO ? "I2C_NO_ACK" : "I2C_READ", ret);
out:
#undef PR2100K_PROBE_READ
	return ret;
}

static ssize_t chip_id_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct pr2100k *pr2100k = to_pr2100k(sd);

	return scnprintf(buf, PAGE_SIZE, "0x%04x\n", pr2100k->chip_id);
}
static DEVICE_ATTR_RO(chip_id);

static const char *pr2100k_standard_name(u8 standard)
{
	static const char * const names[] = { "HDPVI", "HDCVI", "HDA", "HDT" };

	return standard < ARRAY_SIZE(names) ? names[standard] : "UNKNOWN";
}

static ssize_t pr2100k_channel_state_show(struct device *dev, u8 channel,
					   char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct pr2100k *pr2100k = to_pr2100k(sd);
	struct pr2100k_channel state;

	if (channel >= PR2100K_MAX_CHANNELS)
		return -EINVAL;
	mutex_lock(&pr2100k->mutex);
	state = pr2100k->channels[channel];
	mutex_unlock(&pr2100k->mutex);

	return scnprintf(buf, PAGE_SIZE,
		"signal_present=%u locked=%u width=%u height=%u fps=%u standard=%s vc_id=%u pixel_format=UYVY\n",
		state.signal_present, state.locked, state.width, state.height,
		state.fps, pr2100k_standard_name(state.standard), channel);
}

static ssize_t channel0_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return pr2100k_channel_state_show(dev, 0, buf);
}
static DEVICE_ATTR_RO(channel0_state);

static ssize_t channel1_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return pr2100k_channel_state_show(dev, 1, buf);
}
static DEVICE_ATTR_RO(channel1_state);

static ssize_t active_channel_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pr2100k *pr2100k = to_pr2100k(i2c_get_clientdata(client));
	u8 channel;

	mutex_lock(&pr2100k->mutex);
	channel = pr2100k->active_channel;
	mutex_unlock(&pr2100k->mutex);
	return scnprintf(buf, PAGE_SIZE, "%u\n", channel);
}

static ssize_t active_channel_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pr2100k *pr2100k = to_pr2100k(i2c_get_clientdata(client));
	u8 channel;
	int ret;

	ret = kstrtou8(buf, 0, &channel);
	if (ret || channel > PR2100K_ACTIVE_DUAL)
		return -EINVAL;
	mutex_lock(&pr2100k->mutex);
	if (pr2100k->streaming)
		ret = -EBUSY;
	else {
		pr2100k->active_channel = channel;
		/* Advertise the selected input before CIF negotiates its format. */
		if (channel == PR2100K_ACTIVE_DUAL)
			pr2100k->cur_mode = &supported_modes[0];
		else if (pr2100k->channels[channel].locked &&
		    pr2100k->channels[channel].width == 1280 &&
		    pr2100k->channels[channel].height == 720)
			pr2100k->cur_mode = &supported_modes[1];
		else if (pr2100k->channels[channel].locked &&
			 pr2100k->channels[channel].width == 1920 &&
			 pr2100k->channels[channel].height == 1080)
			pr2100k->cur_mode = &supported_modes[0];
	}
	mutex_unlock(&pr2100k->mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(active_channel);

static ssize_t raw_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	static const struct regval regs[] = {
		{0x00, 0x00, 0}, {0x00, 0x01, 0},
		{0x00, 0x10, 0}, {0x00, 0x11, 0}, {0x00, 0x12, 0},
		{0x00, 0x13, 0}, {0x00, 0x14, 0}, {0x00, 0x15, 0},
		{0x00, 0x16, 0}, {0x00, 0x20, 0}, {0x00, 0x21, 0},
		{0x00, 0x30, 0}, {0x00, 0x31, 0}, {0x00, 0x32, 0},
		{0x00, 0x33, 0}, {0x00, 0x34, 0}, {0x00, 0x35, 0},
		{0x00, 0x36, 0},
		{0x01, 0x00, 0}, {0x01, 0x4f, 0}, {0x01, 0x50, 0},
		{0x01, 0xc2, 0}, {0x01, 0xc3, 0},
		{0x02, 0x00, 0}, {0x02, 0x4f, 0}, {0x02, 0x50, 0},
		{0x02, 0xc2, 0}, {0x02, 0xc3, 0},
		{0x05, 0x10, 0}, {0x05, 0x20, 0}, {0x05, 0x30, 0},
		{0x06, 0x04, 0}, {0x06, 0x08, 0},
		{0x06, 0x36, 0}, {0x06, 0x37, 0},
		{0x06, 0x38, 0}, {0x06, 0x39, 0},
		{0x06, 0x46, 0}, {0x06, 0x47, 0},
	};
	struct i2c_client *client = to_i2c_client(dev);
	struct pr2100k *pr2100k = to_pr2100k(i2c_get_clientdata(client));
	ssize_t length = 0;
	size_t i;
	int ret;
	u8 value;

	mutex_lock(&pr2100k->mutex);
	for (i = 0; i < ARRAY_SIZE(regs); ++i) {
		ret = pr2100k_read_reg(client, regs[i].page, regs[i].addr, &value);
		if (ret) {
			length = ret;
			break;
		}
		length += scnprintf(buf + length, PAGE_SIZE - length,
				    "p%u_r%02x=0x%02x%c", regs[i].page,
				    regs[i].addr, value,
				    i + 1 == ARRAY_SIZE(regs) ? '\n' : ' ');
	}
	mutex_unlock(&pr2100k->mutex);
	return length;
}
static DEVICE_ATTR_RO(raw_status);

static int pr2100k_prime_input_detection(struct pr2100k *pr2100k)
{
	bool signal_present, locked;
	enum pr2100k_format_id fmt_id;
	u8 standard;
	bool ch1_720_configured = false;
	int attempt, ch, ret;

	ret = pr2100k_write_array(pr2100k->client, pr2100k_vendor_regs,
				  ARRAY_SIZE(pr2100k_vendor_regs));
	if (ret)
		return ret;
	ret = pr2100k_write_array(pr2100k->client, pr2100k_ch1_decoder_regs,
				  ARRAY_SIZE(pr2100k_ch1_decoder_regs));
	if (ret)
		return ret;
	ret = pr2100k_write_reg(pr2100k->client,
				 PR2100K_PAGE_MIPI_DPHY, 0x04, 0x10);
	if (ret)
		return ret;

	for (attempt = 0; attempt < 20; ++attempt) {
		msleep(100);
		for (ch = 0; ch < PR2100K_MAX_CHANNELS; ++ch) {
			ret = pr2100k_get_channel_status(pr2100k, ch,
							 &signal_present, &locked,
							 &fmt_id, &standard);
			if (ret)
				return ret;
			if (ch == 1 && !ch1_720_configured &&
			    (fmt_id == PR2100K_FMT_720P_25 ||
			     fmt_id == PR2100K_FMT_720P_30)) {
				ret = pr2100k_write_array(pr2100k->client,
						  pr2100k_ch1_720p25_overrides,
						  ARRAY_SIZE(pr2100k_ch1_720p25_overrides));
				if (ret)
					return ret;
				ret = pr2100k_write_reg(pr2100k->client,
						 PR2100K_PAGE_MIPI_DPHY, 0x04, 0x10);
				if (ret)
					return ret;
				ch1_720_configured = true;
			}
			pr2100k_update_channel_state(&pr2100k->channels[ch],
						     signal_present, locked,
						     fmt_id, standard);
		}
		if (pr2100k->channels[0].fmt_id != PR2100K_FMT_UNKNOWN &&
		    pr2100k->channels[1].fmt_id != PR2100K_FMT_UNKNOWN)
			break;
	}

	dev_info(&pr2100k->client->dev,
		 "[PR2100K] input_prime ch0=%ux%u@%u signal=%u lock=%u ch1=%ux%u@%u signal=%u lock=%u\n",
		 pr2100k->channels[0].width, pr2100k->channels[0].height,
		 pr2100k->channels[0].fps, pr2100k->channels[0].signal_present,
		 pr2100k->channels[0].locked, pr2100k->channels[1].width,
		 pr2100k->channels[1].height, pr2100k->channels[1].fps,
		 pr2100k->channels[1].signal_present,
		 pr2100k->channels[1].locked);
	return 0;
}

static int pr2100k_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct pr2100k *pr2100k;
	int ret;

	dev_info(dev, "[PR2100K_BOOT] probe=ENTER i2c_bus=%d addr=0x%02x\n",
		 client->adapter->nr, client->addr);

	pr2100k = devm_kzalloc(dev, sizeof(*pr2100k), GFP_KERNEL);
	if (!pr2100k)
		return -ENOMEM;

	pr2100k->client = client;
	mutex_init(&pr2100k->mutex);
	pr2100k->cur_mode = &supported_modes[0];

	pr2100k->channels[0].width = 1920;
	pr2100k->channels[0].height = 1080;
	pr2100k->channels[0].fps = 25;
	pr2100k->channels[1].width = 1920;
	pr2100k->channels[1].height = 1080;
	pr2100k->channels[1].fps = 25;

	of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX, &pr2100k->module_index);
	of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING, &pr2100k->module_facing);
	of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME, &pr2100k->module_name);
	of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME, &pr2100k->len_name);

	pr2100k->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(pr2100k->xvclk)) {
		ret = PTR_ERR(pr2100k->xvclk);
		return dev_err_probe(dev, ret,
			"[PR2100K_BOOT] resource=xvclk state=NOT_READY\n");
	}

	pr2100k->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pr2100k->reset_gpio)) {
		ret = PTR_ERR(pr2100k->reset_gpio);
		return dev_err_probe(dev, ret,
			"[PR2100K_BOOT] resource=reset_gpio state=NOT_READY\n");
	}
	pr2100k->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(pr2100k->pwdn_gpio)) {
		ret = PTR_ERR(pr2100k->pwdn_gpio);
		return dev_err_probe(dev, ret,
			"[PR2100K_BOOT] resource=pwdn_gpio state=NOT_READY\n");
	}

	pr2100k->dovdd = devm_regulator_get(dev, "dovdd");
	if (IS_ERR(pr2100k->dovdd)) {
		ret = PTR_ERR(pr2100k->dovdd);
		return dev_err_probe(dev, ret,
			"[PR2100K_BOOT] resource=dovdd state=NOT_READY\n");
	}
	pr2100k->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(pr2100k->avdd)) {
		ret = PTR_ERR(pr2100k->avdd);
		return dev_err_probe(dev, ret,
			"[PR2100K_BOOT] resource=avdd state=NOT_READY\n");
	}
	pr2100k->dvdd = devm_regulator_get(dev, "dvdd");
	if (IS_ERR(pr2100k->dvdd)) {
		ret = PTR_ERR(pr2100k->dvdd);
		return dev_err_probe(dev, ret,
			"[PR2100K_BOOT] resource=dvdd state=NOT_READY\n");
	}

	ret = pr2100k_power_on(pr2100k);
	if (ret)
		return ret;

	ret = pr2100k_check_chip_id(pr2100k);
	if (ret) {
		dev_err(dev, "[PR2100K] Check chip ID failed or hardware absent (ret=%d)\n", ret);
		goto err_power_off;
	}
	ret = pr2100k_prime_input_detection(pr2100k);
	if (ret) {
		dev_err(dev, "[PR2100K] input prime failed: %d\n", ret);
		goto err_power_off;
	}

	v4l2_i2c_subdev_init(&pr2100k->subdev, client, &pr2100k_subdev_ops);
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	pr2100k->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif

	v4l2_ctrl_handler_init(&pr2100k->ctrl_handler, 2);
	pr2100k->link_freq = v4l2_ctrl_new_int_menu(&pr2100k->ctrl_handler, NULL,
						    V4L2_CID_LINK_FREQ,
						    ARRAY_SIZE(link_freq_items) - 1, 0,
						    link_freq_items);
	pr2100k->pixel_rate = v4l2_ctrl_new_std(&pr2100k->ctrl_handler, NULL,
						V4L2_CID_PIXEL_RATE,
						0, 800000000, 1, 148500000);
	pr2100k->subdev.ctrl_handler = &pr2100k->ctrl_handler;

	pr2100k->pad.flags = MEDIA_PAD_FL_SOURCE;
	pr2100k->subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&pr2100k->subdev.entity, 1, &pr2100k->pad);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize media pads: %d\n", ret);
		goto err_power_off;
	}

	ret = v4l2_async_register_subdev_sensor_common(&pr2100k->subdev);
	if (ret) {
		dev_err(dev, "Failed to register async subdev: %d\n", ret);
		goto err_clean_entity;
	}

	ret = device_create_file(dev, &dev_attr_chip_id);
	if (ret) {
		dev_err(dev, "Failed to create chip_id attribute: %d\n", ret);
		goto err_unregister_subdev;
	}
	ret = device_create_file(dev, &dev_attr_channel0_state);
	if (ret)
		goto err_remove_chip_id;
	ret = device_create_file(dev, &dev_attr_channel1_state);
	if (ret)
		goto err_remove_channel0_state;
	ret = device_create_file(dev, &dev_attr_active_channel);
	if (ret)
		goto err_remove_channel1_state;
	ret = device_create_file(dev, &dev_attr_raw_status);
	if (ret)
		goto err_remove_active_channel;

	pr2100k->detect_thread = kthread_run(pr2100k_detect_thread_fn, pr2100k, "pr2100k_detect");
	if (IS_ERR(pr2100k->detect_thread))
		pr2100k->detect_thread = NULL;

	dev_info(dev, "[PR2100K] Driver probed successfully on I2C%d address 0x%02x\n",
		 client->adapter->nr, client->addr);
	return 0;

err_remove_active_channel:
	device_remove_file(dev, &dev_attr_active_channel);
err_remove_channel1_state:
	device_remove_file(dev, &dev_attr_channel1_state);
err_remove_channel0_state:
	device_remove_file(dev, &dev_attr_channel0_state);
err_remove_chip_id:
	device_remove_file(dev, &dev_attr_chip_id);

err_unregister_subdev:
	v4l2_async_unregister_subdev(&pr2100k->subdev);
err_clean_entity:
	media_entity_cleanup(&pr2100k->subdev.entity);
err_power_off:
	pr2100k_power_off(pr2100k);
	return ret;
}

static int pr2100k_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct pr2100k *pr2100k = to_pr2100k(sd);

	if (pr2100k->detect_thread)
		kthread_stop(pr2100k->detect_thread);

	device_remove_file(&client->dev, &dev_attr_chip_id);
	device_remove_file(&client->dev, &dev_attr_channel0_state);
	device_remove_file(&client->dev, &dev_attr_channel1_state);
	device_remove_file(&client->dev, &dev_attr_active_channel);
	device_remove_file(&client->dev, &dev_attr_raw_status);
	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&pr2100k->ctrl_handler);
	pr2100k_power_off(pr2100k);
	mutex_destroy(&pr2100k->mutex);
	return 0;
}

static const struct of_device_id pr2100k_of_match[] = {
	{ .compatible = "pixelplus,pr2100k" },
	{ .compatible = "pixelplus,pr2000k" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pr2100k_of_match);

static const struct i2c_device_id pr2100k_match_id[] = {
	{ "pr2100k", 0 },
	{ "pr2000k", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, pr2100k_match_id);

static struct i2c_driver pr2100k_i2c_driver = {
	.driver = {
		.name = PR2100K_NAME,
		.of_match_table = pr2100k_of_match,
	},
	.probe = pr2100k_probe,
	.remove = pr2100k_remove,
	.id_table = pr2100k_match_id,
};

static int __init pr2100k_driver_init(void)
{
	pr_info("[PR2100K_BOOT] driver=REGISTER initcall=device_initcall_sync\n");
	return i2c_add_driver(&pr2100k_i2c_driver);
}

static void __exit pr2100k_driver_exit(void)
{
	i2c_del_driver(&pr2100k_i2c_driver);
}

/*
 * reset-gpios and pwdn-gpios are supplied by the PCA953x expander on I2C3.
 * Register after normal device initcalls so the GPIO provider is available
 * before the media graph starts binding this built-in camera driver.
 */
device_initcall_sync(pr2100k_driver_init);
module_exit(pr2100k_driver_exit);

MODULE_DESCRIPTION("Pixelplus PR2100K Dual AHD to MIPI CSI-2 Decoder Driver");
MODULE_AUTHOR("Rockchip & Dashcam Engineering Team");
MODULE_LICENSE("GPL v2");
