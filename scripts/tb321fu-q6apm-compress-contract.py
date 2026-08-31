#!/usr/bin/env python3
"""Static and arithmetic contract checks for the q6apm compress path."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "sound/soc/qcom/qdsp6/q6apm-dai.c"
AUDIOREACH_SOURCE = ROOT / "sound/soc/qcom/qdsp6/audioreach.c"
Q6APM_SOURCE = ROOT / "sound/soc/qcom/qdsp6/q6apm.c"
U64_MAX = (1 << 64) - 1
MIN_FRAGMENT_SIZE = 8 * 1024
MAX_FRAGMENT_SIZE = 128 * 1024
MIN_FRAGMENTS = 4
MAX_FRAGMENTS = 64


def function_body(source: str, name: str) -> str:
    needle = f"{name}("
    marker = source.find(needle)
    brace = -1
    while marker >= 0:
        candidate = source.find("{", marker + len(needle))
        semicolon = source.find(";", marker + len(needle))
        if candidate >= 0 and (semicolon < 0 or candidate < semicolon):
            brace = candidate
            break
        marker = source.find(needle, marker + len(needle))
    if marker < 0 or brace < 0:
        raise AssertionError(f"missing function body {name}")

    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated body for {name}")


def ordered(body: str, *tokens: str) -> bool:
    position = -1
    for token in tokens:
        position = body.find(token, position + 1)
        if position < 0:
            return False
    return True


def call_expression(body: str, name: str) -> str:
    start = body.find(f"{name}(")
    if start < 0:
        return ""
    depth = 0
    for position in range(start + len(name), len(body)):
        if body[position] == "(":
            depth += 1
        elif body[position] == ")":
            depth -= 1
            if depth == 0:
                return body[start : position + 1]
    return ""


def verify(source: str, audioreach: str, q6apm: str) -> list[str]:
    errors: list[str] = []

    ops_start = source.find("q6apm_dai_compress_ops = {")
    ops_end = source.find("};", ops_start)
    ops = source[ops_start:ops_end]
    if ops_start < 0 or ".copy" not in ops:
        errors.append("compress ops must retain the tested copy transport")
    if ".ack" in ops or ".mmap" in ops:
        errors.append("copy transport must not register ack or mmap")

    validate = function_body(source, "q6apm_compr_validate_geometry")
    for token in (
        "COMPR_PLAYBACK_MIN_FRAGMENT_SIZE",
        "COMPR_PLAYBACK_MAX_FRAGMENT_SIZE",
        "COMPR_PLAYBACK_MIN_NUM_FRAGMENTS",
        "COMPR_PLAYBACK_MAX_NUM_FRAGMENTS",
        "check_mul_overflow",
    ):
        if token not in validate:
            errors.append(f"geometry validation is missing {token}")

    allocate = function_body(source, "q6apm_compr_alloc_dma")
    if "PAGE_ALIGN(ring_bytes)" not in allocate:
        errors.append("DMA allocation must page-align the accepted ring")
    if not ordered(
        allocate,
        "snd_dma_alloc_pages",
        "q6apm_map_memory_fixed_region",
        "if (map_generation)",
        "prtd->memory_mapped = true",
        "prtd->memory_map_generation = map_generation",
        "return ret",
        "if (!ret)",
        "ret = -EPROTO",
        "snd_dma_free_pages",
    ):
        errors.append("DMA allocation/map ownership order is incomplete")
    allocate_map = call_expression(allocate, "q6apm_map_memory_fixed_region")
    if "&map_generation" not in allocate_map:
        errors.append("compressed MAP must return its exact lease generation")
    if "q6apm_get_reset_generation" in allocate:
        errors.append("compressed MAP ownership must not be inferred after return")
    if "prtd->dma_buffer.bytes" not in allocate:
        errors.append("DSP mapping must use the actual allocated byte count")

    release = function_body(source, "q6apm_compr_free_dma")
    if not ordered(
        release,
        "if (!prtd->memory_map_generation)",
        "q6apm_unmap_memory_fixed_region",
        "if (ret)",
        "return ret",
        "prtd->memory_mapped = false",
        "prtd->memory_map_generation = 0",
        "snd_dma_free_pages",
    ):
        errors.append("DMA memory must remain owned when DSP unmap fails")
    release_unmap = call_expression(release, "q6apm_unmap_memory_fixed_region")
    if "prtd->memory_map_generation" not in release_unmap:
        errors.append("compressed UNMAP must target its recorded lease generation")

    open_body = function_body(source, "q6apm_dai_compr_open")
    for forbidden in (
        "snd_dma_alloc_pages",
        "q6apm_map_memory_fixed_region",
        "snd_compr_set_runtime_buffer",
    ):
        if forbidden in open_body:
            errors.append(f"open must not create a fixed maximum ring: {forbidden}")
    if "INIT_DELAYED_WORK(&prtd->drain_work, q6apm_compr_drain_work)" not in open_body:
        errors.append("open must initialize deferred drain notification work")
    compr_release_owner = function_body(source, "q6apm_compr_release_owner")
    if not ordered(
        open_body,
        "try_module_get(THIS_MODULE)",
        "prtd->module_pinned = true",
        "prtd->apm = q6apm_get_apm",
    ) or not ordered(
        compr_release_owner,
        "q6apm_put_apm(prtd->apm)",
        "module_put(THIS_MODULE)",
        "prtd->module_pinned = false",
    ):
        errors.append("compressed DSP ownership must pin the module through final release")

    free_body = function_body(source, "q6apm_dai_compr_free")
    if not ordered(
        free_body,
        "prtd->closing = true",
        "prtd->cstream = NULL",
        "q6apm_graph_quiesce_callbacks(prtd->graph)",
        "q6apm_compr_cancel_error_work(prtd)",
        "q6apm_compr_cancel_drain_work(prtd)",
        "q6apm_compr_cancel_cleanup_work(prtd)",
        "runtime->private_data = NULL",
        "q6apm_compr_try_release(prtd)",
    ):
        errors.append("free must retire callbacks and private work before release")

    error_cancel = function_body(source, "q6apm_compr_cancel_error_work")
    drain_cancel = function_body(source, "q6apm_compr_cancel_drain_work")
    cleanup_cancel = function_body(source, "q6apm_compr_cancel_cleanup_work")
    if "cancel_work_sync(&prtd->error_work)" not in error_cancel:
        errors.append("free must synchronously cancel deferred error work")
    if "cancel_delayed_work_sync(&prtd->drain_work)" not in drain_cancel:
        errors.append("free must synchronously cancel deferred drain work")
    if "cancel_delayed_work_sync(&prtd->cleanup_work)" not in cleanup_cancel:
        errors.append("free must synchronously cancel quarantine cleanup work")

    try_release = function_body(source, "q6apm_compr_try_release")
    if not ordered(
        try_release,
        "prtd->reset_pending = true",
        "q6apm_graph_stop_with_state",
        "if (ret)",
        "return ret",
        "prtd->graph_started = false",
        "q6apm_compr_finish_stop_reset(prtd)",
        "if (ret)",
        "return ret",
        "q6apm_compr_free_dma(prtd)",
    ):
        errors.append("failed close STOP/reset must retain DSP-owned DMA")

    finish_reset = function_body(source, "q6apm_compr_finish_stop_reset")
    if not ordered(
        finish_reset,
        "q6apm_graph_sync_flush_generation",
        "if (generation_changed)",
        "prtd->flush_retry = false",
        "flush_retry = prtd->flush_retry",
        "if (flush_retry || graph_flush_pending)",
        "q6apm_graph_request_flush",
        "q6apm_graph_flush",
        "if (ret)",
        "q6apm_compr_set_flush_retry(prtd, true)",
        "return ret",
        "q6apm_compr_set_flush_retry(prtd, false)",
        "q6apm_graph_try_reset_client",
    ):
        errors.append("client reset must retain or discharge a failed graph flush")

    flush_setter = function_body(source, "q6apm_compr_set_flush_retry")
    if not ordered(
        flush_setter,
        "spin_lock_irqsave",
        "prtd->flush_retry = pending",
        "spin_unlock_irqrestore",
    ):
        errors.append("flush-retry obligation must be synchronized")

    restore = function_body(source, "q6apm_compr_restore_geometry")
    for token in (
        "runtime->fragment_size = prtd->pcm_count",
        "runtime->fragments = prtd->periods",
        "runtime->buffer_size = prtd->pcm_size",
    ):
        if token not in restore:
            errors.append(f"runtime geometry rollback is missing {token}")

    reject_params = function_body(source, "q6apm_compr_params_error")
    if not ordered(
        reject_params,
        "if (core_next_track)",
        "q6apm_compr_restore_geometry",
        "else if (prtd->params_snapshot_valid)",
        "prtd->params_snapshot_valid = false",
        "return ret",
    ):
        errors.append("every failing gapless set_params exit must restore geometry")

    set_params = function_body(source, "q6apm_dai_compr_set_params")
    if not ordered(
        set_params,
        "q6apm_compr_error_is_pending(prtd)",
        "q6apm_compr_stop_error_locked(prtd)",
        "q6apm_compr_params_error(stream, prtd, core_next_track",
        "q6apm_compr_validate_geometry",
        "runtime->fragments != prtd->periods",
        "q6apm_compr_alloc_dma",
        "q6apm_alloc_fragments",
        "q6apm_graph_prepare",
    ):
        errors.append("set_params validation/allocation/prepare order is incomplete")
    if not ordered(
        set_params,
        "q6apm_compr_validate_geometry",
        "if (ret)",
        "q6apm_compr_params_error",
        "runtime->fragments != prtd->periods",
        "q6apm_compr_params_error",
    ):
        errors.append("invalid and mismatched gapless geometry must share rollback")
    if not ordered(set_params, "free_fragments:", "free_dma:", "q6apm_compr_free_dma"):
        errors.append("set_params lacks ordered partial-failure rollback")
    if "q6apm_compr_params_error(runtime" in set_params:
        errors.append("set_params must pass the stream, not its runtime, to rollback")
    if not ordered(
        set_params,
        "q6apm_graph_prepare",
        "prtd->stop_reset = true",
        "prtd->state = Q6APM_STREAM_STOPPED",
    ):
        errors.append("initial prebuffer must be generation-aligned before START")

    reserve = function_body(source, "q6apm_compr_reserve_next")
    for token in (
        "prtd->bytes_sent != prtd->bytes_completed",
        "prtd->gapless_track_end",
        "prtd->user_bytes_received - prtd->copied_total",
        "avail < prtd->pcm_count",
        "prtd->drain_state == Q6APM_DRAIN_NONE",
    ):
        if token not in reserve:
            errors.append(f"DSP reservation contract is missing {token}")
    if "prtd->bytes_sent != prtd->copied_total" in reserve:
        errors.append("user-visible completion must not gate DSP in-flight ownership")

    drain_ready = function_body(source, "q6apm_compr_drain_ready")
    if not ordered(
        drain_ready,
        "if (prtd->next_track)",
        "prtd->gapless_track_end",
        "prtd->bytes_sent == prtd->bytes_completed",
    ):
        errors.append("partial drain readiness is not bound to the old-track end")
    if "if (!prtd->next_track)" in drain_ready:
        errors.append("partial drain readiness contains a contradictory dead branch")

    finish_partial = function_body(source, "q6apm_compr_finish_partial_drain")
    for token in (
        "prtd->bytes_sent = prtd->gapless_next_start",
        "prtd->bytes_completed = prtd->gapless_next_start",
        "prtd->bytes_received = new_received",
        "q6apm_compr_clear_gapless",
    ):
        if token not in finish_partial:
            errors.append(f"partial-drain boundary advance is missing {token}")
    if "copied_total = prtd->gapless_next_start" in finish_partial:
        errors.append("internal gapless padding must not advance copied_total")

    finish_full = function_body(source, "q6apm_compr_finish_full_drain")
    for token in (
        "prtd->bytes_sent != prtd->bytes_completed",
        "prtd->bytes_received != prtd->bytes_completed",
        "prtd->user_bytes_received != prtd->copied_total",
        "q6apm_compr_round_up",
        "prtd->bytes_sent = next_start",
        "prtd->bytes_completed = next_start",
        "prtd->bytes_received = next_start",
        "prtd->state = Q6APM_STREAM_STOPPED",
    ):
        if token not in finish_full:
            errors.append(f"full-drain restart boundary is missing {token}")
    for forbidden in (
        "prtd->copied_total =",
        "prtd->user_bytes_received =",
        "prtd->graph_started = false",
    ):
        if forbidden in finish_full:
            errors.append(f"full drain must preserve user/graph state: {forbidden}")

    mark_error = function_body(source, "q6apm_compr_mark_error_locked")
    if not ordered(
        mark_error,
        "lockdep_assert_held(&prtd->lock)",
        "prtd->drain_notify_pending = false",
        "prtd->error_pending = true",
    ):
        errors.append("fatal error publication must require the transport spinlock")

    compr_send_eos = function_body(source, "q6apm_compr_send_eos")
    if not ordered(
        compr_send_eos,
        "ret = audioreach_shared_memory_send_eos",
        "if (ret > 0)",
        "ret = 0",
        "if (ret < 0)",
        "spin_lock_irqsave(&prtd->lock, flags)",
        "prtd->state = Q6APM_STREAM_STOPPED",
        "q6apm_compr_mark_error_locked(prtd)",
        "spin_unlock_irqrestore(&prtd->lock, flags)",
    ):
        errors.append("EOS failure must publish STOPPED/error atomically before unlock")

    submit = function_body(source, "q6apm_compr_submit")
    if not ordered(
        submit,
        "*submitted = false",
        "q6apm_write_async",
        "prtd->bytes_sent -= bytes",
        "*submitted = true",
    ):
        errors.append("submission ownership/rollback order is incomplete")
    if not ordered(
        submit,
        "ret = q6apm_write_async",
        "if (ret)",
        "spin_lock_irqsave(&prtd->lock, flags)",
        "prtd->bytes_sent -= bytes",
        "prtd->state = Q6APM_STREAM_STOPPED",
        "q6apm_compr_mark_error_locked(prtd)",
        "spin_unlock_irqrestore(&prtd->lock, flags)",
        "return ret",
    ):
        errors.append("submit failure must publish rollback/error atomically before unlock")

    event = function_body(source, "event_handler_compr")
    for token in (
        "prtd->drain_state == Q6APM_DRAIN_EOS_SENT",
        "check_add_overflow(prtd->bytes_completed",
        "check_add_overflow(prtd->copied_total",
        "prtd->bytes_completed = new_completed",
        "prtd->copied_total = new_copied",
        "q6apm_compr_finish_partial_drain",
        "q6apm_compr_finish_full_drain",
        "q6apm_compr_drain_ready",
        "q6apm_compr_report_error",
        "q6apm_compr_schedule_drain_notify",
    ):
        if token not in event:
            errors.append(f"completion lifecycle is missing {token}")
    if "snd_compr_drain_notify" in event:
        errors.append("DSP callback must not notify drain before core enters DRAINING")
    if not ordered(
        event,
        "prtd->bytes_completed = new_completed",
        "prtd->copied_total = new_copied",
        "if (prtd->state != Q6APM_STREAM_RUNNING",
        "q6apm_compr_reserve_next",
    ):
        errors.append("stopped completion must retire without reserving or XRUN")
    if not ordered(
        event,
        "q6apm_compr_submit",
        "if (ret)",
        "} else if (notify_drain)",
        "q6apm_compr_schedule_drain_notify",
    ):
        errors.append("partial-drain continuation must succeed before notification")
    if not ordered(
        event,
        "if (fatal_error)\n\t\tq6apm_compr_mark_error_locked(prtd)",
        "spin_unlock_irqrestore(&prtd->lock, flags)",
        "if (fatal_error)\n\t\tq6apm_compr_report_error(prtd)",
    ):
        errors.append("fatal callback state/error must be atomic before deferred reporting")

    drain_work = function_body(source, "q6apm_compr_drain_work")
    if not ordered(
        drain_work,
        "guard(mutex)(&stream->device->lock)",
        "!prtd->error_pending",
        "stream->runtime->state == SNDRV_PCM_STATE_DRAINING",
        "prtd->drain_notify_pending = false",
        "snd_compr_drain_notify(stream)",
    ):
        errors.append("drain notification is not serialized/cancelled at core DRAINING")
    if not ordered(
        drain_work,
        "else {",
        "retry = true",
        "schedule_delayed_work(&prtd->drain_work, 1)",
    ):
        errors.append("early drain completion must remain pending until DRAINING")

    schedule_drain = function_body(source, "q6apm_compr_schedule_drain_notify")
    if "!prtd->error_pending" not in schedule_drain:
        errors.append("a pending fatal error must suppress new drain notification work")

    stop_error = function_body(source, "q6apm_compr_stop_error_locked")
    if not ordered(
        stop_error,
        "q6apm_compr_mark_error_locked(prtd)",
        "lockdep_assert_held(&stream->device->lock)",
        "snd_compr_stop_error(stream, SNDRV_PCM_STATE_XRUN)",
    ):
        errors.append("operation-context errors must synchronously enter core XRUN")

    report_error = function_body(source, "q6apm_compr_report_error")
    if not ordered(
        report_error,
        "spin_lock_irqsave(&prtd->lock, flags)",
        "q6apm_compr_mark_error_locked(prtd)",
        "spin_unlock_irqrestore(&prtd->lock, flags)",
        "q6apm_compr_queue_error_work(prtd)",
    ):
        errors.append("callback errors must close the next-operation race before work")

    error_work = function_body(source, "q6apm_compr_error_work")
    if error_work.count("!prtd->error_pending") < 2 or not ordered(
        error_work,
        "guard(mutex)(&stream->device->lock)",
        "!prtd->error_pending",
        "q6apm_compr_stop_error_locked(prtd)",
    ):
        errors.append("error work must revalidate pending state under the core mutex")

    trigger = function_body(source, "q6apm_dai_compr_trigger")
    if not ordered(
        trigger,
        "SND_COMPR_TRIGGER_NEXT_TRACK",
        "q6apm_compr_round_up(prtd->bytes_received",
        "prtd->gapless_track_end = prtd->bytes_received",
        "prtd->gapless_next_start = next_start",
    ):
        errors.append("NEXT_TRACK does not freeze the old/new transport boundary")
    if not ordered(
        trigger,
        "if (!was_running)",
        "q6apm_graph_start",
        "q6apm_graph_sync_flush_generation",
        "prtd->graph_started = true",
        "generation_changed && !prtd->stop_reset",
        "prtd->state = Q6APM_STREAM_STOPPED",
        "ret = -EIO",
        "else {",
        "prtd->stop_reset = false",
        "prtd->state = Q6APM_STREAM_RUNNING",
        "q6apm_compr_reserve_next",
    ) or "if (was_running)\n\t\t\tbreak" in trigger:
        errors.append("START must re-arm an already-started full-drain graph")
    for token in (
        "transport_error = true",
        "stream->partial_drain = false",
        "stream->next_track = false",
        "q6apm_compr_operation_error_locked(prtd)",
        "q6apm_compr_complete_stop(prtd, stream)",
    ):
        if token not in trigger:
            errors.append(f"trigger transport-failure closure is missing {token}")
    start_section = trigger[
        trigger.find("case SNDRV_PCM_TRIGGER_START") :
        trigger.find("case SNDRV_PCM_TRIGGER_STOP")
    ]
    if "q6apm_compr_reset_counters" in start_section:
        errors.append("START must not silently erase core-accepted prebuffer")
    if not ordered(
        start_section,
        "q6apm_graph_flush",
        "q6apm_compr_set_flush_retry(prtd",
        "stop_ret != 0",
    ):
        errors.append("START rollback must retain a failed graph flush obligation")
    drain_section = trigger[
        trigger.find("case SND_COMPR_TRIGGER_DRAIN") :
        trigger.find("default:", trigger.find("case SND_COMPR_TRIGGER_DRAIN"))
    ]
    pre_switch = trigger[: trigger.find("switch (cmd)")]
    if "q6apm_compr_report_error(prtd)" in pre_switch + start_section + drain_section:
        errors.append("operation-context START/drain errors must not defer core XRUN")
    stop_section = trigger[
        trigger.find("case SNDRV_PCM_TRIGGER_STOP") :
        trigger.find("case SND_COMPR_TRIGGER_NEXT_TRACK")
    ]
    if (
        "old_state" in stop_section
        or "old_drain_state" in stop_section
        or "transport_error = true" not in stop_section
    ):
        errors.append("STOP/flush failures must fail closed without stale callback rollback")
    if not ordered(
        stop_section,
        "q6apm_graph_flush",
        "if (ret)",
        "q6apm_compr_set_flush_retry(prtd, true)",
        "transport_error = true",
        "q6apm_compr_set_flush_retry(prtd, false)",
    ):
        errors.append("STOP must retain failed flush and clear it only on success")

    copy_body = function_body(source, "q6apm_compr_copy")
    if "ALIGN(" in copy_body:
        errors.append("copy path still uses power-of-two-only ALIGN")
    for token in (
        "prtd->gapless_next_start",
        "old_bytes_received - prtd->bytes_completed",
        "prtd->pcm_size - outstanding - padding",
        "prtd->user_bytes_received = new_user_bytes_received",
        "bool submitted = false",
        "if (!submitted)",
        "prtd->bytes_received = old_bytes_received",
        "q6apm_compr_stop_error_locked(prtd)",
    ):
        if token not in copy_body:
            errors.append(f"transactional copy contract is missing {token}")
    if copy_body.count("if (!submitted)") < 2:
        errors.append("copy must distinguish rollback and return after ownership failure")
    if "q6apm_compr_report_error(prtd)" in copy_body:
        errors.append("operation-context copy errors must not defer core XRUN")
    if not ordered(
        copy_body,
        "q6apm_compr_error_is_pending(prtd)",
        "q6apm_compr_stop_error_locked(prtd)",
        "q6apm_compr_finish_stop_reset(prtd)",
    ):
        errors.append("deferred callback error must gate copy before reset or acceptance")
    if not ordered(
        copy_body,
        "if (outstanding > prtd->pcm_size)",
        "prtd->state = Q6APM_STREAM_STOPPED",
        "q6apm_compr_stop_error_locked(prtd)",
        "return -EIO",
        "if (padding > prtd->pcm_size - outstanding)",
        "return 0",
    ):
        errors.append("hard accounting corruption must not look like temporary backpressure")
    overflow_start = copy_body.find("if (check_add_overflow")
    overflow_end = copy_body.find("temp_copy_start", overflow_start)
    overflow_block = copy_body[overflow_start:overflow_end]
    if (
        "prtd->state = Q6APM_STREAM_STOPPED" not in overflow_block
        or "q6apm_compr_stop_error_locked(prtd)" not in overflow_block
        or "return -EOVERFLOW" not in overflow_block
    ):
        errors.append("counter overflow must synchronously terminate the stream")
    if not ordered(
        copy_body,
        "if (ret)",
        "if (!submitted)",
        "prtd->bytes_received = old_bytes_received",
        "q6apm_compr_stop_error_locked(prtd)",
        "if (!submitted)",
        "return ret",
    ):
        errors.append("unowned copy rollback does not precede the core-visible error")
    if copy_body.count("check_add_overflow") < 2:
        errors.append("internal and user receive accumulation need overflow checks")

    round_up = function_body(source, "q6apm_compr_round_up")
    if not ordered(round_up, "do_div", "check_add_overflow"):
        errors.append("generic rounding lacks division and overflow checks")

    pointer = function_body(source, "q6apm_dai_compr_pointer")
    if not ordered(
        pointer,
        "if (!prtd->pcm_size)",
        "tstamp->copied_total = prtd->copied_total",
        "temp_bytes_completed = prtd->bytes_completed",
        "do_div",
    ):
        errors.append("pointer must separate user totals from the internal ring offset")

    reset = function_body(source, "q6apm_compr_reset_counters")
    for token in (
        "prtd->bytes_completed = 0",
        "prtd->user_bytes_received = 0",
        "prtd->gapless_track_end = 0",
        "prtd->gapless_next_start = 0",
        "prtd->next_track_data = false",
        "prtd->error_pending = false",
    ):
        if token not in reset:
            errors.append(f"STOP/reset closure is missing {token}")

    complete_stop = function_body(source, "q6apm_compr_complete_stop")
    if not ordered(
        complete_stop,
        "attached = stream && prtd->cstream == stream && !prtd->closing",
        "q6apm_compr_clear_gapless",
        "prtd->error_pending = false",
        "prtd->drain_notify_pending = false",
        "if (attached)",
        "stream->next_track = false",
    ):
        errors.append("successful STOP must close private/core gapless state")

    pcm_free = function_body(source, "q6apm_dai_pcm_free")
    if not ordered(
        pcm_free,
        "mapped = owner->mapped",
        "map_generation = owner->map_generation",
        "q6apm_unmap_memory_fixed_region",
        "if (ret)",
        "q6apm_pcm_mark_gone(owner)",
        "owner->mapped != mapped",
        "owner->map_generation != map_generation",
        "owner->mapped = false",
        "owner->map_generation = 0",
        "list_del_init(&owner->node)",
    ):
        errors.append("successful PCM UNMAP must authenticate and retire its owner")
    pcm_free_unmap = call_expression(pcm_free, "q6apm_unmap_memory_fixed_region")
    if "map_generation" not in pcm_free_unmap:
        errors.append("PCM free must UNMAP only its snapshotted lease generation")
    if "if (owner->mapped || owner->quarantined || owner->prtd)" in pcm_free:
        errors.append("PCM teardown must not reject every still-marked UNMAP snapshot")

    pcm_map = function_body(source, "q6apm_dai_memory_map")
    pcm_map_call = call_expression(pcm_map, "q6apm_map_memory_fixed_region")
    if "&map_generation" not in pcm_map_call or not ordered(
        pcm_map,
        "if (map_generation)",
        "owner->mapped = true",
        "owner->map_generation = map_generation",
        "if (!ret && !map_generation)",
        "ret = -EPROTO",
    ):
        errors.append("PCM MAP must publish only its returned lease generation")
    if "q6apm_get_reset_generation" in pcm_map:
        errors.append("PCM MAP ownership must not be inferred after return")

    pcm_cleanup = function_body(source, "q6apm_pcm_cleanup_work")
    pcm_cleanup_unmap = call_expression(
        pcm_cleanup, "q6apm_unmap_memory_fixed_region"
    )
    if "map_generation" not in pcm_cleanup_unmap or not ordered(
        pcm_cleanup,
        "mapped = owner->mapped",
        "map_generation = owner->map_generation",
        "if (!map_generation)",
        "q6apm_unmap_memory_fixed_region",
        "owner->map_generation != map_generation",
        "owner->mapped = false",
        "owner->map_generation = 0",
    ):
        errors.append("PCM cleanup must retire and clear the same lease generation")

    pcm_unclaim = function_body(source, "q6apm_pcm_unclaim_owner")
    if not ordered(
        pcm_unclaim,
        "drop_prtd_ref = owner->prtd_ref",
        "owner->prtd = NULL",
        "owner->prtd_ref = false",
        "if (drop_prtd_ref)",
        "q6apm_dai_rtd_put(prtd)",
    ):
        errors.append("PCM unclaim must drop only an authenticated association kref")

    pcm_quarantine = function_body(source, "q6apm_pcm_quarantine_close")
    if not ordered(
        pcm_quarantine,
        "graph = READ_ONCE(prtd->graph)",
        "WRITE_ONCE(prtd->substream, NULL)",
        "q6apm_graph_quiesce_callbacks(graph)",
        "mutex_lock(&q6apm_pcm_owners_lock)",
        "candidate->prtd == prtd &&",
        "candidate->substream == substream",
    ):
        errors.append("PCM quarantine must detach callbacks before fallible owner lookup")

    pcm_owner_create = function_body(source, "q6apm_pcm_create_owner")
    pcm_owner_release = function_body(source, "q6apm_pcm_owner_release")
    if not ordered(
        pcm_owner_create,
        "get_device(&substream->pcm->card->card_dev)",
        "owner->card = substream->pcm->card",
        "list_add_tail(&owner->node, &q6apm_pcm_owners)",
    ) or "put_device(&owner->card->card_dev)" not in pcm_owner_release:
        errors.append("deferred PCM accounting must hold the card lifetime")
    if not ordered(
        pcm_owner_create,
        "try_module_get(THIS_MODULE)",
        "owner->module_pinned = true",
        "owner->apm = q6apm_get_apm",
    ) or not ordered(
        pcm_owner_release,
        "if (owner->module_pinned)",
        "module_put(THIS_MODULE)",
        "q6apm_put_apm(owner->apm)",
    ):
        errors.append("PCM DSP ownership must pin the module through owner release")

    pcm_prepare = function_body(source, "q6apm_dai_prepare")
    if not ordered(
        pcm_prepare,
        "if (prtd->fragments_allocated)",
        "ret = q6apm_free_fragments",
        "if (ret)",
        "return ret",
        "prtd->fragments_allocated = false",
        "q6apm_alloc_fragments",
        "free_fragments:",
        "reset_ret = q6apm_free_fragments",
        "if (reset_ret)",
        "return reset_ret",
        "prtd->fragments_allocated = false",
    ):
        errors.append("PCM fragment replacement/rollback must retain failed ownership")
    if not ordered(
        pcm_prepare,
        "prtd->queue_ptr = READ_ONCE(runtime->control->appl_ptr)",
        "prtd->state = Q6APM_STREAM_RUNNING",
    ):
        errors.append("PCM prepare must align the playback submission epoch")

    pcm_pending = function_body(source, "q6apm_pcm_pending_frames")
    for token in (
        "queue_ptr >= runtime->boundary",
        "appl_ptr >= runtime->boundary",
        "runtime->boundary - queue_ptr + appl_ptr",
        "*pending > runtime->buffer_size",
    ):
        if token not in pcm_pending:
            errors.append(f"PCM boundary accounting is missing {token}")
    pcm_ack = function_body(source, "q6apm_dai_ack")
    if not ordered(
        pcm_ack,
        "q6apm_pcm_pending_frames(runtime, prtd->queue_ptr, &pending)",
        "pending / runtime->period_size",
        "next_queue = runtime->period_size - tail",
        "q6apm_write_async",
        "prtd->queue_ptr = next_queue",
    ) or "runtime->control->appl_ptr - prtd->queue_ptr" in pcm_ack:
        errors.append("PCM ACK must submit and advance with boundary-aware accounting")

    send_eos = function_body(audioreach, "audioreach_shared_memory_send_eos")
    if not ordered(
        send_eos,
        "ret = gpr_send_port_pkt",
        "if (ret < 0)",
        "graph->eos_pending = false",
        "} else {",
        "ret = 0",
        "return ret",
    ):
        errors.append("AudioReach EOS must normalize non-negative GPR success to zero")

    graph_open = function_body(q6apm, "q6apm_get_audioreach_graph")
    if not ordered(
        graph_open,
        "graph->graph_open_ambiguous = true",
        "graph->graph_quarantined = true",
        "idr_alloc",
        "ret = __q6apm_send_cmd_sync",
        "mutex_lock(&apm->online_lock)",
        "mutex_lock(&graph->state_lock)",
        "if (!ret)",
        "if (!READ_ONCE(apm->online) || graph->graph_reset_retired)",
        "graph->graph_open_ambiguous = false",
        "graph->graph_quarantined = false",
        "} else if (graph->graph_open_ambiguous &&",
        "READ_ONCE(apm->online) &&",
        "!q6apm_graph_result_ambiguous(ret)",
        "graph->graph_open_ambiguous = false",
        "graph->graph_close_confirmed = true",
        "mutex_unlock(&graph->state_lock)",
        "mutex_unlock(&apm->online_lock)",
        "if (ret)",
    ):
        errors.append("OPEN publication and return settlement must be monotonic")
    send_at = graph_open.find("ret = __q6apm_send_cmd_sync")
    open_ambiguous_at = graph_open.find("graph->graph_open_ambiguous = true")
    if open_ambiguous_at < 0 or send_at < 0 or open_ambiguous_at > send_at:
        errors.append("OPEN quarantine must be visible before transport submission")
    if graph_open.count("graph->graph_open_ambiguous = true;") != 1:
        errors.append("the sender must not reinstall ambiguous OPEN after a late result")
    if graph_open.count("graph->graph_close_confirmed = true;") != 1:
        errors.append("only the authenticated online local-failure branch may confirm OPEN absence")

    graph_result = function_body(q6apm, "q6apm_apply_graph_result")
    open_result_at = graph_result.find("if (opcode == APM_CMD_GRAPH_OPEN")
    close_result_at = graph_result.find("if (opcode == APM_CMD_GRAPH_CLOSE")
    open_result = graph_result[open_result_at:close_result_at]
    if not ordered(
        open_result,
        "opcode == APM_CMD_GRAPH_OPEN &&",
        "graph->graph_open_ambiguous",
        "graph->graph_open_ambiguous = false",
        "graph->graph_quarantined = true",
        "graph->graph_close_confirmed = !!status",
        "queue_release = true",
    ) or open_result_at < 0 or close_result_at <= open_result_at:
        errors.append("an authenticated late OPEN result must settle quarantine")

    graph_ambiguous = function_body(q6apm, "q6apm_graph_result_ambiguous")
    if "-ETIMEDOUT" not in graph_ambiguous or "-EPROTO" not in graph_ambiguous:
        errors.append("OPEN ambiguity must retain timeout and protocol failures")
    if "-ENODEV" in graph_ambiguous:
        errors.append("ordinary transport removal must be classified by online state")

    mark_offline = function_body(q6apm, "q6apm_mark_offline_locked")
    final_graph_retirement = (
        "if (final_reset) {\n"
        "\t\t\tgraph->graph_open_ambiguous = false;\n"
        "\t\t\tgraph->graph_close_confirmed = true;\n"
        "\t\t\tgraph->graph_reset_retired = true;"
    )
    if final_graph_retirement not in mark_offline:
        errors.append("only trusted final reset may retire unresolved OPEN ownership")
    if mark_offline.count("graph->graph_close_confirmed = true;") != 1:
        errors.append("ordinary offline transition must not confirm graph retirement")

    if q6apm.count(".suppress_bind_attrs = true") != 1:
        errors.append("APM must suppress unsafe manual bind/unbind")
    unbind_quiescent = function_body(q6apm, "q6apm_unbind_quiescent")
    if not ordered(
        unbind_quiescent,
        "lockdep_assert_held(&apm->online_lock)",
        "mutex_lock(&apm->response_lock)",
        "mutex_lock(&apm->lock)",
        "list_empty(&apm->cmd_tickets)",
        "list_empty(&apm->mem_leases)",
        "list_empty(&apm->graph_clients)",
        "idr_is_empty(&apm->graph_idr)",
        "!atomic_read(&apm->active_graphs)",
        "mutex_unlock(&apm->lock)",
        "mutex_unlock(&apm->response_lock)",
    ):
        errors.append("ordinary unbind must audit the complete DSP ownership registry")
    apm_remove = function_body(q6apm, "apm_remove")
    if not ordered(
        apm_remove,
        "q6apm_mark_offline_locked(apm, final_reset)",
        "if (!final_reset)",
        "q6apm_retire_old_leases(apm)",
        "if (!q6apm_unbind_quiescent(apm))",
        "dev_warn",
        "mutex_unlock(&apm->online_lock)",
    ):
        errors.append("ordinary remove must retire terminal state and expose forced live unbind")

    map_region = function_body(q6apm, "q6apm_map_memory_fixed_region")
    map_send_at = map_region.find("ret = __q6apm_send_cmd_sync")
    map_reconcile = map_region[map_send_at:]
    if map_send_at < 0 or not ordered(
        map_reconcile,
        "generation != atomic_read(&apm->reset_generation)",
        "q6apm_lease_clear_locked",
        "AUDIOREACH_MEM_RESET_RETIRED",
        "ret = -ENETRESET",
    ):
        errors.append("a reset racing MAP success must replace success with -ENETRESET")

    apm_callback = function_body(q6apm, "apm_callback")
    if not ordered(
        apm_callback,
        "ticket->state != Q6APM_CMD_PENDING",
        "ticket->state != Q6APM_CMD_AMBIGUOUS",
        "!q6apm_map_ticket_retained_locked(ticket)",
        "ticket->kind == Q6APM_CMD_MAP && ticket->map_protocol_error",
    ):
        errors.append("terminal MAP transcripts must remain callback-visible")
    basic_at = apm_callback.find("if (hdr->opcode == GPR_BASIC_RSP_RESULT)")
    basic_map_at = apm_callback.find(
        "if (ticket->kind == Q6APM_CMD_MAP)", basic_at
    )
    basic_unmap_at = apm_callback.find(
        "if (ticket->kind == Q6APM_CMD_UNMAP)", basic_map_at
    )
    basic_map = apm_callback[basic_map_at:basic_unmap_at]
    if (
        basic_at < 0
        or basic_map_at < basic_at
        or basic_unmap_at <= basic_map_at
        or not ordered(
            basic_map,
            "if (!ticket->lease)",
            "ticket->lease->map_ticket != ticket",
            "ticket->lease->generation != ticket->generation",
            "if (ticket->map_basic_seen)",
            "ticket->map_basic_status != result->status",
            "goto out",
            "ticket->map_basic_seen = true",
            "ticket->map_basic_status = result->status",
            "if (!result->status)",
            "goto out",
            "if (ticket->map_handle_seen)",
            "goto protocol_error",
            "ticket->lease->map_ticket == ticket",
            "ticket->lease->generation == ticket->generation",
            "ticket->lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN",
            "Q6APM_CMD_CONFIRMED_ERROR",
        )
    ):
        errors.append(
            "MAP basic success must be advisory and basic error quarantined terminal"
        )

    handle_at = apm_callback.find(
        "if (hdr->opcode == APM_CMD_RSP_SHARED_MEM_MAP_REGIONS)"
    )
    spf_at = apm_callback.find(
        "if (hdr->opcode == APM_CMD_RSP_GET_SPF_STATE)", handle_at
    )
    handle_map = apm_callback[handle_at:spf_at]
    if (
        handle_at < 0
        or spf_at <= handle_at
        or not ordered(
            handle_map,
            "ticket->kind != Q6APM_CMD_MAP",
            "ticket->response_opcode != APM_CMD_RSP_SHARED_MEM_MAP_REGIONS",
            "data->payload_size < sizeof(*map_rsp)",
            "if (!map_rsp->mem_map_handle)",
            "if (!ticket->lease)",
            "ticket->map_basic_seen && ticket->map_basic_status",
            "if (ticket->map_handle_seen)",
            "ticket->map_handle != map_rsp->mem_map_handle",
            "goto out",
            "ticket->lease->map_ticket == ticket",
            "ticket->lease->generation == ticket->generation",
            "AUDIOREACH_MEM_MAP_PENDING",
            "AUDIOREACH_MEM_MAP_UNCERTAIN",
            "ticket->map_handle_seen = true",
            "ticket->map_handle = map_rsp->mem_map_handle",
            "ticket->lease->handle = map_rsp->mem_map_handle",
            "ticket->lease->state = AUDIOREACH_MEM_MAPPED",
            "Q6APM_CMD_CONFIRMED_OK",
        )
    ):
        errors.append("a valid dedicated MAP handle must be terminal success")
    if "map_basic_ok" in basic_map or "map_basic_ok" in handle_map:
        errors.append("dedicated MAP success must not require obsolete map_basic_ok")

    protocol_at = apm_callback.find("protocol_error:")
    out_at = apm_callback.find("\nout:", protocol_at)
    protocol_map = apm_callback[protocol_at:out_at]
    if not ordered(
        protocol_map,
        "ticket->kind == Q6APM_CMD_MAP",
        "ticket->map_protocol_error = true",
        "ticket->lease->generation == ticket->generation",
        "ticket->kind != Q6APM_CMD_MAP ||",
        "ticket->lease->map_ticket == ticket",
        "AUDIOREACH_MEM_MAPPED",
        "ticket->lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN",
        "Q6APM_CMD_AMBIGUOUS",
        "-EPROTO",
    ):
        errors.append("MAP protocol poison must be permanent and lease-scoped")
    callback_out = apm_callback[out_at:]
    if not ordered(
        callback_out,
        "ticket->state != Q6APM_CMD_AMBIGUOUS",
        "!q6apm_map_ticket_retained_locked(ticket)",
        "q6apm_ticket_unlink_locked(ticket)",
        "kref_put(&ticket->refcount, q6apm_cmd_ticket_release)",
        "if (retire_leases)",
        "q6apm_retire_old_leases(apm)",
    ):
        errors.append(
            "callback must retain MAP transcripts and retire them after late UNMAP"
        )

    basic_graph_at = apm_callback.find(
        "if (ticket->opcode == APM_CMD_GRAPH_OPEN", basic_unmap_at
    )
    basic_unmap = apm_callback[basic_unmap_at:basic_graph_at]
    if not ordered(
        basic_unmap,
        "AUDIOREACH_MEM_UNMAP_PENDING",
        "AUDIOREACH_MEM_UNMAP_UNCERTAIN",
        "if (!result->status)",
        "q6apm_lease_clear_locked",
        "AUDIOREACH_MEM_UNMAPPED",
        "retire_leases = true",
    ):
        errors.append("late successful UNMAP must request transcript retirement")

    retain_map = function_body(q6apm, "q6apm_map_ticket_retained_locked")
    for token in (
        "ticket->kind != Q6APM_CMD_MAP",
        "lockdep_assert_held(&apm->response_lock)",
        "lease->map_ticket == ticket",
        "lease->generation == ticket->generation",
        "lease->state != AUDIOREACH_MEM_UNMAPPED",
        "lease->state != AUDIOREACH_MEM_RESET_RETIRED",
    ):
        if token not in retain_map:
            errors.append(f"MAP transcript retention is missing {token}")

    send_sync = function_body(q6apm, "__q6apm_send_cmd_sync")
    if not ordered(
        send_sync,
        "kind == Q6APM_CMD_MAP && lease",
        "lease->generation != generation",
        "lease->state != AUDIOREACH_MEM_MAP_PENDING",
        "lease->map_ticket",
        "lease->map_ticket = ticket",
        "list_add_tail(&ticket->node, &apm->cmd_tickets)",
    ) or send_sync.count("!q6apm_map_ticket_retained_locked(ticket)") != 2:
        errors.append("MAP ticket publication/return must retain one lease transcript")

    lease_clear = function_body(q6apm, "q6apm_lease_clear_locked")
    if not ordered(
        lease_clear,
        "lease->handle = 0",
        "lease->users = 0",
        "lease->state = state",
        "lease->map_ticket = NULL",
    ):
        errors.append("trusted lease retirement must detach the MAP transcript")

    retire_leases = function_body(q6apm, "q6apm_retire_old_leases")
    if not ordered(
        retire_leases,
        "ticket->kind != Q6APM_CMD_MAP",
        "ticket->waiter_active",
        "ticket_lease->state != AUDIOREACH_MEM_UNMAPPED",
        "ticket_lease->state != AUDIOREACH_MEM_RESET_RETIRED",
        "ticket_lease->map_ticket = NULL",
        "ticket->listed = false",
        "q6apm_lease_has_ticket_locked",
    ):
        errors.append("terminal lease retirement must bound MAP transcript lifetime")

    unmap_region = function_body(q6apm, "q6apm_unmap_memory_fixed_region")
    if not ordered(
        unmap_region,
        "q6apm_lease_clear_locked",
        "kref_put(&lease->refcount, q6apm_mem_lease_release)",
        "q6apm_retire_old_leases(apm)",
    ):
        errors.append("successful UNMAP must retire the completed MAP transcript")

    if not ordered(
        mark_offline,
        "ticket->lease->map_ticket == ticket",
        "ticket->lease->map_ticket = NULL",
        "ticket->state = final_reset",
        "list_move_tail(&ticket->node, &retired_tickets)",
    ):
        errors.append("transport removal must detach tickets before releasing them")

    return errors


def model_geometry(fragment_size: int, fragments: int) -> int:
    if not MIN_FRAGMENT_SIZE <= fragment_size <= MAX_FRAGMENT_SIZE:
        raise ValueError("fragment size")
    if not MIN_FRAGMENTS <= fragments <= MAX_FRAGMENTS:
        raise ValueError("fragment count")
    return fragment_size * fragments


def model_round_up(value: int, alignment: int) -> int:
    if alignment <= 0:
        raise ValueError("alignment")
    remainder = value % alignment
    result = value if remainder == 0 else value + alignment - remainder
    if result > U64_MAX:
        raise OverflowError("u64")
    return result


def model_pcm_pending(appl: int, queue: int, boundary: int, buffer: int) -> int:
    if boundary <= 0 or not 0 <= appl < boundary or not 0 <= queue < boundary:
        raise ValueError("PCM pointer domain")
    pending = appl - queue if appl >= queue else boundary - queue + appl
    if pending > buffer:
        raise OverflowError("PCM queue epoch")
    return pending


def model_eos_result(ret: int) -> int:
    return ret if ret < 0 else 0


def model_stop_flags(
    success: bool, core_next_track: bool, private_next_track: bool
) -> tuple[bool, bool]:
    if success:
        return False, False
    return core_next_track, private_next_track


def model_close_dma_release(
    graph_started: bool, stop_succeeds: bool, flush_succeeds: bool
) -> bool:
    safe_to_free = True
    if graph_started and not stop_succeeds:
        safe_to_free = False
    if not flush_succeeds:
        safe_to_free = False
    return safe_to_free


def model_generation_start(
    generation_changed: bool, reset_aligned: bool, accepted_bytes: int
) -> tuple[int, bool]:
    xrun = generation_changed and not reset_aligned
    return accepted_bytes, xrun


def model_flush_retry(
    flush_failed: bool, peer_generation_changed: bool, retry_succeeded: bool
) -> bool:
    pending = flush_failed
    if peer_generation_changed:
        pending = False
    if pending and retry_succeeded:
        pending = False
    return pending


def model_gapless_geometry(
    old_fragment_size: int,
    old_fragments: int,
    requested_fragment_size: int,
    requested_fragments: int,
) -> tuple[int, int, int, bool]:
    old_ring = model_geometry(old_fragment_size, old_fragments)
    try:
        requested_ring = model_geometry(requested_fragment_size, requested_fragments)
    except (OverflowError, ValueError):
        return old_fragment_size, old_fragments, old_ring, False
    accepted = (
        requested_fragment_size == old_fragment_size
        and requested_fragments == old_fragments
        and requested_ring == old_ring
    )
    if accepted:
        return requested_fragment_size, requested_fragments, requested_ring, True
    return old_fragment_size, old_fragments, old_ring, False


@dataclass
class GraphOpenModel:
    ambiguous: bool = True
    quarantined: bool = True
    close_confirmed: bool = False
    reset_retired: bool = False
    online: bool = True

    def response(self, status: int) -> None:
        if not self.ambiguous:
            return
        self.ambiguous = False
        self.quarantined = True
        self.close_confirmed = status != 0

    def ordinary_remove(self) -> None:
        self.online = False
        self.quarantined = True

    def trusted_reset(self) -> None:
        self.online = False
        self.ambiguous = False
        self.quarantined = True
        self.close_confirmed = True
        self.reset_retired = True

    def sender_return(self, ret: int, ambiguous_result: bool) -> int:
        if ret == 0:
            if not self.online or self.reset_retired:
                return -1
            self.ambiguous = False
            self.quarantined = False
        elif self.ambiguous and self.online and not ambiguous_result:
            self.ambiguous = False
            self.close_confirmed = True
        return ret


@dataclass
class SupportedUnbindModel:
    module_pins: int
    tickets: int
    leases: int
    graph_clients: int
    graph_cache: int
    active_graphs: int
    apm_krefs: int
    lease_krefs: int
    ticket_krefs: int
    removed: bool = False
    manual_bind_attrs: bool = False

    def snapshot(self) -> tuple[int, ...]:
        return (
            self.module_pins,
            self.tickets,
            self.leases,
            self.graph_clients,
            self.graph_cache,
            self.active_graphs,
            self.apm_krefs,
            self.lease_krefs,
            self.ticket_krefs,
            int(self.removed),
        )

    def quiescent(self) -> bool:
        return not any(
            (
                self.tickets,
                self.leases,
                self.graph_clients,
                self.graph_cache,
                self.active_graphs,
                self.lease_krefs,
                self.ticket_krefs,
            )
        )

    def manual_unbind(self) -> bool:
        if not self.manual_bind_attrs:
            return False
        return self.module_unbind()

    def module_unbind(self) -> bool:
        if self.removed:
            return True
        if self.module_pins or not self.quiescent():
            return False
        assert self.apm_krefs == 1
        self.apm_krefs = 0
        self.removed = True
        return True


def model_map_reconcile(
    send_ret: int, issued_generation: int, current_generation: int, state: str
) -> tuple[int, int, str]:
    if issued_generation != current_generation:
        return -102, 0, "reset-retired"
    lease_generation = 0 if state in ("unmapped", "reset-retired") else issued_generation
    return send_ret, lease_generation, state


@dataclass
class MapProtocolModel:
    state: str = "pending"
    handle: int = 0
    terminal: str | None = None
    poisoned: bool = False
    basic_seen: bool = False
    basic_status: int = 0
    handle_seen: bool = False
    registered: bool = True

    def poison(self) -> None:
        self.state = "uncertain"
        self.poisoned = True
        self.terminal = None

    def basic(self, status: int) -> None:
        if self.poisoned:
            return
        if self.basic_seen:
            if self.basic_status != status:
                self.poison()
            return
        self.basic_seen = True
        self.basic_status = status
        if status == 0:
            return
        if self.handle_seen:
            self.poison()
            return
        self.state = "uncertain"
        self.terminal = "error"

    def dedicated(self, handle: int) -> None:
        if self.poisoned:
            return
        if handle <= 0 or (self.basic_seen and self.basic_status != 0):
            self.poison()
            return
        if self.handle_seen:
            if self.handle != handle:
                self.poison()
            return
        self.handle_seen = True
        self.handle = handle
        self.state = "mapped"
        self.terminal = "ok"

    def unmap(self) -> None:
        if self.poisoned or self.state != "mapped":
            raise RuntimeError("unresolved MAP ownership")
        self.state = "unmapped"
        self.registered = False

    def trusted_reset(self) -> None:
        self.state = "reset-retired"
        self.registered = False

    def unrelated_phase(self) -> None:
        if not self.registered:
            return
        self.poison()


def expect_rejected(callable_obj, *args: int) -> None:
    try:
        callable_obj(*args)
    except (OverflowError, ValueError):
        return
    raise AssertionError(f"expected rejection for {args!r}")


@dataclass
class StreamModel:
    fragment_size: int
    fragments: int = 4
    internal_received: int = 0
    internal_sent: int = 0
    internal_completed: int = 0
    user_received: int = 0
    user_completed: int = 0
    track_end: int = 0
    next_start: int = 0
    in_flight: int = 0
    next_track: bool = False
    next_track_data: bool = False
    drain_requested: bool = False
    partial_drain: bool = False
    xrun: bool = False
    driver_running: bool = True
    graph_started: bool = True
    notify_pending: bool = False

    @property
    def ring_size(self) -> int:
        return self.fragment_size * self.fragments

    @property
    def core_avail(self) -> int:
        return self.ring_size - (self.user_received - self.user_completed)

    @property
    def private_avail(self) -> int:
        return self.ring_size - (
            self.internal_received - self.internal_completed
        )

    def validate(self) -> None:
        assert 0 <= self.internal_completed <= self.internal_sent
        assert self.internal_sent <= self.internal_received
        assert 0 <= self.user_completed <= self.user_received
        assert self.internal_sent - self.internal_completed == self.in_flight
        assert self.internal_received - self.internal_completed <= self.ring_size
        assert self.user_received - self.user_completed <= self.ring_size
        assert self.internal_received - self.internal_completed >= (
            self.user_received - self.user_completed
        )
        assert 0 <= self.private_avail <= self.core_avail <= self.ring_size
        if self.next_track:
            assert self.internal_completed <= self.track_end
            assert self.track_end <= self.internal_received
            assert self.track_end <= self.next_start

    def reserve(self) -> int:
        if self.xrun or not self.driver_running or self.in_flight:
            return 0
        send_limit = self.track_end if self.next_track else self.internal_received
        avail = send_limit - self.internal_sent
        assert 0 <= avail <= self.user_received - self.user_completed
        if not avail:
            return 0
        if avail < self.fragment_size and not self.drain_requested:
            return 0
        submitted = min(avail, self.fragment_size)
        self.internal_sent += submitted
        self.in_flight = submitted
        self.validate()
        return submitted

    def write(self, count: int, fail_before_owner: bool = False) -> int:
        assert count >= 0
        if self.xrun:
            raise RuntimeError("xrun")

        old_received = self.internal_received
        old_user_received = self.user_received
        old_next_track_data = self.next_track_data
        copy_start = self.next_start if self.next_track_data else old_received
        assert copy_start >= old_received
        outstanding = old_received - self.internal_completed
        padding = copy_start - old_received
        private_avail = self.ring_size - outstanding - padding
        accepted = min(count, max(private_avail, 0))
        if not accepted:
            self.validate()
            return 0

        self.internal_received = copy_start + accepted
        self.user_received += accepted
        if old_next_track_data:
            self.next_track_data = False

        submitted = self.reserve()
        if submitted and fail_before_owner:
            self.internal_sent -= submitted
            self.in_flight = 0
            self.internal_received = old_received
            self.user_received = old_user_received
            self.next_track_data = old_next_track_data
            self.xrun = True
            self.validate()
            return -1

        self.validate()
        return accepted

    def complete(self) -> int:
        assert self.in_flight
        completed = self.in_flight
        self.internal_completed += completed
        self.user_completed += completed
        self.in_flight = 0
        if not self.driver_running:
            self.validate()
            return 0
        submitted = self.reserve()
        self.validate()
        return submitted

    def signal_next_track(self) -> None:
        assert not self.next_track
        self.track_end = self.internal_received
        self.next_start = model_round_up(self.track_end, self.fragment_size)
        self.next_track = True
        self.next_track_data = True
        self.validate()

    def request_partial_drain(self) -> int:
        assert self.next_track
        assert not self.drain_requested
        self.partial_drain = True
        self.drain_requested = True
        submitted = self.reserve()
        self.validate()
        return submitted

    def request_full_drain(self) -> int:
        assert not self.next_track
        assert not self.drain_requested
        self.partial_drain = False
        self.drain_requested = True
        submitted = self.reserve()
        self.validate()
        return submitted

    def eos_ready(self) -> bool:
        drain_end = self.track_end if self.partial_drain else self.internal_received
        return (
            self.drain_requested
            and not self.in_flight
            and self.internal_sent == self.internal_completed == drain_end
        )

    def eos_done(self, continuation_fails: bool = False) -> int:
        assert self.partial_drain and self.next_track and self.eos_ready()
        self.internal_sent = self.next_start
        self.internal_completed = self.next_start
        self.internal_received = max(self.internal_received, self.next_start)
        self.track_end = 0
        self.next_start = 0
        self.next_track = False
        self.next_track_data = False
        self.drain_requested = False
        self.partial_drain = False
        submitted = self.reserve()
        if submitted and continuation_fails:
            self.internal_sent -= submitted
            self.in_flight = 0
            self.xrun = True
            self.notify_pending = False
            self.validate()
            return -1
        self.notify_pending = True
        self.validate()
        return submitted

    def full_eos_done(self) -> None:
        assert not self.partial_drain and not self.next_track and self.eos_ready()
        aligned = model_round_up(self.internal_received, self.fragment_size)
        self.internal_sent = aligned
        self.internal_completed = aligned
        self.internal_received = aligned
        self.track_end = 0
        self.next_start = 0
        self.next_track_data = False
        self.drain_requested = False
        self.driver_running = False
        self.notify_pending = True
        self.validate()

    def start(self) -> int:
        assert self.graph_started
        self.driver_running = True
        submitted = self.reserve()
        self.validate()
        return submitted


@dataclass
class DrainNotifyModel:
    core_state: str = "running"
    trigger_holds_device_lock: bool = True
    pending: bool = False
    delivered: bool = False

    def callback(self) -> None:
        self.pending = True

    def worker(self) -> bool:
        if self.trigger_holds_device_lock:
            return False
        pending = self.pending
        self.pending = False
        if not pending or self.core_state != "draining":
            return False
        self.delivered = True
        self.core_state = "setup"
        return True

    def trigger_returns(self) -> None:
        self.core_state = "draining"
        self.trigger_holds_device_lock = False


@dataclass
class DeferredErrorModel:
    core_state: str = "running"
    private_state: str = "running"
    error_pending: bool = False
    accepted: int = 0

    def callback_error(self) -> None:
        self.private_state = "stopped"
        self.error_pending = True

    def write(self, count: int) -> int:
        if self.error_pending:
            self.core_state = "xrun"
            return -1
        self.accepted += count
        return count

    def stop(self, success: bool) -> None:
        if success:
            self.error_pending = False
            self.private_state = "stopped"


@dataclass
class StopFailureModel:
    private_state: str = "running"
    drain_state: str = "eos_sent"
    completed: int = 0
    reserved: int = 0
    notify_pending: bool = False
    error_pending: bool = False

    def begin_stop(self) -> None:
        self.private_state = "stopped"

    def write_done(self, count: int) -> None:
        self.completed += count
        assert self.private_state == "stopped"
        self.reserved = 0

    def eos_done(self) -> None:
        self.drain_state = "none"
        self.notify_pending = True

    def stop_failed(self) -> None:
        self.notify_pending = False
        self.error_pending = True


def run_models() -> None:
    assert model_geometry(MIN_FRAGMENT_SIZE, MIN_FRAGMENTS) == 32 * 1024
    assert model_geometry(MAX_FRAGMENT_SIZE, MAX_FRAGMENTS) == 8 * 1024 * 1024
    expect_rejected(model_geometry, MIN_FRAGMENT_SIZE - 1, MIN_FRAGMENTS)
    expect_rejected(model_geometry, MAX_FRAGMENT_SIZE + 1, MIN_FRAGMENTS)
    expect_rejected(model_geometry, MIN_FRAGMENT_SIZE, MIN_FRAGMENTS - 1)
    expect_rejected(model_geometry, MIN_FRAGMENT_SIZE, MAX_FRAGMENTS + 1)

    assert model_round_up(12_288, 12_288) == 12_288
    assert model_round_up(12_289, 12_288) == 24_576
    assert model_round_up(30_000, 1_001) == 30_030
    assert model_round_up(U64_MAX - 2, 3) == U64_MAX
    expect_rejected(model_round_up, 1, 0)
    expect_rejected(model_round_up, U64_MAX, 6)

    assert model_pcm_pending(6_144, 2_048, 32_768, 8_192) == 4_096
    assert model_pcm_pending(1_024, 30_720, 32_768, 4_096) == 3_072
    assert model_pcm_pending(6_144, 2_048, 32_768, 4_096) == 4_096
    expect_rejected(model_pcm_pending, 6_145, 2_048, 32_768, 4_096)
    expect_rejected(model_pcm_pending, 0, 0, 0, 4_096)
    expect_rejected(model_pcm_pending, 32_768, 0, 32_768, 4_096)
    expect_rejected(model_pcm_pending, 0, 32_768, 32_768, 4_096)

    assert model_eos_result(-5) == -5
    assert model_eos_result(0) == 0
    assert model_eos_result(64) == 0
    assert model_stop_flags(True, True, True) == (False, False)
    assert model_stop_flags(False, True, True) == (True, True)
    assert model_close_dma_release(True, True, True)
    assert not model_close_dma_release(True, False, True)
    assert not model_close_dma_release(True, True, False)
    assert model_generation_start(True, True, 12) == (12, False)
    assert model_generation_start(True, False, 12) == (12, True)
    assert model_generation_start(False, False, 12) == (12, False)
    assert model_flush_retry(True, False, False)
    assert not model_flush_retry(True, True, False)
    assert not model_flush_retry(True, False, True)

    assert model_gapless_geometry(12_288, 7, 12_288, 7) == (
        12_288,
        7,
        86_016,
        True,
    )
    assert model_gapless_geometry(12_288, 7, 16_384, 7) == (
        12_288,
        7,
        86_016,
        False,
    )
    assert model_gapless_geometry(12_288, 7, 12_288, 8) == (
        12_288,
        7,
        86_016,
        False,
    )
    assert model_gapless_geometry(12_288, 7, 1, 7) == (
        12_288,
        7,
        86_016,
        False,
    )

    synchronous_open = GraphOpenModel()
    assert synchronous_open.sender_return(0, False) == 0
    assert not synchronous_open.ambiguous
    assert not synchronous_open.quarantined
    assert not synchronous_open.close_confirmed

    timed_out_open = GraphOpenModel()
    assert timed_out_open.sender_return(-110, True) == -110
    assert timed_out_open.ambiguous and timed_out_open.quarantined
    assert not timed_out_open.close_confirmed

    removed_open = GraphOpenModel()
    removed_open.ordinary_remove()
    assert removed_open.sender_return(-19, False) == -19
    assert removed_open.ambiguous and removed_open.quarantined
    assert not removed_open.close_confirmed

    late_open_success = GraphOpenModel()
    late_open_success.response(0)
    assert late_open_success.sender_return(-110, True) == -110
    assert not late_open_success.ambiguous and late_open_success.quarantined
    assert not late_open_success.close_confirmed

    late_open_error = GraphOpenModel()
    late_open_error.response(1)
    assert late_open_error.sender_return(-110, True) == -110
    assert not late_open_error.ambiguous and late_open_error.close_confirmed

    local_open_failure = GraphOpenModel()
    assert local_open_failure.sender_return(-5, False) == -5
    assert not local_open_failure.ambiguous
    assert local_open_failure.quarantined and local_open_failure.close_confirmed

    reset_open = GraphOpenModel()
    reset_open.trusted_reset()
    assert reset_open.sender_return(-102, False) == -102
    assert reset_open.reset_retired and reset_open.close_confirmed
    assert not reset_open.ambiguous

    success_then_remove = GraphOpenModel()
    success_then_remove.response(0)
    success_then_remove.ordinary_remove()
    assert success_then_remove.sender_return(0, False) == -1
    assert success_then_remove.quarantined
    assert not success_then_remove.close_confirmed

    guarded_unbind = SupportedUnbindModel(
        module_pins=2,
        tickets=1,
        leases=1,
        graph_clients=1,
        graph_cache=1,
        active_graphs=1,
        apm_krefs=4,
        lease_krefs=2,
        ticket_krefs=2,
    )
    guarded_snapshot = guarded_unbind.snapshot()
    for _ in range(32):
        assert not guarded_unbind.manual_unbind()
        assert not guarded_unbind.module_unbind()
        assert guarded_unbind.snapshot() == guarded_snapshot

    quiescent_unbind = SupportedUnbindModel(
        module_pins=0,
        tickets=0,
        leases=0,
        graph_clients=0,
        graph_cache=0,
        active_graphs=0,
        apm_krefs=1,
        lease_krefs=0,
        ticket_krefs=0,
    )
    assert quiescent_unbind.module_unbind()
    assert quiescent_unbind.module_unbind()
    assert quiescent_unbind.apm_krefs == 0

    assert model_map_reconcile(0, 7, 7, "mapped") == (0, 7, "mapped")
    assert model_map_reconcile(0, 7, 8, "mapped") == (
        -102,
        0,
        "reset-retired",
    )

    handle_only = MapProtocolModel()
    handle_only.dedicated(0x1234)
    assert handle_only.state == "mapped" and handle_only.terminal == "ok"
    assert handle_only.registered

    basic_then_handle = MapProtocolModel()
    basic_then_handle.basic(0)
    assert basic_then_handle.state == "pending" and basic_then_handle.terminal is None
    basic_then_handle.dedicated(0x1234)
    assert basic_then_handle.state == "mapped" and basic_then_handle.registered

    handle_then_basic = MapProtocolModel()
    handle_then_basic.dedicated(0x1234)
    handle_then_basic.basic(0)
    assert handle_then_basic.state == "mapped" and not handle_then_basic.poisoned
    assert handle_then_basic.registered

    rejected_map = MapProtocolModel()
    rejected_map.basic(7)
    assert rejected_map.state == "uncertain" and rejected_map.terminal == "error"
    assert rejected_map.registered

    zero_handle = MapProtocolModel()
    zero_handle.dedicated(0)
    assert zero_handle.state == "uncertain" and zero_handle.poisoned
    assert zero_handle.registered

    conflicting_map = MapProtocolModel()
    conflicting_map.dedicated(0x1234)
    conflicting_map.basic(7)
    assert conflicting_map.state == "uncertain" and conflicting_map.poisoned

    error_then_handle = MapProtocolModel()
    error_then_handle.basic(7)
    error_then_handle.dedicated(0x1234)
    assert error_then_handle.state == "uncertain" and error_then_handle.poisoned

    duplicate_handle = MapProtocolModel()
    duplicate_handle.dedicated(0x1234)
    duplicate_handle.dedicated(0x1234)
    assert duplicate_handle.state == "mapped" and not duplicate_handle.poisoned
    assert duplicate_handle.registered

    conflicting_handle = MapProtocolModel()
    conflicting_handle.dedicated(0x1234)
    conflicting_handle.dedicated(0x5678)
    assert conflicting_handle.state == "uncertain" and conflicting_handle.poisoned

    conflicting_basic = MapProtocolModel()
    conflicting_basic.basic(0)
    conflicting_basic.basic(7)
    assert conflicting_basic.state == "uncertain" and conflicting_basic.poisoned

    poison_persists = MapProtocolModel()
    poison_persists.dedicated(0)
    poison_persists.basic(0)
    poison_persists.dedicated(0x1234)
    assert poison_persists.state == "uncertain" and poison_persists.poisoned
    assert poison_persists.registered
    poison_persists.trusted_reset()
    assert poison_persists.state == "reset-retired"
    assert not poison_persists.registered

    clean_retirement = MapProtocolModel()
    clean_retirement.dedicated(0x1234)
    clean_retirement.unmap()
    assert clean_retirement.state == "unmapped"
    assert not clean_retirement.registered

    unknown_phase = MapProtocolModel()
    unknown_phase.unrelated_phase()
    assert unknown_phase.poisoned and unknown_phase.registered

    short = StreamModel(fragment_size=12)
    assert short.write(6) == 6
    assert short.in_flight == 0
    assert short.write(6) == 6
    assert short.in_flight == 12
    assert short.write(12) == 12
    assert short.in_flight == 12
    assert short.reserve() == 0
    assert short.complete() == 12
    assert short.complete() == 0

    reversed_order = StreamModel(fragment_size=12)
    assert reversed_order.write(18) == 18
    assert reversed_order.complete() == 0
    reversed_order.signal_next_track()
    assert (reversed_order.track_end, reversed_order.next_start) == (18, 24)
    assert reversed_order.write(8) == 8
    assert reversed_order.internal_received == 32
    assert reversed_order.user_received == 26
    assert reversed_order.request_partial_drain() == 6
    assert reversed_order.complete() == 0
    assert reversed_order.eos_ready()
    copied_before_eos = reversed_order.user_completed
    core_avail_before_eos = reversed_order.core_avail
    assert reversed_order.eos_done() == 0
    assert reversed_order.internal_completed == 24
    assert reversed_order.user_completed == copied_before_eos == 18
    assert reversed_order.core_avail == core_avail_before_eos
    assert reversed_order.write(4) == 4
    assert reversed_order.in_flight == 12

    drain_first = StreamModel(fragment_size=12)
    assert drain_first.write(18) == 18
    assert drain_first.complete() == 0
    drain_first.signal_next_track()
    assert drain_first.request_partial_drain() == 6
    assert drain_first.complete() == 0
    assert drain_first.eos_done() == 0
    assert drain_first.internal_received == drain_first.internal_completed == 24
    assert drain_first.user_completed == 18
    assert drain_first.write(12) == 12
    assert drain_first.in_flight == 12

    prebuffered = StreamModel(fragment_size=12)
    assert prebuffered.write(18) == 18
    assert prebuffered.complete() == 0
    prebuffered.signal_next_track()
    assert prebuffered.write(12) == 12
    assert prebuffered.request_partial_drain() == 6
    assert prebuffered.complete() == 0
    assert prebuffered.eos_done() == 12
    assert prebuffered.in_flight == 12
    assert prebuffered.notify_pending

    failed_continuation = StreamModel(fragment_size=12)
    assert failed_continuation.write(18) == 18
    assert failed_continuation.complete() == 0
    failed_continuation.signal_next_track()
    assert failed_continuation.write(12) == 12
    assert failed_continuation.request_partial_drain() == 6
    assert failed_continuation.complete() == 0
    assert failed_continuation.eos_done(continuation_fails=True) == -1
    assert failed_continuation.xrun
    assert not failed_continuation.notify_pending

    no_room = StreamModel(fragment_size=12)
    assert no_room.write(42) == 42
    no_room.signal_next_track()
    assert no_room.write(4) == 0
    assert no_room.complete() == 12
    assert no_room.write(4) == 4

    failed = StreamModel(fragment_size=12)
    assert failed.write(12, fail_before_owner=True) == -1
    assert failed.xrun
    assert failed.internal_received == failed.internal_sent == 0
    assert failed.user_received == failed.user_completed == 0
    try:
        failed.write(1)
    except RuntimeError:
        pass
    else:
        raise AssertionError("operation-context XRUN must block the next write")

    owned = StreamModel(fragment_size=12)
    assert owned.write(12) == 12
    owned.xrun = True
    assert owned.internal_received == owned.internal_sent == 12
    assert owned.user_received == 12

    stopped_completion = StreamModel(fragment_size=12)
    assert stopped_completion.write(12) == 12
    stopped_completion.driver_running = False
    assert stopped_completion.complete() == 0
    assert not stopped_completion.xrun
    assert stopped_completion.internal_completed == 12
    assert stopped_completion.user_completed == 12

    full = StreamModel(fragment_size=12)
    assert full.write(18) == 18
    assert full.complete() == 0
    assert full.request_full_drain() == 6
    assert full.complete() == 0
    assert full.eos_ready()
    user_completed_before_full_eos = full.user_completed
    full.full_eos_done()
    assert full.graph_started and not full.driver_running
    assert full.internal_received == full.internal_completed == 24
    assert full.user_completed == user_completed_before_full_eos == 18
    assert full.write(12) == 12
    assert full.in_flight == 0
    assert full.start() == 12
    assert full.in_flight == 12

    notify = DrainNotifyModel()
    notify.callback()
    assert not notify.worker()
    assert notify.pending and not notify.delivered
    notify.trigger_returns()
    assert notify.worker()
    assert notify.delivered and notify.core_state == "setup"

    deferred = DeferredErrorModel()
    deferred.callback_error()
    assert deferred.write(12) == -1
    assert deferred.accepted == 0 and deferred.core_state == "xrun"
    deferred.stop(success=True)
    assert not deferred.error_pending

    write_stop_failure = StopFailureModel()
    write_stop_failure.begin_stop()
    write_stop_failure.write_done(12)
    write_stop_failure.stop_failed()
    assert write_stop_failure.completed == 12
    assert write_stop_failure.reserved == 0
    assert write_stop_failure.private_state == "stopped"
    assert write_stop_failure.error_pending

    eos_stop_failure = StopFailureModel()
    eos_stop_failure.begin_stop()
    eos_stop_failure.eos_done()
    eos_stop_failure.stop_failed()
    assert eos_stop_failure.drain_state == "none"
    assert not eos_stop_failure.notify_pending
    assert eos_stop_failure.error_pending


def mutate_function(source: str, name: str, old: str, new: str) -> str:
    body = function_body(source, name)
    if old not in body:
        raise AssertionError(f"missing mutation target in {name}: {old}")
    mutation = body.replace(old, new, 1)
    return source.replace(body, mutation, 1)


def run_mutations(source: str, audioreach: str, q6apm: str) -> None:
    mutations = (
        source.replace("\t.copy\t\t= q6apm_compr_copy,", "\t.ack\t\t= NULL,\n\t.copy\t\t= q6apm_compr_copy,", 1),
        source.replace("PAGE_ALIGN(ring_bytes)", "ring_bytes", 1),
        source.replace(
            "runtime->fragment_size < COMPR_PLAYBACK_MIN_FRAGMENT_SIZE",
            "runtime->fragment_size < 1",
            1,
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "q6apm_compr_round_up(prtd->bytes_received",
            "q6apm_compr_round_up(prtd->copied_total",
        ),
        source.replace("q6apm_compr_alloc_dma(component, prtd, pdata, ring_bytes)", "broken_alloc(component)", 1),
        source.replace("if (!prtd->pcm_size)", "if (false)", 1),
        mutate_function(
            source,
            "q6apm_compr_restore_geometry",
            "runtime->buffer_size = prtd->pcm_size;",
            "runtime->buffer_size = ring_bytes;",
        ),
        mutate_function(
            source,
            "q6apm_compr_params_error",
            "if (core_next_track)",
            "if (false)",
        ),
        mutate_function(
            source,
            "q6apm_compr_reserve_next",
            "prtd->bytes_sent != prtd->bytes_completed",
            "prtd->bytes_sent != prtd->copied_total",
        ),
        mutate_function(
            source,
            "q6apm_compr_reserve_next",
            "avail < prtd->pcm_count",
            "false",
        ),
        mutate_function(
            source,
            "q6apm_compr_finish_partial_drain",
            "prtd->bytes_completed = prtd->gapless_next_start",
            "prtd->copied_total = prtd->gapless_next_start",
        ),
        mutate_function(
            source,
            "q6apm_compr_submit",
            "*submitted = true",
            "*submitted = false",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "prtd->copied_total = new_copied",
            "prtd->copied_total = prtd->copied_total",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "q6apm_compr_finish_partial_drain(prtd)",
            "0",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "stream->next_track = false",
            "stream->next_track = true",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "q6apm_compr_operation_error_locked(prtd)",
            "q6apm_compr_report_error(prtd)",
        ),
        mutate_function(
            source,
            "q6apm_compr_copy",
            "if (!submitted)",
            "if (false)",
        ),
        mutate_function(
            source,
            "q6apm_compr_copy",
            "prtd->user_bytes_received = new_user_bytes_received",
            "prtd->user_bytes_received = old_user_bytes_received",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_pointer",
            "temp_bytes_completed = prtd->bytes_completed",
            "temp_bytes_completed = prtd->copied_total",
        ),
        mutate_function(
            source,
            "q6apm_compr_reset_counters",
            "prtd->next_track_data = false",
            "prtd->next_track_data = true",
        ),
        mutate_function(
            source,
            "q6apm_compr_finish_full_drain",
            "prtd->bytes_received = next_start",
            "prtd->copied_total = next_start",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "q6apm_compr_finish_full_drain(prtd)",
            "0",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "if (prtd->state != Q6APM_STREAM_RUNNING || prtd->closing ||",
            "if (false || prtd->closing ||",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "q6apm_compr_schedule_drain_notify(prtd)",
            "snd_compr_drain_notify(prtd->cstream)",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "if (!was_running) {",
            "if (was_running) {",
        ),
        mutate_function(
            source,
            "q6apm_compr_copy",
            "q6apm_compr_stop_error_locked(prtd)",
            "q6apm_compr_report_error(prtd)",
        ),
        mutate_function(
            source,
            "q6apm_compr_drain_work",
            "stream->runtime->state == SNDRV_PCM_STATE_DRAINING",
            "true",
        ),
        mutate_function(
            source,
            "q6apm_compr_complete_stop",
            "stream->next_track = false",
            "stream->next_track = true",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_set_params",
            "q6apm_compr_params_error(stream",
            "q6apm_compr_params_error(runtime",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_set_params",
            "prtd->stop_reset = true",
            "prtd->stop_reset = false",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "if (generation_changed && !prtd->stop_reset)",
            "if (false)",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "prtd->drain_state == Q6APM_DRAIN_EOS_SENT",
            "prtd->drain_state != Q6APM_DRAIN_NONE",
        ),
        source.replace(
            "INIT_DELAYED_WORK(&prtd->drain_work, q6apm_compr_drain_work);",
            "/* missing drain work initialization */",
            1,
        ),
        source.replace(
            "cancel_delayed_work_sync(&prtd->drain_work);",
            "/* missing drain work cancellation */",
            1,
        ),
        mutate_function(
            source,
            "q6apm_compr_drain_ready",
            "if (prtd->next_track)\n\t\tdrain_end = prtd->gapless_track_end;",
            "if (prtd->next_track) {\n\t\tif (!prtd->next_track)\n"
            "\t\t\treturn false;\n\t\tdrain_end = prtd->gapless_track_end;\n\t}",
        ),
        mutate_function(
            source,
            "q6apm_compr_mark_error_locked",
            "prtd->error_pending = true",
            "prtd->error_pending = false",
        ),
        mutate_function(
            source,
            "q6apm_compr_report_error",
            "q6apm_compr_mark_error_locked(prtd)",
            "prtd->error_pending = true",
        ),
        mutate_function(
            source,
            "q6apm_compr_submit",
            "q6apm_compr_mark_error_locked(prtd);\n"
            "\t\tspin_unlock_irqrestore(&prtd->lock, flags);",
            "spin_unlock_irqrestore(&prtd->lock, flags);\n"
            "\t\tq6apm_compr_report_error(prtd);",
        ),
        mutate_function(
            source,
            "q6apm_compr_send_eos",
            "q6apm_compr_mark_error_locked(prtd);\n"
            "\t\tspin_unlock_irqrestore(&prtd->lock, flags);",
            "spin_unlock_irqrestore(&prtd->lock, flags);\n"
            "\t\tq6apm_compr_report_error(prtd);",
        ),
        mutate_function(
            source,
            "event_handler_compr",
            "if (fatal_error)\n\t\tq6apm_compr_mark_error_locked(prtd);\n"
            "\tspin_unlock_irqrestore(&prtd->lock, flags);",
            "spin_unlock_irqrestore(&prtd->lock, flags);\n"
            "\tif (fatal_error)\n\t\tq6apm_compr_report_error(prtd);",
        ),
        mutate_function(
            source,
            "q6apm_compr_error_work",
            "!prtd->error_pending",
            "false",
        ),
        mutate_function(
            source,
            "q6apm_compr_drain_work",
            "!prtd->error_pending",
            "true",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "q6apm_compr_operation_error_locked(prtd)",
            "q6apm_compr_report_error(prtd)",
        ),
        mutate_function(
            source,
            "q6apm_compr_copy",
            "if (q6apm_compr_error_is_pending(prtd))",
            "if (false)",
        ),
        mutate_function(
            source,
            "q6apm_compr_copy",
            "if (outstanding > prtd->pcm_size)",
            "if (false)",
        ),
        source.replace(
            "q6apm_compr_stop_error_locked(prtd);\n\t\treturn -EOVERFLOW;",
            "return -EOVERFLOW;",
            1,
        ),
        mutate_function(
            source,
            "q6apm_compr_complete_stop",
            "prtd->error_pending = false",
            "prtd->error_pending = true",
        ),
        mutate_function(
            source,
            "q6apm_compr_finish_stop_reset",
            "q6apm_compr_set_flush_retry(prtd, true)",
            "q6apm_compr_set_flush_retry(prtd, false)",
        ),
        mutate_function(
            source,
            "q6apm_compr_finish_stop_reset",
            "if (flush_retry || graph_flush_pending)",
            "if (false)",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "stop_ret != 0",
            "false",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_trigger",
            "q6apm_compr_set_flush_retry(prtd, true)",
            "q6apm_compr_set_flush_retry(prtd, false)",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_free",
            "q6apm_graph_quiesce_callbacks(prtd->graph)",
            "/* missing callback retirement barrier */",
        ),
        mutate_function(
            source,
            "q6apm_compr_try_release",
            "if (ret)\n\t\t\treturn ret;",
            "if (ret)\n\t\t\tret = 0;",
        ),
        mutate_function(
            source,
            "q6apm_compr_complete_stop",
            "attached = stream && prtd->cstream == stream && !prtd->closing",
            "attached = true",
        ),
        mutate_function(
            source,
            "q6apm_dai_pcm_free",
            "owner->mapped != mapped",
            "owner->mapped",
        ),
        mutate_function(
            source,
            "q6apm_dai_pcm_free",
            "owner->mapped = false",
            "owner->mapped = true",
        ),
        mutate_function(
            source,
            "q6apm_compr_alloc_dma",
            "&map_generation",
            "NULL",
        ),
        mutate_function(
            source,
            "q6apm_compr_alloc_dma",
            "if (map_generation)",
            "if (!ret)",
        ),
        mutate_function(
            source,
            "q6apm_compr_free_dma",
            "prtd->memory_map_generation);",
            "q6apm_get_reset_generation(prtd->apm));",
        ),
        mutate_function(
            source,
            "q6apm_dai_memory_map",
            "&map_generation",
            "NULL",
        ),
        mutate_function(
            source,
            "q6apm_dai_memory_map",
            "owner->map_generation = map_generation",
            "owner->map_generation = q6apm_get_reset_generation(owner->apm)",
        ),
        mutate_function(
            source,
            "q6apm_pcm_cleanup_work",
            "map_generation);",
            "q6apm_get_reset_generation(owner->apm));",
        ),
        mutate_function(
            source,
            "q6apm_dai_pcm_free",
            "map_generation);",
            "q6apm_get_reset_generation(owner->apm));",
        ),
        mutate_function(
            source,
            "q6apm_dai_pcm_free",
            "owner->map_generation != map_generation",
            "false",
        ),
        mutate_function(
            source,
            "q6apm_pcm_unclaim_owner",
            "drop_prtd_ref = owner->prtd_ref",
            "drop_prtd_ref = true",
        ),
        mutate_function(
            source,
            "q6apm_dai_compr_open",
            "prtd->module_pinned = true;",
            "prtd->module_pinned = false;",
        ),
        mutate_function(
            source,
            "q6apm_pcm_create_owner",
            "owner->module_pinned = true;",
            "owner->module_pinned = false;",
        ),
        mutate_function(
            source,
            "q6apm_pcm_quarantine_close",
            "WRITE_ONCE(prtd->substream, NULL)",
            "/* missing callback-owner detachment */",
        ),
        mutate_function(
            source,
            "q6apm_pcm_quarantine_close",
            "candidate->substream == substream",
            "true",
        ),
        mutate_function(
            source,
            "q6apm_pcm_create_owner",
            "get_device(&substream->pcm->card->card_dev)",
            "&substream->pcm->card->card_dev",
        ),
        mutate_function(
            source,
            "q6apm_pcm_owner_release",
            "put_device(&owner->card->card_dev)",
            "/* leaked card reference */",
        ),
        mutate_function(
            source,
            "q6apm_dai_prepare",
            "ret = q6apm_free_fragments(prtd->graph, substream->stream)",
            "q6apm_free_fragments(prtd->graph, substream->stream)",
        ),
        mutate_function(
            source,
            "q6apm_dai_prepare",
            "reset_ret = q6apm_free_fragments(prtd->graph, substream->stream)",
            "q6apm_free_fragments(prtd->graph, substream->stream)",
        ),
        mutate_function(
            source,
            "q6apm_dai_prepare",
            "prtd->queue_ptr = READ_ONCE(runtime->control->appl_ptr)",
            "prtd->queue_ptr = 0",
        ),
        mutate_function(
            source,
            "q6apm_pcm_pending_frames",
            "*pending = runtime->boundary - queue_ptr + appl_ptr",
            "*pending = appl_ptr - queue_ptr",
        ),
        mutate_function(
            source,
            "q6apm_pcm_pending_frames",
            "if (*pending > runtime->buffer_size)",
            "if (false)",
        ),
        mutate_function(
            source,
            "q6apm_dai_ack",
            "next_queue = runtime->period_size - tail",
            "next_queue = prtd->queue_ptr + runtime->period_size",
        ),
        mutate_function(
            source,
            "q6apm_compr_schedule_drain_notify",
            "!prtd->error_pending",
            "true",
        ),
    )
    for index, mutation in enumerate(mutations, 1):
        if mutation == source:
            raise AssertionError(f"mutation {index} did not alter the source")
        if not verify(mutation, audioreach, q6apm):
            raise AssertionError(f"mutation {index} escaped the contract")

    audioreach_mutations = (
        mutate_function(
            audioreach,
            "audioreach_shared_memory_send_eos",
            "ret = 0;",
            "ret = 1;",
        ),
        mutate_function(
            audioreach,
            "audioreach_shared_memory_send_eos",
            "if (ret < 0)",
            "if (ret)",
        ),
    )
    for index, mutation in enumerate(audioreach_mutations, 1):
        if mutation == audioreach:
            raise AssertionError(f"AudioReach mutation {index} did not alter the source")
        if not verify(source, mutation, q6apm):
            raise AssertionError(f"AudioReach mutation {index} escaped the contract")

    q6apm_mutations = (
        q6apm.replace(
            "\t\t.suppress_bind_attrs = true,",
            "\t\t.suppress_bind_attrs = false,",
            1,
        ),
        mutate_function(
            q6apm,
            "q6apm_unbind_quiescent",
            "list_empty(&apm->mem_leases)",
            "true",
        ),
        mutate_function(
            q6apm,
            "apm_remove",
            "q6apm_retire_old_leases(apm);",
            "/* terminal leases retained */",
        ),
        mutate_function(
            q6apm,
            "q6apm_map_memory_fixed_region",
            "\t\tret = -ENETRESET;\n\t} else if (ret == -ETIMEDOUT",
            "\t\tret = 0;\n\t} else if (ret == -ETIMEDOUT",
        ),
        mutate_function(
            q6apm,
            "q6apm_get_audioreach_graph",
            "graph->graph_open_ambiguous = true",
            "graph->graph_open_ambiguous = false",
        ),
        mutate_function(
            q6apm,
            "q6apm_get_audioreach_graph",
            "graph->graph_quarantined = true",
            "graph->graph_quarantined = false",
        ),
        mutate_function(
            q6apm,
            "q6apm_get_audioreach_graph",
            "if (!READ_ONCE(apm->online) || graph->graph_reset_retired)",
            "if (false)",
        ),
        mutate_function(
            q6apm,
            "q6apm_get_audioreach_graph",
            "} else if (graph->graph_open_ambiguous &&",
            "} else if (true &&",
        ),
        mutate_function(
            q6apm,
            "q6apm_get_audioreach_graph",
            "READ_ONCE(apm->online) &&",
            "true &&",
        ),
        mutate_function(
            q6apm,
            "q6apm_get_audioreach_graph",
            "!q6apm_graph_result_ambiguous(ret)",
            "true",
        ),
        mutate_function(
            q6apm,
            "q6apm_apply_graph_result",
            "graph->graph_open_ambiguous) {",
            "true) {",
        ),
        mutate_function(
            q6apm,
            "q6apm_apply_graph_result",
            "graph->graph_open_ambiguous = false",
            "graph->graph_open_ambiguous = true",
        ),
        mutate_function(
            q6apm,
            "q6apm_apply_graph_result",
            "queue_release = true",
            "queue_release = false",
        ),
        mutate_function(
            q6apm,
            "q6apm_mark_offline_locked",
            "if (final_reset) {\n"
            "\t\t\tgraph->graph_open_ambiguous = false;",
            "if (true) {\n"
            "\t\t\tgraph->graph_open_ambiguous = false;",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "!q6apm_map_ticket_retained_locked(ticket))",
            "true)",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->lease->map_ticket != ticket ||",
            "ticket->lease->map_ticket == ticket ||",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->map_basic_status != result->status",
            "ticket->map_basic_status == result->status",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->map_basic_seen = true",
            "ticket->map_basic_seen = false",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "if (!result->status)\n\t\t\t\tgoto out;",
            "if (result->status)\n\t\t\t\tgoto out;",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "if (ticket->map_handle_seen)\n\t\t\t\tgoto protocol_error;",
            "if (ticket->map_handle_seen)\n\t\t\t\tgoto out;",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->lease->state = AUDIOREACH_MEM_MAP_UNCERTAIN;",
            "ticket->lease->state = AUDIOREACH_MEM_UNMAPPED;",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "if (!map_rsp->mem_map_handle)",
            "if (false)",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->response_opcode != APM_CMD_RSP_SHARED_MEM_MAP_REGIONS",
            "ticket->response_opcode != APM_CMD_RSP_GET_SPF_STATE",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "if (ticket->map_basic_seen && ticket->map_basic_status)",
            "if (false)",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->map_handle != map_rsp->mem_map_handle",
            "ticket->map_handle == map_rsp->mem_map_handle",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->lease->map_ticket == ticket &&\n"
            "\t\t    ticket->lease->generation == ticket->generation &&",
            "true &&\n\t\t    ticket->lease->generation == ticket->generation &&",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->map_handle_seen = true",
            "ticket->map_handle_seen = false",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "(ticket->lease->state == AUDIOREACH_MEM_MAP_PENDING ||\n"
            "\t\t     ticket->lease->state == AUDIOREACH_MEM_MAP_UNCERTAIN)",
            "ticket->lease->state == AUDIOREACH_MEM_MAP_PENDING",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->map_protocol_error = true",
            "ticket->map_protocol_error = false",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "ticket->kind != Q6APM_CMD_MAP ||\n"
            "\t\t     ticket->lease->map_ticket == ticket",
            "true ||\n\t\t     ticket->lease->map_ticket == ticket",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            " ||\n\t\t\t     ticket->lease->state == AUDIOREACH_MEM_MAPPED",
            "",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "if (completed && !ticket->waiter_active &&\n"
            "\t    ticket->state != Q6APM_CMD_AMBIGUOUS &&\n"
            "\t    !q6apm_map_ticket_retained_locked(ticket))",
            "if (completed && !ticket->waiter_active)",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "retire_leases = true",
            "retire_leases = false",
        ),
        mutate_function(
            q6apm,
            "apm_callback",
            "if (retire_leases)\n\t\tq6apm_retire_old_leases(apm)",
            "if (false)\n\t\tq6apm_retire_old_leases(apm)",
        ),
        mutate_function(
            q6apm,
            "q6apm_map_ticket_retained_locked",
            "lease->map_ticket == ticket",
            "true",
        ),
        mutate_function(
            q6apm,
            "q6apm_map_ticket_retained_locked",
            "lease->state != AUDIOREACH_MEM_UNMAPPED",
            "true",
        ),
        mutate_function(
            q6apm,
            "__q6apm_send_cmd_sync",
            "lease->map_ticket = ticket",
            "lease->map_ticket = NULL",
        ),
        mutate_function(
            q6apm,
            "__q6apm_send_cmd_sync",
            "if (ticket->state != Q6APM_CMD_AMBIGUOUS &&\n"
            "\t\t    !q6apm_map_ticket_retained_locked(ticket))",
            "if (ticket->state != Q6APM_CMD_AMBIGUOUS)",
        ),
        mutate_function(
            q6apm,
            "q6apm_lease_clear_locked",
            "lease->map_ticket = NULL",
            "/* retained stale MAP ticket */",
        ),
        mutate_function(
            q6apm,
            "q6apm_retire_old_leases",
            "ticket_lease->state != AUDIOREACH_MEM_RESET_RETIRED",
            "ticket_lease->state != AUDIOREACH_MEM_MAP_UNCERTAIN",
        ),
        mutate_function(
            q6apm,
            "q6apm_retire_old_leases",
            "ticket_lease->map_ticket = NULL",
            "/* retained stale lease pointer */",
        ),
        mutate_function(
            q6apm,
            "q6apm_unmap_memory_fixed_region",
            "q6apm_retire_old_leases(apm)",
            "/* retained terminal MAP transcript */",
        ),
        mutate_function(
            q6apm,
            "q6apm_mark_offline_locked",
            "ticket->lease->map_ticket = NULL",
            "/* dangling MAP ticket */",
        ),
    )
    for index, mutation in enumerate(q6apm_mutations, 1):
        if mutation == q6apm:
            raise AssertionError(f"q6apm mutation {index} did not alter the source")
        if not verify(source, audioreach, mutation):
            raise AssertionError(f"q6apm mutation {index} escaped the contract")


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    audioreach = AUDIOREACH_SOURCE.read_text(encoding="utf-8")
    q6apm = Q6APM_SOURCE.read_text(encoding="utf-8")
    errors = verify(source, audioreach, q6apm)
    if errors:
        raise SystemExit("\n".join(errors))
    run_models()
    run_mutations(source, audioreach, q6apm)
    print("Q6APM_COMPRESS_CONTRACT=PASS")


if __name__ == "__main__":
    main()
