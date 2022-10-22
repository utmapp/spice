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

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#endif

#include <common/generated_server_marshallers.h>
#include <common/snd_codec.h>

#include "spice.h"
#include "red-common.h"
#include "main-channel.h"
#include "reds.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "sound.h"
#include "main-channel-client.h"

#define SND_RECEIVE_BUF_SIZE     (16 * 1024 * 2)
#define RECORD_SAMPLES_SIZE (SND_RECEIVE_BUF_SIZE >> 2)

enum SndCommand {
    SND_MIGRATE,
    SND_CTRL,
    SND_VOLUME,
    SND_MUTE,
    SND_END_COMMAND,
};

enum PlaybackCommand {
    SND_PLAYBACK_MODE = SND_END_COMMAND,
    SND_PLAYBACK_PCM,
    SND_PLAYBACK_LATENCY,
};

#define SND_MIGRATE_MASK (1 << SND_MIGRATE)
#define SND_CTRL_MASK (1 << SND_CTRL)
#define SND_VOLUME_MASK (1 << SND_VOLUME)
#define SND_MUTE_MASK (1 << SND_MUTE)
#define SND_VOLUME_MUTE_MASK (SND_VOLUME_MASK|SND_MUTE_MASK)

#define SND_PLAYBACK_MODE_MASK (1 << SND_PLAYBACK_MODE)
#define SND_PLAYBACK_PCM_MASK (1 << SND_PLAYBACK_PCM)
#define SND_PLAYBACK_LATENCY_MASK ( 1 << SND_PLAYBACK_LATENCY)

typedef struct SndChannelClient SndChannelClient;
typedef struct SndChannel SndChannel;
typedef struct PlaybackChannelClient PlaybackChannelClient;
typedef struct RecordChannelClient RecordChannelClient;
typedef struct AudioFrame AudioFrame;
typedef struct AudioFrameContainer AudioFrameContainer;
typedef struct SpicePlaybackState PlaybackChannel;
typedef struct SpiceRecordState RecordChannel;

typedef void (*snd_channel_on_message_done_proc)(SndChannelClient *client);


#define TYPE_SND_CHANNEL_CLIENT snd_channel_client_get_type()
#define SND_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_SND_CHANNEL_CLIENT, SndChannelClient))
GType snd_channel_client_get_type(void) G_GNUC_CONST;

/* Connects an audio client to a Spice client */
struct SndChannelClient {
    RedChannelClient parent;

    bool active;
    bool client_active;

    uint32_t command;

    /* we don't expect very big messages so don't allocate too much
     * bytes, data will be cached in RecordChannelClient::samples */
    uint8_t receive_buf[SND_CODEC_MAX_FRAME_BYTES + 64];
    RedPipeItem persistent_pipe_item;

    snd_channel_on_message_done_proc on_message_done;
};

typedef struct SndChannelClientClass {
    RedChannelClientClass parent_class;
} SndChannelClientClass;

static void playback_channel_client_initable_interface_init(GInitableIface *iface);
static void record_channel_client_initable_interface_init(GInitableIface *iface);
static GInitableIface *playback_channel_client_parent_initable_iface;
static GInitableIface *record_channel_client_parent_initable_iface;

G_DEFINE_TYPE(SndChannelClient, snd_channel_client, RED_TYPE_CHANNEL_CLIENT)


enum {
    RED_PIPE_ITEM_PERSISTENT = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
};


struct AudioFrame {
    uint32_t time;
    uint32_t samples[SND_CODEC_MAX_FRAME_SIZE];
    PlaybackChannelClient *client;
    AudioFrame *next;
    AudioFrameContainer *container;
    bool allocated;
};

#define NUM_AUDIO_FRAMES 3
struct AudioFrameContainer
{
    int refs;
    AudioFrame items[NUM_AUDIO_FRAMES];
};

#define TYPE_PLAYBACK_CHANNEL_CLIENT playback_channel_client_get_type()
#define PLAYBACK_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_PLAYBACK_CHANNEL_CLIENT, PlaybackChannelClient))
GType playback_channel_client_get_type(void) G_GNUC_CONST;

struct PlaybackChannelClient {
    SndChannelClient parent;

    AudioFrameContainer *frames;
    AudioFrame *free_frames;
    AudioFrame *in_progress;   /* Frame being sent to the client */
    AudioFrame *pending_frame; /* Next frame to send to the client */
    uint32_t mode;
    uint32_t latency;
    SndCodec codec;
    uint8_t  encode_buf[SND_CODEC_MAX_COMPRESSED_BYTES];
};

typedef struct PlaybackChannelClientClass {
    SndChannelClientClass parent_class;
} PlaybackChannelClientClass;

G_DEFINE_TYPE_WITH_CODE(PlaybackChannelClient, playback_channel_client, TYPE_SND_CHANNEL_CLIENT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE,
                                              playback_channel_client_initable_interface_init))

typedef struct SpiceVolumeState {
    uint16_t *volume;
    uint8_t volume_nchannels;
    int mute;
} SpiceVolumeState;

#define TYPE_SND_CHANNEL snd_channel_get_type()
#define SND_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_SND_CHANNEL, SndChannel))
GType snd_channel_get_type(void) G_GNUC_CONST;

/* Base class for SpicePlaybackState and SpiceRecordState */
struct SndChannel {
    RedChannel parent;

    bool active;
    SpiceVolumeState volume;
    uint32_t frequency;
};

typedef struct SndChannelClass {
    RedChannelClass parent_class;
} SndChannelClass;

G_DEFINE_TYPE(SndChannel, snd_channel, RED_TYPE_CHANNEL)


#define TYPE_PLAYBACK_CHANNEL playback_channel_get_type()
#define PLAYBACK_CHANNEL(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_PLAYBACK_CHANNEL, PlaybackChannel))
GType playback_channel_get_type(void) G_GNUC_CONST;

struct SpicePlaybackState {
    SndChannel channel;
};

typedef struct PlaybackChannelClass {
    SndChannelClass parent_class;
} PlaybackChannelClass;

