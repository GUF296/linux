// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight driver for the Kinetic KTZ8866
 *
 * Copyright (C) 2022, 2023 Jianhua Lu <lujianhua000@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define DEFAULT_BRIGHTNESS 1500
#define MAX_BRIGHTNESS 2047
#define REG_MAX 0x15

/* reg */
#define SOFT_RESET 0x00
#define DEVICE_ID 0x01
#define BL_CFG1 0x02
#define BL_CFG2 0x03
#define BL_BRT_LSB 0x04
#define BL_BRT_MSB 0x05
#define BL_EN 0x08
#define LCD_BIAS_CFG1 0x09
#define LCD_BIAS_CFG2 0x0A
#define LCD_BIAS_CFG3 0x0B
#define LCD_BOOST_CFG 0x0C
#define OUTP_CFG 0x0D
#define OUTN_CFG 0x0E
#define FLAG 0x0F
#define BL_OPTION1 0x10
#define BL_OPTION2 0x11
#define PWM2DIG_LSBs 0x12
#define PWM2DIG_MSBs 0x13
#define BL_DIMMING 0x14
#define PWM_RAMP_TIME 0x15

/* definition */
#define BL_EN_BIT BIT(6)
#define LCD_BIAS_EN 0x9F
#define PWM_HYST 0x5

struct ktz8866 {
	struct i2c_client *client;
	struct regmap *regmap;
	bool led_on;
	struct gpio_desc *enable_gpio;
	unsigned int brightness;
	unsigned int max_brightness;
};

static const struct regmap_config ktz8866_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
};

static int ktz8866_write(struct ktz8866 *ktz, unsigned int reg,
			 unsigned int val)
{
	return regmap_write(ktz->regmap, reg, val);
}

static int ktz8866_update_bits(struct ktz8866 *ktz, unsigned int reg,
			       unsigned int mask, unsigned int val)
{
	return regmap_update_bits(ktz->regmap, reg, mask, val);
}

static void ktz8866_disable_gpio(void *data)
{
	struct ktz8866 *ktz = data;

	if (ktz->enable_gpio)
		gpiod_set_value_cansleep(ktz->enable_gpio, 0);
}

static int ktz8866_backlight_update_status(struct backlight_device *backlight_dev)
{
	struct ktz8866 *ktz = bl_get_data(backlight_dev);
	unsigned int brightness = backlight_get_brightness(backlight_dev);
	bool led_on = ktz->led_on;
	int ret, rollback_ret;

	if (!ktz->led_on && brightness > 0) {
		ret = ktz8866_update_bits(ktz, BL_EN, BL_EN_BIT, BL_EN_BIT);
		if (ret)
			return ret;
		led_on = true;
	} else if (brightness == 0) {
		ret = ktz8866_update_bits(ktz, BL_EN, BL_EN_BIT, 0);
		if (ret)
			return ret;
		led_on = false;
	}

	/* Set brightness */
	if (brightness > ktz->max_brightness)
		brightness = ktz->max_brightness;

	ret = ktz8866_write(ktz, BL_BRT_LSB, brightness & 0x7);
	if (ret)
		goto rollback_brightness;

	ret = ktz8866_write(ktz, BL_BRT_MSB, (brightness >> 3) & 0xFF);
	if (ret)
		goto rollback_brightness;

	ktz->led_on = led_on;
	ktz->brightness = brightness;

	return 0;

rollback_brightness:
	rollback_ret = ktz8866_write(ktz, BL_BRT_LSB,
				     ktz->brightness & 0x7);
	if (rollback_ret)
		dev_warn(&ktz->client->dev,
			 "failed to restore backlight brightness LSB: %d\n",
			 rollback_ret);
	rollback_ret = ktz8866_write(ktz, BL_BRT_MSB,
				     (ktz->brightness >> 3) & 0xff);
	if (rollback_ret)
		dev_warn(&ktz->client->dev,
			 "failed to restore backlight brightness MSB: %d\n",
			 rollback_ret);
	if (led_on != ktz->led_on) {
		rollback_ret = ktz8866_update_bits(ktz, BL_EN, BL_EN_BIT,
						   ktz->led_on ? BL_EN_BIT : 0);
		if (rollback_ret)
			dev_warn(&ktz->client->dev,
				 "failed to restore backlight enable state: %d\n",
				 rollback_ret);
	}

	return ret;
}

static const struct backlight_ops ktz8866_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ktz8866_backlight_update_status,
};

