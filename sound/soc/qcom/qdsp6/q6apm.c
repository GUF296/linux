// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/soc/qcom,gpr.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
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

static void q6apm_release(struct kref *ref);
static void q6apm_mem_lease_release(struct kref *ref);
static void q6apm_cmd_ticket_release(struct kref *ref);
static void q6apm_put_audioreach_graph(struct kref *ref);
static void q6apm_release_audioreach_graph_work(struct work_struct *work);
static void q6apm_graph_release(struct kref *ref);
static void q6apm_queue_graph_release(struct audioreach_graph *graph);
static void q6apm_graph_put_ref(struct audioreach_graph *graph);
static void q6apm_retire_port_commands(struct q6apm *apm, u64 graph_cookie,
				       bool reset);

static void q6apm_maybe_queue_graph_release(struct audioreach_graph *graph)
{
	struct q6apm *apm;
	bool queue = false;

	if (!graph)
		return;
	apm = graph->apm;
	if (!apm)
		return;

	/*
	 * The cache reference keeps the graph alive while the worker is queued.
	 * The worker itself checks temporary refs and pending operations; queue on
	 * every transition to a clientless cache so a later ref/operation drop can
	 * retry even if the first worker exited before marking quarantine.
	 */
	mutex_lock(&apm->lock);
	if (graph->cache_ref && !graph->destroying) {
		mutex_lock(&graph->state_lock);
		queue = !graph->clients;
		mutex_unlock(&graph->state_lock);
	}
	mutex_unlock(&apm->lock);
	if (queue)
		q6apm_queue_graph_release(graph);
}

/*
 * Authenticate and pin the currently published APM while dispatching a
 * response.  APR quiesces new callbacks before the client remove callback;
 * the kref also protects a callback already admitted through this helper.
 */
static struct q6apm *q6apm_get_apm_from_gdev(gpr_device_t *gdev)
{
	struct q6apm *apm;

	if (!gdev)
		return NULL;

	mutex_lock(&g_apm_lock);
	apm = dev_get_drvdata(&gdev->dev);
	if (!apm || apm != g_apm || !kref_get_unless_zero(&apm->refcount))
		apm = NULL;
	mutex_unlock(&g_apm_lock);

	return apm;
}

uint32_t q6apm_next_token(void)
{
	uint32_t token;

	do {
		token = (uint32_t)atomic_inc_return(&q6apm_token);
	} while (!token);

	return token;
}

static u64 q6apm_next_graph_cookie(struct q6apm *apm)
{
	u64 cookie;

	/* A graph ID is reusable; the cookie is the identity used by late replies. */
	do {
		cookie = (u64)atomic64_inc_return(&apm->graph_cookie_seq);
	} while (!cookie);

	return cookie;
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
	/* Ticket/lease references must keep the parent alive until they are
	 * unlinked and their own krefs have reached zero.  Never raw-free a
	 * kref-managed child from this callback: a late response may still own it.
	 */
	if (WARN_ON_ONCE(!list_empty(&apm->cmd_tickets)) ||
	    WARN_ON_ONCE(!list_empty(&apm->mem_leases)) ||
	    WARN_ON_ONCE(!list_empty(&apm->graph_clients)))
		return;
	idr_destroy(&apm->graph_idr);
	idr_destroy(&apm->graph_info_idr);
	idr_destroy(&apm->sub_graphs_idr);
	idr_destroy(&apm->containers_idr);
	idr_destroy(&apm->modules_idr);
	kfree(apm);
}

static void q6apm_mem_lease_release(struct kref *ref)
{
	struct q6apm_mem_lease *lease =
		container_of(ref, struct q6apm_mem_lease, refcount);
	struct q6apm *apm = lease->apm;
	bool put_apm = lease->apm_ref;

	lease->apm_ref = false;
	kfree(lease);
	if (put_apm)
		q6apm_put_apm(apm);
}

static void q6apm_cmd_ticket_release(struct kref *ref)
{
	struct q6apm_cmd_ticket *ticket =
		container_of(ref, struct q6apm_cmd_ticket, refcount);
	struct audioreach_graph *graph = ticket->graph;
	bool graph_ref = ticket->graph_ref;

	if (ticket->lease)
		kref_put(&ticket->lease->refcount, q6apm_mem_lease_release);
	if (graph_ref)
		q6apm_graph_put_ref(graph);
	if (ticket->apm_ref)
		kref_put(&ticket->apm->refcount, q6apm_release);
	kfree(ticket);
}

struct q6apm *q6apm_get_apm(struct device *dev)
{
	struct q6apm *apm = NULL;
	struct q6apm *candidate;

	if (!dev || !dev->parent)
		return NULL;

	mutex_lock(&g_apm_lock);
	candidate = dev_get_drvdata(dev->parent);
	if (candidate && candidate == g_apm &&
	    kref_get_unless_zero(&candidate->refcount))
		apm = candidate;
	mutex_unlock(&g_apm_lock);

	return apm;
}
EXPORT_SYMBOL_GPL(q6apm_get_apm);

void q6apm_put_apm(struct q6apm *apm)
{
	if (apm)
		kref_put(&apm->refcount, q6apm_release);
}
EXPORT_SYMBOL_GPL(q6apm_put_apm);

unsigned int q6apm_get_reset_generation(struct q6apm *apm)
{
	if (!apm)
		return 0;

	return atomic_read(&apm->reset_generation);
}
EXPORT_SYMBOL_GPL(q6apm_get_reset_generation);

static struct q6apm_cmd_ticket *
q6apm_find_ticket_locked(struct q6apm *apm, u32 token)
{
	struct q6apm_cmd_ticket *ticket = NULL;

	lockdep_assert_held(&apm->response_lock);
	list_for_each_entry(ticket, &apm->cmd_tickets, node)
		if (ticket->token == token)
			return ticket;

	return NULL;
}

static bool
q6apm_has_unresolved_ticket_locked(struct q6apm *apm, u32 graph_id,
				   u64 graph_cookie,
				   enum q6apm_cmd_kind kind,
				   struct q6apm_mem_lease *lease)
{
	struct q6apm_cmd_ticket *ticket;

	lockdep_assert_held(&apm->response_lock);
	list_for_each_entry(ticket, &apm->cmd_tickets, node) {
		bool active = ticket->state == Q6APM_CMD_PENDING ||
			ticket->state == Q6APM_CMD_AMBIGUOUS;
		bool same_lease = lease && ticket->lease == lease;
		bool same_graph = graph_cookie &&
			ticket->graph_id == graph_id &&
			ticket->graph_cookie == graph_cookie;

		if (active &&
		    (same_lease || same_graph ||
		     (!lease && !graph_cookie && ticket->kind == kind &&
		      ticket->graph_id == graph_id)))
			return true;
	}

	return false;
}

static struct q6apm_mem_lease *
q6apm_find_lease_locked(struct q6apm *apm, u32 generation, u32 graph_id)
{
	struct q6apm_mem_lease *lease;

	lockdep_assert_held(&apm->lock);
	list_for_each_entry(lease, &apm->mem_leases, node)
		if (lease->generation == generation && lease->graph_id == graph_id)
			return lease;

	return NULL;
}

static struct q6apm_mem_lease *
q6apm_get_lease_locked(struct q6apm *apm, u32 graph_id)
{
	struct q6apm_mem_lease *lease;
	u32 generation = atomic_read(&apm->reset_generation);

	lockdep_assert_held(&apm->lock);
	lease = q6apm_find_lease_locked(apm, generation, graph_id);
	if (lease)
		return lease;

	lease = kzalloc_obj(*lease);
	if (!lease)
		return NULL;
	lease->apm = apm;
	lease->graph_id = graph_id;
	lease->generation = generation;
	lease->state = AUDIOREACH_MEM_UNMAPPED;
	kref_init(&lease->refcount);
	kref_get(&apm->refcount);
	lease->apm_ref = true;
	list_add_tail(&lease->node, &apm->mem_leases);
	return lease;
}

/*
 * Remove a ticket from the authentication registry.  The registry reference
 * is deliberately not dropped here: callers may hold response_lock, while
 * q6apm_cmd_ticket_release() can release an APM/lease reference and must run
 * after that lock is dropped.
 */
static bool q6apm_ticket_unlink_locked(struct q6apm_cmd_ticket *ticket)
{
	struct q6apm *apm = ticket->apm;

	lockdep_assert_held(&apm->response_lock);
	if (!ticket->listed)
		return false;

	list_del_init(&ticket->node);
	ticket->listed = false;
	return true;
}

static bool q6apm_lease_has_ticket_locked(struct q6apm *apm,
					  struct q6apm_mem_lease *lease)
{
	struct q6apm_cmd_ticket *ticket;

	lockdep_assert_held(&apm->response_lock);
	list_for_each_entry(ticket, &apm->cmd_tickets, node)
		if (ticket->lease == lease)
			return true;

	return false;
}

/*
 * Retire only leases whose generation/ownership is no longer usable.  A
 * ticket reference is checked under response_lock before the registry
 * reference is removed, so a late response can never observe freed storage.
 */
static void q6apm_retire_old_leases(struct q6apm *apm)
{
	struct q6apm_cmd_ticket *ticket, *ticket_tmp;
	struct q6apm_mem_lease *lease, *tmp;
	LIST_HEAD(retired_tickets);
	LIST_HEAD(retired);
	unsigned int generation;

	if (!apm)
		return;

	generation = atomic_read(&apm->reset_generation);
	mutex_lock(&apm->response_lock);
	mutex_lock(&apm->lock);
	/*
	 * A terminal MAP ticket is retained while its lease may still belong to
	 * the DSP.  Once the exact lease is retired, remove that transcript before
	 * dropping the lease's registry reference.  This keeps late callbacks from
	 * observing a freed ticket while bounding the normal successful lifecycle.
	 */
	list_for_each_entry_safe(ticket, ticket_tmp, &apm->cmd_tickets, node) {
		struct q6apm_mem_lease *ticket_lease = ticket->lease;
		bool terminal = ticket->state != Q6APM_CMD_PENDING &&
			ticket->state != Q6APM_CMD_AMBIGUOUS;

		if (ticket->kind != Q6APM_CMD_MAP || !ticket_lease ||
		    ticket->waiter_active || !terminal ||
		    (ticket_lease->state != AUDIOREACH_MEM_UNMAPPED &&
		     ticket_lease->state != AUDIOREACH_MEM_RESET_RETIRED))
			continue;
		if (ticket_lease->map_ticket == ticket)
			ticket_lease->map_ticket = NULL;
		list_del_init(&ticket->node);
		ticket->listed = false;
		list_add_tail(&ticket->node, &retired_tickets);
	}
	list_for_each_entry_safe(lease, tmp, &apm->mem_leases, node) {
		bool stale_generation = lease->generation != generation;
		bool terminal = lease->state == AUDIOREACH_MEM_RESET_RETIRED ||
			(lease->state == AUDIOREACH_MEM_UNMAPPED && !lease->users);

		if (!stale_generation && !terminal)
			continue;
		if (q6apm_lease_has_ticket_locked(apm, lease))
			continue;
		list_del_init(&lease->node);
		list_add_tail(&lease->node, &retired);
	}
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->response_lock);

	list_for_each_entry_safe(ticket, ticket_tmp, &retired_tickets, node) {
		list_del_init(&ticket->node);
		kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
	}
	list_for_each_entry_safe(lease, tmp, &retired, node) {
		list_del_init(&lease->node);
		kref_put(&lease->refcount, q6apm_mem_lease_release);
	}
}

static void q6apm_lease_clear_locked(struct q6apm_mem_lease *lease,
				     enum q6apm_mem_lease_state state)
{
	struct q6apm *apm = lease->apm;

	lockdep_assert_held(&apm->lock);
	lease->handle = 0;
	lease->users = 0;
	lease->state = state;
	lease->map_ticket = NULL;
}

static void q6apm_clear_graph_operations_locked(struct q6apm *apm)
{
	struct audioreach_graph *graph;
	int id;

	lockdep_assert_held(&apm->lock);
	/*
	 * A reset retires the corresponding command tickets without a DSP
	 * response.  Drop only the in-flight operation marker here; a committed
	 * flush request remains visible so the next client operation can decide
	 * whether it needs to flush again.
	 */
	idr_for_each_entry(&apm->graph_idr, graph, id) {
		mutex_lock(&graph->state_lock);
		graph->operation_pending = false;
		graph->pending_flush = false;
		mutex_unlock(&graph->state_lock);
	}
}

static void q6apm_ticket_rollback_locked(struct q6apm_cmd_ticket *ticket)
{
	struct q6apm_mem_lease *lease = ticket->lease;

	if (!lease)
		return;

	lockdep_assert_held(&ticket->apm->lock);
	if (ticket->kind == Q6APM_CMD_MAP &&
	    (lease->state == AUDIOREACH_MEM_MAP_PENDING ||
	     lease->state == AUDIOREACH_MEM_MAP_UNCERTAIN))
		q6apm_lease_clear_locked(lease, AUDIOREACH_MEM_UNMAPPED);
	else if (ticket->kind == Q6APM_CMD_UNMAP &&
		 (lease->state == AUDIOREACH_MEM_UNMAP_PENDING ||
		  lease->state == AUDIOREACH_MEM_UNMAP_UNCERTAIN))
		lease->state = AUDIOREACH_MEM_MAPPED;
}

