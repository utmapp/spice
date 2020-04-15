/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>
#include <glib.h>

#include <spice/protocol.h>
#include <spice/qxl_dev.h>
#include <spice/stats.h>
#include <common/lz.h>
#include <common/rect.h>
#include <common/region.h>
#include <common/ring.h>

#include "display-channel.h"
#include "video-stream.h"

#include "spice.h"
#include "red-worker.h"
#include "red-qxl.h"
#include "cursor-channel.h"
#include "tree.h"
#include "red-record-qxl.h"

// compatibility for FreeBSD
#ifdef HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#define pthread_setname_np pthread_set_name_np
#endif

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 1

#define INF_EVENT_WAIT ~0

struct RedWorker {
    pthread_t thread;
    QXLInstance *qxl;
    SpiceWatch *dispatch_watch;
    SpiceCoreInterfaceInternal core;

    unsigned int event_timeout;

    DisplayChannel *display_channel;
    uint32_t display_poll_tries;
    gboolean was_blocked;

    CursorChannel *cursor_channel;
    uint32_t cursor_poll_tries;

    RedMemSlotInfo mem_slots;

    uint32_t process_display_generation;
    RedStatNode stat;
    RedStatCounter wakeup_counter;
    RedStatCounter command_counter;
    RedStatCounter full_loop_counter;
    RedStatCounter total_loop_counter;

    bool driver_cap_monitors_config;

    RedRecord *record;
    GMainLoop *loop;
};

static gboolean red_process_cursor_cmd(RedWorker *worker, const QXLCommandExt *ext)
{
    RedCursorCmd *cursor_cmd;

    cursor_cmd = red_cursor_cmd_new(worker->qxl, &worker->mem_slots,
                                    ext->group_id, ext->cmd.data);
    if (cursor_cmd == NULL) {
        return FALSE;
    }

    cursor_channel_process_cmd(worker->cursor_channel, cursor_cmd);
    red_cursor_cmd_unref(cursor_cmd);

    return TRUE;
}

static int red_process_cursor(RedWorker *worker, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;

    if (!red_qxl_is_running(worker->qxl)) {
        *ring_is_empty = TRUE;
        return n;
    }

    *ring_is_empty = FALSE;
    while (red_channel_max_pipe_size(RED_CHANNEL(worker->cursor_channel)) <= MAX_PIPE_SIZE) {
        if (!red_qxl_get_cursor_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;
            if (worker->cursor_poll_tries < CMD_RING_POLL_RETRIES) {
                worker->event_timeout = MIN(worker->event_timeout, CMD_RING_POLL_TIMEOUT);
            } else if (worker->cursor_poll_tries == CMD_RING_POLL_RETRIES &&
                       !red_qxl_req_cursor_notification(worker->qxl)) {
                continue;
            }
            worker->cursor_poll_tries++;
            return n;
        }

        if (worker->record) {
            red_record_qxl_command(worker->record, &worker->mem_slots, ext_cmd);
        }

        worker->cursor_poll_tries = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_CURSOR:
            red_process_cursor_cmd(worker, &ext_cmd);
            break;
        default:
            spice_warning("bad command type");
        }
        n++;
    }
    worker->was_blocked = TRUE;
    return n;
}

static gboolean red_process_surface_cmd(RedWorker *worker, QXLCommandExt *ext, gboolean loadvm)
{
    RedSurfaceCmd *surface_cmd;

    surface_cmd = red_surface_cmd_new(worker->qxl, &worker->mem_slots,
                                      ext->group_id, ext->cmd.data);
    if (surface_cmd == NULL) {
        return false;
    }
    display_channel_process_surface_cmd(worker->display_channel, surface_cmd, loadvm);
    red_surface_cmd_unref(surface_cmd);

    return true;
}