G_DEFINE_TYPE(PlaybackChannel, playback_channel, TYPE_SND_CHANNEL)


#define TYPE_RECORD_CHANNEL record_channel_get_type()
#define RECORD_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_RECORD_CHANNEL, RecordChannel))
GType record_channel_get_type(void) G_GNUC_CONST;

struct SpiceRecordState {
    SndChannel channel;
};

typedef struct RecordChannelClass {
    SndChannelClass parent_class;
} RecordChannelClass;

G_DEFINE_TYPE(RecordChannel, record_channel, TYPE_SND_CHANNEL)


#define TYPE_RECORD_CHANNEL_CLIENT record_channel_client_get_type()
#define RECORD_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_RECORD_CHANNEL_CLIENT, RecordChannelClient))
GType record_channel_client_get_type(void) G_GNUC_CONST;

struct RecordChannelClient {
    SndChannelClient parent;
    uint32_t samples[RECORD_SAMPLES_SIZE];
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t mode;
    uint32_t mode_time;
    uint32_t start_time;
    SndCodec codec;
    uint8_t  decode_buf[SND_CODEC_MAX_FRAME_BYTES];
};

typedef struct RecordChannelClientClass {
    SndChannelClientClass parent_class;
} RecordChannelClientClass;

G_DEFINE_TYPE_WITH_CODE(RecordChannelClient, record_channel_client, TYPE_SND_CHANNEL_CLIENT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_INITABLE,
                                              record_channel_client_initable_interface_init))


/* A list of all Spice{Playback,Record}State objects */
static GList *snd_channels;

static void snd_send(SndChannelClient * client);

/* sound channels only support a single client */
static SndChannelClient *snd_channel_get_client(SndChannel *channel)
{
    GList *clients = red_channel_get_clients(RED_CHANNEL(channel));
    if (clients == NULL) {
        return NULL;
    }

    return clients->data;
}

static RedsState* snd_channel_get_server(SndChannelClient *client)
{
    g_return_val_if_fail(client != NULL, NULL);
    return red_channel_get_server(red_channel_client_get_channel(RED_CHANNEL_CLIENT(client)));
}

static void snd_playback_free_frame(PlaybackChannelClient *playback_client, AudioFrame *frame)
{
    frame->client = playback_client;
    frame->next = playback_client->free_frames;
    playback_client->free_frames = frame;
}

static void snd_playback_on_message_done(SndChannelClient *client)
{
    PlaybackChannelClient *playback_client = (PlaybackChannelClient *)client;
    if (playback_client->in_progress) {
        snd_playback_free_frame(playback_client, playback_client->in_progress);
        playback_client->in_progress = NULL;
        if (playback_client->pending_frame) {
            client->command |= SND_PLAYBACK_PCM_MASK;
            snd_send(client);
        }
    }
}

static bool snd_record_handle_write(RecordChannelClient *record_client, size_t size, void *message)
{
    SpiceMsgcRecordPacket *packet;
    uint32_t write_pos;
    uint8_t* data;
    uint32_t len;
    uint32_t now;

    if (!record_client) {
        return false;
    }

    packet = (SpiceMsgcRecordPacket *)message;

    if (record_client->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        data = packet->data;
        size = packet->data_size >> 2;
        size = MIN(size, RECORD_SAMPLES_SIZE);
     } else {
        int decode_size;
        decode_size = sizeof(record_client->decode_buf);
        if (snd_codec_decode(record_client->codec, packet->data, packet->data_size,
                    record_client->decode_buf, &decode_size) != SND_CODEC_OK)
            return false;
        data = record_client->decode_buf;
        size = decode_size >> 2;
    }

    write_pos = record_client->write_pos % RECORD_SAMPLES_SIZE;
    record_client->write_pos += size;
    len = RECORD_SAMPLES_SIZE - write_pos;
    now = MIN(len, size);
    size -= now;
    memcpy(record_client->samples + write_pos, data, now << 2);

    if (size) {
        memcpy(record_client->samples, data + (now << 2), size << 2);
    }

    if (record_client->write_pos - record_client->read_pos > RECORD_SAMPLES_SIZE) {
        record_client->read_pos = record_client->write_pos - RECORD_SAMPLES_SIZE;
    }
    return true;
}

static
const char* spice_audio_data_mode_to_string(gint mode)
{
    static const char * const str[] = {
        [ SPICE_AUDIO_DATA_MODE_INVALID ] = "invalid",
        [ SPICE_AUDIO_DATA_MODE_RAW ] = "raw",
        [ SPICE_AUDIO_DATA_MODE_CELT_0_5_1 ] = "celt",
        [ SPICE_AUDIO_DATA_MODE_OPUS ] = "opus",
    };
    if (mode >= 0 && mode < G_N_ELEMENTS(str)) {
        return str[mode];
    }

    return "unknown audio codec";
}

static bool
record_channel_handle_message(RedChannelClient *rcc, uint16_t type, uint32_t size, void *message)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(rcc);

    switch (type) {
    case SPICE_MSGC_RECORD_DATA:
        return snd_record_handle_write(record_client, size, message);
    case SPICE_MSGC_RECORD_MODE: {
        SpiceMsgcRecordMode *mode = (SpiceMsgcRecordMode *)message;
        SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
        record_client->mode_time = mode->time;
        if (mode->mode != SPICE_AUDIO_DATA_MODE_RAW) {
            if (snd_codec_is_capable(mode->mode, channel->frequency)) {
                if (snd_codec_create(&record_client->codec, mode->mode, channel->frequency,
                                     SND_CODEC_DECODE) == SND_CODEC_OK) {
                    record_client->mode = mode->mode;
                } else {
                    red_channel_warning(red_channel_client_get_channel(rcc),
                                        "create decoder failed");
                    return false;
                }
            }
            else {
                red_channel_warning(red_channel_client_get_channel(rcc),
                                    "unsupported mode %d",
                                    record_client->mode);
                return false;
            }
        }
        else
            record_client->mode = mode->mode;

        spice_debug("record client %p using mode %s", record_client,
                    spice_audio_data_mode_to_string(record_client->mode));
        break;
    }

    case SPICE_MSGC_RECORD_START_MARK: {
        SpiceMsgcRecordStartMark *mark = (SpiceMsgcRecordStartMark *)message;
        record_client->start_time = mark->time;
        break;
    }
    default:
        return red_channel_client_handle_message(rcc, type, size, message);
    }
    return true;
}