static int ktz8866_init(struct ktz8866 *ktz)
{
	unsigned int val = 0;
	int ret;

	if (of_device_is_compatible(ktz->client->dev.of_node, "lenovo,ktz8866")) {
		/*
		 * Matches the vendor KTZ8866 bootstrap table:
		 * 00=01, delay 10 ms, 02=da, 11=37, 15=68.
		 */
		ret = ktz8866_write(ktz, SOFT_RESET, 0x01);
		if (ret)
			return ret;
		usleep_range(10000, 11000);
		ret = ktz8866_write(ktz, BL_CFG1, 0xda);
		if (ret)
			return ret;
		ret = ktz8866_write(ktz, BL_OPTION2, 0x37);
		if (ret)
			return ret;
		ret = ktz8866_write(ktz, PWM_RAMP_TIME, 0x68);
		if (ret)
			return ret;
	}

	if (!of_property_read_u32(ktz->client->dev.of_node, "current-num-sinks", &val)) {
		if (!val || val > 6)
			return -EINVAL;
		ret = ktz8866_write(ktz, BL_EN, BIT(val) - 1);
	} else {
		/* Enable all 6 current sinks if the number of current sinks isn't specified. */
		ret = ktz8866_write(ktz, BL_EN, BIT(6) - 1);
	}
	if (ret)
		return ret;

	if (!of_property_read_u32(ktz->client->dev.of_node, "kinetic,current-ramp-delay-ms", &val)) {
		if (val && val <= 128 && is_power_of_2(val))
			ret = ktz8866_write(ktz, BL_CFG2,
					    BIT(7) | (ilog2(val) << 3) | PWM_HYST);
		else if (val >= 192 && val <= 640 && !(val % 64))
			ret = ktz8866_write(ktz, BL_CFG2,
					    BIT(7) | ((5 + val / 64) << 3) | PWM_HYST);
		else
			return -EINVAL;
		if (ret)
			return ret;
	}

	if (!of_property_read_u32(ktz->client->dev.of_node, "kinetic,led-enable-ramp-delay-ms", &val)) {
		if (val == 0) {
			ret = ktz8866_write(ktz, BL_DIMMING, 0);
		} else if (val <= 16384 && is_power_of_2(val)) {
			unsigned int ramp_off_time = ilog2(val) + 1;
			unsigned int ramp_on_time = ramp_off_time << 4;
			ret = ktz8866_write(ktz, BL_DIMMING,
					    ramp_on_time | ramp_off_time);
		} else {
			return -EINVAL;
		}
		if (ret)
			return ret;
	}

	if (of_property_read_bool(ktz->client->dev.of_node, "kinetic,enable-lcd-bias")) {
		ret = ktz8866_write(ktz, LCD_BIAS_CFG1, LCD_BIAS_EN);
		if (ret)
			return ret;
	}

	return 0;
}

static int ktz8866_probe(struct i2c_client *client)
{
	struct backlight_device *backlight_dev;
	struct backlight_properties props;
	struct ktz8866 *ktz;
	unsigned int val;
	int ret = 0;

	ktz = devm_kzalloc(&client->dev, sizeof(*ktz), GFP_KERNEL);
	if (!ktz)
		return -ENOMEM;

	ktz->client = client;
	ktz->regmap = devm_regmap_init_i2c(client, &ktz8866_regmap_config);
	if (IS_ERR(ktz->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(ktz->regmap), "failed to init regmap\n");

	ret = devm_regulator_get_enable_optional(&client->dev, "vddpos");
	if (ret && ret != -ENODEV)
		return dev_err_probe(&client->dev, ret, "get regulator vddpos failed\n");
	ret = devm_regulator_get_enable_optional(&client->dev, "vddneg");
	if (ret && ret != -ENODEV)
		return dev_err_probe(&client->dev, ret, "get regulator vddneg failed\n");

	ktz->enable_gpio = devm_gpiod_get_optional(&client->dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(ktz->enable_gpio))
		return PTR_ERR(ktz->enable_gpio);

	ret = devm_add_action_or_reset(&client->dev, ktz8866_disable_gpio, ktz);
	if (ret)
		return ret;

	if (ktz->enable_gpio)
		usleep_range(10000, 11000);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	ktz->max_brightness = MAX_BRIGHTNESS;
	if (!of_property_read_u32(client->dev.of_node, "max-brightness", &val))
		ktz->max_brightness = min_t(unsigned int, val, MAX_BRIGHTNESS);

	props.max_brightness = ktz->max_brightness;
	props.brightness = DEFAULT_BRIGHTNESS;
	if (!of_property_read_u32(client->dev.of_node, "default-brightness-level", &val))
		props.brightness = min_t(unsigned int, val, ktz->max_brightness);
	props.scale = BACKLIGHT_SCALE_LINEAR;

	backlight_dev = devm_backlight_device_register(&client->dev, "ktz8866-backlight",
					&client->dev, ktz, &ktz8866_backlight_ops, &props);
	if (IS_ERR(backlight_dev))
		return dev_err_probe(&client->dev, PTR_ERR(backlight_dev),
				"failed to register backlight device\n");

	ret = ktz8866_init(ktz);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to initialize backlight controller\n");

	i2c_set_clientdata(client, backlight_dev);
	ret = backlight_update_status(backlight_dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to set initial backlight state\n");

	return 0;
}

static void ktz8866_remove(struct i2c_client *client)
{
	struct backlight_device *backlight_dev = i2c_get_clientdata(client);
	int ret;

	backlight_dev->props.brightness = 0;
	ret = backlight_update_status(backlight_dev);
	if (ret)
		dev_warn(&client->dev, "failed to disable backlight: %d\n", ret);
}

static const struct i2c_device_id ktz8866_ids[] = {
	{ "ktz8866" },
	{}
};
MODULE_DEVICE_TABLE(i2c, ktz8866_ids);

static const struct of_device_id ktz8866_match_table[] = {
	{
		.compatible = "lenovo,ktz8866",
	},
	{
		.compatible = "kinetic,ktz8866",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ktz8866_match_table);

static struct i2c_driver ktz8866_driver = {
	.driver = {
		.name = "ktz8866",
		.of_match_table = ktz8866_match_table,
	},
	.probe = ktz8866_probe,
	.remove = ktz8866_remove,
	.id_table = ktz8866_ids,
};

module_i2c_driver(ktz8866_driver);

MODULE_DESCRIPTION("Kinetic KTZ8866 Backlight Driver");
MODULE_AUTHOR("Jianhua Lu <lujianhua000@gmail.com>");
MODULE_LICENSE("GPL");
