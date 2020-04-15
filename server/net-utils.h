/*
   Copyright (C) 2009-2017 Red Hat, Inc.

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

#ifndef RED_NET_UTILS_H_
#define RED_NET_UTILS_H_

#include <stdbool.h>

bool red_socket_set_keepalive(int fd, bool enable, int timeout);
bool red_socket_set_no_delay(int fd, bool no_delay);
int red_socket_get_no_delay(int fd);
bool red_socket_set_non_blocking(int fd, bool non_blocking);
void red_socket_set_nosigpipe(int fd, bool enable);

#endif /* RED_NET_UTILS_H_ */
