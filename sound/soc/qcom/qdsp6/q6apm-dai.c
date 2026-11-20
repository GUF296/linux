// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2021, Linaro Limited

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <sound/pcm.h>
#include <sound/core.h>
#include <asm/div64.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <sound/pcm_params.h>
#include "q6apm.h"

#define DRV_NAME "q6apm-dai"

#define PLAYBACK_MIN_NUM_PERIODS	2
#define PLAYBACK_MAX_NUM_PERIODS	8
#define PLAYBACK_MAX_PERIOD_SIZE	65536
#define PLAYBACK_MIN_PERIOD_SIZE	128
#define CAPTURE_MIN_NUM_PERIODS		2
#define CAPTURE_MAX_NUM_PERIODS		8
#define CAPTURE_MAX_PERIOD_SIZE		65536
#define CAPTURE_MIN_PERIOD_SIZE		6144
#define BUFFER_BYTES_MAX (PLAYBACK_MAX_NUM_PERIODS * PLAYBACK_MAX_PERIOD_SIZE)
#define BUFFER_BYTES_MIN (PLAYBACK_MIN_NUM_PERIODS * PLAYBACK_MIN_PERIOD_SIZE)
#define COMPR_PLAYBACK_MAX_FRAGMENT_SIZE (128 * 1024)
#define COMPR_PLAYBACK_MAX_NUM_FRAGMENTS (16 * 4)
#define COMPR_PLAYBACK_MIN_FRAGMENT_SIZE (8 * 1024)
#define COMPR_PLAYBACK_MIN_NUM_FRAGMENTS (4)
#define Q6APM_CLEANUP_RETRY_DELAY msecs_to_jiffies(1000)
#define SID_MASK_DEFAULT	0xF

static const struct snd_compr_codec_caps q6apm_compr_caps = {
	.num_descriptors = 1,
	.descriptor[0].max_ch = 2,
	.descriptor[0].sample_rates = {	8000, 11025, 12000, 16000, 22050,
					24000, 32000, 44100, 48000, 88200,
					96000, 176400, 192000 },
	.descriptor[0].num_sample_rates = 13,
	.descriptor[0].bit_rate[0] = 320,
	.descriptor[0].bit_rate[1] = 128,
	.descriptor[0].num_bitrates = 2,
	.descriptor[0].profiles = 0,
	.descriptor[0].modes = SND_AUDIOCHANMODE_MP3_STEREO,
	.descriptor[0].formats = 0,
};

enum stream_state {
	Q6APM_STREAM_IDLE = 0,
	Q6APM_STREAM_STOPPED,
	Q6APM_STREAM_RUNNING,
};

enum drain_state {
	Q6APM_DRAIN_NONE,
	Q6APM_DRAIN_REQUESTED,
	Q6APM_DRAIN_EOS_SENT,
};

struct q6apm_pcm_owner;

struct q6apm_dai_rtd {
	/* Base reference is held by the ALSA callback/runtime owner. */
	struct kref refcount;
	struct device *dev;
	struct q6apm *apm;
	struct snd_pcm_substream *substream;
	struct snd_compr_stream *cstream;
	struct snd_codec codec;
	struct snd_compr_params codec_param;
	struct snd_dma_buffer dma_buffer;
	phys_addr_t phys;
	unsigned int pcm_size;
	unsigned int pcm_count;
	unsigned int periods;
	u64 bytes_sent;
	u64 bytes_received;
	u64 bytes_completed;
	u64 user_bytes_received;
	u64 copied_total;
	u64 gapless_track_end;
	u64 gapless_next_start;
	/* Transaction snapshot for the ALSA geometry installed by set_params. */
	u32 params_old_fragment_size;
	u32 params_old_fragments;
	snd_pcm_uframes_t params_old_buffer_size;
	bool params_snapshot_valid;
	u16 bits_per_sample;
	snd_pcm_uframes_t queue_ptr;
	bool next_track;
	bool next_track_data;
	bool graph_started;
	/* EOS completion can leave the shared graph enrolled while the DSP
	 * decoder is stopped; the next START must explicitly re-arm it.
	 */
	bool rearm_pending;
	bool fragments_allocated;
	bool memory_mapped;
	unsigned int memory_map_generation;
	bool stop_reset;
	bool reset_pending;
	bool flush_retry;
	bool closing;
	bool error_pending;
	bool drain_notify_pending;
	bool module_pinned;
	bool quarantined;
	/* Work items hold a reference while queued or running. */
	bool error_work_queued;
	bool drain_work_queued;
	bool cleanup_work_queued;
	enum stream_state state;
	enum drain_state drain_state;
	struct q6apm_graph *graph;
	/* Protects compressed-stream accounting and lifecycle state. */
	spinlock_t lock;
	struct work_struct error_work;
	struct delayed_work drain_work;
	struct delayed_work cleanup_work;
	struct q6apm_pcm_owner *pcm_owner;
};

struct q6apm_pcm_owner {
	struct list_head node;
	struct kref refcount;
	struct delayed_work cleanup_work;
	struct q6apm_dai_rtd *prtd;
	struct device *dev;
	struct q6apm *apm;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;
	struct snd_card *card;
	struct snd_dma_buffer saved_dma;
	int graph_id;
	unsigned int stream;
	unsigned int map_generation;
	bool mapped;
	bool quarantined;
	bool buffer_saved;
	bool pcm_gone;
	bool accounted;
	bool module_pinned;
	bool prtd_ref;
	/* Serialized with q6apm_pcm_owners_lock. */
	bool cleanup_queued;
	bool cleanup_stop;
	bool listed;
};

static DEFINE_MUTEX(q6apm_pcm_owners_lock);
static LIST_HEAD(q6apm_pcm_owners);

static void q6apm_dai_rtd_release(struct kref *ref)
{
	struct q6apm_dai_rtd *prtd =
		container_of(ref, struct q6apm_dai_rtd, refcount);

	/* All graph, device and module ownership is released before this point. */
	kfree(prtd);
}

static void q6apm_dai_rtd_put(struct q6apm_dai_rtd *prtd)
{
	if (prtd)
		kref_put(&prtd->refcount, q6apm_dai_rtd_release);
}

static void q6apm_pcm_owner_release(struct kref *ref)
{
	struct q6apm_pcm_owner *owner =
		container_of(ref, struct q6apm_pcm_owner, refcount);

	WARN_ON(owner->cleanup_queued);
	WARN_ON(owner->prtd);
	WARN_ON(owner->prtd_ref);
	WARN_ON(owner->saved_dma.area);
	if (owner->module_pinned)
		module_put(THIS_MODULE);
	if (owner->card)
		put_device(&owner->card->card_dev);
	if (owner->dev)
		put_device(owner->dev);
	if (owner->apm)
		q6apm_put_apm(owner->apm);
	kfree(owner);
}

static void q6apm_pcm_owner_put(struct q6apm_pcm_owner *owner)
{
	if (owner)
		kref_put(&owner->refcount, q6apm_pcm_owner_release);
}

