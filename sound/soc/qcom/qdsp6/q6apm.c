// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/soc/qcom,gpr.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/wait.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include "audioreach.h"
#include "q6apm.h"

/* Graph Management */
struct apm_graph_mgmt_cmd {
	struct apm_module_param_data param_data;
	uint32_t num_sub_graphs;
	uint32_t sub_graph_id_list[];
} __packed;

#define APM_GRAPH_MGMT_PSIZE(p, n) ALIGN(struct_size(p, sub_graph_id_list, n), 8)

static struct q6apm *g_apm;
static DEFINE_MUTEX(g_apm_lock);
static atomic_t q6apm_token = ATOMIC_INIT(0);

uint32_t q6apm_next_token(void)
{
	uint32_t token;

	do {
		token = (uint32_t)atomic_inc_return(&q6apm_token);
	} while (!token);

	return token;
}

static void q6apm_release(struct kref *ref)
{
	struct q6apm *apm = container_of(ref, struct q6apm, refcount);

	WARN_ON(atomic_read(&apm->active_graphs));
	WARN_ON(!idr_is_empty(&apm->graph_idr));
	WARN_ON(!idr_is_empty(&apm->graph_info_idr));
	WARN_ON(!idr_is_empty(&apm->sub_graphs_idr));
	WARN_ON(!idr_is_empty(&apm->containers_idr));
	WARN_ON(!idr_is_empty(&apm->modules_idr));
	idr_destroy(&apm->graph_idr);
	idr_destroy(&apm->graph_info_idr);
	idr_destroy(&apm->sub_graphs_idr);
	idr_destroy(&apm->containers_idr);
	idr_destroy(&apm->modules_idr);
	kfree(apm);
}

int q6apm_send_cmd_sync(struct q6apm *apm, const struct gpr_pkt *pkt,
			uint32_t rsp_opcode)
{
	gpr_device_t *gdev = apm->gdev;
	struct gpr_pkt *tx_pkt = (struct gpr_pkt *)pkt;
	uint32_t graph_id;
	int ret;

	mutex_lock(&apm->online_lock);
	if (!READ_ONCE(apm->online)) {
		ret = -ENETRESET;
		goto unlock;
	}

	mutex_lock(&apm->cmd_lock);
	graph_id = tx_pkt->hdr.token;
	mutex_lock(&apm->response_lock);
	tx_pkt->hdr.token = q6apm_next_token();
	apm->pending_token = tx_pkt->hdr.token;
	apm->pending_opcode = tx_pkt->hdr.opcode;
	apm->pending_graph_id = graph_id;
	apm->response_pending = true;
	mutex_unlock(&apm->response_lock);

	ret = audioreach_send_cmd_sync_locked(&gdev->dev, gdev,
					      &apm->result, &apm->cmd_lock,
					      NULL, &apm->wait, pkt,
					      rsp_opcode);
	mutex_lock(&apm->response_lock);
	apm->response_pending = false;
	mutex_unlock(&apm->response_lock);
	mutex_unlock(&apm->cmd_lock);
unlock:
	mutex_unlock(&apm->online_lock);
	return ret;
}

static int audioreach_graph_mgmt_cmd(struct audioreach_graph *graph,
				     uint32_t opcode);

static struct audioreach_graph *q6apm_get_audioreach_graph(struct q6apm *apm, uint32_t graph_id)
{
	struct audioreach_graph_info *info;
	struct audioreach_graph *graph;
	int generation, id, ret;

retry:
	down_read(&apm->graph_lock);
	if (!READ_ONCE(apm->online)) {
		graph = ERR_PTR(-ENETRESET);
		goto unlock_read;
	}

	mutex_lock(&apm->lock);
	graph = idr_find(&apm->graph_idr, graph_id);
	if (graph && kref_get_unless_zero(&graph->refcount)) {
		mutex_unlock(&apm->lock);
		goto unlock_read;
	}
	generation = atomic_read(&apm->graph_generation);
	mutex_unlock(&apm->lock);
	up_read(&apm->graph_lock);
	if (graph) {
		wait_event(apm->graph_wait,
			   atomic_read(&apm->graph_generation) != generation);
		goto retry;
	}

	down_write(&apm->graph_lock);
	if (!READ_ONCE(apm->online)) {
		graph = ERR_PTR(-ENETRESET);
		goto unlock_write;
	}

	/* Another opener may have populated the cache before we got write access. */
	mutex_lock(&apm->lock);
	graph = idr_find(&apm->graph_idr, graph_id);
	if (graph && kref_get_unless_zero(&graph->refcount)) {
		mutex_unlock(&apm->lock);
		goto unlock_write;
	}
	if (graph) {
		generation = atomic_read(&apm->graph_generation);
		mutex_unlock(&apm->lock);
		up_write(&apm->graph_lock);
		wait_event(apm->graph_wait,
			   atomic_read(&apm->graph_generation) != generation);
		goto retry;
	}
	info = idr_find(&apm->graph_info_idr, graph_id);
	mutex_unlock(&apm->lock);

	if (!info) {
		graph = ERR_PTR(-ENODEV);
		goto unlock_write;
	}

	graph = kzalloc_obj(*graph);
	if (!graph) {
		graph = ERR_PTR(-ENOMEM);
		goto unlock_write;
	}

	graph->apm = apm;
	graph->info = info;
	graph->id = graph_id;

	graph->graph = audioreach_alloc_graph_pkt(apm, info);
	if (IS_ERR(graph->graph)) {
		void *err = graph->graph;

		kfree(graph);
		graph = ERR_CAST(err);
		goto unlock_write;
	}

	kref_init(&graph->refcount);
	mutex_init(&graph->state_lock);

	ret = q6apm_send_cmd_sync(apm, graph->graph, 0);
	if (ret) {
		kfree(graph->graph);
		kfree(graph);
		graph = ERR_PTR(ret);
		goto unlock_write;
	}

	mutex_lock(&apm->lock);
	id = idr_alloc(&apm->graph_idr, graph, graph_id, graph_id + 1, GFP_KERNEL);
	if (id < 0) {
		dev_err(apm->dev, "Unable to allocate graph id (%d)\n", graph_id);
		mutex_unlock(&apm->lock);
		audioreach_graph_mgmt_cmd(graph, APM_CMD_GRAPH_CLOSE);
		kfree(graph->graph);
		kfree(graph);
		graph = ERR_PTR(id);
		goto unlock_write;
	}
	mutex_unlock(&apm->lock);

	kref_get(&apm->refcount);
	atomic_inc(&apm->active_graphs);
	up_write(&apm->graph_lock);
	return graph;

unlock_read:
	up_read(&apm->graph_lock);
	return graph;
unlock_write:
	up_write(&apm->graph_lock);
	return graph;
}

