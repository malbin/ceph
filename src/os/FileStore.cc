// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>

#if defined(__linux__)
#include <linux/fs.h>
#endif

#include <iostream>
#include <map>

#if defined(__FreeBSD__)
#include "include/inttypes.h"
#endif

#include "include/compat.h"
#include "include/fiemap.h"

#include "common/xattr.h"

#if defined(DARWIN) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#endif // DARWIN


#include <fstream>
#include <sstream>

#include "FileStore.h"
#include "common/BackTrace.h"
#include "include/types.h"
#include "FileJournal.h"

#include "osd/osd_types.h"
#include "include/color.h"
#include "include/buffer.h"

#include "common/Timer.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/run_cmd.h"
#include "common/safe_io.h"
#include "common/perf_counters.h"
#include "common/sync_filesystem.h"
#include "HashIndex.h"
#include "DBObjectMap.h"
#include "LevelDBStore.h"

#include "common/ceph_crypto.h"
using ceph::crypto::SHA1;

#ifndef __CYGWIN__
#  include "btrfs_ioctl.h"
#endif

#include "include/assert.h"

#include "common/config.h"

#define dout_subsys ceph_subsys_filestore
#undef dout_prefix
#define dout_prefix *_dout << "filestore(" << basedir << ") "

#if defined(__linux__)
# ifndef BTRFS_SUPER_MAGIC
static const __SWORD_TYPE BTRFS_SUPER_MAGIC(0x9123683E);
# endif
#endif

#define ATTR_MAX_NAME_LEN  128
#define ATTR_MAX_BLOCK_LEN 2048

#define COMMIT_SNAP_ITEM "snap_%lld"
#define CLUSTER_SNAP_ITEM "clustersnap_%s"

#define REPLAY_GUARD_XATTR "user.cephos.seq"

/*
 * long file names will have the following format:
 *
 * prefix_hash_index_cookie
 *
 * The prefix will just be the first X bytes of the original file name.
 * The cookie is a constant string that shows whether this file name
 * is hashed
 */

#define FILENAME_LFN_DIGEST_SIZE CEPH_CRYPTO_SHA1_DIGESTSIZE

#define FILENAME_MAX_LEN        4096    // the long file name size
#define FILENAME_SHORT_LEN      255     // the short file name size
#define FILENAME_COOKIE         "long"  // ceph long file name
#define FILENAME_HASH_LEN       FILENAME_LFN_DIGEST_SIZE
#define FILENAME_EXTRA	        4       // underscores and digit

#define LFN_ATTR "user.cephos.lfn"

#define FILENAME_PREFIX_LEN (FILENAME_SHORT_LEN - FILENAME_HASH_LEN - (sizeof(FILENAME_COOKIE) - 1) - FILENAME_EXTRA)
#define ALIGN_DOWN(x, by) ((x) - ((x) % (by)))
#define ALIGNED(x, by) (!((x) % (by)))
#define ALIGN_UP(x, by) (ALIGNED((x), (by)) ? (x) : (ALIGN_DOWN((x), (by)) + (by)))


ostream& operator<<(ostream& out, const FileStore::OpSequencer& s)
{
  assert(&out);
  return out << *s.parent;
}

int do_getxattr(const char *fn, const char *name, void *val, size_t size);
int do_fgetxattr(int fd, const char *name, void *val, size_t size);
int do_setxattr(const char *fn, const char *name, const void *val, size_t size);
int do_fsetxattr(int fd, const char *name, const void *val, size_t size);
int do_setxattr(const char *fn, const char *name, const void *val, size_t size);
int do_listxattr(const char *fn, char *names, size_t len);
int do_removexattr(const char *fn, const char *name);

static int sys_fgetxattr(int fd, const char *name, void *val, size_t size)
{
  int r = ::ceph_os_fgetxattr(fd, name, val, size);
  return (r < 0 ? -errno : r);
}

static int sys_setxattr(const char *fn, const char *name, const void *val, size_t size)
{
  int r = ::ceph_os_setxattr(fn, name, val, size);
  return (r < 0 ? -errno : r);
}

static int sys_removexattr(const char *fn, const char *name)
{
  int r = ::ceph_os_removexattr(fn, name);
  return (r < 0 ? -errno : r);
}

int sys_listxattr(const char *fn, char *names, size_t len)
{
  int r = ::ceph_os_listxattr(fn, names, len);
  return (r < 0 ? -errno : r);
}

int FileStore::get_cdir(coll_t cid, char *s, int len) 
{
  const string &cid_str(cid.to_str());
  return snprintf(s, len, "%s/current/%s", basedir.c_str(), cid_str.c_str());
}

int FileStore::get_index(coll_t cid, Index *index)
{
  char path[PATH_MAX];
  get_cdir(cid, path, sizeof(path));
  return index_manager.get_index(cid, path, index);
}

int FileStore::init_index(coll_t cid)
{
  char path[PATH_MAX];
  get_cdir(cid, path, sizeof(path));
  return index_manager.init_index(cid, path, on_disk_version);
}

int FileStore::lfn_find(coll_t cid, const hobject_t& oid, IndexedPath *path)
{
  Index index; 
  int r, exist;
  r = get_index(cid, &index);
  if (r < 0)
    return r;

  r = index->lookup(oid, path, &exist);
  if (r < 0)
    return r;
  if (!exist)
    return -ENOENT;
  return 0;
}

int FileStore::lfn_getxattr(coll_t cid, const hobject_t& oid, const char *name, void *val, size_t size)
{
  IndexedPath path;
  int r = lfn_find(cid, oid, &path);
  if (r < 0)
    return r;
  return do_getxattr(path->path(), name, val, size);
}

int FileStore::lfn_setxattr(coll_t cid, const hobject_t& oid, const char *name, const void *val, size_t size)
{
  IndexedPath path;
  int r = lfn_find(cid, oid, &path);
  if (r < 0)
    return r;
  return do_setxattr(path->path(), name, val, size);
}

int FileStore::lfn_removexattr(coll_t cid, const hobject_t& oid, const char *name)
{
  IndexedPath path;
  int r = lfn_find(cid, oid, &path);
  if (r < 0)
    return r;
  return do_removexattr(path->path(), name);
}

int FileStore::lfn_listxattr(coll_t cid, const hobject_t& oid, char *names, size_t len)
{
  IndexedPath path;
  int r = lfn_find(cid, oid, &path);
  if (r < 0)
    return r;
  return do_listxattr(path->path(), names, len);
}

int FileStore::lfn_truncate(coll_t cid, const hobject_t& oid, off_t length)
{
  IndexedPath path;
  int r = lfn_find(cid, oid, &path);
  if (r < 0)
    return r;
  r = ::truncate(path->path(), length);
  if (r < 0)
    return -errno;
  return r;
}

int FileStore::lfn_stat(coll_t cid, const hobject_t& oid, struct stat *buf)
{
  IndexedPath path;
  int r = lfn_find(cid, oid, &path);
  if (r < 0)
    return r;
  r = ::stat(path->path(), buf);
  if (r < 0)
    return -errno;
  return 0;
}

int FileStore::lfn_open(coll_t cid, const hobject_t& oid, int flags, mode_t mode,
			IndexedPath *path,
			Index *index) {
  Index index2;
  IndexedPath path2;
  if (!path)
    path = &path2;
  int fd, exist;
  int r = 0;
  if (!index) {
    index = &index2;
  }
  if (!(*index)) {
    r = get_index(cid, index);
  }
  if (r < 0) {
    derr << "error getting collection index for " << cid
	 << ": " << cpp_strerror(-r) << dendl;
    return r;
  }
  r = (*index)->lookup(oid, path, &exist);
  if (r < 0) {
    derr << "could not find " << oid << " in index: "
	 << cpp_strerror(-r) << dendl;
    return r;
  }

  r = ::open((*path)->path(), flags, mode);
  if (r < 0) {
    r = -errno;
    dout(10) << "error opening file " << (*path)->path() << " with flags="
	     << flags << " and mode=" << mode << ": " << cpp_strerror(-r) << dendl;
    return r;
  }
  fd = r;

  if ((flags & O_CREAT) && (!exist)) {
    r = (*index)->created(oid, (*path)->path());
    if (r < 0) {
      TEMP_FAILURE_RETRY(::close(fd));
      derr << "error creating " << oid << " (" << (*path)->path()
	   << ") in index: " << cpp_strerror(-r) << dendl;
      return r;
    }
  }
  return fd;
}

int FileStore::lfn_open(coll_t cid, const hobject_t& oid, int flags, mode_t mode, IndexedPath *path)
{
  return lfn_open(cid, oid, flags, mode, path, 0);
}

int FileStore::lfn_open(coll_t cid, const hobject_t& oid, int flags, mode_t mode)
{
  return lfn_open(cid, oid, flags, mode, 0, 0);
}

int FileStore::lfn_open(coll_t cid, const hobject_t& oid, int flags)
{
  return lfn_open(cid, oid, flags, 0);
}

int FileStore::lfn_link(coll_t c, coll_t cid, const hobject_t& o) 
{
  Index index_new, index_old;
  IndexedPath path_new, path_old;
  int exist;
  int r;
  if (c < cid) {
    r = get_index(cid, &index_new);
    if (r < 0)
      return r;
    r = get_index(c, &index_old);
    if (r < 0)
      return r;
  } else {
    r = get_index(c, &index_old);
    if (r < 0)
      return r;
    r = get_index(cid, &index_new);
    if (r < 0)
      return r;
  }

  r = index_old->lookup(o, &path_old, &exist);
  if (r < 0)
    return r;
  if (!exist)
    return -ENOENT;

  r = index_new->lookup(o, &path_new, &exist);
  if (r < 0)
    return r;
  if (exist)
    return -EEXIST;

  dout(25) << "lfn_link path_old: " << path_old << dendl;
  dout(25) << "lfn_link path_new: " << path_new << dendl;
  r = ::link(path_old->path(), path_new->path());
  if (r < 0)
    return -errno;

  r = index_new->created(o, path_new->path());
  if (r < 0)
    return r;
  return 0;
}

int FileStore::lfn_unlink(coll_t cid, const hobject_t& o,
			  const SequencerPosition &spos)
{
  Index index;
  int r = get_index(cid, &index);
  if (r < 0)
    return r;
  {
    IndexedPath path;
    int exist;
    r = index->lookup(o, &path, &exist);
    if (r < 0)
      return r;

    struct stat st;
    r = ::stat(path->path(), &st);
    if (r < 0) {
      return -errno;
    }
    if (st.st_nlink == 1) {
      r = object_map->clear(o, &spos);
      if (r < 0 && r != -ENOENT)
	return r;
    } else {
      /* Ensure that replay of this op doesn't result in the object_map
       * going away.
       */
      if (!btrfs_stable_commits)
	object_map->sync(&o, &spos);
    }
  }
  return index->unlink(o);
}

static void get_raw_xattr_name(const char *name, int i, char *raw_name, int raw_len)
{
  int r;
  int pos = 0;

  while (*name) {
    switch (*name) {
    case '@': /* escape it */
      pos += 2;
      assert (pos < raw_len - 1);
      *raw_name = '@';
      raw_name++;
      *raw_name = '@';
      break;
    default:
      pos++;
      assert(pos < raw_len - 1);
      *raw_name = *name;
      break;
    }
    name++;
    raw_name++;
  }

  if (!i) {
    *raw_name = '\0';
  } else {
    r = snprintf(raw_name, raw_len, "@%d", i);
    assert(r < raw_len - pos);
  }
}

static int translate_raw_name(const char *raw_name, char *name, int name_len, bool *is_first)
{
  int pos = 0;

  generic_dout(10) << "translate_raw_name raw_name=" << raw_name << dendl;
  const char *n = name;

  *is_first = true;
  while (*raw_name) {
    switch (*raw_name) {
    case '@': /* escape it */
      raw_name++;
      if (!*raw_name)
        break;
      if (*raw_name != '@') {
        *is_first = false;
        goto done;
      }

    /* fall through */
    default:
      *name = *raw_name;
      break;
    }
    pos++;
    assert(pos < name_len);
    name++;
    raw_name++;
  }
done:
  *name = '\0';
  generic_dout(10) << "translate_raw_name name=" << n << dendl;
  return pos;
}

int do_fgetxattr_len(int fd, const char *name)
{
  int i = 0, total = 0;
  char raw_name[ATTR_MAX_NAME_LEN * 2 + 16];
  int r;

  do {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    r = sys_fgetxattr(fd, raw_name, 0, 0);
    if (!i && r < 0) {
      return r;
    }
    if (r < 0)
      break;
    total += r;
    i++;
  } while (r == ATTR_MAX_BLOCK_LEN);

  return total;
}

int do_getxattr(const char *fn, const char *name, void *val, size_t size)
{
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0)
    return -errno;
  int r = do_fgetxattr(fd, name, val, size);
  TEMP_FAILURE_RETRY(::close(fd));
  return r;
}

int do_fgetxattr(int fd, const char *name, void *val, size_t size)
{
  int i = 0, pos = 0;
  char raw_name[ATTR_MAX_NAME_LEN * 2 + 16];
  int ret = 0;
  int r;
  size_t chunk_size;

  if (!size)
    return do_fgetxattr_len(fd, name);

  do {
    chunk_size = (size < ATTR_MAX_BLOCK_LEN ? size : ATTR_MAX_BLOCK_LEN);
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    size -= chunk_size;

    r = sys_fgetxattr(fd, raw_name, (char *)val + pos, chunk_size);
    if (r < 0) {
      ret = r;
      break;
    }

    if (r > 0)
      pos += r;

    i++;
  } while (size && r == ATTR_MAX_BLOCK_LEN);

  if (r >= 0) {
    ret = pos;
    /* is there another chunk? that can happen if the last read size span over
       exactly one block */
    if (chunk_size == ATTR_MAX_BLOCK_LEN) {
      get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
      r = sys_fgetxattr(fd, raw_name, 0, 0);
      if (r > 0) { // there's another chunk.. the original buffer was too small
        ret = -ERANGE;
      }
    }
  }
  return ret;
}

int do_setxattr(const char *fn, const char *name, const void *val, size_t size) {
  int i = 0, pos = 0;
  char raw_name[ATTR_MAX_NAME_LEN * 2 + 16];
  int ret = 0;
  size_t chunk_size;

  do {
    chunk_size = (size < ATTR_MAX_BLOCK_LEN ? size : ATTR_MAX_BLOCK_LEN);
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    size -= chunk_size;

    int r = sys_setxattr(fn, raw_name, (char *)val + pos, chunk_size);
    if (r < 0) {
      ret = r;
      break;
    }
    pos  += chunk_size;
    ret = pos;
    i++;
  } while (size);

  /* if we're exactly at a chunk size, remove the next one (if wasn't removed
     before) */
  if (ret >= 0 && chunk_size == ATTR_MAX_BLOCK_LEN) {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    do_removexattr(fn, raw_name);
  }
  
  return ret;
}

int do_fsetxattr(int fd, const char *name, const void *val, size_t size)
{
  int i = 0, pos = 0;
  char raw_name[ATTR_MAX_NAME_LEN * 2 + 16];
  int ret = 0;
  size_t chunk_size;

  do {
    chunk_size = (size < ATTR_MAX_BLOCK_LEN ? size : ATTR_MAX_BLOCK_LEN);
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    size -= chunk_size;

    int r = ::ceph_os_fsetxattr(fd, raw_name, (char *)val + pos, chunk_size);
    if (r < 0) {
      ret = r;
      break;
    }
    pos  += chunk_size;
    ret = pos;
    i++;
  } while (size);

  /* if we're exactly at a chunk size, remove the next one (if wasn't removed
     before) */
  if (ret >= 0 && chunk_size == ATTR_MAX_BLOCK_LEN) {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    ::ceph_os_fremovexattr(fd, raw_name);
  }
  
  return ret;
}

int do_removexattr(const char *fn, const char *name) {
  int i = 0;
  char raw_name[ATTR_MAX_NAME_LEN * 2 + 16];
  int r;

  do {
    get_raw_xattr_name(name, i, raw_name, sizeof(raw_name));
    r = sys_removexattr(fn, raw_name);
    if (!i && r < 0) {
      return r;
    }
    i++;
  } while (r >= 0);
  return 0;
}

int do_listxattr(const char *fn, char *names, size_t len) {
  int r;

  if (!len)
   return sys_listxattr(fn, names, len);

  r = sys_listxattr(fn, 0, 0);
  if (r < 0)
    return r;

  size_t total_len = r  * 2; // should be enough
  char *full_buf = (char *)malloc(total_len * 2);
  if (!full_buf)
    return -ENOMEM;

  r = sys_listxattr(fn, full_buf, total_len);
  if (r < 0)
    return r;

  char *p = full_buf;
  char *end = full_buf + r;
  char *dest = names;
  char *dest_end = names + len;

  while (p < end) {
    char name[ATTR_MAX_NAME_LEN * 2 + 16];
    int attr_len = strlen(p);
    bool is_first;
    int name_len = translate_raw_name(p, name, sizeof(name), &is_first);
    if (is_first)  {
      if (dest + name_len > dest_end) {
        r = -ERANGE;
        goto done;
      }
      strcpy(dest, name);
      dest += name_len + 1;
    }
    p += attr_len + 1;
  }
  r = dest - names;

done:
  free(full_buf);
  return r;
}