static struct audioreach_graph *
q6apm_apply_graph_result(struct q6apm *apm, u32 graph_id, u64 graph_cookie,
			 u32 opcode, u32 status, bool flush_after_stop)
{
	struct audioreach_graph *graph;
	bool queue_release = false;

	/* apm->lock protects the IDR lookup and pins the graph with its kref. */
	mutex_lock(&apm->lock);
	graph = idr_find(&apm->graph_idr, graph_id);
	if (graph && (graph->instance_cookie != graph_cookie || graph->destroying))
		graph = NULL;
	if (graph && !kref_get_unless_zero(&graph->refcount))
		graph = NULL;
	mutex_unlock(&apm->lock);
	if (graph) {
		mutex_lock(&graph->state_lock);
		if (opcode == APM_CMD_GRAPH_OPEN &&
		    graph->graph_open_ambiguous) {
			/*
			 * A late OPEN settles the quarantine.  A successful OPEN still
			 * needs a serialized CLOSE; an error proves no DSP graph exists.
			 */
			graph->graph_open_ambiguous = false;
			graph->graph_quarantined = true;
			graph->graph_close_confirmed = !!status;
			queue_release = true;
		}
		if (opcode == APM_CMD_GRAPH_CLOSE && graph->graph_quarantined) {
			graph->graph_close_confirmed = !status;
			queue_release = true;
		}
		if (graph->operation_pending &&
		    graph->pending_opcode == opcode) {
			if (!status) {
				switch (opcode) {
				case APM_CMD_GRAPH_START:
					if (!graph->start_count)
						graph->start_count = 1;
					break;
				case APM_CMD_GRAPH_STOP:
					graph->start_count = 0;
					graph->flush_pending = flush_after_stop;
					break;
				case APM_CMD_GRAPH_FLUSH:
					graph->flush_pending = false;
					graph->flush_generation++;
					break;
				default:
					break;
				}
			}
			graph->pending_flush = false;
			graph->operation_pending = false;
		}
		mutex_unlock(&graph->state_lock);
		if (queue_release)
			q6apm_queue_graph_release(graph);
	}
	/* The caller must drop this reference after response_lock is released. */
	return graph;
}

static bool q6apm_graph_result_ambiguous(int ret)
{
	/* A timeout or malformed response may leave the command executing in DSP. */
	return ret == -ETIMEDOUT || ret == -EPROTO;
}

static void q6apm_ticket_complete_locked(struct q6apm_cmd_ticket *ticket,
					 enum q6apm_cmd_state state,
					 int result)
{
	lockdep_assert_held(&ticket->apm->response_lock);
	ticket->state = state;
	ticket->result = result;
	complete_all(&ticket->done);
}

/*
 * MAP tickets are also response transcripts.  Keep them registered while the
 * exact lease can still be owned by the DSP; otherwise a late basic/handle
 * phase would be misclassified as an unrelated token and could not poison a
 * contradictory transaction.  The caller must hold response_lock before
 * taking apm->lock, matching the callback and retirement paths.
 */
static bool q6apm_map_ticket_retained_locked(struct q6apm_cmd_ticket *ticket)
{
	struct q6apm_mem_lease *lease;
	struct q6apm *apm;
	bool retained = false;

	if (!ticket || ticket->kind != Q6APM_CMD_MAP || !ticket->lease)
		return false;

	apm = ticket->apm;
	lockdep_assert_held(&apm->response_lock);
	lease = ticket->lease;
	mutex_lock(&apm->lock);
	retained = lease->map_ticket == ticket &&
		lease->generation == ticket->generation &&
		lease->state != AUDIOREACH_MEM_UNMAPPED &&
		lease->state != AUDIOREACH_MEM_RESET_RETIRED;
	mutex_unlock(&apm->lock);

	return retained;
}

static int __q6apm_send_cmd_sync(struct q6apm *apm,
				 const struct gpr_pkt *pkt, u32 rsp_opcode,
				 enum q6apm_cmd_kind kind,
				 struct q6apm_mem_lease *lease,
				 struct audioreach_graph *graph,
				 u32 expected_generation, u32 lease_handle)
{
	gpr_device_t *gdev;
	struct gpr_pkt *tx_pkt;
	struct q6apm_cmd_ticket *ticket = NULL;
	u32 graph_id;
	u32 generation;
	bool registry_unlinked = false;
	bool online_locked = false;
	bool cmd_locked = false;
	int ret;

	if (!apm || !pkt)
		return -EINVAL;
	if (!kref_get_unless_zero(&apm->refcount))
		return -ENODEV;

	tx_pkt = (struct gpr_pkt *)pkt;
	graph_id = graph ? graph->id : tx_pkt->hdr.token;

	mutex_lock(&apm->online_lock);
	online_locked = true;
	if (!READ_ONCE(apm->online)) {
		ret = -ENETRESET;
		goto out;
	}
	generation = atomic_read(&apm->reset_generation);
	if (expected_generation && expected_generation != generation) {
		ret = -ENETRESET;
		goto out;
	}

	mutex_lock(&apm->cmd_lock);
	cmd_locked = true;
	/* Serialize the generation check and ticket publication with reset. */
	if (!READ_ONCE(apm->online) ||
	    generation != atomic_read(&apm->reset_generation)) {
		ret = -ENETRESET;
		goto out;
	}
	mutex_lock(&apm->response_lock);
	if (q6apm_has_unresolved_ticket_locked(apm, graph_id,
					       graph ? graph->instance_cookie : 0,
					       kind, lease)) {
		mutex_unlock(&apm->response_lock);
		ret = -EBUSY;
		goto out;
	}
	mutex_unlock(&apm->response_lock);
	ticket = kzalloc_obj(*ticket);
	if (!ticket) {
		ret = -ENOMEM;
		goto out;
	}
	kref_init(&ticket->refcount);
	INIT_LIST_HEAD(&ticket->node);
	init_completion(&ticket->done);
	ticket->apm = apm;
	ticket->lease = lease;
	kref_get(&apm->refcount);
	ticket->apm_ref = true;
	if (lease)
		kref_get(&lease->refcount);
	if (graph) {
		/* A late response may outlive the caller's temporary graph reference. */
		if (!kref_get_unless_zero(&graph->refcount)) {
			ret = -ENODEV;
			kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
			ticket = NULL;
			goto out;
		}
		ticket->graph = graph;
		ticket->graph_ref = true;
	}
	mutex_lock(&apm->response_lock);
	if (graph) {
		/*
		 * Serialize publication against graph destruction.  The destruction
		 * path checks response_lock before taking apm->lock, so retain that
		 * order here.
		 */
		mutex_lock(&apm->lock);
		if (graph->destroying || !graph->cache_ref) {
			mutex_unlock(&apm->lock);
			mutex_unlock(&apm->response_lock);
			ret = -ENODEV;
			goto out;
		}
		mutex_unlock(&apm->lock);
	}
	/* Do not let a 32-bit token wrap alias a still-authenticated ticket. */
	do {
		ticket->token = q6apm_next_token();
	} while (q6apm_find_ticket_locked(apm, ticket->token));
	ticket->opcode = tx_pkt->hdr.opcode;
	ticket->graph_id = graph_id;
	ticket->graph_cookie = graph ? graph->instance_cookie : 0;
	ticket->response_opcode = rsp_opcode;
	ticket->generation = generation;
	ticket->lease_handle = lease_handle;
	ticket->kind = kind;
	if (graph && tx_pkt->hdr.opcode == APM_CMD_GRAPH_STOP) {
		mutex_lock(&graph->state_lock);
		ticket->flush_after_stop = graph->pending_flush;
		mutex_unlock(&graph->state_lock);
	}
	ticket->state = Q6APM_CMD_PENDING;
	ticket->waiter_active = true;
	if (kind == Q6APM_CMD_MAP && lease) {
		/* The MAP caller serialized this pending lease under online_lock. */
		mutex_lock(&apm->lock);
		if (lease->generation != generation ||
		    lease->state != AUDIOREACH_MEM_MAP_PENDING ||
		    lease->map_ticket) {
			mutex_unlock(&apm->lock);
			mutex_unlock(&apm->response_lock);
			ret = -EBUSY;
			goto out;
		}
		lease->map_ticket = ticket;
		mutex_unlock(&apm->lock);
	}
	kref_get(&ticket->refcount); /* registry reference */
	ticket->listed = true;
	list_add_tail(&ticket->node, &apm->cmd_tickets);
	tx_pkt->hdr.token = ticket->token;
	mutex_unlock(&apm->response_lock);

	gdev = READ_ONCE(apm->gdev);
	ret = gdev ? gpr_send_pkt(gdev, pkt) : -ENODEV;
	if (ret < 0) {
		mutex_lock(&apm->response_lock);
		if (ticket->state == Q6APM_CMD_PENDING) {
			mutex_lock(&apm->lock);
			q6apm_ticket_rollback_locked(ticket);
			mutex_unlock(&apm->lock);
			ticket->waiter_active = false;
			q6apm_ticket_complete_locked(ticket,
						     Q6APM_CMD_CONFIRMED_ERROR,
						     ret);
		} else {
			/*
			 * A response may race a transport error.  Preserve the
			 * authenticated response and its ownership state.
			 */
			ret = ticket->result;
			ticket->waiter_active = false;
		}
		if (ticket->state != Q6APM_CMD_AMBIGUOUS &&
		    !q6apm_map_ticket_retained_locked(ticket))
			registry_unlinked = q6apm_ticket_unlink_locked(ticket);
		mutex_unlock(&apm->response_lock);
		goto out;
	}
	mutex_unlock(&apm->online_lock);
	online_locked = false;

	wait_for_completion_timeout(&ticket->done, 5 * HZ);
	mutex_lock(&apm->response_lock);
	if (ticket->state == Q6APM_CMD_PENDING) {
		/* The token remains registered: a late response still owns the lease. */
		ticket->state = Q6APM_CMD_AMBIGUOUS;
		ticket->result = -ETIMEDOUT;
		ticket->waiter_active = false;
		ret = -ETIMEDOUT;
	} else {
		ret = ticket->result;
	}
	ticket->waiter_active = false;
	if (ticket->state != Q6APM_CMD_AMBIGUOUS &&
	    !q6apm_map_ticket_retained_locked(ticket))
		registry_unlinked = q6apm_ticket_unlink_locked(ticket);
	mutex_unlock(&apm->response_lock);

out:
	if (online_locked)
		mutex_unlock(&apm->online_lock);
	if (cmd_locked)
		mutex_unlock(&apm->cmd_lock);
	if (registry_unlinked)
		kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
	if (ticket)
		kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
	kref_put(&apm->refcount, q6apm_release);
	return ret;
}

int q6apm_send_cmd_sync(struct q6apm *apm, const struct gpr_pkt *pkt,
			uint32_t rsp_opcode)
{
	return __q6apm_send_cmd_sync(apm, pkt, rsp_opcode,
						     Q6APM_CMD_GENERIC, NULL, NULL, 0, 0);
}

/*
 * Take a reference to the cached AudioReach graph while its client handle is
 * still live.  q6apm_graph_finish_close() uses the same graph lock when it
 * clears @ar_graph, so a close cannot free the object between the load and
 * kref_get_unless_zero().
 */
static struct audioreach_graph *
q6apm_graph_get_ar_graph(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;

	if (!graph)
		return NULL;

	mutex_lock(&graph->lock);
	if (READ_ONCE(graph->closing)) {
		ar_graph = NULL;
	} else {
		ar_graph = READ_ONCE(graph->ar_graph);
		if (ar_graph) {
			mutex_lock(&ar_graph->state_lock);
			if (ar_graph->destroying || ar_graph->graph_quarantined ||
			    !kref_get_unless_zero(&ar_graph->refcount))
				ar_graph = NULL;
			mutex_unlock(&ar_graph->state_lock);
		}
	}
	mutex_unlock(&graph->lock);

	return ar_graph;
}

int q6apm_send_cmd_sync_graph(struct q6apm_graph *graph,
			      const struct gpr_pkt *pkt,
			      uint32_t rsp_opcode)
{
	struct audioreach_graph *ar_graph;
	struct q6apm *apm;
	int ret;

	if (!graph || !pkt)
		return -EINVAL;

	/* Pin the cached graph before dropping the client-handle lock. */
	apm = READ_ONCE(graph->apm);
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return -ENODEV;
	if (!apm) {
		q6apm_graph_put_ref(ar_graph);
		return -ENODEV;
	}

	ret = __q6apm_send_cmd_sync(apm, pkt, rsp_opcode,
				    Q6APM_CMD_GENERIC, NULL, ar_graph, 0, 0);
	q6apm_graph_put_ref(ar_graph);

	return ret;
}

/*
 * Port commands receive their basic response on the graph port rather than
 * the APM service port.  Keep the same authenticated terminal-state rules as
 * the APM ticket path, but store the ticket on the persistent graph handle so
 * a timeout cannot be consumed by a later SET_CFG operation.
 */
