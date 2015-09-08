/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <fcntl.h>
#include <fnmatch.h>

#include "process-util.h"
#include "path-util.h"
#include "special.h"
#include "cgroup-util.h"
#include "cgroup.h"

#define CGROUP_CPU_QUOTA_PERIOD_USEC ((usec_t) 100 * USEC_PER_MSEC)

void cgroup_context_init(CGroupContext *c) {
        assert(c);

        /* Initialize everything to the kernel defaults, assuming the
         * structure is preinitialized to 0 */

        c->cpu_shares = (unsigned long) -1;
        c->startup_cpu_shares = (unsigned long) -1;
        c->memory_limit = (uint64_t) -1;
        c->blockio_weight = (unsigned long) -1;
        c->startup_blockio_weight = (unsigned long) -1;

        c->cpu_quota_per_sec_usec = USEC_INFINITY;
}

void cgroup_context_free_device_allow(CGroupContext *c, CGroupDeviceAllow *a) {
        assert(c);
        assert(a);

        LIST_REMOVE(device_allow, c->device_allow, a);
        free(a->path);
        free(a);
}

void cgroup_context_free_blockio_device_weight(CGroupContext *c, CGroupBlockIODeviceWeight *w) {
        assert(c);
        assert(w);

        LIST_REMOVE(device_weights, c->blockio_device_weights, w);
        free(w->path);
        free(w);
}

void cgroup_context_free_blockio_device_bandwidth(CGroupContext *c, CGroupBlockIODeviceBandwidth *b) {
        assert(c);
        assert(b);

        LIST_REMOVE(device_bandwidths, c->blockio_device_bandwidths, b);
        free(b->path);
        free(b);
}

void cgroup_context_done(CGroupContext *c) {
        assert(c);

        while (c->blockio_device_weights)
                cgroup_context_free_blockio_device_weight(c, c->blockio_device_weights);

        while (c->blockio_device_bandwidths)
                cgroup_context_free_blockio_device_bandwidth(c, c->blockio_device_bandwidths);

        while (c->device_allow)
                cgroup_context_free_device_allow(c, c->device_allow);
}

void cgroup_context_dump(CGroupContext *c, FILE* f, const char *prefix) {
        CGroupBlockIODeviceBandwidth *b;
        CGroupBlockIODeviceWeight *w;
        CGroupDeviceAllow *a;
        char u[FORMAT_TIMESPAN_MAX];

        assert(c);
        assert(f);

        prefix = strempty(prefix);

        fprintf(f,
                "%sCPUAccounting=%s\n"
                "%sBlockIOAccounting=%s\n"
                "%sMemoryAccounting=%s\n"
                "%sCPUShares=%lu\n"
                "%sStartupCPUShares=%lu\n"
                "%sCPUQuotaPerSecSec=%s\n"
                "%sBlockIOWeight=%lu\n"
                "%sStartupBlockIOWeight=%lu\n"
                "%sMemoryLimit=%" PRIu64 "\n"
                "%sDevicePolicy=%s\n"
                "%sDelegate=%s\n",
                prefix, yes_no(c->cpu_accounting),
                prefix, yes_no(c->blockio_accounting),
                prefix, yes_no(c->memory_accounting),
                prefix, c->cpu_shares,
                prefix, c->startup_cpu_shares,
                prefix, format_timespan(u, sizeof(u), c->cpu_quota_per_sec_usec, 1),
                prefix, c->blockio_weight,
                prefix, c->startup_blockio_weight,
                prefix, c->memory_limit,
                prefix, cgroup_device_policy_to_string(c->device_policy),
                prefix, yes_no(c->delegate));

        LIST_FOREACH(device_allow, a, c->device_allow)
                fprintf(f,
                        "%sDeviceAllow=%s %s%s%s\n",
                        prefix,
                        a->path,
                        a->r ? "r" : "", a->w ? "w" : "", a->m ? "m" : "");

        LIST_FOREACH(device_weights, w, c->blockio_device_weights)
                fprintf(f,
                        "%sBlockIODeviceWeight=%s %lu",
                        prefix,
                        w->path,
                        w->weight);

        LIST_FOREACH(device_bandwidths, b, c->blockio_device_bandwidths) {
                char buf[FORMAT_BYTES_MAX];

                fprintf(f,
                        "%s%s=%s %s\n",
                        prefix,
                        b->read ? "BlockIOReadBandwidth" : "BlockIOWriteBandwidth",
                        b->path,
                        format_bytes(buf, sizeof(buf), b->bandwidth));
        }
}

static int lookup_blkio_device(const char *p, dev_t *dev) {
        struct stat st;
        int r;

        assert(p);
        assert(dev);

        r = stat(p, &st);
        if (r < 0)
                return log_warning_errno(errno, "Couldn't stat device %s: %m", p);

        if (S_ISBLK(st.st_mode))
                *dev = st.st_rdev;
        else if (major(st.st_dev) != 0) {
                /* If this is not a device node then find the block
                 * device this file is stored on */
                *dev = st.st_dev;

                /* If this is a partition, try to get the originating
                 * block device */
                block_get_whole_disk(*dev, dev);
        } else {
                log_warning("%s is not a block device and file system block device cannot be determined or is not local.", p);
                return -ENODEV;
        }

        return 0;
}

static int whitelist_device(const char *path, const char *node, const char *acc) {
        char buf[2+DECIMAL_STR_MAX(dev_t)*2+2+4];
        struct stat st;
        int r;

        assert(path);
        assert(acc);

        if (stat(node, &st) < 0) {
                log_warning("Couldn't stat device %s", node);
                return -errno;
        }

        if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
                log_warning("%s is not a device.", node);
                return -ENODEV;
        }

        sprintf(buf,
                "%c %u:%u %s",
                S_ISCHR(st.st_mode) ? 'c' : 'b',
                major(st.st_rdev), minor(st.st_rdev),
                acc);

        r = cg_set_attribute("devices", path, "devices.allow", buf);
        if (r < 0)
                log_full_errno(IN_SET(r, -ENOENT, -EROFS, -EINVAL) ? LOG_DEBUG : LOG_WARNING, r,
                               "Failed to set devices.allow on %s: %m", path);

        return r;
}

