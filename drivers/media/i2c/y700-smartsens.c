// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal Lenovo Y700 SmartSens sensor bring-up driver.
 */

#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/minmax.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define Y700_SS_CHIP_ID_REG	CCI_REG16(0x3107)
#define Y700_SS_STREAM_REG	CCI_REG8(0x0100)
#define Y700_SS_DEFAULT_CLK_FREQ 19200000
#define Y700_SS_MBUS_CODE	MEDIA_BUS_FMT_SBGGR10_1X10
#define Y700_SS_RGB_DEPTH	10
#define Y700_SS_MAX_SUPPLIES	4
#define Y700_SS_SLEEP_US	(2 * USEC_PER_MSEC)
#define Y700_SS_EXPOSURE_MIN	1
#define Y700_SS_EXPOSURE_MARGIN	6
#define Y700_SS_ANALOGUE_GAIN_MIN	1024
#define Y700_SS_ANALOGUE_GAIN_MAX	16384
#define Y700_SS_ANALOGUE_GAIN_DEFAULT	1024
#define SC820CS_LDO3_SUPPLY_INDEX	2
#define SC820CS_DEFAULT_LDO3_UV		1800000
#define SC820CS_LDO3_UV		2784000

enum y700_ss_model {
	Y700_SS_SC202CS,
	Y700_SS_SC820CS,
};

struct y700_ss_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

#include "sc202cs-y700-regs.h"
#include "sc820cs-y700-regs.h"

static const struct cci_reg_sequence y700_ss_stream_on_regs[] = {
	{ Y700_SS_STREAM_REG, 0x01 },
};

static const struct cci_reg_sequence y700_ss_stream_off_regs[] = {
	{ Y700_SS_STREAM_REG, 0x00 },
};

static const char * const sc820cs_test_pattern_menu[] = {
	"Disabled",
	"Black",
};

static const struct cci_reg_sequence sc820cs_test_pattern_disabled_regs[] = {
	{ CCI_REG8(0x4501), 0xc4 },
};

static const struct cci_reg_sequence sc820cs_test_pattern_black_regs[] = {
	{ CCI_REG8(0x3337), 0x30 },
	{ CCI_REG8(0x391f), 0xc8 },
	{ CCI_REG8(0x5000), 0x08 },
	{ CCI_REG8(0x3e08), 0x00 },
	{ CCI_REG8(0x3e07), 0x80 },
	{ CCI_REG8(0x4501), 0x00 },
	{ CCI_REG8(0x4502), 0x00 },
};

struct y700_ss_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts_def;
	u32 exposure_def;
	const struct y700_ss_reg_list reg_list;
	const struct y700_ss_reg_list stream_on_list;
	const struct y700_ss_reg_list stream_off_list;
};

struct y700_ss_reset_step {
	unsigned int value;
	unsigned int delay_us;
};

struct y700_ss_gain_step {
	u32 gain;
	u8 coarse;
};

struct y700_ss_info {
	const char *name;
	enum y700_ss_model model;
	u32 chip_id;
	const char * const *supply_names;
	const unsigned int *supply_delays_us;
	unsigned int num_supplies;
	const struct y700_ss_reset_step *reset_steps;
	unsigned int num_reset_steps;
	const struct y700_ss_mode *mode;
	const struct y700_ss_mode *modes;
	unsigned int num_modes;
	unsigned int data_lanes;
	const s64 *link_freq_menu_items;
	unsigned int num_link_freqs;
};

struct y700_ss {
	struct device *dev;
	const struct y700_ss_info *info;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *xclk;
	struct regulator_bulk_data supplies[Y700_SS_MAX_SUPPLIES];
	struct gpio_desc *reset_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *test_pattern;

	struct regmap *regmap;
	unsigned long link_freq_bitmap;
	const struct y700_ss_mode *cur_mode;
};

static const s64 y700_ss_link_freq_menu_items[] = {
	560 * HZ_PER_MHZ,
};

static const s64 sc820cs_y700_link_freq_menu_items[] = {
	364800000,
	560 * HZ_PER_MHZ,
};

