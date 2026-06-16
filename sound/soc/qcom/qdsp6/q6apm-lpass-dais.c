// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2021, Linaro Limited

#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include "q6dsp-lpass-ports.h"
#include "q6dsp-common.h"
#include "audioreach.h"
#include "q6apm.h"

#define AUDIOREACH_BE_PCM_BASE	16
#define Y700_SEC_TDM_BCLK_RATE	12288000

struct q6apm_lpass_dai_data {
	struct q6apm_graph *graph[APM_PORT_MAX];
	bool is_port_started[APM_PORT_MAX];
	struct audioreach_module_config module_config[APM_PORT_MAX];
	struct clk *sec_tdm_ibit_clk;
	struct clk *sec_tdm_ebit_clk;
	struct clk *sec_mi2s_ibit_clk;
	struct clk *sec_mi2s_ebit_clk;
	struct clk *lpass_macro_clk;
	struct clk *lpass_dcodec_clk;
	bool sec_tdm_clks_enabled;
};

static struct clk *q6apm_lpass_y700_sec_ibit_clk(struct q6apm_lpass_dai_data *dai_data,
						 int dai_id)
{
	return dai_data->sec_mi2s_ibit_clk;
}

static int q6dma_set_channel_map(struct snd_soc_dai *dai,
				 unsigned int tx_num,
				 const unsigned int *tx_ch_mask,
				 unsigned int rx_num,
				 const unsigned int *rx_ch_mask)
{

	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int i;

	switch (dai->id) {
	case WSA_CODEC_DMA_TX_0:
	case WSA_CODEC_DMA_TX_1:
	case WSA_CODEC_DMA_TX_2:
	case VA_CODEC_DMA_TX_0:
	case VA_CODEC_DMA_TX_1:
	case VA_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
	case TX_CODEC_DMA_TX_4:
	case TX_CODEC_DMA_TX_5:
		if (!tx_ch_mask) {
			dev_err(dai->dev, "tx slot not found\n");
			return -EINVAL;
		}

		if (tx_num > AR_PCM_MAX_NUM_CHANNEL) {
			dev_err(dai->dev, "invalid tx num %d\n",
				tx_num);
			return -EINVAL;
		}
		for (i = 0; i < tx_num; i++)
			cfg->channel_map[i] = tx_ch_mask[i];

		break;
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
	case RX_CODEC_DMA_RX_0:
	case RX_CODEC_DMA_RX_1:
	case RX_CODEC_DMA_RX_2:
	case RX_CODEC_DMA_RX_3:
	case RX_CODEC_DMA_RX_4:
	case RX_CODEC_DMA_RX_5:
	case RX_CODEC_DMA_RX_6:
	case RX_CODEC_DMA_RX_7:
		/* rx */
		if (!rx_ch_mask) {
			dev_err(dai->dev, "rx slot not found\n");
			return -EINVAL;
		}
		if (rx_num > APM_PORT_MAX_AUDIO_CHAN_CNT) {
			dev_err(dai->dev, "invalid rx num %d\n",
				rx_num);
			return -EINVAL;
		}
		for (i = 0; i < rx_num; i++)
			cfg->channel_map[i] = rx_ch_mask[i];

		break;
	default:
		dev_err(dai->dev, "%s: invalid dai id 0x%x\n",
			__func__, dai->id);
		return -EINVAL;
	}

	return 0;
}

