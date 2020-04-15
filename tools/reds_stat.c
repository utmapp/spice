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

#define _GNU_SOURCE
#include <config.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <glob.h>
#include <sys/stat.h>
#include <spice/stats.h>
#include <common/verify.h>

#define TAB_LEN 4
#define VALUE_TABS 7
#define INVALID_STAT_REF (~(uint32_t)0)

verify(sizeof(SpiceStat) == 20 || sizeof(SpiceStat) == 24);

static SpiceStatNode *reds_nodes = NULL;
static uint64_t *values = NULL;

static void print_stat_tree(int32_t node_index, int depth)
{
    SpiceStatNode *node = &reds_nodes[node_index];

    if ((node->flags & SPICE_STAT_NODE_MASK_SHOW) == SPICE_STAT_NODE_MASK_SHOW) {
        printf("%*s%s", depth * TAB_LEN, "", node->name);
        if (node->flags & SPICE_STAT_NODE_FLAG_VALUE) {
            printf(":%*s%"PRIu64" (%"PRIu64")\n", (int) ((VALUE_TABS - depth) * TAB_LEN - strlen(node->name) - 1), "",
                   node->value, node->value - values[node_index]);
            values[node_index] = node->value;
        } else {
            printf("\n");
            if (node->first_child_index != INVALID_STAT_REF) {
                print_stat_tree(node->first_child_index, depth + 1);
            }
        }
    }
    if (node->next_sibling_index != INVALID_STAT_REF) {
        print_stat_tree(node->next_sibling_index, depth);
    }
}

// look for a single file /dev/shm/spice.XXX and extract XXX pid
static pid_t search_pid(void)
{
    pid_t pid = 0;
    glob_t globbuf;

    if (glob("/dev/shm/spice.*", 0, NULL, &globbuf)) {
        return pid;
    }
    if (globbuf.gl_pathc == 1) {
        const char *p = strchr(globbuf.gl_pathv[0], '.');
        if (p) {
            pid = atoi(p+1);
        }
    }
    globfree(&globbuf);
    return pid;
}

int main(int argc, char **argv)
{
    char *shm_name;
    pid_t kvm_pid = 0;
    uint32_t num_of_nodes = 0;
    size_t shm_size;
    int shm_name_len;
    int ret = EXIT_FAILURE;
    int fd;
    struct stat st;
    unsigned header_size = sizeof(SpiceStat);
    SpiceStat *reds_stat = (SpiceStat *)MAP_FAILED;

    if (argc == 2) {
        kvm_pid = atoi(argv[1]);
    } else if (argc == 1) {
        kvm_pid = search_pid();
    }

    if (argc > 2 || !kvm_pid) {
        fprintf(stderr, "usage: reds_stat [qemu_pid] (e.g. `pgrep qemu`)\n");
        return ret;
    }
    shm_name_len = strlen(SPICE_STAT_SHM_NAME) + 64;
    if (!(shm_name = (char *)malloc(shm_name_len))) {
        perror("malloc");
        return ret;
    }
    snprintf(shm_name, shm_name_len, SPICE_STAT_SHM_NAME, kvm_pid);
    if ((fd = shm_open(shm_name, O_RDONLY, 0444)) == -1) {
        perror("shm_open");
        free(shm_name);
        return ret;
    }
    shm_size = sizeof(SpiceStat);
    reds_stat = (SpiceStat *)mmap(NULL, shm_size, PROT_READ, MAP_SHARED, fd, 0);
    if (fstat(fd, &st) == 0) {
        unsigned size = st.st_size % sizeof(SpiceStatNode);
        if (size == 20 || size == 24) {
            header_size = size;
        }
    }
    if (reds_stat == (SpiceStat *)MAP_FAILED) {
        perror("mmap");
        goto error;
    }
    if (reds_stat->magic != SPICE_STAT_MAGIC) {
        fprintf(stderr, "bad magic %u\n", reds_stat->magic);
        goto error;
    }
    if (reds_stat->version != SPICE_STAT_VERSION) {
        fprintf(stderr, "bad version %u\n", reds_stat->version);
        goto error;
    }
    while (1) {
        if (system("clear") != 0) {
            printf("\n\n\n");
        }
        printf("spice statistics\n\n");
        if (num_of_nodes != reds_stat->num_of_nodes) {
            num_of_nodes = reds_stat->num_of_nodes;
            munmap(reds_stat, shm_size);
            shm_size = header_size + num_of_nodes * sizeof(SpiceStatNode);
            reds_stat = (SpiceStat *)mmap(NULL, shm_size, PROT_READ, MAP_SHARED, fd, 0);
            if (reds_stat == (SpiceStat *)MAP_FAILED) {
                perror("mmap");
                goto error;
            }
            reds_nodes = (SpiceStatNode *)((char *) reds_stat + header_size);
            values = (uint64_t *)realloc(values, num_of_nodes * sizeof(uint64_t));
            if (values == NULL) {
                perror("realloc");
                goto error;
            }
            memset(values, 0, num_of_nodes * sizeof(uint64_t));
        }
        print_stat_tree(reds_stat->root_index, 0);
        sleep(1);
    }
    ret = EXIT_SUCCESS;

error:
    free(values);
    if (reds_stat != (SpiceStat *)MAP_FAILED) {
        munmap(reds_stat, shm_size);
    }
    shm_unlink(shm_name);
    free(shm_name);
    return ret;
}