FileStore::FileStore(const std::string &base, const std::string &jdev, const char *name, bool do_update) :
  internal_name(name),
  basedir(base), journalpath(jdev),
  btrfs(false),
  btrfs_stable_commits(false),
  blk_size(0),
  btrfs_trans_start_end(false), btrfs_clone_range(false),
  btrfs_snap_create(false),
  btrfs_snap_destroy(false),
  btrfs_snap_create_v2(false),
  btrfs_wait_sync(false),
  ioctl_fiemap(false),
  fsid_fd(-1), op_fd(-1),
  basedir_fd(-1), current_fd(-1),
  index_manager(do_update),
  ondisk_finisher(g_ceph_context),
  lock("FileStore::lock"),
  force_sync(false), sync_epoch(0),
  sync_entry_timeo_lock("sync_entry_timeo_lock"),
  timer(g_ceph_context, sync_entry_timeo_lock),
  stop(false), sync_thread(this),
  default_osr("default"),
  op_queue_len(0), op_queue_bytes(0), op_finisher(g_ceph_context), next_finish(0),
  op_tp(g_ceph_context, "FileStore::op_tp", g_conf->filestore_op_threads),
  op_wq(this, g_conf->filestore_op_thread_timeout,
	g_conf->filestore_op_thread_suicide_timeout, &op_tp),
  flusher_queue_len(0), flusher_thread(this),
  logger(NULL),
  m_filestore_btrfs_clone_range(g_conf->filestore_btrfs_clone_range),
  m_filestore_btrfs_snap (g_conf->filestore_btrfs_snap ),
  m_filestore_commit_timeout(g_conf->filestore_commit_timeout),
  m_filestore_fiemap(g_conf->filestore_fiemap),
  m_filestore_flusher (g_conf->filestore_flusher ),
  m_filestore_fsync_flushes_journal_data(g_conf->filestore_fsync_flushes_journal_data),
  m_filestore_journal_parallel(g_conf->filestore_journal_parallel ),
  m_filestore_journal_trailing(g_conf->filestore_journal_trailing),
  m_filestore_journal_writeahead(g_conf->filestore_journal_writeahead),
  m_filestore_fiemap_threshold(g_conf->filestore_fiemap_threshold),
  m_filestore_sync_flush(g_conf->filestore_sync_flush),
  m_filestore_flusher_max_fds(g_conf->filestore_flusher_max_fds),
  m_filestore_flush_min(g_conf->filestore_flush_min),
  m_filestore_max_sync_interval(g_conf->filestore_max_sync_interval),
  m_filestore_min_sync_interval(g_conf->filestore_min_sync_interval),
  do_update(do_update),
  m_journal_dio(g_conf->journal_dio),
  m_journal_aio(g_conf->journal_aio),
  m_osd_rollback_to_cluster_snap(g_conf->osd_rollback_to_cluster_snap),
  m_osd_use_stale_snap(g_conf->osd_use_stale_snap),
  m_filestore_queue_max_ops(g_conf->filestore_queue_max_ops),
  m_filestore_queue_max_bytes(g_conf->filestore_queue_max_bytes),
  m_filestore_queue_committing_max_ops(g_conf->filestore_queue_committing_max_ops),
  m_filestore_queue_committing_max_bytes(g_conf->filestore_queue_committing_max_bytes),
  m_filestore_do_dump(false),
  m_filestore_dump_fmt(true)
{
  m_filestore_kill_at.set(g_conf->filestore_kill_at);

  ostringstream oss;
  oss << basedir << "/current";
  current_fn = oss.str();

  ostringstream sss;
  sss << basedir << "/current/commit_op_seq";
  current_op_seq_fn = sss.str();

  ostringstream omss;
  omss << basedir << "/current/omap";
  omap_dir = omss.str();

  // initialize logger
  PerfCountersBuilder plb(g_ceph_context, internal_name, l_os_first, l_os_last);

  plb.add_u64(l_os_jq_max_ops, "journal_queue_max_ops");
  plb.add_u64(l_os_jq_ops, "journal_queue_ops");
  plb.add_u64_counter(l_os_j_ops, "journal_ops");
  plb.add_u64(l_os_jq_max_bytes, "journal_queue_max_bytes");
  plb.add_u64(l_os_jq_bytes, "journal_queue_bytes");
  plb.add_u64_counter(l_os_j_bytes, "journal_bytes");
  plb.add_fl_avg(l_os_j_lat, "journal_latency");
  plb.add_u64(l_os_oq_max_ops, "op_queue_max_ops");
  plb.add_u64(l_os_oq_ops, "op_queue_ops");
  plb.add_u64_counter(l_os_ops, "ops");
  plb.add_u64(l_os_oq_max_bytes, "op_queue_max_bytes");
  plb.add_u64(l_os_oq_bytes, "op_queue_bytes");
  plb.add_u64_counter(l_os_bytes, "bytes");
  plb.add_fl_avg(l_os_apply_lat, "apply_latency");
  plb.add_u64(l_os_committing, "committing");

  plb.add_u64_counter(l_os_commit, "commitcycle");
  plb.add_fl_avg(l_os_commit_len, "commitcycle_interval");
  plb.add_fl_avg(l_os_commit_lat, "commitcycle_latency");
  plb.add_u64_counter(l_os_j_full, "journal_full");

  logger = plb.create_perf_counters();
}

FileStore::~FileStore()
{
  if (journal)
    journal->logger = NULL;
  delete logger;

  if (m_filestore_do_dump) {
    dump_stop();
  }
}

static void get_attrname(const char *name, char *buf, int len)
{
  snprintf(buf, len, "user.ceph.%s", name);
}

bool parse_attrname(char **name)
{
  if (strncmp(*name, "user.ceph.", 10) == 0) {
    *name += 10;
    return true;
  }
  return false;
}

static int do_fiemap(int fd, off_t start, size_t len, struct fiemap **pfiemap)
{
  struct fiemap *fiemap = NULL;
  int size;
  int ret;

  fiemap = (struct fiemap*)calloc(sizeof(struct fiemap), 1);
  if (!fiemap)
    return -ENOMEM;

  fiemap->fm_start = start;
  fiemap->fm_length = len;

  fsync(fd); /* flush extents to disk if needed */

  if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
    ret = -errno;
    goto done_err;
  }

  size = sizeof(struct fiemap_extent) * (fiemap->fm_mapped_extents);

  fiemap = (struct fiemap *)realloc(fiemap, sizeof(struct fiemap) +
                                    size);
  if (!fiemap) {
    ret = -ENOMEM;
    goto done_err;
  }

  memset(fiemap->fm_extents, 0, size);

  fiemap->fm_extent_count = fiemap->fm_mapped_extents;
  fiemap->fm_mapped_extents = 0;

  if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
    ret = -errno;
    goto done_err;
  }
  *pfiemap = fiemap;

  return 0;

done_err:
  *pfiemap = NULL;
  free(fiemap);
  return ret;
}

int FileStore::statfs(struct statfs *buf)
{
  if (::statfs(basedir.c_str(), buf) < 0)
    return -errno;
  return 0;
}


int FileStore::open_journal()
{
  if (journalpath.length()) {
    dout(10) << "open_journal at " << journalpath << dendl;
    journal = new FileJournal(fsid, &finisher, &sync_cond, journalpath.c_str(),
			      m_journal_dio, m_journal_aio);
    if (journal)
      journal->logger = logger;
  }
  return 0;
}

int FileStore::dump_journal(ostream& out)
{
  int r;

  if (!journalpath.length())
    return -EINVAL;

  FileJournal *journal = new FileJournal(fsid, &finisher, &sync_cond, journalpath.c_str(), m_journal_dio);
  r = journal->dump(out);
  delete journal;
  return r;
}

int FileStore::mkfs()
{
  int ret = 0;
  int basedir_fd;
  char fsid_fn[PATH_MAX];
  struct stat st;
  uuid_d old_fsid;

#if defined(__linux__)
  struct btrfs_ioctl_vol_args volargs;
  memset(&volargs, 0, sizeof(volargs));
#endif

  dout(1) << "mkfs in " << basedir << dendl;
  basedir_fd = ::open(basedir.c_str(), O_RDONLY);
  if (basedir_fd < 0) {
    ret = -errno;
    derr << "mkfs failed to open base dir " << basedir << ": " << cpp_strerror(ret) << dendl;
    return ret;
  }

  // open+lock fsid
  snprintf(fsid_fn, sizeof(fsid_fn), "%s/fsid", basedir.c_str());
  fsid_fd = ::open(fsid_fn, O_RDWR|O_CREAT, 0644);
  if (fsid_fd < 0) {
    ret = -errno;
    derr << "mkfs: failed to open " << fsid_fn << ": " << cpp_strerror(ret) << dendl;
    goto close_basedir_fd;
  }

  if (lock_fsid() < 0) {
    ret = -EBUSY;
    goto close_fsid_fd;
  }

  if (read_fsid(fsid_fd, &old_fsid) < 0 || old_fsid.is_zero()) {
    if (fsid.is_zero()) {
      fsid.generate_random();
      dout(1) << "mkfs generated fsid " << fsid << dendl;
    } else {
      dout(1) << "mkfs using provided fsid " << fsid << dendl;
    }

    char fsid_str[40];
    fsid.print(fsid_str);
    strcat(fsid_str, "\n");
    ret = ::ftruncate(fsid_fd, 0);
    if (ret < 0) {
      ret = -errno;
      derr << "mkfs: failed to truncate fsid: "
	   << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    ret = safe_write(fsid_fd, fsid_str, strlen(fsid_str));
    if (ret < 0) {
      derr << "mkfs: failed to write fsid: "
	   << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    if (::fsync(fsid_fd) < 0) {
      ret = errno;
      derr << "mkfs: close failed: can't write fsid: "
	   << cpp_strerror(ret) << dendl;
      goto close_fsid_fd;
    }
    dout(10) << "mkfs fsid is " << fsid << dendl;
  } else {
    if (!fsid.is_zero() && fsid != old_fsid) {
      derr << "mkfs on-disk fsid " << old_fsid << " != provided " << fsid << dendl;
      ret = -EINVAL;
      goto close_fsid_fd;
    }
    fsid = old_fsid;
    dout(1) << "mkfs fsid is already set to " << fsid << dendl;
  }

  // version stamp
  ret = write_version_stamp();
  if (ret < 0) {
    derr << "mkfs: write_version_stamp() failed: "
	 << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  }

  // current
  ret = ::stat(current_fn.c_str(), &st);
  if (ret == 0) {
    // current/ exists
    if (!S_ISDIR(st.st_mode)) {
      ret = -EINVAL;
      derr << "mkfs current/ exists but is not a directory" << dendl;
      goto close_fsid_fd;
    }

#if defined(__linux__)
    // is current/ a btrfs subvolume?
    //  check fsid, and compare st_dev to see if it's a subvolume.
    struct stat basest;
    struct statfs basefs, currentfs;
    ::fstat(basedir_fd, &basest);
    ::fstatfs(basedir_fd, &basefs);
    ::statfs(current_fn.c_str(), &currentfs);
    if (basefs.f_type == BTRFS_SUPER_MAGIC &&
	currentfs.f_type == BTRFS_SUPER_MAGIC &&
	basest.st_dev != st.st_dev) {
      dout(2) << " current appears to be a btrfs subvolume" << dendl;
      btrfs_stable_commits = true;
    }
#endif
  } else {
#if defined(__linux__)
    volargs.fd = 0;
    strcpy(volargs.name, "current");
    if (::ioctl(basedir_fd, BTRFS_IOC_SUBVOL_CREATE, (unsigned long int)&volargs)) {
      ret = -errno;
      if (ret == -EOPNOTSUPP || ret == -ENOTTY) {
	dout(2) << " BTRFS_IOC_SUBVOL_CREATE ioctl failed, trying mkdir "
		<< current_fn << dendl;
#endif
	if (::mkdir(current_fn.c_str(), 0755)) {
	  ret = -errno;
	  derr << "mkfs: mkdir " << current_fn << " failed: "
	       << cpp_strerror(ret) << dendl;
	  goto close_fsid_fd;
	}
#if defined(__linux__)
      }
      else {
	derr << "mkfs: BTRFS_IOC_SUBVOL_CREATE failed with error "
	     << cpp_strerror(ret) << dendl;
	goto close_fsid_fd;
      }
    }
    else {
      // ioctl succeeded. yay
      dout(2) << " created btrfs subvol " << current_fn << dendl;
      if (::chmod(current_fn.c_str(), 0755)) {
	ret = -errno;
	derr << "mkfs: failed to chmod " << current_fn << " to 0755: "
	     << cpp_strerror(ret) << dendl;
	goto close_fsid_fd;
      }
      btrfs_stable_commits = true;
    }
#endif
  }

  // write initial op_seq
  {
    uint64_t initial_seq = 0;
    int fd = read_op_seq(&initial_seq);
    if (fd < 0) {
      derr << "mkfs: failed to create " << current_op_seq_fn << ": "
	   << cpp_strerror(fd) << dendl;
      goto close_fsid_fd;
    }
    if (initial_seq == 0) {
      int err = write_op_seq(fd, 1);
      if (err < 0) {
	TEMP_FAILURE_RETRY(::close(fd));
	derr << "mkfs: failed to write to " << current_op_seq_fn << ": "
	     << cpp_strerror(err) << dendl;
	goto close_fsid_fd;
      }

      if (btrfs_stable_commits) {
	// create snap_1 too
	snprintf(volargs.name, sizeof(volargs.name), COMMIT_SNAP_ITEM, 1ull);
	volargs.fd = ::open(current_fn.c_str(), O_RDONLY);
	assert(volargs.fd >= 0);
	if (::ioctl(basedir_fd, BTRFS_IOC_SNAP_CREATE, (unsigned long int)&volargs)) {
	  ret = -errno;
	  if (ret != -EEXIST) {
	    derr << "mkfs: failed to create " << volargs.name << ": "
		 << cpp_strerror(ret) << dendl;
	    goto close_fsid_fd;
	  }
	}
	if (::fchmod(volargs.fd, 0755)) {
	  TEMP_FAILURE_RETRY(::close(volargs.fd));
	  ret = -errno;
	  derr << "mkfs: failed to chmod " << basedir << "/" << volargs.name << " to 0755: "
	       << cpp_strerror(ret) << dendl;
	  goto close_fsid_fd;
	}
	TEMP_FAILURE_RETRY(::close(volargs.fd));
      }
    }
    TEMP_FAILURE_RETRY(::close(fd));  
  }

  {
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB *db;
    leveldb::Status status = leveldb::DB::Open(options, omap_dir, &db);
    if (status.ok()) {
      delete db;
      dout(1) << "leveldb db exists/created" << dendl;
    } else {
      derr << "mkfs failed to create leveldb: " << status.ToString() << dendl;
      ret = -1;
      goto close_fsid_fd;
    }
  }

  // journal?
  ret = mkjournal();
  if (ret)
    goto close_fsid_fd;

  dout(1) << "mkfs done in " << basedir << dendl;
  ret = 0;

 close_fsid_fd:
  TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
 close_basedir_fd:
  TEMP_FAILURE_RETRY(::close(basedir_fd));
  basedir_fd = -1;
  return ret;
}

int FileStore::mkjournal()
{
  // read fsid
  int ret;
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/fsid", basedir.c_str());
  int fd = ::open(fn, O_RDONLY, 0644);
  if (fd < 0) {
    int err = errno;
    derr << "FileStore::mkjournal: open error: " << cpp_strerror(err) << dendl;
    return -err;
  }
  ret = read_fsid(fd, &fsid);
  if (ret < 0) {
    derr << "FileStore::mkjournal: read error: " << cpp_strerror(ret) << dendl;
    TEMP_FAILURE_RETRY(::close(fd));
    return ret;
  }
  TEMP_FAILURE_RETRY(::close(fd));

  ret = 0;

  open_journal();
  if (journal) {
    ret = journal->check();
    if (ret < 0) {
      ret = journal->create();
      if (ret)
	dout(0) << "mkjournal error creating journal on " << journalpath
		<< ": " << cpp_strerror(ret) << dendl;
      else
	dout(0) << "mkjournal created journal on " << journalpath << dendl;
    }
    delete journal;
    journal = 0;
  }
  return ret;
}

int FileStore::read_fsid(int fd, uuid_d *uuid)
{
  char fsid_str[40];
  int ret = safe_read(fd, fsid_str, sizeof(fsid_str));
  if (ret < 0)
    return ret;
  if (ret == 8) {
    // old 64-bit fsid... mirror it.
    *(uint64_t*)&uuid->uuid[0] = *(uint64_t*)fsid_str;
    *(uint64_t*)&uuid->uuid[8] = *(uint64_t*)fsid_str;
    return 0;
  }

  if (ret > 36)
    fsid_str[36] = 0;
  if (!uuid->parse(fsid_str))
    return -EINVAL;
  return 0;
}

int FileStore::lock_fsid()
{
  struct flock l;
  memset(&l, 0, sizeof(l));
  l.l_type = F_WRLCK;
  l.l_whence = SEEK_SET;
  l.l_start = 0;
  l.l_len = 0;
  int r = ::fcntl(fsid_fd, F_SETLK, &l);
  if (r < 0) {
    int err = errno;
    dout(0) << "lock_fsid failed to lock " << basedir << "/fsid, is another ceph-osd still running? "
	    << cpp_strerror(err) << dendl;
    return -err;
  }
  return 0;
}

bool FileStore::test_mount_in_use()
{
  dout(5) << "test_mount basedir " << basedir << " journal " << journalpath << dendl;
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/fsid", basedir.c_str());

  // verify fs isn't in use

  fsid_fd = ::open(fn, O_RDWR, 0644);
  if (fsid_fd < 0)
    return 0;   // no fsid, ok.
  bool inuse = lock_fsid() < 0;
  TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
  return inuse;
}

int FileStore::_test_fiemap()
{
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/fiemap_test", basedir.c_str());

  int fd = ::open(fn, O_CREAT|O_RDWR|O_TRUNC, 0644);
  if (fd < 0) {
    fd = -errno;
    derr << "_test_fiemap unable to create " << fn << ": " << cpp_strerror(fd) << dendl;
    return fd;
  }

  // ext4 has a bug in older kernels where fiemap will return an empty
  // result in some cases.  this is a file layout that triggers the bug
  // on 2.6.34-rc5.
  int v[] = {
    0x0000000000016000, 0x0000000000007000,
    0x000000000004a000, 0x0000000000007000,
    0x0000000000060000, 0x0000000000001000,
    0x0000000000061000, 0x0000000000008000,
    0x0000000000069000, 0x0000000000007000,
    0x00000000000a3000, 0x000000000000c000,
    0x000000000024e000, 0x000000000000c000,
    0x000000000028b000, 0x0000000000009000,
    0x00000000002b1000, 0x0000000000003000,
    0, 0
  };
  for (int i=0; v[i]; i++) {
    int off = v[i++];
    int len = v[i];

    // write a large extent
    char buf[len];
    memset(buf, 1, sizeof(buf));
    ::lseek(fd, off, SEEK_SET);
    int r = safe_write(fd, buf, sizeof(buf));
    if (r < 0) {
      derr << "_test_fiemap failed to write to " << fn << ": " << cpp_strerror(r) << dendl;
      return r;
    }
  }
  ::fsync(fd);

  // fiemap an extent inside that
  struct fiemap *fiemap;
  int r = do_fiemap(fd, 2430421, 59284, &fiemap);
  if (r < 0) {
    dout(0) << "mount FIEMAP ioctl is NOT supported" << dendl;
    ioctl_fiemap = false;
  } else {
    if (fiemap->fm_mapped_extents == 0) {
      dout(0) << "mount FIEMAP ioctl is supported, but buggy -- upgrade your kernel" << dendl;
      ioctl_fiemap = false;
    } else {
      dout(0) << "mount FIEMAP ioctl is supported and appears to work" << dendl;
      ioctl_fiemap = true;
    }
  }
  if (!m_filestore_fiemap) {
    dout(0) << "mount FIEMAP ioctl is disabled via 'filestore fiemap' config option" << dendl;
    ioctl_fiemap = false;
  }
  free(fiemap);

  ::unlink(fn);
  TEMP_FAILURE_RETRY(::close(fd));
  return 0;
}

int FileStore::_detect_fs()
{
  char fn[PATH_MAX];
  int x = rand();
  int y = x+1;

  snprintf(fn, sizeof(fn), "%s/xattr_test", basedir.c_str());

  int tmpfd = ::open(fn, O_CREAT|O_WRONLY|O_TRUNC, 0700);
  if (tmpfd < 0) {
    int ret = -errno;
    derr << "_detect_fs unable to create " << fn << ": " << cpp_strerror(ret) << dendl;
    return ret;
  }

  int ret = do_setxattr(fn, "user.test", &x, sizeof(x));
  if (ret >= 0)
    ret = do_getxattr(fn, "user.test", &y, sizeof(y));
  if ((ret < 0) || (x != y)) {
    derr << "Extended attributes don't appear to work. ";
    if (ret)
      *_dout << "Got error " + cpp_strerror(ret) + ". ";
    *_dout << "If you are using ext3 or ext4, be sure to mount the underlying "
	   << "file system with the 'user_xattr' option." << dendl;
    return -ENOTSUP;
  }

  char buf[1000];
  memset(buf, 0, sizeof(buf)); // shut up valgrind
  do_setxattr(fn, "user.test", &buf, sizeof(buf));
  do_setxattr(fn, "user.test2", &buf, sizeof(buf));
  do_setxattr(fn, "user.test3", &buf, sizeof(buf));
  do_setxattr(fn, "user.test4", &buf, sizeof(buf));
  ret = do_setxattr(fn, "user.test5", &buf, sizeof(buf));
  if (ret == -ENOSPC) {
    if (!g_conf->filestore_xattr_use_omap) {
      derr << "limited size xattrs -- enable filestore_xattr_use_omap" << dendl;
      return -ENOTSUP;
    } else {
      derr << "limited size xattrs -- filestore_xattr_use_omap enabled" << dendl;
    }
  }
  do_removexattr(fn, "user.test");
  do_removexattr(fn, "user.test2");
  do_removexattr(fn, "user.test3");
  do_removexattr(fn, "user.test4");
  do_removexattr(fn, "user.test5");

  ::unlink(fn);
  TEMP_FAILURE_RETRY(::close(tmpfd));

  int fd = ::open(basedir.c_str(), O_RDONLY);
  if (fd < 0)
    return -errno;

  int r = _test_fiemap();
  if (r < 0)
    return -r;

  struct statfs st;
  r = ::fstatfs(fd, &st);
  if (r < 0)
    return -errno;
  blk_size = st.f_bsize;

#if defined(__linux__)
  if (st.f_type == BTRFS_SUPER_MAGIC) {
    dout(0) << "mount detected btrfs" << dendl;      
    btrfs = true;

    btrfs_stable_commits = btrfs && m_filestore_btrfs_snap;

    // clone_range?
    if (m_filestore_btrfs_clone_range) {
      btrfs_clone_range = true;
      int r = _do_clone_range(fsid_fd, -1, 0, 1, 0);
      if (r == -EBADF) {
	dout(0) << "mount btrfs CLONE_RANGE ioctl is supported" << dendl;
      } else {
	btrfs_clone_range = false;
	dout(0) << "mount btrfs CLONE_RANGE ioctl is NOT supported: " << cpp_strerror(r) << dendl;
      }
    } else {
      dout(0) << "mount btrfs CLONE_RANGE ioctl is DISABLED via 'filestore btrfs clone range' option" << dendl;
    }

    struct btrfs_ioctl_vol_args vol_args;
    memset(&vol_args, 0, sizeof(vol_args));

    // create test source volume
    vol_args.fd = 0;
    strcpy(vol_args.name, "test_subvol");
    r = ::ioctl(fd, BTRFS_IOC_SUBVOL_CREATE, &vol_args);
    if (r != 0) {
      r = -errno;
      dout(0) << "mount  failed to create simple subvolume " << vol_args.name << ": " << cpp_strerror(r) << dendl;
    }
    int srcfd = ::openat(fd, vol_args.name, O_RDONLY);
    if (srcfd < 0) {
      r = -errno;
      dout(0) << "mount  failed to open " << vol_args.name << ": " << cpp_strerror(r) << dendl;
    }

    // snap_create and snap_destroy?
    vol_args.fd = srcfd;
    strcpy(vol_args.name, "sync_snap_test");
    r = ::ioctl(fd, BTRFS_IOC_SNAP_CREATE, &vol_args);
    int err = errno;
    if (r == 0 || errno == EEXIST) {
      dout(0) << "mount btrfs SNAP_CREATE is supported" << dendl;
      btrfs_snap_create = true;

      r = ::ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &vol_args);
      if (r == 0) {
	dout(0) << "mount btrfs SNAP_DESTROY is supported" << dendl;
	btrfs_snap_destroy = true;
      } else {
	err = -errno;
	dout(0) << "mount btrfs SNAP_DESTROY failed: " << cpp_strerror(err) << dendl;

	if (err == -EPERM && getuid() != 0) {
	  dout(0) << "btrfs SNAP_DESTROY failed with EPERM as non-root; remount with -o user_subvol_rm_allowed" << dendl;
	  cerr << TEXT_YELLOW
	       << "btrfs SNAP_DESTROY failed as non-root; remount with -o user_subvol_rm_allowed"
	       << TEXT_NORMAL
	       << std::endl;
	} else if (err == -EOPNOTSUPP) {
	  derr << "btrfs SNAP_DESTROY ioctl not supported; you need a kernel newer than 2.6.32" << dendl;
	}
      }
    } else {
      dout(0) << "mount btrfs SNAP_CREATE failed: " << cpp_strerror(err) << dendl;
    }

    if (m_filestore_btrfs_snap && !btrfs_snap_destroy) {
      dout(0) << "mount btrfs snaps enabled, but no SNAP_DESTROY ioctl; DISABLING" << dendl;
      btrfs_stable_commits = false;
    }

    // start_sync?
    __u64 transid = 0;
    r = ::ioctl(fd, BTRFS_IOC_START_SYNC, &transid);
    if (r < 0) {
      int err = errno;
      dout(0) << "mount btrfs START_SYNC got " << cpp_strerror(err) << dendl;
    }
    if (r == 0 && transid > 0) {
      dout(0) << "mount btrfs START_SYNC is supported (transid " << transid << ")" << dendl;

      // do we have wait_sync too?
      r = ::ioctl(fd, BTRFS_IOC_WAIT_SYNC, &transid);
      if (r == 0 || errno == ERANGE) {
	dout(0) << "mount btrfs WAIT_SYNC is supported" << dendl;
	btrfs_wait_sync = true;
      } else {
	int err = errno;
	dout(0) << "mount btrfs WAIT_SYNC is NOT supported: " << cpp_strerror(err) << dendl;
      }
    } else {
      int err = errno;
      dout(0) << "mount btrfs START_SYNC is NOT supported: " << cpp_strerror(err) << dendl;
    }

    if (btrfs_wait_sync) {
      // async snap creation?
      struct btrfs_ioctl_vol_args_v2 async_args;
      memset(&async_args, 0, sizeof(async_args));
      async_args.fd = srcfd;
      async_args.flags = BTRFS_SUBVOL_CREATE_ASYNC;
      strcpy(async_args.name, "async_snap_test");

      // remove old one, first
      struct stat st;
      strcpy(vol_args.name, async_args.name);
      if (::fstatat(fd, vol_args.name, &st, 0) == 0) {
	dout(0) << "mount btrfs removing old async_snap_test" << dendl;
	r = ::ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &vol_args);
	if (r != 0) {
	  int err = errno;
	  dout(0) << "mount  failed to remove old async_snap_test: " << cpp_strerror(err) << dendl;
	}
      }

      r = ::ioctl(fd, BTRFS_IOC_SNAP_CREATE_V2, &async_args);
      if (r == 0 || errno == EEXIST) {
	dout(0) << "mount btrfs SNAP_CREATE_V2 is supported" << dendl;
	btrfs_snap_create_v2 = true;
      
	// clean up
	strcpy(vol_args.name, "async_snap_test");
	r = ::ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &vol_args);
	if (r != 0) {
	  int err = errno;
	  dout(0) << "mount btrfs SNAP_DESTROY failed: " << cpp_strerror(err) << dendl;
	}
      } else {
	int err = errno;
	dout(0) << "mount btrfs SNAP_CREATE_V2 is NOT supported: "
		<< cpp_strerror(err) << dendl;
      }
    }

    // clean up test subvol
    if (srcfd >= 0)
      TEMP_FAILURE_RETRY(::close(srcfd));

    strcpy(vol_args.name, "test_subvol");
    r = ::ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &vol_args);
    if (r < 0) {
      r = -errno;
      dout(0) << "mount  failed to remove " << vol_args.name << ": " << cpp_strerror(r) << dendl;
    }

    if (m_filestore_btrfs_snap && !btrfs_snap_create_v2) {
      dout(0) << "mount WARNING: btrfs snaps enabled, but no SNAP_CREATE_V2 ioctl (from kernel 2.6.37+)" << dendl;
      cerr << TEXT_YELLOW
	   << " ** WARNING: 'filestore btrfs snap' is enabled (for safe transactions,\n"	 
	   << "             rollback), but btrfs does not support the SNAP_CREATE_V2 ioctl\n"
	   << "             (added in Linux 2.6.37).  Expect slow btrfs sync/commit\n"
	   << "             performance.\n"
	   << TEXT_NORMAL;
    }

  } else