static const char * const sc202cs_supply_names[] = {
	"ldo7",
	"ldo6",
};

static const unsigned int sc202cs_supply_delays_us[] = {
	USEC_PER_MSEC,
	USEC_PER_MSEC,
};

static const struct y700_ss_reset_step sc202cs_reset_steps[] = {
	{ 0, 2 * USEC_PER_MSEC },
	{ 1, 2 * USEC_PER_MSEC },
	{ 0, 5 * USEC_PER_MSEC },
};

static const char * const sc820cs_supply_names[] = {
	"ldo7",
	"ldo2",
	"ldo3",
};

static const unsigned int sc820cs_supply_delays_us[] = {
	USEC_PER_MSEC,
	USEC_PER_MSEC,
	2 * USEC_PER_MSEC,
};

static const struct y700_ss_reset_step sc820cs_reset_steps[] = {
	{ 0, 5 * USEC_PER_MSEC },
};

static const struct y700_ss_mode sc202cs_modes[] = {
{
	.width = 1600,
	.height = 1200,
	.hts = 1912,
	.vts_def = 1242,
	.exposure_def = 0x4d4,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(sc202cs_y700_mode_1600x1200_regs),
		.regs = sc202cs_y700_mode_1600x1200_regs,
	},
	.stream_on_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_on_regs),
		.regs = y700_ss_stream_on_regs,
	},
	.stream_off_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_off_regs),
		.regs = y700_ss_stream_off_regs,
	},
},
};

static const struct y700_ss_mode sc820cs_modes[] = {
{
	.width = 3264,
	.height = 2448,
	.hts = 3888,
	.vts_def = 2500,
	.exposure_def = 750,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(sc820cs_y700_mode_3264x2448_regs),
		.regs = sc820cs_y700_mode_3264x2448_regs,
	},
	.stream_on_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_on_regs),
		.regs = y700_ss_stream_on_regs,
	},
	.stream_off_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_off_regs),
		.regs = y700_ss_stream_off_regs,
	},
},
{
	.width = 3264,
	.height = 1836,
	.hts = 3888,
	.vts_def = 2500,
	.exposure_def = 750,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(sc820cs_y700_mode_3264x1836_regs),
		.regs = sc820cs_y700_mode_3264x1836_regs,
	},
	.stream_on_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_on_regs),
		.regs = y700_ss_stream_on_regs,
	},
	.stream_off_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_off_regs),
		.regs = y700_ss_stream_off_regs,
	},
},
{
	.width = 1632,
	.height = 1224,
	.hts = 3888,
	.vts_def = 2500,
	.exposure_def = 750,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(sc820cs_y700_mode_1632x1224_regs),
		.regs = sc820cs_y700_mode_1632x1224_regs,
	},
	.stream_on_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_on_regs),
		.regs = y700_ss_stream_on_regs,
	},
	.stream_off_list = {
		.num_of_regs = ARRAY_SIZE(y700_ss_stream_off_regs),
		.regs = y700_ss_stream_off_regs,
	},
},
};

static const struct y700_ss_info sc202cs_info = {
	.name = "sc202cs",
	.model = Y700_SS_SC202CS,
	.chip_id = 0xeb52,
	.supply_names = sc202cs_supply_names,
	.supply_delays_us = sc202cs_supply_delays_us,
	.num_supplies = ARRAY_SIZE(sc202cs_supply_names),
	.reset_steps = sc202cs_reset_steps,
	.num_reset_steps = ARRAY_SIZE(sc202cs_reset_steps),
	.mode = &sc202cs_modes[0],
	.modes = sc202cs_modes,
	.num_modes = ARRAY_SIZE(sc202cs_modes),
	.data_lanes = 1,
	.link_freq_menu_items = y700_ss_link_freq_menu_items,
	.num_link_freqs = ARRAY_SIZE(y700_ss_link_freq_menu_items),
};