static void q6apm_compr_work_put(struct q6apm_dai_rtd *prtd, bool *queued)
{
	unsigned long flags;
	bool drop = false;

	spin_lock_irqsave(&prtd->lock, flags);
	if (*queued) {
		*queued = false;
		drop = true;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (drop)
		q6apm_dai_rtd_put(prtd);
}

static bool q6apm_compr_queue_error_work(struct q6apm_dai_rtd *prtd);
static bool q6apm_compr_queue_drain_work(struct q6apm_dai_rtd *prtd,
					 unsigned long delay);
static bool q6apm_compr_queue_cleanup_work(struct q6apm_dai_rtd *prtd,
					   unsigned long delay);
static void q6apm_compr_cancel_error_work(struct q6apm_dai_rtd *prtd);
static void q6apm_compr_cancel_drain_work(struct q6apm_dai_rtd *prtd);
static void q6apm_compr_cancel_cleanup_work(struct q6apm_dai_rtd *prtd);

static bool q6apm_compr_queue_error_work(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	bool queue = false;

	spin_lock_irqsave(&prtd->lock, flags);
	if (!prtd->closing && !prtd->error_work_queued) {
		prtd->error_work_queued = true;
		kref_get(&prtd->refcount);
		queue = true;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (queue)
		schedule_work(&prtd->error_work);
	return queue;
}

static bool q6apm_compr_queue_drain_work(struct q6apm_dai_rtd *prtd,
					 unsigned long delay)
{
	unsigned long flags;
	bool queue = false;

	spin_lock_irqsave(&prtd->lock, flags);
	if (!prtd->closing && !prtd->drain_work_queued) {
		prtd->drain_work_queued = true;
		kref_get(&prtd->refcount);
		queue = true;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (queue)
		schedule_delayed_work(&prtd->drain_work, delay);
	return queue;
}

static bool q6apm_compr_queue_cleanup_work(struct q6apm_dai_rtd *prtd,
					   unsigned long delay)
{
	unsigned long flags;
	bool queue = false;

	spin_lock_irqsave(&prtd->lock, flags);
	if (!prtd->cleanup_work_queued) {
		prtd->cleanup_work_queued = true;
		kref_get(&prtd->refcount);
		queue = true;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (queue)
		schedule_delayed_work(&prtd->cleanup_work, delay);
	return queue;
}

static void q6apm_compr_cancel_error_work(struct q6apm_dai_rtd *prtd)
{
	bool cancelled = cancel_work_sync(&prtd->error_work);

	if (cancelled)
		q6apm_compr_work_put(prtd, &prtd->error_work_queued);
}

static void q6apm_compr_cancel_drain_work(struct q6apm_dai_rtd *prtd)
{
	bool cancelled = cancel_delayed_work_sync(&prtd->drain_work);

	if (cancelled)
		q6apm_compr_work_put(prtd, &prtd->drain_work_queued);
}

static void q6apm_compr_cancel_cleanup_work(struct q6apm_dai_rtd *prtd)
{
	bool cancelled = cancel_delayed_work_sync(&prtd->cleanup_work);

	if (cancelled)
		q6apm_compr_work_put(prtd, &prtd->cleanup_work_queued);
}

/*
 * A quarantine owner is kept in the registry until pcm_free has handed the
 * persistent substream away.  The queued bit covers both pending and running
 * work; cleanup_stop prevents a worker from re-arming itself while pcm_free
 * is joining it.
 */
static void q6apm_pcm_schedule_cleanup(struct q6apm_pcm_owner *owner,
				       unsigned long delay)
{
	bool queue = false;

	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->listed && !owner->cleanup_stop && !owner->cleanup_queued) {
		owner->cleanup_queued = true;
		kref_get(&owner->refcount);
		queue = true;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (queue)
		schedule_delayed_work(&owner->cleanup_work, delay);
}

static void q6apm_pcm_cancel_cleanup(struct q6apm_pcm_owner *owner)
{
	bool cancelled;
	bool drop_work = false;

	mutex_lock(&q6apm_pcm_owners_lock);
	owner->cleanup_stop = true;
	mutex_unlock(&q6apm_pcm_owners_lock);

	cancelled = cancel_delayed_work_sync(&owner->cleanup_work);
	mutex_lock(&q6apm_pcm_owners_lock);
	if (cancelled && owner->cleanup_queued) {
		owner->cleanup_queued = false;
		drop_work = true;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (drop_work)
		q6apm_pcm_owner_put(owner);
}

struct q6apm_dai_data {
	long long sid;
};

static int q6apm_compr_validate_geometry(struct snd_compr_runtime *runtime,
					 size_t *ring_bytes)
{
	if (runtime->fragment_size < COMPR_PLAYBACK_MIN_FRAGMENT_SIZE ||
	    runtime->fragment_size > COMPR_PLAYBACK_MAX_FRAGMENT_SIZE ||
	    runtime->fragments < COMPR_PLAYBACK_MIN_NUM_FRAGMENTS ||
	    runtime->fragments > COMPR_PLAYBACK_MAX_NUM_FRAGMENTS)
		return -EINVAL;

	if (check_mul_overflow((size_t)runtime->fragment_size,
			       (size_t)runtime->fragments, ring_bytes))
		return -EOVERFLOW;

	return 0;
}

static void q6apm_compr_restore_geometry(struct snd_compr_runtime *runtime,
					 const struct q6apm_dai_rtd *prtd)
{
	runtime->fragment_size = prtd->pcm_count;
	runtime->fragments = prtd->periods;
	runtime->buffer_size = prtd->pcm_size;
}

static int q6apm_compr_params_error(struct snd_compr_stream *stream,
				    struct q6apm_dai_rtd *prtd,
				    bool core_next_track, int ret)
{
	/* During gapless set_params the core has already installed the requested
	 * geometry, while the active DSP ring still uses the previous geometry.
	 * That ring is authoritative on every rejected gapless request, including
	 * validation failures reached before the mismatch check.
	 */
	if (core_next_track) {
		q6apm_compr_restore_geometry(stream->runtime, prtd);
	} else if (prtd->params_snapshot_valid) {
		stream->runtime->fragment_size = prtd->params_old_fragment_size;
		stream->runtime->fragments = prtd->params_old_fragments;
		stream->runtime->buffer_size = prtd->params_old_buffer_size;
	}
	prtd->params_snapshot_valid = false;

	return ret;
}

static int q6apm_compr_round_up(u64 value, u32 alignment, u64 *aligned)
{
	u64 quotient = value;
	u32 remainder;

	if (!alignment)
		return -EINVAL;

	remainder = do_div(quotient, alignment);
	if (!remainder) {
		*aligned = value;
		return 0;
	}

	if (check_add_overflow(value, (u64)alignment - remainder, aligned))
		return -EOVERFLOW;

	return 0;
}

static int q6apm_compr_alloc_dma(struct snd_soc_component *component,
				 struct q6apm_dai_rtd *prtd,
				 const struct q6apm_dai_data *pdata,
				 size_t ring_bytes)
{
	struct device *dev = component->dev;
	unsigned int map_generation = 0;
	size_t alloc_bytes = PAGE_ALIGN(ring_bytes);
	int ret;

	if (prtd->dma_buffer.area || prtd->memory_mapped)
		return -EBUSY;

	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, dev, alloc_bytes,
				  &prtd->dma_buffer);
	if (ret)
		return ret;

	if (pdata->sid < 0)
		prtd->phys = prtd->dma_buffer.addr;
	else
		prtd->phys = prtd->dma_buffer.addr | (pdata->sid << 32);

	ret = q6apm_map_memory_fixed_region(prtd->apm, prtd->graph->id,
					    prtd->phys,
					    prtd->dma_buffer.bytes,
					    &map_generation);
	if (map_generation) {
		/* A nonzero generation is the authoritative ownership result, even
		 * when transport or protocol completion returned an error.
		 */
		prtd->memory_mapped = true;
		prtd->memory_map_generation = map_generation;
		return ret;
	}
	if (!ret)
		ret = -EPROTO;
	snd_dma_free_pages(&prtd->dma_buffer);
	memset(&prtd->dma_buffer, 0, sizeof(prtd->dma_buffer));
	prtd->phys = 0;
	return ret;
}

static int q6apm_compr_free_dma(struct q6apm_dai_rtd *prtd)
{
	int ret;

	if (prtd->memory_mapped) {
		if (!prtd->memory_map_generation)
			return -EUCLEAN;
		if (!prtd->graph)
			return -ENODEV;
		ret = q6apm_unmap_memory_fixed_region(prtd->apm,
						      prtd->graph->id,
						      prtd->memory_map_generation);
		if (ret)
			return ret;
		prtd->memory_mapped = false;
		prtd->memory_map_generation = 0;
	}

	if (prtd->dma_buffer.area) {
		snd_dma_free_pages(&prtd->dma_buffer);
		memset(&prtd->dma_buffer, 0, sizeof(prtd->dma_buffer));
	}
	prtd->phys = 0;

	return 0;
}

static const struct snd_pcm_hardware q6apm_dai_hardware_capture = {
	.info =                 (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_NO_REWINDS | SNDRV_PCM_INFO_SYNC_APPLPTR |
				 SNDRV_PCM_INFO_BATCH),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE),
	.rates =                SNDRV_PCM_RATE_8000_48000,
	.rate_min =             8000,
	.rate_max =             48000,
	.channels_min =         2,
	.channels_max =         4,
	.buffer_bytes_max =     CAPTURE_MAX_NUM_PERIODS * CAPTURE_MAX_PERIOD_SIZE,
	.period_bytes_min =	CAPTURE_MIN_PERIOD_SIZE,
	.period_bytes_max =     CAPTURE_MAX_PERIOD_SIZE,
	.periods_min =          CAPTURE_MIN_NUM_PERIODS,
	.periods_max =          CAPTURE_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

static const struct snd_pcm_hardware q6apm_dai_hardware_playback = {
	.info =                 (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_NO_REWINDS | SNDRV_PCM_INFO_SYNC_APPLPTR |
				 SNDRV_PCM_INFO_BATCH),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE),
	.rates =                SNDRV_PCM_RATE_8000_192000,
	.rate_min =             8000,
	.rate_max =             192000,
	.channels_min =         2,
	.channels_max =         8,
	.buffer_bytes_max =     (PLAYBACK_MAX_NUM_PERIODS * PLAYBACK_MAX_PERIOD_SIZE),
	.period_bytes_min =	PLAYBACK_MIN_PERIOD_SIZE,
	.period_bytes_max =     PLAYBACK_MAX_PERIOD_SIZE,
	.periods_min =          PLAYBACK_MIN_NUM_PERIODS,
	.periods_max =          PLAYBACK_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

static bool q6apm_dai_cmd_ambiguous(int ret)
{
	return ret == -ETIMEDOUT || ret == -EPROTO;
}

static void q6apm_pcm_cleanup_work(struct work_struct *work);
static int q6apm_pcm_mark_gone(struct q6apm_pcm_owner *owner);

static struct q6apm_pcm_owner *
q6apm_pcm_find_owner_locked(const struct snd_pcm_substream *substream)
{
	struct q6apm_pcm_owner *owner;

	lockdep_assert_held(&q6apm_pcm_owners_lock);
	list_for_each_entry(owner, &q6apm_pcm_owners, node) {
		if (owner->substream == substream)
			return owner;
	}

	return NULL;
}

static int q6apm_pcm_claim_owner(struct snd_pcm_substream *substream,
				 struct q6apm_dai_rtd *prtd)
{
	struct q6apm_pcm_owner *owner;
	int ret = 0;

	mutex_lock(&q6apm_pcm_owners_lock);
	owner = q6apm_pcm_find_owner_locked(substream);
	if (!owner) {
		ret = -ENODEV;
	} else if (owner->quarantined || owner->prtd) {
		ret = -EBUSY;
	} else if (!kref_get_unless_zero(&prtd->refcount)) {
		ret = -ENODEV;
	} else {
		owner->prtd = prtd;
		owner->prtd_ref = true;
		prtd->pcm_owner = owner;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);

	return ret;
}

static void q6apm_pcm_unclaim_owner(struct q6apm_dai_rtd *prtd)
{
	struct q6apm_pcm_owner *owner = NULL;
	struct q6apm_pcm_owner *candidate;
	bool drop_prtd_ref = false;

	mutex_lock(&q6apm_pcm_owners_lock);
	list_for_each_entry(candidate, &q6apm_pcm_owners, node) {
		if (candidate->prtd == prtd) {
			owner = candidate;
			break;
		}
	}
	if (owner) {
		drop_prtd_ref = owner->prtd_ref;
		owner->prtd = NULL;
		owner->prtd_ref = false;
	}
	prtd->pcm_owner = NULL;
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (drop_prtd_ref)
		q6apm_dai_rtd_put(prtd);
}

static struct q6apm_pcm_owner *
q6apm_pcm_create_owner(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream, int graph_id)
{
	struct q6apm_pcm_owner *owner;

	owner = kzalloc_obj(*owner);
	if (!owner)
		return ERR_PTR(-ENOMEM);
	kref_init(&owner->refcount);
	/* A PCM owner can outlive the ALSA runtime while an unmap/graph close is
	 * being retried.  Pin the driver before publishing that owner so the
	 * deferred path never has to discover an unloading module after it has
	 * already hidden the persistent DMA buffer.
	 */
	if (!try_module_get(THIS_MODULE)) {
		q6apm_pcm_owner_put(owner);
		return ERR_PTR(-ENODEV);
	}
	owner->module_pinned = true;

	owner->apm = q6apm_get_apm(component->dev);
	if (!owner->apm) {
		q6apm_pcm_owner_put(owner);
		return ERR_PTR(-ENODEV);
	}
	owner->dev = get_device(component->dev);
	if (!owner->dev) {
		q6apm_pcm_owner_put(owner);
		return ERR_PTR(-ENODEV);
	}
	if (!get_device(&substream->pcm->card->card_dev)) {
		q6apm_pcm_owner_put(owner);
		return ERR_PTR(-ENODEV);
	}
	owner->card = substream->pcm->card;
	owner->pcm = substream->pcm;
	owner->substream = substream;
	owner->graph_id = graph_id;
	owner->stream = substream->stream;
	owner->accounted = !!substream->dma_buffer.area;
	INIT_DELAYED_WORK(&owner->cleanup_work, q6apm_pcm_cleanup_work);

	mutex_lock(&q6apm_pcm_owners_lock);
	if (q6apm_pcm_find_owner_locked(substream)) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		q6apm_pcm_owner_put(owner);
		return ERR_PTR(-EEXIST);
	}
	list_add_tail(&owner->node, &q6apm_pcm_owners);
	owner->listed = true;
	mutex_unlock(&q6apm_pcm_owners_lock);

	return owner;
}

static int q6apm_pcm_pin_quarantine(struct q6apm_pcm_owner *owner)
{
	int ret = 0;

	mutex_lock(&q6apm_pcm_owners_lock);
	/* Owners are pinned before publication.  A missing pin is an invariant
	 * violation: never hide an ALSA buffer that no deferred path can retire.
	 */
	if (WARN_ON_ONCE(!owner->module_pinned))
		ret = -ENODEV;
	mutex_unlock(&q6apm_pcm_owners_lock);

	return ret;
}

static int q6apm_pcm_save_buffer_locked(struct q6apm_pcm_owner *owner)
{
	lockdep_assert_held(&q6apm_pcm_owners_lock);
	if (owner->buffer_saved)
		return 0;
	if (!owner->substream)
		return -ENODATA;

	owner->saved_dma = owner->substream->dma_buffer;
	memset(&owner->substream->dma_buffer, 0,
	       sizeof(owner->substream->dma_buffer));
	owner->buffer_saved = true;

	return 0;
}

static int q6apm_pcm_drop_accounting_locked(struct q6apm_pcm_owner *owner)
{
	lockdep_assert_held(&q6apm_pcm_owners_lock);
	if (!owner->accounted || !owner->buffer_saved ||
	    !owner->saved_dma.area)
		return 0;
	if (!owner->card)
		return -ENODEV;

	mutex_lock(&owner->card->memory_mutex);
	if (owner->card->total_pcm_alloc_bytes < owner->saved_dma.bytes) {
		mutex_unlock(&owner->card->memory_mutex);
		return -EOVERFLOW;
	}
	owner->card->total_pcm_alloc_bytes -= owner->saved_dma.bytes;
	mutex_unlock(&owner->card->memory_mutex);
	owner->accounted = false;
	return 0;
}

static void q6apm_pcm_cleanup_done(struct q6apm_pcm_owner *owner,
				   bool remove_from_registry)
{
	bool drop_work = false;
	bool drop_registry = false;

	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->cleanup_queued) {
		owner->cleanup_queued = false;
		drop_work = true;
	}
	if (remove_from_registry && owner->listed) {
		list_del_init(&owner->node);
		owner->listed = false;
		drop_registry = true;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);

	if (drop_work)
		q6apm_pcm_owner_put(owner);
	if (drop_registry)
		q6apm_pcm_owner_put(owner);
}

static u32 q6apm_compr_reserve_next(struct q6apm_dai_rtd *prtd,
				    u32 *wflags, bool *last)
{
	u64 avail, send_limit, user_avail;
	u32 bytes;

	*wflags = 0;
	*last = false;
	if (prtd->state != Q6APM_STREAM_RUNNING || !prtd->pcm_count)
		return 0;
	if (prtd->bytes_sent < prtd->bytes_completed ||
	    prtd->bytes_received < prtd->bytes_sent ||
	    prtd->user_bytes_received < prtd->copied_total) {
		prtd->state = Q6APM_STREAM_STOPPED;
		return 0;
	}

	/* Keep one DSP buffer in flight so completion accounting is ordered. */
	if (prtd->bytes_sent != prtd->bytes_completed)
		return 0;

	send_limit = prtd->bytes_received;
	if (prtd->next_track) {
		if (prtd->gapless_track_end < prtd->bytes_sent ||
		    prtd->gapless_track_end > prtd->bytes_received) {
			prtd->state = Q6APM_STREAM_STOPPED;
			return 0;
		}
		send_limit = prtd->gapless_track_end;
	}

	avail = send_limit - prtd->bytes_sent;
	if (!avail)
		return 0;
	user_avail = prtd->user_bytes_received - prtd->copied_total;
	if (avail > user_avail) {
		prtd->state = Q6APM_STREAM_STOPPED;
		return 0;
	}

	if (avail < prtd->pcm_count &&
	    prtd->drain_state == Q6APM_DRAIN_NONE)
		return 0;

	bytes = min_t(uint64_t, avail, prtd->pcm_count);
	*last = prtd->drain_state != Q6APM_DRAIN_NONE &&
		bytes == avail;
	if (*last && prtd->next_track)
		*wflags |= APM_LAST_BUFFER_FLAG;
	prtd->bytes_sent += bytes;

	return bytes;
}

static bool q6apm_compr_drain_ready(const struct q6apm_dai_rtd *prtd)
{
	u64 drain_end = prtd->bytes_received;

	if (prtd->next_track)
		drain_end = prtd->gapless_track_end;

	return prtd->drain_state == Q6APM_DRAIN_REQUESTED &&
		prtd->bytes_sent == prtd->bytes_completed &&
		prtd->bytes_completed == drain_end;
}

static void q6apm_compr_mark_error_locked(struct q6apm_dai_rtd *prtd)
{
	lockdep_assert_held(&prtd->lock);
	prtd->drain_notify_pending = false;
	prtd->error_pending = true;
}

static int q6apm_compr_send_eos(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&prtd->lock, flags);
	if (prtd->drain_state != Q6APM_DRAIN_REQUESTED) {
		spin_unlock_irqrestore(&prtd->lock, flags);
		return 0;
	}
	prtd->drain_state = Q6APM_DRAIN_EOS_SENT;
	spin_unlock_irqrestore(&prtd->lock, flags);

	ret = audioreach_shared_memory_send_eos(prtd->graph);
	/* GPR uses any non-negative value for success; ALSA callbacks must
	 * return the conventional zero success value.
	 */
	if (ret > 0)
		ret = 0;
	if (ret < 0) {
		spin_lock_irqsave(&prtd->lock, flags);
		if (prtd->drain_state == Q6APM_DRAIN_EOS_SENT)
			prtd->drain_state = Q6APM_DRAIN_NONE;
		prtd->state = Q6APM_STREAM_STOPPED;
		q6apm_compr_mark_error_locked(prtd);
		spin_unlock_irqrestore(&prtd->lock, flags);
	}

	return ret;
}

static int q6apm_compr_submit(struct q6apm_dai_rtd *prtd, u32 bytes,
			      u32 wflags, bool send_eos, bool *submitted)
{
	unsigned long flags;
	u64 reserved_end;
	int ret;

	if (submitted)
		*submitted = false;
	spin_lock_irqsave(&prtd->lock, flags);
	reserved_end = prtd->bytes_sent;
	spin_unlock_irqrestore(&prtd->lock, flags);

	ret = q6apm_write_async(prtd->graph, bytes, 0, 0, wflags);
	if (ret) {
		spin_lock_irqsave(&prtd->lock, flags);
		/* Do not roll back a reservation already retired by a callback. */
		if (prtd->bytes_sent == reserved_end &&
		    reserved_end >= bytes &&
		    prtd->bytes_completed < reserved_end)
			prtd->bytes_sent -= bytes;
		prtd->drain_state = Q6APM_DRAIN_NONE;
		prtd->state = Q6APM_STREAM_STOPPED;
		q6apm_compr_mark_error_locked(prtd);
		spin_unlock_irqrestore(&prtd->lock, flags);
		return ret;
	}
	if (submitted)
		*submitted = true;

	if (send_eos)
		ret = q6apm_compr_send_eos(prtd);

	return ret;
}

static void q6apm_compr_reset_counters(struct q6apm_dai_rtd *prtd)
{
	prtd->bytes_sent = 0;
	prtd->bytes_received = 0;
	prtd->bytes_completed = 0;
	prtd->user_bytes_received = 0;
	prtd->copied_total = 0;
	prtd->gapless_track_end = 0;
	prtd->gapless_next_start = 0;
	prtd->next_track = false;
	prtd->next_track_data = false;
	prtd->rearm_pending = false;
	prtd->drain_state = Q6APM_DRAIN_NONE;
	prtd->error_pending = false;
}

static void q6apm_compr_clear_gapless(struct q6apm_dai_rtd *prtd)
{
	prtd->gapless_track_end = 0;
	prtd->gapless_next_start = 0;
	prtd->next_track = false;
	prtd->next_track_data = false;
}

static void q6apm_compr_set_flush_retry(struct q6apm_dai_rtd *prtd,
					bool pending)
{
	unsigned long flags;

	spin_lock_irqsave(&prtd->lock, flags);
	prtd->flush_retry = pending;
	spin_unlock_irqrestore(&prtd->lock, flags);
}

static void q6apm_compr_complete_stop(struct q6apm_dai_rtd *prtd,
				      struct snd_compr_stream *stream)
{
	unsigned long flags;
	bool attached;

	spin_lock_irqsave(&prtd->lock, flags);
	attached = stream && prtd->cstream == stream && !prtd->closing;
	q6apm_compr_clear_gapless(prtd);
	prtd->error_pending = false;
	prtd->drain_notify_pending = false;
	spin_unlock_irqrestore(&prtd->lock, flags);
	/* The caller holds the compressed-device mutex, so update the core
	 * stream while it is still a live callback target.
	 */
	if (attached)
		stream->next_track = false;
}

static int q6apm_compr_finish_partial_drain(struct q6apm_dai_rtd *prtd)
{
	u64 new_received;

	if (!prtd->next_track ||
	    prtd->gapless_next_start < prtd->gapless_track_end ||
	    prtd->bytes_sent != prtd->gapless_track_end ||
	    prtd->bytes_completed != prtd->gapless_track_end ||
	    prtd->bytes_received < prtd->gapless_track_end)
		return -EIO;

	new_received = max(prtd->bytes_received, prtd->gapless_next_start);
	if (new_received - prtd->gapless_next_start > prtd->pcm_size)
		return -EOVERFLOW;

	prtd->bytes_sent = prtd->gapless_next_start;
	prtd->bytes_completed = prtd->gapless_next_start;
	prtd->bytes_received = new_received;

	q6apm_compr_clear_gapless(prtd);

	return 0;
}

static int q6apm_compr_finish_full_drain(struct q6apm_dai_rtd *prtd)
{
	u64 next_start;
	int ret;

	if (prtd->next_track ||
	    prtd->bytes_sent != prtd->bytes_completed ||
	    prtd->bytes_received != prtd->bytes_completed ||
	    prtd->user_bytes_received != prtd->copied_total)
		return -EIO;

	ret = q6apm_compr_round_up(prtd->bytes_received, prtd->pcm_count,
				   &next_start);
	if (ret)
		return ret;

	prtd->bytes_sent = next_start;
	prtd->bytes_completed = next_start;
	prtd->bytes_received = next_start;
	q6apm_compr_clear_gapless(prtd);
	prtd->state = Q6APM_STREAM_STOPPED;
	prtd->rearm_pending = true;

	return 0;
}

static int q6apm_compr_finish_stop_reset(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	bool generation_changed;
	bool graph_started;
	bool flush_retry;
	bool graph_flush_pending;
	bool pending;
	int ret;

	spin_lock_irqsave(&prtd->lock, flags);
	pending = prtd->reset_pending;
	graph_started = prtd->graph_started;
	flush_retry = prtd->flush_retry;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (!pending && !flush_retry &&
	    !q6apm_graph_flush_pending(prtd->graph))
		return 0;
	/* A local reset cannot release buffers while this client is enrolled. */
	if (graph_started)
		return -EBUSY;

	/* A peer may have completed a graph-wide flush since our STOP. */
	generation_changed = q6apm_graph_sync_flush_generation(prtd->graph);
	spin_lock_irqsave(&prtd->lock, flags);
	if (generation_changed)
		prtd->flush_retry = false;
	flush_retry = prtd->flush_retry;
	spin_unlock_irqrestore(&prtd->lock, flags);

	graph_flush_pending = q6apm_graph_flush_pending(prtd->graph);
	if (flush_retry || graph_flush_pending) {
		/* A failed shared flush is a retry obligation, not a reset result. */
		if (!q6apm_graph_request_flush(prtd->graph))
			return -EBUSY;
		ret = q6apm_graph_flush(prtd->graph);
		if (ret) {
			q6apm_compr_set_flush_retry(prtd, true);
			return ret;
		}
		q6apm_compr_set_flush_retry(prtd, false);
	}
	if (!pending)
		return 0;
	if (!q6apm_graph_try_reset_client(prtd->graph))
		return -EBUSY;

	spin_lock_irqsave(&prtd->lock, flags);
	if (prtd->reset_pending) {
		q6apm_compr_reset_counters(prtd);
		prtd->reset_pending = false;
		/* Preserve data pre-buffered after this reset across a peer flush. */
		prtd->stop_reset = true;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);

	return 0;
}

static int q6apm_compr_try_release(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	bool graph_started;
	bool fragments_allocated;
	bool flush_retry;
	int ret;

	if (!prtd->graph)
		return -ENODEV;

	spin_lock_irqsave(&prtd->lock, flags);
	graph_started = prtd->graph_started;
	prtd->reset_pending = true;
	spin_unlock_irqrestore(&prtd->lock, flags);

	if (graph_started) {
		q6apm_graph_quiesce_callbacks(prtd->graph);
		ret = q6apm_graph_stop_with_state(prtd->graph, NULL);
		if (ret)
			return ret;

		spin_lock_irqsave(&prtd->lock, flags);
		prtd->graph_started = false;
		spin_unlock_irqrestore(&prtd->lock, flags);
	}

	ret = q6apm_compr_finish_stop_reset(prtd);
	if (ret)
		return ret;

	spin_lock_irqsave(&prtd->lock, flags);
	graph_started = prtd->graph_started;
	fragments_allocated = prtd->fragments_allocated;
	flush_retry = prtd->flush_retry;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (graph_started || flush_retry ||
	    q6apm_graph_flush_pending(prtd->graph))
		return -EBUSY;

	if (fragments_allocated) {
		q6apm_graph_quiesce_callbacks(prtd->graph);
		ret = q6apm_free_fragments(prtd->graph,
					   SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			return ret;
		spin_lock_irqsave(&prtd->lock, flags);
		prtd->fragments_allocated = false;
		spin_unlock_irqrestore(&prtd->lock, flags);
	}

	return q6apm_compr_free_dma(prtd);
}

static int q6apm_compr_release_owner(struct q6apm_dai_rtd *prtd)
{
	int ret;

	WARN_ON(prtd->graph &&
		(prtd->fragments_allocated || prtd->memory_mapped ||
		 prtd->dma_buffer.area));
	if (prtd->graph) {
		ret = q6apm_graph_close(prtd->graph);
		if (ret)
			return ret;
		prtd->graph = NULL;
	}
	if (prtd->dev) {
		put_device(prtd->dev);
		prtd->dev = NULL;
	}
	if (prtd->apm) {
		q6apm_put_apm(prtd->apm);
		prtd->apm = NULL;
	}
	if (prtd->module_pinned) {
		module_put(THIS_MODULE);
		prtd->module_pinned = false;
	}

	return 0;
}

static void q6apm_compr_cleanup_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct q6apm_dai_rtd *prtd = container_of(dwork, struct q6apm_dai_rtd,
						 cleanup_work);
		int ret;

	ret = q6apm_compr_try_release(prtd);
	if (ret) {
		if (prtd->dev)
			dev_warn_ratelimited(prtd->dev,
					     "compressed close still owns DSP resources: %d\n",
					     ret);
		schedule_delayed_work(&prtd->cleanup_work,
				      Q6APM_CLEANUP_RETRY_DELAY);
		return;
	}

	ret = q6apm_compr_release_owner(prtd);
	if (ret) {
		if (prtd->dev)
			dev_warn_ratelimited(prtd->dev,
					     "compressed graph close still pending: %d\n",
					     ret);
		schedule_delayed_work(&prtd->cleanup_work,
				      Q6APM_CLEANUP_RETRY_DELAY);
		return;
	}
	q6apm_compr_work_put(prtd, &prtd->cleanup_work_queued);
	q6apm_dai_rtd_put(prtd);
}

static void q6apm_compr_stop_error_locked(struct q6apm_dai_rtd *prtd)
{
	struct snd_compr_stream *stream;
	unsigned long flags;
	bool closing;

	spin_lock_irqsave(&prtd->lock, flags);
	stream = prtd->cstream;
	closing = prtd->closing;
	q6apm_compr_mark_error_locked(prtd);
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (stream && !closing) {
		lockdep_assert_held(&stream->device->lock);
		snd_compr_stop_error(stream, SNDRV_PCM_STATE_XRUN);
	}
}

static void q6apm_compr_error_work(struct work_struct *work)
{
	struct q6apm_dai_rtd *prtd = container_of(work, struct q6apm_dai_rtd,
						 error_work);
	struct snd_compr_stream *stream;
	unsigned long flags;

	spin_lock_irqsave(&prtd->lock, flags);
	stream = prtd->closing || !prtd->error_pending ? NULL : prtd->cstream;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (!stream) {
		q6apm_compr_work_put(prtd, &prtd->error_work_queued);
		return;
	}
	guard(mutex)(&stream->device->lock);
	spin_lock_irqsave(&prtd->lock, flags);
	stream = prtd->closing || !prtd->error_pending ? NULL : prtd->cstream;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (stream)
		q6apm_compr_stop_error_locked(prtd);
	q6apm_compr_work_put(prtd, &prtd->error_work_queued);
}

static void q6apm_compr_report_error(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	bool queue;

	spin_lock_irqsave(&prtd->lock, flags);
	q6apm_compr_mark_error_locked(prtd);
	queue = !prtd->closing;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (queue)
		q6apm_compr_queue_error_work(prtd);
}

static void q6apm_compr_operation_error_locked(struct q6apm_dai_rtd *prtd)
{
	/* Operation callbacks other than release run under the core mutex. */
	q6apm_compr_stop_error_locked(prtd);
}

static bool q6apm_compr_error_is_pending(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&prtd->lock, flags);
	pending = prtd->error_pending;
	spin_unlock_irqrestore(&prtd->lock, flags);

	return pending;
}

