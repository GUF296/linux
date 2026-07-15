// SPDX-License-Identifier: GPL-2.0
/* aw882xx_calib.c cali_module
 *
 * Copyright (c) 2020 AWINIC Technology CO., LTD
 *
 *  Author: Nick Li <liweilei@awinic.com.cn>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
/*#define DEBUG*/
#include <linux/module.h>
#include <asm/ioctls.h>
#include <linux/capability.h>
#include <linux/ctype.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "aw882xx.h"
#include "aw882xx_dsp.h"
#include "aw882xx_log.h"
#include "aw882xx_calib.h"

static const char *cali_str[CALI_STR_MAX] = {"none", "start_cali", "cali_re",
	"cali_f0", "store_re", "show_re", "show_r0", "show_cali_f0", "show_f0",
	"show_te", "show_st", "dev_sel", "get_ver", "get_dev_num",
	"cali_q_f0", "show_q_f0", "get_re_range"
};

static char *ch_name[AW_DEV_CH_MAX] = {"pri_l", "pri_r", "sec_l", "sec_r",
						"tert_l", "tert_r", "quat_l", "quat_r"};
static unsigned int g_cali_re_time = AW_CALI_RE_DEFAULT_TIMER;
static unsigned int g_cali_status;
static unsigned int g_misc_users;
static unsigned int g_class_users;
static DEFINE_MUTEX(g_cali_lock);
static DEFINE_MUTEX(g_cali_dsp_lock);

struct aw_cali_file_ctx {
	struct mutex lock;
	unsigned int channel;
	unsigned int pending_cmd;
	char response[256];
	size_t response_len;
};

#ifdef CONFIG_COMPAT
struct aw_compat_ptr_params_data {
	compat_int_t len;
	compat_uptr_t data;
};

struct aw_compat_ioctl_msg {
	compat_int_t type;
	compat_int_t opcode_id;
	compat_int_t version;
	compat_int_t data_len;
	compat_uptr_t data_buf;
	compat_int_t reseriver[2];
};

#define AW_IOCTL_SET_PTR_PARAM_NUM32 \
	_IOWR(AW_IOCTL_MAGIC, 12, struct aw_compat_ptr_params_data)
#define AW_IOCTL_MSG32 \
	_IOWR(AW_IOCTL_MAGIC, 16, struct aw_compat_ioctl_msg)
#endif

#ifndef AW_AUDIOREACH_PLATFORM
#define AW_CALI_STORE_EXAMPLE
#endif

#ifdef AW_CALI_STORE_EXAMPLE
 /*write cali to persist file example*/
#define AWINIC_CALI_FILE  "/mnt/vendor/persist/factory/audio/aw_cali.bin"
#define AW_INT_DEC_DIGIT 10

static ssize_t aw_fs_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
#ifdef AW_KERNEL_VER_OVER_5_4_0
	return kernel_read(file, buf, count, pos);
#else
	return vfs_read(file, buf, count, pos);
#endif
}

static ssize_t aw_fs_write(struct file *file, char *buf, size_t count, loff_t *pos)
{
#ifdef AW_KERNEL_VER_OVER_5_4_0
	return kernel_write(file, buf, count, pos);
#else
	return vfs_write(file, buf, count, pos);
#endif
}

static int aw_cali_write_cali_re_to_file(int32_t cali_re, int channel)
{
	struct file *fp = NULL;
	char buf[AW_INT_DEC_DIGIT + 1] = {0};
	loff_t pos = 0;
	ssize_t written;
	int len;
#if !defined AW_KERNEL_VER_OVER_6_1_0
	mm_segment_t fs;
#endif

	if (channel < 0 || channel >= AW_DEV_CH_MAX)
		return -EINVAL;

	fp = filp_open(AWINIC_CALI_FILE, O_RDWR | O_CREAT, 0600);
	if (IS_ERR(fp)) {
		aw_pr_err("channel:%d open %s failed, error=%ld",
			channel, AWINIC_CALI_FILE, PTR_ERR(fp));
		return PTR_ERR(fp);
	}

	pos = AW_INT_DEC_DIGIT * channel;

	len = scnprintf(buf, sizeof(buf), "%10d", cali_re);
	if (len != AW_INT_DEC_DIGIT) {
		filp_close(fp, NULL);
		return -ERANGE;
	}

#ifdef AW_KERNEL_VER_OVER_6_1_0
#elif defined AW_KERNEL_VER_OVER_5_10_0
	fs = force_uaccess_begin();
#else
	fs = get_fs();
	set_fs(KERNEL_DS);
#endif

	written = aw_fs_write(fp, buf, AW_INT_DEC_DIGIT, &pos);

#ifdef AW_KERNEL_VER_OVER_6_1_0
#elif defined AW_KERNEL_VER_OVER_5_10_0
	force_uaccess_end(fs);
#else
	set_fs(fs);
#endif

	if (written != AW_INT_DEC_DIGIT) {
		filp_close(fp, NULL);
		return written < 0 ? written : -EIO;
	}

	aw_pr_info("channel:%d buf:%s cali_re:%d",
			channel, buf, cali_re);

	filp_close(fp, NULL);
	return 0;
}

static int aw_cali_get_read_cali_re(int32_t *cali_re, int channel)
{
	struct file *fp = NULL;
	/*struct inode *node;*/
	int f_size;
	char *buf = NULL;
	int32_t int_cali_re = 0;
	loff_t pos = 0;
	ssize_t bytes_read;
	int ret;
#if !defined AW_KERNEL_VER_OVER_6_1_0
	mm_segment_t fs;
#endif

	char *re_buf = NULL;

	if (!cali_re || channel < 0 || channel >= AW_DEV_CH_MAX)
		return -EINVAL;

	fp = filp_open(AWINIC_CALI_FILE, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		aw_pr_err("channel:%d open %s failed, error=%ld",
			channel, AWINIC_CALI_FILE, PTR_ERR(fp));
		return PTR_ERR(fp);
	}

	pos = AW_INT_DEC_DIGIT * channel;

	/*node = fp->f_dentry->d_inode;*/
	/*f_size = node->i_size;*/
	f_size = AW_INT_DEC_DIGIT;

	buf = kzalloc(f_size + 1, GFP_KERNEL);
	if (!buf) {
		filp_close(fp, NULL);
		return -ENOMEM;
	}

#ifdef AW_KERNEL_VER_OVER_6_1_0
#elif defined AW_KERNEL_VER_OVER_5_10_0
		fs = force_uaccess_begin();
#else
		fs = get_fs();
		set_fs(KERNEL_DS);
#endif

	bytes_read = aw_fs_read(fp, buf, f_size, &pos);

#ifdef AW_KERNEL_VER_OVER_6_1_0
#elif defined AW_KERNEL_VER_OVER_5_10_0
		force_uaccess_end(fs);
#else
		set_fs(fs);
#endif

	if (bytes_read != f_size) {
		ret = bytes_read < 0 ? bytes_read : -EIO;
		goto out;
	}

	re_buf = skip_spaces(buf);
	ret = kstrtoint(re_buf, 10, &int_cali_re);
	if (ret)
		goto out;
	*cali_re = int_cali_re;

	re_buf = NULL;
	aw_pr_info("channel:%d buf:%s int_cali_re: %d",
		channel, buf, int_cali_re);

out:
	kfree(buf);
	filp_close(fp, NULL);

	return ret;
}
#endif

/*custom need add to set/get cali_re form/to nv*/
static int aw_cali_write_re_to_nvram(int32_t cali_re, int32_t channel)
{
#ifdef AW_CALI_STORE_EXAMPLE
	if (channel < 0 || channel >= AW_DEV_CH_MAX) {
		aw_pr_err("unsupported channel [%d]", channel);
		return -EINVAL;
	}
	return aw_cali_write_cali_re_to_file(cali_re, channel);
#else
	return 0;
#endif
}

int aw882xx_cali_read_re_from_nvram(int32_t *cali_re, int32_t channel)
{
	/*custom add, if success return value is 0 , else -1*/
#ifdef AW_CALI_STORE_EXAMPLE
	if (!cali_re || channel < 0 || channel >= AW_DEV_CH_MAX) {
		aw_pr_err("unsupported channel [%d]", channel);
		return -EINVAL;
	}
	return aw_cali_get_read_cali_re(cali_re, channel);
#else
	return 0;
#endif
}

bool aw882xx_cali_check_result(struct aw_cali_desc *cali_desc)
{
	if (cali_desc->cali_check_st &&
		(cali_desc->cali_result == CALI_RESULT_ERROR)) {
		return false;
	} else {
		return true;
	}
}

static void aw_cali_svc_run_mute(struct aw_device *aw_dev, int8_t cali_result)
{
	aw_dev_dbg(aw_dev->dev, "enter");

	if (aw_dev->cali_desc.cali_check_st) {
		if (cali_result == CALI_RESULT_ERROR)
			aw882xx_dev_mute(aw_dev, true);
		else if (cali_result == CALI_RESULT_NORMAL)
			aw882xx_dev_mute(aw_dev, false);
		else
			aw_dev_info(aw_dev->dev, "unsupported result");
	} else {
		aw_dev_info(aw_dev->dev, "cali check disable");
	}

	aw_dev_info(aw_dev->dev, "done");
}

static int aw_cali_svc_get_dev_re_range(struct aw_device *aw_dev,
						uint32_t *data_buf)
{
	data_buf[RE_MIN_FLAG] = aw_dev->re_min;
	data_buf[RE_MAX_FLAG] = aw_dev->re_max;

	return 0;
}

static int aw_cali_svc_get_devs_re_range(struct aw_device *aw_dev,
								uint32_t *data_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev = NULL;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel < num) {
			/*buf[0]:pri_l_re_min, buf[1]:pri_l_re_min, buf[2]:pri_r_re_min, buf[3]:pri_r_re_min*/
			data_buf[RE_MIN_FLAG + local_dev->channel * 2] =
				local_dev->re_min;
			data_buf[RE_MAX_FLAG + local_dev->channel * 2] =
				local_dev->re_max;
			span = max(span, (int)local_dev->channel + 1);
		} else {
			aw_dev_err(local_dev->dev, "channel num[%d] overflow buf num[%d]",
							local_dev->channel, num);
			ret = -EINVAL;
			goto out;
		}
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_store_cali_re(struct aw_device *aw_dev, int32_t re)
{
	int ret;

	if ((re < aw_dev->re_min) || (re > aw_dev->re_max)) {
		aw_dev_info(aw_dev->dev, "re[%d] out of range {%d, %d}",
						re, aw_dev->re_min, aw_dev->re_max);
		ret = -ERANGE;
		goto store_cali_re_failed;
	}

	ret = aw_cali_write_re_to_nvram(re, aw_dev->channel);
	if (ret) {
		aw_dev_err(aw_dev->dev, "write re to nvram failed: %d", ret);
		goto store_cali_re_failed;
	}

	aw_dev->cali_desc.cali_re = re;
	aw_dev_info(aw_dev->dev, "store cali re is %d", re);

	if (aw_dev->cali_desc.cali_check_st)
		aw_dev->cali_desc.cali_result = CALI_RESULT_NORMAL;

	return 0;

store_cali_re_failed:
	if (aw_dev->cali_desc.cali_check_st) {
		aw_dev->cali_desc.cali_result = CALI_RESULT_ERROR;
		aw_cali_svc_run_mute(aw_dev, aw_dev->cali_desc.cali_result);
	}
	return ret;
}