static const struct y700_ss_info sc820cs_info = {
	.name = "sc820cs",
	.model = Y700_SS_SC820CS,
	.chip_id = 0xd154,
	.supply_names = sc820cs_supply_names,
	.supply_delays_us = sc820cs_supply_delays_us,
	.num_supplies = ARRAY_SIZE(sc820cs_supply_names),
	.reset_steps = sc820cs_reset_steps,
	.num_reset_steps = ARRAY_SIZE(sc820cs_reset_steps),
	.mode = &sc820cs_modes[0],
	.modes = sc820cs_modes,
	.num_modes = ARRAY_SIZE(sc820cs_modes),
	.data_lanes = 4,
	.link_freq_menu_items = sc820cs_y700_link_freq_menu_items,
	.num_link_freqs = ARRAY_SIZE(sc820cs_y700_link_freq_menu_items),
};

static inline struct y700_ss *to_y700_ss(struct v4l2_subdev *sd)
{
	return container_of(sd, struct y700_ss, sd);
}

static const struct y700_ss_mode *y700_ss_cur_mode(struct y700_ss *sensor)
{
	return sensor->cur_mode ?: sensor->info->mode;
}

static u64 y700_ss_to_pixel_rate(const struct y700_ss_info *info)
{
	u64 pixel_rate = info->link_freq_menu_items[0] * 2 * info->data_lanes;

	return div_u64(pixel_rate, Y700_SS_RGB_DEPTH);
}

static unsigned int y700_ss_sc820cs_ldo3_target_uv(struct y700_ss *sensor)
{
	if (sensor->info->model != Y700_SS_SC820CS)
		return 0;

	return SC820CS_LDO3_UV;
}

static int y700_ss_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct y700_ss *sensor = to_y700_ss(sd);
	const struct y700_ss_info *info = sensor->info;
	unsigned int ldo3_uv = y700_ss_sc820cs_ldo3_target_uv(sensor);
	unsigned int num_enabled = 0;
	int ret, i;

	for (i = 0; i < info->num_supplies; i++) {
		if (ldo3_uv && i == SC820CS_LDO3_SUPPLY_INDEX) {
			ret = regulator_set_voltage(sensor->supplies[i].consumer,
						    ldo3_uv, ldo3_uv);
			if (ret)
				goto disable_supplies;
		}

		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret)
			goto disable_supplies;
		num_enabled++;

		fsleep(info->supply_delays_us[i]);
	}

	for (i = 0; i < info->num_reset_steps; i++) {
		gpiod_set_value_cansleep(sensor->reset_gpio,
					 info->reset_steps[i].value);
		fsleep(info->reset_steps[i].delay_us);
	}

	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto assert_reset;

	fsleep(5 * USEC_PER_MSEC);

	return 0;

assert_reset:
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
disable_supplies:
	while (num_enabled)
		regulator_disable(sensor->supplies[--num_enabled].consumer);

	return ret;
}

static int y700_ss_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct y700_ss *sensor = to_y700_ss(sd);
	unsigned int ldo3_uv = y700_ss_sc820cs_ldo3_target_uv(sensor);
	int ret;
	int i;

	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	fsleep(Y700_SS_SLEEP_US);

	for (i = sensor->info->num_supplies - 1; i >= 0; i--)
		regulator_disable(sensor->supplies[i].consumer);

	if (ldo3_uv) {
		ret = regulator_set_voltage(
			sensor->supplies[SC820CS_LDO3_SUPPLY_INDEX].consumer,
			SC820CS_DEFAULT_LDO3_UV, SC820CS_DEFAULT_LDO3_UV);
		if (ret)
			dev_warn(dev, "failed to restore SC820CS ldo3 voltage: %d\n",
				 ret);
	}

	return 0;
}

static int y700_ss_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = Y700_SS_MBUS_CODE;
	return 0;
}

static int y700_ss_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct y700_ss *sensor = to_y700_ss(sd);
	const struct y700_ss_mode *mode;

	if (fse->code != Y700_SS_MBUS_CODE ||
	    fse->index >= sensor->info->num_modes)
		return -EINVAL;

	mode = &sensor->info->modes[fse->index];

	fse->min_width = mode->width;
	fse->max_width = mode->width;
	fse->min_height = mode->height;
	fse->max_height = mode->height;

	return 0;
}