static int whitelist_major(const char *path, const char *name, char type, const char *acc) {
        _cleanup_fclose_ FILE *f = NULL;
        char line[LINE_MAX];
        bool good = false;
        int r;

        assert(path);
        assert(acc);
        assert(type == 'b' || type == 'c');

        f = fopen("/proc/devices", "re");
        if (!f)
                return log_warning_errno(errno, "Cannot open /proc/devices to resolve %s (%c): %m", name, type);

        FOREACH_LINE(line, f, goto fail) {
                char buf[2+DECIMAL_STR_MAX(unsigned)+3+4], *p, *w;
                unsigned maj;

                truncate_nl(line);

                if (type == 'c' && streq(line, "Character devices:")) {
                        good = true;
                        continue;
                }

                if (type == 'b' && streq(line, "Block devices:")) {
                        good = true;
                        continue;
                }

                if (isempty(line)) {
                        good = false;
                        continue;
                }

                if (!good)
                        continue;

                p = strstrip(line);

                w = strpbrk(p, WHITESPACE);
                if (!w)
                        continue;
                *w = 0;

                r = safe_atou(p, &maj);
                if (r < 0)
                        continue;
                if (maj <= 0)
                        continue;

                w++;
                w += strspn(w, WHITESPACE);

                if (fnmatch(name, w, 0) != 0)
                        continue;

                sprintf(buf,
                        "%c %u:* %s",
                        type,
                        maj,
                        acc);

                r = cg_set_attribute("devices", path, "devices.allow", buf);
                if (r < 0)
                        log_full_errno(IN_SET(r, -ENOENT, -EROFS, -EINVAL) ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to set devices.allow on %s: %m", path);
        }

        return 0;

fail:
        log_warning_errno(errno, "Failed to read /proc/devices: %m");
        return -errno;
}

void cgroup_context_apply(CGroupContext *c, CGroupMask mask, const char *path, ManagerState state) {
        bool is_root;
        int r;

        assert(c);
        assert(path);

        if (mask == 0)
                return;

        /* Some cgroup attributes are not supported on the root cgroup,
         * hence silently ignore */
        is_root = isempty(path) || path_equal(path, "/");
        if (is_root)
                /* Make sure we don't try to display messages with an empty path. */
                path = "/";

        /* We generally ignore errors caused by read-only mounted
         * cgroup trees (assuming we are running in a container then),
         * and missing cgroups, i.e. EROFS and ENOENT. */

        if ((mask & CGROUP_MASK_CPU) && !is_root) {
                char buf[MAX(DECIMAL_STR_MAX(unsigned long), DECIMAL_STR_MAX(usec_t)) + 1];

                sprintf(buf, "%lu\n",
                        IN_SET(state, MANAGER_STARTING, MANAGER_INITIALIZING) && c->startup_cpu_shares != (unsigned long) -1 ? c->startup_cpu_shares :
                        c->cpu_shares != (unsigned long) -1 ? c->cpu_shares : 1024);
                r = cg_set_attribute("cpu", path, "cpu.shares", buf);
                if (r < 0)
                        log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to set cpu.shares on %s: %m", path);

                sprintf(buf, USEC_FMT "\n", CGROUP_CPU_QUOTA_PERIOD_USEC);
                r = cg_set_attribute("cpu", path, "cpu.cfs_period_us", buf);
                if (r < 0)
                        log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to set cpu.cfs_period_us on %s: %m", path);

                if (c->cpu_quota_per_sec_usec != USEC_INFINITY) {
                        sprintf(buf, USEC_FMT "\n", c->cpu_quota_per_sec_usec * CGROUP_CPU_QUOTA_PERIOD_USEC / USEC_PER_SEC);
                        r = cg_set_attribute("cpu", path, "cpu.cfs_quota_us", buf);
                } else
                        r = cg_set_attribute("cpu", path, "cpu.cfs_quota_us", "-1");
                if (r < 0)
                        log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to set cpu.cfs_quota_us on %s: %m", path);
        }

        if (mask & CGROUP_MASK_BLKIO) {
                char buf[MAX3(DECIMAL_STR_MAX(unsigned long)+1,
                              DECIMAL_STR_MAX(dev_t)*2+2+DECIMAL_STR_MAX(unsigned long)*1,
                              DECIMAL_STR_MAX(dev_t)*2+2+DECIMAL_STR_MAX(uint64_t)+1)];
                CGroupBlockIODeviceWeight *w;
                CGroupBlockIODeviceBandwidth *b;

                if (!is_root) {
                        sprintf(buf, "%lu\n", IN_SET(state, MANAGER_STARTING, MANAGER_INITIALIZING) && c->startup_blockio_weight != (unsigned long) -1 ? c->startup_blockio_weight :
                                c->blockio_weight != (unsigned long) -1 ? c->blockio_weight : 1000);
                        r = cg_set_attribute("blkio", path, "blkio.weight", buf);
                        if (r < 0)
                                log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                               "Failed to set blkio.weight on %s: %m", path);

                        /* FIXME: no way to reset this list */
                        LIST_FOREACH(device_weights, w, c->blockio_device_weights) {
                                dev_t dev;

                                r = lookup_blkio_device(w->path, &dev);
                                if (r < 0)
                                        continue;

                                sprintf(buf, "%u:%u %lu", major(dev), minor(dev), w->weight);
                                r = cg_set_attribute("blkio", path, "blkio.weight_device", buf);
                                if (r < 0)
                                        log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                                       "Failed to set blkio.weight_device on %s: %m", path);
                        }
                }

                /* FIXME: no way to reset this list */
                LIST_FOREACH(device_bandwidths, b, c->blockio_device_bandwidths) {
                        const char *a;
                        dev_t dev;

                        r = lookup_blkio_device(b->path, &dev);
                        if (r < 0)
                                continue;

                        a = b->read ? "blkio.throttle.read_bps_device" : "blkio.throttle.write_bps_device";

                        sprintf(buf, "%u:%u %" PRIu64 "\n", major(dev), minor(dev), b->bandwidth);
                        r = cg_set_attribute("blkio", path, a, buf);
                        if (r < 0)
                                log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                               "Failed to set %s on %s: %m", a, path);
                }
        }

        if ((mask & CGROUP_MASK_MEMORY) && !is_root) {
                if (c->memory_limit != (uint64_t) -1) {
                        char buf[DECIMAL_STR_MAX(uint64_t) + 1];

                        sprintf(buf, "%" PRIu64 "\n", c->memory_limit);

                        if (cg_unified() <= 0)
                                r = cg_set_attribute("memory", path, "memory.limit_in_bytes", buf);
                        else
                                r = cg_set_attribute("memory", path, "memory.max", buf);

                } else {
                        if (cg_unified() <= 0)
                                r = cg_set_attribute("memory", path, "memory.limit_in_bytes", "-1");
                        else
                                r = cg_set_attribute("memory", path, "memory.max", "max");
                }

                if (r < 0)
                        log_full_errno(IN_SET(r, -ENOENT, -EROFS) ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to set memory.limit_in_bytes/memory.max on %s: %m", path);
        }

        if ((mask & CGROUP_MASK_DEVICES) && !is_root) {
                CGroupDeviceAllow *a;

                /* Changing the devices list of a populated cgroup
                 * might result in EINVAL, hence ignore EINVAL
                 * here. */

                if (c->device_allow || c->device_policy != CGROUP_AUTO)
                        r = cg_set_attribute("devices", path, "devices.deny", "a");
                else
                        r = cg_set_attribute("devices", path, "devices.allow", "a");
                if (r < 0)
                        log_full_errno(IN_SET(r, -ENOENT, -EROFS, -EINVAL) ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to reset devices.list on %s: %m", path);

                if (c->device_policy == CGROUP_CLOSED ||
                    (c->device_policy == CGROUP_AUTO && c->device_allow)) {
                        static const char auto_devices[] =
                                "/dev/null\0" "rwm\0"
                                "/dev/zero\0" "rwm\0"
                                "/dev/full\0" "rwm\0"
                                "/dev/random\0" "rwm\0"
                                "/dev/urandom\0" "rwm\0"
                                "/dev/tty\0" "rwm\0"
                                "/dev/pts/ptmx\0" "rw\0"; /* /dev/pts/ptmx may not be duplicated, but accessed */

                        const char *x, *y;

                        NULSTR_FOREACH_PAIR(x, y, auto_devices)
                                whitelist_device(path, x, y);

                        whitelist_major(path, "pts", 'c', "rw");
                        whitelist_major(path, "kdbus", 'c', "rw");
                        whitelist_major(path, "kdbus/*", 'c', "rw");
                }

                LIST_FOREACH(device_allow, a, c->device_allow) {
                        char acc[4];
                        unsigned k = 0;

                        if (a->r)
                                acc[k++] = 'r';
                        if (a->w)
                                acc[k++] = 'w';
                        if (a->m)
                                acc[k++] = 'm';

                        if (k == 0)
                                continue;

                        acc[k++] = 0;

                        if (startswith(a->path, "/dev/"))
                                whitelist_device(path, a->path, acc);
                        else if (startswith(a->path, "block-"))
                                whitelist_major(path, a->path + 6, 'b', acc);
                        else if (startswith(a->path, "char-"))
                                whitelist_major(path, a->path + 5, 'c', acc);
                        else
                                log_debug("Ignoring device %s while writing cgroup attribute.", a->path);
                }
        }
}

CGroupMask cgroup_context_get_mask(CGroupContext *c) {
        CGroupMask mask = 0;

        /* Figure out which controllers we need */

        if (c->cpu_accounting ||
            c->cpu_shares != (unsigned long) -1 ||
            c->startup_cpu_shares != (unsigned long) -1 ||
            c->cpu_quota_per_sec_usec != USEC_INFINITY)
                mask |= CGROUP_MASK_CPUACCT | CGROUP_MASK_CPU;

        if (c->blockio_accounting ||
            c->blockio_weight != (unsigned long) -1 ||
            c->startup_blockio_weight != (unsigned long) -1 ||
            c->blockio_device_weights ||
            c->blockio_device_bandwidths)
                mask |= CGROUP_MASK_BLKIO;

        if (c->memory_accounting ||
            c->memory_limit != (uint64_t) -1)
                mask |= CGROUP_MASK_MEMORY;

        if (c->device_allow ||
            c->device_policy != CGROUP_AUTO)
                mask |= CGROUP_MASK_DEVICES;

        return mask;
}

CGroupMask unit_get_own_mask(Unit *u) {
        CGroupContext *c;

        /* Returns the mask of controllers the unit needs for itself */

        c = unit_get_cgroup_context(u);
        if (!c)
                return 0;

        /* If delegation is turned on, then turn on all cgroups,
         * unless we are on the legacy hierarchy and the process we
         * fork into it is known to drop privileges, and hence
         * shouldn't get access to the controllers.
         *
         * Note that on the unified hierarchy it is safe to delegate
         * controllers to unprivileged services. */

        if (c->delegate) {
                ExecContext *e;

                e = unit_get_exec_context(u);
                if (!e ||
                    exec_context_maintains_privileges(e) ||
                    cg_unified() > 0)
                        return _CGROUP_MASK_ALL;
        }

        return cgroup_context_get_mask(c);
}

CGroupMask unit_get_members_mask(Unit *u) {
        assert(u);

        /* Returns the mask of controllers all of the unit's children
         * require, merged */

        if (u->cgroup_members_mask_valid)
                return u->cgroup_members_mask;

        u->cgroup_members_mask = 0;

        if (u->type == UNIT_SLICE) {
                Unit *member;
                Iterator i;

                SET_FOREACH(member, u->dependencies[UNIT_BEFORE], i) {

                        if (member == u)
                                continue;

                        if (UNIT_DEREF(member->slice) != u)
                                continue;

                        u->cgroup_members_mask |=
                                unit_get_own_mask(member) |
                                unit_get_members_mask(member);
                }
        }

        u->cgroup_members_mask_valid = true;
        return u->cgroup_members_mask;
}

CGroupMask unit_get_siblings_mask(Unit *u) {
        assert(u);

        /* Returns the mask of controllers all of the unit's siblings
         * require, i.e. the members mask of the unit's parent slice
         * if there is one. */

        if (UNIT_ISSET(u->slice))
                return unit_get_members_mask(UNIT_DEREF(u->slice));

        return unit_get_own_mask(u) | unit_get_members_mask(u);
}

CGroupMask unit_get_subtree_mask(Unit *u) {

        /* Returns the mask of this subtree, meaning of the group
         * itself and its children. */

        return unit_get_own_mask(u) | unit_get_members_mask(u);
}

CGroupMask unit_get_target_mask(Unit *u) {
        CGroupMask mask;

        /* This returns the cgroup mask of all controllers to enable
         * for a specific cgroup, i.e. everything it needs itself,
         * plus all that its children need, plus all that its siblings
         * need. This is primarily useful on the legacy cgroup
         * hierarchy, where we need to duplicate each cgroup in each
         * hierarchy that shall be enabled for it. */

        mask = unit_get_own_mask(u) | unit_get_members_mask(u) | unit_get_siblings_mask(u);
        mask &= u->manager->cgroup_supported;

        return mask;
}

CGroupMask unit_get_enable_mask(Unit *u) {
        CGroupMask mask;

        /* This returns the cgroup mask of all controllers to enable
         * for the children of a specific cgroup. This is primarily
         * useful for the unified cgroup hierarchy, where each cgroup
         * controls which controllers are enabled for its children. */

        mask = unit_get_members_mask(u);
        mask &= u->manager->cgroup_supported;

        return mask;
}

/* Recurse from a unit up through its containing slices, propagating
 * mask bits upward. A unit is also member of itself. */
void unit_update_cgroup_members_masks(Unit *u) {
        CGroupMask m;
        bool more;

        assert(u);

        /* Calculate subtree mask */
        m = unit_get_subtree_mask(u);

        /* See if anything changed from the previous invocation. If
         * not, we're done. */
        if (u->cgroup_subtree_mask_valid && m == u->cgroup_subtree_mask)
                return;

        more =
                u->cgroup_subtree_mask_valid &&
                ((m & ~u->cgroup_subtree_mask) != 0) &&
                ((~m & u->cgroup_subtree_mask) == 0);

        u->cgroup_subtree_mask = m;
        u->cgroup_subtree_mask_valid = true;

        if (UNIT_ISSET(u->slice)) {
                Unit *s = UNIT_DEREF(u->slice);

                if (more)
                        /* There's more set now than before. We
                         * propagate the new mask to the parent's mask
                         * (not caring if it actually was valid or
                         * not). */

                        s->cgroup_members_mask |= m;

                else
                        /* There's less set now than before (or we
                         * don't know), we need to recalculate
                         * everything, so let's invalidate the
                         * parent's members mask */

                        s->cgroup_members_mask_valid = false;

                /* And now make sure that this change also hits our
                 * grandparents */
                unit_update_cgroup_members_masks(s);
        }
}

static const char *migrate_callback(CGroupMask mask, void *userdata) {
        Unit *u = userdata;

        assert(mask != 0);
        assert(u);

        while (u) {
                if (u->cgroup_path &&
                    u->cgroup_realized &&
                    (u->cgroup_realized_mask & mask) == mask)
                        return u->cgroup_path;

                u = UNIT_DEREF(u->slice);
        }

        return NULL;
}

char *unit_default_cgroup_path(Unit *u) {
        _cleanup_free_ char *escaped = NULL, *slice = NULL;
        int r;

        assert(u);

        if (unit_has_name(u, SPECIAL_ROOT_SLICE))
                return strdup(u->manager->cgroup_root);

        if (UNIT_ISSET(u->slice) && !unit_has_name(UNIT_DEREF(u->slice), SPECIAL_ROOT_SLICE)) {
                r = cg_slice_to_path(UNIT_DEREF(u->slice)->id, &slice);
                if (r < 0)
                        return NULL;
        }

        escaped = cg_escape(u->id);
        if (!escaped)
                return NULL;

        if (slice)
                return strjoin(u->manager->cgroup_root, "/", slice, "/", escaped, NULL);
        else
                return strjoin(u->manager->cgroup_root, "/", escaped, NULL);
}

int unit_set_cgroup_path(Unit *u, const char *path) {
        _cleanup_free_ char *p = NULL;
        int r;

        assert(u);

        if (path) {
                p = strdup(path);
                if (!p)
                        return -ENOMEM;
        } else
                p = NULL;

        if (streq_ptr(u->cgroup_path, p))
                return 0;

        if (p) {
                r = hashmap_put(u->manager->cgroup_unit, p, u);
                if (r < 0)
                        return r;
        }

        unit_release_cgroup(u);

        u->cgroup_path = p;
        p = NULL;

        return 1;
}

int unit_watch_cgroup(Unit *u) {
        _cleanup_free_ char *populated = NULL;
        int r;

        assert(u);

        if (!u->cgroup_path)
                return 0;

        if (u->cgroup_inotify_wd >= 0)
                return 0;

        /* Only applies to the unified hierarchy */
        r = cg_unified();
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed detect wether the unified hierarchy is used: %m");
        if (r == 0)
                return 0;

        /* Don't watch the root slice, it's pointless. */
        if (unit_has_name(u, SPECIAL_ROOT_SLICE))
                return 0;

        r = hashmap_ensure_allocated(&u->manager->cgroup_inotify_wd_unit, &trivial_hash_ops);
        if (r < 0)
                return log_oom();

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, "cgroup.populated", &populated);
        if (r < 0)
                return log_oom();

        u->cgroup_inotify_wd = inotify_add_watch(u->manager->cgroup_inotify_fd, populated, IN_MODIFY);
        if (u->cgroup_inotify_wd < 0) {

                if (errno == ENOENT) /* If the directory is already
                                      * gone we don't need to track
                                      * it, so this is not an error */
                        return 0;

                return log_unit_error_errno(u, errno, "Failed to add inotify watch descriptor for control group %s: %m", u->cgroup_path);
        }

        r = hashmap_put(u->manager->cgroup_inotify_wd_unit, INT_TO_PTR(u->cgroup_inotify_wd), u);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to add inotify watch descriptor to hash map: %m");

        return 0;
}

