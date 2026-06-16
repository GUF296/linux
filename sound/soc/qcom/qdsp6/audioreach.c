// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <dt-bindings/soc/qcom,gpr.h>
#include "q6apm.h"
#include "audioreach.h"

/*
 * Y700 daily AudioReach endpoint defaults.
 *
 * -1 keeps the normal topology/runtime-derived value. Non-negative values are
 * the accepted Y700-specific payload fields that are still needed for the
 * daily route.
 */
static const int y700_i2s_lpaif_type = -1;

static const int y700_i2s_intf_idx = -1;

static const int y700_i2s_sd_line_idx = -1;

static const int y700_i2s_ws_src = -1;

static const int y700_i2s_frame_size_factor = -1;

static const int y700_mfc_force_rate = -1;

static const int y700_mfc_force_channels = -1;

static const int y700_mfc_force_bit_width = -1;

static const int y700_tdm_lpaif_type = -1;

static const int y700_tdm_intf_idx = -1;

static const int y700_tdm_sync_src = -1;

static const int y700_tdm_ctrl_data_oe = -1;

static const int y700_tdm_slot_mask = -1;

static const int y700_tdm_nslots = -1;

static const int y700_tdm_slot_width = -1;

static const int y700_tdm_sync_mode = -1;

static const int y700_tdm_invert_sync = -1;

static const int y700_tdm_sync_delay = -1;

static const int y700_tdm_lane_mask = -1;

static const int y700_tdm_frame_size_factor = -1;

static const int y700_tdm_media_rate = -1;

static const int y700_tdm_media_channels = -1;

static const int y700_tdm_media_bit_width = -1;

/*
 * Bit mask for TDM endpoint params:
 *   bit0 MF, bit1 FSF, bit2 INTF, bit3 LANE, bit4 CLK, bit5 POWER, bit6 DMA_ALIGN.
 * Default keeps the current known path: MF + FSF + INTF + LANE + CLK.
 */
static const int y700_tdm_cfg_mask = 0x1f;

static const int y700_tdm_combined_setcfg;

static const int y700_tdm_clk_attr = -1;

static const int y700_tdm_clk_root = -1;

static const int y700_tdm_clk_freq = -1;

static const int y700_tdm_power_mode = -1;

static const int y700_tdm_dma_align = -1;

static const int y700_spr_delay_path_end_iid = 0x4023;

static const int y700_spr_delay_path_graph = 0;

/* SubGraph Config */
struct apm_sub_graph_data {
	struct apm_sub_graph_cfg sub_graph_cfg;
	struct apm_prop_data perf_data;
	struct apm_sg_prop_id_perf_mode perf;
	struct apm_prop_data dir_data;
	struct apm_sg_prop_id_direction dir;
	struct apm_prop_data sid_data;
	struct apm_sg_prop_id_scenario_id sid;

} __packed;

#define APM_SUB_GRAPH_CFG_NPROP	3

struct apm_sub_graph_params  {
	struct apm_module_param_data param_data;
	uint32_t num_sub_graphs;
	struct apm_sub_graph_data sg_cfg[];
} __packed;

#define APM_SUB_GRAPH_PSIZE(p, n) ALIGN(struct_size(p, sg_cfg, n), 8)

/* container config */
struct apm_container_obj  {
	struct apm_container_cfg container_cfg;
	/* Capability ID list */
	struct apm_prop_data cap_data;
	uint32_t num_capability_id;
	uint32_t capability_id;

	/* Container graph Position */
	struct apm_prop_data pos_data;
	struct apm_cont_prop_id_graph_pos pos;

	/* Container Stack size */
	struct apm_prop_data stack_data;
	struct apm_cont_prop_id_stack_size stack;

	/* Container proc domain id */
	struct apm_prop_data domain_data;
	struct apm_cont_prop_id_domain domain;
} __packed;

struct apm_container_params  {
	struct apm_module_param_data param_data;
	uint32_t num_containers;
	struct apm_container_obj cont_obj[];
} __packed;

#define APM_CONTAINER_PSIZE(p, n) ALIGN(struct_size(p, cont_obj, n), 8)

/* Module List config */
struct apm_mod_list_obj {
	/* Modules list cfg */
	uint32_t sub_graph_id;
	uint32_t container_id;
	uint32_t num_modules;
	struct apm_module_obj mod_cfg[];
} __packed;

#define APM_MOD_LIST_OBJ_PSIZE(p, n) struct_size(p, mod_cfg, n)

struct apm_module_list_params {
	struct apm_module_param_data param_data;
	uint32_t num_modules_list;
	/* Module list config array */
	struct apm_mod_list_obj mod_list_obj[];
} __packed;


/* Module Properties */
struct apm_mod_prop_obj {
	u32 instance_id;
	u32 num_props;
	struct apm_prop_data prop_data_1;
	struct apm_module_prop_id_port_info prop_id_port;
} __packed;

struct apm_prop_list_params {
	struct apm_module_param_data param_data;
	u32 num_modules_prop_cfg;
	struct apm_mod_prop_obj mod_prop_obj[];

} __packed;

#define APM_MOD_PROP_PSIZE(p, n) ALIGN(struct_size(p, mod_prop_obj, n), 8)

/* Module Connections */
struct apm_mod_conn_list_params {
	struct apm_module_param_data param_data;
	u32 num_connections;
	struct apm_module_conn_obj conn_obj[];

} __packed;

#define APM_MOD_CONN_PSIZE(p, n) ALIGN(struct_size(p, conn_obj, n), 8)

struct apm_graph_open_params {
	struct apm_cmd_header *cmd_header;
	struct apm_sub_graph_params *sg_data;
	struct apm_container_params *cont_data;
	struct apm_module_list_params *mod_list_data;
	struct apm_prop_list_params *mod_prop_data;
	struct apm_mod_conn_list_params *mod_conn_list_data;
} __packed;

struct apm_pcm_module_media_fmt_cmd {
	struct apm_module_param_data param_data;
	struct param_id_pcm_output_format_cfg header;
	struct payload_pcm_output_format_cfg media_cfg;
} __packed;

struct apm_rd_shmem_module_config_cmd {
	struct apm_module_param_data param_data;
	struct param_id_rd_sh_mem_cfg cfg;
} __packed;

struct apm_sh_module_media_fmt_cmd {
	struct media_format header;
	struct payload_media_fmt_pcm cfg;
} __packed;

#define APM_SHMEM_FMT_CFG_PSIZE(ch) ALIGN( \
				sizeof(struct apm_sh_module_media_fmt_cmd) + \
				ch * sizeof(uint8_t), 8)

/* num of channels as argument */
#define APM_PCM_MODULE_FMT_CMD_PSIZE(ch) ALIGN( \
				sizeof(struct apm_pcm_module_media_fmt_cmd) + \
				ch * sizeof(uint8_t), 8)

#define APM_PCM_OUT_FMT_CFG_PSIZE(p, n) ALIGN(struct_size(p, channel_mapping, n), 4)

struct apm_i2s_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_i2s_intf_cfg cfg;
} __packed;

#define APM_I2S_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_i2s_module_intf_cfg), 8)

struct apm_tdm_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_tdm_intf_cfg cfg;
} __packed;

#define APM_TDM_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_tdm_module_intf_cfg), 8)

struct apm_tdm_module_lane_cfg {
	struct apm_module_param_data param_data;
	struct param_id_tdm_lane_cfg cfg;
} __packed;

#define APM_TDM_LANE_CFG_PSIZE ALIGN(sizeof(struct apm_tdm_module_lane_cfg), 8)

struct apm_module_hw_ep_mf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_ep_mf mf;
} __packed;

#define APM_HW_EP_CFG_PSIZE ALIGN(sizeof(struct apm_module_hw_ep_mf_cfg), 8)

#define APM_MFC_CFG_PSIZE(p, n) ALIGN(struct_size(p, channel_mapping, n), 4)

struct apm_module_frame_size_factor_cfg {
	struct apm_module_param_data param_data;
	uint32_t frame_size_factor;
} __packed;

#define APM_FS_CFG_PSIZE ALIGN(sizeof(struct apm_module_frame_size_factor_cfg), 8)

struct apm_module_hw_intf_clk_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_intf_clk_cfg cfg;
} __packed;

#define APM_HW_INTF_CLK_CFG_PSIZE \
	ALIGN(sizeof(struct apm_module_hw_intf_clk_cfg), 8)

struct apm_module_spr_delay_path_end_cfg {
	struct apm_module_param_data param_data;
	uint32_t module_instance_id;
} __packed;

#define APM_SPR_DELAY_PATH_END_CFG_PSIZE \
	ALIGN(sizeof(struct apm_module_spr_delay_path_end_cfg), 8)

struct apm_module_hw_ep_power_mode_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_ep_power_mode_cfg power_mode;
} __packed;

#define APM_HW_EP_PMODE_CFG_PSIZE ALIGN(sizeof(struct apm_module_hw_ep_power_mode_cfg),	8)

struct apm_module_hw_ep_dma_data_align_cfg {
	struct apm_module_param_data param_data;
	struct param_id_hw_ep_dma_data_align align;
} __packed;

#define APM_HW_EP_DALIGN_CFG_PSIZE ALIGN(sizeof(struct apm_module_hw_ep_dma_data_align_cfg), 8)

struct apm_gain_module_cfg {
	struct apm_module_param_data param_data;
	struct param_id_gain_cfg gain_cfg;
} __packed;

#define APM_GAIN_CFG_PSIZE ALIGN(sizeof(struct apm_gain_module_cfg), 8)

struct apm_codec_dma_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_codec_dma_intf_cfg cfg;
} __packed;

#define APM_CDMA_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_codec_dma_module_intf_cfg), 8)

struct apm_display_port_module_intf_cfg {
	struct apm_module_param_data param_data;
	struct param_id_display_port_intf_cfg cfg;
} __packed;
#define APM_DP_INTF_CFG_PSIZE ALIGN(sizeof(struct apm_display_port_module_intf_cfg), 8)

struct apm_module_sp_vi_op_mode_cfg {
	struct apm_module_param_data param_data;
	struct param_id_sp_vi_op_mode_cfg cfg;
} __packed;