static int y700_ss_update_ctrl_ranges(struct y700_ss *sensor,
				      const struct y700_ss_mode *mode)
{
	s64 hblank = mode->hts - mode->width;
	s64 vblank = mode->vts_def - mode->height;
	int exposure_max = mode->vts_def - Y700_SS_EXPOSURE_MARGIN;
	int ret;

	ret = __v4l2_ctrl_modify_range(sensor->hblank, hblank, hblank, 1,
				       hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(sensor->vblank, vblank, vblank, 1,
				       vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(sensor->exposure,
				       Y700_SS_EXPOSURE_MIN, exposure_max,
				       1, min_t(int, mode->exposure_def,
						exposure_max));
	if (ret)
		return ret;

	return __v4l2_ctrl_s_ctrl(sensor->exposure,
				  min_t(int, sensor->exposure->val,
					exposure_max));
}

static void y700_ss_update_pad_format(const struct y700_ss_mode *mode,
				      struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = Y700_SS_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int y700_ss_set_format(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct y700_ss *sensor = to_y700_ss(sd);
	struct v4l2_mbus_framefmt *mbus_fmt;
	const struct y700_ss_mode *mode;
	struct v4l2_rect *crop;

	mode = v4l2_find_nearest_size(sensor->info->modes,
				      sensor->info->num_modes,
				      width, height,
				      fmt->format.width, fmt->format.height);

	y700_ss_update_pad_format(mode, &fmt->format);

	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->width = fmt->format.width;
	crop->height = fmt->format.height;

	mbus_fmt = v4l2_subdev_state_get_format(state, 0);
	*mbus_fmt = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		sensor->cur_mode = mode;
		return y700_ss_update_ctrl_ranges(sensor, mode);
	}

	return 0;
}

static int y700_ss_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	struct y700_ss *sensor = to_y700_ss(sd);
	const struct y700_ss_mode *mode = y700_ss_cur_mode(sensor);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = mode->width;
		sel->r.height = mode->height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int y700_ss_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
	};

	y700_ss_update_pad_format(y700_ss_cur_mode(to_y700_ss(sd)),
				  &fmt.format);
	return y700_ss_set_format(sd, state, &fmt);
}

static int y700_ss_start_streaming(struct y700_ss *sensor)
{
	const struct y700_ss_mode *mode = y700_ss_cur_mode(sensor);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(sensor->regmap, mode->reg_list.regs,
				  mode->reg_list.num_of_regs, NULL);
	if (ret)
		goto err_rpm_put;

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret)
		goto err_rpm_put;

	ret = cci_multi_reg_write(sensor->regmap, mode->stream_on_list.regs,
				  mode->stream_on_list.num_of_regs, NULL);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put(sensor->dev);
	return ret;
}

static int y700_ss_stop_streaming(struct y700_ss *sensor)
{
	const struct y700_ss_mode *mode = y700_ss_cur_mode(sensor);
	int ret;

	ret = cci_multi_reg_write(sensor->regmap, mode->stream_off_list.regs,
				  mode->stream_off_list.num_of_regs, NULL);
	if (ret)
		dev_err(sensor->dev, "failed to stop stream: %d\n", ret);

	pm_runtime_put(sensor->dev);
	return ret;
}

static int y700_ss_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct y700_ss *sensor = to_y700_ss(sd);
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	ret = enable ? y700_ss_start_streaming(sensor) :
		       y700_ss_stop_streaming(sensor);
	v4l2_subdev_unlock_state(state);

	return ret;
}

static const struct v4l2_subdev_video_ops y700_ss_video_ops = {
	.s_stream = y700_ss_s_stream,
};

static const struct v4l2_subdev_pad_ops y700_ss_pad_ops = {
	.enum_mbus_code = y700_ss_enum_mbus_code,
	.enum_frame_size = y700_ss_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = y700_ss_set_format,
	.get_selection = y700_ss_get_selection,
};