static bool snd_channel_send_migrate(SndChannelClient *client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgMigrate migrate;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE);
    migrate.flags = 0;
    spice_marshall_msg_migrate(m, &migrate);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool snd_playback_send_migrate(PlaybackChannelClient *client)
{
    return snd_channel_send_migrate(SND_CHANNEL_CLIENT(client));
}

static bool snd_send_volume(SndChannelClient *client, uint32_t cap, int msg)
{
    SpiceMsgAudioVolume *vol;
    uint8_t c;
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
    SpiceVolumeState *st = &channel->volume;

    if (!red_channel_client_test_remote_cap(rcc, cap)) {
        return false;
    }

    vol = alloca(sizeof (SpiceMsgAudioVolume) +
                 st->volume_nchannels * sizeof (uint16_t));
    red_channel_client_init_send_data(rcc, msg);
    vol->nchannels = st->volume_nchannels;
    for (c = 0; c < st->volume_nchannels; ++c) {
        vol->volume[c] = st->volume[c];
    }
    spice_marshall_SpiceMsgAudioVolume(m, vol);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool snd_playback_send_volume(PlaybackChannelClient *playback_client)
{
    return snd_send_volume(SND_CHANNEL_CLIENT(playback_client), SPICE_PLAYBACK_CAP_VOLUME,
                           SPICE_MSG_PLAYBACK_VOLUME);
}

static bool snd_send_mute(SndChannelClient *client, uint32_t cap, int msg)
{
    SpiceMsgAudioMute mute;
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
    SpiceVolumeState *st = &channel->volume;

    if (!red_channel_client_test_remote_cap(rcc, cap)) {
        return false;
    }

    red_channel_client_init_send_data(rcc, msg);
    mute.mute = st->mute;
    spice_marshall_SpiceMsgAudioMute(m, &mute);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool snd_playback_send_mute(PlaybackChannelClient *playback_client)
{
    return snd_send_mute(SND_CHANNEL_CLIENT(playback_client), SPICE_PLAYBACK_CAP_VOLUME,
                         SPICE_MSG_PLAYBACK_MUTE);
}

static bool snd_playback_send_latency(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgPlaybackLatency latency_msg;

    spice_debug("latency %u", playback_client->latency);
    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_LATENCY);
    latency_msg.latency_ms = playback_client->latency;
    spice_marshall_msg_playback_latency(m, &latency_msg);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool snd_playback_send_start(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgPlaybackStart start;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_START);
    start.channels = SPICE_INTERFACE_PLAYBACK_CHAN;
    start.frequency = SND_CHANNEL(red_channel_client_get_channel(rcc))->frequency;
    spice_assert(SPICE_INTERFACE_PLAYBACK_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    start.time = reds_get_mm_time();
    spice_marshall_msg_playback_start(m, &start);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool snd_playback_send_stop(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_STOP);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static int snd_playback_send_ctl(PlaybackChannelClient *playback_client)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(playback_client);

    if ((client->client_active = client->active)) {
        return snd_playback_send_start(playback_client);
    } else {
        return snd_playback_send_stop(playback_client);
    }
}

static bool snd_record_send_start(RecordChannelClient *record_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(record_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgRecordStart start;

    red_channel_client_init_send_data(rcc, SPICE_MSG_RECORD_START);

    start.channels = SPICE_INTERFACE_RECORD_CHAN;
    start.frequency = SND_CHANNEL(red_channel_client_get_channel(rcc))->frequency;
    spice_assert(SPICE_INTERFACE_RECORD_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    spice_marshall_msg_record_start(m, &start);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool snd_record_send_stop(RecordChannelClient *record_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(record_client);

    red_channel_client_init_send_data(rcc, SPICE_MSG_RECORD_STOP);

    red_channel_client_begin_send_message(rcc);
    return true;
}

static int snd_record_send_ctl(RecordChannelClient *record_client)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(record_client);

    if ((client->client_active = client->active)) {
        return snd_record_send_start(record_client);
    } else {
        return snd_record_send_stop(record_client);
    }
}

static bool snd_record_send_volume(RecordChannelClient *record_client)
{
    return snd_send_volume(SND_CHANNEL_CLIENT(record_client), SPICE_RECORD_CAP_VOLUME,
                           SPICE_MSG_RECORD_VOLUME);
}

static bool snd_record_send_mute(RecordChannelClient *record_client)
{
    return snd_send_mute(SND_CHANNEL_CLIENT(record_client), SPICE_RECORD_CAP_VOLUME,
                         SPICE_MSG_RECORD_MUTE);
}

static bool snd_record_send_migrate(RecordChannelClient *record_client)
{
    /* No need for migration data: if recording has started before migration,
     * the client receives RECORD_STOP from the src before the migration completion
     * notification (when the vm is stopped).
     * Afterwards, when the vm starts on the dest, the client receives RECORD_START. */
    return snd_channel_send_migrate(SND_CHANNEL_CLIENT(record_client));
}

static bool snd_playback_send_write(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    AudioFrame *frame;
    SpiceMsgPlaybackPacket msg;
    RedPipeItem *pipe_item = &SND_CHANNEL_CLIENT(playback_client)->persistent_pipe_item;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_DATA);

    frame = playback_client->in_progress;
    msg.time = frame->time;

    spice_marshall_msg_playback_data(m, &msg);

    if (playback_client->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        spice_marshaller_add_by_ref_full(m, (uint8_t *)frame->samples,
                                         snd_codec_frame_size(playback_client->codec) *
                                         sizeof(frame->samples[0]),
                                         marshaller_unref_pipe_item, pipe_item);
    }
    else {
        int n = sizeof(playback_client->encode_buf);
        if (snd_codec_encode(playback_client->codec, (uint8_t *) frame->samples,
                                    snd_codec_frame_size(playback_client->codec) * sizeof(frame->samples[0]),
                                    playback_client->encode_buf, &n) != SND_CODEC_OK) {
            red_channel_warning(red_channel_client_get_channel(rcc), "encode failed");
            red_channel_client_disconnect(rcc);
            return false;
        }
        spice_marshaller_add_by_ref_full(m, playback_client->encode_buf, n,
                                         marshaller_unref_pipe_item, pipe_item);
    }

    red_channel_client_begin_send_message(rcc);
    return true;
}

static bool playback_send_mode(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgPlaybackMode mode;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_MODE);
    mode.time = reds_get_mm_time();
    mode.mode = playback_client->mode;
    spice_marshall_msg_playback_mode(m, &mode);

    red_channel_client_begin_send_message(rcc);
    return true;
}

/* This function is called when the "persistent" item is removed from the
 * queue. Note that there is not free call as the item is allocated into
 * SndChannelClient.
 * This is used to have a simple item in RedChannelClient queue but to send
 * multiple messages in a row if possible.
 * During realtime sound transmission you usually don't want to queue too
 * much data or having retransmission preferring instead loosing some
 * samples.
 */
static void snd_persistent_pipe_item_free(struct RedPipeItem *item)
{
    SndChannelClient *client = SPICE_CONTAINEROF(item, SndChannelClient, persistent_pipe_item);

    red_pipe_item_init_full(item, RED_PIPE_ITEM_PERSISTENT,
                            snd_persistent_pipe_item_free);

    if (client->on_message_done) {
        client->on_message_done(client);
    }
}

static void snd_send(SndChannelClient * client)
{
    RedChannelClient *rcc;

    g_return_if_fail(RED_IS_CHANNEL_CLIENT(client));

    rcc = RED_CHANNEL_CLIENT(client);
    if (!red_channel_client_pipe_is_empty(rcc) || !client->command) {
        return;
    }
    // just append a dummy item and push!
    red_pipe_item_init_full(&client->persistent_pipe_item, RED_PIPE_ITEM_PERSISTENT,
                            snd_persistent_pipe_item_free);
    red_channel_client_pipe_add_push(rcc, &client->persistent_pipe_item);
}

static void playback_channel_send_item(RedChannelClient *rcc, G_GNUC_UNUSED RedPipeItem *item)
{
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(rcc);
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);

    client->command &= SND_PLAYBACK_MODE_MASK|SND_PLAYBACK_PCM_MASK|
                       SND_CTRL_MASK|SND_VOLUME_MUTE_MASK|
                       SND_MIGRATE_MASK|SND_PLAYBACK_LATENCY_MASK;
    while (client->command) {
        if (client->command & SND_PLAYBACK_MODE_MASK) {
            client->command &= ~SND_PLAYBACK_MODE_MASK;
            if (playback_send_mode(playback_client)) {
                break;
            }
        }
        if (client->command & SND_PLAYBACK_PCM_MASK) {
            spice_assert(!playback_client->in_progress && playback_client->pending_frame);
            playback_client->in_progress = playback_client->pending_frame;
            playback_client->pending_frame = NULL;
            client->command &= ~SND_PLAYBACK_PCM_MASK;
            if (snd_playback_send_write(playback_client)) {
                break;
            }
            red_channel_warning(red_channel_client_get_channel(rcc),
                                "snd_send_playback_write failed");
        }
        if (client->command & SND_CTRL_MASK) {
            client->command &= ~SND_CTRL_MASK;
            if (snd_playback_send_ctl(playback_client)) {
                break;
            }
        }
        if (client->command & SND_VOLUME_MASK) {
            client->command &= ~SND_VOLUME_MASK;
            if (snd_playback_send_volume(playback_client)) {
                break;
            }
        }
        if (client->command & SND_MUTE_MASK) {
            client->command &= ~SND_MUTE_MASK;
            if (snd_playback_send_mute(playback_client)) {
                break;
            }
        }
        if (client->command & SND_MIGRATE_MASK) {
            client->command &= ~SND_MIGRATE_MASK;
            if (snd_playback_send_migrate(playback_client)) {
                break;
            }
        }
        if (client->command & SND_PLAYBACK_LATENCY_MASK) {
            client->command &= ~SND_PLAYBACK_LATENCY_MASK;
            if (snd_playback_send_latency(playback_client)) {
                break;
            }
        }
    }
    snd_send(client);
}

static void record_channel_send_item(RedChannelClient *rcc, G_GNUC_UNUSED RedPipeItem *item)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(rcc);
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);

    client->command &= SND_CTRL_MASK|SND_VOLUME_MUTE_MASK|SND_MIGRATE_MASK;
    while (client->command) {
        if (client->command & SND_CTRL_MASK) {
            client->command &= ~SND_CTRL_MASK;
            if (snd_record_send_ctl(record_client)) {
                break;
            }
        }
        if (client->command & SND_VOLUME_MASK) {
            client->command &= ~SND_VOLUME_MASK;
            if (snd_record_send_volume(record_client)) {
                break;
            }
        }
        if (client->command & SND_MUTE_MASK) {
            client->command &= ~SND_MUTE_MASK;
            if (snd_record_send_mute(record_client)) {
                break;
            }
        }
        if (client->command & SND_MIGRATE_MASK) {
            client->command &= ~SND_MIGRATE_MASK;
            if (snd_record_send_migrate(record_client)) {
                break;
            }
        }
    }
    snd_send(client);
}