#define APM_SP_VI_OP_MODE_CFG_PSIZE(ch) ALIGN( \
				sizeof(struct apm_module_sp_vi_op_mode_cfg) + \
				(ch) * sizeof(uint32_t), 8)

struct apm_module_sp_vi_ex_mode_cfg {
	struct apm_module_param_data param_data;
	struct param_id_sp_vi_ex_mode_cfg cfg;
} __packed;

#define APM_SP_VI_EX_MODE_CFG_PSIZE ALIGN(sizeof(struct apm_module_sp_vi_ex_mode_cfg), 8)

struct apm_module_sp_vi_channel_map_cfg {
	struct apm_module_param_data param_data;
	struct param_id_sp_vi_channel_map_cfg cfg;
} __packed;

#define APM_SP_VI_CH_MAP_CFG_PSIZE(ch) ALIGN( \
				sizeof(struct apm_module_sp_vi_channel_map_cfg) + \
				(ch) * sizeof(uint32_t), 8)

static void *__audioreach_alloc_pkt(int payload_size, uint32_t opcode, uint32_t token,
				    uint32_t src_port, uint32_t dest_port, bool has_cmd_hdr)
{
	struct gpr_pkt *pkt;
	void *p;
	int pkt_size = GPR_HDR_SIZE + payload_size;

	if (has_cmd_hdr)
		pkt_size += APM_CMD_HDR_SIZE;

	p = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	pkt = p;
	pkt->hdr.version = GPR_PKT_VER;
	pkt->hdr.hdr_size = GPR_PKT_HEADER_WORD_SIZE;
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.dest_port = dest_port;
	pkt->hdr.src_port = src_port;

	pkt->hdr.dest_domain = GPR_DOMAIN_ID_ADSP;
	pkt->hdr.src_domain = GPR_DOMAIN_ID_APPS;
	pkt->hdr.token = token;
	pkt->hdr.opcode = opcode;

	if (has_cmd_hdr) {
		struct apm_cmd_header *cmd_header;

		p = p + GPR_HDR_SIZE;
		cmd_header = p;
		cmd_header->payload_size = payload_size;
	}

	return pkt;
}

void *audioreach_alloc_pkt(int payload_size, uint32_t opcode, uint32_t token,
			   uint32_t src_port, uint32_t dest_port)
{
	return __audioreach_alloc_pkt(payload_size, opcode, token, src_port, dest_port, false);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_pkt);

void *audioreach_alloc_apm_pkt(int pkt_size, uint32_t opcode, uint32_t token, uint32_t src_port)
{
	return __audioreach_alloc_pkt(pkt_size, opcode, token, src_port, APM_MODULE_INSTANCE_ID,
				      false);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_apm_pkt);

void *audioreach_alloc_cmd_pkt(int payload_size, uint32_t opcode, uint32_t token,
			       uint32_t src_port, uint32_t dest_port)
{
	return __audioreach_alloc_pkt(payload_size, opcode, token, src_port, dest_port, true);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_cmd_pkt);

void *audioreach_alloc_apm_cmd_pkt(int pkt_size, uint32_t opcode, uint32_t token)
{
	return __audioreach_alloc_pkt(pkt_size, opcode, token, GPR_APM_MODULE_IID,
				       APM_MODULE_INSTANCE_ID, true);
}
EXPORT_SYMBOL_GPL(audioreach_alloc_apm_cmd_pkt);

void audioreach_set_default_channel_mapping(u8 *ch_map, int num_channels)
{
	if (num_channels == 1) {
		ch_map[0] =  PCM_CHANNEL_FL;
	} else if (num_channels == 2) {
		ch_map[0] =  PCM_CHANNEL_FL;
		ch_map[1] =  PCM_CHANNEL_FR;
	} else if (num_channels == 4) {
		ch_map[0] =  PCM_CHANNEL_FL;
		ch_map[1] =  PCM_CHANNEL_FR;
		ch_map[2] =  PCM_CHANNEL_LS;
		ch_map[3] =  PCM_CHANNEL_RS;
	} else if (num_channels == 6) {
		ch_map[0] =  PCM_CHANNEL_FL;
		ch_map[1] =  PCM_CHANNEL_FR;
		ch_map[2] =  PCM_CHANNEL_LFE;
		ch_map[3] =  PCM_CHANNEL_FC;
		ch_map[4] =  PCM_CHANNEL_LS;
		ch_map[5] =  PCM_CHANNEL_RS;
	} else if (num_channels == 8) {
		ch_map[0] =  PCM_CHANNEL_FL;
		ch_map[1] =  PCM_CHANNEL_FR;
		ch_map[2] =  PCM_CHANNEL_LFE;
		ch_map[3] =  PCM_CHANNEL_FC;
		ch_map[4] =  PCM_CHANNEL_LS;
		ch_map[5] =  PCM_CHANNEL_RS;
		ch_map[6] =  PCM_CHANNEL_LB;
		ch_map[7] =  PCM_CHANNEL_RB;
	}
}
EXPORT_SYMBOL_GPL(audioreach_set_default_channel_mapping);

static void apm_populate_container_config(struct apm_container_obj *cfg,
					  const struct audioreach_container *cont)
{

	/* Container Config */
	cfg->container_cfg.container_id = cont->container_id;
	cfg->container_cfg.num_prop = 4;

	/* Capability list */
	cfg->cap_data.prop_id = APM_CONTAINER_PROP_ID_CAPABILITY_LIST;
	cfg->cap_data.prop_size = APM_CONTAINER_PROP_ID_CAPABILITY_SIZE;
	cfg->num_capability_id = 1;
	cfg->capability_id = cont->capability_id;

	/* Graph Position */
	cfg->pos_data.prop_id = APM_CONTAINER_PROP_ID_GRAPH_POS;
	cfg->pos_data.prop_size = sizeof(struct apm_cont_prop_id_graph_pos);
	cfg->pos.graph_pos = cont->graph_pos;

	/* Stack size */
	cfg->stack_data.prop_id = APM_CONTAINER_PROP_ID_STACK_SIZE;
	cfg->stack_data.prop_size = sizeof(struct apm_cont_prop_id_stack_size);
	cfg->stack.stack_size = cont->stack_size;

	/* Proc domain */
	cfg->domain_data.prop_id = APM_CONTAINER_PROP_ID_PROC_DOMAIN;
	cfg->domain_data.prop_size = sizeof(struct apm_cont_prop_id_domain);
	cfg->domain.proc_domain = cont->proc_domain;
}

static void apm_populate_sub_graph_config(struct apm_sub_graph_data *cfg,
					  const struct audioreach_sub_graph *sg)
{
	cfg->sub_graph_cfg.sub_graph_id = sg->sub_graph_id;
	cfg->sub_graph_cfg.num_sub_graph_prop = APM_SUB_GRAPH_CFG_NPROP;

	/* Perf Mode */
	cfg->perf_data.prop_id = APM_SUB_GRAPH_PROP_ID_PERF_MODE;
	cfg->perf_data.prop_size = APM_SG_PROP_ID_PERF_MODE_SIZE;
	cfg->perf.perf_mode = sg->perf_mode;

	/* Direction */
	cfg->dir_data.prop_id = APM_SUB_GRAPH_PROP_ID_DIRECTION;
	cfg->dir_data.prop_size = APM_SG_PROP_ID_DIR_SIZE;
	cfg->dir.direction = sg->direction;

	/* Scenario ID */
	cfg->sid_data.prop_id = APM_SUB_GRAPH_PROP_ID_SCENARIO_ID;
	cfg->sid_data.prop_size = APM_SG_PROP_ID_SID_SIZE;
	cfg->sid.scenario_id = sg->scenario_id;
}

static void apm_populate_module_prop_obj(struct apm_mod_prop_obj *obj,
					 const struct audioreach_module *module)
{

	obj->instance_id = module->instance_id;
	obj->num_props = 1;
	obj->prop_data_1.prop_id = APM_MODULE_PROP_ID_PORT_INFO;
	obj->prop_data_1.prop_size = APM_MODULE_PROP_ID_PORT_INFO_SZ;
	obj->prop_id_port.max_ip_port = module->max_ip_port;
	obj->prop_id_port.max_op_port = module->max_op_port;
}

static void apm_populate_module_list_obj(struct apm_mod_list_obj *obj,
					 const struct audioreach_container *container,
					 int sub_graph_id)
{
	struct audioreach_module *module;
	int i;

	obj->sub_graph_id = sub_graph_id;
	obj->container_id = container->container_id;
	obj->num_modules = container->num_modules;
	i = 0;
	list_for_each_entry(module, &container->modules_list, node) {
		obj->mod_cfg[i].module_id = module->module_id;
		obj->mod_cfg[i].instance_id = module->instance_id;
		i++;
	}
}