static int audioreach_graph_mgmt_cmd(struct audioreach_graph *graph, uint32_t opcode)
{
	struct audioreach_graph_info *info = graph->info;
	int num_sub_graphs = info->num_sub_graphs;
	struct apm_module_param_data *param_data;
	struct apm_graph_mgmt_cmd *mgmt_cmd;
	struct audioreach_sub_graph *sg;
	struct q6apm *apm = graph->apm;
	int i = 0, payload_size = APM_GRAPH_MGMT_PSIZE(mgmt_cmd, num_sub_graphs);

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size, opcode, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	mgmt_cmd = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	mgmt_cmd->num_sub_graphs = num_sub_graphs;

	param_data = &mgmt_cmd->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_SUB_GRAPH_LIST;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;

	list_for_each_entry(sg, &info->sg_list, node)
		mgmt_cmd->sub_graph_id_list[i++] = sg->sub_graph_id;

	return q6apm_send_cmd_sync(apm, pkt, 0);
}

static void q6apm_put_audioreach_graph(struct kref *ref)
{
	struct audioreach_graph *graph;
	struct audioreach_graph *removed;
	struct q6apm *apm;
	int ret;

	graph = container_of(ref, struct audioreach_graph, refcount);
	apm = graph->apm;

	if (READ_ONCE(apm->online)) {
		ret = audioreach_graph_mgmt_cmd(graph, APM_CMD_GRAPH_CLOSE);
		if (ret)
			dev_warn(apm->dev, "failed to close graph %u: %d\n",
				 graph->id, ret);
	}

	mutex_lock(&apm->lock);
	removed = idr_remove(&apm->graph_idr, graph->id);
	mutex_unlock(&apm->lock);
	WARN_ON(removed != graph);

	kfree(graph->graph);
	kfree(graph);
	if (atomic_dec_and_test(&apm->active_graphs)) {
		down_write(&apm->graph_lock);
		audioreach_tplg_free_deferred(apm);
		up_write(&apm->graph_lock);
	}
	atomic_inc(&apm->graph_generation);
	wake_up_all(&apm->graph_wait);
	kref_put(&apm->refcount, q6apm_release);
}


static int q6apm_get_apm_state(struct q6apm *apm)
{
	int ret;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(0,
								APM_CMD_GET_SPF_STATE, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	ret = q6apm_send_cmd_sync(apm, pkt, APM_CMD_RSP_GET_SPF_STATE);
	if (ret)
		return ret;

	return apm->state;
}

bool q6apm_is_adsp_ready(void)
{
	int state = -ENODEV;

	mutex_lock(&g_apm_lock);
	if (g_apm)
		state = q6apm_get_apm_state(g_apm);
	mutex_unlock(&g_apm_lock);

	return state > 0;
}
EXPORT_SYMBOL_GPL(q6apm_is_adsp_ready);

static struct audioreach_module *__q6apm_find_module_by_mid(struct q6apm *apm,
						    struct audioreach_graph_info *info,
						    uint32_t mid)
{
	struct audioreach_container *container;
	struct audioreach_sub_graph *sgs;
	struct audioreach_module *module;

	list_for_each_entry(sgs, &info->sg_list, node) {
		list_for_each_entry(container, &sgs->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if (mid == module->module_id)
					return module;
			}
		}
	}

	return NULL;
}

int q6apm_graph_media_format_shmem(struct q6apm_graph *graph,
				   struct audioreach_module_config *cfg)
{
	struct audioreach_module *module;

	if (cfg->direction == SNDRV_PCM_STREAM_CAPTURE)
		module = q6apm_find_module_by_mid(graph, MODULE_ID_RD_SHARED_MEM_EP);
	else
		module = q6apm_find_module_by_mid(graph, MODULE_ID_WR_SHARED_MEM_EP);

	if (!module)
		return -ENODEV;

	return audioreach_set_media_format(graph, module, cfg);

}
EXPORT_SYMBOL_GPL(q6apm_graph_media_format_shmem);

int q6apm_map_memory_fixed_region(struct device *dev, unsigned int graph_id, phys_addr_t phys,
				  size_t sz)
{
	struct audioreach_graph *cached_graph;
	struct audioreach_graph_info *info;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct apm_shared_map_region_payload *mregions;
	struct apm_cmd_shared_mem_map_regions *cmd;
	int payload_size = sizeof(*cmd) + (sizeof(*mregions));
	uint32_t buf_sz;
	int ret;
	void *p;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size,
						APM_CMD_SHARED_MEM_MAP_REGIONS, graph_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);
	if (!sz || sz > U32_MAX - (SZ_4K - 1))
		return -EINVAL;
	/* DSP expects size should be aligned to 4K. */
	buf_sz = ALIGN(sz, SZ_4K);

	mutex_lock(&apm->lock);
	cached_graph = idr_find(&apm->graph_idr, graph_id);
	if (cached_graph && !kref_get_unless_zero(&cached_graph->refcount))
		cached_graph = NULL;
	mutex_unlock(&apm->lock);
	if (!cached_graph)
		down_read(&apm->graph_lock);

	mutex_lock(&apm->lock);
	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info) {
		mutex_unlock(&apm->lock);
		ret = -ENODEV;
		goto unlock_graph;
	}
	mutex_unlock(&apm->lock);

	mutex_lock(&info->mem_map_lock);
	mutex_lock(&apm->lock);
	if (info->mem_map_handle) {
		if (info->mem_map_phys != phys || info->mem_map_size != buf_sz) {
			mutex_unlock(&apm->lock);
			ret = -EBUSY;
			goto unlock_map;
		}
		if (info->mem_map_users == UINT_MAX) {
			mutex_unlock(&apm->lock);
			ret = -EOVERFLOW;
			goto unlock_map;
		}
		info->mem_map_users++;
		mutex_unlock(&apm->lock);
		ret = 0;
		goto unlock_map;
	}
	mutex_unlock(&apm->lock);

	p = (void *)pkt + GPR_HDR_SIZE;
	cmd = p;
	cmd->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = 1;
	cmd->property_flag = 0x0;

	mregions = p + sizeof(*cmd);

	mregions->shm_addr_lsw = lower_32_bits(phys);
	mregions->shm_addr_msw = upper_32_bits(phys);
	mregions->mem_size_bytes = buf_sz;

	ret = q6apm_send_cmd_sync(apm, pkt,
				  APM_CMD_RSP_SHARED_MEM_MAP_REGIONS);
	if (!ret) {
		mutex_lock(&apm->lock);
		info->mem_map_phys = phys;
		info->mem_map_size = buf_sz;
		info->mem_map_users = 1;
		mutex_unlock(&apm->lock);
	}
