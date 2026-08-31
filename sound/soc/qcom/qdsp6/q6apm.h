/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __Q6APM_H__
#define __Q6APM_H__
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <sound/soc.h>
#include <linux/of_platform.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/soc/qcom/apr.h>
#include "../common.h"
#include "audioreach.h"

#define APM_PORT_MAX		LPASS_MAX_PORT
#define APM_PORT_MAX_AUDIO_CHAN_CNT 8
#define PCM_CHANNEL_NULL 0
#define PCM_CHANNEL_FL    1	/* Front left channel. */
#define PCM_CHANNEL_FR    2	/* Front right channel. */
#define PCM_CHANNEL_FC    3	/* Front center channel. */
#define PCM_CHANNEL_LS   4	/* Left surround channel. */
#define PCM_CHANNEL_RS   5	/* Right surround channel. */
#define PCM_CHANNEL_LFE  6	/* Low frequency effect channel. */
#define PCM_CHANNEL_CS   7	/* Center surround channel; Rear center ch */
#define PCM_CHANNEL_LB   8	/* Left back channel; Rear left channel. */
#define PCM_CHANNEL_RB   9	/* Right back channel; Rear right channel. */
#define PCM_CHANNELS   10	/* Top surround channel. */

#define APM_TIMESTAMP_FLAG	0x80000000
#define FORMAT_LINEAR_PCM	0x0000
/* APM client callback events */
#define APM_CMD_EOS				0x0003
#define APM_CLIENT_EVENT_CMD_EOS_DONE		0x1003
#define APM_CMD_CLOSE				0x0004
#define APM_CLIENT_EVENT_CMD_CLOSE_DONE		0x1004
#define APM_CLIENT_EVENT_CMD_RUN_DONE		0x1008
#define APM_CLIENT_EVENT_DATA_WRITE_DONE	0x1009
#define APM_CLIENT_EVENT_DATA_READ_DONE		0x100a
#define APM_MAX_SESSIONS			8
#define APM_LAST_BUFFER_FLAG			BIT(30)
#define NO_TIMESTAMP				0xFF00

struct q6apm;
struct q6apm_graph;
struct audioreach_graph;
struct q6apm_mem_lease;

enum q6apm_cmd_state {
	Q6APM_CMD_IDLE = 0,
	Q6APM_CMD_PENDING,
	Q6APM_CMD_CONFIRMED_OK,
	Q6APM_CMD_CONFIRMED_ERROR,
	Q6APM_CMD_AMBIGUOUS,
	Q6APM_CMD_ORPHANED,
	Q6APM_CMD_RESET_RETIRED,
};

enum q6apm_cmd_kind {
	Q6APM_CMD_GENERIC = 0,
	Q6APM_CMD_MAP,
	Q6APM_CMD_UNMAP,
};

enum q6apm_mem_lease_state {
	AUDIOREACH_MEM_UNMAPPED = 0,
	AUDIOREACH_MEM_MAP_PENDING,
	AUDIOREACH_MEM_MAPPED,
	AUDIOREACH_MEM_MAP_UNCERTAIN,
	AUDIOREACH_MEM_UNMAP_PENDING,
	AUDIOREACH_MEM_UNMAP_UNCERTAIN,
	AUDIOREACH_MEM_RESET_RETIRED,
};

struct q6apm_cmd_ticket {
	struct list_head node;
	struct kref refcount;
	struct q6apm *apm;
	u32 token;
	u32 opcode;
	u32 graph_id;
	/* Monotonic identity of the cached graph instance, not just its ID. */
	u64 graph_cookie;
	u32 response_opcode;
	u32 generation;
	int result;
	enum q6apm_cmd_state state;
	enum q6apm_cmd_kind kind;
	bool waiter_active;
	bool listed;
	bool apm_ref;
	bool flush_after_stop;
	u32 lease_handle;
	struct q6apm_mem_lease *lease;
	/* A graph ticket pins the cached graph until a late response is settled. */
	struct audioreach_graph *graph;
	bool graph_ref;
	/* MAP responses form a small authenticated transcript. */
	bool map_basic_seen;
	u32 map_basic_status;
	bool map_handle_seen;
	u32 map_handle;
	bool map_protocol_error;
	struct completion done;
};

struct q6apm_mem_lease {
	struct list_head node;
	struct kref refcount;
	struct q6apm *apm;
	u32 graph_id;
	phys_addr_t phys;
	size_t size;
	u32 handle;
	u32 users;
	u32 generation;
	enum q6apm_mem_lease_state state;
	/* The ticket that currently owns this generation's MAP transaction. */
	struct q6apm_cmd_ticket *map_ticket;
	bool apm_ref;
};