static int red_process_display(RedWorker *worker, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;
    uint64_t start = spice_get_monotonic_time_ns();

    if (!red_qxl_is_running(worker->qxl)) {
        *ring_is_empty = TRUE;
        return n;
    }

    stat_inc_counter(worker->total_loop_counter, 1);

    worker->process_display_generation++;
    *ring_is_empty = FALSE;
    while (red_channel_max_pipe_size(RED_CHANNEL(worker->display_channel)) <= MAX_PIPE_SIZE) {
        if (!red_qxl_get_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;
            if (worker->display_poll_tries < CMD_RING_POLL_RETRIES) {
                worker->event_timeout = MIN(worker->event_timeout, CMD_RING_POLL_TIMEOUT);
            } else if (worker->display_poll_tries == CMD_RING_POLL_RETRIES &&
                       !red_qxl_req_cmd_notification(worker->qxl)) {
                continue;
            }
            worker->display_poll_tries++;
            return n;
        }

        if (worker->record) {
            red_record_qxl_command(worker->record, &worker->mem_slots, ext_cmd);
        }

        stat_inc_counter(worker->command_counter, 1);
        worker->display_poll_tries = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_DRAW: {
            RedDrawable *red_drawable;
            red_drawable = red_drawable_new(worker->qxl, &worker->mem_slots,
                                            ext_cmd.group_id, ext_cmd.cmd.data,
                                            ext_cmd.flags); // returns with 1 ref

            if (red_drawable != NULL) {
                display_channel_process_draw(worker->display_channel, red_drawable,
                                             worker->process_display_generation);
                red_drawable_unref(red_drawable);
            }
            break;
        }
        case QXL_CMD_UPDATE: {
            RedUpdateCmd *update;

            update = red_update_cmd_new(worker->qxl, &worker->mem_slots,
                                        ext_cmd.group_id, ext_cmd.cmd.data);
            if (update == NULL) {
                break;
            }
            if (!display_channel_validate_surface(worker->display_channel, update->surface_id)) {
                spice_warning("Invalid surface in QXL_CMD_UPDATE");
            } else {
                display_channel_draw(worker->display_channel, &update->area, update->surface_id);
                red_qxl_notify_update(worker->qxl, update->update_id);
            }
            red_update_cmd_unref(update);
            break;
        }
        case QXL_CMD_MESSAGE: {
            RedMessage *message;

            message = red_message_new(worker->qxl, &worker->mem_slots,
                                      ext_cmd.group_id, ext_cmd.cmd.data);
            if (message == NULL) {
                break;
            }
#ifdef DEBUG
            spice_warning("MESSAGE: %.*s", message.len, message.data);
#endif
            red_message_unref(message);
            break;
        }
        case QXL_CMD_SURFACE:
            red_process_surface_cmd(worker, &ext_cmd, FALSE);
            break;

        default:
            spice_error("bad command type");
        }
        n++;
        if (red_channel_all_blocked(RED_CHANNEL(worker->display_channel))
            || spice_get_monotonic_time_ns() - start > NSEC_PER_SEC / 100) {
            worker->event_timeout = 0;
            return n;
        }
    }
    worker->was_blocked = TRUE;
    stat_inc_counter(worker->full_loop_counter, 1);
    return n;
}

static bool red_process_is_blocked(RedWorker *worker)
{
    return red_channel_max_pipe_size(RED_CHANNEL(worker->cursor_channel)) > MAX_PIPE_SIZE ||
           red_channel_max_pipe_size(RED_CHANNEL(worker->display_channel)) > MAX_PIPE_SIZE;
}