static int q6tdm_set_tdm_slot(struct snd_soc_dai *dai,
			      unsigned int tx_mask,
			      unsigned int rx_mask,
			      int slots, int slot_width)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	unsigned int cap_mask;

	if (slot_width != 16 && slot_width != 32) {
		dev_err(dai->dev, "%s: invalid slot_width %d\n",
			__func__, slot_width);
		return -EINVAL;
	}

	switch (slots) {
	case 2:
		cap_mask = 0x03;
		break;
	case 4:
		cap_mask = 0x0f;
		break;
	case 8:
		cap_mask = 0xff;
		break;
	case 16:
		cap_mask = 0xffff;
		break;
	default:
		dev_err(dai->dev, "%s: invalid slots %d\n", __func__, slots);
		return -EINVAL;
	}

	if (dai->id < PRIMARY_TDM_RX_0 || dai->id > QUINARY_TDM_TX_7) {
		dev_err(dai->dev, "%s: invalid dai id 0x%x\n",
			__func__, dai->id);
		return -EINVAL;
	}

	if (dai->id & 0x1)
		tx_mask &= cap_mask;
	else
		rx_mask &= cap_mask;

	if (dai->id == SECONDARY_TDM_RX_0 || dai->id == SECONDARY_MI2S_RX)
		cfg->frame_size_factor = slots;
	else
		cfg->frame_size_factor = slots;
	cfg->sd_line_mask = (dai->id & 0x1) ? tx_mask : rx_mask;

	return 0;
}

static int q6i2s_set_tdm_slot(struct snd_soc_dai *dai,
			      unsigned int tx_mask,
			      unsigned int rx_mask,
			      int slots, int slot_width)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	unsigned int cap_mask;

	if (dai->id != SECONDARY_MI2S_RX)
		return -ENOTSUPP;

	if (slot_width != 16 && slot_width != 32) {
		dev_err(dai->dev, "%s: invalid slot_width %d\n",
			__func__, slot_width);
		return -EINVAL;
	}

	switch (slots) {
	case 2:
		cap_mask = 0x03;
		break;
	case 4:
		cap_mask = 0x0f;
		break;
	case 8:
		cap_mask = 0xff;
		break;
	case 16:
		cap_mask = 0xffff;
		break;
	default:
		dev_err(dai->dev, "%s: invalid slots %d\n", __func__, slots);
		return -EINVAL;
	}

	cfg->frame_size_factor = 1;
	cfg->sd_line_mask = rx_mask & cap_mask;

	return 0;
}

static int q6tdm_set_channel_map(struct snd_soc_dai *dai,
				 unsigned int tx_num, const unsigned int *tx_slot,
				 unsigned int rx_num, const unsigned int *rx_slot)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	unsigned int num;
	const unsigned int *slot;

	if (dai->id < PRIMARY_TDM_RX_0 || dai->id > QUINARY_TDM_TX_7) {
		dev_err(dai->dev, "%s: invalid dai id 0x%x\n",
			__func__, dai->id);
		return -EINVAL;
	}

	if (dai->id & 0x1) {
		num = tx_num;
		slot = tx_slot;
	} else {
		num = rx_num;
		slot = rx_slot;
	}

	if (!slot) {
		dev_err(dai->dev, "%s: slot map not found\n", __func__);
		return -EINVAL;
	}

	if (num > AR_PCM_MAX_NUM_CHANNEL) {
		dev_err(dai->dev, "%s: invalid channel count %u\n",
			__func__, num);
		return -EINVAL;
	}

	/*
	 * The TDM slot offsets describe placement on the physical frame, not
	 * PCM channel identities. Keep the media-format channel map valid for
	 * modules such as MFC; slot placement is carried separately through the
	 * TDM endpoint configuration.
	 */
	audioreach_set_default_channel_mapping(cfg->channel_map, num);

	cfg->num_channels = num;
	return 0;
}

static int q6i2s_set_channel_map(struct snd_soc_dai *dai,
				 unsigned int tx_num, const unsigned int *tx_slot,
				 unsigned int rx_num, const unsigned int *rx_slot)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	unsigned int i;

	if (dai->id != SECONDARY_MI2S_RX)
		return q6dma_set_channel_map(dai, tx_num, tx_slot,
					     rx_num, rx_slot);

	if (rx_num > AR_PCM_MAX_NUM_CHANNEL) {
		dev_err(dai->dev, "%s: invalid channel count %u\n",
			__func__, rx_num);
		return -EINVAL;
	}

	audioreach_set_default_channel_mapping(cfg->channel_map, rx_num);
	cfg->num_channels = rx_num;

	for (i = rx_num; i < AR_PCM_MAX_NUM_CHANNEL; i++)
		cfg->channel_map[i] = 0;

	return 0;
}