/*********************************aw_cali_sevice*********************************************************/
static void aw_cali_svc_set_cali_status(struct aw_device *aw_dev, int status)
{
	if (status)
		g_cali_status = true;
	else
		g_cali_status = false;

	aw_dev_info(aw_dev->dev, "cali %s",
		(status == 0) ? ("disable") : ("enable"));
}

int aw882xx_cali_svc_get_cali_status(void)
{
	return g_cali_status;
}

static int aw_cali_svc_dev_get_re(struct aw_device *aw_dev)
{
	int ret, i;
	int32_t re[AW_CALI_READ_RE_TIMES] = {0};
	s64 sum = 0;

	for (i = 0; i < AW_CALI_READ_RE_TIMES; i++) {
		ret = aw882xx_dsp_read_r0(aw_dev, &re[i]);
		if (ret) {
			aw_dev_err(aw_dev->dev, "get re failed!");
			if (aw_dev->cali_desc.cali_check_st) {
				aw_dev->cali_desc.cali_result = CALI_RESULT_ERROR;
				aw_cali_svc_run_mute(aw_dev, aw_dev->cali_desc.cali_result);
			}
			return ret;
		}
		sum += re[i];
		usleep_range(AW_32000_US, AW_32000_US + 10);
	}
	re[0] = sum / AW_CALI_READ_RE_TIMES;

	ret = aw_cali_store_cali_re(aw_dev, re[0]);
	return ret;
}

static int aw_cali_svc_dev_get_f0(struct aw_device *aw_dev)
{
	int ret, i;
	int32_t f0[AW_CALI_READ_F0_Q_TIMES] = {0};
	s64 sum = 0;

	for (i = 0; i < AW_CALI_READ_F0_Q_TIMES; i++) {
		ret = aw882xx_dsp_read_f0(aw_dev, &f0[i]);
		if (ret) {
			aw_dev_err(aw_dev->dev, "get f0 failed!");
			return ret;
		}
		sum += f0[i];
		usleep_range(AW_70000_US, AW_70000_US + 10);
	}
	f0[0] = sum / AW_CALI_READ_F0_Q_TIMES;

	aw_dev->cali_desc.cali_f0 = f0[0];
	return 0;
}

static int aw_cali_svc_dev_get_f0_q(struct aw_device *aw_dev)
{
	int ret, i;
	int32_t f0[AW_CALI_READ_F0_Q_TIMES] = {0};
	int32_t q[AW_CALI_READ_F0_Q_TIMES] = {0};
	s64 sum_f0 = 0, sum_q = 0;

	for (i = 0; i < AW_CALI_READ_F0_Q_TIMES; i++) {
		ret = aw882xx_dsp_read_f0_q(aw_dev, &f0[i], &q[i]);
		if (ret) {
			aw_dev_err(aw_dev->dev, "get f0 failed!");
			return ret;
		}
		sum_f0 += f0[i];
		sum_q += q[i];
		usleep_range(AW_70000_US, AW_70000_US + 10);
	}
	f0[0] = sum_f0 / AW_CALI_READ_F0_Q_TIMES;
	q[0] = sum_q / AW_CALI_READ_F0_Q_TIMES;
	aw_dev->cali_desc.cali_f0 = f0[0];
	aw_dev->cali_desc.cali_q = q[0];
	aw_dev_dbg(aw_dev->dev, "cali f0 is %d, q is %d",
			aw_dev->cali_desc.cali_f0, aw_dev->cali_desc.cali_q);
	return 0;
}


static int aw_cali_svc_dev_cali_mode_en(struct aw_device *aw_dev, int type,
					bool is_enable, unsigned int flag)
{
	int ret = 0;
	int tmp;
	bool aux_enabled = false;

	/* open cali mode */
	if (is_enable) {
		aw882xx_dev_iv_forbidden_output(aw_dev, false);

		if (type == CALI_TYPE_RE) {
			if (flag & CALI_OPS_HMUTE) {
				ret = aw882xx_dsp_hmute_en(aw_dev, true);
				if (ret < 0)
					goto enable_failed;
				aux_enabled = true;
			}
			ret = aw882xx_dsp_cali_en(aw_dev, MSG_CALI_RE_ENABLE_DATA);
			if (ret < 0)
				goto enable_failed;
		} else {
			if (flag & CALI_OPS_NOISE) {
				ret = aw882xx_dsp_noise_en(aw_dev, true);
				if (ret < 0)
					goto enable_failed;
				aux_enabled = true;
			}
			ret = aw882xx_dsp_cali_en(aw_dev, MSG_CALI_F0_ENABLE_DATA);
			if (ret < 0)
				goto enable_failed;
		}

		aw_cali_svc_set_cali_status(aw_dev, true);
		return 0;
	}

	ret = aw882xx_dsp_cali_en(aw_dev, MSG_CALI_DISABLE_DATA);

	if (type == CALI_TYPE_RE) {
		/*close mute*/
		if (flag & CALI_OPS_HMUTE) {
			tmp = aw882xx_dsp_hmute_en(aw_dev, false);
			if (!ret && tmp < 0)
				ret = tmp;
		}
	} else {
		/*close mute*/
		if (flag & CALI_OPS_NOISE) {
			tmp = aw882xx_dsp_noise_en(aw_dev, false);
			if (!ret && tmp < 0)
				ret = tmp;
		}
	}

	/*close cali mode*/
	aw_cali_svc_set_cali_status(aw_dev, false);

	aw882xx_dev_iv_forbidden_output(aw_dev, true);
	return ret;

enable_failed:
	aw882xx_dsp_cali_en(aw_dev, MSG_CALI_DISABLE_DATA);
	if (aux_enabled) {
		if (type == CALI_TYPE_RE)
			aw882xx_dsp_hmute_en(aw_dev, false);
		else
			aw882xx_dsp_noise_en(aw_dev, false);
	}
	aw_cali_svc_set_cali_status(aw_dev, false);
	aw882xx_dev_iv_forbidden_output(aw_dev, true);
	return ret;
}

static int aw_cali_svc_dev_cali_re(struct aw_device *aw_dev, unsigned int flag)
{
	int ret, cleanup_ret;
	bool enabled = false;

	aw_cali_svc_run_mute(aw_dev, CALI_RESULT_NORMAL);

	ret = aw_cali_svc_dev_cali_mode_en(aw_dev, CALI_TYPE_RE, true, flag);
	if (ret < 0)
		return ret;
	enabled = true;

	/*wait time*/
	msleep(g_cali_re_time);

	/*dev get echo cali re*/
	ret = aw_cali_svc_dev_get_re(aw_dev);
	if (enabled) {
		cleanup_ret = aw_cali_svc_dev_cali_mode_en(aw_dev,
							 CALI_TYPE_RE, false, flag);
		if (!ret && cleanup_ret < 0)
			ret = cleanup_ret;
	}
	return ret;
}

static int aw_cali_svc_devs_get_cali_cali_data(struct aw_device_list *dev_list,
							unsigned int data_type)
{
	unsigned int i;
	int ret = 0;
	struct aw_device *local_dev = NULL;

	for (i = 0; i < dev_list->count; i++) {
		local_dev = dev_list->devices[i];
		if (data_type == CALI_DATA_RE) {
			ret = aw_cali_svc_dev_get_re(local_dev);
		} else if (data_type == CALI_DATA_F0) {
			ret = aw_cali_svc_dev_get_f0(local_dev);
		} else if (data_type == CALI_DATA_F0_Q) {
			ret = aw_cali_svc_dev_get_f0_q(local_dev);
		} else {
			aw_dev_err(local_dev->dev, "unsupport cali data type :%d", data_type);
			return -EINVAL;
		}
		if (ret < 0) {
			aw_dev_err(local_dev->dev, "get cali data type[%d] failed", data_type);
			return ret;
		}
	}

	return ret;
}

static int aw_cali_svc_devs_cali_mode_enable(struct aw_device_list *dev_list,
								int type, unsigned int flag,
								bool is_enable)
{
	unsigned int i;
	int ret = 0, tmp;
	struct aw_device *local_dev = NULL;

	for (i = 0; i < dev_list->count; i++) {
		local_dev = dev_list->devices[i];
		if (is_enable)
			aw_cali_svc_run_mute(local_dev, CALI_RESULT_NORMAL);
		tmp = aw_cali_svc_dev_cali_mode_en(local_dev, type, is_enable, flag);
		if (tmp < 0) {
			if (is_enable) {
				ret = tmp;
				goto unwind;
			}
			if (!ret)
				ret = tmp;
		}
		if (!is_enable && (type == CALI_TYPE_F0))
			aw_cali_svc_run_mute(local_dev, local_dev->cali_desc.cali_result);
	}

	return ret;

unwind:
	while (i--) {
		local_dev = dev_list->devices[i];
		aw_cali_svc_dev_cali_mode_en(local_dev, type, false, flag);
		if (type == CALI_TYPE_F0)
			aw_cali_svc_run_mute(local_dev,
						local_dev->cali_desc.cali_result);
	}
	return ret;
}

static int aw_cali_svc_devs_cali_re(struct aw_device *aw_dev, unsigned int flag)
{
	struct aw_device_list dev_list;
	int ret, cleanup_ret;
	bool enabled = false;

	aw_dev_info(aw_dev->dev, "enter");

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, " get dev list failed");
		return ret;
	}

	ret = aw_cali_svc_devs_cali_mode_enable(&dev_list, CALI_TYPE_RE, flag, true);
	if (ret < 0)
		goto out;
	enabled = true;

	msleep(g_cali_re_time);

	ret = aw_cali_svc_devs_get_cali_cali_data(&dev_list, CALI_DATA_RE);
out:
	if (enabled) {
		cleanup_ret = aw_cali_svc_devs_cali_mode_enable(&dev_list,
							  CALI_TYPE_RE, flag, false);
		if (!ret && cleanup_ret < 0)
			ret = cleanup_ret;
	}
	aw882xx_dev_put_list(&dev_list);
	return ret;
}


static int aw_cali_svc_cali_re(struct aw_device *aw_dev, bool is_single, unsigned int flag)
{
	if (is_single)
		return aw_cali_svc_dev_cali_re(aw_dev, flag);
	else
		return aw_cali_svc_devs_cali_re(aw_dev, flag);
	return 0;
}