static int unit_create_cgroup(
                Unit *u,
                CGroupMask target_mask,
                CGroupMask enable_mask) {

        CGroupContext *c;
        int r;

        assert(u);

        c = unit_get_cgroup_context(u);
        if (!c)
                return 0;

        if (!u->cgroup_path) {
                _cleanup_free_ char *path = NULL;

                path = unit_default_cgroup_path(u);
                if (!path)
                        return log_oom();

                r = unit_set_cgroup_path(u, path);
                if (r == -EEXIST)
                        return log_unit_error_errno(u, r, "Control group %s exists already.", path);
                if (r < 0)
                        return log_unit_error_errno(u, r, "Failed to set unit's control group path to %s: %m", path);
        }

        /* First, create our own group */
        r = cg_create_everywhere(u->manager->cgroup_supported, target_mask, u->cgroup_path);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to create cgroup %s: %m", u->cgroup_path);

        /* Start watching it */
        (void) unit_watch_cgroup(u);

        /* Enable all controllers we need */
        r = cg_enable_everywhere(u->manager->cgroup_supported, enable_mask, u->cgroup_path);
        if (r < 0)
                log_unit_warning_errno(u, r, "Failed to enable controllers on cgroup %s, ignoring: %m", u->cgroup_path);

        /* Keep track that this is now realized */
        u->cgroup_realized = true;
        u->cgroup_realized_mask = target_mask;

        if (u->type != UNIT_SLICE && !c->delegate) {

                /* Then, possibly move things over, but not if
                 * subgroups may contain processes, which is the case
                 * for slice and delegation units. */
                r = cg_migrate_everywhere(u->manager->cgroup_supported, u->cgroup_path, u->cgroup_path, migrate_callback, u);
                if (r < 0)
                        log_unit_warning_errno(u, r, "Failed to migrate cgroup from to %s, ignoring: %m", u->cgroup_path);
        }

        return 0;
}