int q6apm_send_cmd_sync_graph_port(struct q6apm_graph *graph,
				   const struct gpr_pkt *pkt,
				   uint32_t rsp_opcode)
{
	struct gpr_pkt *tx_pkt = (struct gpr_pkt *)pkt;
	gpr_port_t *port;
	struct q6apm *apm;
	unsigned long timeout;
	u32 token;
	u32 generation;
	bool callback_owner;
	bool callback_locked = false;
	bool online_locked = false;
	unsigned int command_refs_to_drop = 0;
	int ret;

	if (!graph || !pkt)
		return -EINVAL;
	/*
	 * A graph port currently delivers the APR/GPR basic result.  Refuse an
	 * unsupported secondary response instead of arming a waiter that can
	 * never be completed by graph_callback().
	 */
	if (rsp_opcode && rsp_opcode != GPR_BASIC_RSP_RESULT)
		return -EINVAL;
	if (!kref_get_unless_zero(&graph->refcount))
		return -ENODEV;

	mutex_lock(&graph->lifecycle_lock);
	callback_owner = graph->callback_task == current;
	mutex_unlock(&graph->lifecycle_lock);
	if (!callback_owner) {
		mutex_lock(&graph->callback_lock);
		callback_locked = true;
	}

	mutex_lock(&graph->lock);
	port = graph->port;
	apm = graph->apm;
	if (READ_ONCE(graph->closing) || !port || !graph->ar_graph || !apm) {
		ret = -ENODEV;
		goto unlock_graph;
	}
	mutex_lock(&apm->online_lock);
	online_locked = true;
	if (!READ_ONCE(apm->online)) {
		ret = -ENETRESET;
		goto unlock_online;
	}
	generation = q6apm_get_reset_generation(apm);
	if (!generation) {
		ret = -ENETRESET;
		goto unlock_online;
	}

	mutex_lock(&graph->response_lock);
	if ((graph->command_pending || graph->command_ambiguous) &&
	    graph->pending_cmd_generation != generation) {
		/*
		 * A generation transition is the only host-side evidence that can
		 * retire an unanswered port command.
		 */
		graph->command_pending = false;
		graph->command_ambiguous = false;
		graph->command_result = -ENETRESET;
		wake_up_all(&graph->cmd_wait);
		if (graph->command_ref_held) {
			graph->command_ref_held = false;
			command_refs_to_drop++;
		}
	}
	if (graph->command_pending || graph->command_ambiguous) {
		ret = -EBUSY;
	} else {
		token = q6apm_next_token();
		tx_pkt->hdr.token = token;
		graph->pending_cmd_token = token;
		graph->pending_cmd_opcode = tx_pkt->hdr.opcode;
		graph->pending_cmd_generation = generation;
		graph->pending_cmd_graph_cookie = graph->graph_cookie;
		graph->command_result = -EINPROGRESS;
		graph->command_pending = true;
		/*
		 * Keep the handle alive after a timeout until the authenticated late
		 * response or a trusted reset retires this command.
		 */
		kref_get(&graph->refcount);
		graph->command_ref_held = true;
		ret = 0;
	}
	mutex_unlock(&graph->response_lock);
	if (ret)
		goto unlock_online;

	ret = gpr_send_port_pkt(port, pkt);
	if (ret < 0) {
		mutex_lock(&graph->response_lock);
		if (graph->command_pending && graph->pending_cmd_token == token) {
			graph->command_pending = false;
			graph->command_ambiguous = false;
			graph->command_result = ret;
			wake_up_all(&graph->cmd_wait);
			if (graph->command_ref_held) {
				graph->command_ref_held = false;
				command_refs_to_drop++;
			}
		} else if (graph->pending_cmd_token == token) {
			ret = graph->command_result;
		}
		mutex_unlock(&graph->response_lock);
		goto unlock_online;
	}

	/*
	 * Keep callback_lock across the wait, preventing graph_close() from
	 * freeing the port while a response is still eligible to arrive.
	 */
	mutex_unlock(&apm->online_lock);
	online_locked = false;
	mutex_unlock(&graph->lock);
	timeout = wait_event_timeout(graph->cmd_wait,
				     !READ_ONCE(graph->command_pending),
				     5 * HZ);
	mutex_lock(&graph->response_lock);
	if (graph->pending_cmd_token != token) {
		ret = -ENETRESET;
	} else if (graph->command_pending) {
		/*
		 * The DSP may have executed the command; retain the authenticated
		 * token in the ambiguous state and reject reuse until a matching
		 * late response or trusted generation transition arrives.
		 */
		if (!timeout)
			dev_dbg(graph->dev, "graph command timeout token %u\n", token);
		graph->command_pending = false;
		graph->command_ambiguous = true;
		graph->command_result = -ETIMEDOUT;
		ret = -ETIMEDOUT;
	} else {
		ret = graph->command_result;
	}
	mutex_unlock(&graph->response_lock);
	goto unlock_callback;

unlock_online:
	if (online_locked)
		mutex_unlock(&apm->online_lock);
unlock_graph:
	mutex_unlock(&graph->lock);
unlock_callback:
	if (callback_locked)
		mutex_unlock(&graph->callback_lock);
	while (command_refs_to_drop--)
		kref_put(&graph->refcount, q6apm_graph_release);
	kref_put(&graph->refcount, q6apm_graph_release);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_send_cmd_sync_graph_port);

static int audioreach_graph_mgmt_cmd(struct audioreach_graph *graph,
				     uint32_t opcode);
static void q6apm_destroy_audioreach_graph(struct audioreach_graph *graph);
static void q6apm_queue_graph_release(struct audioreach_graph *graph);

static struct audioreach_graph *q6apm_get_audioreach_graph(struct q6apm *apm, uint32_t graph_id)
{
	struct audioreach_graph_info *info;
	struct audioreach_graph *graph;
	bool reset_retired;
	int generation, id, ret;

retry:
	down_read(&apm->graph_lock);
	if (!READ_ONCE(apm->online)) {
		graph = ERR_PTR(-ENETRESET);
		goto unlock_read;
	}