unlock_map:
	mutex_unlock(&info->mem_map_lock);
unlock_graph:
	if (cached_graph)
		kref_put(&cached_graph->refcount,
			 q6apm_put_audioreach_graph);
	else
		up_read(&apm->graph_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_map_memory_fixed_region);

int q6apm_alloc_fragments(struct q6apm_graph *graph, unsigned int dir, phys_addr_t phys,
				size_t period_sz, unsigned int periods)
{
	struct audioreach_graph_data *data;
	struct audio_buffer *buf;
	int cnt;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	mutex_lock(&graph->lock);

	data->dsp_buf = 0;

	if (data->buf) {
		mutex_unlock(&graph->lock);
		return 0;
	}

	buf = kzalloc_objs(struct audio_buffer, periods);
	if (!buf) {
		mutex_unlock(&graph->lock);
		return -ENOMEM;
	}

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	data->buf = buf;

	buf[0].phys = phys;
	buf[0].size = period_sz;

	for (cnt = 1; cnt < periods; cnt++) {
		if (period_sz > 0) {
			buf[cnt].phys = buf[0].phys + (cnt * period_sz);
			buf[cnt].size = period_sz;
		}
	}
	data->num_periods = periods;

	mutex_unlock(&graph->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_alloc_fragments);

int q6apm_unmap_memory_fixed_region(struct device *dev, unsigned int graph_id)
{
	struct apm_cmd_shared_mem_unmap_regions *cmd;
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph *cached_graph;
	struct audioreach_graph_info *info;
	uint32_t mem_map_handle;
	int ret;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(sizeof(*cmd),
						APM_CMD_SHARED_MEM_UNMAP_REGIONS, graph_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	mutex_lock(&apm->lock);
	cached_graph = idr_find(&apm->graph_idr, graph_id);
	if (cached_graph && !kref_get_unless_zero(&cached_graph->refcount))
		cached_graph = NULL;
	mutex_unlock(&apm->lock);
	if (!cached_graph)
		down_read(&apm->graph_lock);

	mutex_lock(&apm->lock);
	info = idr_find(&apm->graph_info_idr, graph_id);
	if (!info) {
		mutex_unlock(&apm->lock);
		ret = -ENODEV;
		goto unlock_graph;
	}
	mutex_unlock(&apm->lock);

	mutex_lock(&info->mem_map_lock);
	mutex_lock(&apm->lock);
	mem_map_handle = info->mem_map_handle;
	if (mem_map_handle && info->mem_map_users > 1) {
		info->mem_map_users--;
		mutex_unlock(&apm->lock);
		ret = 0;
		goto unlock_map;
	}
	mutex_unlock(&apm->lock);
	if (!mem_map_handle) {
		ret = 0;
		goto unlock_map;
	}

	cmd = (void *)pkt + GPR_HDR_SIZE;
	cmd->mem_map_handle = mem_map_handle;

	ret = q6apm_send_cmd_sync(apm, pkt,
				  APM_CMD_SHARED_MEM_UNMAP_REGIONS);
	if (!ret) {
		mutex_lock(&apm->lock);
		info->mem_map_phys = 0;
		info->mem_map_size = 0;
		info->mem_map_users = 0;
		mutex_unlock(&apm->lock);
	}
unlock_map:
	mutex_unlock(&info->mem_map_lock);
unlock_graph:
	if (cached_graph)
		kref_put(&cached_graph->refcount,
			 q6apm_put_audioreach_graph);
	else
		up_read(&apm->graph_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_unmap_memory_fixed_region);

int q6apm_free_fragments(struct q6apm_graph *graph, unsigned int dir)
{
	audioreach_graph_free_buf(graph);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_free_fragments);

int q6apm_remove_initial_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_REMOVE_INITIAL_SILENCE, samples);
}
EXPORT_SYMBOL_GPL(q6apm_remove_initial_silence);

int q6apm_remove_trailing_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_REMOVE_TRAILING_SILENCE, samples);
}
EXPORT_SYMBOL_GPL(q6apm_remove_trailing_silence);

int q6apm_enable_compress_module(struct device *dev, struct q6apm_graph *graph, bool en)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	return audioreach_send_u32_param(graph, module, PARAM_ID_MODULE_ENABLE, en);
}
EXPORT_SYMBOL_GPL(q6apm_enable_compress_module);

int q6apm_set_real_module_id(struct device *dev, struct q6apm_graph *graph,
			     uint32_t codec_id)
{
	struct audioreach_module *module;
	uint32_t module_id;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_PLACEHOLDER_DECODER);
	if (!module)
		return -ENODEV;

	switch (codec_id) {
	case SND_AUDIOCODEC_MP3:
		module_id = MODULE_ID_MP3_DECODE;
		break;
	case SND_AUDIOCODEC_AAC:
		module_id = MODULE_ID_AAC_DEC;
		break;
	case SND_AUDIOCODEC_FLAC:
		module_id = MODULE_ID_FLAC_DEC;
		break;
	case SND_AUDIOCODEC_OPUS_RAW:
		module_id = MODULE_ID_OPUS_DEC;
		break;
	default:
		return -EINVAL;
	}

	return audioreach_send_u32_param(graph, module, PARAM_ID_REAL_MODULE_ID,
					 module_id);
}
EXPORT_SYMBOL_GPL(q6apm_set_real_module_id);