#endif /* __linux__ */
  {
    dout(0) << "mount did NOT detect btrfs" << dendl;
    btrfs = false;
  }

  bool have_syncfs = false;
#ifdef HAVE_SYS_SYNCFS
  if (syncfs(fd) == 0) {
    dout(0) << "mount syncfs(2) syscall fully supported (by glibc and kernel)" << dendl;
    have_syncfs = true;
  } else {
    dout(0) << "mount syncfs(2) syscall supported by glibc BUT NOT the kernel" << dendl;
  }
#else
  dout(0) << "mount syncfs(2) syscall not support by glibc" << dendl;
#endif
  if (!have_syncfs) {
    if (btrfs) {
      dout(0) << "mount no syncfs(2), but the btrfs SYNC ioctl will suffice" << dendl;
    } else if (m_filestore_fsync_flushes_journal_data) {
      dout(0) << "mount no syncfs(2), but 'filestore fsync flushes journal data = true', so fsync will suffice." << dendl;
    } else {
      dout(0) << "mount no syncfs(2), must use sync(2)." << dendl;
      dout(0) << "mount WARNING: multiple ceph-osd daemons on the same host will be slow" << dendl;
    }
  }

  TEMP_FAILURE_RETRY(::close(fd));
  return 0;
}

int FileStore::_sanity_check_fs()
{
  // sanity check(s)

  if ((int)m_filestore_journal_writeahead +
      (int)m_filestore_journal_parallel +
      (int)m_filestore_journal_trailing > 1) {
    dout(0) << "mount ERROR: more than one of filestore journal {writeahead,parallel,trailing} enabled" << dendl;
    cerr << TEXT_RED 
	 << " ** WARNING: more than one of 'filestore journal {writeahead,parallel,trailing}'\n"
	 << "             is enabled in ceph.conf.  You must choose a single journal mode."
	 << TEXT_NORMAL << std::endl;
    return -EINVAL;
  }

  if (!btrfs) {
    if (!journal || !m_filestore_journal_writeahead) {
      dout(0) << "mount WARNING: no btrfs, and no journal in writeahead mode; data may be lost" << dendl;
      cerr << TEXT_RED 
	   << " ** WARNING: no btrfs AND (no journal OR journal not in writeahead mode)\n"
	   << "             For non-btrfs volumes, a writeahead journal is required to\n"
	   << "             maintain on-disk consistency in the event of a crash.  Your conf\n"
	   << "             should include something like:\n"
	   << "        osd journal = /path/to/journal_device_or_file\n"
	   << "        filestore journal writeahead = true\n"
	   << TEXT_NORMAL;
    }
  }

  if (!journal) {
    dout(0) << "mount WARNING: no journal" << dendl;
    cerr << TEXT_YELLOW
	 << " ** WARNING: No osd journal is configured: write latency may be high.\n"
	 << "             If you will not be using an osd journal, write latency may be\n"
	 << "             relatively high.  It can be reduced somewhat by lowering\n"
	 << "             filestore_max_sync_interval, but lower values mean lower write\n"
	 << "             throughput, especially with spinning disks.\n"
	 << TEXT_NORMAL;
  }

  return 0;
}

int FileStore::update_version_stamp()
{
  return write_version_stamp();
}

int FileStore::version_stamp_is_valid(uint32_t *version)
{
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/store_version", basedir.c_str());
  int fd = ::open(fn, O_RDONLY, 0644);
  if (fd < 0) {
    if (errno == ENOENT)
      return 0;
    else 
      return -errno;
  }
  bufferptr bp(PATH_MAX);
  int ret = safe_read(fd, bp.c_str(), bp.length());
  TEMP_FAILURE_RETRY(::close(fd));
  if (ret < 0)
    return -errno;
  bufferlist bl;
  bl.push_back(bp);
  bufferlist::iterator i = bl.begin();
  ::decode(*version, i);
  if (*version == on_disk_version)
    return 1;
  else
    return 0;
}