	mutex_lock(&apm->lock);
	graph = idr_find(&apm->graph_idr, graph_id);
	if (graph) {
		mutex_lock(&graph->state_lock);
		reset_retired = graph->graph_reset_retired;
		if (graph->destroying || graph->graph_quarantined ||
		    graph->graph_open_ambiguous || reset_retired) {
			mutex_unlock(&graph->state_lock);
			mutex_unlock(&apm->lock);
			graph = ERR_PTR(reset_retired ? -ENETRESET : -EBUSY);
			goto unlock_read;
		}
		mutex_unlock(&graph->state_lock);
	}
	if (graph && kref_get_unless_zero(&graph->refcount)) {
		mutex_unlock(&apm->lock);
		goto unlock_read;
	}
	generation = atomic_read(&apm->graph_generation);
	mutex_unlock(&apm->lock);
	up_read(&apm->graph_lock);
	if (graph) {
		wait_event(apm->graph_wait,
			   !READ_ONCE(apm->online) ||
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
	if (graph) {
		mutex_lock(&graph->state_lock);
		reset_retired = graph->graph_reset_retired;
		if (graph->destroying || graph->graph_quarantined ||
		    graph->graph_open_ambiguous || reset_retired) {
			mutex_unlock(&graph->state_lock);
			mutex_unlock(&apm->lock);
			graph = ERR_PTR(reset_retired ? -ENETRESET : -EBUSY);
			goto unlock_write;
		}
		mutex_unlock(&graph->state_lock);
	}
	if (graph && kref_get_unless_zero(&graph->refcount)) {
		mutex_unlock(&apm->lock);
		goto unlock_write;
	}
	if (graph) {
		generation = atomic_read(&apm->graph_generation);
		mutex_unlock(&apm->lock);
		up_write(&apm->graph_lock);
		wait_event(apm->graph_wait,
			   !READ_ONCE(apm->online) ||
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
	graph->instance_cookie = q6apm_next_graph_cookie(apm);

	graph->graph = audioreach_alloc_graph_pkt(apm, info);
	if (IS_ERR(graph->graph)) {
		void *err = graph->graph;

		kfree(graph);
		graph = ERR_CAST(err);
		goto unlock_write;
	}
	/*
	 * Preserve the graph identity while q6apm_send_cmd_sync replaces the
	 * transport token with its authenticated command ticket.
	 */
	((struct gpr_pkt *)graph->graph)->hdr.token = graph_id;

	kref_init(&graph->refcount);
	INIT_WORK(&graph->release_work, q6apm_release_audioreach_graph_work);
	mutex_init(&graph->state_lock);
	/*
	 * Publish OPEN as provisionally ambiguous.  The response callback may run
	 * immediately after the synchronous wait expires, so the quarantine must
	 * be visible before the ticket is sent rather than being added by the
	 * timeout path afterwards.
	 */
	graph->graph_open_ambiguous = true;
	graph->graph_quarantined = true;

	/*
	 * Publish the graph before OPEN so a lost OPEN response still has a
	 * stable IDR/cookie identity for a late response or trusted reset.  The
	 * initial kref is the cache reference; callers consume additional refs.
	 */
	mutex_lock(&apm->lock);
	id = idr_alloc(&apm->graph_idr, graph, graph_id, graph_id + 1,
		       GFP_KERNEL);
	if (id < 0) {
		mutex_unlock(&apm->lock);
		kfree(graph->graph);
		kfree(graph);
		graph = ERR_PTR(id);
		goto unlock_write;
	}
	graph->cache_ref = true;
	/*
	 * Keep the creator's raw pointer alive across OPEN and a concurrent reset.
	 * On success this becomes the caller reference; every error path drops it
	 * only after publishing the graph's quarantine/retirement state.
	 */
	kref_get(&graph->refcount);
	kref_get(&apm->refcount);
	atomic_inc(&apm->active_graphs);
	mutex_unlock(&apm->lock);

	ret = __q6apm_send_cmd_sync(apm, graph->graph, 0,
				    Q6APM_CMD_GENERIC, NULL, graph, 0, 0);
	/*
	 * Serialize the final OPEN classification with transport removal.  An
	 * ordinary unbind completes a waiter with -ENODEV but is not evidence that
	 * the DSP rejected OPEN.  Preserve quarantine in that case.  Conversely,
	 * a non-ambiguous local failure while the transport remains online proves
	 * that no remote graph was created.
	 */
	mutex_lock(&apm->online_lock);
	mutex_lock(&graph->state_lock);
	if (!ret) {
		if (!READ_ONCE(apm->online) || graph->graph_reset_retired) {
			ret = -ENETRESET;
		} else {
			graph->graph_open_ambiguous = false;
			graph->graph_quarantined = false;
		}
	} else if (graph->graph_open_ambiguous &&
		   READ_ONCE(apm->online) &&
		   !q6apm_graph_result_ambiguous(ret)) {
		graph->graph_open_ambiguous = false;
		graph->graph_close_confirmed = true;
	}
	mutex_unlock(&graph->state_lock);
	mutex_unlock(&apm->online_lock);
	if (ret) {
		up_write(&apm->graph_lock);
		q6apm_graph_put_ref(graph);
		return ERR_PTR(ret);
	}

	/* The creator reference acquired at publication is the returned client ref. */
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
	u32 num_sub_graphs = info->num_sub_graphs;
	struct apm_module_param_data *param_data;
	struct apm_graph_mgmt_cmd *mgmt_cmd;
	struct audioreach_sub_graph *sg;
	struct q6apm *apm = graph->apm;
	size_t payload_size = APM_GRAPH_MGMT_PSIZE(mgmt_cmd, num_sub_graphs);
	u32 i = 0;
	struct gpr_pkt *pkt __free(kfree) = NULL;

	if (payload_size < sizeof(*mgmt_cmd) || payload_size > U32_MAX)
		return -EOVERFLOW;

	pkt = audioreach_alloc_apm_cmd_pkt(payload_size, opcode, graph->id);
	if (IS_ERR(pkt))
		return PTR_ERR(pkt);

	mgmt_cmd = (void *)pkt + GPR_HDR_SIZE + APM_CMD_HDR_SIZE;

	mgmt_cmd->num_sub_graphs = num_sub_graphs;

	param_data = &mgmt_cmd->param_data;
	param_data->module_instance_id = APM_MODULE_INSTANCE_ID;
	param_data->param_id = APM_PARAM_ID_SUB_GRAPH_LIST;
	param_data->param_size = payload_size - APM_MODULE_PARAM_DATA_SIZE;

	list_for_each_entry(sg, &info->sg_list, node) {
		if (i >= num_sub_graphs)
			return -EINVAL;
		mgmt_cmd->sub_graph_id_list[i++] = sg->sub_graph_id;
	}
	if (i != num_sub_graphs)
		return -EINVAL;

	return __q6apm_send_cmd_sync(apm, pkt, 0, Q6APM_CMD_GENERIC,
					NULL, graph, 0, 0);
}

static bool q6apm_graph_has_ticket(struct audioreach_graph *graph)
{
	struct q6apm_cmd_ticket *ticket;
	bool found = false;

	if (!graph || !graph->apm)
		return false;
	mutex_lock(&graph->apm->response_lock);
	list_for_each_entry(ticket, &graph->apm->cmd_tickets, node) {
		if (ticket->graph == graph) {
			found = true;
			break;
		}
	}
	mutex_unlock(&graph->apm->response_lock);

	return found;
}

/*
 * Destroy only after a confirmed CLOSE or a trusted reset.  The IDR/cache
 * reference is the final ownership barrier; all lookups are rejected once
 * @destroying is published under apm->lock.
 */
static void q6apm_destroy_audioreach_graph(struct audioreach_graph *graph)
{
	struct q6apm *apm;
	u64 graph_cookie;
	bool removed = false;
	bool can_destroy;

	if (!graph || !graph->apm)
		return;
	if (q6apm_graph_has_ticket(graph))
		return;

	apm = graph->apm;
	mutex_lock(&apm->lock);
	mutex_lock(&graph->state_lock);
	can_destroy = graph->cache_ref && !graph->destroying && !graph->clients &&
		(graph->graph_close_confirmed || graph->graph_reset_retired) &&
		refcount_read(&graph->refcount.refcount) == 1;
	if (can_destroy && idr_find(&apm->graph_idr, graph->id) == graph) {
		graph->destroying = true;
		idr_remove(&apm->graph_idr, graph->id);
		graph->cache_ref = false;
		removed = true;
	}
	mutex_unlock(&graph->state_lock);
	mutex_unlock(&apm->lock);
	if (!removed)
		return;

	/*
	 * No other reference can be acquired after @destroying is set and the
	 * object is removed from the IDR.  The worker is the only active caller.
	 */
	graph_cookie = graph->instance_cookie;
	q6apm_retire_port_commands(apm, graph_cookie, false);
	kfree(graph->graph);
	kfree(graph);
	if (atomic_dec_and_test(&apm->active_graphs)) {
		down_write(&apm->graph_lock);
		audioreach_tplg_free_deferred(apm);
		up_write(&apm->graph_lock);
	}
	atomic_inc(&apm->graph_generation);
	wake_up_all(&apm->graph_wait);
	/* The IDR/cache reference kept the parent alive. */
	kref_put(&apm->refcount, q6apm_release);
}

static void q6apm_queue_graph_release(struct audioreach_graph *graph)
{
	bool schedule = false;

	if (!graph)
		return;
	mutex_lock(&graph->state_lock);
	if (!graph->destroying && !graph->graph_release_work_queued) {
		graph->graph_release_work_queued = true;
		schedule = true;
	}
	mutex_unlock(&graph->state_lock);
	if (schedule && !schedule_work(&graph->release_work)) {
		mutex_lock(&graph->state_lock);
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
	}
	if (!schedule && graph->apm)
		dev_dbg(graph->apm->dev,
			"graph %u release work already queued\n", graph->id);
}

static void q6apm_graph_put_ref(struct audioreach_graph *graph)
{
	if (!graph)
		return;

	/*
	 * The cache reference keeps the object alive across a non-final drop.  Test
	 * release eligibility after the decrement so the worker observes the new
	 * refcount under state_lock and cannot strand the sole cache reference.
	 */
	if (!kref_put(&graph->refcount, q6apm_put_audioreach_graph))
		q6apm_maybe_queue_graph_release(graph);
}

static void q6apm_put_audioreach_graph(struct kref *ref)
{
	struct audioreach_graph *graph =
		container_of(ref, struct audioreach_graph, refcount);

	/*
	 * The cache reference normally prevents this callback.  If it does fire,
	 * retain the object as a quarantined work reference rather than freeing a
	 * graph whose DSP ownership is not proven retired.
	 */
	if (graph->destroying) {
		kfree(graph->graph);
		kfree(graph);
		return;
	}
	kref_init(&graph->refcount);
	q6apm_queue_graph_release(graph);
}

static void q6apm_release_audioreach_graph_work(struct work_struct *work)
{
	struct audioreach_graph *graph =
		container_of(work, struct audioreach_graph, release_work);
	struct q6apm *apm = graph->apm;
	bool destroy = false;
	int ret;

	mutex_lock(&graph->state_lock);
	if (graph->destroying) {
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
		return;
	}
	if (!graph->cache_ref || graph->clients || graph->operation_pending ||
	    refcount_read(&graph->refcount.refcount) != 1) {
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
		return;
	}
	graph->graph_quarantined = true;
	if (graph->graph_reset_retired || graph->graph_close_confirmed)
		destroy = true;
	if (!destroy && (!apm || !READ_ONCE(apm->online))) {
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
		return;
	}
	if (destroy) {
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
		q6apm_destroy_audioreach_graph(graph);
		return;
	}
	graph->operation_pending = true;
	graph->pending_opcode = APM_CMD_GRAPH_CLOSE;
	graph->pending_flush = false;
	mutex_unlock(&graph->state_lock);

	ret = audioreach_graph_mgmt_cmd(graph, APM_CMD_GRAPH_CLOSE);
	if (!ret) {
		mutex_lock(&graph->state_lock);
		graph->graph_close_confirmed = true;
		graph->operation_pending = false;
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
		q6apm_destroy_audioreach_graph(graph);
	} else {
		mutex_lock(&graph->state_lock);
		graph->operation_pending = false;
		graph->graph_release_work_queued = false;
		mutex_unlock(&graph->state_lock);
		/*
		 * Both timeout and malformed replies remain quarantined until a
		 * matching late response or trusted reset.
		 */
		dev_warn(apm->dev, "failed to close graph %u: %d\n", graph->id, ret);
	}
}

/*
 * Graph-port commands live on per-client wrappers, not the cached graph IDR.
 * A trusted reset retires every old-generation slot; a confirmed final graph
 * CLOSE also proves that commands for that exact cached graph cookie can no
 * longer own DSP state.  Process one pinned wrapper at a time so retirement is
 * deterministic and cannot silently skip a graph on allocation failure.
 */
static void q6apm_retire_port_commands(struct q6apm *apm, u64 graph_cookie,
				       bool reset)
{
	struct q6apm_graph *graph, *victim;
	u32 generation;
	bool drop_command_ref;

	if (!apm)
		return;
	generation = q6apm_get_reset_generation(apm);

	for (;;) {
		victim = NULL;
		drop_command_ref = false;
		mutex_lock(&apm->lock);
		list_for_each_entry(graph, &apm->graph_clients, node) {
			bool eligible;

			mutex_lock(&graph->response_lock);
			eligible = graph->command_pending || graph->command_ambiguous;
			eligible = eligible &&
				(reset ? graph->pending_cmd_generation != generation :
				 graph_cookie &&
				 graph->pending_cmd_graph_cookie == graph_cookie);
			if (!eligible) {
				mutex_unlock(&graph->response_lock);
				continue;
			}
			if (WARN_ON_ONCE(!kref_get_unless_zero(&graph->refcount))) {
				mutex_unlock(&graph->response_lock);
				mutex_unlock(&apm->lock);
				return;
			}

			victim = graph;
			graph->command_pending = false;
			graph->command_ambiguous = false;
			graph->command_result = reset ? -ENETRESET : -ESHUTDOWN;
			if (graph->command_ref_held) {
				graph->command_ref_held = false;
				drop_command_ref = true;
			}
			wake_up_all(&graph->cmd_wait);
			mutex_unlock(&graph->response_lock);
			break;
		}
		mutex_unlock(&apm->lock);

		if (!victim)
			break;
		if (drop_command_ref)
			kref_put(&victim->refcount, q6apm_graph_release);
		kref_put(&victim->refcount, q6apm_graph_release);
	}
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

	return READ_ONCE(apm->state);
}

bool q6apm_is_adsp_ready(void)
{
	struct q6apm *apm = NULL;
	int state = -ENODEV;

	mutex_lock(&g_apm_lock);
	if (g_apm && kref_get_unless_zero(&g_apm->refcount))
		apm = g_apm;
	mutex_unlock(&g_apm_lock);
	if (apm) {
		state = q6apm_get_apm_state(apm);
		q6apm_put_apm(apm);
	}

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

	if (!graph || !cfg)
		return -EINVAL;
	if (cfg->direction != SNDRV_PCM_STREAM_PLAYBACK &&
	    cfg->direction != SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;

	if (cfg->direction == SNDRV_PCM_STREAM_CAPTURE)
		module = q6apm_find_module_by_mid(graph, MODULE_ID_RD_SHARED_MEM_EP);
	else
		module = q6apm_find_module_by_mid(graph, MODULE_ID_WR_SHARED_MEM_EP);

	if (!module)
		return -ENODEV;

	return audioreach_set_media_format(graph, module, cfg);

}
EXPORT_SYMBOL_GPL(q6apm_graph_media_format_shmem);

int q6apm_map_memory_fixed_region(struct q6apm *apm, unsigned int graph_id,
				  phys_addr_t phys, size_t sz,
				  unsigned int *lease_generation)
{
	struct q6apm_mem_lease *lease;
	struct apm_shared_map_region_payload *mregions;
	struct apm_cmd_shared_mem_map_regions *cmd;
	const size_t payload_size = sizeof(*cmd) + sizeof(*mregions);
	u64 aligned_sz;
	phys_addr_t end;
	u32 buf_sz;
	u32 generation;
	int ret;
	void *p;
	struct gpr_pkt *pkt __free(kfree) = NULL;

	if (!lease_generation)
		return -EINVAL;
	*lease_generation = 0;
	if (!apm || !kref_get_unless_zero(&apm->refcount))
		return -ENODEV;
	if (!sz || !IS_ALIGNED(phys, SZ_4K)) {
		ret = -EINVAL;
		goto put_map_apm;
	}
	if (check_add_overflow((u64)sz, (u64)SZ_4K - 1, &aligned_sz)) {
		ret = -EOVERFLOW;
		goto put_map_apm;
	}
	aligned_sz &= ~((u64)SZ_4K - 1);
	if (!aligned_sz || aligned_sz > U32_MAX) {
		ret = -EOVERFLOW;
		goto put_map_apm;
	}
	buf_sz = (u32)aligned_sz;
	if (check_add_overflow(phys, (phys_addr_t)buf_sz, &end) ||
	    end < phys) {
		ret = -EOVERFLOW;
		goto put_map_apm;
	}

	/*
	 * Serialize the online check with reset/offline transitions.  Without
	 * this, a stale mapped lease could be reused after the transport died.
	 */
	mutex_lock(&apm->online_lock);
	if (!READ_ONCE(apm->online)) {
		ret = -ENETRESET;
		goto unlock_map_online;
	}
	mutex_lock(&apm->lock);
	generation = atomic_read(&apm->reset_generation);
	lease = q6apm_get_lease_locked(apm, graph_id);
	if (!lease) {
		ret = -ENOMEM;
		goto unlock_map_lock;
	}

	switch (lease->state) {
	case AUDIOREACH_MEM_MAPPED:
		if (!lease->handle || lease->phys != phys ||
		    lease->size != buf_sz) {
			ret = -EBUSY;
			goto unlock_map_lock;
		}
		if (lease->users == UINT_MAX) {
			ret = -EOVERFLOW;
			goto unlock_map_lock;
		}
		lease->users++;
		*lease_generation = generation;
		ret = 0;
		goto unlock_map_lock;
	case AUDIOREACH_MEM_UNMAPPED:
		lease->phys = phys;
		lease->size = buf_sz;
		lease->generation = generation;
		lease->handle = 0;
		lease->users = 1;
		lease->state = AUDIOREACH_MEM_MAP_PENDING;
		kref_get(&lease->refcount);
		break;
	case AUDIOREACH_MEM_RESET_RETIRED:
		ret = -ENETRESET;
		goto unlock_map_lock;
	default:
		/*
		 * An uncertain transaction must be resolved by its late response
		 * or a trusted reset before host DMA can be reused.
		 */
		ret = -EBUSY;
		goto unlock_map_lock;
	}
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->online_lock);

	pkt = audioreach_alloc_apm_cmd_pkt(payload_size,
					   APM_CMD_SHARED_MEM_MAP_REGIONS,
					   graph_id);
	if (IS_ERR(pkt)) {
		ret = PTR_ERR(pkt);
		mutex_lock(&apm->lock);
		if (lease->generation == generation &&
		    (lease->state == AUDIOREACH_MEM_MAP_PENDING ||
		     lease->state == AUDIOREACH_MEM_MAP_UNCERTAIN))
			q6apm_lease_clear_locked(lease, AUDIOREACH_MEM_UNMAPPED);
		mutex_unlock(&apm->lock);
		kref_put(&lease->refcount, q6apm_mem_lease_release);
		goto put_map_apm;
	}

	p = (void *)pkt + GPR_HDR_SIZE;
	cmd = p;
	cmd->mem_pool_id = APM_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = 1;
	cmd->property_flag = 0x0;

	mregions = p + sizeof(*cmd);

	mregions->shm_addr_lsw = lower_32_bits(phys);
	mregions->shm_addr_msw = upper_32_bits(phys);
	mregions->mem_size_bytes = buf_sz;

	ret = __q6apm_send_cmd_sync(apm, pkt,
				    APM_CMD_RSP_SHARED_MEM_MAP_REGIONS,
				    Q6APM_CMD_MAP, lease, NULL, generation, 0);
	mutex_lock(&apm->lock);
	if (lease->generation == generation &&
	    generation != atomic_read(&apm->reset_generation)) {
		q6apm_lease_clear_locked(lease, AUDIOREACH_MEM_RESET_RETIRED);
		/* The caller cannot consume a handle retired before this return. */
		ret = -ENETRESET;
	} else if (ret == -ETIMEDOUT && lease->generation == generation &&
	    lease->state == AUDIOREACH_MEM_MAP_PENDING)
		lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
	else if (ret && ret != -ETIMEDOUT &&
		 ret != -ENETRESET &&
		 lease->generation == generation &&
		 lease->state == AUDIOREACH_MEM_MAP_PENDING)
		q6apm_lease_clear_locked(lease, AUDIOREACH_MEM_UNMAPPED);
	else if (!ret && lease->generation == generation &&
		 (lease->state != AUDIOREACH_MEM_MAPPED ||
			  !lease->handle)) {
		lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
		ret = -EPROTO;
	}
	if (lease->generation == generation &&
	    generation == atomic_read(&apm->reset_generation) &&
	    lease->state != AUDIOREACH_MEM_UNMAPPED &&
	    lease->state != AUDIOREACH_MEM_RESET_RETIRED)
		*lease_generation = generation;
	mutex_unlock(&apm->lock);
	kref_put(&lease->refcount, q6apm_mem_lease_release);

put_map_apm:
	kref_put(&apm->refcount, q6apm_release);
	return ret;

unlock_map_lock:
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->online_lock);
	goto put_map_apm;

unlock_map_online:
	mutex_unlock(&apm->online_lock);
	goto put_map_apm;
}
EXPORT_SYMBOL_GPL(q6apm_map_memory_fixed_region);

int q6apm_alloc_fragments(struct q6apm_graph *graph, unsigned int dir, phys_addr_t phys,
				size_t period_sz, unsigned int periods)
{
	struct audioreach_graph_data *data;
	struct audio_buffer *buf;
	u64 total_size;
	phys_addr_t end;
	int cnt;

	if (!graph || !periods || !period_sz || period_sz > U32_MAX)
		return -EINVAL;
	if (dir != SNDRV_PCM_STREAM_PLAYBACK &&
	    dir != SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;
	if (check_mul_overflow((u64)period_sz, (u64)periods, &total_size) ||
	    check_add_overflow(phys, (phys_addr_t)total_size, &end) ||
	    end < phys)
		return -EOVERFLOW;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		data = &graph->rx_data;
	else
		data = &graph->tx_data;

	mutex_lock(&graph->lock);

	if (data->buf) {
		mutex_unlock(&graph->lock);
		return 0;
	}

	data->dsp_buf = 0;

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
		buf[cnt].phys = buf[0].phys + (cnt * period_sz);
		buf[cnt].size = period_sz;
	}
	data->num_periods = periods;

	mutex_unlock(&graph->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_alloc_fragments);

int q6apm_unmap_memory_fixed_region(struct q6apm *apm,
				    unsigned int graph_id,
				    unsigned int expected_generation)
{
	struct q6apm_mem_lease *lease;
	struct apm_cmd_shared_mem_unmap_regions *cmd;
	u32 mem_map_handle;
	u32 generation;
	int ret;
	struct gpr_pkt *pkt __free(kfree) = NULL;

	if (!expected_generation)
		return -EINVAL;
	if (!apm || !kref_get_unless_zero(&apm->refcount))
		return -ENODEV;

	/* Serialize generation selection with reset and transport removal. */
	mutex_lock(&apm->online_lock);
	generation = atomic_read(&apm->reset_generation);
	if (expected_generation != generation) {
		/* A trusted reset retired this generation; do not touch its successor. */
		ret = 0;
		goto unlock_unmap_online;
	}
	mutex_lock(&apm->lock);
	lease = q6apm_find_lease_locked(apm, expected_generation, graph_id);
	if (!lease) {
		/* An absent lease for the exact generation is already retired. */
		ret = 0;
		goto unlock_unmap_lock;
	}

	switch (lease->state) {
	case AUDIOREACH_MEM_UNMAPPED:
	case AUDIOREACH_MEM_RESET_RETIRED:
		ret = 0;
		goto unlock_unmap_lock;
	default:
		break;
	}
	if (!READ_ONCE(apm->online)) {
		/* Ordinary removal is not proof that the DSP released this lease. */
		ret = -ENETRESET;
		goto unlock_unmap_lock;
	}

	switch (lease->state) {
	case AUDIOREACH_MEM_MAPPED:
		break;
	default:
		ret = -EBUSY;
		goto unlock_unmap_lock;
	}

	mem_map_handle = lease->handle;
	if (!mem_map_handle) {
		lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
		ret = -EBUSY;
		goto unlock_unmap_lock;
	}
	if (lease->users > 1) {
		lease->users--;
		ret = 0;
		goto unlock_unmap_lock;
	}
	if (!lease->users) {
		lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
		ret = -EBUSY;
		goto unlock_unmap_lock;
	}
	lease->state = AUDIOREACH_MEM_UNMAP_PENDING;
	kref_get(&lease->refcount);
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->online_lock);

	pkt = audioreach_alloc_apm_cmd_pkt(sizeof(*cmd),
					   APM_CMD_SHARED_MEM_UNMAP_REGIONS,
					   graph_id);
	if (IS_ERR(pkt)) {
		ret = PTR_ERR(pkt);
		mutex_lock(&apm->lock);
		if (expected_generation !=
		    atomic_read(&apm->reset_generation)) {
			if (lease->generation == expected_generation)
				q6apm_lease_clear_locked(lease,
							 AUDIOREACH_MEM_RESET_RETIRED);
			ret = 0;
		} else if (lease->generation == expected_generation &&
		    (lease->state == AUDIOREACH_MEM_UNMAP_PENDING ||
		     lease->state == AUDIOREACH_MEM_UNMAP_UNCERTAIN))
			lease->state = AUDIOREACH_MEM_MAPPED;
		mutex_unlock(&apm->lock);
		kref_put(&lease->refcount, q6apm_mem_lease_release);
		goto put_unmap_apm;
	}

	cmd = (void *)pkt + GPR_HDR_SIZE;
	cmd->mem_map_handle = mem_map_handle;

	ret = __q6apm_send_cmd_sync(apm, pkt, 0, Q6APM_CMD_UNMAP,
				    lease, NULL, expected_generation,
				    mem_map_handle);
	mutex_lock(&apm->lock);
	if (expected_generation != atomic_read(&apm->reset_generation)) {
		if (lease->generation == expected_generation)
			q6apm_lease_clear_locked(lease,
						 AUDIOREACH_MEM_RESET_RETIRED);
		ret = 0;
	} else if (ret == -ETIMEDOUT &&
		   lease->generation == expected_generation &&
		   lease->state == AUDIOREACH_MEM_UNMAP_PENDING)
		lease->state = AUDIOREACH_MEM_UNMAP_UNCERTAIN;
	else if (ret == -ENETRESET &&
		 lease->generation == expected_generation &&
		 lease->state == AUDIOREACH_MEM_UNMAP_PENDING)
		lease->state = AUDIOREACH_MEM_UNMAP_UNCERTAIN;
	else if (!ret && lease->generation == expected_generation &&
		 lease->state != AUDIOREACH_MEM_UNMAPPED) {
		lease->state = AUDIOREACH_MEM_UNMAP_UNCERTAIN;
		ret = -EPROTO;
	} else if (ret && ret != -ETIMEDOUT && ret != -ENETRESET &&
		   lease->generation == expected_generation &&
		   lease->state == AUDIOREACH_MEM_UNMAP_PENDING) {
		lease->state = AUDIOREACH_MEM_MAPPED;
	}
	mutex_unlock(&apm->lock);
	kref_put(&lease->refcount, q6apm_mem_lease_release);
	/* Retire the completed MAP transcript before a future same-lease MAP. */
	q6apm_retire_old_leases(apm);

put_unmap_apm:
	kref_put(&apm->refcount, q6apm_release);
	return ret;

unlock_unmap_lock:
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->online_lock);
	goto put_unmap_apm;

unlock_unmap_online:
	mutex_unlock(&apm->online_lock);
	goto put_unmap_apm;
}
EXPORT_SYMBOL_GPL(q6apm_unmap_memory_fixed_region);

static int q6apm_get_mem_map_handle_locked(struct q6apm *apm, u32 graph_id,
					   u32 *handle)
{
	struct q6apm_mem_lease *lease;
	u32 generation;
	int ret = 0;

	if (!apm || !handle)
		return -EINVAL;
	lockdep_assert_held(&apm->online_lock);

	*handle = 0;
	if (!READ_ONCE(apm->online)) {
		ret = -ENETRESET;
		return ret;
	}

	generation = atomic_read(&apm->reset_generation);
	mutex_lock(&apm->lock);
	lease = q6apm_find_lease_locked(apm, generation, graph_id);
	if (!lease)
		ret = -ENODEV;
	else if (lease->state != AUDIOREACH_MEM_MAPPED ||
		 lease->users == 0 || !lease->handle)
		ret = -EAGAIN;
	else
		*handle = lease->handle;
	mutex_unlock(&apm->lock);

	return ret;
}

static bool q6apm_data_mem_handle_valid(struct q6apm_graph *graph, u32 handle)
{
	struct q6apm_mem_lease *lease;
	struct q6apm *apm;
	u32 generation;
	bool valid = false;

	if (!graph || !handle)
		return false;
	apm = graph->apm;
	if (!apm)
		return false;

	mutex_lock(&apm->online_lock);
	if (!READ_ONCE(apm->online))
		goto unlock_online;
	generation = atomic_read(&apm->reset_generation);
	mutex_lock(&apm->lock);
	lease = q6apm_find_lease_locked(apm, generation, graph->id);
	valid = lease && lease->state == AUDIOREACH_MEM_MAPPED &&
		lease->users && lease->handle == handle;
	mutex_unlock(&apm->lock);

unlock_online:
	mutex_unlock(&apm->online_lock);
	return valid;
}

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
	struct audioreach_graph_info *info;
	struct audioreach_sub_graph *sgs;
	struct audioreach_container *container;
	struct audioreach_module *module;
	int ret;

	if (!graph || !cfg || !graph->info)
		return -EINVAL;
	info = graph->info;

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

	if (!graph)
		return -EINVAL;

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
	struct q6apm *apm = graph->apm;
	bool put_apm = graph->apm_ref;

	WARN_ON(graph->port);
	WARN_ON(graph->ar_graph);
	WARN_ON(graph->command_ref_held);
	if (graph->listed) {
		mutex_lock(&apm->lock);
		if (graph->listed) {
			list_del_init(&graph->node);
			graph->listed = false;
		}
		mutex_unlock(&apm->lock);
	}
	graph->apm_ref = false;
	kfree(graph);
	if (put_apm)
		q6apm_put_apm(apm);
}

static void q6apm_graph_finish_close(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	bool queue_release = false;

	mutex_lock(&graph->lock);
	ar_graph = READ_ONCE(graph->ar_graph);
	WRITE_ONCE(graph->ar_graph, NULL);
	mutex_unlock(&graph->lock);

	if (ar_graph) {
		mutex_lock(&ar_graph->state_lock);
		if (ar_graph->clients)
			ar_graph->clients--;
		else
			WARN_ON_ONCE(1);
		queue_release = !ar_graph->clients;
		mutex_unlock(&ar_graph->state_lock);
		if (queue_release)
			q6apm_queue_graph_release(ar_graph);
		q6apm_graph_put_ref(ar_graph);
	}
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
	/* q6apm_graph_close() takes this work reference before scheduling. */
	kref_put(&graph->refcount, q6apm_graph_release);
}

static void q6apm_graph_call_client(struct q6apm_graph *graph,
					  uint32_t opcode, uint32_t token,
					  uint32_t bytes, void *payload)
{
	q6apm_cb cb;
	void *priv;

	lockdep_assert_held(&graph->callback_lock);

	mutex_lock(&graph->lifecycle_lock);
	if (READ_ONCE(graph->closing) || !graph->cb) {
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
	gpr_port_t *port;
	struct gpr_pkt *pkt;
	uint32_t token;
	u32 dsp_buf;
	u32 mem_map_handle;
	struct q6apm *apm;
	int ret;

	if (!graph)
		return -EINVAL;
	apm = graph->apm;
	if (!apm)
		return -ENODEV;
	mutex_lock(&graph->lock);
	if (graph->flushing) {
		ret = -EBUSY;
		goto unlock;
	}
	if (READ_ONCE(graph->closing) || !graph->ar_graph || !graph->port || !graph->info) {
		ret = -ENODEV;
		goto unlock;
	}
	port = graph->port;
	if (!graph->rx_data.buf || !graph->rx_data.num_periods ||
	    graph->rx_data.dsp_buf >= graph->rx_data.num_periods) {
		ret = -ENODATA;
		goto unlock;
	}
	/* Keep reset/offline transitions out of the handle-to-send window. */
	mutex_lock(&apm->online_lock);
	ret = q6apm_get_mem_map_handle_locked(apm, graph->id, &mem_map_handle);
	if (ret)
		goto unlock_online;

	dsp_buf = graph->rx_data.dsp_buf;
	ab = &graph->rx_data.buf[dsp_buf];
	if (ab->in_flight) {
		ret = -EBUSY;
		goto unlock_online;
	}
	if (!len || len > ab->size) {
		ret = -EINVAL;
		goto unlock_online;
	}
	token = q6apm_next_token();

	pkt = audioreach_alloc_pkt(sizeof(*write_buffer),
				   DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2,
				   token, port->id, graph->shm_iid);
	if (IS_ERR(pkt)) {
		ret = PTR_ERR(pkt);
		goto unlock_online;
	}
	write_buffer = (void *)pkt + GPR_HDR_SIZE;

	write_buffer->buf_addr_lsw = lower_32_bits(ab->phys);
	write_buffer->buf_addr_msw = upper_32_bits(ab->phys);
	write_buffer->buf_size = len;
	write_buffer->timestamp_lsw = lsw_ts;
	write_buffer->timestamp_msw = msw_ts;
	write_buffer->mem_map_handle = mem_map_handle;
	write_buffer->flags = wflags;

	/* Publish ownership before send so an immediate response can be matched. */
	ab->submitted = len;
	ab->token = token;
	ab->in_flight = true;
	graph->rx_data.dsp_buf++;
	if (graph->rx_data.dsp_buf >= graph->rx_data.num_periods)
		graph->rx_data.dsp_buf = 0;

	ret = gpr_send_port_pkt(port, pkt);
	kfree(pkt);
	mutex_unlock(&apm->online_lock);
	if (ret < 0) {
		if (ab->in_flight && ab->token == token) {
			ab->submitted = 0;
			ab->token = 0;
			ab->in_flight = false;
			graph->rx_data.dsp_buf = dsp_buf;
		}
		goto unlock;
	}
	ret = 0;

unlock:
	mutex_unlock(&graph->lock);
	return ret;

unlock_online:
	mutex_unlock(&apm->online_lock);
	goto unlock;
}
EXPORT_SYMBOL_GPL(q6apm_write_async);

int q6apm_read(struct q6apm_graph *graph)
{
	struct data_cmd_rd_sh_mem_ep_data_buffer_v2 *read_buffer;
	struct audioreach_graph_data *port;
	gpr_port_t *gpr_port;
	struct audio_buffer *ab;

	struct gpr_pkt *pkt;
	uint32_t token;
	u32 dsp_buf;
	u32 mem_map_handle;
	struct q6apm *apm;
	int ret;

	if (!graph)
		return -EINVAL;
	apm = graph->apm;
	if (!apm)
		return -ENODEV;
	mutex_lock(&graph->lock);
	port = &graph->tx_data;
	if (graph->flushing) {
		ret = -EBUSY;
		goto unlock;
	}
	if (READ_ONCE(graph->closing) || !graph->ar_graph || !graph->port || !graph->info) {
		ret = -ENODEV;
		goto unlock;
	}
	gpr_port = graph->port;
	if (!port->buf || !port->num_periods ||
	    port->dsp_buf >= port->num_periods) {
		ret = -ENODATA;
		goto unlock;
	}
	/* Keep reset/offline transitions out of the handle-to-send window. */
	mutex_lock(&apm->online_lock);
	ret = q6apm_get_mem_map_handle_locked(apm, graph->id, &mem_map_handle);
	if (ret)
		goto unlock_online;
	dsp_buf = port->dsp_buf;
	ab = &port->buf[dsp_buf];
	if (ab->in_flight) {
		ret = -EBUSY;
		goto unlock_online;
	}
	token = q6apm_next_token();
	pkt = audioreach_alloc_pkt(sizeof(*read_buffer),
				   DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2,
				   token, gpr_port->id, graph->shm_iid);
	if (IS_ERR(pkt)) {
		ret = PTR_ERR(pkt);
		goto unlock_online;
	}
	read_buffer = (void *)pkt + GPR_HDR_SIZE;

	read_buffer->buf_addr_lsw = lower_32_bits(ab->phys);
	read_buffer->buf_addr_msw = upper_32_bits(ab->phys);
	read_buffer->mem_map_handle = mem_map_handle;
	read_buffer->buf_size = ab->size;

	/* Publish ownership before send so an immediate response can be matched. */
	ab->submitted = ab->size;
	ab->token = token;
	ab->in_flight = true;
	port->dsp_buf++;
	if (port->dsp_buf >= port->num_periods)
		port->dsp_buf = 0;

	ret = gpr_send_port_pkt(gpr_port, pkt);
	kfree(pkt);
	mutex_unlock(&apm->online_lock);
	if (ret < 0) {
		if (ab->in_flight && ab->token == token) {
			ab->submitted = 0;
			ab->token = 0;
			ab->in_flight = false;
			port->dsp_buf = dsp_buf;
		}
		goto unlock;
	}
	ret = 0;

unlock:
	mutex_unlock(&graph->lock);
	return ret;

unlock_online:
	mutex_unlock(&apm->online_lock);
	goto unlock;
}
EXPORT_SYMBOL_GPL(q6apm_read);

int q6apm_get_hw_pointer(struct q6apm_graph *graph, int dir)
{
	struct audioreach_graph_data *data;

	if (!graph ||
	    (dir != SNDRV_PCM_STREAM_PLAYBACK &&
	     dir != SNDRV_PCM_STREAM_CAPTURE))
		return -EINVAL;

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
	const struct gpr_hdr *hdr;
	struct device *dev;
	uint32_t client_event;
	uint32_t bytes = 0;
	phys_addr_t phys;
	bool drop_command_ref = false;
	bool failed;
	int buffer;

	if (!data || !graph)
		return -EINVAL;
	if (!kref_get_unless_zero(&graph->refcount))
		return 0;

	(void)op;
	hdr = &data->hdr;
	dev = graph->dev;
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
		if (!data->payload || data->payload_size < sizeof(*done)) {
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
		if (!q6apm_data_mem_handle_valid(graph, done->mem_map_handle)) {
			dev_err(dev, "WR BUFF Unexpected map handle %08x\n",
				done->mem_map_handle);
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
		if (!data->payload || data->payload_size < sizeof(*rd_done)) {
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
		if (!q6apm_data_mem_handle_valid(graph, rd_done->mem_map_handle)) {
			dev_err(dev, "RD BUFF Unexpected map handle %08x\n",
				rd_done->mem_map_handle);
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
		if ((!graph->command_pending && !graph->command_ambiguous) ||
		    hdr->token != graph->pending_cmd_token) {
			mutex_unlock(&graph->response_lock);
			dev_dbg(dev,
				"ignoring stale graph response 0x%08x token %u\n",
				hdr->opcode, hdr->token);
			break;
		}
		/*
		 * The port callback has no transport-generation argument.  Read the
		 * current APM generation while the authenticated slot is locked and
		 * retire a stale slot only at that trusted boundary.
		 */
		if (!graph->apm ||
		    graph->pending_cmd_generation !=
			q6apm_get_reset_generation(graph->apm) ||
		    graph->pending_cmd_graph_cookie != graph->graph_cookie) {
			graph->command_pending = false;
			graph->command_ambiguous = false;
			graph->command_result = -ENETRESET;
			if (graph->command_ref_held) {
				graph->command_ref_held = false;
				drop_command_ref = true;
			}
			wake_up_all(&graph->cmd_wait);
			mutex_unlock(&graph->response_lock);
			dev_dbg(dev, "retiring graph response from an old generation\n");
			break;
		}
		if (!data->payload || data->payload_size < sizeof(*result)) {
			dev_err(dev, "short graph basic response: %d bytes\n",
				data->payload_size);
			/*
			 * A malformed response does not prove that the DSP rejected the
			 * command.  Wake the waiter with an ambiguous result, but retain
			 * the token so a matching late response can settle ownership.
			 */
			graph->command_pending = false;
			graph->command_ambiguous = true;
			graph->command_result = -EPROTO;
			wake_up_all(&graph->cmd_wait);
			mutex_unlock(&graph->response_lock);
			break;
		}
		if (result->opcode != graph->pending_cmd_opcode) {
			dev_err(dev,
				"graph response opcode mismatch: expected 0x%08x got 0x%08x\n",
				graph->pending_cmd_opcode, result->opcode);
			graph->command_pending = false;
			graph->command_ambiguous = true;
			graph->command_result = -EPROTO;
			wake_up_all(&graph->cmd_wait);
			mutex_unlock(&graph->response_lock);
			break;
		}
		/*
		 * Only the command recorded in the slot may consume it.  Publish one
		 * terminal result and wake both an active waiter and any close/retry
		 * worker.
		 */
		graph->command_result = result->status ? -EINVAL : 0;
		graph->command_pending = false;
		graph->command_ambiguous = false;
		if (graph->command_ref_held) {
			graph->command_ref_held = false;
			drop_command_ref = true;
		}
		if (result->status)
			dev_err(dev, "Error (%d) Processing 0x%08x cmd\n",
				result->status, result->opcode);
		wake_up_all(&graph->cmd_wait);
		mutex_unlock(&graph->response_lock);
		break;
	default:
		break;
	}
	if (drop_command_ref)
		kref_put(&graph->refcount, q6apm_graph_release);
	kref_put(&graph->refcount, q6apm_graph_release);
	return 0;
}

struct q6apm_graph *q6apm_graph_open(struct device *dev, q6apm_cb cb,
				     void *priv, int graph_id, int dir)
{
	struct q6apm *apm;
	struct audioreach_graph *ar_graph;
	struct q6apm_graph *graph;
	int ret;

	if (!dev || !dev->parent || graph_id < 0 ||
	    (dir != SNDRV_PCM_STREAM_PLAYBACK &&
	     dir != SNDRV_PCM_STREAM_CAPTURE))
		return ERR_PTR(-EINVAL);
	/*
	 * Pair the lookup with the APM's global registry reference.  A direct
	 * drvdata read can race apm_remove() clearing and releasing the object.
	 */
	apm = q6apm_get_apm(dev);
	if (!apm)
		return ERR_PTR(-ENODEV);

	ar_graph = q6apm_get_audioreach_graph(apm, graph_id);
	if (IS_ERR(ar_graph)) {
		dev_err(dev, "No graph found with id %d\n", graph_id);
		kref_put(&apm->refcount, q6apm_release);
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
	graph->graph_cookie = ar_graph->instance_cookie;
	graph->dev = dev;
	INIT_LIST_HEAD(&graph->node);
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

	/*
	 * Publish the wrapper and transfer the lookup's APM reference atomically
	 * with reset/offline.  The registry keeps timed-out port commands reachable
	 * after close has detached the cached graph.
	 */
	mutex_lock(&apm->online_lock);
	if (!READ_ONCE(apm->online)) {
		ret = -ENETRESET;
		goto unlock_open_online;
	}
	graph->port = gpr_alloc_port(apm->gdev, dev, graph_callback, graph);
	if (IS_ERR(graph->port)) {
		ret = PTR_ERR(graph->port);
		graph->port = NULL;
		goto unlock_open_online;
	}
	mutex_lock(&apm->lock);
	mutex_lock(&ar_graph->state_lock);
	if (idr_find(&apm->graph_idr, ar_graph->id) != ar_graph ||
	    ar_graph->destroying || ar_graph->graph_quarantined ||
	    ar_graph->graph_reset_retired)
		ret = -EBUSY;
	else
		ret = 0;
	if (!ret) {
		ar_graph->clients++;
		graph->apm_ref = true;
		graph->listed = true;
		list_add_tail(&graph->node, &apm->graph_clients);
	}
	mutex_unlock(&ar_graph->state_lock);
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->online_lock);
	if (ret) {
		gpr_free_port(graph->port);
		graph->port = NULL;
		goto free_graph;
	}

	return graph;

unlock_open_online:
	mutex_unlock(&apm->online_lock);
	goto free_graph;

free_graph:
	kfree(graph);
put_ar_graph:
	q6apm_graph_put_ref(ar_graph);
	kref_put(&apm->refcount, q6apm_release);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6apm_graph_open);

int q6apm_graph_close(struct q6apm_graph *graph)
{
	gpr_port_t *port;
	bool defer_close;

	if (!graph)
		return -EINVAL;

	mutex_lock(&graph->lifecycle_lock);
	if (READ_ONCE(graph->closing)) {
		mutex_unlock(&graph->lifecycle_lock);
		return 0;
	}
	WRITE_ONCE(graph->closing, true);
	defer_close = graph->callback_task == current;
	graph->cb = NULL;
	graph->priv = NULL;
	mutex_unlock(&graph->lifecycle_lock);

	/*
	 * Callbacks take callback_lock before graph->lock.  Take the same locks
	 * while detaching the port so a sender cannot race gpr_free_port().  A
	 * callback-originated close already owns callback_lock and must avoid a
	 * recursive mutex acquisition.
	 */
	if (!defer_close)
		mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	port = graph->port;
	graph->port = NULL;
	mutex_unlock(&graph->lock);
	if (!defer_close)
		mutex_unlock(&graph->callback_lock);

	if (port)
		gpr_free_port(port);

	if (defer_close) {
		/*
		 * The caller may drop its final handle reference as soon as this
		 * callback returns.  Keep the close worker's graph storage alive.
		 */
		kref_get(&graph->refcount);
		if (WARN_ON_ONCE(!schedule_work(&graph->close_work)))
			kref_put(&graph->refcount, q6apm_graph_release);
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
	struct audioreach_graph *ar_graph;
	int ret;

	if (!graph)
		return -EINVAL;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return -ENODEV;

	ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_PREPARE);
	q6apm_graph_put_ref(ar_graph);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_prepare);

int q6apm_graph_start(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	bool send = false;
	int ret;

	if (!graph)
		return -EINVAL;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return -ENODEV;

	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->operation_pending || ar_graph->flush_pending) {
		ret = -EBUSY;
	} else if (ar_graph->start_count == 0) {
		ar_graph->operation_pending = true;
		ar_graph->pending_opcode = APM_CMD_GRAPH_START;
		ar_graph->pending_flush = false;
		send = true;
		ret = 0;
	} else if (ar_graph->start_count == INT_MAX) {
		ret = -EOVERFLOW;
	} else {
		ar_graph->start_count++;
		ret = 0;
	}
	mutex_unlock(&ar_graph->state_lock);
	if (!send)
		goto put_ar_graph;

	ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_START);
	if (!q6apm_graph_result_ambiguous(ret)) {
		mutex_lock(&ar_graph->state_lock);
		if (ar_graph->operation_pending &&
		    ar_graph->pending_opcode == APM_CMD_GRAPH_START)
			ar_graph->operation_pending = false;
		mutex_unlock(&ar_graph->state_lock);
	}

put_ar_graph:
	q6apm_graph_put_ref(ar_graph);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_start);

int q6apm_graph_rearm(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	int ret;

	if (!graph)
		return -EINVAL;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return -ENODEV;

	/*
	 * A full compressed drain leaves the shared enrollment in place while
	 * the DSP graph needs an explicit START before accepting the next track.
	 */
	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->operation_pending) {
		ret = -EBUSY;
	} else if (ar_graph->flush_pending) {
		ret = -EBUSY;
	} else if (ar_graph->start_count <= 0) {
		ret = -EINVAL;
	} else {
		ar_graph->operation_pending = true;
		ar_graph->pending_opcode = APM_CMD_GRAPH_START;
		ar_graph->pending_flush = false;
		ret = 0;
	}
	mutex_unlock(&ar_graph->state_lock);
	if (ret)
		goto put_ar_graph;

	ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_START);
	if (!q6apm_graph_result_ambiguous(ret)) {
		mutex_lock(&ar_graph->state_lock);
		if (ar_graph->operation_pending &&
		    ar_graph->pending_opcode == APM_CMD_GRAPH_START)
			ar_graph->operation_pending = false;
		mutex_unlock(&ar_graph->state_lock);
	}

put_ar_graph:
	q6apm_graph_put_ref(ar_graph);
	return ret;
}
EXPORT_SYMBOL_GPL(q6apm_graph_rearm);

static int __q6apm_graph_stop(struct q6apm_graph *graph, bool request_flush,
				      bool *last)
{
	struct audioreach_graph *ar_graph;
	bool send = false;
	int ret = 0;

	if (!graph)
		return -EINVAL;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return -ENODEV;

	if (last)
		*last = false;

	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->operation_pending) {
		ret = -EBUSY;
		goto unlock;
	}
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