int q6apm_graph_media_format_pcm(struct q6apm_graph *graph, struct audioreach_module_config *cfg)
{
	struct audioreach_graph_info *info = graph->info;
	struct audioreach_sub_graph *sgs;
	struct audioreach_container *container;
	struct audioreach_module *module;
	int ret;

	list_for_each_entry(sgs, &info->sg_list, node) {
		list_for_each_entry(container, &sgs->container_list, node) {
			list_for_each_entry(module, &container->modules_list, node) {
				if ((module->module_id == MODULE_ID_WR_SHARED_MEM_EP) ||
					(module->module_id == MODULE_ID_RD_SHARED_MEM_EP))
					continue;

				ret = audioreach_set_media_format(graph, module, cfg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;

}
EXPORT_SYMBOL_GPL(q6apm_graph_media_format_pcm);

static int q6apm_graph_get_tx_shmem_module_iid(struct q6apm_graph *graph)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_RD_SHARED_MEM_EP);
	if (!module)
		return -ENODEV;

	return module->instance_id;

}

int q6apm_graph_get_rx_shmem_module_iid(struct q6apm_graph *graph)
{
	struct audioreach_module *module;

	module = q6apm_find_module_by_mid(graph, MODULE_ID_WR_SHARED_MEM_EP);
	if (!module)
		return -ENODEV;

	return module->instance_id;

}
EXPORT_SYMBOL_GPL(q6apm_graph_get_rx_shmem_module_iid);

static int q6apm_find_inflight_buffer(const struct audioreach_graph_data *data,
				       uint32_t token)
{
	uint32_t i;

	for (i = 0; i < data->num_periods; i++) {
		if (data->buf[i].in_flight && data->buf[i].token == token)
			return i;
	}

	return -ENOENT;
}

static void q6apm_graph_release(struct kref *ref)
{
	struct q6apm_graph *graph = container_of(ref, struct q6apm_graph,
					      refcount);

	WARN_ON(graph->port);
	WARN_ON(graph->ar_graph);
	kfree(graph);
}

static void q6apm_graph_finish_close(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;

	mutex_lock(&graph->lock);
	ar_graph = graph->ar_graph;
	graph->ar_graph = NULL;
	mutex_unlock(&graph->lock);

	if (ar_graph)
		kref_put(&ar_graph->refcount, q6apm_put_audioreach_graph);
	kref_put(&graph->refcount, q6apm_graph_release);
}

static void q6apm_graph_close_work(struct work_struct *work)
{
	struct q6apm_graph *graph = container_of(work, struct q6apm_graph,
					      close_work);

	/* A callback-originated close must outlive the callback stack. */
	mutex_lock(&graph->callback_lock);
	mutex_unlock(&graph->callback_lock);
	q6apm_graph_finish_close(graph);
}

static void q6apm_graph_call_client(struct q6apm_graph *graph,
					  uint32_t opcode, uint32_t token,
					  uint32_t bytes, void *payload)
{
	q6apm_cb cb;
	void *priv;

	lockdep_assert_held(&graph->callback_lock);

	mutex_lock(&graph->lifecycle_lock);
	if (graph->closing || !graph->cb) {
		mutex_unlock(&graph->lifecycle_lock);
		return;
	}
	graph->callback_task = current;
	cb = graph->cb;
	priv = graph->priv;
	mutex_unlock(&graph->lifecycle_lock);

	cb(opcode, token, bytes, payload, priv);

	mutex_lock(&graph->lifecycle_lock);
	if (graph->callback_task == current)
		graph->callback_task = NULL;
	mutex_unlock(&graph->lifecycle_lock);
}

static void q6apm_retire_inflight_buffer(struct audioreach_graph_data *data,
					 int buffer)
{
	data->buf[buffer].submitted = 0;
	data->buf[buffer].token = 0;
	data->buf[buffer].in_flight = false;
	atomic_set(&data->hw_ptr, buffer + 1);
}

static void q6apm_reset_graph_data(struct audioreach_graph_data *data)
{
	uint32_t i;

	for (i = 0; i < data->num_periods; i++) {
		data->buf[i].submitted = 0;
		data->buf[i].token = 0;
		data->buf[i].in_flight = false;
	}
	data->dsp_buf = 0;
	atomic_set(&data->hw_ptr, 0);
}

static bool q6apm_graph_data_idle(const struct audioreach_graph_data *data)
{
	uint32_t i;

	for (i = 0; i < data->num_periods; i++) {
		if (data->buf[i].in_flight)
			return false;
	}

	return true;
}

static bool q6apm_graph_client_idle_locked(const struct q6apm_graph *graph)
{
	return !graph->flushing && !graph->eos_pending &&
		q6apm_graph_data_idle(&graph->rx_data) &&
		q6apm_graph_data_idle(&graph->tx_data);
}

int q6apm_write_async(struct q6apm_graph *graph, uint32_t len, uint32_t msw_ts,
			      uint32_t lsw_ts, uint32_t wflags)
{
	struct apm_data_cmd_wr_sh_mem_ep_data_buffer_v2 *write_buffer;
	struct audio_buffer *ab;
	struct gpr_pkt *pkt;
	uint32_t token;
	int ret;

	mutex_lock(&graph->lock);
	if (graph->flushing) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!graph->rx_data.buf || !graph->rx_data.num_periods ||
	    graph->rx_data.dsp_buf >= graph->rx_data.num_periods) {
		ret = -ENODATA;
		goto unlock;
	}

	ab = &graph->rx_data.buf[graph->rx_data.dsp_buf];
	if (ab->in_flight) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!len || len > ab->size) {
		ret = -EINVAL;
		goto unlock;
	}
	token = q6apm_next_token();

	pkt = audioreach_alloc_pkt(sizeof(*write_buffer),
				   DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2,
				   token, graph->port->id, graph->shm_iid);
	if (IS_ERR(pkt)) {
		ret = PTR_ERR(pkt);
		goto unlock;
	}
	write_buffer = (void *)pkt + GPR_HDR_SIZE;

	write_buffer->buf_addr_lsw = lower_32_bits(ab->phys);
	write_buffer->buf_addr_msw = upper_32_bits(ab->phys);
	write_buffer->buf_size = len;
	write_buffer->timestamp_lsw = lsw_ts;
	write_buffer->timestamp_msw = msw_ts;
	write_buffer->mem_map_handle = graph->info->mem_map_handle;
	write_buffer->flags = wflags;

	ret = gpr_send_port_pkt(graph->port, pkt);
	kfree(pkt);
	if (ret < 0)
		goto unlock;

	ab->submitted = len;
	ab->token = token;
	ab->in_flight = true;
	graph->rx_data.dsp_buf++;
	if (graph->rx_data.dsp_buf >= graph->rx_data.num_periods)
		graph->rx_data.dsp_buf = 0;
	ret = 0;

unlock:
	mutex_unlock(&graph->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_write_async);

int q6apm_read(struct q6apm_graph *graph)
{
	struct data_cmd_rd_sh_mem_ep_data_buffer_v2 *read_buffer;
	struct audioreach_graph_data *port;
	struct audio_buffer *ab;

	struct gpr_pkt *pkt;
	uint32_t token;
	int ret;

	mutex_lock(&graph->lock);
	port = &graph->tx_data;
	if (graph->flushing) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!port->buf || !port->num_periods ||
	    port->dsp_buf >= port->num_periods) {
		ret = -ENODATA;
		goto unlock;
	}
	ab = &port->buf[port->dsp_buf];
	if (ab->in_flight) {
		ret = -EBUSY;
		goto unlock;
	}
	token = q6apm_next_token();
	pkt = audioreach_alloc_pkt(sizeof(*read_buffer),
				   DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2,
				   token, graph->port->id, graph->shm_iid);
	if (IS_ERR(pkt)) {
		ret = PTR_ERR(pkt);
		goto unlock;
	}
	read_buffer = (void *)pkt + GPR_HDR_SIZE;

	read_buffer->buf_addr_lsw = lower_32_bits(ab->phys);
	read_buffer->buf_addr_msw = upper_32_bits(ab->phys);
	read_buffer->mem_map_handle = graph->info->mem_map_handle;
	read_buffer->buf_size = ab->size;

	ret = gpr_send_port_pkt(graph->port, pkt);
	kfree(pkt);
	if (ret < 0)
		goto unlock;

	ab->submitted = ab->size;
	ab->token = token;
	ab->in_flight = true;
	port->dsp_buf++;

	if (port->dsp_buf >= port->num_periods)
		port->dsp_buf = 0;
	ret = 0;

unlock:
	mutex_unlock(&graph->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_read);

int q6apm_get_hw_pointer(struct q6apm_graph *graph, int dir)
{
	struct audioreach_graph_data *data;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	return (int)atomic_read(&data->hw_ptr);
}
EXPORT_SYMBOL_GPL(q6apm_get_hw_pointer);

static int graph_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	struct data_cmd_rsp_rd_sh_mem_ep_data_buffer_done_v2 *rd_done;
	struct data_cmd_rsp_wr_sh_mem_ep_data_buffer_done_v2 *done;
	const struct gpr_ibasic_rsp_result_t *result;
	struct q6apm_graph *graph = priv;
	const struct gpr_hdr *hdr = &data->hdr;
	struct device *dev = graph->dev;
	uint32_t client_event;
	uint32_t bytes = 0;
	phys_addr_t phys;
	bool failed;
	int buffer;

	kref_get(&graph->refcount);
	result = data->payload;

	switch (hdr->opcode) {
	case DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2:
		client_event = APM_CLIENT_EVENT_DATA_WRITE_DONE;
		mutex_lock(&graph->callback_lock);
		mutex_lock(&graph->lock);
		if (!graph->ar_graph || !graph->rx_data.buf ||
		    !graph->rx_data.num_periods) {
			mutex_unlock(&graph->lock);
			mutex_unlock(&graph->callback_lock);
			break;
		}
		buffer = q6apm_find_inflight_buffer(&graph->rx_data,
						      hdr->token);
		if (buffer < 0) {
			mutex_unlock(&graph->lock);
			mutex_unlock(&graph->callback_lock);
			dev_dbg(dev, "ignoring stale write-done token %u\n",
				hdr->token);
			break;
		}
		if (data->payload_size < sizeof(*done)) {
			/* The header token is still enough to retire this submission. */
			dev_err(dev, "short write-done response: %d bytes\n",
				data->payload_size);
			q6apm_retire_inflight_buffer(&graph->rx_data, buffer);
			mutex_unlock(&graph->lock);
			wake_up_all(&graph->data_wait);
			q6apm_graph_call_client(graph, client_event, buffer, 0,
						NULL);
			mutex_unlock(&graph->callback_lock);
			break;
		}
		done = data->payload;
		phys = graph->rx_data.buf[buffer].phys;
		bytes = graph->rx_data.buf[buffer].submitted;
		failed = !bytes || bytes > graph->rx_data.buf[buffer].size;
		if (failed) {
			dev_err(dev, "invalid submitted size %u for write token %d\n",
				bytes, buffer);
		}
		if (lower_32_bits(phys) != done->buf_addr_lsw ||
		    upper_32_bits(phys) != done->buf_addr_msw) {
			dev_err(dev, "WR BUFF Unexpected addr %08x-%08x\n", done->buf_addr_lsw,
				done->buf_addr_msw);
			failed = true;
		}
		q6apm_retire_inflight_buffer(&graph->rx_data, buffer);
		mutex_unlock(&graph->lock);
		wake_up_all(&graph->data_wait);
		if (failed || done->status) {
			dev_err(dev, "write-done failed for token %d: %u\n",
				buffer, done->status);
			bytes = 0;
		}
		q6apm_graph_call_client(graph, client_event, buffer, bytes,
					data->payload);
		mutex_unlock(&graph->callback_lock);

		break;
	case DATA_CMD_RSP_RD_SH_MEM_EP_DATA_BUFFER_V2:
		client_event = APM_CLIENT_EVENT_DATA_READ_DONE;
		mutex_lock(&graph->callback_lock);
		mutex_lock(&graph->lock);
		if (!graph->ar_graph || !graph->tx_data.buf ||
		    !graph->tx_data.num_periods) {
			mutex_unlock(&graph->lock);
			mutex_unlock(&graph->callback_lock);
			break;
		}
		buffer = q6apm_find_inflight_buffer(&graph->tx_data,
						      hdr->token);
		if (buffer < 0) {
			mutex_unlock(&graph->lock);
			mutex_unlock(&graph->callback_lock);
			dev_dbg(dev, "ignoring stale read-done token %u\n",
				hdr->token);
			break;
		}
		if (data->payload_size < sizeof(*rd_done)) {
			/* The header token is still enough to retire this submission. */
			dev_err(dev, "short read-done response: %d bytes\n",
				data->payload_size);
			q6apm_retire_inflight_buffer(&graph->tx_data, buffer);
			mutex_unlock(&graph->lock);
			wake_up_all(&graph->data_wait);
			q6apm_graph_call_client(graph, client_event, buffer, 0,
						NULL);
			mutex_unlock(&graph->callback_lock);
			break;
		}
		rd_done = data->payload;
		phys = graph->tx_data.buf[buffer].phys;
		failed = rd_done->data_size > graph->tx_data.buf[buffer].size;
		if (failed) {
			dev_err(dev, "read-done size %u exceeds buffer %u\n",
				rd_done->data_size,
				graph->tx_data.buf[buffer].size);
		}
		if (upper_32_bits(phys) != rd_done->buf_addr_msw ||
		    lower_32_bits(phys) != rd_done->buf_addr_lsw) {
			dev_err(dev, "RD BUFF Unexpected addr %08x-%08x\n", rd_done->buf_addr_lsw,
				rd_done->buf_addr_msw);
			failed = true;
		}
		q6apm_retire_inflight_buffer(&graph->tx_data, buffer);
		mutex_unlock(&graph->lock);
		wake_up_all(&graph->data_wait);
		bytes = failed || rd_done->status ? 0 : rd_done->data_size;
		if (failed || rd_done->status)
			dev_err(dev, "read-done failed for token %d: %u\n",
				buffer, rd_done->status);
		q6apm_graph_call_client(graph, client_event, buffer, bytes,
					data->payload);
		mutex_unlock(&graph->callback_lock);
		break;
	case DATA_CMD_WR_SH_MEM_EP_EOS_RENDERED:
		client_event = APM_CLIENT_EVENT_CMD_EOS_DONE;
		mutex_lock(&graph->callback_lock);
		mutex_lock(&graph->lock);
		if (!graph->ar_graph || !graph->eos_pending ||
		    hdr->token != graph->pending_eos_token) {
			mutex_unlock(&graph->lock);
			mutex_unlock(&graph->callback_lock);
			dev_dbg(dev, "ignoring stale EOS token %u\n", hdr->token);
			break;
		}
		graph->eos_pending = false;
		mutex_unlock(&graph->lock);
		wake_up_all(&graph->data_wait);
		q6apm_graph_call_client(graph, client_event, hdr->token, 0,
					data->payload);
		mutex_unlock(&graph->callback_lock);
		break;
	case GPR_BASIC_RSP_RESULT:
		mutex_lock(&graph->response_lock);
		if (!graph->command_pending ||
		    hdr->token != graph->pending_cmd_token) {
			mutex_unlock(&graph->response_lock);
			dev_dbg(dev,
				"ignoring stale graph response 0x%08x token %u\n",
				hdr->opcode, hdr->token);
			break;
		}
		if (data->payload_size < sizeof(*result)) {
			dev_err(dev, "short graph basic response: %d bytes\n",
				data->payload_size);
			graph->result.opcode = graph->pending_cmd_opcode;
			graph->result.status = 1;
			wake_up(&graph->cmd_wait);
			mutex_unlock(&graph->response_lock);
			break;
		}
		if (result->opcode != graph->pending_cmd_opcode) {
			dev_err(dev,
				"graph response opcode mismatch: expected 0x%08x got 0x%08x\n",
				graph->pending_cmd_opcode, result->opcode);
			graph->result.opcode = graph->pending_cmd_opcode;
			graph->result.status = 1;
			wake_up(&graph->cmd_wait);
			mutex_unlock(&graph->response_lock);
			break;
		}
		switch (result->opcode) {
		case APM_CMD_SHARED_MEM_MAP_REGIONS:
		case DATA_CMD_WR_SH_MEM_EP_MEDIA_FORMAT:
		case APM_CMD_SET_CFG:
			graph->result.opcode = result->opcode;
			graph->result.status = result->status;
			if (result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n",
					result->status, result->opcode);
			wake_up(&graph->cmd_wait);
			break;
		default:
			break;
		}
		mutex_unlock(&graph->response_lock);
		break;
	default:
		break;
	}
	kref_put(&graph->refcount, q6apm_graph_release);
	return 0;
}