static bool snd_channel_client_config_socket(RedChannelClient *rcc)
{
    RedStream *stream = red_channel_client_get_stream(rcc);
    RedClient *red_client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc = red_client_get_main(red_client);

#ifdef SO_PRIORITY
    int priority = 6;
    if (setsockopt(stream->socket, SOL_SOCKET, SO_PRIORITY, (void*)&priority,
                   sizeof(priority)) == -1) {
        if (errno != ENOTSUP) {
            red_channel_warning(red_channel_client_get_channel(rcc),
                                "setsockopt failed, %s", strerror(errno));
        }
    }
#endif

#ifdef IPTOS_LOWDELAY
    int tos = IPTOS_LOWDELAY;
    if (setsockopt(stream->socket, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) == -1) {
        if (errno != ENOTSUP) {
            red_channel_warning(red_channel_client_get_channel(rcc),
                                "setsockopt failed, %s",
                                strerror(errno));
        }
    }
#endif

    red_stream_set_no_delay(stream, !main_channel_client_is_low_bandwidth(mcc));

    return true;
}

static uint8_t*
snd_channel_client_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);
    // If message is too big allocate one, this should never happen
    if (size > sizeof(client->receive_buf)) {
        return g_malloc(size);
    }
    return client->receive_buf;
}

static void
snd_channel_client_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size,
                                    uint8_t *msg)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);
    if (msg != client->receive_buf) {
        g_free(msg);
    }
}