	ar_graph->operation_pending = true;
	ar_graph->pending_opcode = APM_CMD_GRAPH_STOP;
	ar_graph->pending_flush = request_flush;
	send = true;

unlock:
	mutex_unlock(&ar_graph->state_lock);
	if (!send)
		goto put_ar_graph;

	ret = audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_STOP);
	if (!q6apm_graph_result_ambiguous(ret)) {
		mutex_lock(&ar_graph->state_lock);
		if (ar_graph->operation_pending &&
		    ar_graph->pending_opcode == APM_CMD_GRAPH_STOP)
			ar_graph->operation_pending = false;
		mutex_unlock(&ar_graph->state_lock);
	}
	if (!ret && last)
		*last = true;

put_ar_graph:
	q6apm_graph_put_ref(ar_graph);
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
	struct audioreach_graph *ar_graph;
	bool ready = false;

	if (!graph)
		return false;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return false;

	mutex_lock(&ar_graph->state_lock);
	/*
	 * Do not queue a flush behind an unresolved graph operation.  In
	 * particular, a START response may otherwise leave flush_pending set
	 * while the graph is running, making the shared state impossible to
	 * drain or re-arm.
	 */
	if (!ar_graph->start_count && !ar_graph->operation_pending) {
		ar_graph->flush_pending = true;
		ready = true;
	}
	mutex_unlock(&ar_graph->state_lock);
	q6apm_graph_put_ref(ar_graph);

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
	if (!graph)
		return;
	mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	__q6apm_graph_reset_client(graph);
	mutex_unlock(&graph->lock);
	mutex_unlock(&graph->callback_lock);
}
EXPORT_SYMBOL_GPL(q6apm_graph_reset_client);