struct q6apm_graph *q6apm_graph_open(struct device *dev, q6apm_cb cb,
				     void *priv, int graph_id, int dir)
{
	struct q6apm *apm = dev_get_drvdata(dev->parent);
	struct audioreach_graph *ar_graph;
	struct q6apm_graph *graph;
	int ret;

	ar_graph = q6apm_get_audioreach_graph(apm, graph_id);
	if (IS_ERR(ar_graph)) {
		dev_err(dev, "No graph found with id %d\n", graph_id);
		return ERR_CAST(ar_graph);
	}

	graph = kzalloc_obj(*graph);
	if (!graph) {
		ret = -ENOMEM;
		goto put_ar_graph;
	}

	graph->apm = apm;
	graph->priv = priv;
	graph->cb = cb;
	graph->info = ar_graph->info;
	graph->ar_graph = ar_graph;
	graph->id = ar_graph->id;
	graph->dev = dev;
	mutex_lock(&ar_graph->state_lock);
	graph->flush_generation = ar_graph->flush_generation;
	mutex_unlock(&ar_graph->state_lock);

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		ret = q6apm_graph_get_rx_shmem_module_iid(graph);
	else
		ret = q6apm_graph_get_tx_shmem_module_iid(graph);
	if (ret < 0)
		goto free_graph;
	graph->shm_iid = ret;


	mutex_init(&graph->lock);
	mutex_init(&graph->callback_lock);
	mutex_init(&graph->lifecycle_lock);
	mutex_init(&graph->response_lock);
	kref_init(&graph->refcount);
	INIT_WORK(&graph->close_work, q6apm_graph_close_work);
	init_waitqueue_head(&graph->cmd_wait);
	init_waitqueue_head(&graph->data_wait);

	graph->port = gpr_alloc_port(apm->gdev, dev, graph_callback, graph);
	if (IS_ERR(graph->port)) {
		ret = PTR_ERR(graph->port);
		goto free_graph;
	}

	return graph;

free_graph:
	kfree(graph);
put_ar_graph:
	kref_put(&ar_graph->refcount, q6apm_put_audioreach_graph);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6apm_graph_open);