static void audioreach_populate_graph(struct q6apm *apm,
				      const struct audioreach_graph_info *info,
				      struct apm_graph_open_params *open,
				      const struct list_head *sg_list,
				      int num_sub_graphs)
{
	struct apm_mod_conn_list_params *mc_data = open->mod_conn_list_data;
	struct apm_module_list_params *ml_data = open->mod_list_data;
	struct apm_prop_list_params *mp_data = open->mod_prop_data;
	struct apm_container_params *c_data = open->cont_data;
	struct apm_sub_graph_params *sg_data = open->sg_data;
	int ncontainer = 0, nmodule = 0, nconn = 0;
	struct apm_mod_prop_obj *module_prop_obj;
	struct audioreach_container *container;
	struct apm_module_conn_obj *conn_obj;
	struct audioreach_module *module;
	struct audioreach_sub_graph *sg;
	struct apm_container_obj *cobj;
	struct apm_mod_list_obj *mlobj;
	int i = 0;

	mlobj = &ml_data->mod_list_obj[0];


	if (info->dst_mod_inst_id && info->src_mod_inst_id) {
		conn_obj = &mc_data->conn_obj[nconn];
		conn_obj->src_mod_inst_id = info->src_mod_inst_id;
		conn_obj->src_mod_op_port_id = info->src_mod_op_port_id;
		conn_obj->dst_mod_inst_id = info->dst_mod_inst_id;
		conn_obj->dst_mod_ip_port_id = info->dst_mod_ip_port_id;
		nconn++;
	}

	list_for_each_entry(sg, sg_list, node) {
		struct apm_sub_graph_data *sg_cfg = &sg_data->sg_cfg[i++];

		apm_populate_sub_graph_config(sg_cfg, sg);

		list_for_each_entry(container, &sg->container_list, node) {
			cobj = &c_data->cont_obj[ncontainer];

			apm_populate_container_config(cobj, container);
			apm_populate_module_list_obj(mlobj, container, sg->sub_graph_id);

			list_for_each_entry(module, &container->modules_list, node) {
				int pn;

				module_prop_obj = &mp_data->mod_prop_obj[nmodule++];
				apm_populate_module_prop_obj(module_prop_obj, module);

				if (!module->max_op_port)
					continue;

				for (pn = 0; pn < module->max_op_port; pn++) {
					if (module->dst_mod_inst_id[pn]) {
						conn_obj = &mc_data->conn_obj[nconn];
						conn_obj->src_mod_inst_id = module->instance_id;
						conn_obj->src_mod_op_port_id =
								module->src_mod_op_port_id[pn];
						conn_obj->dst_mod_inst_id =
								module->dst_mod_inst_id[pn];
						conn_obj->dst_mod_ip_port_id =
								module->dst_mod_ip_port_id[pn];
						nconn++;
					}
				}
			}
			mlobj = (void *) mlobj + APM_MOD_LIST_OBJ_PSIZE(mlobj,
									container->num_modules);

			ncontainer++;
		}
	}
}

void *audioreach_alloc_graph_pkt(struct q6apm *apm,
				 const struct audioreach_graph_info *info)
{
	int payload_size, sg_sz, cont_sz, ml_sz, mp_sz, mc_sz;
	struct apm_module_param_data  *param_data;
	struct apm_container_params *cont_params;
	struct audioreach_container *container;
	struct apm_sub_graph_params *sg_params;
	struct apm_mod_conn_list_params *mcon;
	struct apm_graph_open_params params;
	struct apm_prop_list_params *mprop;
	struct audioreach_module *module;
	struct audioreach_sub_graph *sgs;
	struct apm_mod_list_obj *mlobj;
	const struct list_head *sg_list;
	int num_connections = 0;
	int num_containers = 0;
	int num_sub_graphs = 0;
	int num_modules = 0;
	int num_modules_list;
	struct gpr_pkt *pkt;
	void *p;

	sg_list = &info->sg_list;
	ml_sz = 0;

	/* add FE-BE connections */
	if (info->dst_mod_inst_id && info->src_mod_inst_id)
		num_connections++;

	list_for_each_entry(sgs, sg_list, node) {
		num_sub_graphs++;
		list_for_each_entry(container, &sgs->container_list, node) {
			num_containers++;
			num_modules += container->num_modules;
			ml_sz = ml_sz + sizeof(struct apm_module_list_params) +
				APM_MOD_LIST_OBJ_PSIZE(mlobj, container->num_modules);

			list_for_each_entry(module, &container->modules_list, node) {
				num_connections += module->num_connections;
			}
		}
	}

	num_modules_list = num_containers;
	sg_sz = APM_SUB_GRAPH_PSIZE(sg_params, num_sub_graphs);
	cont_sz = APM_CONTAINER_PSIZE(cont_params, num_containers);

	ml_sz = ALIGN(ml_sz, 8);

	mp_sz = APM_MOD_PROP_PSIZE(mprop, num_modules);
	mc_sz =	APM_MOD_CONN_PSIZE(mcon, num_connections);

	payload_size = sg_sz + cont_sz + ml_sz + mp_sz + mc_sz;
	pkt = audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_GRAPH_OPEN, 0);
	if (IS_ERR(pkt))
		return pkt;

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	/* SubGraph */
	params.sg_data = p;
	param_data = &params.sg_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_SUB_GRAPH_CONFIG;
	param_data->param_size = sg_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.sg_data->num_sub_graphs = num_sub_graphs;
	p += sg_sz;

	/* Container */
	params.cont_data = p;
	param_data = &params.cont_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_CONTAINER_CONFIG;
	param_data->param_size = cont_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.cont_data->num_containers = num_containers;
	p += cont_sz;

	/* Module List*/
	params.mod_list_data = p;
	param_data = &params.mod_list_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_MODULE_LIST;
	param_data->param_size = ml_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.mod_list_data->num_modules_list = num_modules_list;
	p += ml_sz;

	/* Module Properties */
	params.mod_prop_data = p;
	param_data = &params.mod_prop_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_MODULE_PROP;
	param_data->param_size = mp_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.mod_prop_data->num_modules_prop_cfg = num_modules;
	p += mp_sz;

	/* Module Connections */
	params.mod_conn_list_data = p;
	param_data = &params.mod_conn_list_data->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_MODULE_CONN;
	param_data->param_size = mc_sz - APM_MODULE_PARAM_DATA_SIZE;
	params.mod_conn_list_data->num_connections = num_connections;
	p += mc_sz;

	audioreach_populate_graph(apm, info, &params, sg_list, num_sub_graphs);

	return pkt;
}
EXPORT_SYMBOL_GPL(audioreach_alloc_graph_pkt);

int audioreach_send_cmd_sync(struct device *dev, gpr_device_t *gdev,
			     struct gpr_ibasic_rsp_result_t *result, struct mutex *cmd_lock,
			     gpr_port_t *port, wait_queue_head_t *cmd_wait,
			     const struct gpr_pkt *pkt, uint32_t rsp_opcode)
{

	const struct gpr_hdr *hdr = &pkt->hdr;
	int rc;

	mutex_lock(cmd_lock);
	result->opcode = 0;
	result->status = 0;

	if (port)
		rc = gpr_send_port_pkt(port, pkt);
	else if (gdev)
		rc = gpr_send_pkt(gdev, pkt);
	else
		rc = -EINVAL;

	if (rc < 0)
		goto err;

	if (rsp_opcode)
		rc = wait_event_timeout(*cmd_wait, (result->opcode == hdr->opcode) ||
					(result->opcode == rsp_opcode),	5 * HZ);
	else
		rc = wait_event_timeout(*cmd_wait, (result->opcode == hdr->opcode), 5 * HZ);

	if (!rc) {
		dev_err(dev, "CMD timeout for [%x] opcode\n", hdr->opcode);
		rc = -ETIMEDOUT;
	} else if (result->status > 0) {
		dev_err(dev, "DSP returned error[%x] %x\n", hdr->opcode, result->status);
		rc = -EINVAL;
	} else {
		/* DSP successfully finished the command */
		rc = 0;
	}

err:
	mutex_unlock(cmd_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(audioreach_send_cmd_sync);

int audioreach_graph_send_cmd_sync(struct q6apm_graph *graph, const struct gpr_pkt *pkt,
				   uint32_t rsp_opcode)
{

	return audioreach_send_cmd_sync(graph->dev, NULL,  &graph->result, &graph->lock,
					graph->port, &graph->cmd_wait, pkt, rsp_opcode);
}
EXPORT_SYMBOL_GPL(audioreach_graph_send_cmd_sync);

static int audioreach_display_port_set_media_format(struct q6apm_graph *graph,
						    const struct audioreach_module *module,
						    const struct audioreach_module_config *cfg)
{
	struct apm_display_port_module_intf_cfg *intf_cfg;
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_module_param_data *param_data;
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	int ic_sz = APM_DP_INTF_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int size = ic_sz + ep_sz + fs_sz;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	hw_cfg = p;
	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = cfg->sample_rate;
	hw_cfg->mf.bit_width = cfg->bit_width;
	hw_cfg->mf.num_channels = cfg->num_channels;
	hw_cfg->mf.data_format = module->data_format;
	p += ep_sz;

	fs_cfg = p;
	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	fs_cfg->frame_size_factor = cfg->frame_size_factor ?:
				    module->frame_size_factor ?: 1;
	p += fs_sz;

	intf_cfg = p;
	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_DISPLAY_PORT_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.channel_allocation = cfg->channel_allocation;
	intf_cfg->cfg.mst_idx = 0;
	intf_cfg->cfg.dptx_idx = cfg->dp_idx;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

/* LPASS Codec DMA port Module Media Format Setup */
static int audioreach_codec_dma_set_media_format(struct q6apm_graph *graph,
						 const struct audioreach_module *module,
						 const struct audioreach_module_config *cfg)
{
	struct apm_codec_dma_module_intf_cfg *intf_cfg;
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_module_hw_ep_power_mode_cfg *pm_cfg;
	struct apm_module_param_data *param_data;
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	int ic_sz = APM_CDMA_INTF_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int pm_sz = APM_HW_EP_PMODE_CFG_PSIZE;
	int size = ic_sz + ep_sz + fs_sz + pm_sz;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	hw_cfg = p;
	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = cfg->sample_rate;
	hw_cfg->mf.bit_width = cfg->bit_width;
	hw_cfg->mf.num_channels = cfg->num_channels;
	hw_cfg->mf.data_format = module->data_format;
	p += ep_sz;

	fs_cfg = p;
	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	fs_cfg->frame_size_factor = cfg->frame_size_factor ?:
				    module->frame_size_factor ?: 1;
	p += fs_sz;

	intf_cfg = p;
	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_CODEC_DMA_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.lpaif_type = module->hw_interface_type;
	intf_cfg->cfg.intf_index = module->hw_interface_idx;
	intf_cfg->cfg.active_channels_mask = (1 << cfg->num_channels) - 1;
	p += ic_sz;

	pm_cfg = p;
	param_data = &pm_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_POWER_MODE_CFG;
	param_data->param_size = pm_sz - APM_MODULE_PARAM_DATA_SIZE;
	pm_cfg->power_mode.power_mode = 0;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

int audioreach_send_u32_param(struct q6apm_graph *graph,
			      const struct audioreach_module *module,
			      uint32_t param_id, uint32_t param_val)
{
	struct apm_module_param_data *param_data;
	uint32_t *param;
	int payload_size = sizeof(uint32_t) + APM_MODULE_PARAM_DATA_SIZE;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return -ENOMEM;

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = param_id;
	param_data->param_size = sizeof(uint32_t);

	p = p + APM_MODULE_PARAM_DATA_SIZE;
	param = p;
	*param = param_val;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}
EXPORT_SYMBOL_GPL(audioreach_send_u32_param);

static int audioreach_sal_limiter_enable(struct q6apm_graph *graph,
					 const struct audioreach_module *module,
					 bool enable)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_SAL_LIMITER_ENABLE, enable);
}

static int audioreach_sal_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_SAL_OUTPUT_CFG,  cfg->bit_width);
}