static void q6apm_compr_drain_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct q6apm_dai_rtd *prtd = container_of(dwork, struct q6apm_dai_rtd,
						 drain_work);
	struct snd_compr_stream *stream;
	unsigned long flags;
	bool pending;
	bool notify;
	bool retry;
	bool rescheduled;

	spin_lock_irqsave(&prtd->lock, flags);
	stream = prtd->closing ? NULL : prtd->cstream;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (!stream) {
		q6apm_compr_work_put(prtd, &prtd->drain_work_queued);
		return;
	}

	guard(mutex)(&stream->device->lock);
	spin_lock_irqsave(&prtd->lock, flags);
	pending = !prtd->closing && prtd->drain_notify_pending &&
		  prtd->cstream == stream;
	retry = false;
	notify = false;
	if (pending && !prtd->error_pending) {
		if (stream->runtime->state == SNDRV_PCM_STATE_DRAINING) {
			notify = true;
			prtd->drain_notify_pending = false;
		} else {
			/* Keep the only completion until the core observes DRAINING. */
			switch (stream->runtime->state) {
			case SNDRV_PCM_STATE_OPEN:
			case SNDRV_PCM_STATE_XRUN:
			case SNDRV_PCM_STATE_DISCONNECTED:
				retry = false;
				prtd->drain_notify_pending = false;
				break;
			default:
				retry = true;
				break;
			}
		}
	} else if (pending) {
		prtd->drain_notify_pending = false;
	}
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (notify) {
		snd_compr_drain_notify(stream);
		q6apm_compr_work_put(prtd, &prtd->drain_work_queued);
	} else if (retry) {
		rescheduled = false;
		spin_lock_irqsave(&prtd->lock, flags);
		if (!prtd->closing && prtd->drain_notify_pending) {
			schedule_delayed_work(&prtd->drain_work, 1);
			rescheduled = true;
		}
		spin_unlock_irqrestore(&prtd->lock, flags);
		if (!rescheduled)
			q6apm_compr_work_put(prtd, &prtd->drain_work_queued);
	} else {
		q6apm_compr_work_put(prtd, &prtd->drain_work_queued);
	}
}