int FileStore::write_version_stamp()
{
  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/store_version", basedir.c_str());
  int fd = ::open(fn, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if (fd < 0)
    return -errno;
  bufferlist bl;
  ::encode(on_disk_version, bl);
  
  int ret = safe_write(fd, bl.c_str(), bl.length());
  TEMP_FAILURE_RETRY(::close(fd));
  if (ret < 0)
    return -errno;
  return 0;
}

int FileStore::read_op_seq(uint64_t *seq)
{
  int op_fd = ::open(current_op_seq_fn.c_str(), O_CREAT|O_RDWR, 0644);
  if (op_fd < 0)
    return -errno;
  char s[40];
  memset(s, 0, sizeof(s));
  int ret = safe_read(op_fd, s, sizeof(s) - 1);
  if (ret < 0) {
    derr << "error reading " << current_op_seq_fn << ": " << cpp_strerror(ret) << dendl;
    TEMP_FAILURE_RETRY(::close(op_fd));
    return ret;
  }
  *seq = atoll(s);
  return op_fd;
}

int FileStore::write_op_seq(int fd, uint64_t seq)
{
  char s[30];
  snprintf(s, sizeof(s), "%" PRId64 "\n", seq);
  int ret = TEMP_FAILURE_RETRY(::pwrite(fd, s, strlen(s), 0));
  if (ret < 0)
    return -errno;
  return ret;
}

int FileStore::mount() 
{
  int ret;
  char buf[PATH_MAX];
  uint64_t initial_op_seq;
  set<string> cluster_snaps;

  dout(5) << "basedir " << basedir << " journal " << journalpath << dendl;
  
  // make sure global base dir exists
  if (::access(basedir.c_str(), R_OK | W_OK)) {
    ret = -errno;
    derr << "FileStore::mount: unable to access basedir '" << basedir << "': "
	 << cpp_strerror(ret) << dendl;
    goto done;
  }

  // get fsid
  snprintf(buf, sizeof(buf), "%s/fsid", basedir.c_str());
  fsid_fd = ::open(buf, O_RDWR, 0644);
  if (fsid_fd < 0) {
    ret = -errno;
    derr << "FileStore::mount: error opening '" << buf << "': "
	 << cpp_strerror(ret) << dendl;
    goto done;
  }

  ret = read_fsid(fsid_fd, &fsid);
  if (ret < 0) {
    derr << "FileStore::mount: error reading fsid_fd: " << cpp_strerror(ret)
	 << dendl;
    goto close_fsid_fd;
  }

  if (lock_fsid() < 0) {
    derr << "FileStore::mount: lock_fsid failed" << dendl;
    ret = -EBUSY;
    goto close_fsid_fd;
  }

  dout(10) << "mount fsid is " << fsid << dendl;

  // test for btrfs, xattrs, etc.
  ret = _detect_fs();
  if (ret)
    goto close_fsid_fd;

  uint32_t version_stamp;
  ret = version_stamp_is_valid(&version_stamp);
  if (ret < 0) {
    derr << "FileStore::mount : error in version_stamp_is_valid: "
	 << cpp_strerror(ret) << dendl;
    goto close_fsid_fd;
  } else if (ret == 0) {
    if (do_update) {
      derr << "FileStore::mount : stale version stamp detected: "
	   << version_stamp 
	   << ". Proceeding, do_update "
	   << "is set, performing disk format upgrade."
	   << dendl;
    } else {
      ret = -EINVAL;
      derr << "FileStore::mount : stale version stamp " << version_stamp
	   << ". Please run the FileStore update script before starting the "
	   << "OSD, or set filestore_update_to to " << on_disk_version
	   << dendl;
      goto close_fsid_fd;
    }
  }

  // open some dir handles
  basedir_fd = ::open(basedir.c_str(), O_RDONLY);
  if (basedir_fd < 0) {
    ret = -errno;
    derr << "FileStore::mount: failed to open " << basedir << ": "
	 << cpp_strerror(ret) << dendl;
    basedir_fd = -1;
    goto close_fsid_fd;
  }

  {
    // get snap list
    DIR *dir = ::opendir(basedir.c_str());
    if (!dir) {
      ret = -errno;
      derr << "FileStore::mount: opendir '" << basedir << "' failed: "
	   << cpp_strerror(ret) << dendl;
      goto close_basedir_fd;
    }

    struct dirent *de;
    while (::readdir_r(dir, (struct dirent *)buf, &de) == 0) {
      if (!de)
	break;
      long long unsigned c;
      char clustersnap[PATH_MAX];
      if (sscanf(de->d_name, COMMIT_SNAP_ITEM, &c) == 1)
	snaps.push_back(c);
      else if (sscanf(de->d_name, CLUSTER_SNAP_ITEM, clustersnap) == 1)
	cluster_snaps.insert(clustersnap);
    }
    
    if (::closedir(dir) < 0) {
      ret = -errno;
      derr << "FileStore::closedir(basedir) failed: error " << cpp_strerror(ret)
	   << dendl;
      goto close_basedir_fd;
    }

    dout(0) << "mount found snaps " << snaps << dendl;
    if (cluster_snaps.size())
      dout(0) << "mount found cluster snaps " << cluster_snaps << dendl;
  }

  if (m_osd_rollback_to_cluster_snap.length() &&
      cluster_snaps.count(m_osd_rollback_to_cluster_snap) == 0) {
    derr << "rollback to cluster snapshot '" << m_osd_rollback_to_cluster_snap << "': not found" << dendl;
    ret = -ENOENT;
    goto close_basedir_fd;
  }

  char nosnapfn[200];
  snprintf(nosnapfn, sizeof(nosnapfn), "%s/nosnap", current_fn.c_str());

  if (btrfs_stable_commits) {
    if (snaps.empty()) {
      dout(0) << "mount WARNING: no consistent snaps found, store may be in inconsistent state" << dendl;
    } else if (!btrfs) {
      dout(0) << "mount WARNING: not btrfs, store may be in inconsistent state" << dendl;
    } else {
      char s[PATH_MAX];
      uint64_t curr_seq = 0;

      if (m_osd_rollback_to_cluster_snap.length()) {
	derr << TEXT_RED
	     << " ** NOTE: rolling back to cluster snapshot " << m_osd_rollback_to_cluster_snap << " **"
	     << TEXT_NORMAL
	     << dendl;
	assert(cluster_snaps.count(m_osd_rollback_to_cluster_snap));
	snprintf(s, sizeof(s), "%s/" CLUSTER_SNAP_ITEM, basedir.c_str(),
		 m_osd_rollback_to_cluster_snap.c_str());
      } else {
	{
	  int fd = read_op_seq(&curr_seq);
	  if (fd >= 0) {
	    TEMP_FAILURE_RETRY(::close(fd));
	  }
	}
	if (curr_seq)
	  dout(10) << " current/ seq was " << curr_seq << dendl;
	else
	  dout(10) << " current/ missing entirely (unusual, but okay)" << dendl;

	uint64_t cp = snaps.back();
	dout(10) << " most recent snap from " << snaps << " is " << cp << dendl;

	// if current/ is marked as non-snapshotted, refuse to roll
	// back (without clear direction) to avoid throwing out new
	// data.
	struct stat st;
	if (::stat(nosnapfn, &st) == 0) {
	  if (!m_osd_use_stale_snap) {
	    derr << "ERROR: " << nosnapfn << " exists, not rolling back to avoid losing new data" << dendl;
	    derr << "Force rollback to old snapshotted version with 'osd use stale snap = true'" << dendl;
	    derr << "config option for --osd-use-stale-snap startup argument." << dendl;
	    ret = -ENOTSUP;
	    goto close_basedir_fd;
	  }
	  derr << "WARNING: user forced start with data sequence mismatch: current was " << curr_seq
	       << ", newest snap is " << cp << dendl;
	  cerr << TEXT_YELLOW
	       << " ** WARNING: forcing the use of stale snapshot data **"
	       << TEXT_NORMAL << std::endl;
	}

        dout(10) << "mount rolling back to consistent snap " << cp << dendl;
	snprintf(s, sizeof(s), "%s/" COMMIT_SNAP_ITEM, basedir.c_str(), (long long unsigned)cp);
      }

      btrfs_ioctl_vol_args vol_args;
      memset(&vol_args, 0, sizeof(vol_args));
      vol_args.fd = 0;
      strcpy(vol_args.name, "current");

      // drop current?
      if (curr_seq > 0) {
	ret = ::ioctl(basedir_fd, BTRFS_IOC_SNAP_DESTROY, &vol_args);
	if (ret) {
	  ret = -errno;
	  derr << "FileStore::mount: error removing old current subvol: " << cpp_strerror(ret) << dendl;
	  char s[PATH_MAX];
	  snprintf(s, sizeof(s), "%s/current.remove.me.%d", basedir.c_str(), rand());
	  if (::rename(current_fn.c_str(), s)) {
	    ret = -errno;
	    derr << "FileStore::mount: error renaming old current subvol: "
		 << cpp_strerror(ret) << dendl;
	    goto close_basedir_fd;
	  }
	}
      }

      // roll back
      vol_args.fd = ::open(s, O_RDONLY);
      if (vol_args.fd < 0) {
	ret = -errno;
	derr << "FileStore::mount: error opening '" << s << "': " << cpp_strerror(ret) << dendl;
	goto close_basedir_fd;
      }
      if (::ioctl(basedir_fd, BTRFS_IOC_SNAP_CREATE, &vol_args)) {
	ret = -errno;
	derr << "FileStore::mount: error ioctl(BTRFS_IOC_SNAP_CREATE) failed: " << cpp_strerror(ret) << dendl;
	TEMP_FAILURE_RETRY(::close(vol_args.fd));
	goto close_basedir_fd;
      }
      TEMP_FAILURE_RETRY(::close(vol_args.fd));
    }
  }
  initial_op_seq = 0;

  current_fd = ::open(current_fn.c_str(), O_RDONLY);
  if (current_fd < 0) {
    ret = -errno;
    derr << "FileStore::mount: error opening: " << current_fn << ": " << cpp_strerror(ret) << dendl;
    goto close_basedir_fd;
  }

  assert(current_fd >= 0);

  op_fd = read_op_seq(&initial_op_seq);
  if (op_fd < 0) {
    derr << "FileStore::mount: read_op_seq failed" << dendl;
    goto close_current_fd;
  }

  dout(5) << "mount op_seq is " << initial_op_seq << dendl;
  if (initial_op_seq == 0) {
    derr << "mount initial op seq is 0; something is wrong" << dendl;
    ret = -EINVAL;
    goto close_current_fd;
  }

  if (!btrfs_stable_commits) {
    // mark current/ as non-snapshotted so that we don't rollback away
    // from it.
    int r = ::creat(nosnapfn, 0644);
    if (r < 0) {
      derr << "FileStore::mount: failed to create current/nosnap" << dendl;
      goto close_current_fd;
    }
  } else {
    // clear nosnap marker, if present.
    ::unlink(nosnapfn);
  }

  {
    LevelDBStore *omap_store = new LevelDBStore(omap_dir);
    stringstream err;
    if (omap_store->init(err)) {
      derr << "Error initializing leveldb: " << err.str() << dendl;
      ret = -1;
      goto close_current_fd;
    }
    DBObjectMap *dbomap = new DBObjectMap(omap_store);
    ret = dbomap->init(do_update);
    if (ret < 0) {
      derr << "Error initializing DBObjectMap: " << ret << dendl;
      goto close_current_fd;
    }
    stringstream err2;

    if (g_conf->filestore_debug_omap_check && !dbomap->check(err2)) {
      derr << err2.str() << dendl;;
      ret = -EINVAL;
      goto close_current_fd;
    }
    object_map.reset(dbomap);
  }

  // journal
  open_journal();

  // select journal mode?
  if (journal) {
    if (!m_filestore_journal_writeahead &&
	!m_filestore_journal_parallel &&
	!m_filestore_journal_trailing) {
      if (!btrfs) {
	m_filestore_journal_writeahead = true;
	dout(0) << "mount: enabling WRITEAHEAD journal mode: btrfs not detected" << dendl;
      } else if (!btrfs_stable_commits) {
	m_filestore_journal_writeahead = true;
	dout(0) << "mount: enabling WRITEAHEAD journal mode: 'filestore btrfs snap' mode is not enabled" << dendl;
      } else if (!btrfs_snap_create_v2) {
	m_filestore_journal_writeahead = true;
	dout(0) << "mount: enabling WRITEAHEAD journal mode: btrfs SNAP_CREATE_V2 ioctl not detected (v2.6.37+)" << dendl;
      } else {
	m_filestore_journal_parallel = true;
	dout(0) << "mount: enabling PARALLEL journal mode: btrfs, SNAP_CREATE_V2 detected and 'filestore btrfs snap' mode is enabled" << dendl;
      }
    } else {
      if (m_filestore_journal_writeahead)
	dout(0) << "mount: WRITEAHEAD journal mode explicitly enabled in conf" << dendl;
      if (m_filestore_journal_parallel)
	dout(0) << "mount: PARALLEL journal mode explicitly enabled in conf" << dendl;
      if (m_filestore_journal_trailing)
	dout(0) << "mount: TRAILING journal mode explicitly enabled in conf" << dendl;
    }
    if (m_filestore_journal_writeahead)
      journal->set_wait_on_full(true);
  }

  ret = _sanity_check_fs();
  if (ret) {
    derr << "FileStore::mount: _sanity_check_fs failed with error "
	 << ret << dendl;
    goto close_current_fd;
  }

  // Cleanup possibly invalid collections
  {
    vector<coll_t> collections;
    ret = list_collections(collections);
    if (ret < 0) {
      derr << "Error " << ret << " while listing collections" << dendl;
      goto close_current_fd;
    }
    for (vector<coll_t>::iterator i = collections.begin();
	 i != collections.end();
	 ++i) {
      Index index;
      ret = get_index(*i, &index);
      if (ret < 0) {
	derr << "Unable to mount index " << *i 
	     << " with error: " << ret << dendl;
	goto close_current_fd;
      }
      index->cleanup();
    }
  }

  sync_thread.create();

  ret = journal_replay(initial_op_seq);
  if (ret < 0) {
    derr << "mount failed to open journal " << journalpath << ": " << cpp_strerror(ret) << dendl;
    if (ret == -ENOTTY) {
      derr << "maybe journal is not pointing to a block device and its size "
	   << "wasn't configured?" << dendl;
    }

    // stop sync thread
    lock.Lock();
    stop = true;
    sync_cond.Signal();
    lock.Unlock();
    sync_thread.join();

    goto close_current_fd;
  }

  {
    stringstream err2;
    if (g_conf->filestore_debug_omap_check && !object_map->check(err2)) {
      derr << err2.str() << dendl;;
      ret = -EINVAL;
      goto close_current_fd;
    }
  }

  journal_start();

  op_tp.start();
  flusher_thread.create();
  op_finisher.start();
  ondisk_finisher.start();

  timer.init();

  g_ceph_context->get_perfcounters_collection()->add(logger);

  g_ceph_context->_conf->add_observer(this);

  // all okay.
  return 0;

close_current_fd:
  TEMP_FAILURE_RETRY(::close(current_fd));
  current_fd = -1;
close_basedir_fd:
  TEMP_FAILURE_RETRY(::close(basedir_fd));
  basedir_fd = -1;
close_fsid_fd:
  TEMP_FAILURE_RETRY(::close(fsid_fd));
  fsid_fd = -1;
done:
  return ret;
}

int FileStore::umount() 
{
  dout(5) << "umount " << basedir << dendl;
  
  g_ceph_context->_conf->remove_observer(this);

  start_sync();

  lock.Lock();
  stop = true;
  sync_cond.Signal();
  flusher_cond.Signal();
  lock.Unlock();
  sync_thread.join();
  op_tp.stop();
  flusher_thread.join();

  journal_stop();

  g_ceph_context->get_perfcounters_collection()->remove(logger);

  op_finisher.stop();
  ondisk_finisher.stop();

  if (fsid_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(fsid_fd));
    fsid_fd = -1;
  }
  if (op_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(op_fd));
    op_fd = -1;
  }
  if (current_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(current_fd));
    current_fd = -1;
  }
  if (basedir_fd >= 0) {
    TEMP_FAILURE_RETRY(::close(basedir_fd));
    basedir_fd = -1;
  }
  object_map.reset();

  {
    Mutex::Locker l(sync_entry_timeo_lock);
    timer.shutdown();
  }

  // nothing
  return 0;
}


int FileStore::get_max_object_name_length()
{
  lock.Lock();
  int ret = pathconf(basedir.c_str(), _PC_NAME_MAX);
  if (ret < 0) {
    int err = errno;
    lock.Unlock();
    if (err == 0)
      return -EDOM;
    return -err;
  }
  lock.Unlock();
  return ret;
}



/// -----------------------------

FileStore::Op *FileStore::build_op(list<Transaction*>& tls,
				   Context *onreadable,
				   Context *onreadable_sync,
				   TrackedOpRef osd_op)
{
  uint64_t bytes = 0, ops = 0;
  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       p++) {
    bytes += (*p)->get_num_bytes();
    ops += (*p)->get_num_ops();
  }

  Op *o = new Op;
  o->start = ceph_clock_now(g_ceph_context);
  o->tls.swap(tls);
  o->onreadable = onreadable;
  o->onreadable_sync = onreadable_sync;
  o->ops = ops;
  o->bytes = bytes;
  o->osd_op = osd_op;
  return o;
}



void FileStore::queue_op(OpSequencer *osr, Op *o)
{
  assert(journal_lock.is_locked());
  // initialize next_finish on first op
  if (next_finish == 0)
    next_finish = op_seq;

  // mark apply start _now_, because we need to drain the entire apply
  // queue during commit in order to put the store in a consistent
  // state.
  _op_apply_start(o->op);
  op_tp.lock();

  osr->queue(o);

  logger->inc(l_os_ops);
  logger->inc(l_os_bytes, o->bytes);

  op_tp.unlock();

  dout(5) << "queue_op " << o << " seq " << o->op
	  << " " << *osr
	  << " " << o->bytes << " bytes"
	  << "   (queue has " << op_queue_len << " ops and " << op_queue_bytes << " bytes)"
	  << dendl;
  op_wq.queue(osr);
}

void FileStore::op_queue_reserve_throttle(Op *o)
{
  op_tp.lock();
  _op_queue_reserve_throttle(o, "op_queue_reserve_throttle");
  op_tp.unlock();
}

void FileStore::_op_queue_reserve_throttle(Op *o, const char *caller)
{
  // Do not call while holding the journal lock!
  uint64_t max_ops = m_filestore_queue_max_ops;
  uint64_t max_bytes = m_filestore_queue_max_bytes;

  if (is_committing()) {
    max_ops += m_filestore_queue_committing_max_ops;
    max_bytes += m_filestore_queue_committing_max_bytes;
  }

  logger->set(l_os_oq_max_ops, max_ops);
  logger->set(l_os_oq_max_bytes, max_bytes);

  while ((max_ops && (op_queue_len + 1) > max_ops) ||
	 (max_bytes && op_queue_bytes      // let single large ops through!
	  && (op_queue_bytes + o->bytes) > max_bytes)) {
    dout(2) << caller << " waiting: "
	     << op_queue_len + 1 << " > " << max_ops << " ops || "
	     << op_queue_bytes + o->bytes << " > " << max_bytes << dendl;
    op_tp.wait(op_throttle_cond);
  }

  op_queue_len++;
  op_queue_bytes += o->bytes;

  logger->set(l_os_oq_ops, op_queue_len);
  logger->set(l_os_oq_bytes, op_queue_bytes);
}

void FileStore::_op_queue_release_throttle(Op *o)
{
  // Called with op_tp lock!
  op_queue_len--;
  op_queue_bytes -= o->bytes;
  op_throttle_cond.Signal();

  logger->set(l_os_oq_ops, op_queue_len);
  logger->set(l_os_oq_bytes, op_queue_bytes);
}

void FileStore::_do_op(OpSequencer *osr)
{
  osr->apply_lock.Lock();
  Op *o = osr->peek_queue();

  dout(5) << "_do_op " << o << " seq " << o->op << " " << *osr << "/" << osr->parent << " start" << dendl;
  int r = do_transactions(o->tls, o->op);
  op_apply_finish(o->op);
  dout(10) << "_do_op " << o << " seq " << o->op << " r = " << r
	   << ", finisher " << o->onreadable << " " << o->onreadable_sync << dendl;
  
  /*dout(10) << "op_entry finished " << o->bytes << " bytes, queue now "
	   << op_queue_len << " ops, " << op_queue_bytes << " bytes" << dendl;
  */
}

void FileStore::_finish_op(OpSequencer *osr)
{
  Op *o = osr->dequeue();
  
  dout(10) << "_finish_op " << o << " seq " << o->op << " " << *osr << "/" << osr->parent << dendl;
  osr->apply_lock.Unlock();  // locked in _do_op

  // called with tp lock held
  _op_queue_release_throttle(o);

  utime_t lat = ceph_clock_now(g_ceph_context);
  lat -= o->start;
  logger->finc(l_os_apply_lat, lat);

  if (o->onreadable_sync) {
    o->onreadable_sync->finish(0);
    delete o->onreadable_sync;
  }
  op_finisher.queue(o->onreadable);
  delete o;
}


struct C_JournaledAhead : public Context {
  FileStore *fs;
  FileStore::OpSequencer *osr;
  FileStore::Op *o;
  Context *ondisk;

  C_JournaledAhead(FileStore *f, FileStore::OpSequencer *os, FileStore::Op *o, Context *ondisk):
    fs(f), osr(os), o(o), ondisk(ondisk) { }
  void finish(int r) {
    fs->_journaled_ahead(osr, o, ondisk);
  }
};

int FileStore::queue_transaction(Sequencer *osr, Transaction *t)
{
  list<Transaction*> tls;
  tls.push_back(t);
  return queue_transactions(osr, tls, new C_DeleteTransaction(t));
}