static void snd_set_command(SndChannelClient *client, uint32_t command)
{
    if (!client) {
        return;
    }
    client->command |= command;
}

static void snd_channel_set_volume(SndChannel *channel,
                                   uint8_t nchannels, uint16_t *volume)
{
    SpiceVolumeState *st = &channel->volume;
    SndChannelClient *client = snd_channel_get_client(channel);

    st->volume_nchannels = nchannels;
    g_free(st->volume);
    st->volume = g_memdup(volume, sizeof(uint16_t) * nchannels);

    if (!client || nchannels == 0)
        return;

    snd_set_command(client, SND_VOLUME_MASK);
    snd_send(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_volume(SpicePlaybackInstance *sin,
                                                  uint8_t nchannels,
                                                  uint16_t *volume)
{
    snd_channel_set_volume(&sin->st->channel, nchannels, volume);
}

static void snd_channel_set_mute(SndChannel *channel, uint8_t mute)
{
    SpiceVolumeState *st = &channel->volume;
    SndChannelClient *client = snd_channel_get_client(channel);

    st->mute = mute;

    if (!client)
        return;

    snd_set_command(client, SND_MUTE_MASK);
    snd_send(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_mute(SpicePlaybackInstance *sin, uint8_t mute)
{
    snd_channel_set_mute(&sin->st->channel, mute);
}

static void snd_channel_client_start(SndChannelClient *client)
{
    spice_assert(!client->active);
    client->active = true;
    if (!client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

static void playback_channel_client_start(SndChannelClient *client)
{
    if (!client) {
        return;
    }

    reds_disable_mm_time(snd_channel_get_server(client));
    snd_channel_client_start(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_start(SpicePlaybackInstance *sin)
{
    SndChannel *channel = &sin->st->channel;
    channel->active = true;
    return playback_channel_client_start(snd_channel_get_client(channel));
}

SPICE_GNUC_VISIBLE void spice_server_playback_stop(SpicePlaybackInstance *sin)
{
    SndChannelClient *client = snd_channel_get_client(&sin->st->channel);

    sin->st->channel.active = false;
    if (!client)
        return;
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(client);
    spice_assert(client->active);
    reds_enable_mm_time(snd_channel_get_server(client));
    client->active = false;
    if (client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
        client->command &= ~SND_PLAYBACK_PCM_MASK;

        if (playback_client->pending_frame) {
            spice_assert(!playback_client->in_progress);
            snd_playback_free_frame(playback_client,
                                    playback_client->pending_frame);
            playback_client->pending_frame = NULL;
        }
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_get_buffer(SpicePlaybackInstance *sin,
                                                         uint32_t **frame, uint32_t *num_samples)
{
    SndChannelClient *client = snd_channel_get_client(&sin->st->channel);

    *frame = NULL;
    *num_samples = 0;
    if (!client) {
        return;
    }
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(client);
    if (!playback_client->free_frames) {
        return;
    }
    spice_assert(client->active);
    if (!playback_client->free_frames->allocated) {
        playback_client->free_frames->allocated = true;
        ++playback_client->frames->refs;
    }

    *frame = playback_client->free_frames->samples;
    playback_client->free_frames = playback_client->free_frames->next;
    *num_samples = snd_codec_frame_size(playback_client->codec);
}

SPICE_GNUC_VISIBLE void spice_server_playback_put_samples(SpicePlaybackInstance *sin, uint32_t *samples)
{
    PlaybackChannelClient *playback_client;
    AudioFrame *frame;

    frame = SPICE_CONTAINEROF(samples, AudioFrame, samples[0]);
    if (frame->allocated) {
        frame->allocated = false;
        if (--frame->container->refs == 0) {
            g_free(frame->container);
            return;
        }
    }
    playback_client = frame->client;
    if (!playback_client || snd_channel_get_client(&sin->st->channel) != SND_CHANNEL_CLIENT(playback_client)) {
        /* lost last reference, client has been destroyed previously */
        spice_debug("audio samples belong to a disconnected client");
        return;
    }
    spice_assert(SND_CHANNEL_CLIENT(playback_client)->active);

    if (playback_client->pending_frame) {
        snd_playback_free_frame(playback_client, playback_client->pending_frame);
    }
    frame->time = reds_get_mm_time();
    playback_client->pending_frame = frame;
    snd_set_command(SND_CHANNEL_CLIENT(playback_client), SND_PLAYBACK_PCM_MASK);
    snd_send(SND_CHANNEL_CLIENT(playback_client));
}

void snd_set_playback_latency(RedClient *client, uint32_t latency)
{
    GList *l;

    for (l = snd_channels; l != NULL; l = l->next) {
        SndChannel *now = l->data;
        SndChannelClient *scc = snd_channel_get_client(now);
        uint32_t type;
        g_object_get(RED_CHANNEL(now), "channel-type", &type, NULL);
        if (type == SPICE_CHANNEL_PLAYBACK && scc &&
            red_channel_client_get_client(RED_CHANNEL_CLIENT(scc)) == client) {

            if (red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(scc),
                SPICE_PLAYBACK_CAP_LATENCY)) {
                PlaybackChannelClient* playback = (PlaybackChannelClient*)scc;

                playback->latency = latency;
                snd_set_command(scc, SND_PLAYBACK_LATENCY_MASK);
                snd_send(scc);
            } else {
                spice_debug("client doesn't not support SPICE_PLAYBACK_CAP_LATENCY");
            }
        }
    }
}

static int snd_desired_audio_mode(bool playback_compression, int frequency,
                                  bool client_can_celt, bool client_can_opus)
{
    if (!playback_compression)
        return SPICE_AUDIO_DATA_MODE_RAW;

    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency))
        return SPICE_AUDIO_DATA_MODE_OPUS;

    if (client_can_celt && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, frequency))
        return SPICE_AUDIO_DATA_MODE_CELT_0_5_1;

    return SPICE_AUDIO_DATA_MODE_RAW;
}

static void
playback_channel_client_finalize(GObject *object)
{
    int i;
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(object);
    SndChannelClient *client = SND_CHANNEL_CLIENT(playback_client);

    // free frames, unref them
    for (i = 0; i < NUM_AUDIO_FRAMES; ++i) {
        playback_client->frames->items[i].client = NULL;
    }
    if (--playback_client->frames->refs == 0) {
        g_free(playback_client->frames);
    }

    if (client->active) {
        reds_enable_mm_time(snd_channel_get_server(client));
    }

    snd_codec_destroy(&playback_client->codec);

    G_OBJECT_CLASS(playback_channel_client_parent_class)->finalize(object);
}

static void
playback_channel_client_constructed(GObject *object)
{
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(object);
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    RedChannel *red_channel = red_channel_client_get_channel(rcc);
    SndChannel *channel = SND_CHANNEL(red_channel);
    SndChannelClient *scc = SND_CHANNEL_CLIENT(playback_client);

    G_OBJECT_CLASS(playback_channel_client_parent_class)->constructed(object);

    scc->on_message_done = snd_playback_on_message_done;

    bool client_can_celt = red_channel_client_test_remote_cap(rcc,
                                          SPICE_PLAYBACK_CAP_CELT_0_5_1);
    bool client_can_opus = red_channel_client_test_remote_cap(rcc,
                                          SPICE_PLAYBACK_CAP_OPUS);
    bool playback_compression =
        reds_config_get_playback_compression(red_channel_get_server(red_channel));
    int desired_mode = snd_desired_audio_mode(playback_compression, channel->frequency,
                                              client_can_celt, client_can_opus);
    if (desired_mode != SPICE_AUDIO_DATA_MODE_RAW) {
        if (snd_codec_create(&playback_client->codec, desired_mode, channel->frequency,
                             SND_CODEC_ENCODE) == SND_CODEC_OK) {
            playback_client->mode = desired_mode;
        } else {
            red_channel_warning(red_channel, "create encoder failed");
        }
    }

    spice_debug("playback client %p using mode %s", playback_client,
                spice_audio_data_mode_to_string(playback_client->mode));
}

static gboolean playback_channel_client_initable_init(GInitable *initable,
                                                      GCancellable *cancellable,
                                                      GError **error)
{
    gboolean success;
    RedClient *red_client = red_channel_client_get_client(RED_CHANNEL_CLIENT(initable));
    SndChannelClient *scc = SND_CHANNEL_CLIENT(initable);
    RedChannel *red_channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(initable));
    SndChannel *channel = SND_CHANNEL(red_channel);

    success = playback_channel_client_parent_initable_iface->init(initable, cancellable, error);
    if (!success) {
        return FALSE;
    }

    if (!red_client_during_migrate_at_target(red_client)) {
        snd_set_command(scc, SND_PLAYBACK_MODE_MASK);
        if (channel->volume.volume_nchannels) {
            snd_set_command(scc, SND_VOLUME_MUTE_MASK);
        }
    }

    if (channel->active) {
        playback_channel_client_start(scc);
    }
    snd_send(scc);

    return TRUE;
}

static void playback_channel_client_initable_interface_init(GInitableIface *iface)
{
    playback_channel_client_parent_initable_iface = g_type_interface_peek_parent(iface);

    iface->init = playback_channel_client_initable_init;
}

static void snd_set_peer(RedChannel *red_channel, RedClient *client, RedStream *stream,
                         RedChannelCapabilities *caps, GType type)
{
    SndChannel *channel = SND_CHANNEL(red_channel);
    SndChannelClient *snd_client = snd_channel_get_client(channel);

    /* sound channels currently only support a single client */
    if (snd_client) {
        red_channel_client_disconnect(RED_CHANNEL_CLIENT(snd_client));
    }

    snd_client = g_initable_new(type,
                                NULL, NULL,
                                "channel", channel,
                                "client", client,
                                "stream", stream,
                                "caps", caps,
                                NULL);
    g_warn_if_fail(snd_client != NULL);
}

static void snd_set_playback_peer(RedChannel *red_channel, RedClient *client, RedStream *stream,
                                  G_GNUC_UNUSED int migration,
                                  RedChannelCapabilities *caps)
{
    snd_set_peer(red_channel, client, stream, caps,
                 TYPE_PLAYBACK_CHANNEL_CLIENT);
}

static void snd_migrate_channel_client(RedChannelClient *rcc)
{
    snd_set_command(SND_CHANNEL_CLIENT(rcc), SND_MIGRATE_MASK);
    snd_send(SND_CHANNEL_CLIENT(rcc));
}

SPICE_GNUC_VISIBLE void spice_server_record_set_volume(SpiceRecordInstance *sin,
                                                uint8_t nchannels,
                                                uint16_t *volume)
{
    snd_channel_set_volume(&sin->st->channel, nchannels, volume);
}

SPICE_GNUC_VISIBLE void spice_server_record_set_mute(SpiceRecordInstance *sin, uint8_t mute)
{
    snd_channel_set_mute(&sin->st->channel, mute);
}

static void record_channel_client_start(SndChannelClient *client)
{
    if (!client) {
        return;
    }

    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(client);
    record_client->read_pos = record_client->write_pos = 0;   //todo: improve by
                                                              //stream generation
    snd_channel_client_start(client);
}

SPICE_GNUC_VISIBLE void spice_server_record_start(SpiceRecordInstance *sin)
{
    SndChannel *channel = &sin->st->channel;
    channel->active = true;
    record_channel_client_start(snd_channel_get_client(channel));
}

SPICE_GNUC_VISIBLE void spice_server_record_stop(SpiceRecordInstance *sin)
{
    SndChannelClient *client = snd_channel_get_client(&sin->st->channel);

    sin->st->channel.active = false;
    if (!client)
        return;
    spice_assert(client->active);
    client->active = false;
    if (client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_record_get_samples(SpiceRecordInstance *sin,
                                                            uint32_t *samples, uint32_t bufsize)
{
    SndChannelClient *client = snd_channel_get_client(&sin->st->channel);
    uint32_t read_pos;
    uint32_t now;
    uint32_t len;

    if (!client)
        return 0;
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(client);
    spice_assert(client->active);

    if (record_client->write_pos < RECORD_SAMPLES_SIZE / 2) {
        return 0;
    }

    len = MIN(record_client->write_pos - record_client->read_pos, bufsize);

    read_pos = record_client->read_pos % RECORD_SAMPLES_SIZE;
    record_client->read_pos += len;
    now = MIN(len, RECORD_SAMPLES_SIZE - read_pos);
    memcpy(samples, &record_client->samples[read_pos], now * 4);
    if (now < len) {
        memcpy(samples + now, record_client->samples, (len - now) * 4);
    }
    return len;
}

static uint32_t snd_get_best_rate(SndChannelClient *client, uint32_t cap_opus)
{
    bool client_can_opus = true;
    if (client) {
        client_can_opus = red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(client), cap_opus);
    }

    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, SND_CODEC_ANY_FREQUENCY))
        return SND_CODEC_OPUS_PLAYBACK_FREQ;

    return SND_CODEC_CELT_PLAYBACK_FREQ;
}

static void snd_set_rate(SndChannel *channel, uint32_t frequency, uint32_t cap_opus)
{
    RedChannel *red_channel = RED_CHANNEL(channel);
    channel->frequency = frequency;
    if (red_channel && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency)) {
        red_channel_set_cap(red_channel, cap_opus);
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_playback_rate(SpicePlaybackInstance *sin)
{
    SndChannelClient *client = sin ? snd_channel_get_client(&sin->st->channel) : NULL;
    return snd_get_best_rate(client, SPICE_PLAYBACK_CAP_OPUS);
}

SPICE_GNUC_VISIBLE void spice_server_set_playback_rate(SpicePlaybackInstance *sin, uint32_t frequency)
{
    snd_set_rate(&sin->st->channel, frequency, SPICE_PLAYBACK_CAP_OPUS);
}

SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_record_rate(SpiceRecordInstance *sin)
{
    SndChannelClient *client = sin ? snd_channel_get_client(&sin->st->channel) : NULL;
    return snd_get_best_rate(client, SPICE_RECORD_CAP_OPUS);
}

SPICE_GNUC_VISIBLE void spice_server_set_record_rate(SpiceRecordInstance *sin, uint32_t frequency)
{
    snd_set_rate(&sin->st->channel, frequency, SPICE_RECORD_CAP_OPUS);
}

static void
record_channel_client_finalize(GObject *object)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(object);

    snd_codec_destroy(&record_client->codec);

    G_OBJECT_CLASS(record_channel_client_parent_class)->finalize(object);
}

static gboolean record_channel_client_initable_init(GInitable *initable,
                                                    GCancellable *cancellable,
                                                    GError **error)
{
    gboolean success;
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(initable);
    RedChannel *red_channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(record_client));
    SndChannel *channel = SND_CHANNEL(red_channel);
    SndChannelClient *scc = SND_CHANNEL_CLIENT(record_client);

    success = record_channel_client_parent_initable_iface->init(initable, cancellable, error);
    if (!success) {
        return FALSE;
    }

    if (channel->volume.volume_nchannels) {
        snd_set_command(scc, SND_VOLUME_MUTE_MASK);
    }

    if (channel->active) {
        record_channel_client_start(scc);
    }
    snd_send(scc);

    return TRUE;
}

static void record_channel_client_initable_interface_init(GInitableIface *iface)
{
    record_channel_client_parent_initable_iface = g_type_interface_peek_parent(iface);

    iface->init = record_channel_client_initable_init;
}

static void snd_set_record_peer(RedChannel *red_channel, RedClient *client, RedStream *stream,
                                G_GNUC_UNUSED int migration,
                                RedChannelCapabilities *caps)
{
    snd_set_peer(red_channel, client, stream, caps,
                 TYPE_RECORD_CHANNEL_CLIENT);
}

static void add_channel(SndChannel *channel)
{
    snd_channels = g_list_prepend(snd_channels, channel);
}

static void remove_channel(SndChannel *channel)
{
    snd_channels = g_list_remove(snd_channels, channel);
}

static void
snd_channel_init(SndChannel *self)
{
    self->frequency = SND_CODEC_CELT_PLAYBACK_FREQ; /* Default to the legacy rate */
}

static void
snd_channel_finalize(GObject *object)
{
    SndChannel *channel = SND_CHANNEL(object);

    remove_channel(channel);

    g_free(channel->volume.volume);
    channel->volume.volume = NULL;

    G_OBJECT_CLASS(snd_channel_parent_class)->finalize(object);
}

static void
snd_channel_class_init(SndChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = snd_channel_finalize;
}

static void
playback_channel_init(PlaybackChannel *self)
{
}

static void
playback_channel_constructed(GObject *object)
{
    SndChannel *self = SND_CHANNEL(object);
    RedsState *reds = red_channel_get_server(RED_CHANNEL(self));

    G_OBJECT_CLASS(playback_channel_parent_class)->constructed(object);

    if (snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, SND_CODEC_ANY_FREQUENCY)) {
        red_channel_set_cap(RED_CHANNEL(self), SPICE_PLAYBACK_CAP_CELT_0_5_1);
    }
    red_channel_set_cap(RED_CHANNEL(self), SPICE_PLAYBACK_CAP_VOLUME);

    add_channel(self);
    reds_register_channel(reds, RED_CHANNEL(self));
}

static void
playback_channel_class_init(PlaybackChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = playback_channel_constructed;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_PLAYBACK, NULL);
    channel_class->handle_message = red_channel_client_handle_message;
    channel_class->send_item = playback_channel_send_item;

    // client callbacks
    channel_class->connect = snd_set_playback_peer;
    channel_class->migrate = snd_migrate_channel_client;
}

void snd_attach_playback(RedsState *reds, SpicePlaybackInstance *sin)
{
    sin->st = g_object_new(TYPE_PLAYBACK_CHANNEL,
                           "spice-server", reds,
                           "core-interface", reds_get_core_interface(reds),
                           "channel-type", SPICE_CHANNEL_PLAYBACK,
                           "id", 0,
                           NULL);
}

static void
record_channel_init(RecordChannel *self)
{
}

static void
record_channel_constructed(GObject *object)
{
    SndChannel *self = SND_CHANNEL(object);
    RedsState *reds = red_channel_get_server(RED_CHANNEL(self));

    G_OBJECT_CLASS(record_channel_parent_class)->constructed(object);

    if (snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, SND_CODEC_ANY_FREQUENCY)) {
        red_channel_set_cap(RED_CHANNEL(self), SPICE_RECORD_CAP_CELT_0_5_1);
    }
    red_channel_set_cap(RED_CHANNEL(self), SPICE_RECORD_CAP_VOLUME);

    add_channel(self);
    reds_register_channel(reds, RED_CHANNEL(self));
}