static int aw_cali_svc_dev_cali_f0(struct aw_device *aw_dev, unsigned int flag)
{
	int ret, cleanup_ret;

	aw_cali_svc_run_mute(aw_dev, CALI_RESULT_NORMAL);

	ret = aw_cali_svc_dev_cali_mode_en(aw_dev, CALI_TYPE_F0, true, flag);
	if (ret < 0)
		return ret;

	/*wait time*/
	msleep(5 * 1000);

	/*dev get cali f0*/
	ret = aw_cali_svc_dev_get_f0(aw_dev);
	cleanup_ret = aw_cali_svc_dev_cali_mode_en(aw_dev,
							 CALI_TYPE_F0, false, flag);
	if (!ret && cleanup_ret < 0)
		ret = cleanup_ret;
	aw_cali_svc_run_mute(aw_dev, aw_dev->cali_desc.cali_result);
	return ret;
}

static int aw_cali_svc_devs_cali_f0(struct aw_device *aw_dev, unsigned int flag)
{
	struct aw_device_list dev_list;
	int ret, cleanup_ret;
	bool enabled = false;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, " get dev list failed");
		return ret;
	}

	ret = aw_cali_svc_devs_cali_mode_enable(&dev_list, CALI_TYPE_F0, flag, true);
	if (ret < 0)
		goto out;
	enabled = true;

	/*wait time*/
	msleep(5 * 1000);

	ret = aw_cali_svc_devs_get_cali_cali_data(&dev_list, CALI_DATA_F0);
out:
	if (enabled) {
		cleanup_ret = aw_cali_svc_devs_cali_mode_enable(&dev_list,
							  CALI_TYPE_F0, flag, false);
		if (!ret && cleanup_ret < 0)
			ret = cleanup_ret;
	}
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_cali_f0(struct aw_device *aw_dev, bool is_single, unsigned int flag)
{
	if (is_single)
		return aw_cali_svc_dev_cali_f0(aw_dev, flag);
	else
		return aw_cali_svc_devs_cali_f0(aw_dev, flag);
	return 0;
}

static int aw_cali_svc_dev_cali_f0_q(struct aw_device *aw_dev, unsigned int flag)
{
	int ret, cleanup_ret;

	aw_cali_svc_run_mute(aw_dev, CALI_RESULT_NORMAL);

	ret = aw_cali_svc_dev_cali_mode_en(aw_dev, CALI_TYPE_F0, true, flag);
	if (ret < 0)
		return ret;


	/*wait time*/
	msleep(5 * 1000);

	/*dev get cali f0 q*/
	ret = aw_cali_svc_dev_get_f0_q(aw_dev);
	cleanup_ret = aw_cali_svc_dev_cali_mode_en(aw_dev,
							 CALI_TYPE_F0, false, flag);
	if (!ret && cleanup_ret < 0)
		ret = cleanup_ret;
	aw_cali_svc_run_mute(aw_dev, aw_dev->cali_desc.cali_result);
	return ret;

}

static int aw_cali_svc_devs_cali_f0_q(struct aw_device *aw_dev, unsigned int flag)
{
	struct aw_device_list dev_list;
	int ret, cleanup_ret;
	bool enabled = false;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed ");
		return ret;
	}

	ret = aw_cali_svc_devs_cali_mode_enable(&dev_list, CALI_TYPE_F0, flag, true);
	if (ret < 0)
		goto out;
	enabled = true;

	/*wait time*/
	msleep(5 * 1000);

	/*dev get cali f0 q*/
	ret = aw_cali_svc_devs_get_cali_cali_data(&dev_list, CALI_DATA_F0_Q);
out:
	if (enabled) {
		cleanup_ret = aw_cali_svc_devs_cali_mode_enable(&dev_list,
							  CALI_TYPE_F0, flag, false);
		if (!ret && cleanup_ret < 0)
			ret = cleanup_ret;
	}
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_cali_f0_q(struct aw_device *aw_dev, bool is_single, unsigned int flag)
{
	if (is_single)
		return aw_cali_svc_dev_cali_f0_q(aw_dev, flag);
	else
		return aw_cali_svc_devs_cali_f0_q(aw_dev, flag);

	return 0;
}

static int aw_cali_svc_cali_re_f0(struct aw_device *aw_dev, bool is_single, unsigned int flag)
{
	int ret;

	ret = aw_cali_svc_cali_re(aw_dev, is_single, flag);
	if (ret)
		return ret;

	ret = aw_cali_svc_cali_f0(aw_dev, is_single, flag);
	if (ret)
		return ret;

	return 0;
}

static int aw_cali_svc_cali_re_f0_q(struct aw_device *aw_dev, bool is_single, unsigned int flag)
{
	int ret;

	ret = aw_cali_svc_cali_re(aw_dev, is_single, flag);
	if (ret)
		return ret;

	ret = aw_cali_svc_cali_f0_q(aw_dev, is_single, flag);
	if (ret)
		return ret;

	return 0;
}

static int aw_cali_svc_cali_cmd(struct aw_device *aw_dev, int cali_cmd,
				bool is_single, unsigned int flag)
{
	int ret;

	mutex_lock(&g_cali_dsp_lock);
	switch (cali_cmd) {
	case AW_CALI_CMD_RE:
		ret = aw_cali_svc_cali_re(aw_dev, is_single, flag);
		break;
	case AW_CALI_CMD_F0:
		ret = aw_cali_svc_cali_f0(aw_dev, is_single, flag);
		break;
	case AW_CALI_CMD_F0_Q:
		ret = aw_cali_svc_cali_f0_q(aw_dev, is_single, flag);
		break;
	case AW_CALI_CMD_RE_F0:
		ret = aw_cali_svc_cali_re_f0(aw_dev, is_single, flag);
		break;
	case AW_CALI_CMD_RE_F0_Q:
		ret = aw_cali_svc_cali_re_f0_q(aw_dev, is_single, flag);
		break;
	default:
		aw_dev_err(aw_dev->dev, "unsupported cmd %d", cali_cmd);
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&g_cali_dsp_lock);
	return ret;
}

static int aw_cali_svc_get_devs_cali_re(struct aw_device *aw_dev,
					int32_t *re_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			aw_dev_err(aw_dev->dev,
				   "channel num[%d] overflow buf num[%d]",
				   local_dev->channel, num);
			ret = -EINVAL;
			goto out;
		}
		re_buf[local_dev->channel] = local_dev->cali_desc.cali_re;
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_get_devs_r0(struct aw_device *aw_dev,
				   int32_t *re_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			ret = -EINVAL;
			goto out;
		}
		ret = aw882xx_dsp_read_r0(local_dev,
					  &re_buf[local_dev->channel]);
		if (ret) {
			aw_dev_err(local_dev->dev, "get re failed!");
			goto out;
		}
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_get_devs_te(struct aw_device *aw_dev,
				   int32_t *te_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			ret = -EINVAL;
			goto out;
		}
		ret = aw882xx_dsp_read_te(local_dev,
					  &te_buf[local_dev->channel]);
		if (ret) {
			aw_dev_err(local_dev->dev, "get te failed!");
			goto out;
		}
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_get_devs_st(struct aw_device *aw_dev,
				   int32_t *st_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			ret = -EINVAL;
			goto out;
		}
		ret = aw882xx_dsp_read_st(local_dev,
					  &st_buf[local_dev->channel << 1],
					  &st_buf[(local_dev->channel << 1) + 1]);
		if (ret) {
			aw_dev_err(local_dev->dev, "get speaker status failed!");
			goto out;
		}
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_get_devs_cali_f0(struct aw_device *aw_dev,
					int32_t *f0_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			ret = -EINVAL;
			goto out;
		}
		f0_buf[local_dev->channel] = local_dev->cali_desc.cali_f0;
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_get_devs_f0(struct aw_device *aw_dev,
				   int32_t *f0_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			ret = -EINVAL;
			goto out;
		}
		ret = aw882xx_dsp_read_f0(local_dev,
					  &f0_buf[local_dev->channel]);
		if (ret) {
			aw_dev_err(local_dev->dev, "get f0 failed!");
			goto out;
		}
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_get_devs_cali_f0_q(struct aw_device *aw_dev,
				int32_t *f0_buf, int32_t *q_buf, int num)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	unsigned int i;
	int ret, span = 0;

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= num) {
			ret = -EINVAL;
			goto out;
		}
		f0_buf[local_dev->channel] = local_dev->cali_desc.cali_f0;
		q_buf[local_dev->channel] = local_dev->cali_desc.cali_q;
		span = max(span, (int)local_dev->channel + 1);
	}

	ret = span;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static int aw_cali_svc_set_devs_re_str(struct aw_device *aw_dev, const char *re_str)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	int re_data[AW_DEV_CH_MAX] = {0};
	unsigned int i;
	int ret, parsed, cnt = 0;

	parsed = sscanf(re_str,
			"pri_l:%d pri_r:%d sec_l:%d sec_r:%d "
			"tert_l:%d tert_r:%d quat_l:%d quat_r:%d",
						&re_data[AW_DEV_CH_PRI_L],
					&re_data[AW_DEV_CH_PRI_R],
					&re_data[AW_DEV_CH_SEC_L],
					&re_data[AW_DEV_CH_SEC_R],
					&re_data[AW_DEV_CH_TERT_L],
					&re_data[AW_DEV_CH_TERT_R],
					&re_data[AW_DEV_CH_QUAT_L],
					&re_data[AW_DEV_CH_QUAT_R]);

	if (parsed <= 0) {
		aw_dev_err(aw_dev->dev, "unsupport str[%s]", re_str);
		return -EINVAL;
	}

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed");
		return ret;
	}

	aw_dev_dbg(aw_dev->dev, "pri_l:%d pri_r:%d sec_l:%d sec_r:%d tert_l:%d tert_r:%d quat_l:%d quat_r:%d",
					re_data[AW_DEV_CH_PRI_L],
					re_data[AW_DEV_CH_PRI_R],
					re_data[AW_DEV_CH_SEC_L],
					re_data[AW_DEV_CH_SEC_R],
					re_data[AW_DEV_CH_TERT_L],
					re_data[AW_DEV_CH_TERT_R],
					re_data[AW_DEV_CH_QUAT_L],
					re_data[AW_DEV_CH_QUAT_R]);

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		if (local_dev->channel >= parsed ||
		    re_data[local_dev->channel] < local_dev->re_min ||
		    re_data[local_dev->channel] > local_dev->re_max) {
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < dev_list.count; i++) {
		local_dev = dev_list.devices[i];
		ret = aw_cali_store_cali_re(local_dev,
					     re_data[local_dev->channel]);
		if (ret < 0)
			goto out;
		cnt++;
	}
	ret = cnt;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;

}