static int q6hdmi_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int channels = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS)->max;
	int ret;

	cfg->bit_width = params_width(params);
	cfg->sample_rate = params_rate(params);
	cfg->num_channels = channels;
	audioreach_set_default_channel_mapping(cfg->channel_map, channels);

	switch (dai->id) {
	case DISPLAY_PORT_RX_0:
		cfg->dp_idx = 0;
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		cfg->dp_idx = dai->id - DISPLAY_PORT_RX_1 + 1;
		break;
	}

	ret = q6dsp_get_channel_allocation(channels);
	if (ret < 0)
		return ret;

	cfg->channel_allocation = ret;

	return 0;
}

static int q6dma_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int channels = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS)->max;

	cfg->bit_width = params_width(params);
	cfg->sample_rate = params_rate(params);
	cfg->num_channels = channels;
	audioreach_set_default_channel_mapping(cfg->channel_map, channels);

	if (dai->id == SECONDARY_MI2S_RX) {
		cfg->bit_width = 32;
		cfg->num_channels = 8;
		cfg->sd_line_mask = 0x3;
		cfg->frame_size_factor = 1;
		audioreach_set_default_channel_mapping(cfg->channel_map,
						       cfg->num_channels);
	}

	return 0;
}

static int q6tdm_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params,
			   struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	int channels = params_channels(params);

	cfg->bit_width = params_width(params);
	cfg->sample_rate = params_rate(params);
	cfg->num_channels = channels;

	if (dai->id == SECONDARY_TDM_RX_0) {
		/*
		 * Y700 uses a stereo S24 stream on secondary TDM. Android's
		 * group slot_mask=3 describes slot0/slot1, not two physical SD
		 * lines. Keep the data line as SD0 here; the TDM endpoint payload
		 * maps this to slot_mask=3 for the AW2308 pair.
		 */
		cfg->bit_width = 24;
		cfg->num_channels = 2;
		cfg->sd_line_mask = 0x1;
		cfg->frame_size_factor = 8;
		audioreach_set_default_channel_mapping(cfg->channel_map, cfg->num_channels);
	}

	return 0;
}

static void q6apm_lpass_dai_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	int rc;

	if (dai_data->is_port_started[dai->id]) {
		rc = q6apm_graph_stop(dai_data->graph[dai->id]);
		dai_data->is_port_started[dai->id] = false;
		if (rc < 0)
			dev_err(dai->dev, "failed to stop APM port (%d)\n", rc);
	}

	if (dai_data->graph[dai->id]) {
		q6apm_graph_close(dai_data->graph[dai->id]);
		dai_data->graph[dai->id] = NULL;
	}

	if ((dai->id == SECONDARY_TDM_RX_0 || dai->id == SECONDARY_MI2S_RX) &&
	    dai_data->sec_tdm_clks_enabled) {
		if (q6apm_lpass_y700_sec_ibit_clk(dai_data, dai->id))
			clk_disable_unprepare(q6apm_lpass_y700_sec_ibit_clk(dai_data,
									     dai->id));
		dai_data->sec_tdm_clks_enabled = false;
	}
}

static int q6apm_lpass_enable_sec_tdm_clks(struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	int ret;

	if (dai_data->sec_tdm_clks_enabled)
		return 0;

	if (q6apm_lpass_y700_sec_ibit_clk(dai_data, dai->id)) {
		ret = clk_set_rate(q6apm_lpass_y700_sec_ibit_clk(dai_data,
								 dai->id),
				   Y700_SEC_TDM_BCLK_RATE);
		if (ret)
			dev_warn(dai->dev,
				 "Y700 TDM ibit clk_set_rate failed: %d\n",
				 ret);
	}

	ret = q6apm_lpass_y700_sec_ibit_clk(dai_data, dai->id) ?
		clk_prepare_enable(q6apm_lpass_y700_sec_ibit_clk(dai_data,
								 dai->id)) : 0;
	if (ret)
		return ret;

	dai_data->sec_tdm_clks_enabled = true;
	return 0;
}

