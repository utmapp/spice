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
#ifndef CURSOR_CHANNEL_H_
# define CURSOR_CHANNEL_H_

#include "common-graphics-channel.h"
#include "red-parse-qxl.h"

/**
 * This type it's a RedChannel class which implement cursor (mouse)
 * movements.
 * A pointer to CursorChannel can be converted to a RedChannel.
 */
typedef struct CursorChannel CursorChannel;

/**
 * Create CursorChannel.
 * Since CursorChannel is intended to be run in a separate thread,
 * it does not register its own client callbacks since they would
 * be called from a different thread. Therefore users of this
 * class are responsible for registering their own client callbacks
 * for CursorChannel. These 'wrapper' client callbacks must forward
 * execution on to the CursorChannel thread.
 * cursor_channel_client_migrate() and cursor_channel_connect() are
 * provided as helper functions and should only be called from the
 * CursorChannel thread.
 */
CursorChannel*       cursor_channel_new         (RedsState *server, QXLInstance *qxl,
                                                 const SpiceCoreInterfaceInternal *core);

/**
 * Cause the channel to disconnect all clients
 */
void                 cursor_channel_disconnect  (CursorChannel *cursor);
void                 cursor_channel_reset       (CursorChannel *cursor);
void                 cursor_channel_init        (CursorChannel *cursor);
void                 cursor_channel_process_cmd (CursorChannel *cursor, RedCursorCmd *cursor_cmd);
void                 cursor_channel_set_mouse_mode(CursorChannel *cursor, uint32_t mode);

/**
 * Connect a new client to CursorChannel.
 * This is the equivalent of RedChannel client connect callback.
 * See comment on cursor_channel_new.
 */
void                 cursor_channel_connect     (CursorChannel *cursor, RedClient *client, 
                                                 RedsStream *stream,
                                                 int migrate,
                                                 uint32_t *common_caps, int num_common_caps,
                                                 uint32_t *caps, int num_caps);

/**
 * Migrate a client channel from a CursorChannel.
 * This is the equivalent of RedChannel client migrate callback.
 * See comment on cursor_channel_new.
 */
void                 cursor_channel_client_migrate(RedChannelClient *client);

#endif /* CURSOR_CHANNEL_H_ */