void q6apm_graph_quiesce_callbacks(struct q6apm_graph *graph)
{
	if (!graph)
		return;
	mutex_lock(&graph->callback_lock);
	mutex_unlock(&graph->callback_lock);
}
EXPORT_SYMBOL_GPL(q6apm_graph_quiesce_callbacks);

bool q6apm_graph_try_reset_client(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	bool reset = false;
	bool flush_pending;

	if (!graph)
		return false;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return false;

	/*
	 * Keep the shared state lock out of the callback-lock acquisition.  Data
	 * callbacks validate their lease while holding callback_lock and may need
	 * the APM lock; reset/offline takes that lock before state_lock.
	 */
	mutex_lock(&ar_graph->state_lock);
	flush_pending = ar_graph->flush_pending;
	mutex_unlock(&ar_graph->state_lock);
	if (flush_pending)
		goto put_ar_graph;

	mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	mutex_lock(&ar_graph->state_lock);
	if (!ar_graph->flush_pending && q6apm_graph_client_idle_locked(graph)) {
		__q6apm_graph_reset_client(graph);
		reset = true;
	}
	mutex_unlock(&ar_graph->state_lock);
	mutex_unlock(&graph->lock);
	mutex_unlock(&graph->callback_lock);

put_ar_graph:
	q6apm_graph_put_ref(ar_graph);
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
	if (!graph)
		return -EINVAL;
	if (!wait_event_timeout(graph->data_wait,
				q6apm_graph_client_idle(graph), timeout))
		return -ETIMEDOUT;

	return 0;
}
EXPORT_SYMBOL_GPL(q6apm_graph_wait_for_client_idle);