static const struct v4l2_subdev_ops y700_ss_subdev_ops = {
	.video = &y700_ss_video_ops,
	.pad = &y700_ss_pad_ops,
};

static const struct v4l2_subdev_internal_ops y700_ss_internal_ops = {
	.init_state = y700_ss_init_state,
};

static const struct y700_ss_gain_step sc202cs_gain_steps[] = {
	{ 1024, 0x00 },
	{ 2048, 0x01 },
	{ 4096, 0x03 },
	{ 8192, 0x07 },
	{ 16384, 0x0f },
};

static const struct y700_ss_gain_step sc820cs_gain_steps[] = {
	{ 1024, 0x00 },
	{ 2048, 0x08 },
	{ 4096, 0x09 },
	{ 8192, 0x0b },
	{ 16384, 0x0f },
};

static const struct y700_ss_gain_step *
y700_ss_find_gain_step(const struct y700_ss_gain_step *steps,
		       unsigned int num_steps, u32 gain)
{
	unsigned int i;

	for (i = num_steps - 1; i > 0; i--) {
		if (gain >= steps[i].gain)
			break;
	}

	return &steps[i];
}

static u8 y700_ss_fine_gain(u32 gain, const struct y700_ss_gain_step *step)
{
	u32 fine = (gain * 128 + step->gain / 2) / step->gain;

	return clamp_val(fine, 0, 0xff);
}

static int y700_ss_set_sc202cs_exposure(struct y700_ss *sensor, u32 exposure)
{
	struct cci_reg_sequence regs[] = {
		{ CCI_REG8(0x3e00), (exposure >> 12) & 0x0f },
		{ CCI_REG8(0x3e01), (exposure >> 4) & 0xff },
		{ CCI_REG8(0x3e02), (exposure & 0x0f) << 4 },
	};

	return cci_multi_reg_write(sensor->regmap, regs, ARRAY_SIZE(regs), NULL);
}

static int y700_ss_set_sc820cs_exposure(struct y700_ss *sensor, u32 exposure)
{
	u32 encoded = exposure * 2;
	struct cci_reg_sequence regs[] = {
		{ CCI_REG8(0x3e20), (encoded >> 20) & 0x0f },
		{ CCI_REG8(0x3e00), (encoded >> 12) & 0xff },
		{ CCI_REG8(0x3e01), (encoded >> 4) & 0xff },
		{ CCI_REG8(0x3e02), (exposure & 0x07) << 5 },
	};

	return cci_multi_reg_write(sensor->regmap, regs, ARRAY_SIZE(regs), NULL);
}

static int y700_ss_set_sc202cs_gain(struct y700_ss *sensor, u32 gain)
{
	const struct y700_ss_gain_step *step;
	struct cci_reg_sequence regs[2];

	step = y700_ss_find_gain_step(sc202cs_gain_steps,
				      ARRAY_SIZE(sc202cs_gain_steps), gain);

	regs[0].reg = CCI_REG8(0x3e09);
	regs[0].val = step->coarse;
	regs[1].reg = CCI_REG8(0x3e07);
	regs[1].val = y700_ss_fine_gain(gain, step);

	return cci_multi_reg_write(sensor->regmap, regs, ARRAY_SIZE(regs), NULL);
}

static int y700_ss_set_sc820cs_gain(struct y700_ss *sensor, u32 gain)
{
	const struct y700_ss_gain_step *step;
	struct cci_reg_sequence regs[2];

	step = y700_ss_find_gain_step(sc820cs_gain_steps,
				      ARRAY_SIZE(sc820cs_gain_steps), gain);

	regs[0].reg = CCI_REG8(0x3e08);
	regs[0].val = step->coarse;
	regs[1].reg = CCI_REG8(0x3e07);
	regs[1].val = y700_ss_fine_gain(gain, step);

	return cci_multi_reg_write(sensor->regmap, regs, ARRAY_SIZE(regs), NULL);
}

static int y700_ss_set_exposure(struct y700_ss *sensor, u32 exposure)
{
	switch (sensor->info->model) {
	case Y700_SS_SC202CS:
		return y700_ss_set_sc202cs_exposure(sensor, exposure);
	case Y700_SS_SC820CS:
		return y700_ss_set_sc820cs_exposure(sensor, exposure);
	}

	return -EINVAL;
}