int unit_attach_pids_to_cgroup(Unit *u) {
        int r;
        assert(u);

        r = unit_realize_cgroup(u);
        if (r < 0)
                return r;

        r = cg_attach_many_everywhere(u->manager->cgroup_supported, u->cgroup_path, u->pids, migrate_callback, u);
        if (r < 0)
                return r;

        return 0;
}

static bool unit_has_mask_realized(Unit *u, CGroupMask target_mask) {
        assert(u);

        return u->cgroup_realized && u->cgroup_realized_mask == target_mask;
}

/* Check if necessary controllers and attributes for a unit are in place.
 *
 * If so, do nothing.
 * If not, create paths, move processes over, and set attributes.
 *
 * Returns 0 on success and < 0 on failure. */
static int unit_realize_cgroup_now(Unit *u, ManagerState state) {
        CGroupMask target_mask, enable_mask;
        int r;

        assert(u);

        if (u->in_cgroup_queue) {
                LIST_REMOVE(cgroup_queue, u->manager->cgroup_queue, u);
                u->in_cgroup_queue = false;
        }

        target_mask = unit_get_target_mask(u);
        if (unit_has_mask_realized(u, target_mask))
                return 0;

        /* First, realize parents */
        if (UNIT_ISSET(u->slice)) {
                r = unit_realize_cgroup_now(UNIT_DEREF(u->slice), state);
                if (r < 0)
                        return r;
        }

        /* And then do the real work */
        enable_mask = unit_get_enable_mask(u);
        r = unit_create_cgroup(u, target_mask, enable_mask);
        if (r < 0)
                return r;

        /* Finally, apply the necessary attributes. */
        cgroup_context_apply(unit_get_cgroup_context(u), target_mask, u->cgroup_path, state);

        return 0;
}