static int audioreach_module_enable(struct q6apm_graph *graph,
				    const struct audioreach_module *module,
				    bool enable)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_MODULE_ENABLE, enable);
}

static int audioreach_gapless_set_media_format(struct q6apm_graph *graph,
					       const struct audioreach_module *module,
					       const struct audioreach_module_config *cfg)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_EARLY_EOS_DELAY,
					 EARLY_EOS_DELAY_MS);
}

static int audioreach_set_module_config(struct q6apm_graph *graph,
					const struct audioreach_module *module,
					const struct audioreach_module_config *cfg)
{
	int size = le32_to_cpu(module->data->size);
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	memcpy(p, module->data->data, size);

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_mfc_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	struct apm_module_param_data *param_data;
	struct param_id_mfc_media_format *media_format;
	uint32_t num_channels = cfg->num_channels;
	uint32_t sample_rate = cfg->sample_rate;
	uint16_t bit_width = cfg->bit_width;
	int payload_size;
	int i;
	void *p;

	if (y700_mfc_force_channels > 0)
		num_channels = y700_mfc_force_channels;
	if (y700_mfc_force_rate > 0)
		sample_rate = y700_mfc_force_rate;
	if (y700_mfc_force_bit_width > 0)
		bit_width = y700_mfc_force_bit_width;

	payload_size = APM_MFC_CFG_PSIZE(media_format, num_channels) +
		       APM_MODULE_PARAM_DATA_SIZE;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT;
	param_data->param_size = APM_MFC_CFG_PSIZE(media_format, num_channels);
	p = p + APM_MODULE_PARAM_DATA_SIZE;
	media_format = p;

	media_format->sample_rate = sample_rate;
	media_format->bit_width = bit_width;
	media_format->num_channels = num_channels;
	for (i = 0; i < num_channels; i++) {
		if (y700_mfc_force_channels > 0)
			media_format->channel_mapping[i] = i + 1;
		else
			media_format->channel_mapping[i] = cfg->channel_map[i];
	}

	if (y700_mfc_force_rate > 0 || y700_mfc_force_channels > 0 ||
	    y700_mfc_force_bit_width > 0)
		dev_info(graph->dev,
			 "Y700 AR MFC media: graph=%u iid=0x%x rate=%u ch=%u bw=%u cfg rate=%u ch=%u bw=%u force rate=%d ch=%d bw=%d\n",
			 graph->id, module->instance_id,
			 media_format->sample_rate, media_format->num_channels,
			 media_format->bit_width, cfg->sample_rate,
			 cfg->num_channels, cfg->bit_width,
			 y700_mfc_force_rate, y700_mfc_force_channels,
			 y700_mfc_force_bit_width);

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_set_compr_media_format(struct media_format *media_fmt_hdr,
					     void *p,
					     const struct audioreach_module_config *mcfg)
{
	struct payload_media_fmt_aac_t *aac_cfg;
	struct payload_media_fmt_pcm *mp3_cfg;
	struct payload_media_fmt_flac_t *flac_cfg;
	struct payload_media_fmt_opus_t *opus_cfg;

	switch (mcfg->fmt) {
	case SND_AUDIOCODEC_MP3:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_MP3;
		media_fmt_hdr->payload_size = 0;
		p = p + sizeof(*media_fmt_hdr);
		mp3_cfg = p;
		mp3_cfg->sample_rate = mcfg->sample_rate;
		mp3_cfg->bit_width = mcfg->bit_width;
		mp3_cfg->alignment = PCM_LSB_ALIGNED;
		mp3_cfg->bits_per_sample = mcfg->bit_width;
		mp3_cfg->q_factor = mcfg->bit_width - 1;
		mp3_cfg->endianness = PCM_LITTLE_ENDIAN;
		mp3_cfg->num_channels = mcfg->num_channels;
		break;
	case SND_AUDIOCODEC_AAC:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_AAC;
		media_fmt_hdr->payload_size = sizeof(struct payload_media_fmt_aac_t);
		p = p + sizeof(*media_fmt_hdr);
		aac_cfg = p;
		aac_cfg->aac_fmt_flag = 0;
		aac_cfg->audio_obj_type = 5;
		aac_cfg->num_channels = mcfg->num_channels;
		aac_cfg->total_size_of_PCE_bits = 0;
		aac_cfg->sample_rate = mcfg->sample_rate;
		break;
	case SND_AUDIOCODEC_FLAC:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_FLAC;
		media_fmt_hdr->payload_size = sizeof(struct payload_media_fmt_flac_t);
		p = p + sizeof(*media_fmt_hdr);
		flac_cfg = p;
		flac_cfg->sample_size = mcfg->codec.options.flac_d.sample_size;
		flac_cfg->num_channels = mcfg->num_channels;
		flac_cfg->min_blk_size = mcfg->codec.options.flac_d.min_blk_size;
		flac_cfg->max_blk_size = mcfg->codec.options.flac_d.max_blk_size;
		flac_cfg->sample_rate = mcfg->sample_rate;
		flac_cfg->min_frame_size = mcfg->codec.options.flac_d.min_frame_size;
		flac_cfg->max_frame_size = mcfg->codec.options.flac_d.max_frame_size;
		break;
	case SND_AUDIOCODEC_OPUS_RAW:
		media_fmt_hdr->data_format = DATA_FORMAT_RAW_COMPRESSED;
		media_fmt_hdr->fmt_id = MEDIA_FMT_ID_OPUS;
		media_fmt_hdr->payload_size = sizeof(*opus_cfg);
		p = p + sizeof(*media_fmt_hdr);
		opus_cfg = p;
		/* raw opus packets prepended with 4 bytes of length */
		opus_cfg->bitstream_format = 1;
		/*
		 * payload_type:
		 * 0 -- read metadata from opus stream;
		 * 1 -- metadata is provided by filling in the struct here.
		 */
		opus_cfg->payload_type = 1;
		opus_cfg->version = mcfg->codec.options.opus_d.version;
		opus_cfg->num_channels = mcfg->codec.options.opus_d.num_channels;
		opus_cfg->pre_skip = mcfg->codec.options.opus_d.pre_skip;
		opus_cfg->sample_rate = mcfg->codec.options.opus_d.sample_rate;
		opus_cfg->output_gain = mcfg->codec.options.opus_d.output_gain;
		opus_cfg->mapping_family = mcfg->codec.options.opus_d.mapping_family;
		opus_cfg->stream_count = mcfg->codec.options.opus_d.chan_map.stream_count;
		opus_cfg->coupled_count = mcfg->codec.options.opus_d.chan_map.coupled_count;
		memcpy(opus_cfg->channel_mapping, mcfg->codec.options.opus_d.chan_map.channel_map,
		       sizeof(opus_cfg->channel_mapping));
		opus_cfg->reserved[0] = opus_cfg->reserved[1] = opus_cfg->reserved[2] = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int audioreach_compr_set_param(struct q6apm_graph *graph,
			       const struct audioreach_module_config *mcfg)
{
	struct media_format *header;
	int rc;
	void *p;
	int iid = q6apm_graph_get_rx_shmem_module_iid(graph);
	int payload_size = sizeof(struct apm_sh_module_media_fmt_cmd);

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_cmd_pkt(payload_size,
					DATA_CMD_WR_SH_MEM_EP_MEDIA_FORMAT,
					0, graph->port->id, iid);
	if (IS_ERR(pkt))
		return -ENOMEM;

	p = (void *)pkt + GPR_HDR_SIZE;
	header = p;
	rc = audioreach_set_compr_media_format(header, p, mcfg);
	if (rc)
		return rc;

	return gpr_send_port_pkt(graph->port, pkt);
}
EXPORT_SYMBOL_GPL(audioreach_compr_set_param);

static int audioreach_i2s_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_module_param_data *param_data;
	struct apm_i2s_module_intf_cfg *intf_cfg;
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	int ic_sz = APM_I2S_INTF_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int size = ic_sz + ep_sz + fs_sz;
	void *p;
	int ret;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	intf_cfg = p;

	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_I2S_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.lpaif_type = module->hw_interface_type;
	intf_cfg->cfg.intf_idx = module->hw_interface_idx;
	if (y700_i2s_lpaif_type >= 0)
		intf_cfg->cfg.lpaif_type = y700_i2s_lpaif_type;
	if (y700_i2s_intf_idx >= 0)
		intf_cfg->cfg.intf_idx = y700_i2s_intf_idx;
	/*
	 * Y700's stock speaker path is Secondary TDM with an 8-slot backend.
	 * The current I2S_SINK graph starts only when the DSP endpoint is
	 * advertised with the 8-channel selector. The physical lane enablement
	 * is tracked separately by the board routing and PA configuration.
	 */
	if (cfg->sd_line_mask == 0x0f || cfg->sd_line_mask == 0x03)
		intf_cfg->cfg.sd_line_idx = I2S_8CHS;
	else if (cfg->sd_line_mask)
		intf_cfg->cfg.sd_line_idx = cfg->sd_line_mask;
	else if (module->sd_line_idx)
		intf_cfg->cfg.sd_line_idx = module->sd_line_idx;
	else
		intf_cfg->cfg.sd_line_idx = I2S_SD0;
	if (y700_i2s_sd_line_idx >= 0)
		intf_cfg->cfg.sd_line_idx = y700_i2s_sd_line_idx;

	switch (cfg->fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		intf_cfg->cfg.ws_src = CONFIG_I2S_WS_SRC_INTERNAL;
		break;
	case SND_SOC_DAIFMT_BC_FC:
		/* CPU is slave */
		intf_cfg->cfg.ws_src = CONFIG_I2S_WS_SRC_EXTERNAL;
		break;
	default:
		break;
	}
	if (y700_i2s_ws_src >= 0)
		intf_cfg->cfg.ws_src = y700_i2s_ws_src;

	p += ic_sz;
	hw_cfg = p;
	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = cfg->sample_rate;
	hw_cfg->mf.bit_width = cfg->bit_width;
	hw_cfg->mf.num_channels = cfg->num_channels;
	hw_cfg->mf.data_format = module->data_format;

	p += ep_sz;
	fs_cfg = p;
	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	fs_cfg->frame_size_factor = cfg->frame_size_factor ?:
				    module->frame_size_factor ?: 1;
	if (y700_i2s_frame_size_factor >= 0)
		fs_cfg->frame_size_factor = y700_i2s_frame_size_factor;

	if (module->hw_interface_type == I2S_INTF_TYPE_SECONDARY ||
	    y700_i2s_lpaif_type >= 0 || y700_i2s_intf_idx >= 0 ||
	    y700_i2s_sd_line_idx >= 0 || y700_i2s_ws_src >= 0 ||
	    y700_i2s_frame_size_factor >= 0) {
		dev_info(graph->dev,
			 "Y700 AR I2S cfg: graph=%u iid=0x%x module=0x%x hw_type=%u hw_idx=%u cfg rate=%u ch=%u bw=%u fmt=0x%x sd_mask=0x%x fsf=%u mod sd=%u ws=%u fsf=%u data_fmt=%u override type=%d idx=%d sd=%d ws=%d fsf=%d\n",
			 graph->id, module->instance_id, module->module_id,
			 module->hw_interface_type, module->hw_interface_idx,
			 cfg->sample_rate, cfg->num_channels, cfg->bit_width,
			 cfg->fmt, cfg->sd_line_mask, cfg->frame_size_factor,
			 module->sd_line_idx, module->ws_src,
			 module->frame_size_factor, module->data_format,
			 y700_i2s_lpaif_type, y700_i2s_intf_idx,
			 y700_i2s_sd_line_idx, y700_i2s_ws_src,
			 y700_i2s_frame_size_factor);
		dev_info(graph->dev,
			 "Y700 AR I2S pkt: lpaif=%u idx=%u sd=0x%x ws=%u mf rate=%u ch=%u bw=%u data_fmt=%u fsf=%u\n",
			 intf_cfg->cfg.lpaif_type, intf_cfg->cfg.intf_idx,
			 intf_cfg->cfg.sd_line_idx, intf_cfg->cfg.ws_src,
			 hw_cfg->mf.sample_rate, hw_cfg->mf.num_channels,
			 hw_cfg->mf.bit_width, hw_cfg->mf.data_format,
			 fs_cfg->frame_size_factor);
	}

	ret = q6apm_send_cmd_sync(graph->apm, pkt, 0);
	if (ret && module->hw_interface_type == I2S_INTF_TYPE_SECONDARY)
		dev_err(graph->dev,
			"Y700 AR I2S SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);

	return ret;
}

#define Y700_TDM_CFG_MF		BIT(0)
#define Y700_TDM_CFG_FSF	BIT(1)
#define Y700_TDM_CFG_INTF	BIT(2)
#define Y700_TDM_CFG_LANE	BIT(3)
#define Y700_TDM_CFG_CLK	BIT(4)
#define Y700_TDM_CFG_POWER	BIT(5)
#define Y700_TDM_CFG_DMA_ALIGN	BIT(6)

static int audioreach_tdm_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	struct apm_module_hw_ep_mf_cfg *hw_cfg;
	struct apm_module_frame_size_factor_cfg *fs_cfg;
	struct apm_tdm_module_intf_cfg *intf_cfg;
	struct apm_tdm_module_lane_cfg *lane_cfg;
	struct apm_module_hw_intf_clk_cfg *clk_cfg;
	struct apm_module_param_data *param_data;
	u32 slot_mask = cfg->sd_line_mask ?: 1;
	u32 nslots = cfg->num_channels ?: module->frame_size_factor ?: 8;
	u32 bit_width = cfg->bit_width ?: 32;
	u32 media_rate = cfg->sample_rate;
	u32 media_bit_width = bit_width;
	u32 media_channels = cfg->num_channels;
	u32 frame_size_factor = cfg->frame_size_factor ?:
				module->frame_size_factor ?: 1;
	u32 clock_id;
	u32 clock_freq;
	u32 clock_attr = CLOCK_ATTRIBUTE_COUPLE_NO;
	u32 clock_root = CLOCK_ROOT_DEFAULT;
	u32 lane_mask;
	int active_mask = y700_tdm_cfg_mask;
	int ic_sz = APM_TDM_INTF_CFG_PSIZE;
	int lane_sz = APM_TDM_LANE_CFG_PSIZE;
	int ep_sz = APM_HW_EP_CFG_PSIZE;
	int fs_sz = APM_FS_CFG_PSIZE;
	int clk_sz = APM_HW_INTF_CLK_CFG_PSIZE;
	int pm_sz = APM_HW_EP_PMODE_CFG_PSIZE;
	int align_sz = APM_HW_EP_DALIGN_CFG_PSIZE;
	int size = ic_sz;
	void *p;
	int ret;

	if (y700_tdm_media_rate > 0)
		media_rate = y700_tdm_media_rate;
	if (y700_tdm_media_channels > 0)
		media_channels = y700_tdm_media_channels;
	if (y700_tdm_media_bit_width > 0)
		media_bit_width = y700_tdm_media_bit_width;
	if (y700_tdm_slot_mask >= 0)
		slot_mask = y700_tdm_slot_mask;
	if (y700_tdm_nslots >= 0)
		nslots = y700_tdm_nslots;
	if (y700_tdm_slot_width >= 0)
		bit_width = y700_tdm_slot_width;
	if (y700_tdm_frame_size_factor >= 0)
		frame_size_factor = y700_tdm_frame_size_factor;
	if (y700_tdm_power_mode >= 0)
		active_mask |= Y700_TDM_CFG_POWER;
	if (y700_tdm_dma_align >= 0)
		active_mask |= Y700_TDM_CFG_DMA_ALIGN;
	clock_id = CLOCK_ID_PRI_TDM_IBIT + 2 * (y700_tdm_intf_idx >= 0 ?
		   y700_tdm_intf_idx : module->hw_interface_idx);
	clock_freq = media_rate * nslots * bit_width;
	if (y700_tdm_clk_attr >= 0)
		clock_attr = y700_tdm_clk_attr;
	if (y700_tdm_clk_root >= 0)
		clock_root = y700_tdm_clk_root;
	if (y700_tdm_clk_freq > 0)
		clock_freq = y700_tdm_clk_freq;

	dev_info(graph->dev,
		 "Y700 AR TDM control: graph=%u iid=0x%x module=0x%x mask=0x%x combined=%d media_override rate=%d ch=%d bw=%d clk_attr=%d clk_root=%d clk_freq=%d power=%d dma_align=%d\n",
		 graph->id, module->instance_id, module->module_id,
		 active_mask, y700_tdm_combined_setcfg, y700_tdm_media_rate,
		 y700_tdm_media_channels, y700_tdm_media_bit_width,
		 y700_tdm_clk_attr, y700_tdm_clk_root, y700_tdm_clk_freq,
		 y700_tdm_power_mode, y700_tdm_dma_align);

	if (y700_tdm_combined_setcfg) {
		if (active_mask & Y700_TDM_CFG_MF)
			size += ep_sz;
		if (active_mask & Y700_TDM_CFG_FSF)
			size += fs_sz;
		if (active_mask & Y700_TDM_CFG_INTF)
			size += ic_sz;
		if (active_mask & Y700_TDM_CFG_LANE)
			size += lane_sz;
		if (active_mask & Y700_TDM_CFG_CLK)
			size += clk_sz;
		if ((active_mask & Y700_TDM_CFG_POWER) && y700_tdm_power_mode >= 0)
			size += pm_sz;
		if ((active_mask & Y700_TDM_CFG_DMA_ALIGN) && y700_tdm_dma_align >= 0)
			size += align_sz;

		struct gpr_pkt *combined_pkt __free(kfree) =
			audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
		if (IS_ERR(combined_pkt))
			return PTR_ERR(combined_pkt);

		p = (void *)combined_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

		if (active_mask & Y700_TDM_CFG_MF) {
			hw_cfg = p;
			param_data = &hw_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
			param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;
			hw_cfg->mf.sample_rate = media_rate;
			hw_cfg->mf.bit_width = media_bit_width;
			hw_cfg->mf.num_channels = media_channels;
			hw_cfg->mf.data_format = module->data_format;
			p += ep_sz;
		}

		if (active_mask & Y700_TDM_CFG_FSF) {
			fs_cfg = p;
			param_data = &fs_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
			param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
			fs_cfg->frame_size_factor = frame_size_factor;
			p += fs_sz;
		}

		if (active_mask & Y700_TDM_CFG_INTF) {
			intf_cfg = p;
			param_data = &intf_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_TDM_INTF_CFG;
			param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;
			intf_cfg->cfg.lpaif_type = y700_tdm_lpaif_type >= 0 ?
						   y700_tdm_lpaif_type : module->hw_interface_type;
			intf_cfg->cfg.intf_idx = y700_tdm_intf_idx >= 0 ?
						 y700_tdm_intf_idx : module->hw_interface_idx;
			intf_cfg->cfg.sync_src = y700_tdm_sync_src >= 0 ?
						 y700_tdm_sync_src : TDM_SYNC_SRC_INTERNAL;
			intf_cfg->cfg.ctrl_data_out_enable = y700_tdm_ctrl_data_oe >= 0 ?
							     y700_tdm_ctrl_data_oe :
							     TDM_CTRL_DATA_OE_ENABLE;
			intf_cfg->cfg.slot_mask = slot_mask;
			intf_cfg->cfg.nslots_per_frame = nslots;
			intf_cfg->cfg.slot_width = bit_width;
			intf_cfg->cfg.sync_mode = y700_tdm_sync_mode >= 0 ?
						  y700_tdm_sync_mode :
						  TDM_SHORT_SYNC_SLOT_MODE;
			intf_cfg->cfg.ctrl_invert_sync_pulse = y700_tdm_invert_sync >= 0 ?
							       y700_tdm_invert_sync :
							       TDM_SYNC_NORMAL;
			intf_cfg->cfg.ctrl_sync_data_delay = y700_tdm_sync_delay >= 0 ?
							     y700_tdm_sync_delay :
							     TDM_DATA_DELAY_1_BCLK_CYCLE;
			p += ic_sz;
		}

		if (active_mask & Y700_TDM_CFG_LANE) {
			lane_cfg = p;
			param_data = &lane_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_TDM_LANE_CFG;
			param_data->param_size = lane_sz - APM_MODULE_PARAM_DATA_SIZE;
			lane_cfg->cfg.lane_mask = y700_tdm_lane_mask >= 0 ?
						  y700_tdm_lane_mask : slot_mask;
			p += lane_sz;
		}

		if (active_mask & Y700_TDM_CFG_CLK) {
			clk_cfg = p;
			param_data = &clk_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_HW_INTF_CLK_CFG;
			param_data->param_size = clk_sz - APM_MODULE_PARAM_DATA_SIZE;
			clk_cfg->cfg.clock_id = clock_id;
			clk_cfg->cfg.clock_freq = clock_freq;
			clk_cfg->cfg.clock_attri = clock_attr;
			clk_cfg->cfg.clock_root = clock_root;
			p += clk_sz;
		}

		if ((active_mask & Y700_TDM_CFG_POWER) && y700_tdm_power_mode >= 0) {
			struct apm_module_hw_ep_power_mode_cfg *pm_cfg = p;

			param_data = &pm_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_HW_EP_POWER_MODE_CFG;
			param_data->param_size = pm_sz - APM_MODULE_PARAM_DATA_SIZE;
			pm_cfg->power_mode.power_mode = y700_tdm_power_mode;
			p += pm_sz;
		}

		if ((active_mask & Y700_TDM_CFG_DMA_ALIGN) && y700_tdm_dma_align >= 0) {
			struct apm_module_hw_ep_dma_data_align_cfg *align_cfg = p;

			param_data = &align_cfg->param_data;
			param_data->module_instance_id = module->instance_id;
			param_data->error_code = 0;
			param_data->param_id = PARAM_ID_HW_EP_DMA_DATA_ALIGN;
			param_data->param_size = align_sz - APM_MODULE_PARAM_DATA_SIZE;
			align_cfg->align.dma_data_align = y700_tdm_dma_align;
			p += align_sz;
		}

		dev_info(graph->dev,
			 "Y700 AR TDM combined SET_CFG: graph=%u iid=0x%x size=%d mask=0x%x\n",
			 graph->id, module->instance_id, size, active_mask);
		ret = q6apm_send_cmd_sync(graph->apm, combined_pkt, 0);
		if (ret)
			dev_err(graph->dev,
				"Y700 AR TDM combined SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
				graph->id, module->instance_id, ret);
		return ret;
	}

	if (active_mask & Y700_TDM_CFG_MF) {

	struct gpr_pkt *mf_pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(ep_sz, APM_CMD_SET_CFG, 0);
	if (IS_ERR(mf_pkt))
		return PTR_ERR(mf_pkt);

	p = (void *)mf_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	hw_cfg = p;

	param_data = &hw_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_MF_CFG;
	param_data->param_size = ep_sz - APM_MODULE_PARAM_DATA_SIZE;

	hw_cfg->mf.sample_rate = media_rate;

	hw_cfg->mf.bit_width = media_bit_width;
	hw_cfg->mf.num_channels = media_channels;
	hw_cfg->mf.data_format = module->data_format;

	dev_info(graph->dev,
		 "Y700 AR TDM mf-only pkt: graph=%u iid=0x%x module=0x%x rate=%u ch=%u bw=%u cfg_bw=%u data_fmt=%u\n",
		 graph->id, module->instance_id, module->module_id,
		 hw_cfg->mf.sample_rate, hw_cfg->mf.num_channels,
		 hw_cfg->mf.bit_width, bit_width, hw_cfg->mf.data_format);

	ret = q6apm_send_cmd_sync(graph->apm, mf_pkt, 0);
	if (ret) {
		dev_err(graph->dev,
			"Y700 AR TDM MF SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);
		return ret;
	}

	}

	if (active_mask & Y700_TDM_CFG_FSF) {

	struct gpr_pkt *fs_pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(fs_sz, APM_CMD_SET_CFG, 0);
	if (IS_ERR(fs_pkt))
		return PTR_ERR(fs_pkt);

	p = (void *)fs_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	fs_cfg = p;

	param_data = &fs_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_EP_FRAME_SIZE_FACTOR;
	param_data->param_size = fs_sz - APM_MODULE_PARAM_DATA_SIZE;
	frame_size_factor = cfg->frame_size_factor ?:
			    module->frame_size_factor ?: 1;
	if (y700_tdm_frame_size_factor >= 0)
		frame_size_factor = y700_tdm_frame_size_factor;

	fs_cfg->frame_size_factor = frame_size_factor;

	dev_info(graph->dev,
		 "Y700 AR TDM fsf-only pkt: graph=%u iid=0x%x module=0x%x fsf=%u\n",
		 graph->id, module->instance_id, module->module_id,
		 fs_cfg->frame_size_factor);

	ret = q6apm_send_cmd_sync(graph->apm, fs_pkt, 0);
	if (ret) {
		dev_err(graph->dev,
			"Y700 AR TDM FSF SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);
		return ret;
	}

	}

	if (active_mask & Y700_TDM_CFG_INTF) {

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	intf_cfg = p;

	param_data = &intf_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_TDM_INTF_CFG;
	param_data->param_size = ic_sz - APM_MODULE_PARAM_DATA_SIZE;

	intf_cfg->cfg.lpaif_type = module->hw_interface_type;
	intf_cfg->cfg.intf_idx = module->hw_interface_idx;
	if (y700_tdm_lpaif_type >= 0)
		intf_cfg->cfg.lpaif_type = y700_tdm_lpaif_type;
	if (y700_tdm_intf_idx >= 0)
		intf_cfg->cfg.intf_idx = y700_tdm_intf_idx;
	switch (cfg->fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		intf_cfg->cfg.sync_src = TDM_SYNC_SRC_INTERNAL;
		break;
	case SND_SOC_DAIFMT_BC_FC:
		intf_cfg->cfg.sync_src = TDM_SYNC_SRC_EXTERNAL;
		break;
	default:
		intf_cfg->cfg.sync_src = TDM_SYNC_SRC_INTERNAL;
		break;
	}
	if (y700_tdm_sync_src >= 0)
		intf_cfg->cfg.sync_src = y700_tdm_sync_src;
	intf_cfg->cfg.ctrl_data_out_enable = TDM_CTRL_DATA_OE_ENABLE;
	if (y700_tdm_ctrl_data_oe >= 0)
		intf_cfg->cfg.ctrl_data_out_enable = y700_tdm_ctrl_data_oe;
	if (y700_tdm_slot_mask >= 0)
		slot_mask = y700_tdm_slot_mask;
	if (y700_tdm_nslots >= 0)
		nslots = y700_tdm_nslots;
	if (y700_tdm_slot_width >= 0)
		bit_width = y700_tdm_slot_width;
	intf_cfg->cfg.slot_mask = slot_mask;
	intf_cfg->cfg.nslots_per_frame = nslots;
	intf_cfg->cfg.slot_width = bit_width;
	intf_cfg->cfg.sync_mode = TDM_SHORT_SYNC_SLOT_MODE;
	if (y700_tdm_sync_mode >= 0)
		intf_cfg->cfg.sync_mode = y700_tdm_sync_mode;
	intf_cfg->cfg.ctrl_invert_sync_pulse = TDM_SYNC_NORMAL;
	if (y700_tdm_invert_sync >= 0)
		intf_cfg->cfg.ctrl_invert_sync_pulse = y700_tdm_invert_sync;
	intf_cfg->cfg.ctrl_sync_data_delay = TDM_DATA_DELAY_1_BCLK_CYCLE;
	if (y700_tdm_sync_delay >= 0)
		intf_cfg->cfg.ctrl_sync_data_delay = y700_tdm_sync_delay;

	dev_info(graph->dev,
		 "Y700 AR TDM intf-only pkt: graph=%u iid=0x%x module=0x%x lpaif=%u idx=%u sync=%u oe=%u slot=0x%x nslots=%u width=%u mode=%u inv=%u delay=%u rate=%u ch=%u data_fmt=%u fsf=%u overrides type=%d idx=%d sync=%d oe=%d slot=%d nslots=%d width=%d mode=%d inv=%d delay=%d lane=%d fsf=%d\n",
		 graph->id, module->instance_id, module->module_id,
		 intf_cfg->cfg.lpaif_type, intf_cfg->cfg.intf_idx,
		 intf_cfg->cfg.sync_src, intf_cfg->cfg.ctrl_data_out_enable,
		 intf_cfg->cfg.slot_mask, intf_cfg->cfg.nslots_per_frame,
		 intf_cfg->cfg.slot_width, intf_cfg->cfg.sync_mode,
		 intf_cfg->cfg.ctrl_invert_sync_pulse,
		 intf_cfg->cfg.ctrl_sync_data_delay, cfg->sample_rate,
		 cfg->num_channels, module->data_format, frame_size_factor,
		 y700_tdm_lpaif_type, y700_tdm_intf_idx, y700_tdm_sync_src,
		 y700_tdm_ctrl_data_oe, y700_tdm_slot_mask, y700_tdm_nslots,
		 y700_tdm_slot_width, y700_tdm_sync_mode,
		 y700_tdm_invert_sync, y700_tdm_sync_delay,
		 y700_tdm_lane_mask, y700_tdm_frame_size_factor);

	ret = q6apm_send_cmd_sync(graph->apm, pkt, 0);
	if (ret)
		dev_err(graph->dev,
			"Y700 AR TDM SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);
	if (ret)
		return ret;

	}

	if (active_mask & Y700_TDM_CFG_LANE) {

	struct gpr_pkt *lane_pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(lane_sz, APM_CMD_SET_CFG, 0);
	if (IS_ERR(lane_pkt))
		return PTR_ERR(lane_pkt);

	p = (void *)lane_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	lane_cfg = p;

	param_data = &lane_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_TDM_LANE_CFG;
	param_data->param_size = lane_sz - APM_MODULE_PARAM_DATA_SIZE;

	/*
	 * Y700's speaker bus uses SD0 + SD1.  Slot placement and lane enablement
	 * are separate TDM endpoint parameters, so describe the active lanes
	 * explicitly instead of assuming the slot mask also enables them.
	 */
	lane_mask = slot_mask;
	if (y700_tdm_lane_mask >= 0)
		lane_mask = y700_tdm_lane_mask;

	lane_cfg->cfg.lane_mask = lane_mask;

	dev_info(graph->dev,
		 "Y700 AR TDM lane pkt: graph=%u iid=0x%x module=0x%x lane_mask=0x%x\n",
		 graph->id, module->instance_id, module->module_id,
		 lane_cfg->cfg.lane_mask);

	ret = q6apm_send_cmd_sync(graph->apm, lane_pkt, 0);
	if (ret)
		dev_err(graph->dev,
			"Y700 AR TDM LANE SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);
	if (ret)
		return ret;

	}

	if (active_mask & Y700_TDM_CFG_CLK) {

	struct gpr_pkt *clk_pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(clk_sz, APM_CMD_SET_CFG, 0);
	if (IS_ERR(clk_pkt))
		return PTR_ERR(clk_pkt);

	p = (void *)clk_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	clk_cfg = p;

	param_data = &clk_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_HW_INTF_CLK_CFG;
	param_data->param_size = clk_sz - APM_MODULE_PARAM_DATA_SIZE;

	clock_id = CLOCK_ID_PRI_TDM_IBIT + 2 * (y700_tdm_intf_idx >= 0 ?
		   y700_tdm_intf_idx : module->hw_interface_idx);
	clock_freq = media_rate * nslots * bit_width;
	if (y700_tdm_clk_attr >= 0)
		clock_attr = y700_tdm_clk_attr;
	if (y700_tdm_clk_root >= 0)
		clock_root = y700_tdm_clk_root;
	if (y700_tdm_clk_freq > 0)
		clock_freq = y700_tdm_clk_freq;
	clk_cfg->cfg.clock_id = clock_id;
	clk_cfg->cfg.clock_freq = clock_freq;
	clk_cfg->cfg.clock_attri = clock_attr;
	clk_cfg->cfg.clock_root = clock_root;

	dev_info(graph->dev,
		 "Y700 AR TDM clk pkt: graph=%u iid=0x%x module=0x%x clk_id=0x%x freq=%u attr=%u root=%u\n",
		 graph->id, module->instance_id, module->module_id,
		 clk_cfg->cfg.clock_id, clk_cfg->cfg.clock_freq,
		 clk_cfg->cfg.clock_attri, clk_cfg->cfg.clock_root);

	ret = q6apm_send_cmd_sync(graph->apm, clk_pkt, 0);
	if (ret)
		dev_err(graph->dev,
			"Y700 AR TDM CLK SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);
	if (ret)
		return ret;

	}

	if (active_mask & Y700_TDM_CFG_POWER) {
		struct apm_module_hw_ep_power_mode_cfg *pm_cfg;
		struct apm_module_param_data *pd;
		struct gpr_pkt *pm_pkt __free(kfree) =
			audioreach_alloc_apm_cmd_pkt(pm_sz, APM_CMD_SET_CFG, 0);

		if (IS_ERR(pm_pkt))
			return PTR_ERR(pm_pkt);

		p = (void *)pm_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
		pm_cfg = p;
		pd = &pm_cfg->param_data;
		pd->module_instance_id = module->instance_id;
		pd->error_code = 0;
		pd->param_id = PARAM_ID_HW_EP_POWER_MODE_CFG;
		pd->param_size = pm_sz - APM_MODULE_PARAM_DATA_SIZE;
		pm_cfg->power_mode.power_mode = y700_tdm_power_mode;

		dev_info(graph->dev,
			 "Y700 AR TDM power pkt: graph=%u iid=0x%x module=0x%x mode=%d\n",
			 graph->id, module->instance_id, module->module_id,
			 y700_tdm_power_mode);
		ret = q6apm_send_cmd_sync(graph->apm, pm_pkt, 0);
		if (ret) {
			dev_err(graph->dev,
				"Y700 AR TDM POWER SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
				graph->id, module->instance_id, ret);
			return ret;
		}
	}

	if (active_mask & Y700_TDM_CFG_DMA_ALIGN) {
		struct apm_module_hw_ep_dma_data_align_cfg *align_cfg;
		struct apm_module_param_data *pd;
		struct gpr_pkt *align_pkt __free(kfree) =
			audioreach_alloc_apm_cmd_pkt(align_sz, APM_CMD_SET_CFG, 0);

		if (IS_ERR(align_pkt))
			return PTR_ERR(align_pkt);

		p = (void *)align_pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
		align_cfg = p;
		pd = &align_cfg->param_data;
		pd->module_instance_id = module->instance_id;
		pd->error_code = 0;
		pd->param_id = PARAM_ID_HW_EP_DMA_DATA_ALIGN;
		pd->param_size = align_sz - APM_MODULE_PARAM_DATA_SIZE;
		align_cfg->align.dma_data_align = y700_tdm_dma_align;

		dev_info(graph->dev,
			 "Y700 AR TDM dma-align pkt: graph=%u iid=0x%x module=0x%x align=%d\n",
			 graph->id, module->instance_id, module->module_id,
			 y700_tdm_dma_align);
		ret = q6apm_send_cmd_sync(graph->apm, align_pkt, 0);
		if (ret) {
			dev_err(graph->dev,
				"Y700 AR TDM DMA_ALIGN SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
				graph->id, module->instance_id, ret);
			return ret;
		}
	}

	return 0;
}

static int audioreach_spr_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *cfg)
{
	struct apm_module_spr_delay_path_end_cfg *delay_cfg;
	struct apm_module_param_data *param_data;
	int size = APM_SPR_DELAY_PATH_END_CFG_PSIZE;
	void *p;
	int ret;

	if (!y700_spr_delay_path_end_iid) {
		dev_info(graph->dev,
			 "Y700 AR SPR delay-path disabled: graph=%u iid=0x%x module=0x%x\n",
			 graph->id, module->instance_id, module->module_id);
		return 0;
	}

	if (y700_spr_delay_path_graph >= 0 &&
	    graph->id != y700_spr_delay_path_graph) {
		dev_info(graph->dev,
			 "Y700 AR SPR delay-path skipped: graph=%u target_graph=%d iid=0x%x module=0x%x\n",
			 graph->id, y700_spr_delay_path_graph,
			 module->instance_id, module->module_id);
		return 0;
	}

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;
	delay_cfg = p;

	param_data = &delay_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SPR_DELAY_PATH_END;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	delay_cfg->module_instance_id = y700_spr_delay_path_end_iid;

	dev_info(graph->dev,
		 "Y700 AR SPR delay-path pkt: graph=%u iid=0x%x module=0x%x end_iid=0x%x rate=%u ch=%u bw=%u\n",
		 graph->id, module->instance_id, module->module_id,
		 delay_cfg->module_instance_id, cfg->sample_rate,
		 cfg->num_channels, cfg->bit_width);

	ret = q6apm_send_cmd_sync(graph->apm, pkt, 0);
	if (ret)
		dev_err(graph->dev,
			"Y700 AR SPR delay-path SET_CFG failed: graph=%u iid=0x%x ret=%d\n",
			graph->id, module->instance_id, ret);

	return ret;
}

static int audioreach_logging_set_media_format(struct q6apm_graph *graph,
					       const struct audioreach_module *module)
{
	struct apm_module_param_data *param_data;
	struct data_logging_config *cfg;
	int size = sizeof(*cfg) + APM_MODULE_PARAM_DATA_SIZE;
	void *p;

	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_DATA_LOGGING_CONFIG;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	p = p + APM_MODULE_PARAM_DATA_SIZE;
	cfg = p;
	cfg->log_code = module->log_code;
	cfg->log_tap_point_id = module->log_tap_point_id;
	cfg->mode = module->log_mode;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_pcm_set_media_format(struct q6apm_graph *graph,
					   const struct audioreach_module *module,
					   const struct audioreach_module_config *mcfg)
{
	struct payload_pcm_output_format_cfg *media_cfg;
	uint32_t num_channels = mcfg->num_channels;
	struct apm_pcm_module_media_fmt_cmd *cfg;
	struct apm_module_param_data *param_data;
	int payload_size;

	if (num_channels > 4) {
		dev_err(graph->dev, "Error: Invalid channels (%d)!\n", num_channels);
		return -EINVAL;
	}

	payload_size = APM_PCM_MODULE_FMT_CMD_PSIZE(num_channels);

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	cfg = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = &cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_PCM_OUTPUT_FORMAT_CFG;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;

	cfg->header.data_format = DATA_FORMAT_FIXED_POINT;
	cfg->header.fmt_id = MEDIA_FMT_ID_PCM;
	cfg->header.payload_size = APM_PCM_OUT_FMT_CFG_PSIZE(media_cfg, num_channels);

	media_cfg = &cfg->media_cfg;
	media_cfg->alignment = PCM_LSB_ALIGNED;
	media_cfg->bit_width = mcfg->bit_width;
	media_cfg->endianness = PCM_LITTLE_ENDIAN;
	media_cfg->interleaved = module->interleave_type;
	media_cfg->num_channels = mcfg->num_channels;
	media_cfg->q_factor = mcfg->bit_width - 1;
	media_cfg->bits_per_sample = mcfg->bit_width;
	memcpy(media_cfg->channel_mapping, mcfg->channel_map, mcfg->num_channels);

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_shmem_set_media_format(struct q6apm_graph *graph,
					     const struct audioreach_module *module,
					     const struct audioreach_module_config *mcfg)
{
	uint32_t num_channels = mcfg->num_channels;
	struct apm_module_param_data *param_data;
	struct payload_media_fmt_pcm *cfg;
	struct media_format *header;
	int rc, payload_size;
	void *p;

	if (num_channels > 4) {
		dev_err(graph->dev, "Error: Invalid channels (%d)!\n", num_channels);
		return -EINVAL;
	}

	payload_size = APM_SHMEM_FMT_CFG_PSIZE(num_channels) + APM_MODULE_PARAM_DATA_SIZE;

	struct gpr_pkt *pkt __free(kfree) =
		audioreach_alloc_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0,
					 graph->port->id, module->instance_id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_MEDIA_FORMAT;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;
	p = p + APM_MODULE_PARAM_DATA_SIZE;

	header = p;
	if (mcfg->fmt == SND_AUDIOCODEC_PCM) {
		header->data_format = DATA_FORMAT_FIXED_POINT;
		header->fmt_id =  MEDIA_FMT_ID_PCM;
		header->payload_size = payload_size - sizeof(*header);

		p = p + sizeof(*header);
		cfg = p;
		cfg->sample_rate = mcfg->sample_rate;
		cfg->bit_width = mcfg->bit_width;
		cfg->alignment = PCM_LSB_ALIGNED;
		cfg->bits_per_sample = mcfg->bit_width;
		cfg->q_factor = mcfg->bit_width - 1;
		cfg->endianness = PCM_LITTLE_ENDIAN;
		cfg->num_channels = mcfg->num_channels;
		memcpy(cfg->channel_mapping, mcfg->channel_map, mcfg->num_channels);
	} else {
		rc = audioreach_set_compr_media_format(header, p, mcfg);
		if (rc)
			return rc;
	}

	return audioreach_graph_send_cmd_sync(graph, pkt, 0);
}

int audioreach_gain_set_vol_ctrl(struct q6apm *apm,
				 const struct audioreach_module *module, int vol)
{
	struct param_id_vol_ctrl_master_gain *cfg;
	struct apm_module_param_data *param_data;
	int size = sizeof(*cfg) + APM_MODULE_PARAM_DATA_SIZE;
	void *p;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = p;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_VOL_CTRL_MASTER_GAIN;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	p = p + APM_MODULE_PARAM_DATA_SIZE;
	cfg = p;
	cfg->master_gain =  vol;
	return q6apm_send_cmd_sync(apm, pkt, 0);
}
EXPORT_SYMBOL_GPL(audioreach_gain_set_vol_ctrl);

static int audioreach_gain_set(struct q6apm_graph *graph,
			       const struct audioreach_module *module)
{
	struct apm_module_param_data *param_data;
	struct apm_gain_module_cfg *cfg;
	int size = APM_GAIN_CFG_PSIZE;
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_apm_cmd_pkt(size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	cfg = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	param_data = &cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = APM_PARAM_ID_GAIN;
	param_data->param_size = size - APM_MODULE_PARAM_DATA_SIZE;

	cfg->gain_cfg.gain = module->gain;

	return q6apm_send_cmd_sync(graph->apm, pkt, 0);
}

static int audioreach_speaker_protection(struct q6apm_graph *graph,
					 const struct audioreach_module *module,
					 uint32_t operation_mode)
{
	return audioreach_send_u32_param(graph, module, PARAM_ID_SP_OP_MODE,
					 operation_mode);
}

static int audioreach_speaker_protection_vi(struct q6apm_graph *graph,
					    const struct audioreach_module *module,
					    const struct audioreach_module_config *mcfg)
{
	u32 num_channels = mcfg->num_channels;
	struct apm_module_sp_vi_op_mode_cfg *op_cfg;
	struct apm_module_sp_vi_channel_map_cfg *cm_cfg;
	struct apm_module_sp_vi_ex_mode_cfg *ex_cfg;
	int op_sz, cm_sz, ex_sz;
	struct apm_module_param_data *param_data;
	int rc, i, payload_size;
	struct gpr_pkt *pkt;
	void *p;

	if (num_channels > 2) {
		dev_err(graph->dev, "Error: Invalid channels (%d)!\n", num_channels);
		return -EINVAL;
	}

	op_sz = APM_SP_VI_OP_MODE_CFG_PSIZE(num_channels);
	/* Channel mapping for Isense and Vsense, thus twice number of speakers. */
	cm_sz = APM_SP_VI_CH_MAP_CFG_PSIZE(num_channels * 2);
	ex_sz = APM_SP_VI_EX_MODE_CFG_PSIZE;

	payload_size = op_sz + cm_sz + ex_sz;

	pkt = audioreach_alloc_apm_cmd_pkt(payload_size, APM_CMD_SET_CFG, 0);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	p = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	op_cfg = p;
	param_data = &op_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SP_VI_OP_MODE_CFG;
	param_data->param_size = op_sz - APM_MODULE_PARAM_DATA_SIZE;

	op_cfg->cfg.num_channels = num_channels;
	op_cfg->cfg.operation_mode = PARAM_ID_SP_VI_OP_MODE_NORMAL;
	p += op_sz;

	cm_cfg = p;
	param_data = &cm_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SP_VI_CHANNEL_MAP_CFG;
	param_data->param_size = cm_sz - APM_MODULE_PARAM_DATA_SIZE;

	cm_cfg->cfg.num_channels = num_channels * 2;
	for (i = 0; i < num_channels; i++) {
		/*
		 * Map speakers into Vsense and then Isense of each channel.
		 * E.g. for PCM_CHANNEL_FL and PCM_CHANNEL_FR to:
		 * [1, 2, 3, 4]
		 */
		cm_cfg->cfg.channel_mapping[2 * i] = (mcfg->channel_map[i] - 1) * 2 + 1;
		cm_cfg->cfg.channel_mapping[2 * i + 1] = (mcfg->channel_map[i] - 1) * 2 + 2;
	}

	p += cm_sz;

	ex_cfg = p;
	param_data = &ex_cfg->param_data;
	param_data->module_instance_id = module->instance_id;
	param_data->error_code = 0;
	param_data->param_id = PARAM_ID_SP_VI_EX_MODE_CFG;
	param_data->param_size = ex_sz - APM_MODULE_PARAM_DATA_SIZE;

	ex_cfg->cfg.factory_mode = 0;

	rc = q6apm_send_cmd_sync(graph->apm, pkt, 0);

	kfree(pkt);

	return rc;
}

int audioreach_set_media_format(struct q6apm_graph *graph,
				const struct audioreach_module *module,
				const struct audioreach_module_config *cfg)
{
	int rc;

	switch (module->module_id) {
	case MODULE_ID_DATA_LOGGING:
		rc = audioreach_module_enable(graph, module, true);
		if (!rc)
			rc = audioreach_logging_set_media_format(graph, module);
		break;
	case MODULE_ID_PCM_DEC:
	case MODULE_ID_PCM_ENC:
	case MODULE_ID_PCM_CNV:
	case MODULE_ID_PLACEHOLDER_DECODER:
	case MODULE_ID_PLACEHOLDER_ENCODER:
		rc = audioreach_pcm_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_DISPLAY_PORT_SINK:
		rc = audioreach_display_port_set_media_format(graph, module, cfg);
		break;
	case  MODULE_ID_SMECNS_V2:
		rc = audioreach_set_module_config(graph, module, cfg);
		break;
	case MODULE_ID_I2S_SOURCE:
	case MODULE_ID_I2S_SINK:
		rc = audioreach_i2s_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_TDM_SOURCE:
	case MODULE_ID_TDM_SINK:
		rc = audioreach_tdm_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_SPR:
		rc = audioreach_spr_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_WR_SHARED_MEM_EP:
		rc = audioreach_shmem_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_GAIN:
		rc = audioreach_gain_set(graph, module);
		break;
	case MODULE_ID_CODEC_DMA_SINK:
	case MODULE_ID_CODEC_DMA_SOURCE:
		rc = audioreach_codec_dma_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_SAL:
		rc = audioreach_sal_set_media_format(graph, module, cfg);
		if (!rc)
			rc = audioreach_sal_limiter_enable(graph, module, true);
		break;
	case MODULE_ID_MFC:
		rc = audioreach_mfc_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_GAPLESS:
		rc = audioreach_gapless_set_media_format(graph, module, cfg);
		break;
	case MODULE_ID_SPEAKER_PROTECTION:
		rc = audioreach_speaker_protection(graph, module,
						   PARAM_ID_SP_OP_MODE_NORMAL);
		if (!rc)
			rc = audioreach_module_enable(graph, module, true);

		break;
	case MODULE_ID_SPEAKER_PROTECTION_VI:
		rc = audioreach_speaker_protection_vi(graph, module, cfg);
		if (!rc)
			rc = audioreach_module_enable(graph, module, true);
		break;

	default:
		rc = 0;
	}

	return rc;
}
EXPORT_SYMBOL_GPL(audioreach_set_media_format);

void audioreach_graph_free_buf(struct q6apm_graph *graph)
{
	struct audioreach_graph_data *port;

	mutex_lock(&graph->lock);
	port = &graph->rx_data;
	port->num_periods = 0;
	kfree(port->buf);
	port->buf = NULL;

	port = &graph->tx_data;
	port->num_periods = 0;
	kfree(port->buf);
	port->buf = NULL;
	mutex_unlock(&graph->lock);
}
EXPORT_SYMBOL_GPL(audioreach_graph_free_buf);

int audioreach_shared_memory_send_eos(struct q6apm_graph *graph)
{
	struct data_cmd_wr_sh_mem_ep_eos *eos;
	int iid = q6apm_graph_get_rx_shmem_module_iid(graph);
	struct gpr_pkt *pkt __free(kfree) = audioreach_alloc_cmd_pkt(sizeof(*eos),
					DATA_CMD_WR_SH_MEM_EP_EOS, 0, graph->port->id, iid);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	eos = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	eos->policy = WR_SH_MEM_EP_EOS_POLICY_LAST;

	return gpr_send_port_pkt(graph->port, pkt);
}
EXPORT_SYMBOL_GPL(audioreach_shared_memory_send_eos);