static int aw_cali_svc_get_cmd_form_str(struct aw_device *aw_dev, const char *buf)
{
	size_t cmd_len;
	int i;

	for (i = 0; i < CALI_STR_MAX; i++) {
		cmd_len = strlen(cali_str[i]);
		if (!strncmp(cali_str[i], buf, cmd_len) &&
		    (!buf[cmd_len] || buf[cmd_len] == ':' ||
			     isspace((unsigned char)buf[cmd_len])))
			break;

	}

	if (i == CALI_STR_MAX) {
		aw_dev_err(aw_dev->dev, "supported cmd [%s]!", buf);
		return -EINVAL;
	}

	aw_dev_info(aw_dev->dev, "find str [%s]", cali_str[i]);
	return i;
}

static const char *aw_cali_svc_get_cmd_payload(const char *buf, int cmd)
{
	const char *payload = buf + strlen(cali_str[cmd]);

	if (*payload != ':' && !isspace((unsigned char)*payload))
		return ERR_PTR(-EINVAL);
	while (*payload && (*payload == ':' ||
			    isspace((unsigned char)*payload)))
		payload++;
	if (!*payload)
		return ERR_PTR(-EINVAL);

	return payload;
}

/*********************************aw_cali_sevice*********************************************************/


/*****************************attr   start***************************************************/
static ssize_t cali_time_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	uint32_t time;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;

	ret = kstrtouint(buf, 0, &time);
	if (ret < 0) {
		aw_dev_err(aw_dev->dev, "read buf %s failed\n",
			buf);
		return ret;
	}

	if (time < 400 || time > AW_CALI_RE_MAX_TIMER) {
		aw_dev_err(aw_dev->dev, "time:%u outside supported range",
			   time);
		return -EINVAL;
	}

	g_cali_re_time = time;
	aw_dev_dbg(aw_dev->dev, "time:%u", time);

	return count;
}

static ssize_t cali_time_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += scnprintf(buf+len, PAGE_SIZE-len,
		"time: %d\n", g_cali_re_time);

	return len;
}

static ssize_t cali_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;

	ret = aw_cali_svc_get_cmd_form_str(aw_dev, buf);
	if (ret < 0) {
		aw_dev_err(aw_dev->dev, "supported cmd [%s]!", buf);
		return ret;
	}

	switch (ret) {
	case CALI_STR_CALI_RE_F0:
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_RE_F0,
			false, CALI_OPS_HMUTE|CALI_OPS_NOISE);
		break;
	case CALI_STR_CALI_RE:
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_RE,
			false, CALI_OPS_HMUTE);
		break;
	case CALI_STR_CALI_F0:
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_F0,
			false, CALI_OPS_NOISE);
		break;
	default:
		aw_dev_err(aw_dev->dev, "supported cmd [%s]!", buf);
		return -EINVAL;
	}

	return ret < 0 ? ret : count;
}

static ssize_t cali_re_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;

	ret = aw_cali_svc_set_devs_re_str(aw_dev, buf);
	if (ret <= 0) {
		aw_dev_err(aw_dev->dev, "set re str %s failed", buf);
		return ret < 0 ? ret : -EINVAL;
	}

	return count;
}

static ssize_t cali_re_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int ret, i;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;
	ssize_t len = 0;
	int32_t cali_re[AW_DEV_CH_MAX] = {0};

	ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_RE, false, CALI_OPS_HMUTE);
	if (ret < 0) {
		len += scnprintf(buf+len, PAGE_SIZE-len, "cali Re cmd failed\n");
		return len;
	}

	ret = aw_cali_svc_get_devs_cali_re(aw_dev, cali_re, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(aw_dev->dev, "get re failed ");
		len += scnprintf(buf+len, PAGE_SIZE-len, "get re failed\n");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%s:%d mOhms ", ch_name[i], cali_re[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	return len;
}

static ssize_t cali_f0_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;

	ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_F0, false,
					   CALI_OPS_NOISE);
	return ret < 0 ? ret : count;
}

static ssize_t cali_f0_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int ret, i;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;
	ssize_t len = 0;
	int32_t cali_f0[AW_DEV_CH_MAX] = {0};

	ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_F0, false, CALI_OPS_NOISE);
	if (ret < 0) {
		len += scnprintf(buf+len, PAGE_SIZE-len, "cali f0 cmd failed\n");
		return len;
	}

	ret = aw_cali_svc_get_devs_cali_f0(aw_dev, cali_f0, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(aw_dev->dev, "get f0 failed ");
		len += scnprintf(buf+len, PAGE_SIZE-len, "get f0 failed\n");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf+len, PAGE_SIZE-len, "%s:%d ", ch_name[i], cali_f0[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	return len;
}

static ssize_t re_show_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int ret, i;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;
	ssize_t len = 0;
	int32_t cali_re[AW_DEV_CH_MAX] = {0};

	ret = aw_cali_svc_get_devs_cali_re(aw_dev, cali_re, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(aw_dev->dev, "get re failed ");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%s:%d mOhms ", ch_name[i], cali_re[i]);
	}
	len += scnprintf(buf+len, PAGE_SIZE-len, "\n");

	return len;
}

static ssize_t f0_show_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int ret, i;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;
	ssize_t len = 0;
	int32_t cali_f0[AW_DEV_CH_MAX] = {0};

	ret = aw_cali_svc_get_devs_cali_f0(aw_dev, cali_f0, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(aw_dev->dev, "get f0 failed");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf+len, PAGE_SIZE-len, "%s:%d", ch_name[i], cali_f0[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	return len;
}

static ssize_t re_range_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw882xx *aw882xx = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw882xx->aw_pa;
	uint32_t range_buf[RE_RANGE_NUM] = { 0 };

	aw_cali_svc_get_dev_re_range(aw_dev, range_buf);

	len += scnprintf(buf + len, PAGE_SIZE - len,
		"re_min value: [%d] mOhms\n", range_buf[RE_MIN_FLAG]);
	len += scnprintf(buf + len, PAGE_SIZE - len,
		"re_max value: [%d] mOhms\n", range_buf[RE_MAX_FLAG]);

	return len;
}

/*set cali time*/
static DEVICE_ATTR(cali_time, 0600, cali_time_show, cali_time_store);
/*start cali*/
static DEVICE_ATTR_WO(cali);
/*cali re*/
static DEVICE_ATTR(cali_re, 0600, cali_re_show, cali_re_store);
/*cali_f0*/
static DEVICE_ATTR(cali_f0, 0600, cali_f0_show, cali_f0_store);
/*show cali_re*/
static DEVICE_ATTR_RO(re_show);
/*show cali_f0*/
static DEVICE_ATTR_RO(f0_show);
/*show re_range*/
static DEVICE_ATTR_RO(re_range);

static struct attribute *aw_cali_attr[] = {
	&dev_attr_cali_time.attr,
	&dev_attr_cali.attr,
	&dev_attr_cali_re.attr,
	&dev_attr_cali_f0.attr,
	&dev_attr_re_show.attr,
	&dev_attr_f0_show.attr,
	&dev_attr_re_range.attr,
	NULL
};

static struct attribute_group aw_cali_attr_group = {
	.attrs = aw_cali_attr
};

static int aw_cali_attr_init(struct aw_device *aw_dev)
{
	int ret;

	aw_dev_info(aw_dev->dev, "enter");

	ret = sysfs_create_group(&aw_dev->dev->kobj, &aw_cali_attr_group);
	if (ret < 0) {
		aw_dev_info(aw_dev->dev, "error creating sysfs cali attr files");
		return ret;
	}

	return 0;
}

static void aw_cali_attr_deinit(struct aw_device *aw_dev)
{
	sysfs_remove_group(&aw_dev->dev->kobj, &aw_cali_attr_group);
	aw_dev_info(aw_dev->dev, "attr files deinit");
}

/*****************************attr   end***************************************************/

/*****************************class node******************************************************/
static ssize_t aw_cali_class_time_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += scnprintf(buf+len, PAGE_SIZE-len,
		"time: %d\n", g_cali_re_time);

	return len;
}

static ssize_t aw_cali_class_time_store(const struct class *class,
					const struct class_attribute *attr, const char *buf, size_t len)
{
	int ret;
	uint32_t time;

	ret = kstrtouint(buf, 0, &time);
	if (ret < 0) {
		aw_pr_err("read buf %s failed", buf);
		return ret;
	}

	if (time < 400 || time > AW_CALI_RE_MAX_TIMER) {
		aw_pr_err("time:%u outside supported range", time);
		return -EINVAL;
	}

	g_cali_re_time = time;
	aw_pr_dbg("time:%d", time);

	return len;
}

static ssize_t aw_cali_class_cali_re_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
	int ret, i;
	struct aw_device *local_dev;
	ssize_t len = 0;
	int32_t cali_re[AW_DEV_CH_MAX] = {0};

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;

	ret = aw_cali_svc_cali_cmd(local_dev, AW_CALI_CMD_RE, false, CALI_OPS_HMUTE);
	if (ret < 0) {
		len += scnprintf(buf+len, PAGE_SIZE-len, "cali Re cmd failed\n");
		aw882xx_dev_put(local_dev);
		return len;
	}

	ret = aw_cali_svc_get_devs_cali_re(local_dev, cali_re, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(local_dev->dev, "get re failed");
		len += scnprintf(buf+len, PAGE_SIZE-len, "get re failed\n");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%s:%d mOhms ", ch_name[i], cali_re[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	aw882xx_dev_put(local_dev);
	return len;
}

static ssize_t aw_cali_class_cali_re_store(const struct class *class,
					const struct class_attribute *attr, const char *buf, size_t len)
{
	int ret;
	struct aw_device *local_dev;

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;

	ret = aw_cali_svc_set_devs_re_str(local_dev, buf);
	if (ret <= 0) {
		aw_dev_err(local_dev->dev, "set re str %s failed", buf);
		aw882xx_dev_put(local_dev);
		return ret < 0 ? ret : -EINVAL;
	}

	aw882xx_dev_put(local_dev);
	return len;
}