int q6apm_graph_close(struct q6apm_graph *graph)
{
	gpr_port_t *port;
	bool defer_close;

	mutex_lock(&graph->lifecycle_lock);
	if (graph->closing) {
		mutex_unlock(&graph->lifecycle_lock);
		return 0;
	}
	graph->closing = true;
	defer_close = graph->callback_task == current;
	graph->cb = NULL;
	graph->priv = NULL;
	port = graph->port;
	graph->port = NULL;
	mutex_unlock(&graph->lifecycle_lock);

	if (port)
		gpr_free_port(port);

	if (defer_close) {
		WARN_ON_ONCE(!schedule_work(&graph->close_work));
		return 0;
	}

	/* Wait for non-GPR users of callback_lock before releasing graph state. */
	mutex_lock(&graph->callback_lock);
	mutex_unlock(&graph->callback_lock);
	q6apm_graph_finish_close(graph);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_graph_close);

int q6apm_graph_prepare(struct q6apm_graph *graph)
{
	return audioreach_graph_mgmt_cmd(graph->ar_graph, APM_CMD_GRAPH_PREPARE);
}
EXPORT_SYMBOL_GPL(q6apm_graph_prepare);

int q6apm_graph_start(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	int ret = 0;

	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->flush_pending)
		ret = -EBUSY;
	else if (ar_graph->start_count == 0)
		ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_START);

	if (!ret)
		ar_graph->start_count++;
	mutex_unlock(&ar_graph->state_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_start);

static int __q6apm_graph_stop(struct q6apm_graph *graph, bool request_flush,
			      bool *last)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	int ret = 0;

	if (last)
		*last = false;

	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->start_count <= 0) {
		if (request_flush && ar_graph->flush_pending && last)
			*last = true;
		else
			ret = -EINVAL;
		goto unlock;
	}

	if (ar_graph->start_count > 1) {
		ar_graph->start_count--;
		goto unlock;
	}

	ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_STOP);
	if (!ret) {
		ar_graph->start_count = 0;
		ar_graph->flush_pending = request_flush;
		if (last)
			*last = true;
	}

unlock:
	mutex_unlock(&ar_graph->state_lock);
	return ret;
}

int q6apm_graph_stop_with_state(struct q6apm_graph *graph, bool *last)
{
	return __q6apm_graph_stop(graph, true, last);
}
EXPORT_SYMBOL_GPL(q6apm_graph_stop_with_state);