bool q6apm_graph_sync_flush_generation(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	u32 generation;
	bool changed = false;

	if (!graph)
		return false;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return false;

	mutex_lock(&ar_graph->state_lock);
	generation = ar_graph->flush_generation;
	mutex_unlock(&ar_graph->state_lock);
	if (READ_ONCE(graph->flush_generation) != generation) {
		/* Match flush/reset ordering: callback_lock -> graph->lock -> state_lock. */
		mutex_lock(&graph->callback_lock);
		mutex_lock(&graph->lock);
		mutex_lock(&ar_graph->state_lock);
		if (graph->flush_generation != ar_graph->flush_generation) {
			__q6apm_graph_reset_client(graph);
			graph->flush_generation = ar_graph->flush_generation;
			changed = true;
		}
		mutex_unlock(&ar_graph->state_lock);
		mutex_unlock(&graph->lock);
		mutex_unlock(&graph->callback_lock);
	}
	q6apm_graph_put_ref(ar_graph);

	return changed;
}
EXPORT_SYMBOL_GPL(q6apm_graph_sync_flush_generation);

bool q6apm_graph_flush_pending(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	bool pending;

	if (!graph)
		return false;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return false;

	mutex_lock(&ar_graph->state_lock);
	pending = ar_graph->flush_pending;
	mutex_unlock(&ar_graph->state_lock);
	q6apm_graph_put_ref(ar_graph);

	return pending;
}
EXPORT_SYMBOL_GPL(q6apm_graph_flush_pending);

int q6apm_graph_flush(struct q6apm_graph *graph)
{
	struct audioreach_graph *ar_graph;
	int ret;
	bool send = false;

	if (!graph)
		return -EINVAL;
	ar_graph = q6apm_graph_get_ar_graph(graph);
	if (!ar_graph)
		return -ENODEV;

	/*
	 * Publish the barrier before inspecting the shared graph state so a
	 * concurrent data submit cannot slip through while FLUSH is armed.
	 */
	mutex_lock(&graph->lock);
	if (READ_ONCE(graph->closing) || !graph->ar_graph) {
		mutex_unlock(&graph->lock);
		ret = -ENODEV;
		goto put_ar_graph;
	}
	graph->flushing = true;
	mutex_unlock(&graph->lock);

	mutex_lock(&ar_graph->state_lock);
	if (ar_graph->start_count) {
		mutex_unlock(&ar_graph->state_lock);
		ret = -EINVAL;
		goto clear_flushing;
	}
	if (!ar_graph->flush_pending) {
		mutex_unlock(&ar_graph->state_lock);
		ret = 0;
		goto clear_flushing;
	}
	if (ar_graph->operation_pending) {
		mutex_unlock(&ar_graph->state_lock);
		ret = -EBUSY;
		goto clear_flushing;
	}
	ar_graph->operation_pending = true;
	ar_graph->pending_opcode = APM_CMD_GRAPH_FLUSH;
	ar_graph->pending_flush = false;
	send = true;
	mutex_unlock(&ar_graph->state_lock);

	ret = send ? audioreach_graph_mgmt_cmd(ar_graph, APM_CMD_GRAPH_FLUSH) : 0;
	if (!q6apm_graph_result_ambiguous(ret)) {
		mutex_lock(&ar_graph->state_lock);
		if (ar_graph->operation_pending &&
		    ar_graph->pending_opcode == APM_CMD_GRAPH_FLUSH)
			ar_graph->operation_pending = false;
		mutex_unlock(&ar_graph->state_lock);
	}
	if (ret) {
		mutex_lock(&graph->callback_lock);
		mutex_lock(&graph->lock);
		graph->flushing = false;
		mutex_unlock(&graph->lock);
		mutex_unlock(&graph->callback_lock);
		goto put_ar_graph;
	}
	mutex_lock(&graph->callback_lock);
	mutex_lock(&graph->lock);
	__q6apm_graph_reset_client(graph);
	mutex_unlock(&graph->lock);
	mutex_unlock(&graph->callback_lock);
	mutex_lock(&ar_graph->state_lock);
	graph->flush_generation = ar_graph->flush_generation;
	mutex_unlock(&ar_graph->state_lock);

clear_flushing:
	mutex_lock(&graph->lock);
	graph->flushing = false;
	mutex_unlock(&graph->lock);

put_ar_graph:
	q6apm_graph_put_ref(ar_graph);
	return ret;
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

static void q6apm_mark_offline_locked(struct q6apm *apm, bool final_reset)
{
	struct q6apm_cmd_ticket *ticket, *ticket_tmp;
	struct q6apm_mem_lease *lease;
	struct audioreach_graph *graph;
	LIST_HEAD(retired_tickets);
	int id;

	lockdep_assert_held(&apm->online_lock);
	if (!READ_ONCE(apm->online) && !final_reset)
		return;

	WRITE_ONCE(apm->online, false);
	if (final_reset)
		atomic_inc(&apm->reset_generation);
	/* Wake openers waiting for a graph instance so they can observe offline. */
	wake_up_all(&apm->graph_wait);

	mutex_lock(&apm->response_lock);
	mutex_lock(&apm->lock);
	list_for_each_entry(lease, &apm->mem_leases, node) {
		if (final_reset) {
			lease->handle = 0;
			lease->users = 0;
			lease->state = AUDIOREACH_MEM_RESET_RETIRED;
		} else if (lease->state == AUDIOREACH_MEM_MAP_PENDING ||
			   lease->state == AUDIOREACH_MEM_UNMAP_PENDING) {
			lease->state = lease->state == AUDIOREACH_MEM_MAP_PENDING ?
				AUDIOREACH_MEM_MAP_UNCERTAIN :
				AUDIOREACH_MEM_UNMAP_UNCERTAIN;
		}
	}
	q6apm_clear_graph_operations_locked(apm);
	idr_for_each_entry(&apm->graph_idr, graph, id) {
		mutex_lock(&graph->state_lock);
		graph->graph_quarantined = true;
		if (final_reset) {
			graph->graph_open_ambiguous = false;
			graph->graph_close_confirmed = true;
			graph->graph_reset_retired = true;
		}
		mutex_unlock(&graph->state_lock);
	}

	/*
	 * APR driver removal is not proof that the remote discarded DMA mappings.
	 * Retire command authentication here, while preserving the lease state on
	 * an ordinary unbind.  A final transport/PDR reset invalidates everything.
	 */
	list_for_each_entry_safe(ticket, ticket_tmp, &apm->cmd_tickets, node) {
		if (ticket->lease && ticket->lease->map_ticket == ticket)
			ticket->lease->map_ticket = NULL;
		if (ticket->lease) {
			if (ticket->lease->generation == ticket->generation &&
			    !final_reset &&
			    ticket->kind == Q6APM_CMD_MAP &&
			    ticket->lease->state == AUDIOREACH_MEM_MAP_PENDING)
				ticket->lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
			else if (ticket->lease->generation == ticket->generation &&
				 !final_reset &&
				 ticket->kind == Q6APM_CMD_UNMAP &&
				 ticket->lease->state == AUDIOREACH_MEM_UNMAP_PENDING)
				ticket->lease->state = AUDIOREACH_MEM_UNMAP_UNCERTAIN;
		}
		ticket->waiter_active = false;
		ticket->state = final_reset ? Q6APM_CMD_RESET_RETIRED :
			Q6APM_CMD_ORPHANED;
		ticket->result = final_reset ? -ENETRESET : -ENODEV;
		complete_all(&ticket->done);
		/* Keep the registry reference alive until response_lock is released. */
		list_move_tail(&ticket->node, &retired_tickets);
		ticket->listed = false;
	}
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->response_lock);

	list_for_each_entry_safe(ticket, ticket_tmp, &retired_tickets, node) {
		list_del_init(&ticket->node);
		kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
	}

	if (!final_reset)
		return;

	/*
	 * A trusted reset is the only point at which a graph-port command may be
	 * retired without a response.
	 */
	q6apm_retire_port_commands(apm, 0, true);

	/*
	 * Graphs with no remaining client handles can now be destroyed without a
	 * DSP CLOSE; active clients keep the cache until their normal close path.
	 */
	mutex_lock(&apm->lock);
	idr_for_each_entry(&apm->graph_idr, graph, id) {
		bool no_clients;

		mutex_lock(&graph->state_lock);
		no_clients = !graph->clients;
		mutex_unlock(&graph->state_lock);
		if (no_clients)
			q6apm_queue_graph_release(graph);
	}
	mutex_unlock(&apm->lock);

	/*
	 * Ticket references have been dropped above, so stale leases can now be
	 * removed without making a late authenticated response dereference them.
	 */
	q6apm_retire_old_leases(apm);
}

static bool q6apm_unbind_quiescent(struct q6apm *apm)
{
	bool quiescent;

	lockdep_assert_held(&apm->online_lock);
	mutex_lock(&apm->response_lock);
	mutex_lock(&apm->lock);
	quiescent = list_empty(&apm->cmd_tickets) &&
		list_empty(&apm->mem_leases) &&
		list_empty(&apm->graph_clients) &&
		idr_is_empty(&apm->graph_idr) &&
		!atomic_read(&apm->active_graphs);
	mutex_unlock(&apm->lock);
	mutex_unlock(&apm->response_lock);

	return quiescent;
}

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
	INIT_LIST_HEAD(&apm->cmd_tickets);
	INIT_LIST_HEAD(&apm->mem_leases);
	INIT_LIST_HEAD(&apm->graph_clients);
	init_rwsem(&apm->graph_lock);
	init_waitqueue_head(&apm->graph_wait);
	atomic_set(&apm->active_graphs, 0);
	atomic_set(&apm->graph_generation, 0);
	atomic_set(&apm->reset_generation, 1);
	atomic64_set(&apm->graph_cookie_seq, 0);
	apm->dev = dev;
	apm->gdev = gdev;
	apm->online = true;

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
	q6apm_mark_offline_locked(apm, true);
	mutex_unlock(&apm->online_lock);
	mutex_unlock(&g_apm_lock);
	dev_set_drvdata(dev, NULL);
	kref_put(&apm->refcount, q6apm_release);
	return ret;
}