int FileStore::queue_transactions(Sequencer *posr, list<Transaction*> &tls,
				  Context *onreadable, Context *ondisk,
				  Context *onreadable_sync,
				  TrackedOpRef osd_op)
{
  if (g_conf->filestore_blackhole) {
    dout(0) << "queue_transactions filestore_blackhole = TRUE, dropping transaction" << dendl;
    return 0;
  }

  // set up the sequencer
  OpSequencer *osr;
  if (!posr)
    posr = &default_osr;
  if (posr->p) {
    osr = (OpSequencer *)posr->p;
    dout(5) << "queue_transactions existing " << *osr << "/" << osr->parent << dendl; //<< " w/ q " << osr->q << dendl;
  } else {
    osr = new OpSequencer;
    osr->parent = posr;
    posr->p = osr;
    dout(5) << "queue_transactions new " << *osr << "/" << osr->parent << dendl;
  }

  if (journal && journal->is_writeable() && !m_filestore_journal_trailing) {
    Op *o = build_op(tls, onreadable, onreadable_sync, osd_op);
    op_queue_reserve_throttle(o);
    journal->throttle();
    o->op = op_submit_start();

    if (m_filestore_do_dump)
      dump_transactions(o->tls, o->op, osr);

    if (m_filestore_journal_parallel) {
      dout(5) << "queue_transactions (parallel) " << o->op << " " << o->tls << dendl;
      
      _op_journal_transactions(o->tls, o->op, ondisk, osd_op);
      
      // queue inside journal lock, to preserve ordering
      queue_op(osr, o);
    } else if (m_filestore_journal_writeahead) {
      dout(5) << "queue_transactions (writeahead) " << o->op << " " << o->tls << dendl;
      
      osr->queue_journal(o->op);

      _op_journal_transactions(o->tls, o->op,
			       new C_JournaledAhead(this, osr, o, ondisk),
			       osd_op);
    } else {
      assert(0);
    }
    op_submit_finish(o->op);
    return 0;
  }

  uint64_t op = op_submit_start();
  dout(5) << "queue_transactions (trailing journal) " << op << " " << tls << dendl;

  if (m_filestore_do_dump)
    dump_transactions(tls, op, osr);

  _op_apply_start(op);
  int r = do_transactions(tls, op);
    
  if (r >= 0) {
    _op_journal_transactions(tls, op, ondisk, osd_op);
  } else {
    delete ondisk;
  }

  // start on_readable finisher after we queue journal item, as on_readable callback
  // is allowed to delete the Transaction
  if (onreadable_sync) {
    onreadable_sync->finish(r);
    delete onreadable_sync;
  }
  op_finisher.queue(onreadable, r);

  op_submit_finish(op);
  op_apply_finish(op);

  return r;
}

void FileStore::_journaled_ahead(OpSequencer *osr, Op *o, Context *ondisk)
{
  dout(5) << "_journaled_ahead " << o << " seq " << o->op << " " << *osr << " " << o->tls << dendl;

  // this should queue in order because the journal does it's completions in order.
  journal_lock.Lock();
  queue_op(osr, o);
  journal_lock.Unlock();

  osr->dequeue_journal();

  // do ondisk completions async, to prevent any onreadable_sync completions
  // getting blocked behind an ondisk completion.
  if (ondisk) {
    dout(10) << " queueing ondisk " << ondisk << dendl;
    ondisk_finisher.queue(ondisk);
  }
}

int FileStore::do_transactions(list<Transaction*> &tls, uint64_t op_seq)
{
  int r = 0;

  uint64_t bytes = 0, ops = 0;
  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       p++) {
    bytes += (*p)->get_num_bytes();
    ops += (*p)->get_num_ops();
  }

  int trans_num = 0;
  for (list<Transaction*>::iterator p = tls.begin();
       p != tls.end();
       p++, trans_num++) {
    r = _do_transaction(**p, op_seq, trans_num);
    if (r < 0)
      break;
  }
  
  return r;
}

unsigned FileStore::apply_transaction(Transaction &t,
				      Context *ondisk)
{
  list<Transaction*> tls;
  tls.push_back(&t);
  return apply_transactions(tls, ondisk);
}

unsigned FileStore::apply_transactions(list<Transaction*> &tls,
				       Context *ondisk)
{
  // use op pool
  Cond my_cond;
  Mutex my_lock("FileStore::apply_transaction::my_lock");
  int r = 0;
  bool done;
  C_SafeCond *onreadable = new C_SafeCond(&my_lock, &my_cond, &done, &r);
  
  dout(10) << "apply queued" << dendl;
  queue_transactions(NULL, tls, onreadable, ondisk);
  
  my_lock.Lock();
  while (!done)
    my_cond.Wait(my_lock);
  my_lock.Unlock();
  dout(10) << "apply done r = " << r << dendl;
  return r;
}


void FileStore::_set_replay_guard(int fd,
				  const SequencerPosition& spos,
				  const hobject_t *hoid,
				  bool in_progress)
{
  if (btrfs_stable_commits)
    return;

  dout(10) << "_set_replay_guard " << spos << (in_progress ? " START" : "") << dendl;

  _inject_failure();

  // first make sure the previous operation commits
  ::fsync(fd);

  // sync object_map too.  even if this object has a header or keys,
  // it have had them in the past and then removed them, so always
  // sync.
  object_map->sync(hoid, &spos);

  _inject_failure();

  // then record that we did it
  bufferlist v(40);
  ::encode(spos, v);
  ::encode(in_progress, v);
  int r = do_fsetxattr(fd, REPLAY_GUARD_XATTR, v.c_str(), v.length());
  if (r < 0) {
    r = -errno;
    derr << "fsetxattr " << REPLAY_GUARD_XATTR << " got " << cpp_strerror(r) << dendl;
    assert(0 == "fsetxattr failed");
  }

  // and make sure our xattr is durable.
  ::fsync(fd);

  _inject_failure();

  dout(10) << "_set_replay_guard " << spos << " done" << dendl;
}

void FileStore::_close_replay_guard(int fd, const SequencerPosition& spos)
{
  if (btrfs_stable_commits)
    return;

  dout(10) << "_close_replay_guard " << spos << dendl;

  _inject_failure();

  // then record that we are done with this operation
  bufferlist v(40);
  ::encode(spos, v);
  bool in_progress = false;
  ::encode(in_progress, v);
  int r = do_fsetxattr(fd, REPLAY_GUARD_XATTR, v.c_str(), v.length());
  if (r < 0) {
    r = -errno;
    derr << "fsetxattr " << REPLAY_GUARD_XATTR << " got " << cpp_strerror(r) << dendl;
    assert(0 == "fsetxattr failed");
  }

  // and make sure our xattr is durable.
  ::fsync(fd);

  _inject_failure();

  dout(10) << "_close_replay_guard " << spos << " done" << dendl;
}


int FileStore::_check_replay_guard(coll_t cid, hobject_t oid, const SequencerPosition& spos)
{
  if (!replaying || btrfs_stable_commits)
    return 1;

  int fd = lfn_open(cid, oid, 0);
  if (fd < 0) {
    dout(10) << "_check_replay_guard " << cid << " " << oid << " dne" << dendl;
    return 1;  // if file does not exist, there is no guard, and we can replay.
  }
  int ret = _check_replay_guard(fd, spos);
  TEMP_FAILURE_RETRY(::close(fd));
  return ret;
}

int FileStore::_check_replay_guard(coll_t cid, const SequencerPosition& spos)
{
  if (!replaying || btrfs_stable_commits)
    return 1;

  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    dout(10) << "_check_replay_guard " << cid << " dne" << dendl;
    return 1;  // if collection does not exist, there is no guard, and we can replay.
  }
  int ret = _check_replay_guard(fd, spos);
  TEMP_FAILURE_RETRY(::close(fd));
  return ret;
}

int FileStore::_check_replay_guard(int fd, const SequencerPosition& spos)
{
  if (!replaying || btrfs_stable_commits)
    return 1;

  char buf[100];
  int r = do_fgetxattr(fd, REPLAY_GUARD_XATTR, buf, sizeof(buf));
  if (r < 0) {
    dout(20) << "_check_replay_guard no xattr" << dendl;
    return 1;  // no xattr
  }
  bufferlist bl;
  bl.append(buf, r);

  SequencerPosition opos;
  bufferlist::iterator p = bl.begin();
  ::decode(opos, p);
  bool in_progress = false;
  if (!p.end())   // older journals don't have this
    ::decode(in_progress, p);
  if (opos > spos) {
    dout(10) << "_check_replay_guard object has " << opos << " > current pos " << spos
	     << ", now or in future, SKIPPING REPLAY" << dendl;
    return -1;
  } else if (opos == spos) {
    if (in_progress) {
      dout(10) << "_check_replay_guard object has " << opos << " == current pos " << spos
	       << ", in_progress=true, CONDITIONAL REPLAY" << dendl;
      return 0;
    } else {
      dout(10) << "_check_replay_guard object has " << opos << " == current pos " << spos
	       << ", in_progress=false, SKIPPING REPLAY" << dendl;
      return -1;
    }
  } else {
    dout(10) << "_check_replay_guard object has " << opos << " < current pos " << spos
	     << ", in past, will replay" << dendl;
    return 1;
  }
}

unsigned FileStore::_do_transaction(Transaction& t, uint64_t op_seq, int trans_num)
{
  dout(10) << "_do_transaction on " << &t << dendl;

  Transaction::iterator i = t.begin();
  
  SequencerPosition spos(op_seq, trans_num, 0);
  while (i.have_op()) {
    int op = i.get_op();
    int r = 0;

    _inject_failure();

    switch (op) {
    case Transaction::OP_NOP:
      break;
    case Transaction::OP_TOUCH:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _touch(cid, oid);
      }
      break;
      
    case Transaction::OP_WRITE:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	uint64_t off = i.get_length();
	uint64_t len = i.get_length();
	bufferlist bl;
	i.get_bl(bl);
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _write(cid, oid, off, len, bl);
      }
      break;
      
    case Transaction::OP_ZERO:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	uint64_t off = i.get_length();
	uint64_t len = i.get_length();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _zero(cid, oid, off, len);
      }
      break;
      
    case Transaction::OP_TRIMCACHE:
      {
	i.get_cid();
	i.get_oid();
	i.get_length();
	i.get_length();
	// deprecated, no-op
      }
      break;
      
    case Transaction::OP_TRUNCATE:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	uint64_t off = i.get_length();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _truncate(cid, oid, off);
      }
      break;
      
    case Transaction::OP_REMOVE:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _remove(cid, oid, spos);
      }
      break;
      
    case Transaction::OP_SETATTR:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	string name = i.get_attrname();
	bufferlist bl;
	i.get_bl(bl);
	if (_check_replay_guard(cid, oid, spos) > 0) {
	  map<string, bufferptr> to_set;
	  to_set[name] = bufferptr(bl.c_str(), bl.length());
	  r = _setattrs(cid, oid, to_set, spos);
	  if (r == -ENOSPC)
	    dout(0) << " ENOSPC on setxattr on " << cid << "/" << oid
		    << " name " << name << " size " << bl.length() << dendl;
	}
      }
      break;
      
    case Transaction::OP_SETATTRS:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	map<string, bufferptr> aset;
	i.get_attrset(aset);
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _setattrs(cid, oid, aset, spos);
  	if (r == -ENOSPC)
	  dout(0) << " ENOSPC on setxattrs on " << cid << "/" << oid << dendl;
      }
      break;

    case Transaction::OP_RMATTR:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	string name = i.get_attrname();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _rmattr(cid, oid, name.c_str(), spos);
      }
      break;

    case Transaction::OP_RMATTRS:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _rmattrs(cid, oid, spos);
      }
      break;
      
    case Transaction::OP_CLONE:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	hobject_t noid = i.get_oid();
	r = _clone(cid, oid, noid, spos);
      }
      break;

    case Transaction::OP_CLONERANGE:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	hobject_t noid = i.get_oid();
 	uint64_t off = i.get_length();
	uint64_t len = i.get_length();
	r = _clone_range(cid, oid, noid, off, len, off, spos);
      }
      break;

    case Transaction::OP_CLONERANGE2:
      {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	hobject_t noid = i.get_oid();
 	uint64_t srcoff = i.get_length();
	uint64_t len = i.get_length();
 	uint64_t dstoff = i.get_length();
	r = _clone_range(cid, oid, noid, srcoff, len, dstoff, spos);
      }
      break;

    case Transaction::OP_MKCOLL:
      {
	coll_t cid = i.get_cid();
	if (_check_replay_guard(cid, spos) > 0)
	  r = _create_collection(cid);
      }
      break;

    case Transaction::OP_RMCOLL:
      {
	coll_t cid = i.get_cid();
	if (_check_replay_guard(cid, spos) > 0)
	  r = _destroy_collection(cid);
      }
      break;

    case Transaction::OP_COLL_ADD:
      {
	coll_t ncid = i.get_cid();
	coll_t ocid = i.get_cid();
	hobject_t oid = i.get_oid();
	r = _collection_add(ncid, ocid, oid, spos);
      }
      break;

    case Transaction::OP_COLL_REMOVE:
       {
	coll_t cid = i.get_cid();
	hobject_t oid = i.get_oid();
	if (_check_replay_guard(cid, oid, spos) > 0)
	  r = _remove(cid, oid, spos);
       }
      break;

    case Transaction::OP_COLL_MOVE:
      {
	// WARNING: this is deprecated and buggy; only here to replay old journals.
	coll_t ocid = i.get_cid();
	coll_t ncid = i.get_cid();
	hobject_t oid = i.get_oid();
	r = _collection_add(ocid, ncid, oid, spos);
	if (r == 0 &&
	    (_check_replay_guard(ocid, oid, spos) > 0))
	  r = _remove(ocid, oid, spos);
      }
      break;

    case Transaction::OP_COLL_SETATTR:
      {
	coll_t cid = i.get_cid();
	string name = i.get_attrname();
	bufferlist bl;
	i.get_bl(bl);
	if (_check_replay_guard(cid, spos) > 0)
	  r = _collection_setattr(cid, name.c_str(), bl.c_str(), bl.length());
      }
      break;

    case Transaction::OP_COLL_RMATTR:
      {
	coll_t cid = i.get_cid();
	string name = i.get_attrname();
	if (_check_replay_guard(cid, spos) > 0)
	  r = _collection_rmattr(cid, name.c_str());
      }
      break;

    case Transaction::OP_STARTSYNC:
      _start_sync();
      break;

    case Transaction::OP_COLL_RENAME:
      {
	coll_t cid(i.get_cid());
	coll_t ncid(i.get_cid());
	r = _collection_rename(cid, ncid, spos);
      }
      break;

    case Transaction::OP_OMAP_CLEAR:
      {
	coll_t cid(i.get_cid());
	hobject_t oid = i.get_oid();
	r = _omap_clear(cid, oid, spos);
      }
      break;
    case Transaction::OP_OMAP_SETKEYS:
      {
	coll_t cid(i.get_cid());
	hobject_t oid = i.get_oid();
	map<string, bufferlist> aset;
	i.get_attrset(aset);
	r = _omap_setkeys(cid, oid, aset, spos);
      }
      break;
    case Transaction::OP_OMAP_RMKEYS:
      {
	coll_t cid(i.get_cid());
	hobject_t oid = i.get_oid();
	set<string> keys;
	i.get_keyset(keys);
	r = _omap_rmkeys(cid, oid, keys, spos);
      }
      break;
    case Transaction::OP_OMAP_SETHEADER:
      {
	coll_t cid(i.get_cid());
	hobject_t oid = i.get_oid();
	bufferlist bl;
	i.get_bl(bl);
	r = _omap_setheader(cid, oid, bl, spos);
      }
      break;

    default:
      derr << "bad op " << op << dendl;
      assert(0);
    }

    if (r < 0) {
      bool ok = false;

      if (r == -ENOENT && !(op == Transaction::OP_CLONERANGE ||
			    op == Transaction::OP_CLONE ||
			    op == Transaction::OP_CLONERANGE2))
	// -ENOENT is normally okay
	// ...including on a replayed OP_RMCOLL with !stable_commits
	ok = true;
      if (r == -ENODATA)
	ok = true;

      if (replaying && !btrfs_stable_commits) {
	if (r == -EEXIST && op == Transaction::OP_MKCOLL) {
	  dout(10) << "tolerating EEXIST during journal replay on non-btrfs" << dendl;
	  ok = true;
	}
	if (r == -EEXIST && op == Transaction::OP_COLL_ADD) {
	  dout(10) << "tolerating EEXIST during journal replay since btrfs_snap is not enabled" << dendl;
	  ok = true;
	}
	if (r == -EEXIST && op == Transaction::OP_COLL_MOVE) {
	  dout(10) << "tolerating EEXIST during journal replay since btrfs_snap is not enabled" << dendl;
	  ok = true;
	}
	if (r == -ERANGE) {
	  dout(10) << "tolerating ERANGE on replay" << dendl;
	  ok = true;
	}
	if (r == -ENOENT) {
	  dout(10) << "tolerating ENOENT on replay" << dendl;
	  ok = true;
	}
      }

      if (!ok) {
	const char *msg = "unexpected error code";

	if (r == -ENOENT && (op == Transaction::OP_CLONERANGE ||
			     op == Transaction::OP_CLONE ||
			     op == Transaction::OP_CLONERANGE2))
	  msg = "ENOENT on clone suggests osd bug";

	if (r == -ENOSPC)
	  // For now, if we hit _any_ ENOSPC, crash, before we do any damage
	  // by partially applying transactions.
	  msg = "ENOSPC handling not implemented";

	if (r == -ENOTEMPTY) {
	  msg = "ENOTEMPTY suggests garbage data in osd data dir";
	}

	dout(0) << " error " << cpp_strerror(r) << " not handled on operation " << op
		<< " (" << spos << ", or op " << spos.op << ", counting from 0)" << dendl;
	dout(0) << msg << dendl;
	dout(0) << " transaction dump:\n";
	JSONFormatter f(true);
	f.open_object_section("transaction");
	t.dump(&f);
	f.close_section();
	f.flush(*_dout);
	*_dout << dendl;
	assert(0 == "unexpected error");
      }
    }

    spos.op++;
  }

  _inject_failure();

  return 0;  // FIXME count errors
}

  /*********************************************/



// --------------------
// objects

bool FileStore::exists(coll_t cid, const hobject_t& oid)
{
  struct stat st;
  if (stat(cid, oid, &st) == 0)
    return true;
  else 
    return false;
}
  
int FileStore::stat(coll_t cid, const hobject_t& oid, struct stat *st)
{
  int r = lfn_stat(cid, oid, st);
  dout(10) << "stat " << cid << "/" << oid << " = " << r << " (size " << st->st_size << ")" << dendl;
  return r;
}