static void unit_add_to_cgroup_queue(Unit *u) {

        if (u->in_cgroup_queue)
                return;

        LIST_PREPEND(cgroup_queue, u->manager->cgroup_queue, u);
        u->in_cgroup_queue = true;
}

unsigned manager_dispatch_cgroup_queue(Manager *m) {
        ManagerState state;
        unsigned n = 0;
        Unit *i;
        int r;

        state = manager_state(m);

        while ((i = m->cgroup_queue)) {
                assert(i->in_cgroup_queue);

                r = unit_realize_cgroup_now(i, state);
                if (r < 0)
                        log_warning_errno(r, "Failed to realize cgroups for queued unit %s, ignoring: %m", i->id);

                n++;
        }

        return n;
}

static void unit_queue_siblings(Unit *u) {
        Unit *slice;

        /* This adds the siblings of the specified unit and the
         * siblings of all parent units to the cgroup queue. (But
         * neither the specified unit itself nor the parents.) */

        while ((slice = UNIT_DEREF(u->slice))) {
                Iterator i;
                Unit *m;

                SET_FOREACH(m, slice->dependencies[UNIT_BEFORE], i) {
                        if (m == u)
                                continue;

                        /* Skip units that have a dependency on the slice
                         * but aren't actually in it. */
                        if (UNIT_DEREF(m->slice) != slice)
                                continue;

                        /* No point in doing cgroup application for units
                         * without active processes. */
                        if (UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(m)))
                                continue;

                        /* If the unit doesn't need any new controllers
                         * and has current ones realized, it doesn't need
                         * any changes. */
                        if (unit_has_mask_realized(m, unit_get_target_mask(m)))
                                continue;

                        unit_add_to_cgroup_queue(m);
                }

                u = slice;
        }
}