int q6apm_graph_stop(struct q6apm_graph *graph)
{
	return __q6apm_graph_stop(graph, false, NULL);
}
EXPORT_SYMBOL_GPL(q6apm_graph_stop);

bool q6apm_graph_request_flush(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	bool ready = false;

	mutex_lock(&ar_graph->state_lock);
	if (!ar_graph->start_count) {
		ar_graph->flush_pending = true;
		ready = true;
	}
	mutex_unlock(&ar_graph->state_lock);

	return ready;
}
EXPORT_SYMBOL_GPL(q6apm_graph_request_flush);

static void __q6apm_graph_reset_client(struct q6apm_graph *graph)
{
	q6apm_reset_graph_data(&graph->rx_data);
	q6apm_reset_graph_data(&graph->tx_data);
	graph->pending_eos_token = 0;
	graph->eos_pending = false;
	graph->flushing = false;
	wake_up_all(&graph->data_wait);
}

void q6apm_graph_reset_client(struct q6apm_graph *graph)
{
	mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	__q6apm_graph_reset_client(graph);
	mutex_unlock(&graph->lock);
	mutex_unlock(&graph->callback_lock);
}
EXPORT_SYMBOL_GPL(q6apm_graph_reset_client);

void q6apm_graph_quiesce_callbacks(struct q6apm_graph *graph)
{
	mutex_lock(&graph->callback_lock);
	mutex_unlock(&graph->callback_lock);
}
EXPORT_SYMBOL_GPL(q6apm_graph_quiesce_callbacks);

bool q6apm_graph_try_reset_client(struct q6apm_graph *graph)
{
	bool reset = false;

	mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	if (q6apm_graph_client_idle_locked(graph)) {
		__q6apm_graph_reset_client(graph);
		reset = true;
	}
	mutex_unlock(&graph->lock);
	mutex_unlock(&graph->callback_lock);

	return reset;
}
EXPORT_SYMBOL_GPL(q6apm_graph_try_reset_client);

static bool q6apm_graph_client_idle(struct q6apm_graph *graph)
{
	bool idle;

	mutex_lock(&graph->lock);
	idle = q6apm_graph_client_idle_locked(graph);
	mutex_unlock(&graph->lock);

	return idle;
}

int q6apm_graph_wait_for_client_idle(struct q6apm_graph *graph,
				     unsigned long timeout)
{
	if (!wait_event_timeout(graph->data_wait,
				q6apm_graph_client_idle(graph), timeout))
		return -ETIMEDOUT;

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_graph_wait_for_client_idle);

bool q6apm_graph_sync_flush_generation(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	bool changed = false;

	mutex_lock(&ar_graph->state_lock);
	if (graph->flush_generation != ar_graph->flush_generation) {
		mutex_lock(&graph->callback_lock);
		mutex_lock(&graph->lock);
		__q6apm_graph_reset_client(graph);
		graph->flush_generation = ar_graph->flush_generation;
		mutex_unlock(&graph->lock);
		mutex_unlock(&graph->callback_lock);
		changed = true;
	}
	mutex_unlock(&ar_graph->state_lock);

	return changed;
}
EXPORT_SYMBOL_GPL(q6apm_graph_sync_flush_generation);

int q6apm_graph_flush(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph = graph->ar_graph;
	int ret;

	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->start_count) {
		mutex_unlock(&ar_graph->state_lock);
		return -EINVAL;
	}
	if (!ar_graph->flush_pending) {
		mutex_unlock(&ar_graph->state_lock);
		return 0;
	}

	mutex_lock(&graph->lock);
	if (graph->flushing) {
		mutex_unlock(&graph->lock);
		mutex_unlock(&ar_graph->state_lock);
		return -EBUSY;
	}
	graph->flushing = true;
	mutex_unlock(&graph->lock);

	ret = audioreach_graph_mgmt_cmd(graph->ar_graph, APM_CMD_GRAPH_FLUSH);
	if (ret) {
		mutex_lock(&graph->lock);
		graph->flushing = false;
		mutex_unlock(&graph->lock);
		mutex_unlock(&ar_graph->state_lock);
		return ret;
	}
	ar_graph->flush_pending = false;
	ar_graph->flush_generation++;
	mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	__q6apm_graph_reset_client(graph);
	graph->flush_generation = ar_graph->flush_generation;
	mutex_unlock(&graph->lock);
	mutex_unlock(&graph->callback_lock);
	mutex_unlock(&ar_graph->state_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_graph_flush);

static int q6apm_audio_probe(struct snd_soc_component *component)
{
	return audioreach_tplg_init(component);
}

static void q6apm_audio_remove(struct snd_soc_component *component)
{
	struct q6apm *apm = dev_get_drvdata(component->dev);

	/* remove topology */
	snd_soc_tplg_component_remove(component);

	down_write(&apm->graph_lock);
	apm->topology_remove_complete = true;
	audioreach_tplg_free_deferred(apm);
	up_write(&apm->graph_lock);
}

#define APM_AUDIO_DRV_NAME "q6apm-audio"

static const struct snd_soc_component_driver q6apm_audio_component = {
	.name		= APM_AUDIO_DRV_NAME,
	.probe		= q6apm_audio_probe,
	.remove		= q6apm_audio_remove,
	.remove_order   = SND_SOC_COMP_ORDER_LAST,
};

static int apm_probe(gpr_device_t *gdev)
{
	struct device *dev = &gdev->dev;
	struct q6apm *apm;
	int ret;

	apm = kzalloc_obj(*apm);
	if (!apm)
		return -ENOMEM;

	kref_init(&apm->refcount);
	dev_set_drvdata(dev, apm);

	mutex_init(&apm->lock);
	mutex_init(&apm->online_lock);
	mutex_init(&apm->cmd_lock);
	mutex_init(&apm->response_lock);
	init_rwsem(&apm->graph_lock);
	init_waitqueue_head(&apm->graph_wait);
	atomic_set(&apm->active_graphs, 0);
	atomic_set(&apm->graph_generation, 0);
	apm->dev = dev;
	apm->gdev = gdev;
	apm->online = true;
	init_waitqueue_head(&apm->wait);

	INIT_LIST_HEAD(&apm->widget_list);
	idr_init(&apm->graph_idr);
	idr_init(&apm->graph_info_idr);
	idr_init(&apm->sub_graphs_idr);
	idr_init(&apm->containers_idr);

	idr_init(&apm->modules_idr);

	mutex_lock(&g_apm_lock);
	g_apm = apm;
	mutex_unlock(&g_apm_lock);

	ret = snd_soc_register_component(dev, &q6apm_audio_component, NULL, 0);
	if (ret < 0) {
		dev_err(dev, "failed to register q6apm: %d\n", ret);
		goto mark_offline;
	}

	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret) {
		of_platform_depopulate(dev);
		snd_soc_unregister_component(dev);
		goto mark_offline;
	}

	return 0;