int FileStore::read(coll_t cid, const hobject_t& oid, 
                    uint64_t offset, size_t len, bufferlist& bl)
{
  int got;

  dout(15) << "read " << cid << "/" << oid << " " << offset << "~" << len << dendl;

  int fd = lfn_open(cid, oid, O_RDONLY);
  if (fd < 0) {
    dout(10) << "FileStore::read(" << cid << "/" << oid << ") open error: " << cpp_strerror(fd) << dendl;
    return fd;
  }

  if (len == 0) {
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    ::fstat(fd, &st);
    len = st.st_size;
  }

  bufferptr bptr(len);  // prealloc space for entire read
  got = safe_pread(fd, bptr.c_str(), len, offset);
  if (got < 0) {
    dout(10) << "FileStore::read(" << cid << "/" << oid << ") pread error: " << cpp_strerror(got) << dendl;
    TEMP_FAILURE_RETRY(::close(fd));
    return got;
  }
  bptr.set_length(got);   // properly size the buffer
  bl.push_back(bptr);   // put it in the target bufferlist
  TEMP_FAILURE_RETRY(::close(fd));

  dout(10) << "FileStore::read " << cid << "/" << oid << " " << offset << "~"
	   << got << "/" << len << dendl;
  return got;
}

int FileStore::fiemap(coll_t cid, const hobject_t& oid,
                    uint64_t offset, size_t len,
                    bufferlist& bl)
{
  if (!ioctl_fiemap || len <= (size_t)m_filestore_fiemap_threshold) {
    map<uint64_t, uint64_t> m;
    m[offset] = len;
    ::encode(m, bl);
    return 0;
  }


  struct fiemap *fiemap = NULL;
  map<uint64_t, uint64_t> exomap;

  dout(15) << "fiemap " << cid << "/" << oid << " " << offset << "~" << len << dendl;

  int r;
  int fd = lfn_open(cid, oid, O_RDONLY);
  if (fd < 0) {
    r = fd;
    dout(10) << "read couldn't open " << cid << "/" << oid << ": " << cpp_strerror(r) << dendl;
  } else {
    uint64_t i;

    r = do_fiemap(fd, offset, len, &fiemap);
    if (r < 0)
      goto done;

    if (fiemap->fm_mapped_extents == 0)
      goto done;

    struct fiemap_extent *extent = &fiemap->fm_extents[0];

    /* start where we were asked to start */
    if (extent->fe_logical < offset) {
      extent->fe_length -= offset - extent->fe_logical;
      extent->fe_logical = offset;
    }

    i = 0;

    while (i < fiemap->fm_mapped_extents) {
      struct fiemap_extent *next = extent + 1;

      dout(10) << "FileStore::fiemap() fm_mapped_extents=" << fiemap->fm_mapped_extents
	       << " fe_logical=" << extent->fe_logical << " fe_length=" << extent->fe_length << dendl;

      /* try to merge extents */
      while ((i < fiemap->fm_mapped_extents - 1) &&
             (extent->fe_logical + extent->fe_length == next->fe_logical)) {
          next->fe_length += extent->fe_length;
          next->fe_logical = extent->fe_logical;
          extent = next;
          next = extent + 1;
          i++;
      }

      if (extent->fe_logical + extent->fe_length > offset + len)
        extent->fe_length = offset + len - extent->fe_logical;
      exomap[extent->fe_logical] = extent->fe_length;
      i++;
      extent++;
    }
  }

done:
  if (fd >= 0)
    TEMP_FAILURE_RETRY(::close(fd));
  if (r >= 0)
    ::encode(exomap, bl);

  dout(10) << "fiemap " << cid << "/" << oid << " " << offset << "~" << len << " = " << r << " num_extents=" << exomap.size() << " " << exomap << dendl;
  free(fiemap);
  return r;
}