int unit_realize_cgroup(Unit *u) {
        assert(u);

        if (!UNIT_HAS_CGROUP_CONTEXT(u))
                return 0;

        /* So, here's the deal: when realizing the cgroups for this
         * unit, we need to first create all parents, but there's more
         * actually: for the weight-based controllers we also need to
         * make sure that all our siblings (i.e. units that are in the
         * same slice as we are) have cgroups, too. Otherwise, things
         * would become very uneven as each of their processes would
         * get as much resources as all our group together. This call
         * will synchronously create the parent cgroups, but will
         * defer work on the siblings to the next event loop
         * iteration. */

        /* Add all sibling slices to the cgroup queue. */
        unit_queue_siblings(u);

        /* And realize this one now (and apply the values) */
        return unit_realize_cgroup_now(u, manager_state(u->manager));
}

void unit_release_cgroup(Unit *u) {
        assert(u);

        /* Forgets all cgroup details for this cgroup */

        if (u->cgroup_path) {
                (void) hashmap_remove(u->manager->cgroup_unit, u->cgroup_path);
                u->cgroup_path = mfree(u->cgroup_path);
        }

        if (u->cgroup_inotify_wd >= 0) {
                if (inotify_rm_watch(u->manager->cgroup_inotify_fd, u->cgroup_inotify_wd) < 0)
                        log_unit_debug_errno(u, errno, "Failed to remove cgroup inotify watch %i for %s, ignoring", u->cgroup_inotify_wd, u->id);

                (void) hashmap_remove(u->manager->cgroup_inotify_wd_unit, INT_TO_PTR(u->cgroup_inotify_wd));
                u->cgroup_inotify_wd = -1;
        }
}

void unit_prune_cgroup(Unit *u) {
        int r;
        bool is_root_slice;

        assert(u);

        /* Removes the cgroup, if empty and possible, and stops watching it. */

        if (!u->cgroup_path)
                return;

        is_root_slice = unit_has_name(u, SPECIAL_ROOT_SLICE);

        r = cg_trim_everywhere(u->manager->cgroup_supported, u->cgroup_path, !is_root_slice);
        if (r < 0) {
                log_debug_errno(r, "Failed to destroy cgroup %s, ignoring: %m", u->cgroup_path);
                return;
        }

        if (is_root_slice)
                return;

        unit_release_cgroup(u);

        u->cgroup_realized = false;
        u->cgroup_realized_mask = 0;
}

int unit_search_main_pid(Unit *u, pid_t *ret) {
        _cleanup_fclose_ FILE *f = NULL;
        pid_t pid = 0, npid, mypid;
        int r;

        assert(u);
        assert(ret);

        if (!u->cgroup_path)
                return -ENXIO;

        r = cg_enumerate_processes(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, &f);
        if (r < 0)
                return r;

        mypid = getpid();
        while (cg_read_pid(f, &npid) > 0)  {
                pid_t ppid;

                if (npid == pid)
                        continue;

                /* Ignore processes that aren't our kids */
                if (get_parent_of_pid(npid, &ppid) >= 0 && ppid != mypid)
                        continue;

                if (pid != 0)
                        /* Dang, there's more than one daemonized PID
                        in this group, so we don't know what process
                        is the main process. */

                        return -ENODATA;

                pid = npid;
        }

        *ret = pid;
        return 0;
}

static int unit_watch_pids_in_path(Unit *u, const char *path) {
        _cleanup_closedir_ DIR *d = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int ret = 0, r;

        assert(u);
        assert(path);

        r = cg_enumerate_processes(SYSTEMD_CGROUP_CONTROLLER, path, &f);
        if (r < 0)
                ret = r;
        else {
                pid_t pid;

                while ((r = cg_read_pid(f, &pid)) > 0) {
                        r = unit_watch_pid(u, pid);
                        if (r < 0 && ret >= 0)
                                ret = r;
                }

                if (r < 0 && ret >= 0)
                        ret = r;
        }

        r = cg_enumerate_subgroups(SYSTEMD_CGROUP_CONTROLLER, path, &d);
        if (r < 0) {
                if (ret >= 0)
                        ret = r;
        } else {
                char *fn;

                while ((r = cg_read_subgroup(d, &fn)) > 0) {
                        _cleanup_free_ char *p = NULL;

                        p = strjoin(path, "/", fn, NULL);
                        free(fn);

                        if (!p)
                                return -ENOMEM;

                        r = unit_watch_pids_in_path(u, p);
                        if (r < 0 && ret >= 0)
                                ret = r;
                }

                if (r < 0 && ret >= 0)
                        ret = r;
        }

        return ret;
}

int unit_watch_all_pids(Unit *u) {
        assert(u);

        /* Adds all PIDs from our cgroup to the set of PIDs we
         * watch. This is a fallback logic for cases where we do not
         * get reliable cgroup empty notifications: we try to use
         * SIGCHLD as replacement. */

        if (!u->cgroup_path)
                return -ENOENT;

        if (cg_unified() > 0) /* On unified we can use proper notifications */
                return 0;

        return unit_watch_pids_in_path(u, u->cgroup_path);
}