static ssize_t aw_cali_class_cali_f0_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
	int ret, i;
	struct aw_device *local_dev;
	ssize_t len = 0;
	int32_t cali_f0[AW_DEV_CH_MAX] = {0};

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;

	ret = aw_cali_svc_cali_cmd(local_dev, AW_CALI_CMD_F0, false, CALI_OPS_NOISE);
	if (ret < 0) {
		len += scnprintf(buf+len, PAGE_SIZE-len, "cali f0 cmd failed\n");
		aw882xx_dev_put(local_dev);
		return len;
	}

	ret = aw_cali_svc_get_devs_cali_f0(local_dev, cali_f0, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(local_dev->dev, "get f0 failed ");
		len += scnprintf(buf+len, PAGE_SIZE-len, "get f0 failed\n");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf+len, PAGE_SIZE-len, "%s:%d ", ch_name[i], cali_f0[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	aw882xx_dev_put(local_dev);
	return len;
}

static ssize_t aw_cali_class_cali_f0_store(const struct class *class,
				const struct class_attribute *attr, const char *buf, size_t len)
{
	int ret;
	struct aw_device *local_dev;

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;

	ret = aw_cali_svc_cali_cmd(local_dev, AW_CALI_CMD_F0, false,
					   CALI_OPS_NOISE);

	aw882xx_dev_put(local_dev);
	return ret < 0 ? ret : len;
}

static ssize_t aw_cali_class_f0_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
	int ret, i;
	struct aw_device *local_dev;
	ssize_t len = 0;
	int32_t cali_f0[AW_DEV_CH_MAX] = {0};

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;

	ret = aw_cali_svc_get_devs_cali_f0(local_dev, cali_f0, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(local_dev->dev, "get re failed");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf+len, PAGE_SIZE-len, "%s:%d", ch_name[i], cali_f0[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	aw882xx_dev_put(local_dev);
	return len;
}

static ssize_t aw_cali_class_re_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
	int ret, i;
	struct aw_device *local_dev;
	ssize_t len = 0;
	int32_t cali_re[AW_DEV_CH_MAX] = {0};

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;

	ret = aw_cali_svc_get_devs_cali_re(local_dev, cali_re, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(local_dev->dev, "get re failed");
	} else {
		for (i = 0; i < ret; i++)
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%s:%d mOhms", ch_name[i], cali_re[i]);
		len += scnprintf(buf+len, PAGE_SIZE-len, "\n");
	}

	aw882xx_dev_put(local_dev);
	return len;
}

static ssize_t aw_class_re_range_show(const struct class *class, const struct class_attribute *attr, char *buf)
{
	int ret, i;
	ssize_t len = 0;
	struct aw_device *local_dev;
	uint32_t re_value[AW_DEV_RE_RANGE] = { 0 };

	aw_pr_info("enter");

	local_dev = aw882xx_dev_get_first();
	if (!local_dev)
		return -ENODEV;
	ret = aw_cali_svc_get_devs_re_range(local_dev, re_value, AW_DEV_CH_MAX);
	if (ret <= 0) {
		aw_dev_err(local_dev->dev, "get re range failed");
		aw882xx_dev_put(local_dev);
		return -EINVAL;
	}

	for (i = 0; i < ret; i++) {
		len += scnprintf(buf+len, PAGE_SIZE-len, "%s:re_min:%d mOhms re_max:%d mOhms ",
			ch_name[i], re_value[RE_MIN_FLAG + i * RE_RANGE_NUM],
			re_value[RE_MAX_FLAG + i * RE_RANGE_NUM]);
	}
	len += scnprintf(buf+len, PAGE_SIZE-len, "\n");

	aw882xx_dev_put(local_dev);
	return len;
}

static struct class_attribute class_attr_cali_time =
		__ATTR(cali_time, 0600,
		aw_cali_class_time_show, aw_cali_class_time_store);

static struct class_attribute class_attr_re25_calib =
		__ATTR(re25_calib, 0600,
		aw_cali_class_cali_re_show, aw_cali_class_cali_re_store);

static struct class_attribute class_attr_f0_calib =
		__ATTR(f0_calib, 0600,
		aw_cali_class_cali_f0_show, aw_cali_class_cali_f0_store);

static struct class_attribute class_attr_re_show =
		__ATTR(re_show, 0444,
		aw_cali_class_re_show, NULL);

static struct class_attribute class_attr_f0_show =
		__ATTR(f0_show, 0444,
		aw_cali_class_f0_show, NULL);

static struct class_attribute class_att_re_range =
		__ATTR(re_range, 0444,
		aw_class_re_range_show, NULL);

static struct class aw_cali_class = {
	.name = "smartpa",
};

static struct class_attribute *aw_cali_class_attrs[] = {
	&class_attr_re25_calib,
	&class_attr_f0_calib,
	&class_attr_cali_time,
	&class_attr_re_show,
	&class_attr_f0_show,
	&class_att_re_range,
};

static int aw_cali_class_attr_init(struct aw_device *aw_dev)
{
	int ret;
	int i;

	mutex_lock(&g_cali_lock);
	if (g_class_users) {
		g_class_users++;
		mutex_unlock(&g_cali_lock);
		return 0;
	}

	ret = class_register(&aw_cali_class);
	if (ret < 0) {
		aw_dev_info(aw_dev->dev, "error creating class node");
		goto out_unlock;
	}

	for (i = 0; i < ARRAY_SIZE(aw_cali_class_attrs); i++) {
		ret = class_create_file(&aw_cali_class, aw_cali_class_attrs[i]);
		if (ret)
			goto err_files;
	}

	g_class_users = 1;
	mutex_unlock(&g_cali_lock);
	return 0;

err_files:
	while (i--)
		class_remove_file(&aw_cali_class, aw_cali_class_attrs[i]);
	class_unregister(&aw_cali_class);
out_unlock:
	mutex_unlock(&g_cali_lock);
	return ret;
}

static void aw_cali_class_attr_deinit(struct aw_device *aw_dev)
{
	int i;

	mutex_lock(&g_cali_lock);
	if (WARN_ON(!g_class_users)) {
		mutex_unlock(&g_cali_lock);
		return;
	}
	if (--g_class_users) {
		mutex_unlock(&g_cali_lock);
		return;
	}

	for (i = ARRAY_SIZE(aw_cali_class_attrs) - 1; i >= 0; i--)
		class_remove_file(&aw_cali_class, aw_cali_class_attrs[i]);
	class_unregister(&aw_cali_class);
	mutex_unlock(&g_cali_lock);
	aw_dev_info(aw_dev->dev, "unregister class node");
}
/*****************************class node******************************************************/

/*****************************misc node******************************************************/
static int aw_cali_validate_dsp_msg(u32 msg_id, int data_len, bool write)
{
	unsigned int expected_len = 0;
	unsigned int max_len = 0;
	bool allow_read = false;
	bool allow_write = false;

	switch (msg_id) {
	case 0x00000001: /* AW_MSG_ID_ENABLE_CALI */
	case 0x00000002: /* AW_MSG_ID_ENABLE_HMUTE */
	case 0x0000000B: /* AW_MSG_ID_AUDIO_MIX */
		allow_write = true;
		expected_len = sizeof(int32_t);
		break;
	case 0x00000003: /* AW_MSG_ID_F0_Q */
		allow_read = true;
		expected_len = 4 * sizeof(int32_t);
		break;
	case 0x00000006: /* AW_MSG_ID_DIRECT_CUR_FLAG */
		allow_read = true;
		expected_len = 2 * sizeof(int32_t);
		break;
	case 0x00000007: /* AW_MSG_ID_SPK_STATUS */
		allow_read = true;
		expected_len = 8 * sizeof(int32_t);
		break;
	case 0x10013D21: /* AW_MSG_ID_REAL_DATA_L */
	case 0x10013D22: /* AW_MSG_ID_REAL_DATA_R */
		allow_read = true;
		expected_len = AW_CALI_DATA_NUM * sizeof(int32_t);
		break;
	case 0x00000008: /* AW_MSG_ID_VERSION */
	case 0x10013D1B: /* AW_MSG_ID_RE_L */
	case 0x10013D1C: /* AW_MSG_ID_RE_R */
	case 0x10013D17: /* AW_MSG_ID_VMAX_L */
	case 0x10013D18: /* AW_MSG_ID_VMAX_R */
	case 0x0000001F: /* AW_MSG_ID_VOLTAGE_OFFSET */
		allow_read = true;
		allow_write = msg_id != 0x00000008;
		expected_len = sizeof(int32_t);
		break;
	case 0x00000012: /* AW_MSG_ID_VERSION_NEW */
		allow_read = true;
		expected_len = ALGO_VERSION_MAX;
		break;
	case 0x10013D19: /* AW_MSG_ID_CALI_CFG_L */
	case 0x10013D1A: /* AW_MSG_ID_CALI_CFG_R */
		allow_read = true;
		allow_write = true;
		expected_len = AW_CALI_CFG_NUM * sizeof(int32_t);
		break;
	case 0x10013D1D: /* AW_MSG_ID_NOISE_L */
	case 0x10013D1E: /* AW_MSG_ID_NOISE_R */
		allow_write = true;
		expected_len = sizeof(int32_t);
		break;
	case 0x10013D1F: /* AW_MSG_ID_F0_L */
	case 0x10013D20: /* AW_MSG_ID_F0_R */
		allow_read = true;
		expected_len = sizeof(int32_t);
		break;
	case 0x10013D12: /* AW_MSG_ID_PARAMS */
	case 0x10013D2D: /* AW_MSG_ID_PARAMS_1 */
	case 0x10013D41: /* AW_MSG_ID_PARAMS_2 */
	case 0x10013D35: /* AW_MSG_ID_PARAMS_3 */
		allow_write = true;
		max_len = sizeof(struct params_data);
		break;
	default:
		return -EINVAL;
	}

	if ((write && !allow_write) || (!write && !allow_read))
		return -EPERM;
	if (data_len <= 0)
		return -EMSGSIZE;
	if (max_len && (data_len > max_len ||
			!IS_ALIGNED(data_len, sizeof(int32_t))))
		return -EMSGSIZE;
	if (!max_len && data_len != expected_len)
		return -EMSGSIZE;
	if (write && !capable(CAP_SYS_RAWIO))
		return -EPERM;

	return 0;
}

static int aw_cali_misc_open(struct inode *inode, struct file *file)
{
	struct aw_cali_file_ctx *ctx;
	struct aw_device *aw_dev;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	aw_dev = aw882xx_dev_get_by_channel(AW_DEV_CH_PRI_L);
	if (!aw_dev)
		aw_dev = aw882xx_dev_get_first();
	if (!aw_dev) {
		kfree(ctx);
		return -ENODEV;
	}

	ctx->channel = aw_dev->channel;
	ctx->pending_cmd = CALI_STR_NONE;
	mutex_init(&ctx->lock);
	file->private_data = ctx;
	aw_dev_dbg(aw_dev->dev, "misc open success");
	aw882xx_dev_put(aw_dev);
	return 0;
}

static int aw_cali_misc_release(struct inode *inode, struct file *file)
{
	struct aw_cali_file_ctx *ctx = file->private_data;

	file->private_data = NULL;
	kfree(ctx);

	aw_pr_dbg("misc release success");
	return 0;
}

static int aw_cali_misc_params_ptr(struct aw_device *aw_dev, struct ptr_params_data *p_params)
{
	int ret = 0;
	char *p_data = NULL;

	if (p_params->data == NULL || p_params->len <= 0 ||
	    p_params->len > sizeof(struct params_data) ||
	    !IS_ALIGNED(p_params->len, sizeof(int32_t))) {
		aw_dev_err(aw_dev->dev, "p_params error");
		ret = -EMSGSIZE;
		return ret;
	}

	p_data = kzalloc(p_params->len, GFP_KERNEL);
	if (p_data == NULL) {
		ret = -ENOMEM;
		goto exit;
	}

	if (copy_from_user(p_data,
			(void __user *)p_params->data,
			p_params->len)) {
		ret = -EFAULT;
		goto exit;
	}

	ret = aw882xx_dsp_write_params(aw_dev, p_data, p_params->len);
	if (ret < 0) {
		aw_dev_err(aw_dev->dev, "write params failed ");
		goto exit;
	}

exit:
	if (p_data != NULL) {
		kfree(p_data);
		p_data = NULL;
	}

	return ret;
}

static int aw_cali_misc_ops_write(struct aw_device *aw_dev,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int data_len = _IOC_SIZE(cmd);
	char *data_ptr = NULL;
	int32_t data = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	data_ptr = kzalloc(data_len, GFP_KERNEL);
	if (!data_ptr)
		return -ENOMEM;


	if (copy_from_user(data_ptr, (void __user *)arg, data_len)) {
		ret = -EFAULT;
		goto exit;
	}

	switch (cmd) {
		case AW_IOCTL_ENABLE_CALI: {
			if (aw_dev->cali_desc.cali_check_st) {
				if (data_ptr[0])
					aw_cali_svc_run_mute(aw_dev, CALI_RESULT_NORMAL);
				else
					aw_cali_svc_run_mute(aw_dev, aw_dev->cali_desc.cali_result);
			}
			aw_cali_svc_set_cali_status(aw_dev, data_ptr[0]);
		} break;
		case AW_IOCTL_SET_CALI_CFG: {
			ret = aw882xx_dsp_write_cali_cfg(aw_dev, data_ptr, data_len);
		} break;
		case AW_IOCTL_SET_NOISE: {
			data = *(int32_t *)data_ptr;
			ret = aw882xx_dsp_noise_en(aw_dev, data);
		} break;
		case AW_IOCTL_SET_VMAX: {
			ret = aw882xx_dsp_write_vmax(aw_dev, data_ptr, data_len);
		} break;
			case AW_IOCTL_SET_PARAM: {
				ret = aw882xx_dsp_write_params(aw_dev, data_ptr, data_len);
			} break;
			case AW_IOCTL_SET_PTR_PARAM_NUM: {
				ret = aw_cali_misc_params_ptr(aw_dev,
					(struct ptr_params_data *)data_ptr);
		} break;
		case AW_IOCTL_SET_CALI_RE: {
			ret = aw_cali_store_cali_re(aw_dev, *((int32_t *)data_ptr));
			/*ret = aw882xx_dsp_write_cali_re(aw_dev, *(int32_t *)data_ptr);*/
		} break;
		case AW_IOCTL_SET_DSP_HMUTE: {
			data = *(int32_t *)data_ptr;
			ret = aw882xx_dsp_hmute_en(aw_dev, data);
		} break;
		case AW_IOCTL_SET_CALI_CFG_FLAG: {
			data = *(int32_t *)data_ptr;
			/*aw_cali_svc_set_cali_status(aw_dev, data);*/
			ret = aw882xx_dsp_cali_en(aw_dev, data);
		} break;
			default:{
			aw_dev_err(aw_dev->dev, "unsupported  cmd %d", cmd);
			ret = -EINVAL;
		} break;
	}

exit:
	kfree(data_ptr);
	return ret;
}

static int aw_cali_misc_ops_read(struct aw_device *aw_dev,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int data_len = _IOC_SIZE(cmd);
	char *data_ptr = NULL;
	int32_t *data_32_ptr = NULL;

	data_ptr = kzalloc(data_len, GFP_KERNEL);
	if (!data_ptr)
		return -ENOMEM;


	switch (cmd) {
		case AW_IOCTL_GET_CALI_CFG: {
			ret = aw882xx_dsp_read_cali_cfg(aw_dev, data_ptr, data_len);
		} break;
		case AW_IOCTL_GET_CALI_DATA: {
			ret = aw882xx_dsp_read_cali_data(aw_dev, data_ptr, data_len);
		} break;
		case AW_IOCTL_GET_F0: {
			data_32_ptr = (int32_t *)data_ptr;
			ret = aw882xx_dsp_read_f0(aw_dev, data_32_ptr);
		} break;
		case AW_IOCTL_GET_CALI_RE: {
			data_32_ptr = (int32_t *)data_ptr;
			ret = aw882xx_dsp_read_cali_re(aw_dev, data_32_ptr);
		} break;
		case AW_IOCTL_GET_VMAX: {
			ret = aw882xx_dsp_read_vmax(aw_dev, data_ptr, data_len);
		} break;
		case AW_IOCTL_GET_F0_Q: {
			data_32_ptr = (int32_t *)data_ptr;
			ret = aw882xx_dsp_read_f0_q(aw_dev, &data_32_ptr[0], &data_32_ptr[1]);
		} break;
		case AW_IOCTL_GET_RE_RANGE: {
			data_32_ptr = (uint32_t *)data_ptr;
			ret = aw_cali_svc_get_dev_re_range(aw_dev, data_32_ptr);
		} break;
		default:{
			aw_dev_err(aw_dev->dev, "unsupported  cmd %d", cmd);
			ret = -EINVAL;
		} break;
		}
	if (ret)
		goto out;

	if (copy_to_user((void __user *)arg,
		data_ptr, data_len)) {
		ret = -EFAULT;
	}

out:
	kfree(data_ptr);
	return ret;
}

static int aw_cali_misc_read_dsp(struct aw_device *aw_dev,
					 const aw_ioctl_msg_t *msg)
{
	int ret;
	char __user *user_data = (char __user *)msg->data_buf;
	uint32_t dsp_msg_id = (uint32_t)msg->opcode_id;
	int data_len = msg->data_len;
	char *data_ptr = NULL;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (!user_data)
		return -EFAULT;
	ret = aw_cali_validate_dsp_msg(dsp_msg_id, data_len, false);
	if (ret)
		return ret;

	data_ptr = kzalloc(data_len, GFP_KERNEL);
	if (!data_ptr)
		return -ENOMEM;


	ret = aw882xx_dsp_read_dsp_msg(aw_dev, dsp_msg_id, data_ptr, data_len);
	if (ret) {
		aw_dev_err(aw_dev->dev, " write failed");
		goto exit;
	}

	if (copy_to_user((void __user *)user_data,
		data_ptr, data_len))
		ret = -EFAULT;
exit:
	kfree(data_ptr);
	return ret;
}

static int aw_cali_misc_write_dsp(struct aw_device *aw_dev,
				  const aw_ioctl_msg_t *msg)
{
	int ret;
	char __user *user_data = (char __user *)msg->data_buf;
	uint32_t dsp_msg_id = (uint32_t)msg->opcode_id;
	int data_len = msg->data_len;
	char *data_ptr = NULL;

	if (!user_data)
		return -EFAULT;
	ret = aw_cali_validate_dsp_msg(dsp_msg_id, data_len, true);
	if (ret)
		return ret;

	data_ptr = kzalloc(data_len, GFP_KERNEL);
	if (!data_ptr)
		return -ENOMEM;


	if (copy_from_user(data_ptr, (void __user *)user_data, data_len)) {
		aw_dev_err(aw_dev->dev, "copy data failed");
		ret = -EFAULT;
		goto exit;
	}

	ret = aw882xx_dsp_write_dsp_msg(aw_dev, dsp_msg_id, data_ptr, data_len);
	if (ret)
		aw_dev_err(aw_dev->dev, "write failed");

exit:
	kfree(data_ptr);
	return ret;
}


static int aw_cali_misc_ops(struct aw_device *aw_dev,
				unsigned int cmd, unsigned long arg);
static int aw_cali_misc_ops_msg_data(struct aw_device *aw_dev,
					     const aw_ioctl_msg_t *ioctl_msg)
{
	u32 opcode;
	u32 type;

	if (ioctl_msg->type < 0 || ioctl_msg->data_len < 0)
		return -EINVAL;
	if (ioctl_msg->version != AW_IOCTL_MSG_VERSION) {
		aw_dev_err(aw_dev->dev, "unsupported msg version %d", ioctl_msg->version);
		return -EINVAL;
	}

	type = (u32)ioctl_msg->type;
	opcode = (u32)ioctl_msg->opcode_id;
	if (type != AW_IOCTL_MSG_IOCTL && ioctl_msg->opcode_id < 0)
		return -EINVAL;
	if (type == AW_IOCTL_MSG_IOCTL && opcode == AW_IOCTL_MSG)
		return -EINVAL;

	switch (type) {
	case AW_IOCTL_MSG_RD_DSP:
		return aw_cali_misc_read_dsp(aw_dev, ioctl_msg);
	case AW_IOCTL_MSG_WR_DSP:
		return aw_cali_misc_write_dsp(aw_dev, ioctl_msg);
	case AW_IOCTL_MSG_IOCTL:
		return aw_cali_misc_ops(aw_dev, opcode,
				(unsigned long)ioctl_msg->data_buf);
	default:
		aw_dev_err(aw_dev->dev, "unsupported msg type %d", ioctl_msg->type);
		return -EINVAL;
	}
}

static int aw_cali_misc_ops_msg(struct aw_device *aw_dev, unsigned long arg)
{
	aw_ioctl_msg_t ioctl_msg;

	if (copy_from_user(&ioctl_msg, (void __user *)arg, sizeof(ioctl_msg)))
		return -EFAULT;

	return aw_cali_misc_ops_msg_data(aw_dev, &ioctl_msg);
}

static int aw_cali_misc_ops(struct aw_device *aw_dev,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case AW_IOCTL_ENABLE_CALI:
	case AW_IOCTL_SET_CALI_CFG:
	case AW_IOCTL_SET_NOISE:
	case AW_IOCTL_SET_VMAX:
	case AW_IOCTL_SET_PARAM:
	case AW_IOCTL_SET_PTR_PARAM_NUM:
	case AW_IOCTL_SET_CALI_RE:
	case AW_IOCTL_SET_DSP_HMUTE:
	case AW_IOCTL_SET_CALI_CFG_FLAG:
		ret = aw_cali_misc_ops_write(aw_dev, cmd, arg);
		break;
	case AW_IOCTL_GET_CALI_CFG:
	case AW_IOCTL_GET_CALI_DATA:
	case AW_IOCTL_GET_F0:
	case AW_IOCTL_GET_CALI_RE:
	case AW_IOCTL_GET_VMAX:
	case AW_IOCTL_GET_F0_Q:
	case AW_IOCTL_GET_RE_RANGE:
		ret = aw_cali_misc_ops_read(aw_dev, cmd, arg);
		break;
	case AW_IOCTL_MSG: {
		ret = aw_cali_misc_ops_msg(aw_dev, arg);
	} break;
	default:
		aw_dev_err(aw_dev->dev, "unsupported  cmd %d", cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static long aw_cali_misc_unlocked_ioctl(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	struct aw_cali_file_ctx *ctx = file->private_data;
	struct aw_device *aw_dev;
	int ret;

	if (((_IOC_TYPE(cmd)) != (AW_IOCTL_MAGIC))) {
		aw_pr_err("cmd magic err");
		return -EINVAL;
	}
	mutex_lock(&ctx->lock);
	aw_dev = aw882xx_dev_get_by_channel(ctx->channel);
	if (!aw_dev) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}
	mutex_lock(&g_cali_dsp_lock);
	ret = aw_cali_misc_ops(aw_dev, cmd, arg);
	mutex_unlock(&g_cali_dsp_lock);
	aw882xx_dev_put(aw_dev);
	mutex_unlock(&ctx->lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static int aw_cali_misc_compat_params_ptr(struct aw_device *aw_dev,
					  void __user *argp)
{
	struct aw_compat_ptr_params_data compat_params;
	struct ptr_params_data params;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (copy_from_user(&compat_params, argp, sizeof(compat_params)))
		return -EFAULT;

	params.len = compat_params.len;
	params.data = compat_ptr(compat_params.data);
	return aw_cali_misc_params_ptr(aw_dev, &params);
}

static int aw_cali_misc_compat_msg(struct aw_device *aw_dev, void __user *argp)
{
	struct aw_compat_ioctl_msg compat_msg;
	aw_ioctl_msg_t msg;
	u32 opcode;
	u32 type;

	if (copy_from_user(&compat_msg, argp, sizeof(compat_msg)))
		return -EFAULT;
	if (compat_msg.type < 0 || compat_msg.data_len < 0 ||
	    compat_msg.version != AW_IOCTL_MSG_VERSION)
		return -EINVAL;
	type = (u32)compat_msg.type;
	opcode = (u32)compat_msg.opcode_id;
	if (type != AW_IOCTL_MSG_IOCTL && compat_msg.opcode_id < 0)
		return -EINVAL;

	memset(&msg, 0, sizeof(msg));
	msg.type = compat_msg.type;
	msg.opcode_id = compat_msg.opcode_id;
	msg.version = compat_msg.version;
	msg.data_len = compat_msg.data_len;
	msg.data_buf = compat_ptr(compat_msg.data_buf);
	msg.reseriver[0] = compat_msg.reseriver[0];
	msg.reseriver[1] = compat_msg.reseriver[1];

	if (type == AW_IOCTL_MSG_IOCTL &&
	    opcode == AW_IOCTL_SET_PTR_PARAM_NUM32)
		return aw_cali_misc_compat_params_ptr(aw_dev, msg.data_buf);
	if (type == AW_IOCTL_MSG_IOCTL &&
	    (opcode == AW_IOCTL_SET_PTR_PARAM_NUM ||
	     opcode == AW_IOCTL_MSG || opcode == AW_IOCTL_MSG32))
		return -EINVAL;

	return aw_cali_misc_ops_msg_data(aw_dev, &msg);
}

static long aw_cali_misc_compat_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	struct aw_cali_file_ctx *ctx = file->private_data;
	struct aw_device *aw_dev;
	void __user *argp = compat_ptr(arg);
	int ret;

	if (((_IOC_TYPE(cmd)) != (AW_IOCTL_MAGIC))) {
		aw_pr_err("cmd magic err");
		return -EINVAL;
	}
	mutex_lock(&ctx->lock);
	aw_dev = aw882xx_dev_get_by_channel(ctx->channel);
	if (!aw_dev) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}

	mutex_lock(&g_cali_dsp_lock);
	switch (cmd) {
	case AW_IOCTL_SET_PTR_PARAM_NUM32:
		ret = aw_cali_misc_compat_params_ptr(aw_dev, argp);
		break;
	case AW_IOCTL_MSG32:
		ret = aw_cali_misc_compat_msg(aw_dev, argp);
		break;
	case AW_IOCTL_SET_PTR_PARAM_NUM:
	case AW_IOCTL_MSG:
		ret = -EINVAL;
		break;
	default:
		ret = aw_cali_misc_ops(aw_dev, cmd, (unsigned long)argp);
		break;
	}
	mutex_unlock(&g_cali_dsp_lock);

	aw882xx_dev_put(aw_dev);
	mutex_unlock(&ctx->lock);
	return ret;
}
#endif

static ssize_t aw_cali_misc_read(struct file *filp, char __user *buf, size_t size, loff_t *pos)
{
	int i, ret;
	int len = 0;
	struct aw_cali_file_ctx *ctx = filp->private_data;
	struct aw_device *aw_dev;
	char local_buf[256] = { 0 };
	unsigned int dev_num;
	int32_t temp_data[AW_DEV_CH_MAX << 1] = {0};
	int32_t temp_data1[AW_DEV_CH_MAX << 1] = {0};
	uint32_t re_value[AW_DEV_RE_RANGE] = { 0 };

	mutex_lock(&ctx->lock);
	if (ctx->response_len) {
		ret = simple_read_from_buffer(buf, size, pos, ctx->response,
					      ctx->response_len);
		mutex_unlock(&ctx->lock);
		return ret;
	}

	if (ctx->pending_cmd == CALI_STR_NONE) {
		mutex_unlock(&ctx->lock);
		return -EINVAL;
	}

	aw_dev = aw882xx_dev_get_by_channel(ctx->channel);
	if (!aw_dev) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}
	aw_dev_info(aw_dev->dev, "enter");
	mutex_lock(&g_cali_dsp_lock);

	switch (ctx->pending_cmd) {
	case CALI_STR_SHOW_RE: {
		ret = aw_cali_svc_get_devs_cali_re(aw_dev, temp_data, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get cali_re failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf+len, sizeof(local_buf)-len,
						"%s:%d mOhms ", ch_name[i], temp_data[i]);

		len += scnprintf(local_buf+len, sizeof(local_buf)-len, "\n");

	} break;
	case CALI_STR_SHOW_CALI_F0: {
		ret = aw_cali_svc_get_devs_cali_f0(aw_dev, temp_data, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get cali f0 failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf+len, sizeof(local_buf)-len,
						"%s:%d ", ch_name[i], temp_data[i]);

		len += scnprintf(local_buf+len, sizeof(local_buf)-len, "\n");

	} break;
	case CALI_STR_SHOW_R0: {
		ret = aw_cali_svc_get_devs_r0(aw_dev, temp_data, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get r0 failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf+len, sizeof(local_buf)-len,
						"%s:%d mOhms ", ch_name[i], temp_data[i]);
		len += scnprintf(local_buf+len, sizeof(local_buf)-len, "\n");

	} break;
	case CALI_STR_SHOW_TE: {
		ret = aw_cali_svc_get_devs_te(aw_dev, temp_data, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get te failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf+len, sizeof(local_buf)-len,
						"%s:%d ", ch_name[i], temp_data[i]);
		len += scnprintf(local_buf+len, sizeof(local_buf)-len, "\n");

	} break;
	case CALI_STR_SHOW_ST: {
		ret = aw_cali_svc_get_devs_st(aw_dev, temp_data, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get spkr status failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++) {
			len += scnprintf(local_buf+len, sizeof(local_buf)-len,
						"%s:R0 %d mOhms Te %d ",
					ch_name[i], temp_data[i << 1], temp_data[(i << 1) + 1]);
		}
		len += scnprintf(local_buf+len, sizeof(local_buf)-len, "\n");

	} break;
	case CALI_STR_SHOW_F0: {
		ret = aw_cali_svc_get_devs_f0(aw_dev, temp_data, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get f0 failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf+len, sizeof(local_buf)-len,
						"%s:%d ", ch_name[i], temp_data[i]);

		len += scnprintf(local_buf+len, sizeof(local_buf) - len, "\n");

	} break;
	case CALI_STR_VER: {
		if (aw_dev->ops.aw_get_version) {
			ret = aw_dev->ops.aw_get_version(local_buf,
						     sizeof(local_buf));
				if (ret < 0) {
					aw_dev_err(aw_dev->dev, "get version failed");
					ret = -EINVAL;
					goto out_put;
				}
				len = strnlen(local_buf, sizeof(local_buf));
				if (len == sizeof(local_buf)) {
					ret = -EOVERFLOW;
					goto out_put;
				}
			len += scnprintf(local_buf+len, sizeof(local_buf) - len, "\n");
			} else {
				aw_dev_err(aw_dev->dev, "get version is NULL");
				ret = -EINVAL;
				goto out_put;
		}
	} break;
	case CALI_STR_DEV_NUM: {
		if (aw_dev->ops.aw_get_dev_num) {
			dev_num = aw_dev->ops.aw_get_dev_num();
			len += scnprintf(local_buf + len,
					sizeof(local_buf) - len,
					"dev_num:%d\n", dev_num);
			} else {
				aw_dev_err(aw_dev->dev, "get dev num is NULL");
				ret = -EINVAL;
				goto out_put;
		}
		break;
	}
	case CALI_STR_SHOW_F0_Q: {
		ret = aw_cali_svc_get_devs_cali_f0_q(aw_dev,
				temp_data, temp_data1, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get q f0 failed");
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf + len, sizeof(local_buf)-len,
					"%s:f0:%d q:%d", ch_name[i], temp_data[i], temp_data1[i]);

		len += scnprintf(local_buf+len, sizeof(local_buf) - len, "\n");

	} break;
	case CALI_STR_SHOW_RE_RANGE: {
		ret = aw_cali_svc_get_devs_re_range(aw_dev, re_value, AW_DEV_CH_MAX);
			if (ret <= 0) {
				aw_dev_err(aw_dev->dev, "get re range failed");
				ret = -EINVAL;
				goto out_put;
		}
		for (i = 0; i < ret; i++)
			len += scnprintf(local_buf + len, sizeof(local_buf) - len,
				"%s:re_min:%d re_max:%d ",
				ch_name[i], re_value[RE_MIN_FLAG + i * RE_RANGE_NUM],
				re_value[RE_MAX_FLAG + i * RE_RANGE_NUM]);

		len += scnprintf(local_buf + len, sizeof(local_buf) - len, "\n");

	} break;
	default: {
		aw_dev_err(aw_dev->dev, "unsupported flag [%d]", ctx->pending_cmd);
		ret = -EINVAL;
		goto out_put;

	} break;
	}

	ctx->pending_cmd = CALI_STR_NONE;
	memcpy(ctx->response, local_buf, len);
	ctx->response_len = len;
	mutex_unlock(&g_cali_dsp_lock);
	aw882xx_dev_put(aw_dev);
	ret = simple_read_from_buffer(buf, size, pos, ctx->response,
				      ctx->response_len);
	mutex_unlock(&ctx->lock);
	return ret;

out_put:
	mutex_unlock(&g_cali_dsp_lock);
	aw882xx_dev_put(aw_dev);
	mutex_unlock(&ctx->lock);
	return ret;
}

static int aw_cali_misc_switch_dev(struct aw_cali_file_ctx *ctx,
					   struct aw_device *aw_dev, char *cmd_buf)
{
	struct aw_device_list dev_list;
	struct aw_device *local_dev;
	char dev_select[50] = { 0 };
	unsigned int j;
	int ret, i;

	/* get sel dev str */
	ret = sscanf(cmd_buf, "dev_sel:%49s", dev_select);
	if (ret <= 0)
		return -EINVAL;

	for (i = 0; i < AW_DEV_CH_MAX; i++) {
		if (!strcmp(dev_select, ch_name[i]))
			break;
	}

	if (i == AW_DEV_CH_MAX) {
		aw_dev_err(aw_dev->dev, "unsupport dev [%s]", dev_select);
		return -EINVAL;
	}

	ret = aw882xx_dev_get_list(&dev_list);
	if (ret) {
		aw_dev_err(aw_dev->dev, "get dev list failed ");
		return ret;
	}

	for (j = 0; j < dev_list.count; j++) {
		local_dev = dev_list.devices[j];
		if (local_dev->channel == i) {
			ctx->channel = i;
			ctx->pending_cmd = CALI_STR_NONE;
			ctx->response_len = 0;
			aw_dev_info(local_dev->dev, "switch dev to [%s]", ch_name[i]);
			ret = 0;
			goto out;
		}
	}
	aw_dev_err(aw_dev->dev, " unsupport [%s]", dev_select);
	ret = -EINVAL;
out:
	aw882xx_dev_put_list(&dev_list);
	return ret;
}

static ssize_t aw_cali_misc_write(struct file *filp, const char __user *buf,
				  size_t size, loff_t *pos)
{
	int ret = 0;
	char *kernel_buf = NULL;
	const char *payload;
	struct aw_cali_file_ctx *ctx = filp->private_data;
	struct aw_device *aw_dev;

	if (!size || size > PAGE_SIZE)
		return -EMSGSIZE;

	mutex_lock(&ctx->lock);
	aw_dev = aw882xx_dev_get_by_channel(ctx->channel);
	if (!aw_dev) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}

	aw_dev_info(aw_dev->dev, "enter, write size:%d", (int)size);

	kernel_buf = kzalloc(size + 1, GFP_KERNEL);
	if (kernel_buf == NULL) {
		ret = -ENOMEM;
		goto exit;
	}


	if (copy_from_user(kernel_buf,
			(void __user *)buf,
			size)) {
		ret = -EFAULT;
		goto exit;
	}

	ret = aw_cali_svc_get_cmd_form_str(aw_dev, kernel_buf);
	if (ret < 0) {
		aw_dev_err(aw_dev->dev, "upported cmd [%s]! ", kernel_buf);
		ret = -EINVAL;
		goto exit;
	}

	switch (ret) {
	case CALI_STR_CALI_RE_F0: {
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_RE_F0,
					false, CALI_OPS_HMUTE|CALI_OPS_NOISE);
	} break;
	case CALI_STR_CALI_RE: {
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_RE,
					false, CALI_OPS_HMUTE);
	} break;
	case CALI_STR_CALI_F0: {
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_F0,
					false, CALI_OPS_HMUTE|CALI_OPS_NOISE);
	} break;
	case CALI_STR_SET_RE: {
		payload = aw_cali_svc_get_cmd_payload(kernel_buf,
						       CALI_STR_SET_RE);
		if (IS_ERR(payload))
			ret = PTR_ERR(payload);
		else
			ret = aw_cali_svc_set_devs_re_str(aw_dev, payload);
	} break;
	case CALI_STR_DEV_SEL: {
		ret = aw_cali_misc_switch_dev(ctx, aw_dev, kernel_buf);
	} break;
	case CALI_STR_CALI_F0_Q: {
		ret = aw_cali_svc_cali_cmd(aw_dev, AW_CALI_CMD_F0_Q,
					false, CALI_OPS_HMUTE|CALI_OPS_NOISE);
	} break;
	case CALI_STR_SHOW_RE:			/*show cali_re*/
	case CALI_STR_SHOW_R0:			/*show real r0*/
	case CALI_STR_SHOW_CALI_F0:		/*GET DEV CALI_F0*/
	case CALI_STR_SHOW_F0:			/*SHOW REAL F0*/
	case CALI_STR_SHOW_TE:
	case CALI_STR_SHOW_ST:
	case CALI_STR_VER:
	case CALI_STR_DEV_NUM:
	case CALI_STR_SHOW_F0_Q:
	case CALI_STR_SHOW_RE_RANGE:{
		ctx->pending_cmd = ret;
		ctx->response_len = 0;
		*pos = 0;
		ret = 0;
	} break;
	default: {
		aw_dev_err(aw_dev->dev, "unsupported [%s]! ", kernel_buf);
		ret = -EINVAL;
	} break;
	};