int FileStore::_remove(coll_t cid, const hobject_t& oid,
		       const SequencerPosition &spos) 
{
  dout(15) << "remove " << cid << "/" << oid << dendl;
  int r = lfn_unlink(cid, oid, spos);
  dout(10) << "remove " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int FileStore::_truncate(coll_t cid, const hobject_t& oid, uint64_t size)
{
  dout(15) << "truncate " << cid << "/" << oid << " size " << size << dendl;
  int r = lfn_truncate(cid, oid, size);
  dout(10) << "truncate " << cid << "/" << oid << " size " << size << " = " << r << dendl;
  return r;
}


int FileStore::_touch(coll_t cid, const hobject_t& oid)
{
  dout(15) << "touch " << cid << "/" << oid << dendl;

  int flags = O_WRONLY|O_CREAT;
  int fd = lfn_open(cid, oid, flags, 0644);
  int r;
  if (fd >= 0) {
    TEMP_FAILURE_RETRY(::close(fd));
    r = 0;
  } else
    r = fd;
  dout(10) << "touch " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int FileStore::_write(coll_t cid, const hobject_t& oid, 
                     uint64_t offset, size_t len,
                     const bufferlist& bl)
{
  dout(15) << "write " << cid << "/" << oid << " " << offset << "~" << len << dendl;
  int r;

  int64_t actual;

  int flags = O_WRONLY|O_CREAT;
  int fd = lfn_open(cid, oid, flags, 0644);
  if (fd < 0) {
    r = fd;
    dout(0) << "write couldn't open " << cid << "/" << oid << " flags " << flags << ": "
	    << cpp_strerror(r) << dendl;
    goto out;
  }
    
  // seek
  actual = ::lseek64(fd, offset, SEEK_SET);
  if (actual < 0) {
    r = -errno;
    dout(0) << "write lseek64 to " << offset << " failed: " << cpp_strerror(r) << dendl;
    goto out;
  }
  if (actual != (int64_t)offset) {
    dout(0) << "write lseek64 to " << offset << " gave bad offset " << actual << dendl;
    r = -EIO;
    goto out;
  }

  // write
  r = bl.write_fd(fd);
  if (r == 0)
    r = bl.length();

  // flush?
  if ((ssize_t)len < m_filestore_flush_min ||
#ifdef HAVE_SYNC_FILE_RANGE
      !m_filestore_flusher || !queue_flusher(fd, offset, len)
#else
      true
#endif
      ) {
    if (m_filestore_sync_flush)
      ::sync_file_range(fd, offset, len, SYNC_FILE_RANGE_WRITE);
    TEMP_FAILURE_RETRY(::close(fd));
  }

 out:
  dout(10) << "write " << cid << "/" << oid << " " << offset << "~" << len << " = " << r << dendl;
  return r;
}

int FileStore::_zero(coll_t cid, const hobject_t& oid, uint64_t offset, size_t len)
{
  dout(15) << "zero " << cid << "/" << oid << " " << offset << "~" << len << dendl;
  int ret = 0;

#ifdef CEPH_HAVE_FALLOCATE
# if !defined(DARWIN) && !defined(__FreeBSD__)
  // first try to punch a hole.
  int fd = lfn_open(cid, oid, O_RDONLY);
  if (fd < 0) {
    ret = -errno;
    goto out;
  }

  // first try fallocate
  ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE, offset, len);
  if (ret < 0)
    ret = -errno;
  TEMP_FAILURE_RETRY(::close(fd));

  if (ret == 0)
    goto out;  // yay!
  if (ret != -EOPNOTSUPP)
    goto out;  // some other error
# endif
#endif

  // lame, kernel is old and doesn't support it.
  // write zeros.. yuck!
  dout(20) << "zero FALLOC_FL_PUNCH_HOLE not supported, falling back to writing zeros" << dendl;
  {
    bufferptr bp(len);
    bp.zero();
    bufferlist bl;
    bl.push_back(bp);
    ret = _write(cid, oid, offset, len, bl);
  }

 out:
  dout(20) << "zero " << cid << "/" << oid << " " << offset << "~" << len << " = " << ret << dendl;
  return ret;
}

int FileStore::_clone(coll_t cid, const hobject_t& oldoid, const hobject_t& newoid,
		      const SequencerPosition& spos)
{
  dout(15) << "clone " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << dendl;

  if (_check_replay_guard(cid, newoid, spos) < 0)
    return 0;

  int o, n, r;
  {
    Index index;
    IndexedPath from, to;
    o = lfn_open(cid, oldoid, O_RDONLY, 0, &from, &index);
    if (o < 0) {
      r = o;
      goto out2;
    }
    n = lfn_open(cid, newoid, O_CREAT|O_TRUNC|O_WRONLY, 0644, &to, &index);
    if (n < 0) {
      r = n;
      goto out;
    }
    struct stat st;
    ::fstat(o, &st);
    r = _do_clone_range(o, n, 0, st.st_size, 0);
    if (r < 0) {
      r = -errno;
      goto out3;
    }
    dout(20) << "objectmap clone" << dendl;
    r = object_map->clone(oldoid, newoid, &spos);
    if (r < 0 && r != -ENOENT)
      goto out3;
  }

  {
    map<string, bufferptr> aset;
    r = _getattrs(cid, oldoid, aset);
    if (r < 0)
      goto out3;

    r = _setattrs(cid, newoid, aset, spos);
    if (r < 0)
      goto out3;
  }

  // clone is non-idempotent; record our work.
  _set_replay_guard(n, spos, &newoid);

 out3:
  TEMP_FAILURE_RETRY(::close(n));
 out:
  TEMP_FAILURE_RETRY(::close(o));
 out2:
  dout(10) << "clone " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << " = " << r << dendl;
  return r;
}

int FileStore::_do_clone_range(int from, int to, uint64_t srcoff, uint64_t len, uint64_t dstoff)
{
  dout(20) << "_do_clone_range " << srcoff << "~" << len << " to " << dstoff << dendl;
  if (!btrfs_clone_range ||
      srcoff % blk_size != dstoff % blk_size) {
    dout(20) << "_do_clone_range using copy" << dendl;
    return _do_copy_range(from, to, srcoff, len, dstoff);
  }
  int err = 0;
  int r = 0;

  uint64_t srcoffclone = ALIGN_UP(srcoff, blk_size);
  uint64_t dstoffclone = ALIGN_UP(dstoff, blk_size);
  if (srcoffclone >= srcoff + len) {
    dout(20) << "_do_clone_range using copy, extent too short to align srcoff" << dendl;
    return _do_copy_range(from, to, srcoff, len, dstoff);
  }

  uint64_t lenclone = len - (srcoffclone - srcoff);
  if (!ALIGNED(lenclone, blk_size)) {
    struct stat from_stat, to_stat;
    err = ::fstat(from, &from_stat);
    if (err) return -errno;
    err = ::fstat(to , &to_stat);
    if (err) return -errno;
    
    if (srcoff + len != (uint64_t)from_stat.st_size ||
	dstoff + len < (uint64_t)to_stat.st_size) {
      // Not to the end of the file, need to align length as well
      lenclone = ALIGN_DOWN(lenclone, blk_size);
    }
  }
  if (lenclone == 0) {
    // too short
    return _do_copy_range(from, to, srcoff, len, dstoff);
  }
  
  dout(20) << "_do_clone_range cloning " << srcoffclone << "~" << lenclone 
	   << " to " << dstoffclone << " = " << r << dendl;
  btrfs_ioctl_clone_range_args a;
  a.src_fd = from;
  a.src_offset = srcoffclone;
  a.src_length = lenclone;
  a.dest_offset = dstoffclone;
  err = ::ioctl(to, BTRFS_IOC_CLONE_RANGE, &a);
  if (err >= 0) {
    r += err;
  } else if (errno == EINVAL) {
    // Still failed, might be compressed
    dout(20) << "_do_clone_range failed CLONE_RANGE call with -EINVAL, using copy" << dendl;
    return _do_copy_range(from, to, srcoff, len, dstoff);
  } else {
    return -errno;
  }

  // Take care any trimmed from front
  if (srcoffclone != srcoff) {
    err = _do_copy_range(from, to, srcoff, srcoffclone - srcoff, dstoff);
    if (err >= 0) {
      r += err;
    } else {
      return -errno;
    }
  }

  // Copy end
  if (srcoffclone + lenclone != srcoff + len) {
    err = _do_copy_range(from, to, 
			 srcoffclone + lenclone, 
			 (srcoff + len) - (srcoffclone + lenclone), 
			 dstoffclone + lenclone);
    if (err >= 0) {
      r += err;
    } else {
      return -errno;
    }
  }
  dout(20) << "_do_clone_range finished " << srcoff << "~" << len 
	   << " to " << dstoff << " = " << r << dendl;
  return r;
}

int FileStore::_do_copy_range(int from, int to, uint64_t srcoff, uint64_t len, uint64_t dstoff)
{
  dout(20) << "_do_copy_range " << srcoff << "~" << len << " to " << dstoff << dendl;
  int r = 0;
  ::lseek64(from, srcoff, SEEK_SET);
  ::lseek64(to, dstoff, SEEK_SET);
  
  loff_t pos = srcoff;
  loff_t end = srcoff + len;
  int buflen = 4096*32;
  char buf[buflen];
  while (pos < end) {
    int l = MIN(end-pos, buflen);
    r = ::read(from, buf, l);
    dout(25) << "  read from " << pos << "~" << l << " got " << r << dendl;
    if (r < 0) {
      r = -errno;
      derr << "FileStore::_do_copy_range: read error at " << pos << "~" << len
	   << ", " << cpp_strerror(r) << dendl;
      break;
    }
    if (r == 0) {
      // hrm, bad source range, wtf.
      r = -ERANGE;
      derr << "FileStore::_do_copy_range got short read result at " << pos
	      << " of fd " << from << " len " << len << dendl;
      break;
    }
    int op = 0;
    while (op < r) {
      int r2 = safe_write(to, buf+op, r-op);
      dout(25) << " write to " << to << " len " << (r-op)
	       << " got " << r2 << dendl;
      if (r2 < 0) {
	r = r2;
	derr << "FileStore::_do_copy_range: write error at " << pos << "~"
	     << r-op << ", " << cpp_strerror(r) << dendl;

	break;
      }
      op += (r-op);
    }
    if (r < 0)
      break;
    pos += r;
  }
  dout(20) << "_do_copy_range " << srcoff << "~" << len << " to " << dstoff << " = " << r << dendl;
  return r;
}

int FileStore::_clone_range(coll_t cid, const hobject_t& oldoid, const hobject_t& newoid,
			    uint64_t srcoff, uint64_t len, uint64_t dstoff,
			    const SequencerPosition& spos)
{
  dout(15) << "clone_range " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << " " << srcoff << "~" << len << " to " << dstoff << dendl;

  if (_check_replay_guard(cid, newoid, spos) < 0)
    return 0;

  int r;
  int o, n;
  o = lfn_open(cid, oldoid, O_RDONLY);
  if (o < 0) {
    r = o;
    goto out2;
  }
  n = lfn_open(cid, newoid, O_CREAT|O_WRONLY, 0644);
  if (n < 0) {
    r = n;
    goto out;
  }
  r = _do_clone_range(o, n, srcoff, len, dstoff);

  // clone is non-idempotent; record our work.
  _set_replay_guard(n, spos, &newoid);

  TEMP_FAILURE_RETRY(::close(n));
 out:
  TEMP_FAILURE_RETRY(::close(o));
 out2:
  dout(10) << "clone_range " << cid << "/" << oldoid << " -> " << cid << "/" << newoid << " "
	   << srcoff << "~" << len << " to " << dstoff << " = " << r << dendl;
  return r;
}


bool FileStore::queue_flusher(int fd, uint64_t off, uint64_t len)
{
  bool queued;
  lock.Lock();
  if (flusher_queue_len < m_filestore_flusher_max_fds) {
    flusher_queue.push_back(sync_epoch);
    flusher_queue.push_back(fd);
    flusher_queue.push_back(off);
    flusher_queue.push_back(len);
    flusher_queue_len++;
    flusher_cond.Signal();
    dout(10) << "queue_flusher ep " << sync_epoch << " fd " << fd << " " << off << "~" << len
	     << " qlen " << flusher_queue_len
	     << dendl;
    queued = true;
  } else {
    dout(10) << "queue_flusher ep " << sync_epoch << " fd " << fd << " " << off << "~" << len
	     << " qlen " << flusher_queue_len 
	     << " hit flusher_max_fds " << m_filestore_flusher_max_fds
	     << ", skipping async flush" << dendl;
    queued = false;
  }
  lock.Unlock();
  return queued;
}

void FileStore::flusher_entry()
{
  lock.Lock();
  dout(20) << "flusher_entry start" << dendl;
  while (true) {
    if (!flusher_queue.empty()) {
#ifdef HAVE_SYNC_FILE_RANGE
      list<uint64_t> q;
      q.swap(flusher_queue);

      int num = flusher_queue_len;  // see how many we're taking, here

      lock.Unlock();
      while (!q.empty()) {
	uint64_t ep = q.front();
	q.pop_front();
	int fd = q.front();
	q.pop_front();
	uint64_t off = q.front();
	q.pop_front();
	uint64_t len = q.front();
	q.pop_front();
	if (!stop && ep == sync_epoch) {
	  dout(10) << "flusher_entry flushing+closing " << fd << " ep " << ep << dendl;
	  ::sync_file_range(fd, off, len, SYNC_FILE_RANGE_WRITE);
	} else 
	  dout(10) << "flusher_entry JUST closing " << fd << " (stop=" << stop << ", ep=" << ep
		   << ", sync_epoch=" << sync_epoch << ")" << dendl;
	TEMP_FAILURE_RETRY(::close(fd));
      }
      lock.Lock();
      flusher_queue_len -= num;   // they're definitely closed, forget
#endif
    } else {
      if (stop)
	break;
      dout(20) << "flusher_entry sleeping" << dendl;
      flusher_cond.Wait(lock);
      dout(20) << "flusher_entry awoke" << dendl;
    }
  }
  dout(20) << "flusher_entry finish" << dendl;
  lock.Unlock();
}

class SyncEntryTimeout : public Context {
public:
  SyncEntryTimeout(int commit_timeo) 
    : m_commit_timeo(commit_timeo)
  {
  }

  void finish(int r) {
    BackTrace *bt = new BackTrace(1);
    generic_dout(-1) << "FileStore: sync_entry timed out after "
	   << m_commit_timeo << " seconds.\n";
    bt->print(*_dout);
    *_dout << dendl;
    delete bt;
    ceph_abort();
  }
private:
  int m_commit_timeo;
};

void FileStore::sync_entry()
{
  lock.Lock();
  while (!stop) {
    utime_t max_interval;
    max_interval.set_from_double(m_filestore_max_sync_interval);
    utime_t min_interval;
    min_interval.set_from_double(m_filestore_min_sync_interval);

    utime_t startwait = ceph_clock_now(g_ceph_context);
    if (!force_sync) {
      dout(20) << "sync_entry waiting for max_interval " << max_interval << dendl;
      sync_cond.WaitInterval(g_ceph_context, lock, max_interval);
    } else {
      dout(20) << "sync_entry not waiting, force_sync set" << dendl;
    }

    if (force_sync) {
      dout(20) << "sync_entry force_sync set" << dendl;
      force_sync = false;
    } else {
      // wait for at least the min interval
      utime_t woke = ceph_clock_now(g_ceph_context);
      woke -= startwait;
      dout(20) << "sync_entry woke after " << woke << dendl;
      if (woke < min_interval) {
	utime_t t = min_interval;
	t -= woke;
	dout(20) << "sync_entry waiting for another " << t 
		 << " to reach min interval " << min_interval << dendl;
	sync_cond.WaitInterval(g_ceph_context, lock, t);
      }
    }

    list<Context*> fin;
  again:
    fin.swap(sync_waiters);
    lock.Unlock();
    
    if (commit_start()) {
      utime_t start = ceph_clock_now(g_ceph_context);
      uint64_t cp = committing_seq;

      sync_entry_timeo_lock.Lock();
      SyncEntryTimeout *sync_entry_timeo =
	new SyncEntryTimeout(m_filestore_commit_timeout);
      timer.add_event_after(m_filestore_commit_timeout, sync_entry_timeo);
      sync_entry_timeo_lock.Unlock();

      logger->set(l_os_committing, 1);

      // make flusher stop flushing previously queued stuff
      sync_epoch++;

      dout(15) << "sync_entry committing " << cp << " sync_epoch " << sync_epoch << dendl;
      int err = write_op_seq(op_fd, cp);
      if (err < 0) {
	derr << "Error during write_op_seq: " << cpp_strerror(err) << dendl;
	assert(0);
      }
      stringstream errstream;
      if (g_conf->filestore_debug_omap_check && !object_map->check(errstream)) {
	derr << errstream.str() << dendl;
	assert(0);
      }

      if (btrfs_stable_commits) {

	if (btrfs_snap_create_v2) {
	  // be smart!
	  struct btrfs_ioctl_vol_args_v2 async_args;
	  memset(&async_args, 0, sizeof(async_args));
	  async_args.fd = current_fd;
	  async_args.flags = BTRFS_SUBVOL_CREATE_ASYNC;
	  snprintf(async_args.name, sizeof(async_args.name), COMMIT_SNAP_ITEM,
		   (long long unsigned)cp);

	  dout(10) << "taking async snap '" << async_args.name << "'" << dendl;
	  int r = ::ioctl(basedir_fd, BTRFS_IOC_SNAP_CREATE_V2, &async_args);
	  if (r < 0) {
	    int err = errno;
	    derr << "async snap create '" << async_args.name << "' transid " << async_args.transid
		 << " got " << cpp_strerror(err) << dendl;
	    assert(0 == "async snap ioctl error");
	  }
	  dout(20) << "async snap create '" << async_args.name << "' transid " << async_args.transid << dendl;

	  snaps.push_back(cp);

	  commit_started();

	  // wait for commit
	  dout(20) << " waiting for transid " << async_args.transid << " to complete" << dendl;
	  ::ioctl(op_fd, BTRFS_IOC_WAIT_SYNC, &async_args.transid);
	  dout(20) << " done waiting for transid " << async_args.transid << " to complete" << dendl;

	} else {
	  // the synchronous snap create does a sync.
	  struct btrfs_ioctl_vol_args vol_args;
	  memset(&vol_args, 0, sizeof(vol_args));
	  vol_args.fd = current_fd;
	  snprintf(vol_args.name, sizeof(vol_args.name), COMMIT_SNAP_ITEM,
		   (long long unsigned)cp);

	  dout(10) << "taking snap '" << vol_args.name << "'" << dendl;
	  int r = ::ioctl(basedir_fd, BTRFS_IOC_SNAP_CREATE, &vol_args);
	  if (r != 0) {
	    int err = errno;
	    derr << "snap create '" << vol_args.name << "' got error " << err << dendl;
	    assert(r == 0);
	  }
	  dout(20) << "snap create '" << vol_args.name << "' succeeded." << dendl;
	  assert(r == 0);
	  snaps.push_back(cp);
	  
	  commit_started();
	}
      } else
      {
	commit_started();

	if (btrfs) {
	  dout(15) << "sync_entry doing btrfs SYNC" << dendl;
	  // do a full btrfs commit
	  ::ioctl(op_fd, BTRFS_IOC_SYNC);
	} else
        if (m_filestore_fsync_flushes_journal_data) {
	  dout(15) << "sync_entry doing fsync on " << current_op_seq_fn << dendl;
	  // make the file system's journal commit.
	  //  this works with ext3, but NOT ext4
	  ::fsync(op_fd);  
	} else {
	  dout(15) << "sync_entry doing a full sync (syncfs(2) if possible)" << dendl;
	  sync_filesystem(basedir_fd);
	}
      }
      
      utime_t done = ceph_clock_now(g_ceph_context);
      utime_t lat = done - start;
      utime_t dur = done - startwait;
      dout(10) << "sync_entry commit took " << lat << ", interval was " << dur << dendl;

      logger->inc(l_os_commit);
      logger->finc(l_os_commit_lat, lat);
      logger->finc(l_os_commit_len, dur);

      commit_finish();

      logger->set(l_os_committing, 0);

      // remove old snaps?
      if (btrfs_stable_commits) {
	while (snaps.size() > 2) {
	  btrfs_ioctl_vol_args vol_args;
	  memset(&vol_args, 0, sizeof(vol_args));
	  vol_args.fd = 0;
	  snprintf(vol_args.name, sizeof(vol_args.name), COMMIT_SNAP_ITEM,
		   (long long unsigned)snaps.front());

	  snaps.pop_front();
	  dout(10) << "removing snap '" << vol_args.name << "'" << dendl;
	  int r = ::ioctl(basedir_fd, BTRFS_IOC_SNAP_DESTROY, &vol_args);
	  if (r) {
	    int err = errno;
	    derr << "unable to destroy snap '" << vol_args.name << "' got " << cpp_strerror(err) << dendl;
	  }
	}
      }

      dout(15) << "sync_entry committed to op_seq " << cp << dendl;

      sync_entry_timeo_lock.Lock();
      timer.cancel_event(sync_entry_timeo);
      sync_entry_timeo_lock.Unlock();
    }
    
    lock.Lock();
    finish_contexts(g_ceph_context, fin, 0);
    fin.clear();
    if (!sync_waiters.empty()) {
      dout(10) << "sync_entry more waiters, committing again" << dendl;
      goto again;
    }
    if (journal && journal->should_commit_now()) {
      dout(10) << "sync_entry journal says we should commit again (probably is/was full)" << dendl;
      goto again;
    }
  }
  lock.Unlock();
}

void FileStore::_start_sync()
{
  if (!journal) {  // don't do a big sync if the journal is on
    dout(10) << "start_sync" << dendl;
    sync_cond.Signal();
  } else {
    dout(10) << "start_sync - NOOP (journal is on)" << dendl;
  }
}

void FileStore::start_sync()
{
  Mutex::Locker l(lock);
  force_sync = true;
  sync_cond.Signal();
}

void FileStore::start_sync(Context *onsafe)
{
  Mutex::Locker l(lock);
  sync_waiters.push_back(onsafe);
  sync_cond.Signal();
  dout(10) << "start_sync" << dendl;
}

void FileStore::trigger_commit(uint64_t seq)
{
  /*
   * crib the lock -> journal_lock.  we need to start the sync under lock,
   * but once we release lock it will block because journal_lock is held.
   * _trigger_commit() expects journal_lock to be held by the caller.
   */
  lock.Lock();
  dout(10) << "trigger_commit seq" << dendl;
  force_sync = true;
  sync_cond.Signal();
  journal_lock.Lock();
  lock.Unlock();
  _trigger_commit(seq);
  journal_lock.Unlock();
}

void FileStore::sync()
{
  Mutex l("FileStore::sync");
  Cond c;
  bool done;
  C_SafeCond *fin = new C_SafeCond(&l, &c, &done);

  start_sync(fin);

  l.Lock();
  while (!done) {
    dout(10) << "sync waiting" << dendl;
    c.Wait(l);
  }
  l.Unlock();
  dout(10) << "sync done" << dendl;
}

void FileStore::_flush_op_queue()
{
  dout(10) << "_flush_op_queue draining op tp" << dendl;
  op_wq.drain();
  dout(10) << "_flush_op_queue waiting for apply finisher" << dendl;
  op_finisher.wait_for_empty();
}

/*
 * flush - make every queued write readable
 */
void FileStore::flush()
{
  dout(10) << "flush" << dendl;

  if (g_conf->filestore_blackhole) {
    // wait forever
    Mutex lock("FileStore::flush::lock");
    Cond cond;
    lock.Lock();
    while (true)
      cond.Wait(lock);
    assert(0);
  }
 
  if (m_filestore_journal_writeahead) {
    if (journal)
      journal->flush();
    dout(10) << "flush draining ondisk finisher" << dendl;
    ondisk_finisher.wait_for_empty();
  }

  _flush_op_queue();
  dout(10) << "flush complete" << dendl;
}

/*
 * sync_and_flush - make every queued write readable AND committed to disk
 */
void FileStore::sync_and_flush()
{
  dout(10) << "sync_and_flush" << dendl;

  if (m_filestore_journal_writeahead) {
    if (journal)
      journal->flush();
    _flush_op_queue();
  } else if (m_filestore_journal_parallel) {
    _flush_op_queue();
    sync();
  } else {
    _flush_op_queue();
    sync();
  }
  dout(10) << "sync_and_flush done" << dendl;
}

int FileStore::snapshot(const string& name)
{
  dout(10) << "snapshot " << name << dendl;
  sync_and_flush();

  if (!btrfs) {
    dout(0) << "snapshot " << name << " failed, no btrfs" << dendl;
    return -EOPNOTSUPP;
  }

  btrfs_ioctl_vol_args vol_args;
  vol_args.fd = current_fd;
  snprintf(vol_args.name, sizeof(vol_args.name), CLUSTER_SNAP_ITEM, name.c_str());

  int r = ::ioctl(basedir_fd, BTRFS_IOC_SNAP_CREATE, &vol_args);
  if (r) {
    r = -errno;
    derr << "snapshot " << name << " failed: " << cpp_strerror(r) << dendl;
  }

  return r;
}

// -------------------------------
// attributes

// low-level attr helpers
int FileStore::_getattr(coll_t cid, const hobject_t& oid, const char *name, bufferptr& bp)
{
  char val[100];
  int l = lfn_getxattr(cid, oid, name, val, sizeof(val));
  if (l >= 0) {
    bp = buffer::create(l);
    memcpy(bp.c_str(), val, l);
  } else if (l == -ERANGE) {
    l = lfn_getxattr(cid, oid, name, 0, 0);
    if (l > 0) {
      bp = buffer::create(l);
      l = lfn_getxattr(cid, oid, name, bp.c_str(), l);
    }
  }
  return l;
}

int FileStore::_getattr(const char *fn, const char *name, bufferptr& bp)
{
  char val[100];
  int l = do_getxattr(fn, name, val, sizeof(val));
  if (l >= 0) {
    bp = buffer::create(l);
    memcpy(bp.c_str(), val, l);
  } else if (l == -ERANGE) {
    l = do_getxattr(fn, name, 0, 0);
    if (l > 0) {
      bp = buffer::create(l);
      l = do_getxattr(fn, name, bp.c_str(), l);
    }
  }
  return l;
}

// note that this is a clone of the method below.. any change here should be reflected there 
int FileStore::_getattrs(coll_t cid, const hobject_t& oid, map<string,bufferptr>& aset, bool user_only) 
{
  // get attr list
  char names1[100];
  int len = lfn_listxattr(cid, oid, names1, sizeof(names1)-1);
  char *names2 = 0;
  char *name = 0;
  if (len == -ERANGE) {
    len = lfn_listxattr(cid, oid, 0, 0);
    if (len < 0)
      return len;
    dout(10) << " -ERANGE, len is " << len << dendl;
    names2 = new char[len+1];
    len = lfn_listxattr(cid, oid, names2, len);
    dout(10) << " -ERANGE, got " << len << dendl;
    if (len < 0)
      return len;
    name = names2;
  } else if (len < 0)
    return len;
  else
    name = names1;
  name[len] = 0;

  char *end = name + len;
  while (name < end) {
    char *attrname = name;
    if (parse_attrname(&name)) {
      char *set_name = name;
      bool can_get = true;
      if (user_only) {
          if (*set_name =='_')
            set_name++;
          else
            can_get = false;
      }
      if (*set_name && can_get) {
        dout(20) << "getattrs " << cid << "/" << oid << " getting '" << name << "'" << dendl;
      
        int r = _getattr(cid, oid, attrname, aset[set_name]);
        if (r < 0) return r;
      }
    }
    name += strlen(name) + 1;
  }

  delete[] names2;
  return 0;
}


// note that this is a clone of the method above.. any change here should be reflected
// there
int FileStore::_getattrs(const char *fn, map<string,bufferptr>& aset, bool user_only) 
{
  // get attr list
  char names1[100];
  int len = do_listxattr(fn, names1, sizeof(names1)-1);
  char *names2 = 0;
  char *name = 0;
  if (len == -ERANGE) {
    len = do_listxattr(fn, 0, 0);
    if (len < 0)
      return len;
    dout(10) << " -ERANGE, len is " << len << dendl;
    names2 = new char[len+1];
    len = do_listxattr(fn, names2, len);
    dout(10) << " -ERANGE, got " << len << dendl;
    if (len < 0)
      return len;
    name = names2;
  } else if (len < 0)
    return len;
  else
    name = names1;
  name[len] = 0;

  char *end = name + len;
  while (name < end) {
    char *attrname = name;
    if (parse_attrname(&name)) {
      char *set_name = name;
      bool can_get = true;
      if (user_only) {
          if (*set_name =='_')
            set_name++;
          else
            can_get = false;
      }
      if (*set_name && can_get) {
        dout(20) << "getattrs " << fn << " getting '" << name << "'" << dendl;
        //dout(0) << "getattrs " << fn << " set_name '" << set_name << "' user_only=" << user_only << dendl;
      
        int r = _getattr(fn, attrname, aset[set_name]);
        if (r < 0) return r;
      }
    }
    name += strlen(name) + 1;
  }

  delete[] names2;
  return 0;
}

// objects

int FileStore::getattr(coll_t cid, const hobject_t& oid, const char *name, bufferptr &bp)
{
  dout(15) << "getattr " << cid << "/" << oid << " '" << name << "'" << dendl;
  char n[ATTR_MAX_NAME_LEN];
  get_attrname(name, n, ATTR_MAX_NAME_LEN);
  int r = _getattr(cid, oid, n, bp);
  if (r == -ENODATA && g_conf->filestore_xattr_use_omap) {
    map<string, bufferlist> got;
    set<string> to_get;
    to_get.insert(string(name));
    Index index;
    r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      return r;
    }
    r = object_map->get_xattrs(oid, to_get, &got);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " get_xattrs err r =" << r << dendl;
      return r;
    }
    if (!got.size()) {
      dout(10) << __func__ << " got.size() is 0" << dendl;
      return -ENODATA;
    }
    bp = bufferptr(got.begin()->second.c_str(),
		   got.begin()->second.length());
    r = 0;
  }
  dout(10) << "getattr " << cid << "/" << oid << " '" << name << "' = " << r << dendl;
  return r;
}

int FileStore::getattrs(coll_t cid, const hobject_t& oid, map<string,bufferptr>& aset, bool user_only) 
{
  dout(15) << "getattrs " << cid << "/" << oid << dendl;
  int r = _getattrs(cid, oid, aset, user_only);
  if (g_conf->filestore_xattr_use_omap) {
    set<string> omap_attrs;
    map<string, bufferlist> omap_aset;
    Index index;
    int r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      return r;
    }
    r = object_map->get_all_xattrs(oid, &omap_attrs);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not get omap_attrs r = " << r << dendl;
      return r;
    }
    r = object_map->get_xattrs(oid, omap_attrs, &omap_aset);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not get omap_attrs r = " << r << dendl;
      return r;
    }
    assert(omap_attrs.size() == omap_aset.size());
    for (map<string, bufferlist>::iterator i = omap_aset.begin();
	 i != omap_aset.end();
	 ++i) {
      string key;
      if (user_only) {
	if (i->first[0] != '_')
	  continue;
	if (i->first == "_")
	  continue;
	key = i->first.substr(1, i->first.size());
      } else {
	key = i->first;
      }
      aset.insert(make_pair(key,
			    bufferptr(i->second.c_str(), i->second.length())));
    }
  }
  dout(10) << "getattrs " << cid << "/" << oid << " = " << r << dendl;
  return r;
}