int unit_notify_cgroup_empty(Unit *u) {
        int r;

        assert(u);

        if (!u->cgroup_path)
                return 0;

        r = cg_is_empty_recursive(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path);
        if (r <= 0)
                return r;

        unit_add_to_gc_queue(u);

        if (UNIT_VTABLE(u)->notify_cgroup_empty)
                UNIT_VTABLE(u)->notify_cgroup_empty(u);

        return 0;
}

static int on_cgroup_inotify_event(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;

        assert(s);
        assert(fd >= 0);
        assert(m);

        for (;;) {
                union inotify_event_buffer buffer;
                struct inotify_event *e;
                ssize_t l;

                l = read(fd, &buffer, sizeof(buffer));
                if (l < 0) {
                        if (errno == EINTR || errno == EAGAIN)
                                return 0;

                        return log_error_errno(errno, "Failed to read control group inotify events: %m");
                }

                FOREACH_INOTIFY_EVENT(e, buffer, l) {
                        Unit *u;

                        if (e->wd < 0)
                                /* Queue overflow has no watch descriptor */
                                continue;

                        if (e->mask & IN_IGNORED)
                                /* The watch was just removed */
                                continue;

                        u = hashmap_get(m->cgroup_inotify_wd_unit, INT_TO_PTR(e->wd));
                        if (!u) /* Not that inotify might deliver
                                 * events for a watch even after it
                                 * was removed, because it was queued
                                 * before the removal. Let's ignore
                                 * this here safely. */
                                continue;

                        (void) unit_notify_cgroup_empty(u);
                }
        }
}

int manager_setup_cgroup(Manager *m) {
        _cleanup_free_ char *path = NULL;
        CGroupController c;
        int r, unified;
        char *e;

        assert(m);

        /* 1. Determine hierarchy */
        m->cgroup_root = mfree(m->cgroup_root);
        r = cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, 0, &m->cgroup_root);
        if (r < 0)
                return log_error_errno(r, "Cannot determine cgroup we are running in: %m");

        /* Chop off the init scope, if we are already located in it */
        e = endswith(m->cgroup_root, "/" SPECIAL_INIT_SCOPE);

        /* LEGACY: Also chop off the system slice if we are in
         * it. This is to support live upgrades from older systemd
         * versions where PID 1 was moved there. Also see
         * cg_get_root_path(). */
        if (!e && m->running_as == MANAGER_SYSTEM) {
                e = endswith(m->cgroup_root, "/" SPECIAL_SYSTEM_SLICE);
                if (!e)
                        e = endswith(m->cgroup_root, "/system"); /* even more legacy */
        }
        if (e)
                *e = 0;

        /* And make sure to store away the root value without trailing
         * slash, even for the root dir, so that we can easily prepend
         * it everywhere. */
        while ((e = endswith(m->cgroup_root, "/")))
                *e = 0;

        /* 2. Show data */
        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, m->cgroup_root, NULL, &path);
        if (r < 0)
                return log_error_errno(r, "Cannot find cgroup mount point: %m");

        unified = cg_unified();
        if (unified < 0)
                return log_error_errno(r, "Couldn't determine if we are running in the unified hierarchy: %m");
        if (unified > 0)
                log_debug("Unified cgroup hierarchy is located at %s.", path);
        else
                log_debug("Using cgroup controller " SYSTEMD_CGROUP_CONTROLLER ". File system hierarchy is at %s.", path);

        if (!m->test_run) {
                const char *scope_path;

                /* 3. Install agent */
                if (unified) {

                        /* In the unified hierarchy we can can get
                         * cgroup empty notifications via inotify. */

                        m->cgroup_inotify_event_source = sd_event_source_unref(m->cgroup_inotify_event_source);
                        safe_close(m->cgroup_inotify_fd);

                        m->cgroup_inotify_fd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
                        if (m->cgroup_inotify_fd < 0)
                                return log_error_errno(errno, "Failed to create control group inotify object: %m");

                        r = sd_event_add_io(m->event, &m->cgroup_inotify_event_source, m->cgroup_inotify_fd, EPOLLIN, on_cgroup_inotify_event, m);
                        if (r < 0)
                                return log_error_errno(r, "Failed to watch control group inotify object: %m");

                        r = sd_event_source_set_priority(m->cgroup_inotify_event_source, SD_EVENT_PRIORITY_IDLE - 5);
                        if (r < 0)
                                return log_error_errno(r, "Failed to set priority of inotify event source: %m");

                        (void) sd_event_source_set_description(m->cgroup_inotify_event_source, "cgroup-inotify");

                } else if (m->running_as == MANAGER_SYSTEM) {

                        /* On the legacy hierarchy we only get
                         * notifications via cgroup agents. (Which
                         * isn't really reliable, since it does not
                         * generate events when control groups with
                         * children run empty. */

                        r = cg_install_release_agent(SYSTEMD_CGROUP_CONTROLLER, SYSTEMD_CGROUP_AGENT_PATH);
                        if (r < 0)
                                log_warning_errno(r, "Failed to install release agent, ignoring: %m");
                        else if (r > 0)
                                log_debug("Installed release agent.");
                        else if (r == 0)
                                log_debug("Release agent already installed.");
                }

                /* 4. Make sure we are in the special "init.scope" unit in the root slice. */
                scope_path = strjoina(m->cgroup_root, "/" SPECIAL_INIT_SCOPE);
                r = cg_create_and_attach(SYSTEMD_CGROUP_CONTROLLER, scope_path, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to create %s control group: %m", scope_path);

                /* also, move all other userspace processes remaining
                 * in the root cgroup into that scope. */
                r = cg_migrate(SYSTEMD_CGROUP_CONTROLLER, m->cgroup_root, SYSTEMD_CGROUP_CONTROLLER, scope_path, false);
                if (r < 0)
                        log_warning_errno(r, "Couldn't move remaining userspace processes, ignoring: %m");

                /* 5. And pin it, so that it cannot be unmounted */
                safe_close(m->pin_cgroupfs_fd);
                m->pin_cgroupfs_fd = open(path, O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOCTTY|O_NONBLOCK);
                if (m->pin_cgroupfs_fd < 0)
                        return log_error_errno(errno, "Failed to open pin file: %m");

                /* 6.  Always enable hierarchical support if it exists... */
                if (!unified)
                        (void) cg_set_attribute("memory", "/", "memory.use_hierarchy", "1");
        }

        /* 7. Figure out which controllers are supported */
        r = cg_mask_supported(&m->cgroup_supported);
        if (r < 0)
                return log_error_errno(r, "Failed to determine supported controllers: %m");

        for (c = 0; c < _CGROUP_CONTROLLER_MAX; c++)
                log_debug("Controller '%s' supported: %s", cgroup_controller_to_string(c), yes_no(m->cgroup_supported & c));

        return 0;
}