static void
record_channel_class_init(RecordChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = record_channel_constructed;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_RECORD, NULL);
    channel_class->handle_message = record_channel_handle_message;
    channel_class->send_item = record_channel_send_item;

    // client callbacks
    channel_class->connect = snd_set_record_peer;
    channel_class->migrate = snd_migrate_channel_client;
}

void snd_attach_record(RedsState *reds, SpiceRecordInstance *sin)
{
    sin->st = g_object_new(TYPE_RECORD_CHANNEL,
                           "spice-server", reds,
                           "core-interface", reds_get_core_interface(reds),
                           "channel-type", SPICE_CHANNEL_RECORD,
                           "id", 0,
                           NULL);
}

static void snd_detach_common(SndChannel *channel)
{
    if (!channel) {
        return;
    }

    red_channel_destroy(RED_CHANNEL(channel));
}

void snd_detach_playback(SpicePlaybackInstance *sin)
{
    snd_detach_common(&sin->st->channel);
}

void snd_detach_record(SpiceRecordInstance *sin)
{
    snd_detach_common(&sin->st->channel);
}

void snd_set_playback_compression(bool on)
{
    GList *l;

    for (l = snd_channels; l != NULL; l = l->next) {
        SndChannel *now = l->data;
        SndChannelClient *client = snd_channel_get_client(now);
        uint32_t type;
        g_object_get(RED_CHANNEL(now), "channel-type", &type, NULL);
        if (type == SPICE_CHANNEL_PLAYBACK && client) {
            PlaybackChannelClient* playback = PLAYBACK_CHANNEL_CLIENT(client);
            RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback);
            bool client_can_celt = red_channel_client_test_remote_cap(rcc,
                                    SPICE_PLAYBACK_CAP_CELT_0_5_1);
            bool client_can_opus = red_channel_client_test_remote_cap(rcc,
                                    SPICE_PLAYBACK_CAP_OPUS);
            int desired_mode = snd_desired_audio_mode(on, now->frequency,
                                                      client_can_celt, client_can_opus);
            if (playback->mode != desired_mode) {
                playback->mode = desired_mode;
                snd_set_command(client, SND_PLAYBACK_MODE_MASK);
                spice_debug("playback client %p using mode %s", playback,
                            spice_audio_data_mode_to_string(playback->mode));
            }
        }
    }
}