int FileStore::_setattrs(coll_t cid, const hobject_t& oid, map<string,bufferptr>& aset,
			 const SequencerPosition &spos)
{
  map<string, bufferlist> omap_set;
  set<string> omap_remove;
  map<string, bufferptr> inline_set;
  if (g_conf->filestore_xattr_use_omap)
    _getattrs(cid, oid, inline_set);
  dout(15) << "setattrs " << cid << "/" << oid << dendl;
  int r = 0;
  for (map<string,bufferptr>::iterator p = aset.begin();
       p != aset.end();
       ++p) {
    char n[ATTR_MAX_NAME_LEN];
    get_attrname(p->first.c_str(), n, ATTR_MAX_NAME_LEN);
    if (g_conf->filestore_xattr_use_omap) {
      if (p->second.length() > g_conf->filestore_max_inline_xattr_size) {
	if (inline_set.count(p->first)) {
	  inline_set.erase(p->first);
	  r = lfn_removexattr(cid, oid, n);
	  if (r < 0)
	    return r;
	}
	omap_set[p->first].push_back(p->second);
	continue;
      }

      if (!inline_set.count(p->first) &&
	  inline_set.size() >= g_conf->filestore_max_inline_xattrs) {
	if (inline_set.count(p->first)) {
	  inline_set.erase(p->first);
	  r = lfn_removexattr(cid, oid, n);
	  if (r < 0)
	    return r;
	}
	omap_set[p->first].push_back(p->second);
	continue;
      }
      omap_remove.insert(p->first);
      inline_set.insert(*p);
    }

    const char *val;
    if (p->second.length())
      val = p->second.c_str();
    else
      val = "";
    // ??? Why do we skip setting all the other attrs if one fails?
    r = lfn_setxattr(cid, oid, n, val, p->second.length());
    if (r < 0) {
      derr << "FileStore::_setattrs: do_setxattr returned " << r << dendl;
      break;
    }
  }

  if (g_conf->filestore_xattr_use_omap) {
    Index index;
    int r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      return r;
    }
    r = object_map->remove_xattrs(oid, omap_remove, &spos);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not remove_xattrs r = " << r << dendl;
      return r;
    }
    r = object_map->set_xattrs(oid, omap_set, &spos);
    if (r < 0) {
      dout(10) << __func__ << " could not set_xattrs r = " << r << dendl;
      return r;
    }
  }
  dout(10) << "setattrs " << cid << "/" << oid << " = " << r << dendl;
  return r;
}


int FileStore::_rmattr(coll_t cid, const hobject_t& oid, const char *name,
		       const SequencerPosition &spos)
{
  dout(15) << "rmattr " << cid << "/" << oid << " '" << name << "'" << dendl;
  char n[ATTR_MAX_NAME_LEN];
  get_attrname(name, n, ATTR_MAX_NAME_LEN);
  int r = lfn_removexattr(cid, oid, n);
  if (r == -ENODATA && g_conf->filestore_xattr_use_omap) {
    Index index;
    r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      return r;
    }
    set<string> to_remove;
    to_remove.insert(string(name));
    r = object_map->remove_xattrs(oid, to_remove, &spos);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not remove_xattrs index r = " << r << dendl;
      return r;
    }
  }
  dout(10) << "rmattr " << cid << "/" << oid << " '" << name << "' = " << r << dendl;
  return r;
}

int FileStore::_rmattrs(coll_t cid, const hobject_t& oid,
			const SequencerPosition &spos)
{
  dout(15) << "rmattrs " << cid << "/" << oid << dendl;

  map<string,bufferptr> aset;
  int r = _getattrs(cid, oid, aset);
  if (r >= 0) {
    for (map<string,bufferptr>::iterator p = aset.begin(); p != aset.end(); p++) {
      char n[ATTR_MAX_NAME_LEN];
      get_attrname(p->first.c_str(), n, ATTR_MAX_NAME_LEN);
      r = lfn_removexattr(cid, oid, n);
      if (r < 0)
	break;
    }
  }
  if (g_conf->filestore_xattr_use_omap) {
    set<string> omap_attrs;
    Index index;
    r = get_index(cid, &index);
    if (r < 0) {
      dout(10) << __func__ << " could not get index r = " << r << dendl;
      return r;
    }
    r = object_map->get_all_xattrs(oid, &omap_attrs);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not get omap_attrs r = " << r << dendl;
      return r;
    }
    r = object_map->remove_xattrs(oid, omap_attrs, &spos);
    if (r < 0 && r != -ENOENT) {
      dout(10) << __func__ << " could not remove omap_attrs r = " << r << dendl;
      return r;
    }
  }
  dout(10) << "rmattrs " << cid << "/" << oid << " = " << r << dendl;
  return r;
}



// collections

int FileStore::collection_getattr(coll_t c, const char *name,
				  void *value, size_t size) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_getattr " << fn << " '" << name << "' len " << size << dendl;
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  int r = do_getxattr(fn, n, value, size);   
  dout(10) << "collection_getattr " << fn << " '" << name << "' len " << size << " = " << r << dendl;
  return r;
}

int FileStore::collection_getattr(coll_t c, const char *name, bufferlist& bl)
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_getattr " << fn << " '" << name << "'" << dendl;
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  
  buffer::ptr bp;
  int r = _getattr(fn, n, bp);
  bl.push_back(bp);
  dout(10) << "collection_getattr " << fn << " '" << name << "' = " << r << dendl;
  return r;
}

int FileStore::collection_getattrs(coll_t cid, map<string,bufferptr>& aset) 
{
  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  dout(10) << "collection_getattrs " << fn << dendl;
  int r = _getattrs(fn, aset);
  dout(10) << "collection_getattrs " << fn << " = " << r << dendl;
  return r;
}


int FileStore::_collection_setattr(coll_t c, const char *name,
				  const void *value, size_t size) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(10) << "collection_setattr " << fn << " '" << name << "' len " << size << dendl;
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  int r = do_setxattr(fn, n, value, size);
  dout(10) << "collection_setattr " << fn << " '" << name << "' len " << size << " = " << r << dendl;
  return r;
}

int FileStore::_collection_rmattr(coll_t c, const char *name) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_rmattr " << fn << dendl;
  char n[PATH_MAX];
  get_attrname(name, n, PATH_MAX);
  int r = do_removexattr(fn, n);
  dout(10) << "collection_rmattr " << fn << " = " << r << dendl;
  return r;
}


int FileStore::_collection_setattrs(coll_t cid, map<string,bufferptr>& aset) 
{
  char fn[PATH_MAX];
  get_cdir(cid, fn, sizeof(fn));
  dout(15) << "collection_setattrs " << fn << dendl;
  int r = 0;
  for (map<string,bufferptr>::iterator p = aset.begin();
       p != aset.end();
       ++p) {
    char n[PATH_MAX];
    get_attrname(p->first.c_str(), n, PATH_MAX);
    r = do_setxattr(fn, n, p->second.c_str(), p->second.length());
    if (r < 0) break;
  }
  dout(10) << "collection_setattrs " << fn << " = " << r << dendl;
  return r;
}

int FileStore::_collection_rename(const coll_t &cid, const coll_t &ncid,
				  const SequencerPosition& spos)
{
  char new_coll[PATH_MAX], old_coll[PATH_MAX];
  get_cdir(cid, old_coll, sizeof(old_coll));
  get_cdir(ncid, new_coll, sizeof(new_coll));

  if (_check_replay_guard(ncid, spos) < 0)
    return 0;

  int ret = 0;
  if (::rename(old_coll, new_coll)) {
    if (replaying && !btrfs_stable_commits &&
	(errno == EEXIST || errno == ENOTEMPTY))
      ret = 0;   // crashed between rename and set_replay_guard
    else
      ret = -errno;
  }

  if (ret >= 0) {
    int fd = ::open(new_coll, O_RDONLY);
    assert(fd >= 0);
    _set_replay_guard(fd, spos);
    TEMP_FAILURE_RETRY(::close(fd));
  }

  dout(10) << "collection_rename '" << cid << "' to '" << ncid << "'"
	   << ": ret = " << ret << dendl;
  return ret;
}

// --------------------------
// collections

int FileStore::collection_version_current(coll_t c, uint32_t *version)
{
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return r;
  *version = index->collection_version();
  if (*version == on_disk_version)
    return 1;
  else 
    return 0;
}

int FileStore::list_collections(vector<coll_t>& ls) 
{
  dout(10) << "list_collections" << dendl;

  char fn[PATH_MAX];
  snprintf(fn, sizeof(fn), "%s/current", basedir.c_str());

  int r = 0;
  DIR *dir = ::opendir(fn);
  if (!dir) {
    r = -errno;
    derr << "tried opening directory " << fn << ": " << cpp_strerror(-r) << dendl;
    return r;
  }

  struct dirent sde, *de;
  while ((r = ::readdir_r(dir, &sde, &de)) == 0) {
    if (!de)
      break;
    if (de->d_type == DT_UNKNOWN) {
      // d_type not supported (non-ext[234], btrfs), must stat
      struct stat sb;
      char filename[PATH_MAX];
      snprintf(filename, sizeof(filename), "%s/%s", fn, de->d_name);

      r = ::stat(filename, &sb);
      if (r < 0) {
	r = -errno;
	derr << "stat on " << filename << ": " << cpp_strerror(-r) << dendl;
	break;
      }
      if (!S_ISDIR(sb.st_mode)) {
	continue;
      }
    } else if (de->d_type != DT_DIR) {
      continue;
    }
    if (strcmp(de->d_name, "omap") == 0) {
      continue;
    }
    if (de->d_name[0] == '.' &&
	(de->d_name[1] == '\0' ||
	 (de->d_name[1] == '.' &&
	  de->d_name[2] == '\0')))
      continue;
    ls.push_back(coll_t(de->d_name));
  }

  if (r > 0) {
    derr << "trying readdir_r " << fn << ": " << cpp_strerror(r) << dendl;
    r = -r;
  }

  ::closedir(dir);
  return r;
}

int FileStore::collection_stat(coll_t c, struct stat *st) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "collection_stat " << fn << dendl;
  int r = ::stat(fn, st);
  if (r < 0) r = -errno;
  dout(10) << "collection_stat " << fn << " = " << r << dendl;
  return r;
}

bool FileStore::collection_exists(coll_t c) 
{
  struct stat st;
  return collection_stat(c, &st) == 0;
}

bool FileStore::collection_empty(coll_t c) 
{  
  dout(15) << "collection_empty " << c << dendl;
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return false;
  vector<hobject_t> ls;
  collection_list_handle_t handle;
  r = index->collection_list_partial(hobject_t(), 1, 1, 0, &ls, NULL);
  if (r < 0)
    return false;
  return ls.size() > 0;
}

int FileStore::collection_list_partial(coll_t c, hobject_t start,
				       int min, int max, snapid_t seq,
				       vector<hobject_t> *ls, hobject_t *next)
{
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return r;
  r = index->collection_list_partial(start,
				     min, max, seq,
				     ls, next);
  if (r < 0)
    return r;
  return 0;
}

int FileStore::collection_list(coll_t c, vector<hobject_t>& ls) 
{  
  Index index;
  int r = get_index(c, &index);
  if (r < 0)
    return r;
  return index->collection_list(&ls);
}

int FileStore::omap_get(coll_t c, const hobject_t &hoid,
			bufferlist *header,
			map<string, bufferlist> *out)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(c, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->get(hoid, header, out);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}

int FileStore::omap_get_header(coll_t c, const hobject_t &hoid,
			       bufferlist *bl)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(c, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->get_header(hoid, bl);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}

int FileStore::omap_get_keys(coll_t c, const hobject_t &hoid, set<string> *keys)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(c, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->get_keys(hoid, keys);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}

int FileStore::omap_get_values(coll_t c, const hobject_t &hoid,
			       const set<string> &keys,
			       map<string, bufferlist> *out)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(c, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->get_values(hoid, keys, out);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}

int FileStore::omap_check_keys(coll_t c, const hobject_t &hoid,
			       const set<string> &keys,
			       set<string> *out)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(c, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->check_keys(hoid, keys, out);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}

ObjectMap::ObjectMapIterator FileStore::get_omap_iterator(coll_t c,
							  const hobject_t &hoid)
{
  dout(15) << __func__ << " " << c << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(c, hoid, &path);
  if (r < 0)
    return ObjectMap::ObjectMapIterator();
  return object_map->get_iterator(hoid);
}

int FileStore::_create_collection(coll_t c) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "create_collection " << fn << dendl;
  int r = ::mkdir(fn, 0755);
  if (r < 0) r = -errno;
  dout(10) << "create_collection " << fn << " = " << r << dendl;

  if (r < 0) return r;
  return init_index(c);
}

int FileStore::_destroy_collection(coll_t c) 
{
  char fn[PATH_MAX];
  get_cdir(c, fn, sizeof(fn));
  dout(15) << "_destroy_collection " << fn << dendl;
  int r = ::rmdir(fn);
  if (r < 0) r = -errno;
  dout(10) << "_destroy_collection " << fn << " = " << r << dendl;
  return r;
}


int FileStore::_collection_add(coll_t c, coll_t oldcid, const hobject_t& o,
			       const SequencerPosition& spos) 
{
  dout(15) << "collection_add " << c << "/" << o << " from " << oldcid << "/" << o << dendl;
  
  int dstcmp = _check_replay_guard(c, o, spos);
  if (dstcmp < 0)
    return 0;

  // check the src name too; it might have a newer guard, and we don't
  // want to clobber it
  int srccmp = _check_replay_guard(oldcid, o, spos);
  if (srccmp < 0)
    return 0;

  // open guard on object so we don't any previous operations on the
  // new name that will modify the source inode.
  int fd = lfn_open(oldcid, o, 0);
  if (fd < 0) {
    // the source collection/object does not exist. If we are replaying, we
    // should be safe, so just return 0 and move on.
    assert(replaying);
    dout(10) << "collection_add " << c << "/" << o << " from "
        << oldcid << "/" << o << " (dne, continue replay) " << dendl;
    return 0;
  }
  assert(fd >= 0);
  if (dstcmp > 0) {      // if dstcmp == 0 the guard already says "in-progress"
    _set_replay_guard(fd, spos, &o, true);
  }

  int r = lfn_link(oldcid, c, o);
  if (replaying && !btrfs_stable_commits &&
      r == -EEXIST)    // crashed between link() and set_replay_guard()
    r = 0;

  _inject_failure();

  // close guard on object so we don't do this again
  if (r == 0) {
    _close_replay_guard(fd, spos);
  }
  TEMP_FAILURE_RETRY(::close(fd));

  dout(10) << "collection_add " << c << "/" << o << " from " << oldcid << "/" << o << " = " << r << dendl;
  return r;
}

void FileStore::_inject_failure()
{
  if (m_filestore_kill_at.read()) {
    int final = m_filestore_kill_at.dec();
    dout(5) << "_inject_failure " << (final+1) << " -> " << final << dendl;
    if (final == 0) {
      derr << "_inject_failure KILLING" << dendl;
      g_ceph_context->_log->flush();
      _exit(1);
    }
  }
}

int FileStore::_omap_clear(coll_t cid, const hobject_t &hoid,
			   const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(cid, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->clear(hoid, &spos);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}
int FileStore::_omap_setkeys(coll_t cid, const hobject_t &hoid,
			     const map<string, bufferlist> &aset,
			     const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(cid, hoid, &path);
  if (r < 0)
    return r;
  return object_map->set_keys(hoid, aset, &spos);
}
int FileStore::_omap_rmkeys(coll_t cid, const hobject_t &hoid,
			    const set<string> &keys,
			    const SequencerPosition &spos) {
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(cid, hoid, &path);
  if (r < 0)
    return r;
  r = object_map->rm_keys(hoid, keys, &spos);
  if (r < 0 && r != -ENOENT)
    return r;
  return 0;
}
int FileStore::_omap_setheader(coll_t cid, const hobject_t &hoid,
			       const bufferlist &bl,
			       const SequencerPosition &spos)
{
  dout(15) << __func__ << " " << cid << "/" << hoid << dendl;
  IndexedPath path;
  int r = lfn_find(cid, hoid, &path);
  if (r < 0)
    return r;
  return object_map->set_header(hoid, bl, &spos);
}


const char** FileStore::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "filestore_min_sync_interval",
    "filestore_max_sync_interval",
    "filestore_flusher",
    "filestore_flusher_max_fds",
    "filestore_sync_flush",
    "filestore_commit_timeout",
    "filestore_dump_file",
    "filestore_kill_at",
    NULL
  };
  return KEYS;
}

void FileStore::handle_conf_change(const struct md_config_t *conf,
			  const std::set <std::string> &changed)
{
  if (changed.count("filestore_min_sync_interval") ||
      changed.count("filestore_max_sync_interval") ||
      changed.count("filestore_flusher_max_fds") ||
      changed.count("filestore_flush_min") ||
      changed.count("filestore_kill_at")) {
    Mutex::Locker l(lock);
    m_filestore_min_sync_interval = conf->filestore_min_sync_interval;
    m_filestore_max_sync_interval = conf->filestore_max_sync_interval;
    m_filestore_flusher = conf->filestore_flusher;
    m_filestore_flusher_max_fds = conf->filestore_flusher_max_fds;
    m_filestore_flush_min = conf->filestore_flush_min;
    m_filestore_sync_flush = conf->filestore_sync_flush;
    m_filestore_kill_at.set(conf->filestore_kill_at);
  }
  if (changed.count("filestore_commit_timeout")) {
    Mutex::Locker l(sync_entry_timeo_lock);
    m_filestore_commit_timeout = conf->filestore_commit_timeout;
  }
  if (changed.count("filestore_dump_file")) {
    if (conf->filestore_dump_file.length() &&
	conf->filestore_dump_file != "-") {
      dump_start(conf->filestore_dump_file);
    } else {
      dump_stop();
    }
  }
}

void FileStore::dump_start(const std::string& file)
{
  dout(10) << "dump_start " << file << dendl;
  if (m_filestore_do_dump) {
    dump_stop();
  }
  m_filestore_dump_fmt.reset();
  m_filestore_dump_fmt.open_array_section("dump");
  m_filestore_dump.open(file.c_str());
  m_filestore_do_dump = true;
}

void FileStore::dump_stop()
{
  dout(10) << "dump_stop" << dendl;
  m_filestore_do_dump = false;
  if (m_filestore_dump.is_open()) {
    m_filestore_dump_fmt.close_section();
    m_filestore_dump_fmt.flush(m_filestore_dump);
    m_filestore_dump.flush();
    m_filestore_dump.close();
  }
}

void FileStore::dump_transactions(list<ObjectStore::Transaction*>& ls, uint64_t seq, OpSequencer *osr)
{
  m_filestore_dump_fmt.open_array_section("transactions");
  unsigned trans_num = 0;
  for (list<ObjectStore::Transaction*>::iterator i = ls.begin(); i != ls.end(); ++i, ++trans_num) {
    m_filestore_dump_fmt.open_object_section("transaction");
    m_filestore_dump_fmt.dump_string("osr", osr->get_name());
    m_filestore_dump_fmt.dump_unsigned("seq", seq);
    m_filestore_dump_fmt.dump_unsigned("trans_num", trans_num);
    (*i)->dump(&m_filestore_dump_fmt);
    m_filestore_dump_fmt.close_section();
  }
  m_filestore_dump_fmt.close_section();
  m_filestore_dump_fmt.flush(m_filestore_dump);
  m_filestore_dump.flush();
}