void manager_shutdown_cgroup(Manager *m, bool delete) {
        assert(m);

        /* We can't really delete the group, since we are in it. But
         * let's trim it. */
        if (delete && m->cgroup_root)
                (void) cg_trim(SYSTEMD_CGROUP_CONTROLLER, m->cgroup_root, false);

        m->cgroup_inotify_wd_unit = hashmap_free(m->cgroup_inotify_wd_unit);

        m->cgroup_inotify_event_source = sd_event_source_unref(m->cgroup_inotify_event_source);
        m->cgroup_inotify_fd = safe_close(m->cgroup_inotify_fd);

        m->pin_cgroupfs_fd = safe_close(m->pin_cgroupfs_fd);

        m->cgroup_root = mfree(m->cgroup_root);
}

Unit* manager_get_unit_by_cgroup(Manager *m, const char *cgroup) {
        char *p;
        Unit *u;

        assert(m);
        assert(cgroup);

        u = hashmap_get(m->cgroup_unit, cgroup);
        if (u)
                return u;

        p = strdupa(cgroup);
        for (;;) {
                char *e;

                e = strrchr(p, '/');
                if (!e || e == p)
                        return hashmap_get(m->cgroup_unit, SPECIAL_ROOT_SLICE);

                *e = 0;

                u = hashmap_get(m->cgroup_unit, p);
                if (u)
                        return u;
        }
}

Unit *manager_get_unit_by_pid_cgroup(Manager *m, pid_t pid) {
        _cleanup_free_ char *cgroup = NULL;
        int r;

        assert(m);

        if (pid <= 0)
                return NULL;

        r = cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, pid, &cgroup);
        if (r < 0)
                return NULL;

        return manager_get_unit_by_cgroup(m, cgroup);
}

Unit *manager_get_unit_by_pid(Manager *m, pid_t pid) {
        Unit *u;

        assert(m);

        if (pid <= 0)
                return NULL;

        if (pid == 1)
                return hashmap_get(m->units, SPECIAL_INIT_SCOPE);

        u = hashmap_get(m->watch_pids1, PID_TO_PTR(pid));
        if (u)
                return u;

        u = hashmap_get(m->watch_pids2, PID_TO_PTR(pid));
        if (u)
                return u;

        return manager_get_unit_by_pid_cgroup(m, pid);
}

int manager_notify_cgroup_empty(Manager *m, const char *cgroup) {
        Unit *u;

        assert(m);
        assert(cgroup);

        u = manager_get_unit_by_cgroup(m, cgroup);
        if (!u)
                return 0;

        return unit_notify_cgroup_empty(u);
}

int unit_get_memory_current(Unit *u, uint64_t *ret) {
        _cleanup_free_ char *v = NULL;
        int r;

        assert(u);
        assert(ret);

        if (!u->cgroup_path)
                return -ENODATA;

        if ((u->cgroup_realized_mask & CGROUP_MASK_MEMORY) == 0)
                return -ENODATA;

        if (cg_unified() <= 0)
                r = cg_get_attribute("memory", u->cgroup_path, "memory.usage_in_bytes", &v);
        else
                r = cg_get_attribute("memory", u->cgroup_path, "memory.current", &v);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;

        return safe_atou64(v, ret);
}

static int unit_get_cpu_usage_raw(Unit *u, nsec_t *ret) {
        _cleanup_free_ char *v = NULL;
        uint64_t ns;
        int r;

        assert(u);
        assert(ret);

        if (!u->cgroup_path)
                return -ENODATA;

        if ((u->cgroup_realized_mask & CGROUP_MASK_CPUACCT) == 0)
                return -ENODATA;

        r = cg_get_attribute("cpuacct", u->cgroup_path, "cpuacct.usage", &v);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;

        r = safe_atou64(v, &ns);
        if (r < 0)
                return r;

        *ret = ns;
        return 0;
}

int unit_get_cpu_usage(Unit *u, nsec_t *ret) {
        nsec_t ns;
        int r;

        r = unit_get_cpu_usage_raw(u, &ns);
        if (r < 0)
                return r;

        if (ns > u->cpuacct_usage_base)
                ns -= u->cpuacct_usage_base;
        else
                ns = 0;

        *ret = ns;
        return 0;
}

int unit_reset_cpu_usage(Unit *u) {
        nsec_t ns;
        int r;

        assert(u);

        r = unit_get_cpu_usage_raw(u, &ns);
        if (r < 0) {
                u->cpuacct_usage_base = 0;
                return r;
        }

        u->cpuacct_usage_base = ns;
        return 0;
}

bool unit_cgroup_delegate(Unit *u) {
        CGroupContext *c;

        assert(u);

        c = unit_get_cgroup_context(u);
        if (!c)
                return false;

        return c->delegate;
}

static const char* const cgroup_device_policy_table[_CGROUP_DEVICE_POLICY_MAX] = {
        [CGROUP_AUTO] = "auto",
        [CGROUP_CLOSED] = "closed",
        [CGROUP_STRICT] = "strict",
};

DEFINE_STRING_TABLE_LOOKUP(cgroup_device_policy, CGroupDevicePolicy);