typedef int (*red_process_t)(RedWorker *worker, int *ring_is_empty);
static void flush_commands(RedWorker *worker, RedChannel *red_channel,
                           red_process_t process)
{
    for (;;) {
        uint64_t end_time;
        int ring_is_empty;

        process(worker, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (process(worker, &ring_is_empty)) {
            red_channel_push(red_channel);
        }

        if (ring_is_empty) {
            break;
        }
        end_time = spice_get_monotonic_time_ns() + COMMON_CLIENT_TIMEOUT;
        for (;;) {
            red_channel_push(red_channel);
            if (red_channel_max_pipe_size(red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            red_channel_receive(red_channel);
            red_channel_send(red_channel);
            // TODO: MC: the whole timeout will break since it takes lowest timeout, should
            // do it client by client.
            if (spice_get_monotonic_time_ns() >= end_time) {
                // TODO: we need to record the client that actually causes the timeout.
                // So we need to check the locations of the various pipe heads when counting,
                // and disconnect only those/that.
                spice_warning("flush timeout");
                red_channel_disconnect(red_channel);
            } else {
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

static void flush_display_commands(RedWorker *worker)
{
    flush_commands(worker, RED_CHANNEL(worker->display_channel),
                   red_process_display);
}

static void flush_cursor_commands(RedWorker *worker)
{
    flush_commands(worker, RED_CHANNEL(worker->cursor_channel),
                   red_process_cursor);
}

// TODO: on timeout, don't disconnect all channels immediately - try to disconnect the slowest ones
// first and maybe turn timeouts to several timeouts in order to disconnect channels gradually.
// Should use disconnect or shutdown?
static void flush_all_qxl_commands(RedWorker *worker)
{
    flush_display_commands(worker);
    flush_cursor_commands(worker);
}

static void handle_dev_update_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdateAsync *msg = payload;
    QXLRect *qxl_dirty_rects = NULL;
    uint32_t num_dirty_rects = 0;

    spice_return_if_fail(red_qxl_is_running(worker->qxl));
    spice_return_if_fail(qxl_get_interface(worker->qxl)->update_area_complete);

    flush_display_commands(worker);
    display_channel_update(worker->display_channel,
                           msg->surface_id, &msg->qxl_area, msg->clear_dirty_region,
                           &qxl_dirty_rects, &num_dirty_rects);

    red_qxl_update_area_complete(worker->qxl, msg->surface_id,
                                 qxl_dirty_rects, num_dirty_rects);
    g_free(qxl_dirty_rects);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_update(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdate *msg = payload;
    QXLRect *qxl_dirty_rects = msg->qxl_dirty_rects;

    spice_return_if_fail(red_qxl_is_running(worker->qxl));

    flush_display_commands(worker);
    display_channel_update(worker->display_channel,
                           msg->surface_id, msg->qxl_area, msg->clear_dirty_region,
                           &qxl_dirty_rects, &msg->num_dirty_rects);
    if (msg->qxl_dirty_rects == NULL) {
        g_free(qxl_dirty_rects);
    }
}

static void handle_dev_del_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageDelMemslot *msg = payload;
    uint32_t slot_id = msg->slot_id;
    uint32_t slot_group_id = msg->slot_group_id;

    memslot_info_del_slot(&worker->mem_slots, slot_group_id, slot_id);
}

static void handle_dev_destroy_surface_wait(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWait *msg = payload;
    RedWorker *worker = opaque;

    spice_return_if_fail(msg->surface_id == 0);

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
}

static void handle_dev_destroy_surfaces(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    flush_all_qxl_commands(worker);
    display_channel_destroy_surfaces(worker->display_channel);
    cursor_channel_reset(worker->cursor_channel);
}

static void dev_create_primary_surface(RedWorker *worker, uint32_t surface_id,
                                       QXLDevSurfaceCreate surface)
{
    DisplayChannel *display = worker->display_channel;
    uint8_t *line_0;

    spice_debug("trace");
    spice_warn_if_fail(surface_id == 0);
    spice_warn_if_fail(surface.height != 0);

    /* surface can arrive from guest unchecked so make sure
     * guest is not a malicious one and drop invalid requests
     */
    if (!red_validate_surface(surface.width, surface.height,
                              surface.stride, surface.format)) {
        spice_warning("wrong primary surface creation request");
        return;
    }

    line_0 = (uint8_t*)memslot_get_virt(&worker->mem_slots, surface.mem,
                                        surface.height * abs(surface.stride),
                                        surface.group_id);
    if (line_0 == NULL) {
        return;
    }
    if (worker->record) {
        red_record_primary_surface_create(worker->record,
                                          &surface, line_0);
    }

    if (surface.stride < 0) {
        line_0 -= (int32_t)(surface.stride * (surface.height -1));
    }

    display_channel_create_surface(display, 0, surface.width, surface.height, surface.stride, surface.format,
                                   line_0, surface.flags & QXL_SURF_FLAG_KEEP_DATA, TRUE);
    display_channel_set_monitors_config_to_primary(display);

    CommonGraphicsChannel *common = COMMON_GRAPHICS_CHANNEL(display);
    RedChannel *channel = RED_CHANNEL(display);
    if (red_channel_is_connected(channel) &&
        !common_graphics_channel_get_during_target_migrate(common)) {
        /* guest created primary, so it will (hopefully) send a monitors_config
         * now, don't send our own temporary one */
        if (!worker->driver_cap_monitors_config) {
            display_channel_push_monitors_config(display);
        }
        red_channel_pipes_add_empty_msg(channel, SPICE_MSG_DISPLAY_MARK);
        red_channel_push(channel);
    }

    cursor_channel_do_init(worker->cursor_channel);
}

static void handle_dev_create_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurface *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void destroy_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    DisplayChannel *display = worker->display_channel;

    if (!display_channel_validate_surface(display, surface_id)) {
        spice_warning("double destroy of primary surface");
        return;
    }
    spice_warn_if_fail(surface_id == 0);

    flush_all_qxl_commands(worker);
    display_channel_destroy_surface_wait(display, 0);
    display_channel_surface_unref(display, 0);

    /* FIXME: accessing private data only for warning purposes...
    spice_warn_if_fail(ring_is_empty(&display->streams));
    */
    spice_warn_if_fail(!display_channel_surface_has_canvas(display, surface_id));

    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_destroy_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurface *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    destroy_primary_surface(worker, surface_id);
}

static void handle_dev_destroy_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    destroy_primary_surface(worker, surface_id);
    red_qxl_destroy_primary_surface_complete(worker->qxl->st);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_flush_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageFlushSurfacesAsync *msg = payload;

    flush_all_qxl_commands(worker);
    display_channel_flush_all_surfaces(worker->display_channel);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_stop(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    spice_debug("stop");
    spice_assert(red_qxl_is_running(worker->qxl));

    red_qxl_set_running(worker->qxl, false);
    display_channel_update_qxl_running(worker->display_channel, false);

    display_channel_free_glz_drawables(worker->display_channel);
    display_channel_flush_all_surfaces(worker->display_channel);

    /* todo: when the waiting is expected to take long (slow connection and
     * overloaded pipe), don't wait, and in case of migration,
     * purge the pipe, send destroy_all_surfaces
     * to the client (there is no such message right now), and start
     * from scratch on the destination side */
    red_channel_wait_all_sent(RED_CHANNEL(worker->display_channel), COMMON_CLIENT_TIMEOUT);
    red_channel_wait_all_sent(RED_CHANNEL(worker->cursor_channel), COMMON_CLIENT_TIMEOUT);
}

static void handle_dev_start(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    spice_assert(!red_qxl_is_running(worker->qxl));
    if (worker->cursor_channel) {
        CommonGraphicsChannel *common = COMMON_GRAPHICS_CHANNEL(worker->cursor_channel);
        common_graphics_channel_set_during_target_migrate(common, FALSE);
    }
    if (worker->display_channel) {
        CommonGraphicsChannel *common = COMMON_GRAPHICS_CHANNEL(worker->display_channel);
        common_graphics_channel_set_during_target_migrate(common, FALSE);
        display_channel_wait_for_migrate_data(worker->display_channel);
    }
    red_qxl_set_running(worker->qxl, true);
    display_channel_update_qxl_running(worker->display_channel, true);
    worker->event_timeout = 0;
}

static void handle_dev_wakeup(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    stat_inc_counter(worker->wakeup_counter, 1);
    red_qxl_clear_pending(worker->qxl->st, RED_DISPATCHER_PENDING_WAKEUP);
}

static void handle_dev_oom(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    DisplayChannel *display = worker->display_channel;

    RedChannel *display_red_channel = RED_CHANNEL(display);
    int ring_is_empty;

    spice_return_if_fail(red_qxl_is_running(worker->qxl));
    // streams? but without streams also leak
    display_channel_debug_oom(display, "OOM1");
    while (red_process_display(worker, &ring_is_empty)) {
        red_channel_push(display_red_channel);
    }
    if (red_qxl_flush_resources(worker->qxl) == 0) {
        display_channel_free_some(worker->display_channel);
        red_qxl_flush_resources(worker->qxl);
    }
    display_channel_debug_oom(display, "OOM2");
    red_qxl_clear_pending(worker->qxl->st, RED_DISPATCHER_PENDING_OOM);
}

static void handle_dev_reset_cursor(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    cursor_channel_reset(worker->cursor_channel);
}

static void handle_dev_reset_image_cache(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    display_channel_reset_image_cache(worker->display_channel);
}

static void handle_dev_destroy_surface_wait_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWaitAsync *msg = payload;
    RedWorker *worker = opaque;

    display_channel_destroy_surface_wait(worker->display_channel, msg->surface_id);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_destroy_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageDestroySurfacesAsync *msg = payload;

    flush_all_qxl_commands(worker);
    display_channel_destroy_surfaces(worker->display_channel);
    cursor_channel_reset(worker->cursor_channel);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_create_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
    red_qxl_create_primary_surface_complete(worker->qxl->st, &msg->surface);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static inline uint32_t qxl_monitors_config_size(uint32_t heads)
{
    return sizeof(QXLMonitorsConfig) + sizeof(QXLHead) * heads;
}

static void handle_dev_monitors_config_async(void *opaque, void *payload)
{
    RedWorkerMessageMonitorsConfigAsync *msg = payload;
    RedWorker *worker = opaque;
    uint16_t count, max_allowed;
    QXLMonitorsConfig *dev_monitors_config =
        (QXLMonitorsConfig*)memslot_get_virt(&worker->mem_slots, msg->monitors_config,
                                             qxl_monitors_config_size(1),
                                             msg->group_id);

    if (dev_monitors_config == NULL) {
        /* TODO: raise guest bug (requires added QXL interface) */
        goto async_complete;
    }
    worker->driver_cap_monitors_config = true;
    count = dev_monitors_config->count;
    max_allowed = dev_monitors_config->max_allowed;
    if (count == 0) {
        spice_warning("ignoring an empty monitors config message from driver");
        goto async_complete;
    }
    if (count > max_allowed) {
        spice_warning("ignoring malformed monitors_config from driver, "
                      "count > max_allowed %d > %d",
                      count,
                      max_allowed);
        goto async_complete;
    }
    /* get pointer again to check virtual size */
    dev_monitors_config =
        (QXLMonitorsConfig*)memslot_get_virt(&worker->mem_slots, msg->monitors_config,
                                             qxl_monitors_config_size(count),
                                             msg->group_id);
    if (dev_monitors_config == NULL) {
        /* TODO: raise guest bug (requires added QXL interface) */
        goto async_complete;
    }
    display_channel_update_monitors_config(worker->display_channel, dev_monitors_config,
                                           MIN(count, msg->max_monitors),
                                           MIN(max_allowed, msg->max_monitors));
async_complete:
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_set_compression(void *opaque, void *payload)
{
    RedWorkerMessageSetCompression *msg = payload;
    RedWorker *worker = opaque;
    SpiceImageCompression image_compression = msg->image_compression;

    display_channel_set_image_compression(worker->display_channel, image_compression);

    display_channel_compress_stats_print(worker->display_channel);
    display_channel_compress_stats_reset(worker->display_channel);
}

static void handle_dev_set_streaming_video(void *opaque, void *payload)
{
    RedWorkerMessageSetStreamingVideo *msg = payload;
    RedWorker *worker = opaque;

    display_channel_set_stream_video(worker->display_channel, msg->streaming_video);
}

static void handle_dev_set_video_codecs(void *opaque, void *payload)
{
    RedWorkerMessageSetVideoCodecs *msg = payload;
    RedWorker *worker = opaque;

    display_channel_set_video_codecs(worker->display_channel, msg->video_codecs);
    g_array_unref(msg->video_codecs);
}

static void handle_dev_set_mouse_mode(void *opaque, void *payload)
{
    RedWorkerMessageSetMouseMode *msg = payload;
    RedWorker *worker = opaque;

    spice_debug("mouse mode %u", msg->mode);
    cursor_channel_set_mouse_mode(worker->cursor_channel, msg->mode);
}

static void dev_add_memslot(RedWorker *worker, QXLDevMemSlot mem_slot)
{
    memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                          mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                          mem_slot.generation);
}

static void handle_dev_add_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAddMemslot *msg = payload;
    QXLDevMemSlot mem_slot = msg->mem_slot;

    memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                          mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                          mem_slot.generation);
}

static void handle_dev_add_memslot_async(void *opaque, void *payload)
{
    RedWorkerMessageAddMemslotAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_add_memslot(worker, msg->mem_slot);
    red_qxl_async_complete(worker->qxl, msg->base.cookie);
}

static void handle_dev_reset_memslots(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    memslot_info_reset(&worker->mem_slots);
}

static void handle_dev_driver_unload(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    worker->driver_cap_monitors_config = false;
}

static
void handle_dev_gl_scanout(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    display_channel_gl_scanout(worker->display_channel);
}

static
void handle_dev_gl_draw_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageGlDraw *draw = payload;

    display_channel_gl_draw(worker->display_channel, &draw->draw);
}

static void handle_dev_close(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    g_main_loop_quit(worker->loop);
}

static bool loadvm_command(RedWorker *worker, QXLCommandExt *ext)
{
    switch (ext->cmd.type) {
    case QXL_CMD_CURSOR:
        return red_process_cursor_cmd(worker, ext);

    case QXL_CMD_SURFACE:
        return red_process_surface_cmd(worker, ext, TRUE);

    default:
        spice_warning("unhandled loadvm command type (%d)", ext->cmd.type);
    }

    return TRUE;
}

static void handle_dev_loadvm_commands(void *opaque, void *payload)
{
    RedWorkerMessageLoadvmCommands *msg = payload;
    RedWorker *worker = opaque;
    uint32_t i;
    uint32_t count = msg->count;
    QXLCommandExt *ext = msg->ext;

    spice_debug("loadvm_commands");
    for (i = 0 ; i < count ; ++i) {
        if (!loadvm_command(worker, &ext[i])) {
            /* XXX allow failure in loadvm? */
            spice_warning("failed loadvm command type (%d)", ext[i].cmd.type);
        }
    }
}

static void worker_dispatcher_record(void *opaque, uint32_t message_type, void *payload)
{
    RedWorker *worker = opaque;

    red_record_event(worker->record, 1, message_type);
}

static void register_callbacks(Dispatcher *dispatcher)
{
    /* TODO: register cursor & display specific msg in respective channel files */
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE,
                                handle_dev_update,
                                sizeof(RedWorkerMessageUpdate),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE_ASYNC,
                                handle_dev_update_async,
                                sizeof(RedWorkerMessageUpdateAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT,
                                handle_dev_add_memslot,
                                sizeof(RedWorkerMessageAddMemslot),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC,
                                handle_dev_add_memslot_async,
                                sizeof(RedWorkerMessageAddMemslotAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DEL_MEMSLOT,
                                handle_dev_del_memslot,
                                sizeof(RedWorkerMessageDelMemslot),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES,
                                handle_dev_destroy_surfaces,
                                sizeof(RedWorkerMessageDestroySurfaces),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC,
                                handle_dev_destroy_surfaces_async,
                                sizeof(RedWorkerMessageDestroySurfacesAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
                                handle_dev_destroy_primary_surface,
                                sizeof(RedWorkerMessageDestroyPrimarySurface),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC,
                                handle_dev_destroy_primary_surface_async,
                                sizeof(RedWorkerMessageDestroyPrimarySurfaceAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC,
                                handle_dev_create_primary_surface_async,
                                sizeof(RedWorkerMessageCreatePrimarySurfaceAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
                                handle_dev_create_primary_surface,
                                sizeof(RedWorkerMessageCreatePrimarySurface),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
                                handle_dev_reset_image_cache,
                                sizeof(RedWorkerMessageResetImageCache),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_CURSOR,
                                handle_dev_reset_cursor,
                                sizeof(RedWorkerMessageResetCursor),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_WAKEUP,
                                handle_dev_wakeup,
                                sizeof(RedWorkerMessageWakeup),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_OOM,
                                handle_dev_oom,
                                sizeof(RedWorkerMessageOom),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_START,
                                handle_dev_start,
                                sizeof(RedWorkerMessageStart),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC,
                                handle_dev_flush_surfaces_async,
                                sizeof(RedWorkerMessageFlushSurfacesAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_STOP,
                                handle_dev_stop,
                                sizeof(RedWorkerMessageStop),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_LOADVM_COMMANDS,
                                handle_dev_loadvm_commands,
                                sizeof(RedWorkerMessageLoadvmCommands),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_COMPRESSION,
                                handle_dev_set_compression,
                                sizeof(RedWorkerMessageSetCompression),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
                                handle_dev_set_streaming_video,
                                sizeof(RedWorkerMessageSetStreamingVideo),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_VIDEO_CODECS,
                                handle_dev_set_video_codecs,
                                sizeof(RedWorkerMessageSetVideoCodecs),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_MOUSE_MODE,
                                handle_dev_set_mouse_mode,
                                sizeof(RedWorkerMessageSetMouseMode),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
                                handle_dev_destroy_surface_wait,
                                sizeof(RedWorkerMessageDestroySurfaceWait),
                                true);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC,
                                handle_dev_destroy_surface_wait_async,
                                sizeof(RedWorkerMessageDestroySurfaceWaitAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_MEMSLOTS,
                                handle_dev_reset_memslots,
                                sizeof(RedWorkerMessageResetMemslots),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC,
                                handle_dev_monitors_config_async,
                                sizeof(RedWorkerMessageMonitorsConfigAsync),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DRIVER_UNLOAD,
                                handle_dev_driver_unload,
                                sizeof(RedWorkerMessageDriverUnload),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_GL_SCANOUT,
                                handle_dev_gl_scanout,
                                sizeof(RedWorkerMessageGlScanout),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_GL_DRAW_ASYNC,
                                handle_dev_gl_draw_async,
                                sizeof(RedWorkerMessageGlDraw),
                                false);
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CLOSE_WORKER,
                                handle_dev_close,
                                sizeof(RedWorkerMessageClose),
                                false);
}



typedef struct RedWorkerSource {
    GSource source;
    RedWorker *worker;
} RedWorkerSource;

static gboolean worker_source_prepare(GSource *source, gint *p_timeout)
{
    RedWorkerSource *wsource = SPICE_CONTAINEROF(source, RedWorkerSource, source);
    RedWorker *worker = wsource->worker;
    unsigned int timeout;

    timeout = MIN(worker->event_timeout,
                  display_channel_get_streams_timeout(worker->display_channel));

    *p_timeout = (timeout == INF_EVENT_WAIT) ? -1 : timeout;
    if (*p_timeout == 0)
        return TRUE;

    if (worker->was_blocked && !red_process_is_blocked(worker)) {
        return TRUE;
    }

    return FALSE;
}

static gboolean worker_source_check(GSource *source)
{
    RedWorkerSource *wsource = SPICE_CONTAINEROF(source, RedWorkerSource, source);
    RedWorker *worker = wsource->worker;

    return red_qxl_is_running(worker->qxl) /* TODO && worker->pending_process */;
}

static gboolean worker_source_dispatch(GSource *source, GSourceFunc callback,
                                       gpointer user_data)
{
    RedWorkerSource *wsource = SPICE_CONTAINEROF(source, RedWorkerSource, source);
    RedWorker *worker = wsource->worker;
    DisplayChannel *display = worker->display_channel;
    int ring_is_empty;

    /* during migration, in the dest, the display channel can be initialized
       while the global lz data not since migrate data msg hasn't been
       received yet */
    /* TODO: why is this here, and not in display_channel_create */
    display_channel_free_glz_drawables_to_free(display);

    /* TODO: could use its own source */
    video_stream_timeout(display);

    worker->event_timeout = INF_EVENT_WAIT;
    worker->was_blocked = FALSE;
    red_process_cursor(worker, &ring_is_empty);
    red_process_display(worker, &ring_is_empty);

    return TRUE;
}

/* cannot be const */
static GSourceFuncs worker_source_funcs = {
    .prepare = worker_source_prepare,
    .check = worker_source_check,
    .dispatch = worker_source_dispatch,
};

RedWorker* red_worker_new(QXLInstance *qxl)
{
    QXLDevInitInfo init_info;
    RedWorker *worker;
    Dispatcher *dispatcher;
    RedsState *reds = red_qxl_get_server(qxl->st);
    RedChannel *channel;

    red_qxl_get_init_info(qxl, &init_info);

    worker = g_new0(RedWorker, 1);
    worker->core = event_loop_core;
    worker->core.main_context = g_main_context_new();

    worker->record = reds_get_record(reds);
    dispatcher = red_qxl_get_dispatcher(qxl);
    dispatcher_set_opaque(dispatcher, worker);

    worker->qxl = qxl;
    register_callbacks(dispatcher);
    if (worker->record) {
        dispatcher_register_universal_handler(dispatcher, worker_dispatcher_record);
    }

    worker->driver_cap_monitors_config = false;
    char worker_str[SPICE_STAT_NODE_NAME_MAX];
    snprintf(worker_str, sizeof(worker_str), "display[%d]", worker->qxl->id & 0xff);
    stat_init_node(&worker->stat, reds, NULL, worker_str, TRUE);
    stat_init_counter(&worker->wakeup_counter, reds, &worker->stat, "wakeups", TRUE);
    stat_init_counter(&worker->command_counter, reds, &worker->stat, "commands", TRUE);
    stat_init_counter(&worker->full_loop_counter, reds, &worker->stat, "full_loops", TRUE);
    stat_init_counter(&worker->total_loop_counter, reds, &worker->stat, "total_loops", TRUE);

    worker->dispatch_watch =
        dispatcher_create_watch(dispatcher, &worker->core);
    spice_assert(worker->dispatch_watch != NULL);

    GSource *source = g_source_new(&worker_source_funcs, sizeof(RedWorkerSource));
    SPICE_CONTAINEROF(source, RedWorkerSource, source)->worker = worker;
    g_source_attach(source, worker->core.main_context);
    g_source_unref(source);

    memslot_info_init(&worker->mem_slots,
                      init_info.num_memslots_groups,
                      init_info.num_memslots,
                      init_info.memslot_gen_bits,
                      init_info.memslot_id_bits,
                      init_info.internal_groupslot_id);

    worker->event_timeout = INF_EVENT_WAIT;

    worker->cursor_channel = cursor_channel_new(reds, qxl->id,
                                                &worker->core, dispatcher);
    channel = RED_CHANNEL(worker->cursor_channel);
    red_channel_init_stat_node(channel, &worker->stat, "cursor_channel");

    // TODO: handle seamless migration. Temp, setting migrate to FALSE
    worker->display_channel = display_channel_new(reds, qxl, &worker->core, dispatcher,
                                                  FALSE,
                                                  reds_get_streaming_video(reds),
                                                  reds_get_video_codecs(reds),
                                                  init_info.n_surfaces);
    channel = RED_CHANNEL(worker->display_channel);
    red_channel_init_stat_node(channel, &worker->stat, "display_channel");
    display_channel_set_image_compression(worker->display_channel,
                                          spice_server_get_image_compression(reds));

    return worker;
}

static void *red_worker_main(void *arg)
{
    RedWorker *worker = arg;

    spice_debug("begin");
#if defined(__APPLE__)
    pthread_setname_np("SPICE Worker");
#endif
    SPICE_VERIFY(MAX_PIPE_SIZE > WIDE_CLIENT_ACK_WINDOW &&
           MAX_PIPE_SIZE > NARROW_CLIENT_ACK_WINDOW); //ensure wakeup by ack message

    red_channel_reset_thread_id(RED_CHANNEL(worker->cursor_channel));
    red_channel_reset_thread_id(RED_CHANNEL(worker->display_channel));

    GMainLoop *loop = g_main_loop_new(worker->core.main_context, FALSE);
    worker->loop = loop;
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    worker->loop = NULL;

    return NULL;
}

bool red_worker_run(RedWorker *worker)
{
#ifndef _WIN32
    sigset_t thread_sig_mask;
    sigset_t curr_sig_mask;
#endif
    int r;

    spice_return_val_if_fail(worker, FALSE);
    spice_return_val_if_fail(!worker->thread, FALSE);

#ifndef _WIN32
    sigfillset(&thread_sig_mask);
    sigdelset(&thread_sig_mask, SIGILL);
    sigdelset(&thread_sig_mask, SIGFPE);
    sigdelset(&thread_sig_mask, SIGSEGV);
    pthread_sigmask(SIG_SETMASK, &thread_sig_mask, &curr_sig_mask);
#endif
    if ((r = pthread_create(&worker->thread, NULL, red_worker_main, worker))) {
        spice_error("create thread failed %d", r);
    }
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &curr_sig_mask, NULL);
#endif
#if !defined(__APPLE__)
    pthread_setname_np(worker->thread, "SPICE Worker");
#endif

    return r == 0;
}

static void red_worker_close_channel(RedChannel *channel)
{
    red_channel_reset_thread_id(channel);
    red_channel_destroy(channel);
}

/*
 * Free the worker thread. This function should be called by RedQxl
 * after sending a RED_WORKER_MESSAGE_CLOSE_WORKER message;
 * failing to do so will cause a deadlock.
 */
void red_worker_free(RedWorker *worker)
{
    pthread_join(worker->thread, NULL);

    red_worker_close_channel(RED_CHANNEL(worker->cursor_channel));
    worker->cursor_channel = NULL;
    red_worker_close_channel(RED_CHANNEL(worker->display_channel));
    worker->display_channel = NULL;

    if (worker->dispatch_watch) {
        red_watch_remove(worker->dispatch_watch);
    }

    g_main_context_unref(worker->core.main_context);

    if (worker->record) {
        red_record_unref(worker->record);
    }
    memslot_info_destroy(&worker->mem_slots);
    g_free(worker);
}