mark_offline:
	mutex_lock(&g_apm_lock);
	if (g_apm == apm)
		g_apm = NULL;
	mutex_lock(&apm->online_lock);
	WRITE_ONCE(apm->online, false);
	mutex_unlock(&apm->online_lock);
	mutex_unlock(&g_apm_lock);
	dev_set_drvdata(dev, NULL);
	kref_put(&apm->refcount, q6apm_release);
	return ret;
}

static void apm_remove(gpr_device_t *gdev)
{
	struct q6apm *apm = dev_get_drvdata(&gdev->dev);

	mutex_lock(&g_apm_lock);
	if (g_apm == apm)
		g_apm = NULL;
	mutex_lock(&apm->online_lock);
	WRITE_ONCE(apm->online, false);
	mutex_unlock(&apm->online_lock);
	mutex_unlock(&g_apm_lock);

	of_platform_depopulate(&gdev->dev);
	snd_soc_unregister_component(&gdev->dev);
	dev_set_drvdata(&gdev->dev, NULL);
	kref_put(&apm->refcount, q6apm_release);
}

struct audioreach_module *q6apm_find_module_by_mid(struct q6apm_graph *graph, uint32_t mid)
{
	struct audioreach_graph_info *info = graph->info;
	struct q6apm *apm = graph->apm;

	return __q6apm_find_module_by_mid(apm, info, mid);

}

static int apm_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	gpr_device_t *gdev = priv;
	struct audioreach_graph_info *info;
	struct q6apm *apm = dev_get_drvdata(&gdev->dev);
	struct apm_cmd_rsp_shared_mem_map_regions *rsp;
	struct device *dev = &gdev->dev;
	struct gpr_ibasic_rsp_result_t *result;
	const struct gpr_hdr *hdr = &data->hdr;
	uint32_t expected_opcode;
	uint32_t graph_id;
	bool matches;

	if (!apm)
		return 0;

	result = data->payload;
	mutex_lock(&apm->response_lock);
	matches = apm->response_pending && hdr->token == apm->pending_token;
	expected_opcode = apm->pending_opcode;
	graph_id = apm->pending_graph_id;
	if (!matches) {
		mutex_unlock(&apm->response_lock);
		dev_dbg(dev, "ignoring stale APM response 0x%08x token %u\n",
			hdr->opcode, hdr->token);
		return 0;
	}

	switch (hdr->opcode) {
	case APM_CMD_RSP_GET_SPF_STATE:
		apm->result.opcode = hdr->opcode;
		apm->result.status =
			expected_opcode != APM_CMD_GET_SPF_STATE ||
			data->payload_size < sizeof(result->opcode);
		if (apm->result.status) {
			dev_err(dev, "short APM state response: %d bytes\n",
				data->payload_size);
			wake_up(&apm->wait);
			break;
		}
		/* First word of result it state */
		apm->state = result->opcode;
		wake_up(&apm->wait);
		break;
	case GPR_BASIC_RSP_RESULT:
		if (data->payload_size < sizeof(*result)) {
			dev_err(dev, "short APM basic response: %d bytes\n",
				data->payload_size);
			apm->result.opcode = expected_opcode;
			apm->result.status = 1;
			wake_up(&apm->wait);
			break;
		}
		if (result->opcode != expected_opcode) {
			dev_err(dev,
				"APM response opcode mismatch: expected 0x%08x got 0x%08x\n",
				expected_opcode, result->opcode);
			apm->result.opcode = expected_opcode;
			apm->result.status = 1;
			wake_up(&apm->wait);
			break;
		}
		switch (result->opcode) {
		case APM_CMD_SHARED_MEM_MAP_REGIONS:
		case APM_CMD_GRAPH_START:
		case APM_CMD_GRAPH_OPEN:
		case APM_CMD_GRAPH_PREPARE:
		case APM_CMD_GRAPH_CLOSE:
		case APM_CMD_GRAPH_FLUSH:
		case APM_CMD_GRAPH_STOP:
		case APM_CMD_SET_CFG:
			apm->result.opcode = result->opcode;
			apm->result.status = result->status;
			if (result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n", result->status,
					result->opcode);
			wake_up(&apm->wait);
			break;
		case APM_CMD_SHARED_MEM_UNMAP_REGIONS:
			apm->result.opcode = result->opcode;
			apm->result.status = result->status;

			mutex_lock(&apm->lock);
			info = idr_find(&apm->graph_info_idr, graph_id);
			if (info && !result->status) {
				info->mem_map_handle = 0;
			} else if (!info) {
				apm->result.status = 1;
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n", result->status,
					result->opcode);
			}
			mutex_unlock(&apm->lock);
			if (result->status)
				dev_err(dev, "Error (%d) Processing 0x%08x cmd\n",
					result->status, result->opcode);

			wake_up(&apm->wait);
			break;
		default:
			break;
		}
		break;
	case APM_CMD_RSP_SHARED_MEM_MAP_REGIONS:
		apm->result.opcode = hdr->opcode;
		apm->result.status =
			expected_opcode != APM_CMD_SHARED_MEM_MAP_REGIONS ||
			data->payload_size < sizeof(*rsp);
		if (apm->result.status) {
			dev_err(dev, "short APM map response: %d bytes\n",
				data->payload_size);
			wake_up(&apm->wait);
			break;
		}
		rsp = data->payload;

		mutex_lock(&apm->lock);
		info = idr_find(&apm->graph_info_idr, graph_id);
		if (info && rsp->mem_map_handle) {
			info->mem_map_handle = rsp->mem_map_handle;
		} else {
			apm->result.status = 1;
			dev_err(dev, "invalid map response for graph %u\n",
				graph_id);
		}
		mutex_unlock(&apm->lock);

		wake_up(&apm->wait);
		break;
	default:
		break;
	}

	mutex_unlock(&apm->response_lock);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id apm_device_id[]  = {
	{ .compatible = "qcom,q6apm" },
	{},
};
MODULE_DEVICE_TABLE(of, apm_device_id);
#endif

static gpr_driver_t apm_driver = {
	.probe = apm_probe,
	.remove = apm_remove,
	.gpr_callback = apm_callback,
	.driver = {
		.name = "qcom-apm",
		.of_match_table = of_match_ptr(apm_device_id),
	},
};

module_gpr_driver(apm_driver);
MODULE_DESCRIPTION("Audio Process Manager");
MODULE_LICENSE("GPL");