static void q6apm_compr_schedule_drain_notify(struct q6apm_dai_rtd *prtd)
{
	unsigned long flags;
	bool queue;

	spin_lock_irqsave(&prtd->lock, flags);
	queue = !prtd->closing && !prtd->error_pending;
	if (queue)
		prtd->drain_notify_pending = true;
	spin_unlock_irqrestore(&prtd->lock, flags);
	if (queue)
		q6apm_compr_queue_drain_work(prtd, 0);
}

static void event_handler(uint32_t opcode, uint32_t token, uint32_t bytes,
			  void *payload, void *priv)
{
	struct q6apm_dai_rtd *prtd = priv;
	struct snd_pcm_substream *substream;
	int ret;

	if (!kref_get_unless_zero(&prtd->refcount))
		return;
	substream = READ_ONCE(prtd->substream);
	/* Graph accounting is retired before this callback is entered. */
	if (!substream)
		goto out;

	switch (opcode) {
	case APM_CLIENT_EVENT_CMD_EOS_DONE:
		prtd->state = Q6APM_STREAM_STOPPED;
		break;
	case APM_CLIENT_EVENT_DATA_WRITE_DONE:
		if (!bytes) {
			prtd->state = Q6APM_STREAM_STOPPED;
			snd_pcm_stop_xrun(substream);
			break;
		}
		snd_pcm_period_elapsed(substream);

		break;
	case APM_CLIENT_EVENT_DATA_READ_DONE:
		if (!bytes) {
			prtd->state = Q6APM_STREAM_STOPPED;
			snd_pcm_stop_xrun(substream);
			break;
		}
		snd_pcm_period_elapsed(substream);
		if (prtd->state == Q6APM_STREAM_RUNNING) {
			ret = q6apm_read(prtd->graph);
			if (ret) {
				prtd->state = Q6APM_STREAM_STOPPED;
				snd_pcm_stop_xrun(substream);
				dev_err(prtd->graph->dev,
					"capture refill failed: %d\n", ret);
			}
		}

		break;
	default:
		break;
	}

out:
	q6apm_dai_rtd_put(prtd);
}

static void event_handler_compr(uint32_t opcode, uint32_t token,
				uint32_t bytes, void *payload, void *priv)
{
	struct q6apm_dai_rtd *prtd = priv;
	struct snd_compr_stream *substream;
	uint32_t wflags, bytes_to_write = 0;
	unsigned long flags;
	bool eos_now = false;
	bool is_last_buffer = false;
	bool send_eos = false;
	bool notify_drain = false;
	bool fragment_elapsed = false;
	bool fatal_error = false;
	u64 new_completed;
	u64 new_copied;
	int ret;

	if (!kref_get_unless_zero(&prtd->refcount))
		return;

	spin_lock_irqsave(&prtd->lock, flags);
	if (prtd->closing || !prtd->cstream) {
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_dai_rtd_put(prtd);
		return;
	}
	substream = prtd->cstream;
	switch (opcode) {
	case APM_CLIENT_EVENT_CMD_EOS_DONE:
		/* STOP/close owns teardown; a late EOS must not revive a drain. */
		if (prtd->state != Q6APM_STREAM_RUNNING || prtd->closing) {
			prtd->drain_state = Q6APM_DRAIN_NONE;
			prtd->drain_notify_pending = false;
			break;
		}
		if (prtd->drain_state == Q6APM_DRAIN_EOS_SENT) {
			prtd->drain_state = Q6APM_DRAIN_NONE;
			if (prtd->next_track) {
				ret = q6apm_compr_finish_partial_drain(prtd);
				if (ret) {
					prtd->state = Q6APM_STREAM_STOPPED;
					fatal_error = true;
					break;
				}
				bytes_to_write = q6apm_compr_reserve_next(prtd,
									  &wflags,
									  &is_last_buffer);
				if (prtd->state != Q6APM_STREAM_RUNNING) {
					bytes_to_write = 0;
					fatal_error = true;
					break;
				}
			} else {
				ret = q6apm_compr_finish_full_drain(prtd);
				if (ret) {
					prtd->state = Q6APM_STREAM_STOPPED;
					fatal_error = true;
					break;
				}
			}
			notify_drain = true;
		} else {
			prtd->state = Q6APM_STREAM_STOPPED;
			fatal_error = true;
		}
		break;
	case APM_CLIENT_EVENT_DATA_WRITE_DONE:
		if (!bytes || prtd->bytes_sent < prtd->bytes_completed ||
		    bytes > prtd->bytes_sent - prtd->bytes_completed ||
		    prtd->user_bytes_received < prtd->copied_total ||
		    bytes > prtd->user_bytes_received - prtd->copied_total) {
			prtd->state = Q6APM_STREAM_STOPPED;
			prtd->drain_state = Q6APM_DRAIN_NONE;
			fatal_error = true;
			break;
		}
		if (check_add_overflow(prtd->bytes_completed, (u64)bytes,
				       &new_completed) ||
		    check_add_overflow(prtd->copied_total, (u64)bytes,
				       &new_copied)) {
			prtd->state = Q6APM_STREAM_STOPPED;
			prtd->drain_state = Q6APM_DRAIN_NONE;
			fatal_error = true;
			break;
		}
		prtd->bytes_completed = new_completed;
		prtd->copied_total = new_copied;
		fragment_elapsed = true;
		if (prtd->state != Q6APM_STREAM_RUNNING || prtd->closing ||
		    prtd->error_pending)
			break;
		bytes_to_write = q6apm_compr_reserve_next(prtd, &wflags,
							  &is_last_buffer);
		if (prtd->state != Q6APM_STREAM_RUNNING) {
			bytes_to_write = 0;
			prtd->drain_state = Q6APM_DRAIN_NONE;
			fatal_error = true;
			break;
		}
		send_eos = prtd->drain_state == Q6APM_DRAIN_REQUESTED &&
			is_last_buffer;
		eos_now = !bytes_to_write && q6apm_compr_drain_ready(prtd);
		break;
	default:
		break;
	}
	if (fatal_error)
		q6apm_compr_mark_error_locked(prtd);
	spin_unlock_irqrestore(&prtd->lock, flags);

	if (fatal_error)
		q6apm_compr_report_error(prtd);
	if (fragment_elapsed)
		snd_compr_fragment_elapsed(substream);
	if (bytes_to_write) {
		ret = q6apm_compr_submit(prtd, bytes_to_write, wflags,
					 send_eos, NULL);
		if (ret)
			dev_err(prtd->graph->dev,
				"compressed write continuation failed: %d\n", ret);
		if (ret) {
			q6apm_compr_report_error(prtd);
		} else if (notify_drain) {
			q6apm_compr_schedule_drain_notify(prtd);
		}
	} else if (eos_now) {
		ret = q6apm_compr_send_eos(prtd);
		if (ret)
			dev_err(prtd->graph->dev,
				"compressed drain EOS failed: %d\n", ret);
		if (ret)
			q6apm_compr_report_error(prtd);
	} else if (notify_drain) {
		q6apm_compr_schedule_drain_notify(prtd);
	}
	q6apm_dai_rtd_put(prtd);
}

static int q6apm_dai_finish_pcm_reset(struct q6apm_dai_rtd *prtd)
{
	bool flush_ready = false;
	int ret;

	if (prtd->graph_started) {
		WRITE_ONCE(prtd->state, Q6APM_STREAM_STOPPED);
		q6apm_graph_quiesce_callbacks(prtd->graph);
		ret = q6apm_graph_stop_with_state(prtd->graph, &flush_ready);
		if (ret)
			return ret;
		prtd->graph_started = false;
		prtd->reset_pending = true;
	}

	if (!prtd->reset_pending)
		return 0;

	if (flush_ready || q6apm_graph_request_flush(prtd->graph)) {
		ret = q6apm_graph_flush(prtd->graph);
		if (ret)
			return ret;
	} else {
		q6apm_graph_sync_flush_generation(prtd->graph);
		if (!q6apm_graph_try_reset_client(prtd->graph))
			return -EBUSY;
	}

	prtd->reset_pending = false;
	return 0;
}

static int q6apm_pcm_release_graph(struct q6apm_pcm_owner *owner)
{
	struct q6apm_dai_rtd *prtd;
	struct q6apm_graph *graph;
	bool drop_owner_ref = false;
	bool held = false;
	int ret;

	mutex_lock(&q6apm_pcm_owners_lock);
	prtd = owner->prtd;
	if (prtd)
		held = kref_get_unless_zero(&prtd->refcount);
	graph = prtd ? prtd->graph : NULL;
	mutex_unlock(&q6apm_pcm_owners_lock);

	if (!prtd || !held)
		return 0;
	if (!graph) {
		ret = -EIO;
		goto out_put;
	}

	ret = q6apm_dai_finish_pcm_reset(prtd);
	if (ret)
		goto out_put;
	if (prtd->fragments_allocated) {
		q6apm_graph_quiesce_callbacks(graph);
		ret = q6apm_free_fragments(graph, owner->stream);
		if (ret)
			goto out_put;
		prtd->fragments_allocated = false;
	}

	/* q6apm_graph_close() is the callback-owner retirement barrier. */
	ret = q6apm_graph_close(graph);
	if (ret)
		goto out_put;
	WRITE_ONCE(prtd->graph, NULL);

	mutex_lock(&q6apm_pcm_owners_lock);
		if (owner->prtd != prtd) {
			mutex_unlock(&q6apm_pcm_owners_lock);
			/* Another teardown path already consumed the association.  The graph
			 * is nevertheless retired, so only the temporary reference remains.
			 */
		ret = 0;
		goto out_put;
	}
	owner->prtd = NULL;
	if (owner->prtd_ref) {
		owner->prtd_ref = false;
		drop_owner_ref = true;
	}
	if (prtd->pcm_owner == owner)
		prtd->pcm_owner = NULL;
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (drop_owner_ref)
		q6apm_dai_rtd_put(prtd);
	ret = 0;

out_put:
	q6apm_dai_rtd_put(prtd);
	return ret;
}

static void q6apm_pcm_cleanup_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct q6apm_pcm_owner *owner = container_of(dwork,
						      struct q6apm_pcm_owner,
						      cleanup_work);
	struct snd_dma_buffer saved_dma = {};
	struct snd_pcm *pcm;
	bool destroy_owner = false;
	bool wait_for_pcm;
	bool pcm_gone;
	bool mapped;
	bool stopped;
	unsigned int map_generation;
	int ret;

	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->cleanup_stop) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		q6apm_pcm_cleanup_done(owner, false);
		return;
	}
	wait_for_pcm = false;
	pcm = NULL;
	if (!owner->pcm_gone) {
		if (owner->card)
			pcm = owner->pcm;
		else
			wait_for_pcm = true;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (wait_for_pcm) {
		ret = -EBUSY;
		goto retry;
	}
	if (pcm)
		mutex_lock(&pcm->open_mutex);

	/* pcm_free may have won the handoff while we waited for open_mutex. */
	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->cleanup_stop) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (pcm)
			mutex_unlock(&pcm->open_mutex);
		q6apm_pcm_cleanup_done(owner, false);
		return;
	}
	pcm_gone = owner->pcm_gone;
	mutex_unlock(&q6apm_pcm_owners_lock);

	ret = q6apm_pcm_release_graph(owner);
	if (ret)
		goto retry;

	mutex_lock(&q6apm_pcm_owners_lock);
	pcm_gone = owner->pcm_gone;
	mapped = owner->mapped;
	map_generation = owner->map_generation;
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (pcm_gone && mapped) {
		if (!map_generation) {
			ret = -EUCLEAN;
			goto retry;
		}
		ret = q6apm_unmap_memory_fixed_region(owner->apm,
						      owner->graph_id,
						      map_generation);
		if (ret)
			goto retry;
		mutex_lock(&q6apm_pcm_owners_lock);
		if (!owner->mapped || owner->map_generation != map_generation) {
			mutex_unlock(&q6apm_pcm_owners_lock);
			ret = -EAGAIN;
			goto retry;
		}
		owner->mapped = false;
		owner->map_generation = 0;
		mutex_unlock(&q6apm_pcm_owners_lock);
	}

	mutex_lock(&q6apm_pcm_owners_lock);
	/* pcm_free may request cancellation after the first stop check. */
	if (owner->cleanup_stop) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (pcm)
			mutex_unlock(&pcm->open_mutex);
		q6apm_pcm_cleanup_done(owner, false);
		return;
	}
	if (!owner->pcm_gone) {
		if (!owner->substream || !owner->buffer_saved) {
			ret = -EIO;
			mutex_unlock(&q6apm_pcm_owners_lock);
			goto retry;
		}
		owner->substream->dma_buffer = owner->saved_dma;
		memset(&owner->saved_dma, 0, sizeof(owner->saved_dma));
		owner->buffer_saved = false;
		owner->quarantined = false;
	} else {
		ret = q6apm_pcm_drop_accounting_locked(owner);
		if (ret) {
			mutex_unlock(&q6apm_pcm_owners_lock);
			goto retry;
		}
		saved_dma = owner->saved_dma;
		memset(&owner->saved_dma, 0, sizeof(owner->saved_dma));
		owner->buffer_saved = false;
		owner->cleanup_stop = true;
		destroy_owner = true;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (pcm)
		mutex_unlock(&pcm->open_mutex);
	if (saved_dma.area)
		snd_dma_free_pages(&saved_dma);
	q6apm_pcm_cleanup_done(owner, destroy_owner);
	return;

retry:
	if (pcm)
		mutex_unlock(&pcm->open_mutex);
	dev_warn_ratelimited(owner->dev,
			     "PCM close still owns DSP resources: %d\n", ret);
	mutex_lock(&q6apm_pcm_owners_lock);
	stopped = owner->cleanup_stop;
	if (!stopped)
		schedule_delayed_work(&owner->cleanup_work,
				      Q6APM_CLEANUP_RETRY_DELAY);
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (stopped)
		q6apm_pcm_cleanup_done(owner, false);
	/* A retry keeps the work reference; cancellation/terminal paths drop it. */
}