static int q6apm_lpass_dai_trigger(struct snd_pcm_substream *substream, int cmd,
				   struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!dai_data->is_port_started[dai->id]) {
			ret = q6apm_graph_start(dai_data->graph[dai->id]);
			if (ret < 0)
				dev_err(dai->dev, "Failed to start APM port %d\n",
					dai->id);
			else
				dai_data->is_port_started[dai->id] = true;
		}
		break;
	default:
		break;
	}

	return ret;
}

static int q6apm_lpass_dai_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];
	struct q6apm_graph *graph;
	int graph_id = dai->id;
	int rc;

	if (dai->id == SECONDARY_TDM_RX_0 || dai->id == SECONDARY_MI2S_RX) {
		rc = q6apm_lpass_enable_sec_tdm_clks(dai);
		if (rc)
			return rc;
	}

	if (dai_data->is_port_started[dai->id]) {
		q6apm_graph_stop(dai_data->graph[dai->id]);
		dai_data->is_port_started[dai->id] = false;

	}

	/**
	 * It is recommend to load DSP with source graph first and then sink
	 * graph, so sequence for playback and capture will be different
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK && dai_data->graph[dai->id] == NULL) {
		graph = q6apm_graph_open(dai->dev, NULL, dai->dev, graph_id, substream->stream);
		if (IS_ERR(graph)) {
			dev_err(dai->dev, "Failed to open graph (%d)\n", graph_id);
			rc = PTR_ERR(graph);
			return rc;
		}
		dai_data->graph[graph_id] = graph;
	}

	cfg->direction = substream->stream;
	rc = q6apm_graph_media_format_pcm(dai_data->graph[dai->id], cfg);
	if (rc) {
		dev_err(dai->dev, "Failed to set media format %d\n", rc);
		goto err;
	}

	rc = q6apm_graph_prepare(dai_data->graph[dai->id]);
	if (rc) {
		dev_err(dai->dev, "Failed to prepare Graph %d\n", rc);
		goto err;
	}

	return 0;
err:
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		q6apm_graph_close(dai_data->graph[dai->id]);
		dai_data->graph[dai->id] = NULL;
	}
	return rc;
}

static int q6apm_lpass_dai_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct q6apm_graph *graph;
	int graph_id = dai->id;
	int rc;

	if (dai->id == SECONDARY_TDM_RX_0 || dai->id == SECONDARY_MI2S_RX) {
		rc = q6apm_lpass_enable_sec_tdm_clks(dai);
		if (rc)
			return rc;
	}

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		graph = q6apm_graph_open(dai->dev, NULL, dai->dev, graph_id, substream->stream);
		if (IS_ERR(graph)) {
			dev_err(dai->dev, "Failed to open graph (%d)\n", graph_id);
			return PTR_ERR(graph);
		}
		dai_data->graph[graph_id] = graph;
	}

	return 0;
}

static int q6i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct q6apm_lpass_dai_data *dai_data = dev_get_drvdata(dai->dev);
	struct audioreach_module_config *cfg = &dai_data->module_config[dai->id];

	cfg->fmt = fmt;

	return 0;
}

static const struct snd_soc_dai_ops q6dma_ops = {
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.set_channel_map  = q6dma_set_channel_map,
	.hw_params        = q6dma_hw_params,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6i2s_ops = {
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.set_channel_map  = q6i2s_set_channel_map,
	.set_tdm_slot	= q6i2s_set_tdm_slot,
	.hw_params        = q6dma_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6tdm_ops = {
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.set_channel_map = q6tdm_set_channel_map,
	.set_tdm_slot	= q6tdm_set_tdm_slot,
	.hw_params	= q6tdm_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_dai_ops q6hdmi_ops = {
	.prepare	= q6apm_lpass_dai_prepare,
	.startup	= q6apm_lpass_dai_startup,
	.shutdown	= q6apm_lpass_dai_shutdown,
	.hw_params	= q6hdmi_hw_params,
	.set_fmt	= q6i2s_set_fmt,
	.trigger	= q6apm_lpass_dai_trigger,
};

static const struct snd_soc_component_driver q6apm_lpass_dai_component = {
	.name = "q6apm-be-dai-component",
	.of_xlate_dai_name = q6dsp_audio_ports_of_xlate_dai_name,
	.be_pcm_base = AUDIOREACH_BE_PCM_BASE,
	.use_dai_pcm_id = true,
	.remove_order   = SND_SOC_COMP_ORDER_FIRST,
};

static int q6apm_lpass_dai_dev_probe(struct platform_device *pdev)
{
	struct q6dsp_audio_port_dai_driver_config cfg;
	struct q6apm_lpass_dai_data *dai_data;
	struct snd_soc_dai_driver *dais;
	struct device *dev = &pdev->dev;
	int num_dais;

	dai_data = devm_kzalloc(dev, sizeof(*dai_data), GFP_KERNEL);
	if (!dai_data)
		return -ENOMEM;

	dai_data->sec_tdm_ibit_clk = devm_clk_get_optional(dev,
							   "sec-tdm-ibit");
	if (IS_ERR(dai_data->sec_tdm_ibit_clk))
		return PTR_ERR(dai_data->sec_tdm_ibit_clk);

	dai_data->sec_tdm_ebit_clk = devm_clk_get_optional(dev,
							   "sec-tdm-ebit");
	if (IS_ERR(dai_data->sec_tdm_ebit_clk))
		return PTR_ERR(dai_data->sec_tdm_ebit_clk);

	dai_data->sec_mi2s_ibit_clk = devm_clk_get_optional(dev,
							    "sec-mi2s-ibit");
	if (IS_ERR(dai_data->sec_mi2s_ibit_clk))
		return PTR_ERR(dai_data->sec_mi2s_ibit_clk);

	dai_data->sec_mi2s_ebit_clk = devm_clk_get_optional(dev,
							    "sec-mi2s-ebit");
	if (IS_ERR(dai_data->sec_mi2s_ebit_clk))
		return PTR_ERR(dai_data->sec_mi2s_ebit_clk);

	dai_data->lpass_macro_clk = devm_clk_get_optional(dev,
							  "lpass-macro");
	if (IS_ERR(dai_data->lpass_macro_clk))
		return PTR_ERR(dai_data->lpass_macro_clk);

	dai_data->lpass_dcodec_clk = devm_clk_get_optional(dev,
							   "lpass-dcodec");
	if (IS_ERR(dai_data->lpass_dcodec_clk))
		return PTR_ERR(dai_data->lpass_dcodec_clk);

	dev_set_drvdata(dev, dai_data);

	memset(&cfg, 0, sizeof(cfg));
	cfg.q6i2s_ops = &q6i2s_ops;
	cfg.q6tdm_ops = &q6tdm_ops;
	cfg.q6dma_ops = &q6dma_ops;
	cfg.q6hdmi_ops = &q6hdmi_ops;
	dais = q6dsp_audio_ports_set_config(dev, &cfg, &num_dais);

	return devm_snd_soc_register_component(dev, &q6apm_lpass_dai_component, dais, num_dais);
}

#ifdef CONFIG_OF
static const struct of_device_id q6apm_lpass_dai_device_id[] = {
	{ .compatible = "qcom,q6apm-lpass-dais" },
	{},
};
MODULE_DEVICE_TABLE(of, q6apm_lpass_dai_device_id);
#endif

static struct platform_driver q6apm_lpass_dai_platform_driver = {
	.driver = {
		.name = "q6apm-lpass-dais",
		.of_match_table = of_match_ptr(q6apm_lpass_dai_device_id),
	},
	.probe = q6apm_lpass_dai_dev_probe,
};
module_platform_driver(q6apm_lpass_dai_platform_driver);

MODULE_DESCRIPTION("AUDIOREACH APM LPASS dai driver");
MODULE_LICENSE("GPL");