static int y700_ss_set_gain(struct y700_ss *sensor, u32 gain)
{
	switch (sensor->info->model) {
	case Y700_SS_SC202CS:
		return y700_ss_set_sc202cs_gain(sensor, gain);
	case Y700_SS_SC820CS:
		return y700_ss_set_sc820cs_gain(sensor, gain);
	}

	return -EINVAL;
}

static int y700_ss_set_test_pattern(struct y700_ss *sensor, u32 pattern)
{
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;

	if (sensor->info->model != Y700_SS_SC820CS)
		return 0;

	switch (pattern) {
	case 0:
		regs = sc820cs_test_pattern_disabled_regs;
		num_regs = ARRAY_SIZE(sc820cs_test_pattern_disabled_regs);
		break;
	case 1:
		regs = sc820cs_test_pattern_black_regs;
		num_regs = ARRAY_SIZE(sc820cs_test_pattern_black_regs);
		break;
	default:
		return -EINVAL;
	}

	return cci_multi_reg_write(sensor->regmap, regs, num_regs, NULL);
}

static int y700_ss_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct y700_ss *sensor = container_of(ctrl->handler, struct y700_ss,
					     ctrls);
	int ret = 0;

	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = y700_ss_set_exposure(sensor, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = y700_ss_set_gain(sensor, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = y700_ss_set_test_pattern(sensor, ctrl->val);
		break;
	default:
		break;
	}

	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_ctrl_ops y700_ss_ctrl_ops = {
	.s_ctrl = y700_ss_set_ctrl,
};

static int y700_ss_get_regulators(struct device *dev, struct y700_ss *sensor)
{
	unsigned int i;

	for (i = 0; i < sensor->info->num_supplies; i++)
		sensor->supplies[i].supply = sensor->info->supply_names[i];

	return devm_regulator_bulk_get(dev, sensor->info->num_supplies,
				       sensor->supplies);
}

static int y700_ss_parse_fwnode(struct y700_ss *sensor)
{
	struct device *dev = sensor->dev;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL, "endpoint node not found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	if (ret)
		goto done;

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       sensor->info->link_freq_menu_items,
				       sensor->info->num_link_freqs,
				       &sensor->link_freq_bitmap);

done:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	fwnode_handle_put(endpoint);
	return ret;
}

static int y700_ss_init_controls(struct y700_ss *sensor)
{
	const struct y700_ss_mode *mode = sensor->info->mode;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr = &sensor->ctrls;
	s64 hblank = mode->hts - mode->width;
	u64 pixel_rate = y700_ss_to_pixel_rate(sensor->info);
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	sensor->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &y700_ss_ctrl_ops,
						  V4L2_CID_LINK_FREQ,
						  sensor->info->num_link_freqs - 1,
						  0,
						  sensor->info->link_freq_menu_items);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(ctrl_hdlr, &y700_ss_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  pixel_rate, pixel_rate, 1, pixel_rate);
	sensor->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &y700_ss_ctrl_ops,
					   V4L2_CID_HBLANK,
					   hblank, hblank, 1, hblank);
	sensor->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &y700_ss_ctrl_ops,
					   V4L2_CID_VBLANK,
					   mode->vts_def - mode->height,
					   mode->vts_def - mode->height, 1,
					   mode->vts_def - mode->height);
	sensor->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &y700_ss_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     Y700_SS_EXPOSURE_MIN,
					     mode->vts_def - Y700_SS_EXPOSURE_MARGIN,
					     1, mode->exposure_def);
	sensor->analogue_gain =
		v4l2_ctrl_new_std(ctrl_hdlr, &y700_ss_ctrl_ops,
				  V4L2_CID_ANALOGUE_GAIN,
				  Y700_SS_ANALOGUE_GAIN_MIN,
				  Y700_SS_ANALOGUE_GAIN_MAX, 1,
				  Y700_SS_ANALOGUE_GAIN_DEFAULT);
	if (sensor->info->model == Y700_SS_SC820CS)
		sensor->test_pattern =
			v4l2_ctrl_new_std_menu_items(ctrl_hdlr,
						     &y700_ss_ctrl_ops,
						     V4L2_CID_TEST_PATTERN,
						     ARRAY_SIZE(sc820cs_test_pattern_menu) - 1,
						     0, 0,
						     sc820cs_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		goto error_ctrls;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &y700_ss_ctrl_ops,
					      &props);
	if (ret)
		goto error_ctrls;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_ctrls;
	}

	sensor->sd.ctrl_handler = ctrl_hdlr;
	return 0;