static int q6apm_pcm_quarantine_close(struct q6apm_dai_rtd *prtd,
				      struct snd_pcm_substream *substream)
{
	struct q6apm_pcm_owner *owner = NULL;
	struct q6apm_pcm_owner *candidate;
	struct q6apm_graph *graph;
	bool owner_ref = false;
	int ret;

	/* ALSA can release @substream as soon as close returns, including when
	 * registry recovery below fails.  Detach it before any fallible owner
	 * lookup, then wait for callbacks that may hold the old pointer.
	 */
	graph = READ_ONCE(prtd->graph);
	WRITE_ONCE(prtd->substream, NULL);
	if (graph)
		q6apm_graph_quiesce_callbacks(graph);

	/* Resolve the owner under its registry lock; prtd->pcm_owner is only a
	 * convenience backlink and may be stale during pcm_free teardown.
	 */
	mutex_lock(&q6apm_pcm_owners_lock);
	list_for_each_entry(candidate, &q6apm_pcm_owners, node) {
		if (candidate->prtd == prtd &&
		    candidate->substream == substream) {
			owner = candidate;
			kref_get(&owner->refcount);
			owner_ref = true;
			break;
		}
	}
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (!owner)
		return -ENODEV;
	ret = q6apm_pcm_pin_quarantine(owner);

	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->prtd != prtd || owner->pcm_gone) {
		ret = -EIO;
	} else {
		int save_ret = q6apm_pcm_save_buffer_locked(owner);

		if (save_ret)
			ret = save_ret;
		else
			owner->quarantined = true;
		/* A failed module pin is a deliberate leak-safe quarantine: the
		 * callback is detached, but no work containing module text is queued.
		 */
		owner->cleanup_stop = !!ret;
	}
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (ret && ret != -ENODEV) {
		if (owner_ref)
			q6apm_pcm_owner_put(owner);
		return ret;
	}

	if (!ret)
		q6apm_pcm_schedule_cleanup(owner, 0);
	if (owner_ref)
		q6apm_pcm_owner_put(owner);
	return ret;
}

static int q6apm_dai_prepare(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	struct audioreach_module_config cfg;
	struct device *dev = component->dev;
	struct q6apm_dai_data *pdata;
	int i, ret, reset_ret;

	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata)
		return -EINVAL;

	if (!prtd || !prtd->graph) {
		dev_err(dev, "%s: private data null or audio client freed\n", __func__);
		return -EINVAL;
	}

	cfg.direction = substream->stream;
	cfg.sample_rate = runtime->rate;
	cfg.num_channels = runtime->channels;
	cfg.bit_width = prtd->bits_per_sample;
	cfg.fmt = SND_AUDIOCODEC_PCM;
	audioreach_set_default_channel_mapping(cfg.channel_map, runtime->channels);
	ret = q6apm_dai_finish_pcm_reset(prtd);
	if (ret)
		return ret;
	if (prtd->fragments_allocated) {
		ret = q6apm_free_fragments(prtd->graph, substream->stream);
		if (ret) {
			dev_err(dev, "failed to retire old PCM fragments: %d\n", ret);
			return ret;
		}
		prtd->fragments_allocated = false;
	}

	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	/* rate and channels are sent to audio driver */
	ret = q6apm_graph_media_format_shmem(prtd->graph, &cfg);
	if (ret < 0) {
		dev_err(dev, "%s: q6apm_open_write failed\n", __func__);
		return ret;
	}

	ret = q6apm_graph_media_format_pcm(prtd->graph, &cfg);
	if (ret < 0) {
		dev_err(dev, "%s: CMD Format block failed\n", __func__);
		return ret;
	}

	ret = q6apm_alloc_fragments(prtd->graph, substream->stream, prtd->phys,
				(prtd->pcm_size / prtd->periods), prtd->periods);

	if (ret < 0) {
		dev_err(dev, "Audio Start: Buffer Allocation failed rc = %d\n",	ret);
		return ret;
	}
	prtd->fragments_allocated = true;

	ret = q6apm_graph_prepare(prtd->graph);
	if (ret) {
		dev_err(dev, "Failed to prepare Graph %d\n", ret);
		if (q6apm_dai_cmd_ambiguous(ret)) {
			prtd->reset_pending = true;
			return ret;
		}
		goto free_fragments;
	}

	ret = q6apm_graph_start(prtd->graph);
	if (ret) {
		dev_err(dev, "Failed to Start Graph %d\n", ret);
		if (q6apm_dai_cmd_ambiguous(ret)) {
			/* A lost response cannot prove that the DSP did not start. */
			prtd->graph_started = true;
			prtd->reset_pending = true;
			prtd->state = Q6APM_STREAM_STOPPED;
			return ret;
		}
		goto free_fragments;
	}
	prtd->graph_started = true;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		/* Queue the buffers for Capture ONLY after graph is started */
		for (i = 0; i < runtime->periods; i++) {
			ret = q6apm_read(prtd->graph);
			if (ret) {
				dev_err(dev, "Failed to queue capture buffer %d: %d\n",
					i, ret);
				goto stop_graph;
			}
		}

	}

	/* Now that graph as been prepared and started update the internal state accordingly */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		prtd->queue_ptr = READ_ONCE(runtime->control->appl_ptr);
	prtd->state = Q6APM_STREAM_RUNNING;

	return 0;

stop_graph:
	prtd->state = Q6APM_STREAM_STOPPED;
	reset_ret = q6apm_dai_finish_pcm_reset(prtd);
	if (reset_ret) {
		dev_err(dev, "Failed to quiesce graph during prepare rollback: %d\n",
			reset_ret);
		return ret;
	}
free_fragments:
	reset_ret = q6apm_free_fragments(prtd->graph, substream->stream);
	if (reset_ret) {
		dev_err(dev, "failed to retire PCM fragments during rollback: %d\n",
			reset_ret);
		return reset_ret;
	}
	prtd->fragments_allocated = false;
	return ret;
}

static int q6apm_pcm_pending_frames(const struct snd_pcm_runtime *runtime,
				    snd_pcm_uframes_t queue_ptr,
				    snd_pcm_uframes_t *pending)
{
	snd_pcm_uframes_t appl_ptr;

	if (!runtime->period_size || !runtime->boundary ||
	    runtime->period_size > runtime->boundary ||
	    queue_ptr >= runtime->boundary)
		return -EINVAL;

	appl_ptr = READ_ONCE(runtime->control->appl_ptr);
	if (appl_ptr >= runtime->boundary)
		return -EIO;
	if (appl_ptr >= queue_ptr)
		*pending = appl_ptr - queue_ptr;
	else
		*pending = runtime->boundary - queue_ptr + appl_ptr;
	if (*pending > runtime->buffer_size)
		return -EPIPE;

	return 0;
}

static int q6apm_dai_ack(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	snd_pcm_uframes_t pending;
	unsigned int avail_periods;
	int i, ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = q6apm_pcm_pending_frames(runtime, prtd->queue_ptr, &pending);
		if (ret)
			return ret;
		avail_periods = pending / runtime->period_size;
		for (i = 0; i < avail_periods; i++) {
			snd_pcm_uframes_t tail = runtime->boundary -
				prtd->queue_ptr;
			snd_pcm_uframes_t next_queue;

			if (runtime->period_size >= tail)
				next_queue = runtime->period_size - tail;
			else
				next_queue = prtd->queue_ptr + runtime->period_size;
			ret = q6apm_write_async(prtd->graph, prtd->pcm_count, 0, 0, NO_TIMESTAMP);
			if (ret < 0) {
				dev_err(component->dev, "Error queuing playback buffer %d\n", ret);
				return ret;
			}
			prtd->queue_ptr = next_queue;
		}
	}

	return ret;
}

static int q6apm_dai_trigger(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		/* TODO support be handled via SoftPause Module */
		prtd->state = Q6APM_STREAM_STOPPED;
		prtd->queue_ptr = 0;
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int q6apm_dai_open(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(soc_prtd, 0);
	struct device *dev = component->dev;
	struct q6apm_dai_data *pdata;
	struct q6apm_dai_rtd *prtd;
	int graph_id, ret;

	graph_id = cpu_dai->driver->id;

	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata) {
		dev_err(dev, "Drv data not found ..\n");
		return -EINVAL;
	}

	prtd = kzalloc_obj(*prtd);
	if (prtd == NULL)
		return -ENOMEM;
	kref_init(&prtd->refcount);

	spin_lock_init(&prtd->lock);
	prtd->substream = substream;
	ret = q6apm_pcm_claim_owner(substream, prtd);
	if (ret)
		goto err;
	prtd->graph = q6apm_graph_open(dev, event_handler, prtd, graph_id, substream->stream);
	if (IS_ERR(prtd->graph)) {
		dev_err(dev, "%s: Could not allocate memory\n", __func__);
		ret = PTR_ERR(prtd->graph);
		goto err;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		runtime->hw = q6apm_dai_hardware_playback;
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		runtime->hw = q6apm_dai_hardware_capture;

	/* Ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0) {
		dev_err(dev, "snd_pcm_hw_constraint_integer failed\n");
		goto err;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
						   BUFFER_BYTES_MIN, BUFFER_BYTES_MAX);
		if (ret < 0) {
			dev_err(dev, "constraint for buffer bytes min max ret = %d\n", ret);
			goto err;
		}
	}

	/* setup 10ms latency to accommodate DSP restrictions */
	ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 480);
	if (ret < 0) {
		dev_err(dev, "constraint for period bytes step ret = %d\n", ret);
		goto err;
	}

	ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 480);
	if (ret < 0) {
		dev_err(dev, "constraint for buffer bytes step ret = %d\n", ret);
		goto err;
	}

	runtime->private_data = prtd;
	runtime->dma_bytes = BUFFER_BYTES_MAX;
	if (pdata->sid < 0)
		prtd->phys = substream->dma_buffer.addr;
	else
		prtd->phys = substream->dma_buffer.addr | (pdata->sid << 32);

	return 0;
err:
	if (!IS_ERR_OR_NULL(prtd->graph))
		q6apm_graph_close(prtd->graph);
	q6apm_pcm_unclaim_owner(prtd);
	q6apm_dai_rtd_put(prtd);

	return ret;
}

static int q6apm_dai_close(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	int quarantine_ret;
	int ret;

	if (!prtd)
		return 0;

	ret = q6apm_dai_finish_pcm_reset(prtd);
	if (ret == -EBUSY) {
		ret = q6apm_graph_wait_for_client_idle(prtd->graph, msecs_to_jiffies(10000));
		if (!ret)
			ret = q6apm_dai_finish_pcm_reset(prtd);
	}
	if (ret) {
		dev_err(component->dev,
			"PCM DSP buffers did not quiesce; quarantining owner: %d\n",
			ret);
		quarantine_ret = q6apm_pcm_quarantine_close(prtd, substream);
		if (quarantine_ret)
			dev_crit(component->dev,
				 "failed to attach PCM quarantine owner: %d\n",
				 quarantine_ret);
		runtime->private_data = NULL;
		/* A successful quarantine transfers the owner association's kref to
		 * the cleanup path; release the ALSA/runtime reference now.  If the
		 * transfer failed, retain it rather than freeing a live callback target.
		 */
		if (!quarantine_ret)
			q6apm_dai_rtd_put(prtd);
		return 0;
	}
	/* Detach the ALSA callback target before waiting for graph retirement. */
	WRITE_ONCE(prtd->substream, NULL);
	q6apm_graph_quiesce_callbacks(prtd->graph);
	if (prtd->fragments_allocated) {
		ret = q6apm_free_fragments(prtd->graph, substream->stream);
		if (ret) {
			dev_err(component->dev,
				"PCM fragments did not retire; quarantining owner: %d\n",
				ret);
			quarantine_ret = q6apm_pcm_quarantine_close(prtd, substream);
			if (quarantine_ret)
				dev_crit(component->dev,
					 "failed to attach PCM quarantine owner: %d\n",
					quarantine_ret);
			runtime->private_data = NULL;
			if (!quarantine_ret)
				q6apm_dai_rtd_put(prtd);
			return 0;
		}
		prtd->fragments_allocated = false;
	}

	ret = q6apm_graph_close(prtd->graph);
	if (ret) {
		dev_err(component->dev,
			"PCM graph did not close; quarantining owner: %d\n", ret);
		quarantine_ret = q6apm_pcm_quarantine_close(prtd, substream);
		if (quarantine_ret)
			dev_crit(component->dev,
				 "failed to attach PCM quarantine owner: %d\n",
				quarantine_ret);
		runtime->private_data = NULL;
		if (!quarantine_ret)
			q6apm_dai_rtd_put(prtd);
		return 0;
	}
	prtd->graph = NULL;
	q6apm_pcm_unclaim_owner(prtd);
	q6apm_dai_rtd_put(prtd);
	runtime->private_data = NULL;

	return 0;
}