static void apm_remove(gpr_device_t *gdev)
{
	struct q6apm *apm = dev_get_drvdata(&gdev->dev);
	unsigned long flags;
	bool final_reset;

	spin_lock_irqsave(&gdev->svc.lock, flags);
	final_reset = !gdev->svc.pr;
	spin_unlock_irqrestore(&gdev->svc.lock, flags);

	mutex_lock(&g_apm_lock);
	if (g_apm == apm)
		g_apm = NULL;
	mutex_lock(&apm->online_lock);
	q6apm_mark_offline_locked(apm, final_reset);
	if (!final_reset) {
		/* Manual bind/unbind is suppressed below.  A normal module removal is
		 * therefore reachable only after the PCM/compressed module pins and all
		 * DSP ownership have drained.  Keep a forced in-kernel unbind fail-closed:
		 * retire confirmed terminal state, but never manufacture a DSP reset.
		 */
		q6apm_retire_old_leases(apm);
		if (!q6apm_unbind_quiescent(apm))
			dev_warn(apm->dev,
				 "forced APM unbind retained live DSP resources\n");
	}
	mutex_unlock(&apm->online_lock);
	mutex_unlock(&g_apm_lock);

	of_platform_depopulate(&gdev->dev);
	snd_soc_unregister_component(&gdev->dev);
	dev_set_drvdata(&gdev->dev, NULL);
	kref_put(&apm->refcount, q6apm_release);
}

struct audioreach_module *q6apm_find_module_by_mid(struct q6apm_graph *graph, uint32_t mid)
{
	struct audioreach_graph_info *info;
	struct q6apm *apm;

	if (!graph || !graph->info || !graph->apm)
		return NULL;
	info = graph->info;
	apm = graph->apm;

	return __q6apm_find_module_by_mid(apm, info, mid);

}

static int apm_callback(const struct gpr_resp_pkt *data, void *priv, int op)
{
	gpr_device_t *gdev = priv;
	struct q6apm *apm;
	struct q6apm_cmd_ticket *ticket;
	const struct gpr_hdr *hdr;
	const struct gpr_ibasic_rsp_result_t *result;
	const struct apm_cmd_rsp_shared_mem_map_regions *map_rsp;
	struct device *dev;
	struct audioreach_graph *graph_ref = NULL;
	bool malformed = false;
	bool completed = false;
	bool registry_unlinked = false;
	bool retire_leases = false;

	(void)op;
	if (!data || !gdev)
		return -EINVAL;
	apm = q6apm_get_apm_from_gdev(gdev);
	if (!apm)
		return 0;
	dev = &gdev->dev;
	hdr = &data->hdr;

	mutex_lock(&apm->response_lock);
	ticket = q6apm_find_ticket_locked(apm, hdr->token);
	if (!ticket) {
		mutex_unlock(&apm->response_lock);
		dev_dbg(dev, "ignoring unknown APM response 0x%08x token %u\n",
			hdr->opcode, hdr->token);
		q6apm_put_apm(apm);
		return 0;
	}
	/* Hold a callback reference while the ticket is authenticated and updated. */
	kref_get(&ticket->refcount);
	if (ticket->state != Q6APM_CMD_PENDING &&
	    ticket->state != Q6APM_CMD_AMBIGUOUS &&
	    !q6apm_map_ticket_retained_locked(ticket))
		goto out;
	if (ticket->generation != atomic_read(&apm->reset_generation)) {
		mutex_lock(&apm->lock);
		if (ticket->lease)
			q6apm_lease_clear_locked(ticket->lease,
						 AUDIOREACH_MEM_RESET_RETIRED);
		mutex_unlock(&apm->lock);
		q6apm_ticket_complete_locked(ticket, Q6APM_CMD_RESET_RETIRED,
					     -ENETRESET);
		completed = true;
		goto out;
	}
	if (ticket->kind == Q6APM_CMD_MAP && ticket->map_protocol_error)
		goto out;

	if (hdr->opcode == GPR_BASIC_RSP_RESULT) {
		if (!data->payload || data->payload_size < sizeof(*result)) {
			malformed = true;
			goto protocol_error;
		}
		result = data->payload;
		/* The echoed command opcode is part of ticket authentication. */
		if (result->opcode != ticket->opcode) {
			dev_err(dev,
				"mismatched APM response token %u: expected 0x%08x got 0x%08x\n",
				hdr->token, ticket->opcode, result->opcode);
			goto protocol_error;
		}
		/*
		 * GET_SPF_STATE returns the state in its dedicated response
		 * payload.  A successful basic response carries no state value;
		 * leave the ticket pending instead of returning stale apm->state.
		 */
		if (ticket->opcode == APM_CMD_GET_SPF_STATE) {
			if (result->status) {
				q6apm_ticket_complete_locked(ticket,
							     Q6APM_CMD_CONFIRMED_ERROR,
							     -EINVAL);
				completed = true;
			}
			goto out;
		}

		if (ticket->kind == Q6APM_CMD_MAP) {
			/*
			 * A basic MAP success carries no handle and is advisory.  Record
			 * every phase so a later contradictory response cannot be mistaken
			 * for an unrelated event after the waiter has already returned.
			 */
			if (!ticket->lease)
				goto protocol_error;
			mutex_lock(&apm->lock);
			if (ticket->lease->map_ticket != ticket ||
			    ticket->lease->generation != ticket->generation) {
				mutex_unlock(&apm->lock);
				goto protocol_error;
			}
			mutex_unlock(&apm->lock);
			if (ticket->map_basic_seen) {
				if (ticket->map_basic_status != result->status)
					goto protocol_error;
				goto out;
			}
			ticket->map_basic_seen = true;
			ticket->map_basic_status = result->status;
			if (!result->status)
				goto out;
			if (ticket->map_handle_seen)
				goto protocol_error;
			/*
			 * An error phase is terminal for the waiter, but it does not
			 * prove that a command-specific handle cannot arrive later.  Keep
			 * the lease quarantined until reset or a trusted retirement.
			 */
			mutex_lock(&apm->lock);
			if (ticket->lease->map_ticket == ticket &&
			    ticket->lease->generation == ticket->generation) {
				ticket->lease->handle = 0;
				ticket->lease->users = 0;
				ticket->lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
			}
			mutex_unlock(&apm->lock);
			q6apm_ticket_complete_locked(ticket,
						     Q6APM_CMD_CONFIRMED_ERROR,
						     -EINVAL);
			completed = true;
			dev_err(dev, "APM MAP rejected: %u\n", result->status);
			goto out;
		}

		if (ticket->kind == Q6APM_CMD_UNMAP) {
			bool valid = false;

			mutex_lock(&apm->lock);
			if (ticket->lease &&
			    ticket->lease->generation == ticket->generation &&
			    ticket->lease->handle == ticket->lease_handle &&
			    (ticket->lease->state == AUDIOREACH_MEM_UNMAP_PENDING ||
			     ticket->lease->state == AUDIOREACH_MEM_UNMAP_UNCERTAIN)) {
				valid = true;
				if (!result->status) {
					q6apm_lease_clear_locked(ticket->lease,
								 AUDIOREACH_MEM_UNMAPPED);
					retire_leases = true;
				} else {
					ticket->lease->state = AUDIOREACH_MEM_MAPPED;
				}
			}
			mutex_unlock(&apm->lock);
			if (!valid && !result->status)
				goto protocol_error;
			q6apm_ticket_complete_locked(ticket,
						     result->status ?
						     Q6APM_CMD_CONFIRMED_ERROR :
						     Q6APM_CMD_CONFIRMED_OK,
						     result->status ? -EINVAL : 0);
			completed = true;
			if (result->status)
				dev_err(dev, "APM UNMAP rejected: %u\n", result->status);
			goto out;
		}

		if (ticket->opcode == APM_CMD_GRAPH_OPEN ||
		    ticket->opcode == APM_CMD_GRAPH_CLOSE ||
		    ticket->opcode == APM_CMD_GRAPH_START ||
		    ticket->opcode == APM_CMD_GRAPH_STOP ||
		    ticket->opcode == APM_CMD_GRAPH_FLUSH)
			graph_ref = q6apm_apply_graph_result(apm, ticket->graph_id,
							     ticket->graph_cookie,
							     ticket->opcode,
							     result->status,
							     ticket->flush_after_stop);
		q6apm_ticket_complete_locked(ticket,
					     result->status ?
					     Q6APM_CMD_CONFIRMED_ERROR :
					 Q6APM_CMD_CONFIRMED_OK,
					     result->status ? -EINVAL : 0);
		completed = true;
		goto out;
	}

	if (hdr->opcode == APM_CMD_RSP_SHARED_MEM_MAP_REGIONS) {
		if (ticket->kind != Q6APM_CMD_MAP ||
		    ticket->response_opcode != APM_CMD_RSP_SHARED_MEM_MAP_REGIONS)
			goto protocol_error;
		if (!data->payload || data->payload_size < sizeof(*map_rsp)) {
			malformed = true;
			goto protocol_error;
		}
		map_rsp = data->payload;
		if (!map_rsp->mem_map_handle)
			goto protocol_error;
		if (!ticket->lease)
			goto protocol_error;
		if (ticket->map_basic_seen && ticket->map_basic_status)
			goto protocol_error;
		if (ticket->map_handle_seen) {
			if (ticket->map_handle != map_rsp->mem_map_handle)
				goto protocol_error;
			/* Identical dedicated handles are compatible duplicates. */
			goto out;
		}

		mutex_lock(&apm->lock);
		if (ticket->lease->map_ticket == ticket &&
		    ticket->lease->generation == ticket->generation &&
		    (ticket->lease->state == AUDIOREACH_MEM_MAP_PENDING ||
		     ticket->lease->state == AUDIOREACH_MEM_MAP_UNCERTAIN)) {
			ticket->map_handle_seen = true;
			ticket->map_handle = map_rsp->mem_map_handle;
			ticket->lease->handle = map_rsp->mem_map_handle;
			ticket->lease->users = max_t(u32, ticket->lease->users, 1);
			ticket->lease->state = AUDIOREACH_MEM_MAPPED;
		} else {
			mutex_unlock(&apm->lock);
			goto protocol_error;
		}
		mutex_unlock(&apm->lock);
		q6apm_ticket_complete_locked(ticket, Q6APM_CMD_CONFIRMED_OK, 0);
		completed = true;
		goto out;
	}

	if (hdr->opcode == APM_CMD_RSP_GET_SPF_STATE) {
		u32 state;

		if (ticket->opcode != APM_CMD_GET_SPF_STATE ||
		    ticket->response_opcode != APM_CMD_RSP_GET_SPF_STATE ||
		    !data->payload || data->payload_size < sizeof(state)) {
			malformed = true;
			goto protocol_error;
		}
		state = *(u32 *)data->payload;
		WRITE_ONCE(apm->state, state);
		q6apm_ticket_complete_locked(ticket, Q6APM_CMD_CONFIRMED_OK, 0);
		completed = true;
		goto out;
	}

	/*
	 * An unknown token is filtered above.  Reaching this point with a live
	 * ticket token is an authenticated response-opcode conflict.
	 */
	goto protocol_error;

protocol_error:
	if (ticket->kind == Q6APM_CMD_MAP)
		ticket->map_protocol_error = true;
	if (ticket->lease) {
		mutex_lock(&apm->lock);
		if (ticket->lease->generation == ticket->generation &&
		    (ticket->kind != Q6APM_CMD_MAP ||
		     ticket->lease->map_ticket == ticket)) {
			if (ticket->kind == Q6APM_CMD_MAP &&
			    (ticket->lease->state == AUDIOREACH_MEM_MAP_PENDING ||
			     ticket->lease->state == AUDIOREACH_MEM_MAP_UNCERTAIN ||
			     ticket->lease->state == AUDIOREACH_MEM_MAPPED))
				ticket->lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;
			else if (ticket->kind == Q6APM_CMD_UNMAP &&
				 (ticket->lease->state == AUDIOREACH_MEM_UNMAP_PENDING ||
				  ticket->lease->state == AUDIOREACH_MEM_UNMAP_UNCERTAIN))
				ticket->lease->state = AUDIOREACH_MEM_UNMAP_UNCERTAIN;
		}
		mutex_unlock(&apm->lock);
	}
	if (malformed)
		dev_err(dev, "malformed APM response 0x%08x for token %u\n",
			hdr->opcode, hdr->token);
	q6apm_ticket_complete_locked(ticket, Q6APM_CMD_AMBIGUOUS, -EPROTO);
	completed = true;

out:
	if (completed && !ticket->waiter_active &&
	    ticket->state != Q6APM_CMD_AMBIGUOUS &&
	    !q6apm_map_ticket_retained_locked(ticket))
		registry_unlinked = q6apm_ticket_unlink_locked(ticket);
	mutex_unlock(&apm->response_lock);
	if (registry_unlinked)
		kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
	if (graph_ref)
		q6apm_graph_put_ref(graph_ref);
	kref_put(&ticket->refcount, q6apm_cmd_ticket_release);
	if (retire_leases)
		q6apm_retire_old_leases(apm);
	q6apm_put_apm(apm);
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
		.suppress_bind_attrs = true,
	},
};

module_gpr_driver(apm_driver);
MODULE_DESCRIPTION("Audio Process Manager");
MODULE_LICENSE("GPL");