struct q6apm {
	struct kref refcount;
	struct device *dev;
	gpr_device_t *gdev;

	/* Serializes online/offline transitions against in-flight commands. */
	struct mutex online_lock;
	struct mutex cmd_lock;
	struct mutex lock;
	struct mutex response_lock;
	struct list_head cmd_tickets;
	atomic_t reset_generation;
	struct list_head mem_leases;
	/* Client graph handles retained across ambiguous graph-port commands. */
	struct list_head graph_clients;
	/* Distinguishes cached graph instances that reuse a graph ID. */
	atomic64_t graph_cookie_seq;
	/* Pins topology data while cached graphs reference it. */
	struct rw_semaphore graph_lock;
	wait_queue_head_t graph_wait;
	atomic_t active_graphs;
	atomic_t graph_generation;
	bool topology_deferred;
	bool topology_remove_complete;
	uint32_t state;
	bool online;

	struct list_head widget_list;
	struct idr graph_idr;
	struct idr graph_info_idr;
	struct idr sub_graphs_idr;
	struct idr containers_idr;
	struct idr modules_idr;
};

struct audio_buffer {
	phys_addr_t phys;
	uint32_t size;		/* size of buffer */
	uint32_t submitted;	/* bytes in the current DSP transaction */
	uint32_t token;
	bool in_flight;
};

struct audioreach_graph_data {
	struct audio_buffer *buf;
	uint32_t num_periods;
	uint32_t dsp_buf;
	atomic_t hw_ptr;
};

struct audioreach_graph {
	struct audioreach_graph_info *info;
	uint32_t id;
	/* Unique for this cached instance; graph IDs may be reused after close. */
	u64 instance_cookie;
	int start_count;
	/* Cached Graph data */
	void *graph;
	struct kref refcount;
	/* Runs a final close outside the single-threaded GPR receive worker. */
	struct work_struct release_work;
	/* Protects graph start/stop reference state. */
	struct mutex state_lock;
	bool flush_pending;
	u32 flush_generation;
	bool operation_pending;
	u32 pending_opcode;
	bool pending_flush;
	/* The IDR/cache reference is retained while close/reset is quarantined. */
	bool cache_ref;
	bool graph_quarantined;
	bool graph_close_confirmed;
	bool graph_open_ambiguous;
	bool graph_reset_retired;
	bool graph_release_work_queued;
	bool destroying;
	/* Number of client handles holding an ar_graph reference. */
	u32 clients;
	struct q6apm *apm;
};

/*
 * Client callbacks run serialized from the GPR receive workqueue and may
 * sleep.  @payload is owned by the receive packet and must not be retained;
 * failed or malformed data completions may pass a NULL payload with zero
 * bytes.  q6apm_graph_close() is permitted from a callback, but invalidates
 * the graph handle immediately and defers destruction until the callback
 * returns.  It detaches @cb and @priv before returning; outside a callback it
 * also waits for an in-flight callback to retire.  A quarantined DSP graph
 * therefore cannot retain access to the client's callback owner.
 */
typedef void (*q6apm_cb) (uint32_t opcode, uint32_t token,
				  uint32_t bytes, void *payload, void *priv);
struct q6apm_graph {
	struct list_head node;
	void *priv;
	q6apm_cb cb;
	uint32_t id;
	/* Identity of the cached graph instance used by port responses. */
	u64 graph_cookie;
	uint32_t shm_iid;
	struct device *dev;
	struct q6apm *apm;
	gpr_port_t *port;
	struct audioreach_graph_data rx_data;
	struct audioreach_graph_data tx_data;
	wait_queue_head_t cmd_wait;
	wait_queue_head_t data_wait;
	struct mutex lock;
	/* Serializes client callbacks against flush and buffer teardown. */
	struct mutex callback_lock;
	/* Serializes callback dispatch and close-state transitions. */
	struct mutex lifecycle_lock;
	struct task_struct *callback_task;
	struct kref refcount;
	struct work_struct close_work;
	bool closing;
	bool listed;
	bool apm_ref;
	struct mutex response_lock;
	uint32_t pending_cmd_token;
	uint32_t pending_cmd_opcode;
	u32 pending_cmd_generation;
	u64 pending_cmd_graph_cookie;
	int command_result;
	uint32_t pending_eos_token;
	u32 flush_generation;
	bool command_pending;
	bool command_ambiguous;
	/* Held from command publication until a valid response or trusted reset. */
	bool command_ref_held;
	bool eos_pending;
	bool flushing;
	struct audioreach_graph *ar_graph;
	struct audioreach_graph_info *info;
};