static snd_pcm_uframes_t q6apm_dai_pointer(struct snd_soc_component *component,
					   struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	snd_pcm_uframes_t ptr;

	ptr = q6apm_get_hw_pointer(prtd->graph, substream->stream) * runtime->period_size;
	if (ptr)
		return ptr - 1;

	return 0;
}

static int q6apm_dai_hw_params(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;

	prtd->pcm_size = params_buffer_bytes(params);
	prtd->periods = params_periods(params);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		prtd->bits_per_sample = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		prtd->bits_per_sample = 24;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int q6apm_dai_memory_map(struct snd_soc_component *component,
				struct snd_pcm_substream *substream, int graph_id,
				struct q6apm_pcm_owner *owner)
{
	struct q6apm_dai_data *pdata;
	struct device *dev = component->dev;
	phys_addr_t phys;
	unsigned int map_generation = 0;
	int ret;

	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata) {
		dev_err(component->dev, "Drv data not found ..\n");
		return -EINVAL;
	}

	if (pdata->sid < 0)
		phys = substream->dma_buffer.addr;
	else
		phys = substream->dma_buffer.addr | (pdata->sid << 32);

	ret = q6apm_map_memory_fixed_region(owner->apm, graph_id, phys,
					    BUFFER_BYTES_MAX,
					    &map_generation);
	if (map_generation) {
		mutex_lock(&q6apm_pcm_owners_lock);
		owner->mapped = true;
		owner->map_generation = map_generation;
		mutex_unlock(&q6apm_pcm_owners_lock);
	}
	if (!ret && !map_generation)
		ret = -EPROTO;
	if (ret < 0)
		dev_err(dev, "Audio Start: Buffer Allocation failed rc = %d\n",	ret);

	return ret;
}

static void
q6apm_pcm_abort_new(struct snd_soc_component *component,
		    struct q6apm_pcm_owner *owners[2])
{
	struct q6apm_pcm_owner *owner;
	bool mapped;
	int ret;
	int stream;

	for (stream = 0; stream < 2; stream++) {
		owner = owners[stream];
		if (!owner)
			continue;

		mutex_lock(&q6apm_pcm_owners_lock);
		mapped = owner->mapped;
		if (!mapped && owner->listed) {
			list_del_init(&owner->node);
			owner->listed = false;
		}
		mutex_unlock(&q6apm_pcm_owners_lock);

		if (!mapped) {
			q6apm_pcm_owner_put(owner);
			continue;
		}

		/*
		 * pcm_new() is failing, so ALSA may destroy the fixed buffer as
		 * soon as this callback returns.  Move confirmed or ambiguous
		 * DSP ownership into the private retirement path first.
		 */
		ret = q6apm_pcm_mark_gone(owner);
		if (ret)
			dev_crit(component->dev,
				 "failed to retain pcm_new DMA owner: %d\n", ret);
	}
}

static int q6apm_dai_pcm_new(struct snd_soc_component *component, struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_pcm *pcm = rtd->pcm;
	struct q6apm_pcm_owner *owners[2] = {};
	struct q6apm_pcm_owner *owner;
	/*
	 * Allocate one extra page as a workaround for a DSP bug where 32-bit
	 * address arithmetic can overflow when the buffer is placed near the
	 * end of the addressable range.
	 */
	int size = BUFFER_BYTES_MAX + PAGE_SIZE;
	int graph_id, ret;
	struct snd_pcm_substream *substream;

	graph_id = cpu_dai->driver->id;

	ret = snd_pcm_set_fixed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, component->dev, size);
	if (ret)
		return ret;

	/* Note: DSP backend dais are uni-directional ONLY(either playback or capture) */
	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		substream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
		owner = q6apm_pcm_create_owner(component, substream, graph_id);
		if (IS_ERR(owner)) {
			ret = PTR_ERR(owner);
			goto abort;
		}
		owners[SNDRV_PCM_STREAM_PLAYBACK] = owner;
		ret = q6apm_dai_memory_map(component, substream, graph_id,
					   owner);
		if (ret)
			goto abort;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		substream = pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
		owner = q6apm_pcm_create_owner(component, substream, graph_id);
		if (IS_ERR(owner)) {
			ret = PTR_ERR(owner);
			goto abort;
		}
		owners[SNDRV_PCM_STREAM_CAPTURE] = owner;
		ret = q6apm_dai_memory_map(component, substream, graph_id,
					   owner);
		if (ret)
			goto abort;
	}

	return 0;

abort:
	q6apm_pcm_abort_new(component, owners);
	return ret;
}

static int q6apm_pcm_mark_gone(struct q6apm_pcm_owner *owner)
{
	struct q6apm_dai_rtd *prtd;
	struct snd_pcm_substream *substream;
	struct q6apm_graph *graph;
	bool prtd_ref = false;
	int pin_ret;
	int accounting_ret;
	int ret;

	/* Pin before publishing a deferred owner.  A failed pin uses the
	 * synchronous leak-safe path below and never queues module code.
	 */
	pin_ret = q6apm_pcm_pin_quarantine(owner);
	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->pcm_gone) {
		if (pin_ret)
			owner->cleanup_stop = true;
		else
			owner->cleanup_stop = false;
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (!pin_ret)
			q6apm_pcm_schedule_cleanup(owner, 0);
		return pin_ret;
	}
	prtd = owner->prtd;
	substream = owner->substream;
	if (prtd && !kref_get_unless_zero(&prtd->refcount)) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		return -EAGAIN;
	}
	prtd_ref = !!prtd;
	mutex_unlock(&q6apm_pcm_owners_lock);

	if (prtd && substream) {
		WRITE_ONCE(prtd->substream, NULL);
		graph = READ_ONCE(prtd->graph);
		if (graph)
			q6apm_graph_quiesce_callbacks(graph);
	}

	mutex_lock(&q6apm_pcm_owners_lock);
	if (owner->prtd != prtd || owner->substream != substream) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (prtd_ref)
			q6apm_dai_rtd_put(prtd);
		return -EAGAIN;
	}
	ret = q6apm_pcm_save_buffer_locked(owner);
	if (ret) {
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (prtd_ref)
			q6apm_dai_rtd_put(prtd);
		return ret;
	}
	if (pin_ret) {
		/* The descriptor is hidden, but without a module reference there is
		 * no safe worker that can retire it.  Keep accounting and the owner
		 * registry intact; this unreachable invariant-violation path leaks
		 * rather than exposing a potentially DSP-owned buffer to ALSA.
		 */
		owner->quarantined = true;
		owner->pcm_gone = true;
		owner->cleanup_stop = true;
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (prtd_ref)
			q6apm_dai_rtd_put(prtd);
		return pin_ret;
	}
	accounting_ret = q6apm_pcm_drop_accounting_locked(owner);
	if (accounting_ret) {
		/* Accounting failure must remain fail-closed: the saved descriptor is
		 * already hidden from ALSA, so never restore it while DSP ownership or
		 * retirement state is unresolved.  Keep the owner discoverable and let
		 * the retry path repair accounting before releasing the DMA.
		 */
		owner->quarantined = true;
		owner->pcm_gone = true;
		owner->cleanup_stop = !!pin_ret;
		mutex_unlock(&q6apm_pcm_owners_lock);
		if (!pin_ret)
			q6apm_pcm_schedule_cleanup(owner, 0);
		if (prtd_ref)
			q6apm_dai_rtd_put(prtd);
		return accounting_ret;
	}
	owner->quarantined = true;
	owner->pcm_gone = true;
	/* Keep the persistent PCM/substream identity until pcm_free().  It is
	 * the registry key that lets ALSA hand off an unretired owner safely.
	 */
	owner->cleanup_stop = !!pin_ret;
	mutex_unlock(&q6apm_pcm_owners_lock);
	if (pin_ret) {
		if (prtd_ref)
			q6apm_dai_rtd_put(prtd);
		return pin_ret;
	}

	q6apm_pcm_schedule_cleanup(owner, 0);
	if (prtd_ref)
		q6apm_dai_rtd_put(prtd);
	return 0;
}

static void q6apm_dai_pcm_free(struct snd_soc_component *component, struct snd_pcm *pcm)
{
	struct q6apm_pcm_owner *owner;
	struct snd_pcm_substream *substream;
	bool needs_quarantine;
	bool mapped;
	bool drop_registry;
	unsigned int map_generation;
	int ret;
	int stream;

	for (stream = 0; stream < 2; stream++) {
		/*
		 * Stop and join a quarantine worker before taking open_mutex.  The
		 * worker takes open_mutex while it is handing a live PCM buffer back;
		 * cancel_delayed_work_sync() while holding that mutex would deadlock
		 * if the worker is already waiting for it.
		 */
		mutex_lock(&pcm->open_mutex);
		substream = pcm->streams[stream].substream;
		if (!substream) {
			mutex_unlock(&pcm->open_mutex);
			continue;
		}

		mutex_lock(&q6apm_pcm_owners_lock);
		owner = q6apm_pcm_find_owner_locked(substream);
		if (!owner) {
			/*
			 * Only a registered owner can have submitted a map.
			 * This is the expected rollback state when owner
			 * creation failed before q6apm_dai_memory_map().
			 */
			mutex_unlock(&q6apm_pcm_owners_lock);
			mutex_unlock(&pcm->open_mutex);
			continue;
		}
		/* Keep the owner alive across cancellation and the second lock phase. */
		kref_get(&owner->refcount);
		/* Prevent a running/pending worker from touching the PCM while the
		 * core is about to destroy it.
		 */
		owner->cleanup_stop = true;
		mapped = owner->mapped;
		map_generation = owner->map_generation;
		needs_quarantine = owner->quarantined || owner->prtd;
		mutex_unlock(&q6apm_pcm_owners_lock);
		mutex_unlock(&pcm->open_mutex);
		q6apm_pcm_cancel_cleanup(owner);

		/*
		 * The owner is now stopped from scheduling cleanup work.  Keep the
		 * PCM stable while deciding whether to hand its buffer back or move
		 * the owner into the private post-PCM quarantine path.
		 */
		mutex_lock(&pcm->open_mutex);

		if (needs_quarantine) {
			ret = q6apm_pcm_mark_gone(owner);
			if (ret)
				dev_crit(component->dev,
					 "failed to retain quarantined PCM buffer: %d\n",
					 ret);
			goto out_unlock_owner;
		}

		if (mapped)
			ret = q6apm_unmap_memory_fixed_region(owner->apm,
							      owner->graph_id,
							      map_generation);
		else
			ret = 0;
		if (ret) {
			dev_err(component->dev,
				"PCM unmap uncertain; retaining DMA buffer: %d\n",
				ret);
			ret = q6apm_pcm_mark_gone(owner);
			if (ret)
				dev_crit(component->dev,
					 "failed to attach PCM unmap quarantine: %d\n",
					 ret);
			goto out_unlock_owner;
		}

		mutex_lock(&q6apm_pcm_owners_lock);
		if (!owner->listed || owner->pcm != pcm ||
		    owner->substream != substream || owner->mapped != mapped ||
		    owner->map_generation != map_generation ||
		    owner->quarantined || owner->prtd) {
			mutex_unlock(&q6apm_pcm_owners_lock);
			dev_err(component->dev,
				"PCM owner changed during teardown; retaining it\n");
			goto out_unlock_pcm;
		}
		/* The authenticated UNMAP above retired the snapshot represented by
		 * @mapped.  Clear it only after proving no owner transition raced the
		 * command; otherwise keep the registry entry fail-closed.
		 */
		owner->mapped = false;
		owner->map_generation = 0;
		owner->cleanup_stop = true;
		drop_registry = owner->listed;
		if (drop_registry) {
			list_del_init(&owner->node);
			owner->listed = false;
		}
		mutex_unlock(&q6apm_pcm_owners_lock);
		mutex_unlock(&pcm->open_mutex);
		q6apm_pcm_owner_put(owner); /* temporary handoff reference */
		if (drop_registry)
			q6apm_pcm_owner_put(owner); /* registry reference */
		continue;

out_unlock_owner:
		mutex_unlock(&pcm->open_mutex);
		q6apm_pcm_owner_put(owner);
		continue;

out_unlock_pcm:
		mutex_unlock(&pcm->open_mutex);
		q6apm_pcm_owner_put(owner);
	}
}

static int q6apm_dai_compr_open(struct snd_soc_component *component,
				struct snd_compr_stream *stream)
{
	struct snd_soc_pcm_runtime *rtd = stream->private_data;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd;
	struct q6apm_dai_data *pdata;
	struct device *dev = component->dev;
	int ret;
	int graph_id;

	graph_id = cpu_dai->driver->id;
	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata)
		return -EINVAL;

	prtd = kzalloc_obj(*prtd);
	if (prtd == NULL)
		return -ENOMEM;
	kref_init(&prtd->refcount);

	if (!try_module_get(THIS_MODULE)) {
		ret = -ENODEV;
		goto free_prtd;
	}
	prtd->module_pinned = true;
	prtd->dev = get_device(component->dev);
	if (!prtd->dev) {
		ret = -ENODEV;
		goto put_module;
	}
	prtd->apm = q6apm_get_apm(component->dev);
	if (!prtd->apm) {
		ret = -ENODEV;
		goto put_module;
	}
	prtd->cstream = stream;
	spin_lock_init(&prtd->lock);
	INIT_WORK(&prtd->error_work, q6apm_compr_error_work);
	INIT_DELAYED_WORK(&prtd->drain_work, q6apm_compr_drain_work);
	INIT_DELAYED_WORK(&prtd->cleanup_work, q6apm_compr_cleanup_work);
	prtd->graph = q6apm_graph_open(dev, event_handler_compr, prtd, graph_id,
						SNDRV_PCM_STREAM_PLAYBACK);
	if (IS_ERR(prtd->graph)) {
		ret = PTR_ERR(prtd->graph);
		goto put_module;
	}

	ret = q6apm_enable_compress_module(dev, prtd->graph, true);
	if (ret)
		goto close_graph;

	runtime->private_data = prtd;
	return 0;