exit:
	if (kernel_buf)
		aw_dev_info(aw_dev->dev, "cmd [%s]! ", kernel_buf);

	kfree(kernel_buf);
	kernel_buf = NULL;

	aw882xx_dev_put(aw_dev);
	mutex_unlock(&ctx->lock);
	return ret < 0 ? ret : size;
}

static const struct file_operations aw_cali_misc_fops = {
	.owner = THIS_MODULE,
	.open = aw_cali_misc_open,
	.read = aw_cali_misc_read,
	.write = aw_cali_misc_write,
	.release = aw_cali_misc_release,
	.unlocked_ioctl = aw_cali_misc_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aw_cali_misc_compat_ioctl,
#endif

};

static struct miscdevice misc_cali = {
	.name = "aw882xx_smartpa",
	.minor = MISC_DYNAMIC_MINOR,
	.fops  = &aw_cali_misc_fops,
	.mode = 0600,
};

static int aw_cali_misc_init(struct aw_device *aw_dev)
{
	int ret;

	mutex_lock(&g_cali_lock);
	if (g_misc_users) {
		g_misc_users++;
		mutex_unlock(&g_cali_lock);
		return 0;
	}

	ret = misc_register(&misc_cali);
	if (ret) {
		aw_dev_err(aw_dev->dev, "misc register fail: %d\n", ret);
		mutex_unlock(&g_cali_lock);
		return ret;
	}
	g_misc_users = 1;
	aw_dev_dbg(aw_dev->dev, "misc register success");
	mutex_unlock(&g_cali_lock);

	return 0;
}