static void
snd_channel_client_class_init(SndChannelClientClass *klass)
{
    RedChannelClientClass *client_class = RED_CHANNEL_CLIENT_CLASS(klass);

    client_class->config_socket = snd_channel_client_config_socket;
    client_class->alloc_recv_buf = snd_channel_client_alloc_recv_buf;
    client_class->release_recv_buf = snd_channel_client_release_recv_buf;
}

static void
snd_channel_client_init(SndChannelClient *self)
{
}

static void
playback_channel_client_class_init(PlaybackChannelClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructed = playback_channel_client_constructed;
    object_class->finalize = playback_channel_client_finalize;
}

static void snd_playback_alloc_frames(PlaybackChannelClient *playback)
{
    int i;

    playback->frames = g_new0(AudioFrameContainer, 1);
    playback->frames->refs = 1;
    for (i = 0; i < NUM_AUDIO_FRAMES; ++i) {
        playback->frames->items[i].container = playback->frames;
        snd_playback_free_frame(playback, &playback->frames->items[i]);
    }
}

static void
playback_channel_client_init(PlaybackChannelClient *playback)
{
    playback->mode = SPICE_AUDIO_DATA_MODE_RAW;
    snd_playback_alloc_frames(playback);
}

static void
record_channel_client_class_init(RecordChannelClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = record_channel_client_finalize;
}

static void
record_channel_client_init(RecordChannelClient *record)
{
    record->mode = SPICE_AUDIO_DATA_MODE_RAW;
}