uint32_t q6apm_next_token(void);

/* Graph Operations */
struct q6apm_graph *q6apm_graph_open(struct device *dev, q6apm_cb cb,
				     void *priv, int graph_id, int dir);
int q6apm_graph_close(struct q6apm_graph *graph);
int q6apm_graph_prepare(struct q6apm_graph *graph);
int q6apm_graph_start(struct q6apm_graph *graph);
int q6apm_graph_rearm(struct q6apm_graph *graph);
int q6apm_graph_stop(struct q6apm_graph *graph);
int q6apm_graph_stop_with_state(struct q6apm_graph *graph, bool *last);
bool q6apm_graph_request_flush(struct q6apm_graph *graph);
int q6apm_graph_flush(struct q6apm_graph *graph);
bool q6apm_graph_sync_flush_generation(struct q6apm_graph *graph);
bool q6apm_graph_flush_pending(struct q6apm_graph *graph);
void q6apm_graph_reset_client(struct q6apm_graph *graph);
void q6apm_graph_quiesce_callbacks(struct q6apm_graph *graph);
bool q6apm_graph_try_reset_client(struct q6apm_graph *graph);
int q6apm_graph_wait_for_client_idle(struct q6apm_graph *graph,
				     unsigned long timeout);

/* Media Format */
int q6apm_graph_media_format_pcm(struct q6apm_graph *graph,
				 struct audioreach_module_config *cfg);

int q6apm_graph_media_format_shmem(struct q6apm_graph *graph,
				   struct audioreach_module_config *cfg);

/* read/write related */
int q6apm_read(struct q6apm_graph *graph);
int q6apm_write_async(struct q6apm_graph *graph, uint32_t len, uint32_t msw_ts,
		      uint32_t lsw_ts, uint32_t wflags);

/* Memory Map related */
struct q6apm *q6apm_get_apm(struct device *dev);
void q6apm_put_apm(struct q6apm *apm);
unsigned int q6apm_get_reset_generation(struct q6apm *apm);
/*
 * @lease_generation is zero unless this call leaves DSP ownership possible.
 * A nonzero value is the exact generation the caller must pass to UNMAP,
 * including when MAP returns an ambiguity error and DMA must be quarantined.
 */
int q6apm_map_memory_fixed_region(struct q6apm *apm,
				  unsigned int graph_id, phys_addr_t phys,
				  size_t sz,
				  unsigned int *lease_generation);
int q6apm_alloc_fragments(struct q6apm_graph *graph,
			unsigned int dir, phys_addr_t phys,
			size_t period_sz, unsigned int periods);
int q6apm_free_fragments(struct q6apm_graph *graph, unsigned int dir);
/* UNMAP never consumes a same-ID lease from a different generation. */
int q6apm_unmap_memory_fixed_region(struct q6apm *apm,
				    unsigned int graph_id,
				    unsigned int expected_generation);
/* Helpers */
int q6apm_send_cmd_sync(struct q6apm *apm, const struct gpr_pkt *pkt,
			uint32_t rsp_opcode);
int q6apm_send_cmd_sync_graph(struct q6apm_graph *graph,
			      const struct gpr_pkt *pkt,
			      uint32_t rsp_opcode);
int q6apm_send_cmd_sync_graph_port(struct q6apm_graph *graph,
				   const struct gpr_pkt *pkt,
				   uint32_t rsp_opcode);

/* Callback for graph specific */
struct audioreach_module *q6apm_find_module_by_mid(struct q6apm_graph *graph,
						    uint32_t mid);
int q6apm_graph_get_rx_shmem_module_iid(struct q6apm_graph *graph);

bool q6apm_is_adsp_ready(void);

int q6apm_enable_compress_module(struct device *dev, struct q6apm_graph *graph, bool en);
int q6apm_remove_initial_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples);
int q6apm_remove_trailing_silence(struct device *dev, struct q6apm_graph *graph, uint32_t samples);
int q6apm_set_real_module_id(struct device *dev, struct q6apm_graph *graph, uint32_t codec_id);
int q6apm_get_hw_pointer(struct q6apm_graph *graph, int dir);
#endif /* __APM_GRAPH_ */