static void aw_cali_misc_deinit(struct aw_device *aw_dev)
{
	mutex_lock(&g_cali_lock);
	if (WARN_ON(!g_misc_users)) {
		mutex_unlock(&g_cali_lock);
		return;
	}
	if (--g_misc_users) {
		mutex_unlock(&g_cali_lock);
		return;
	}
	misc_deregister(&misc_cali);
	mutex_unlock(&g_cali_lock);
	aw_dev_dbg(aw_dev->dev, " misc unregister done");
}

/*****************************misc node******************************************************/
static void aw_cali_parse_dt(struct aw_device *aw_dev)
{
	int ret = -1;
	struct device_node *np = aw_dev->dev->of_node;
	const char *cali_mode_str = NULL;
	uint32_t cali_check = CALI_CHECK_DISABLE;
	struct aw_cali_desc *desc = &aw_dev->cali_desc;

	ret = of_property_read_u32(np, "aw-cali-check", &cali_check);
	if (ret < 0) {
		aw_dev_info(aw_dev->dev, "cali-check get failed ,default turn off");
		cali_check = CALI_CHECK_DISABLE;
	}

	desc->cali_check_st = cali_check;
	aw_dev_info(aw_dev->dev, "cali check :%s",
			(desc->cali_check_st) ? "enable" : "disable");

	ret = of_property_read_string(np, "aw-cali-mode", &cali_mode_str);
	if (ret < 0) {
		desc->mode = AW_CALI_MODE_ALL;
		aw_dev_info(aw_dev->dev, "cali function in use");
		return;
	}

	if (!strcmp(cali_mode_str, "none"))
		desc->mode = AW_CALI_MODE_NONE;
	else
		desc->mode = AW_CALI_MODE_ALL;

	aw_dev_info(aw_dev->dev, "cali mode is %d", desc->mode);
}