close_graph:
	q6apm_graph_close(prtd->graph);
put_module:
	if (prtd->apm)
		q6apm_put_apm(prtd->apm);
	if (prtd->dev)
		put_device(prtd->dev);
	if (prtd->module_pinned)
		module_put(THIS_MODULE);
free_prtd:
	q6apm_dai_rtd_put(prtd);
	return ret;
}

static int q6apm_dai_compr_free(struct snd_soc_component *component,
				struct snd_compr_stream *stream)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	bool schedule_cleanup = false;
	int ret;
	unsigned long flags;

	if (!prtd)
		return 0;

	spin_lock_irqsave(&prtd->lock, flags);
	prtd->closing = true;
	prtd->state = Q6APM_STREAM_STOPPED;
	/* The ALSA core frees @stream immediately after this callback returns. */
	prtd->cstream = NULL;
	spin_unlock_irqrestore(&prtd->lock, flags);
	/* No callback may enqueue work after this barrier. */
	if (prtd->graph)
		q6apm_graph_quiesce_callbacks(prtd->graph);
	q6apm_compr_cancel_error_work(prtd);
	q6apm_compr_cancel_drain_work(prtd);
	q6apm_compr_cancel_cleanup_work(prtd);
	cancel_delayed_work_sync(&stream->error_work);

	/* Detach the runtime before any asynchronous quarantine retry is queued. */
	runtime->private_data = NULL;
	ret = q6apm_compr_try_release(prtd);
	if (ret) {
		spin_lock_irqsave(&prtd->lock, flags);
		prtd->quarantined = true;
		schedule_cleanup = true;
		spin_unlock_irqrestore(&prtd->lock, flags);
		dev_warn(component->dev,
			 "compressed close deferred while DSP resources remain owned: %d\n",
			 ret);
	}

	if (schedule_cleanup) {
		q6apm_compr_queue_cleanup_work(prtd, 0);
		return 0;
	}

	ret = q6apm_compr_release_owner(prtd);
	if (ret) {
		spin_lock_irqsave(&prtd->lock, flags);
		prtd->quarantined = true;
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_compr_queue_cleanup_work(prtd, 0);
		return 0;
	}
	q6apm_dai_rtd_put(prtd);

	return 0;
}

static int q6apm_dai_compr_get_caps(struct snd_soc_component *component,
				    struct snd_compr_stream *stream,
				    struct snd_compr_caps *caps)
{
	caps->direction = SND_COMPRESS_PLAYBACK;
	caps->min_fragment_size = COMPR_PLAYBACK_MIN_FRAGMENT_SIZE;
	caps->max_fragment_size = COMPR_PLAYBACK_MAX_FRAGMENT_SIZE;
	caps->min_fragments = COMPR_PLAYBACK_MIN_NUM_FRAGMENTS;
	caps->max_fragments = COMPR_PLAYBACK_MAX_NUM_FRAGMENTS;
	caps->num_codecs = 4;
	caps->codecs[0] = SND_AUDIOCODEC_MP3;
	caps->codecs[1] = SND_AUDIOCODEC_AAC;
	caps->codecs[2] = SND_AUDIOCODEC_FLAC;
	caps->codecs[3] = SND_AUDIOCODEC_OPUS_RAW;

	return 0;
}

static int q6apm_dai_compr_get_codec_caps(struct snd_soc_component *component,
					  struct snd_compr_stream *stream,
					  struct snd_compr_codec_caps *codec)
{
	switch (codec->codec) {
	case SND_AUDIOCODEC_MP3:
		*codec = q6apm_compr_caps;
		break;
	default:
		break;
	}

	return 0;
}

static int q6apm_dai_compr_pointer(struct snd_soc_component *component,
				   struct snd_compr_stream *stream,
				   struct snd_compr_tstamp64 *tstamp)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	u64 temp_bytes_completed;

	guard(spinlock_irqsave)(&prtd->lock);
	if (!prtd->pcm_size)
		return -EINVAL;
	tstamp->copied_total = prtd->copied_total;
	temp_bytes_completed = prtd->bytes_completed;
	tstamp->byte_offset = do_div(temp_bytes_completed, prtd->pcm_size);

	return 0;
}

static int q6apm_dai_compr_trigger(struct snd_soc_component *component,
					struct snd_compr_stream *stream, int cmd)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	u32 bytes_to_write = 0, wflags = 0;
	unsigned long flags;
	unsigned long idle_timeout = msecs_to_jiffies(10000);
	bool is_last_buffer = false;
	bool send_eos = false;
	bool graph_stopped = false;
	bool generation_changed = false;
	bool transport_error = false;
	bool was_running;
	bool rearm_pending;
	bool core_next_track;
	bool partial;
	u64 next_start;
	int ret = 0, stop_ret, reset_ret, wait_ret;

	core_next_track = READ_ONCE(stream->next_track);
	if (cmd != SNDRV_PCM_TRIGGER_STOP &&
	    cmd != SNDRV_PCM_TRIGGER_SUSPEND &&
	    cmd != SNDRV_PCM_TRIGGER_PAUSE_PUSH &&
	    q6apm_compr_error_is_pending(prtd)) {
		q6apm_compr_operation_error_locked(prtd);
		return -EIO;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&prtd->lock, flags);
		was_running = prtd->graph_started;
		rearm_pending = prtd->rearm_pending;
		spin_unlock_irqrestore(&prtd->lock, flags);
		ret = q6apm_compr_finish_stop_reset(prtd);
		if (ret) {
			transport_error = true;
			break;
		}

		if (!was_running) {
			ret = q6apm_graph_start(prtd->graph);
			if (ret) {
				transport_error = true;
				break;
			}
		} else if (rearm_pending) {
			ret = q6apm_graph_rearm(prtd->graph);
			if (ret) {
				transport_error = true;
				break;
			}
		}
		/* Check peer flushes for both a fresh and an already-started graph. */
		generation_changed =
			q6apm_graph_sync_flush_generation(prtd->graph);
		spin_lock_irqsave(&prtd->lock, flags);
		prtd->graph_started = true;
		prtd->rearm_pending = false;
		if (generation_changed && !prtd->stop_reset) {
			prtd->state = Q6APM_STREAM_STOPPED;
			ret = -EIO;
		} else {
			prtd->stop_reset = false;
			prtd->state = Q6APM_STREAM_RUNNING;
			bytes_to_write = q6apm_compr_reserve_next(prtd, &wflags,
								  &is_last_buffer);
			if (prtd->state != Q6APM_STREAM_RUNNING)
				ret = -EIO;
			send_eos = prtd->drain_state == Q6APM_DRAIN_REQUESTED &&
				is_last_buffer;
		}
		spin_unlock_irqrestore(&prtd->lock, flags);
		if (!ret && bytes_to_write) {
			wflags |= NO_TIMESTAMP;
			ret = q6apm_compr_submit(prtd, bytes_to_write, wflags,
						 send_eos, NULL);
		}
		if (ret) {
			transport_error = true;
			stop_ret = q6apm_graph_stop_with_state(prtd->graph, &graph_stopped);
			if (!stop_ret) {
				spin_lock_irqsave(&prtd->lock, flags);
				prtd->graph_started = false;
				prtd->reset_pending = true;
				spin_unlock_irqrestore(&prtd->lock, flags);
				if (graph_stopped) {
					stop_ret = q6apm_graph_flush(prtd->graph);
					q6apm_compr_set_flush_retry(prtd, stop_ret != 0);
					if (stop_ret)
						dev_err(component->dev,
							"failed to flush compressed graph after START rollback: %d\n",
							stop_ret);
				}
			}
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&prtd->lock, flags);
		was_running = prtd->graph_started;
		prtd->state = Q6APM_STREAM_STOPPED;
		spin_unlock_irqrestore(&prtd->lock, flags);
		if (was_running) {
			q6apm_graph_quiesce_callbacks(prtd->graph);
			if (cmd == SNDRV_PCM_TRIGGER_STOP)
				ret = q6apm_graph_stop_with_state(prtd->graph, &graph_stopped);
			else
				ret = q6apm_graph_stop(prtd->graph);
			if (ret) {
				transport_error = true;
				break;
			}
			spin_lock_irqsave(&prtd->lock, flags);
			prtd->graph_started = false;
			spin_unlock_irqrestore(&prtd->lock, flags);
		}
		if (cmd == SNDRV_PCM_TRIGGER_STOP) {
			spin_lock_irqsave(&prtd->lock, flags);
			prtd->drain_state = Q6APM_DRAIN_NONE;
			prtd->reset_pending = true;
			spin_unlock_irqrestore(&prtd->lock, flags);
			if (!graph_stopped)
				graph_stopped = q6apm_graph_request_flush(prtd->graph);
			if (graph_stopped || q6apm_graph_flush_pending(prtd->graph)) {
				if (!graph_stopped)
					graph_stopped = q6apm_graph_request_flush(prtd->graph);
				if (!graph_stopped) {
					ret = -EBUSY;
					transport_error = true;
					break;
				}
				ret = q6apm_graph_flush(prtd->graph);
				if (ret) {
					q6apm_compr_set_flush_retry(prtd, true);
					transport_error = true;
					break;
				}
				q6apm_compr_set_flush_retry(prtd, false);
			}
			reset_ret = q6apm_compr_finish_stop_reset(prtd);
			if (reset_ret == -EBUSY) {
				wait_ret = q6apm_graph_wait_for_client_idle(prtd->graph,
									    idle_timeout);
				if (!wait_ret)
					reset_ret = q6apm_compr_finish_stop_reset(prtd);
			}
			if (reset_ret) {
				ret = reset_ret;
				transport_error = true;
				break;
			}
			q6apm_compr_complete_stop(prtd, stream);
		}
		break;
	case SND_COMPR_TRIGGER_NEXT_TRACK:
		spin_lock_irqsave(&prtd->lock, flags);
		if (core_next_track != prtd->next_track) {
			ret = -EIO;
			transport_error = true;
		} else if (core_next_track) {
			ret = -EBUSY;
		} else if (prtd->state != Q6APM_STREAM_RUNNING) {
			ret = -EPERM;
		} else if (!prtd->pcm_count ||
			   prtd->bytes_received < prtd->bytes_completed ||
			   prtd->user_bytes_received < prtd->copied_total) {
			ret = -EIO;
			transport_error = true;
		} else {
			ret = q6apm_compr_round_up(prtd->bytes_received,
						   prtd->pcm_count, &next_start);
			if (!ret) {
				prtd->gapless_track_end = prtd->bytes_received;
				prtd->gapless_next_start = next_start;
				prtd->next_track = true;
				prtd->next_track_data = true;
			}
		}
		spin_unlock_irqrestore(&prtd->lock, flags);
		break;
	case SND_COMPR_TRIGGER_DRAIN:
	case SND_COMPR_TRIGGER_PARTIAL_DRAIN:
		partial = cmd == SND_COMPR_TRIGGER_PARTIAL_DRAIN;
		spin_lock_irqsave(&prtd->lock, flags);
		if (prtd->state != Q6APM_STREAM_RUNNING) {
			ret = -EPERM;
		} else if (prtd->drain_state != Q6APM_DRAIN_NONE) {
			ret = -EBUSY;
		} else if (core_next_track != prtd->next_track ||
			   partial != core_next_track) {
			ret = -EIO;
			transport_error = true;
		} else {
			prtd->drain_state = Q6APM_DRAIN_REQUESTED;
			bytes_to_write = q6apm_compr_reserve_next(prtd, &wflags,
								  &is_last_buffer);
			if (prtd->state != Q6APM_STREAM_RUNNING) {
				prtd->drain_state = Q6APM_DRAIN_NONE;
				ret = -EIO;
				transport_error = true;
			} else {
				send_eos = !bytes_to_write &&
					q6apm_compr_drain_ready(prtd);
			}
		}
		spin_unlock_irqrestore(&prtd->lock, flags);
		if (!ret && bytes_to_write)
			ret = q6apm_compr_submit(prtd, bytes_to_write, wflags,
						 is_last_buffer, NULL);
		else if (!ret && send_eos)
			ret = q6apm_compr_send_eos(prtd);
		if (ret)
			transport_error = true;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (transport_error) {
		if (cmd == SND_COMPR_TRIGGER_PARTIAL_DRAIN) {
			spin_lock_irqsave(&prtd->lock, flags);
			q6apm_compr_clear_gapless(prtd);
			spin_unlock_irqrestore(&prtd->lock, flags);
			stream->partial_drain = false;
			stream->next_track = false;
		}
		/* Trigger callbacks run under the compressed-device mutex.  Publish
		 * XRUN synchronously for every operation-context transport failure,
		 * including STOP/flush, so no later operation can enter before the
		 * private stopped/error state is visible to ALSA.
		 */
		q6apm_compr_operation_error_locked(prtd);
	}

	return ret;
}