error_ctrls:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	return ret;
}

static int y700_ss_identify_module(struct y700_ss *sensor)
{
	u64 val;
	int ret;

	ret = cci_read(sensor->regmap, Y700_SS_CHIP_ID_REG, &val, NULL);
	if (ret)
		return dev_err_probe(sensor->dev, ret, "failed to read chip id\n");

	if (val != sensor->info->chip_id)
		return dev_err_probe(sensor->dev, -ENXIO,
				     "%s chip id mismatch: 0x%04x != 0x%04llx\n",
				     sensor->info->name, sensor->info->chip_id,
				     val);

	dev_dbg(sensor->dev, "%s chip id: 0x%04llx\n",
		sensor->info->name, val);

	return 0;
}

static int y700_ss_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct y700_ss *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;
	sensor->info = device_get_match_data(dev);
	if (!sensor->info)
		return -EINVAL;
	sensor->cur_mode = sensor->info->mode;

	ret = y700_ss_parse_fwnode(sensor);
	if (ret)
		return ret;

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to init CCI\n");

	sensor->xclk = devm_v4l2_sensor_clk_get_legacy(dev, NULL, true,
						       Y700_SS_DEFAULT_CLK_FREQ);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");

	ret = y700_ss_get_regulators(dev, sensor);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	sensor->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset gpio\n");

	v4l2_i2c_subdev_init(&sensor->sd, client, &y700_ss_subdev_ops);
	v4l2_i2c_subdev_set_name(&sensor->sd, client, sensor->info->name,
				 NULL);
	sensor->sd.internal_ops = &y700_ss_internal_ops;

	ret = y700_ss_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on sensor\n");

	ret = y700_ss_identify_module(sensor);
	if (ret)
		goto err_power_off;

	ret = y700_ss_init_controls(sensor);
	if (ret)
		goto err_power_off;

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.dev = dev;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrls_free;

	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto err_media_cleanup;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_rpm;

	return 0;

err_rpm:
	pm_runtime_disable(dev);
	v4l2_subdev_cleanup(&sensor->sd);
err_media_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
err_ctrls_free:
	v4l2_ctrl_handler_free(&sensor->ctrls);
err_power_off:
	y700_ss_power_off(dev);
	return ret;
}

static void y700_ss_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct y700_ss *sensor = to_y700_ss(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		y700_ss_power_off(sensor->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id y700_ss_of_match[] = {
	{ .compatible = "smartsens,sc202cs", .data = &sc202cs_info },
	{ .compatible = "smartsens,sc820cs", .data = &sc820cs_info },
	{ }
};
MODULE_DEVICE_TABLE(of, y700_ss_of_match);

static DEFINE_RUNTIME_DEV_PM_OPS(y700_ss_pm_ops, y700_ss_power_off,
				 y700_ss_power_on, NULL);

static struct i2c_driver y700_ss_i2c_driver = {
	.driver = {
		.name = "y700-smartsens",
		.of_match_table = y700_ss_of_match,
		.pm = pm_ptr(&y700_ss_pm_ops),
	},
	.probe = y700_ss_probe,
	.remove = y700_ss_remove,
};
module_i2c_driver(y700_ss_i2c_driver);

MODULE_DESCRIPTION("Lenovo Y700 SmartSens camera sensor bring-up driver");
MODULE_AUTHOR("Lenovo Y700 mainline port");
MODULE_LICENSE("GPL");