static int aw_cali_parse_re_dt(struct aw_device *aw_dev)
{
	int ret;

	ret = of_property_read_u32(aw_dev->dev->of_node, "aw-re-min", &aw_dev->re_min);
	if (ret < 0) {
		aw_dev->re_min = AW_CALI_RE_DEFAULT_MIN;
		aw_dev_info(aw_dev->dev, "read aw-re-min failed, use default");
	}

	ret = of_property_read_u32(aw_dev->dev->of_node, "aw-re-max", &aw_dev->re_max);
	if (ret < 0) {
		aw_dev->re_max = AW_CALI_RE_DEFAULT_MAX;
		aw_dev_info(aw_dev->dev, "read aw-re-max failed, use default");
	}

	if (aw_dev->re_min >= aw_dev->re_max) {
		aw_dev_err(aw_dev->dev, "re max must be greater than re min");
		return -EINVAL;
	}

	aw_dev_info(aw_dev->dev, "re min: %d, re max: %d",
					aw_dev->re_min, aw_dev->re_max);
	return 0;
}

int aw882xx_cali_init(struct aw_cali_desc *cali_desc)
{
	int ret;

	struct aw_device *aw_dev =
		container_of(cali_desc, struct aw_device, cali_desc);

	cali_desc->cali_f0 = 0;
	cali_desc->cali_re = 0;
	cali_desc->cali_q = 0;
	cali_desc->status = 0;
	cali_desc->attr_registered = false;
	cali_desc->class_registered = false;
	cali_desc->misc_registered = false;

	aw_cali_parse_dt(aw_dev);
	ret = aw_cali_parse_re_dt(aw_dev);
	if (ret)
		return ret;

	if (cali_desc->mode == AW_CALI_MODE_ALL) {
		ret = aw_cali_attr_init(aw_dev);
		if (ret)
			return ret;
		cali_desc->attr_registered = true;

		ret = aw_cali_class_attr_init(aw_dev);
		if (ret)
			goto err_attr;
		cali_desc->class_registered = true;
	} else {
		aw_dev_info(aw_dev->dev, "cali mode is %d", cali_desc->mode);
	}

	ret = aw_cali_misc_init(aw_dev);
	if (ret)
		goto err_class;
	cali_desc->misc_registered = true;

	cali_desc->cali_result = CALI_RESULT_NONE;

	return 0;

err_class:
	if (cali_desc->class_registered) {
		aw_cali_class_attr_deinit(aw_dev);
		cali_desc->class_registered = false;
	}
err_attr:
	if (cali_desc->attr_registered) {
		aw_cali_attr_deinit(aw_dev);
		cali_desc->attr_registered = false;
	}
	return ret;
}

void aw882xx_cali_deinit(struct aw_cali_desc *cali_desc)
{
	struct aw_device *aw_dev =
		container_of(cali_desc, struct aw_device, cali_desc);

	if (cali_desc->misc_registered) {
		aw_cali_misc_deinit(aw_dev);
		cali_desc->misc_registered = false;
	}

	if (cali_desc->class_registered) {
		aw_cali_class_attr_deinit(aw_dev);
		cali_desc->class_registered = false;
	}

	if (cali_desc->attr_registered) {
		aw_cali_attr_deinit(aw_dev);
		cali_desc->attr_registered = false;
	}
}