static int q6apm_dai_compr_set_params(struct snd_soc_component *component,
					      struct snd_compr_stream *stream,
					      struct snd_compr_params *params)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	struct q6apm_dai_data *pdata;
	struct audioreach_module_config cfg = {};
	struct snd_codec *codec = &params->codec;
	size_t ring_bytes;
	int dir = stream->direction;
	int cleanup_ret;
	int ret;
	bool core_next_track;
	bool private_next_track;

	if (!prtd)
		return -EINVAL;
	prtd->params_old_fragment_size = runtime->fragment_size;
	prtd->params_old_fragments = runtime->fragments;
	prtd->params_old_buffer_size = runtime->buffer_size;
	prtd->params_snapshot_valid = true;

	/* The core has already installed the requested geometry at this point. */
	core_next_track = READ_ONCE(stream->next_track);
	spin_lock_irq(&prtd->lock);
	private_next_track = prtd->next_track;
	spin_unlock_irq(&prtd->lock);
	if (core_next_track != private_next_track) {
		q6apm_compr_stop_error_locked(prtd);
		return q6apm_compr_params_error(stream, prtd, core_next_track,
						-EIO);
	}
	pdata = snd_soc_component_get_drvdata(component);
	if (q6apm_compr_error_is_pending(prtd)) {
		q6apm_compr_stop_error_locked(prtd);
		return q6apm_compr_params_error(stream, prtd, core_next_track,
						-EIO);
	}
	if (!pdata)
		return q6apm_compr_params_error(stream, prtd, core_next_track,
					-EINVAL);
	ret = q6apm_compr_validate_geometry(runtime, &ring_bytes);
	if (ret)
		return q6apm_compr_params_error(stream, prtd, core_next_track, ret);
	ret = q6apm_compr_finish_stop_reset(prtd);
	if (ret)
		return q6apm_compr_params_error(stream, prtd, core_next_track, ret);

	if (core_next_track &&
	    (runtime->fragments != prtd->periods ||
	     runtime->fragment_size != prtd->pcm_count ||
	     ring_bytes != prtd->pcm_size))
		return q6apm_compr_params_error(stream, prtd, core_next_track,
					-EINVAL);

	if (!core_next_track) {
		if (prtd->graph_started)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						  -EBUSY);
		if (prtd->fragments_allocated) {
			ret = q6apm_free_fragments(prtd->graph,
						   SNDRV_PCM_STREAM_PLAYBACK);
			if (ret)
				return q6apm_compr_params_error(stream, prtd,
							       core_next_track, ret);
			prtd->fragments_allocated = false;
		}
		ret = q6apm_compr_free_dma(prtd);
		if (ret)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						ret);

		ret = q6apm_set_real_module_id(component->dev, prtd->graph, codec->id);
		if (ret)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						ret);

		cfg.direction = dir;
		cfg.sample_rate = codec->sample_rate;
		cfg.num_channels = 2;
		cfg.bit_width = 16;
		cfg.fmt = codec->id;
		audioreach_set_default_channel_mapping(cfg.channel_map,
						       cfg.num_channels);
		memcpy(&cfg.codec, codec, sizeof(*codec));

		ret = q6apm_graph_media_format_shmem(prtd->graph, &cfg);
		if (ret < 0)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						ret);

		ret = q6apm_graph_media_format_pcm(prtd->graph, &cfg);
		if (ret)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						ret);

		ret = q6apm_compr_alloc_dma(component, prtd, pdata, ring_bytes);
		if (ret)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						ret);

		ret = q6apm_alloc_fragments(prtd->graph, SNDRV_PCM_STREAM_PLAYBACK,
						prtd->phys, runtime->fragment_size,
						runtime->fragments);
		if (ret < 0)
			goto free_dma;
		prtd->fragments_allocated = true;

		ret = q6apm_graph_prepare(prtd->graph);
		if (ret) {
			if (q6apm_dai_cmd_ambiguous(ret)) {
				/* Keep mapped DMA until the ambiguous prepare is resolved. */
				prtd->reset_pending = true;
				prtd->state = Q6APM_STREAM_STOPPED;
				return q6apm_compr_params_error(stream, prtd,
							       core_next_track, ret);
			}
			goto free_fragments;
		}

		memcpy(&prtd->codec, codec, sizeof(*codec));
		prtd->periods = runtime->fragments;
		prtd->pcm_count = runtime->fragment_size;
		prtd->pcm_size = ring_bytes;
		prtd->bits_per_sample = 16;
		prtd->stop_reset = true;
		prtd->state = Q6APM_STREAM_STOPPED;
	} else {
		cfg.direction = dir;
		cfg.sample_rate = codec->sample_rate;
		cfg.num_channels = 2;
		cfg.bit_width = prtd->bits_per_sample;
		cfg.fmt = codec->id;
		memcpy(&cfg.codec, codec, sizeof(*codec));

		ret = audioreach_compr_set_param(prtd->graph,  &cfg);
		if (ret < 0)
			return q6apm_compr_params_error(stream, prtd, core_next_track,
						ret);
	}
	prtd->params_snapshot_valid = false;
	return 0;

free_fragments:
	cleanup_ret = q6apm_free_fragments(prtd->graph,
					   SNDRV_PCM_STREAM_PLAYBACK);
	if (cleanup_ret) {
		prtd->reset_pending = true;
		return q6apm_compr_params_error(stream, prtd, core_next_track,
						cleanup_ret);
	}
	prtd->fragments_allocated = false;
free_dma:
	cleanup_ret = q6apm_compr_free_dma(prtd);
	if (cleanup_ret)
		return q6apm_compr_params_error(stream, prtd, core_next_track,
						cleanup_ret);
	return q6apm_compr_params_error(stream, prtd, core_next_track, ret);
}

static int q6apm_dai_compr_set_metadata(struct snd_soc_component *component,
					struct snd_compr_stream *stream,
					struct snd_compr_metadata *metadata)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	int ret = 0;

	if (q6apm_compr_error_is_pending(prtd)) {
		q6apm_compr_stop_error_locked(prtd);
		return -EIO;
	}

	switch (metadata->key) {
	case SNDRV_COMPRESS_ENCODER_PADDING:
		ret = q6apm_remove_trailing_silence(component->dev, prtd->graph,
						    metadata->value[0]);
		break;
	case SNDRV_COMPRESS_ENCODER_DELAY:
		ret = q6apm_remove_initial_silence(component->dev, prtd->graph,
						   metadata->value[0]);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int q6apm_compr_copy(struct snd_soc_component *component,
			    struct snd_compr_stream *stream, char __user *buf,
			    size_t count)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6apm_dai_rtd *prtd = runtime->private_data;
	void *dstn;
	size_t copy;
	u32 wflags = 0, bytes_to_write = 0;
	u32 app_pointer;
	unsigned long flags;
	u64 copy_start;
	u64 new_user_bytes_received;
	u64 new_bytes_received;
	u64 old_bytes_received;
	u64 old_user_bytes_received;
	u64 outstanding;
	u64 padding;
	u64 private_avail;
	u64 temp_copy_start;
	enum stream_state old_state;
	bool old_next_track_data;
	bool is_last_buffer = false;
	bool send_eos = false;
	bool submitted = false;
	int ret;

	if (q6apm_compr_error_is_pending(prtd)) {
		q6apm_compr_stop_error_locked(prtd);
		return -EIO;
	}
	ret = q6apm_compr_finish_stop_reset(prtd);
	if (ret)
		return ret;

	if (!prtd->pcm_size || !prtd->dma_buffer.area ||
	    prtd->dma_buffer.bytes < prtd->pcm_size || count > prtd->pcm_size)
		return -EINVAL;

	spin_lock_irqsave(&prtd->lock, flags);
	if (prtd->bytes_received < prtd->bytes_completed ||
	    prtd->user_bytes_received < prtd->copied_total) {
		prtd->state = Q6APM_STREAM_STOPPED;
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_compr_stop_error_locked(prtd);
		return -EIO;
	}

	old_bytes_received = prtd->bytes_received;
	old_user_bytes_received = prtd->user_bytes_received;
	old_state = prtd->state;
	old_next_track_data = prtd->next_track_data;
	copy_start = old_bytes_received;
	/* Keep DSP-only fragment padding out of user-visible byte totals. */
	if (old_next_track_data) {
		if (!prtd->next_track ||
		    prtd->gapless_next_start < copy_start) {
			prtd->state = Q6APM_STREAM_STOPPED;
			spin_unlock_irqrestore(&prtd->lock, flags);
			q6apm_compr_stop_error_locked(prtd);
			return -EIO;
		}
		copy_start = prtd->gapless_next_start;
	}

	outstanding = old_bytes_received - prtd->bytes_completed;
	padding = copy_start - old_bytes_received;
	if (outstanding > prtd->pcm_size) {
		prtd->state = Q6APM_STREAM_STOPPED;
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_compr_stop_error_locked(prtd);
		return -EIO;
	}
	if (padding > prtd->pcm_size - outstanding) {
		spin_unlock_irqrestore(&prtd->lock, flags);
		return 0;
	}
	private_avail = prtd->pcm_size - outstanding - padding;
	count = min_t(u64, count, private_avail);
	if (!count) {
		spin_unlock_irqrestore(&prtd->lock, flags);
		return 0;
	}
	if (check_add_overflow(copy_start, (u64)count,
			       &new_bytes_received) ||
	    check_add_overflow(old_user_bytes_received, (u64)count,
			       &new_user_bytes_received)) {
		prtd->state = Q6APM_STREAM_STOPPED;
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_compr_stop_error_locked(prtd);
		return -EOVERFLOW;
	}
	temp_copy_start = copy_start;
	app_pointer = do_div(temp_copy_start, prtd->pcm_size);
	spin_unlock_irqrestore(&prtd->lock, flags);

	dstn = prtd->dma_buffer.area + app_pointer;

	if (count < prtd->pcm_size - app_pointer) {
		if (copy_from_user(dstn, buf, count))
			return -EFAULT;
	} else {
		copy = prtd->pcm_size - app_pointer;
		if (copy_from_user(dstn, buf, copy))
			return -EFAULT;
		if (copy_from_user(prtd->dma_buffer.area, buf + copy, count - copy))
			return -EFAULT;
	}

	spin_lock_irqsave(&prtd->lock, flags);
	if (prtd->bytes_received != old_bytes_received ||
	    prtd->user_bytes_received != old_user_bytes_received ||
	    prtd->state != old_state ||
	    prtd->next_track_data != old_next_track_data) {
		prtd->state = Q6APM_STREAM_STOPPED;
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_compr_stop_error_locked(prtd);
		return -EIO;
	}

	prtd->bytes_received = new_bytes_received;
	prtd->user_bytes_received = new_user_bytes_received;
	if (old_next_track_data)
		prtd->next_track_data = false;

	/* Kick off the data to dsp if its starving!! */
	bytes_to_write = q6apm_compr_reserve_next(prtd, &wflags, &is_last_buffer);
	if (prtd->state != old_state) {
		prtd->bytes_received = old_bytes_received;
		prtd->user_bytes_received = old_user_bytes_received;
		prtd->next_track_data = old_next_track_data;
		spin_unlock_irqrestore(&prtd->lock, flags);
		q6apm_compr_stop_error_locked(prtd);
		return -EIO;
	}
	send_eos = prtd->drain_state == Q6APM_DRAIN_REQUESTED &&
		is_last_buffer;
	spin_unlock_irqrestore(&prtd->lock, flags);

	if (bytes_to_write) {
		ret = q6apm_compr_submit(prtd, bytes_to_write, wflags,
					 send_eos, &submitted);
		if (ret) {
			if (!submitted) {
				spin_lock_irqsave(&prtd->lock, flags);
				prtd->bytes_received = old_bytes_received;
				prtd->user_bytes_received = old_user_bytes_received;
				prtd->next_track_data = old_next_track_data;
				spin_unlock_irqrestore(&prtd->lock, flags);
			}
			q6apm_compr_stop_error_locked(prtd);
			if (!submitted)
				return ret;
		}
	}

	return count;
}

static const struct snd_compress_ops q6apm_dai_compress_ops = {
	.open		= q6apm_dai_compr_open,
	.free		= q6apm_dai_compr_free,
	.get_caps	= q6apm_dai_compr_get_caps,
	.get_codec_caps	= q6apm_dai_compr_get_codec_caps,
	.pointer	= q6apm_dai_compr_pointer,
	.trigger	= q6apm_dai_compr_trigger,
	.set_params	= q6apm_dai_compr_set_params,
	.set_metadata	= q6apm_dai_compr_set_metadata,
	.copy		= q6apm_compr_copy,
};

static const struct snd_soc_component_driver q6apm_fe_dai_component = {
	.name		= DRV_NAME,
	.open		= q6apm_dai_open,
	.close		= q6apm_dai_close,
	.prepare	= q6apm_dai_prepare,
	.pcm_new	= q6apm_dai_pcm_new,
	.pcm_free	= q6apm_dai_pcm_free,
	.hw_params	= q6apm_dai_hw_params,
	.pointer	= q6apm_dai_pointer,
	.trigger	= q6apm_dai_trigger,
	.ack		= q6apm_dai_ack,
	.compress_ops	= &q6apm_dai_compress_ops,
	.use_dai_pcm_id = true,
	.remove_order   = SND_SOC_COMP_ORDER_EARLY,
};

static int q6apm_dai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct q6apm_dai_data *pdata;
	struct of_phandle_args args;
	int rc;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	rc = of_parse_phandle_with_fixed_args(node, "iommus", 1, 0, &args);
	if (rc < 0)
		pdata->sid = -1;
	else
		pdata->sid = args.args[0] & SID_MASK_DEFAULT;

	dev_set_drvdata(dev, pdata);

	return devm_snd_soc_register_component(dev, &q6apm_fe_dai_component, NULL, 0);
}

#ifdef CONFIG_OF
static const struct of_device_id q6apm_dai_device_id[] = {
	{ .compatible = "qcom,q6apm-dais" },
	{},
};
MODULE_DEVICE_TABLE(of, q6apm_dai_device_id);
#endif

static struct platform_driver q6apm_dai_platform_driver = {
	.driver = {
		.name = "q6apm-dai",
		.of_match_table = of_match_ptr(q6apm_dai_device_id),
	},
	.probe = q6apm_dai_probe,
};
module_platform_driver(q6apm_dai_platform_driver);

MODULE_DESCRIPTION("Q6APM dai driver");
MODULE_LICENSE("GPL");
