/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2011, 2018 by Delphix. All rights reserved.
 * Copyright 2017 Joyent, Inc.
 */

#include <sys/spa.h>
#include <sys/fm/fs/zfs.h>
#include <sys/spa_impl.h>
#include <sys/nvpair.h>
#include <sys/uio.h>
#include <sys/fs/zfs.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_ioctl.h>
#include <sys/utsname.h>
#include <sys/systeminfo.h>
#include <sys/sunddi.h>
#include <sys/zfeature.h>
#ifdef _KERNEL
#include <sys/kobj.h>
#include <sys/zone.h>
#endif

/*
 * Pool configuration repository.
 *
 * Pool configuration is stored as a packed nvlist on the filesystem.  By
 * default, all pools are stored in /etc/zfs/zpool.cache and loaded on boot
 * (when the ZFS module is loaded).  Pools can also have the 'cachefile'
 * property set that allows them to be stored in an alternate location until
 * the control of external software.
 *
 * For each cache file, we have a single nvlist which holds all the
 * configuration information.  When the module loads, we read this information
 * from /etc/zfs/zpool.cache and populate the SPA namespace.  This namespace is
 * maintained independently in spa.c.  Whenever the namespace is modified, or
 * the configuration of a pool is changed, we call spa_write_cachefile(), which
 * walks through all the active pools and writes the configuration to disk.
 */

static uint64_t spa_config_generation = 1;

/*
 * This can be overridden in userland to preserve an alternate namespace for
 * userland pools when doing testing.
 */
const char *spa_config_path = ZPOOL_CACHE;

/*
 * Called when the module is first loaded, this routine loads the configuration
 * file into the SPA namespace.  It does not actually open or load the pools; it
 * only populates the namespace.
 */
void
spa_config_load(void)
{
	void *buf = NULL;
	nvlist_t *nvlist, *child;
	nvpair_t *nvpair;
	char *pathname;
	struct _buf *file;
	uint64_t fsize;

	/*
	 * Open the configuration file.
	 */
	pathname = kmem_alloc(MAXPATHLEN, KM_SLEEP);

	(void) snprintf(pathname, MAXPATHLEN, "%s%s",
	    (rootdir != NULL) ? "./" : "", spa_config_path);

	file = kobj_open_file(pathname);

	kmem_free(pathname, MAXPATHLEN);

	if (file == (struct _buf *)-1)
		return;

	if (kobj_get_filesize(file, &fsize) != 0)
		goto out;

	buf = kmem_alloc(fsize, KM_SLEEP);

	/*
	 * Read the nvlist from the file.
	 */
	if (kobj_read_file(file, buf, fsize, 0) < 0)
		goto out;

	/*
	 * Unpack the nvlist.
	 */
	if (nvlist_unpack(buf, fsize, &nvlist, KM_SLEEP) != 0)
		goto out;

	/*
	 * Iterate over all elements in the nvlist, creating a new spa_t for
	 * each one with the specified configuration.
	 */
	mutex_enter(&spa_namespace_lock);
	nvpair = NULL;
	while ((nvpair = nvlist_next_nvpair(nvlist, nvpair)) != NULL) {
		if (nvpair_type(nvpair) != DATA_TYPE_NVLIST)
			continue;

		child = fnvpair_value_nvlist(nvpair);

		if (spa_lookup(nvpair_name(nvpair)) != NULL)
			continue;
		(void) spa_add(nvpair_name(nvpair), child, NULL);
	}
	mutex_exit(&spa_namespace_lock);

	nvlist_free(nvlist);

out:
	if (buf != NULL)
		kmem_free(buf, fsize);

	kobj_close_file(file);
}

static int
spa_config_write(spa_config_dirent_t *dp, nvlist_t *nvl)
{
	size_t buflen;
	char *buf;
	vnode_t *vp;
	int oflags = FWRITE | FTRUNC | FCREAT | FOFFMAX;
	char *temp;
	int err;

	/*
	 * If the nvlist is empty (NULL), then remove the old cachefile.
	 */
	if (nvl == NULL) {
		err = vn_remove(dp->scd_path, UIO_SYSSPACE, RMFILE);
		return (err);
	}

	/*
	 * Pack the configuration into a buffer.
	 */
	buf = fnvlist_pack(nvl, &buflen);
	temp = kmem_zalloc(MAXPATHLEN, KM_SLEEP);

	/*
	 * Write the configuration to disk.  We need to do the traditional
	 * 'write to temporary file, sync, move over original' to make sure we
	 * always have a consistent view of the data.
	 */
	(void) snprintf(temp, MAXPATHLEN, "%s.tmp", dp->scd_path);

	err = vn_open(temp, UIO_SYSSPACE, oflags, 0644, &vp, CRCREAT, 0);
	if (err == 0) {
		err = vn_rdwr(UIO_WRITE, vp, buf, buflen, 0, UIO_SYSSPACE,
		    0, RLIM64_INFINITY, kcred, NULL);
		if (err == 0)
			err = VOP_FSYNC(vp, FSYNC, kcred, NULL);
		if (err == 0)
			err = vn_rename(temp, dp->scd_path, UIO_SYSSPACE);
		(void) VOP_CLOSE(vp, oflags, 1, 0, kcred, NULL);
		VN_RELE(vp);
	}

	(void) vn_remove(temp, UIO_SYSSPACE, RMFILE);

	fnvlist_pack_free(buf, buflen);
	kmem_free(temp, MAXPATHLEN);
	return (err);
}

/*
 * Synchronize pool configuration to disk.  This must be called with the
 * namespace lock held. Synchronizing the pool cache is typically done after
 * the configuration has been synced to the MOS. This exposes a window where
 * the MOS config will have been updated but the cache file has not. If
 * the system were to crash at that instant then the cached config may not
 * contain the correct information to open the pool and an explicit import
 * would be required.
 */
void
spa_write_cachefile(spa_t *target, boolean_t removing, boolean_t postsysevent)
{
	spa_config_dirent_t *dp, *tdp;
	nvlist_t *nvl;
	boolean_t ccw_failure;
	int error;
	char *pool_name;

	ASSERT(MUTEX_HELD(&spa_namespace_lock));

	if (rootdir == NULL || !(spa_mode_global & FWRITE))
		return;

	/*
	 * Iterate over all cachefiles for the pool, past or present.  When the
	 * cachefile is changed, the new one is pushed onto this list, allowing
	 * us to update previous cachefiles that no longer contain this pool.
	 */
	ccw_failure = B_FALSE;
	for (dp = list_head(&target->spa_config_list); dp != NULL;
	    dp = list_next(&target->spa_config_list, dp)) {
		spa_t *spa = NULL;
		if (dp->scd_path == NULL)
			continue;

		/*
		 * Iterate over all pools, adding any matching pools to 'nvl'.
		 */
		nvl = NULL;
		while ((spa = spa_next(spa)) != NULL) {
			/*
			 * Skip over our own pool if we're about to remove
			 * ourselves from the spa namespace or any pool that
			 * is readonly. Since we cannot guarantee that a
			 * readonly pool would successfully import upon reboot,
			 * we don't allow them to be written to the cache file.
			 */
			if ((spa == target && removing) ||
			    !spa_writeable(spa))
				continue;

			mutex_enter(&spa->spa_props_lock);
			tdp = list_head(&spa->spa_config_list);
			if (spa->spa_config == NULL ||
			    tdp->scd_path == NULL ||
			    strcmp(tdp->scd_path, dp->scd_path) != 0) {
				mutex_exit(&spa->spa_props_lock);
				continue;
			}

			if (nvl == NULL)
				nvl = fnvlist_alloc();

			if (spa->spa_import_flags & ZFS_IMPORT_TEMP_NAME) {
				pool_name = fnvlist_lookup_string(
				    spa->spa_config, ZPOOL_CONFIG_POOL_NAME);
			} else {
				pool_name = spa_name(spa);
			}

			fnvlist_add_nvlist(nvl, pool_name,
			    spa->spa_config);
			mutex_exit(&spa->spa_props_lock);
		}

		error = spa_config_write(dp, nvl);
		if (error != 0)
			ccw_failure = B_TRUE;
		nvlist_free(nvl);
	}

	if (ccw_failure) {
		/*
		 * Keep trying so that configuration data is
		 * written if/when any temporary filesystem
		 * resource issues are resolved.
		 */
		if (target->spa_ccw_fail_time == 0) {
			zfs_ereport_post(FM_EREPORT_ZFS_CONFIG_CACHE_WRITE,
			    target, NULL, NULL, 0, 0);
		}
		target->spa_ccw_fail_time = gethrtime();
		spa_async_request(target, SPA_ASYNC_CONFIG_UPDATE);
	} else {
		/*
		 * Do not rate limit future attempts to update
		 * the config cache.
		 */
		target->spa_ccw_fail_time = 0;
	}

	/*
	 * Remove any config entries older than the current one.
	 */
	dp = list_head(&target->spa_config_list);
	while ((tdp = list_next(&target->spa_config_list, dp)) != NULL) {
		list_remove(&target->spa_config_list, tdp);
		if (tdp->scd_path != NULL)
			spa_strfree(tdp->scd_path);
		kmem_free(tdp, sizeof (spa_config_dirent_t));
	}

	spa_config_generation++;

	if (postsysevent)
		spa_event_notify(target, NULL, NULL, ESC_ZFS_CONFIG_SYNC);
}

/*
 * Sigh.  Inside a local zone, we don't have access to /etc/zfs/zpool.cache,
 * and we don't want to allow the local zone to see all the pools anyway.
 * So we have to invent the ZFS_IOC_CONFIG ioctl to grab the configuration
 * information for all pool visible within the zone.
 */
nvlist_t *
spa_all_configs(uint64_t *generation)
{
	nvlist_t *pools;
	spa_t *spa = NULL;

	if (*generation == spa_config_generation)
		return (NULL);

	pools = fnvlist_alloc();

	mutex_enter(&spa_namespace_lock);
	while ((spa = spa_next(spa)) != NULL) {
		if (INGLOBALZONE(curproc) ||
		    zone_dataset_visible(spa_name(spa), NULL)) {
			mutex_enter(&spa->spa_props_lock);
			fnvlist_add_nvlist(pools, spa_name(spa),
			    spa->spa_config);
			mutex_exit(&spa->spa_props_lock);
		}
	}
	*generation = spa_config_generation;
	mutex_exit(&spa_namespace_lock);

	return (pools);
}

void
spa_config_set(spa_t *spa, nvlist_t *config)
{
	mutex_enter(&spa->spa_props_lock);
	if (spa->spa_config != NULL && spa->spa_config != config)
		nvlist_free(spa->spa_config);
	spa->spa_config = config;
	mutex_exit(&spa->spa_props_lock);
}

/*
 * Generate the pool's configuration based on the current in-core state.
 *
 * We infer whether to generate a complete config or just one top-level config
 * based on whether vd is the root vdev.
 */
nvlist_t *
spa_config_generate(spa_t *spa, vdev_t *vd, uint64_t txg, int getstats)
{
	nvlist_t *config, *nvroot;
	vdev_t *rvd = spa->spa_root_vdev;
	unsigned long hostid = 0;
	boolean_t locked = B_FALSE;
	uint64_t split_guid;
	char *pool_name;

	if (vd == NULL) {
		vd = rvd;
		locked = B_TRUE;
		spa_config_enter(spa, SCL_CONFIG | SCL_STATE, FTAG, RW_READER);
	}

	ASSERT(spa_config_held(spa, SCL_CONFIG | SCL_STATE, RW_READER) ==
	    (SCL_CONFIG | SCL_STATE));

	/*
	 * If txg is -1, report the current value of spa->spa_config_txg.
	 */
	if (txg == -1ULL)
		txg = spa->spa_config_txg;

	/*
	 * Originally, users had to handle spa namespace collisions by either
	 * exporting the already imported pool or by specifying a new name for
	 * the pool with a conflicting name. In the case of root pools from
	 * virtual guests, neither approach to collision resolution is
	 * reasonable. This is addressed by extending the new name syntax with
	 * an option to specify that the new name is temporary. When specified,
	 * ZFS_IMPORT_TEMP_NAME will be set in spa->spa_import_flags to tell us
	 * to use the previous name, which we do below.
	 */
	if (spa->spa_import_flags & ZFS_IMPORT_TEMP_NAME) {
		pool_name = fnvlist_lookup_string(spa->spa_config,
		    ZPOOL_CONFIG_POOL_NAME);
	} else {
		pool_name = spa_name(spa);
	}

	config = fnvlist_alloc();

	fnvlist_add_uint64(config, ZPOOL_CONFIG_VERSION, spa_version(spa));
	fnvlist_add_string(config, ZPOOL_CONFIG_POOL_NAME, pool_name);
	fnvlist_add_uint64(config, ZPOOL_CONFIG_POOL_STATE, spa_state(spa));
	fnvlist_add_uint64(config, ZPOOL_CONFIG_POOL_TXG, txg);
	fnvlist_add_uint64(config, ZPOOL_CONFIG_POOL_GUID, spa_guid(spa));
	if (spa->spa_comment != NULL) {
		fnvlist_add_string(config, ZPOOL_CONFIG_COMMENT,
		    spa->spa_comment);
	}

	hostid = spa_get_hostid();
	if (hostid != 0) {
		fnvlist_add_uint64(config, ZPOOL_CONFIG_HOSTID, hostid);
	}
	fnvlist_add_string(config, ZPOOL_CONFIG_HOSTNAME, utsname.nodename);

	int config_gen_flags = 0;
	if (vd != rvd) {
		fnvlist_add_uint64(config, ZPOOL_CONFIG_TOP_GUID,
		    vd->vdev_top->vdev_guid);
		fnvlist_add_uint64(config, ZPOOL_CONFIG_GUID,
		    vd->vdev_guid);
		if (vd->vdev_isspare) {
			fnvlist_add_uint64(config,
			    ZPOOL_CONFIG_IS_SPARE, 1ULL);
		}
		if (vd->vdev_islog) {
			fnvlist_add_uint64(config,
			    ZPOOL_CONFIG_IS_LOG, 1ULL);
		}
		vd = vd->vdev_top;		/* label contains top config */
	} else {
		/*
		 * Only add the (potentially large) split information
		 * in the mos config, and not in the vdev labels
		 */
		if (spa->spa_config_splitting != NULL)
			fnvlist_add_nvlist(config, ZPOOL_CONFIG_SPLIT,
			    spa->spa_config_splitting);
		fnvlist_add_boolean(config,
		    ZPOOL_CONFIG_HAS_PER_VDEV_ZAPS);

		config_gen_flags |= VDEV_CONFIG_MOS;
	}

	/*
	 * Add the top-level config.  We even add this on pools which
	 * don't support holes in the namespace.
	 */
	vdev_top_config_generate(spa, config);

	/*
	 * If we're splitting, record the original pool's guid.
	 */
	if (spa->spa_config_splitting != NULL &&
	    nvlist_lookup_uint64(spa->spa_config_splitting,
	    ZPOOL_CONFIG_SPLIT_GUID, &split_guid) == 0) {
		fnvlist_add_uint64(config, ZPOOL_CONFIG_SPLIT_GUID,
		    split_guid);
	}

	nvroot = vdev_config_generate(spa, vd, getstats, config_gen_flags);
	fnvlist_add_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, nvroot);
	nvlist_free(nvroot);

	/*
	 * Store what's necessary for reading the MOS in the label.
	 */
	fnvlist_add_nvlist(config, ZPOOL_CONFIG_FEATURES_FOR_READ,
	    spa->spa_label_features);

	if (getstats && spa_load_state(spa) == SPA_LOAD_NONE) {
		ddt_histogram_t *ddh;
		ddt_stat_t *dds;
		ddt_object_t *ddo;

		ddh = kmem_zalloc(sizeof (ddt_histogram_t), KM_SLEEP);
		ddt_get_dedup_histogram(spa, ddh);
		fnvlist_add_uint64_array(config,
		    ZPOOL_CONFIG_DDT_HISTOGRAM,
		    (uint64_t *)ddh, sizeof (*ddh) / sizeof (uint64_t));
		kmem_free(ddh, sizeof (ddt_histogram_t));

		ddo = kmem_zalloc(sizeof (ddt_object_t), KM_SLEEP);
		ddt_get_dedup_object_stats(spa, ddo);
		fnvlist_add_uint64_array(config,
		    ZPOOL_CONFIG_DDT_OBJ_STATS,
		    (uint64_t *)ddo, sizeof (*ddo) / sizeof (uint64_t));
		kmem_free(ddo, sizeof (ddt_object_t));

		dds = kmem_zalloc(sizeof (ddt_stat_t), KM_SLEEP);
		ddt_get_dedup_stats(spa, dds);
		fnvlist_add_uint64_array(config,
		    ZPOOL_CONFIG_DDT_STATS,
		    (uint64_t *)dds, sizeof (*dds) / sizeof (uint64_t));
		kmem_free(dds, sizeof (ddt_stat_t));
	}

	if (locked)
		spa_config_exit(spa, SCL_CONFIG | SCL_STATE, FTAG);

	return (config);
}

/*
 * Update all disk labels, generate a fresh config based on the current
 * in-core state, and sync the global config cache (do not sync the config
 * cache if this is a booting rootpool).
 */
void
spa_config_update(spa_t *spa, int what)
{
	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t txg;
	int c;

	ASSERT(MUTEX_HELD(&spa_namespace_lock));

	spa_config_enter(spa, SCL_ALL, FTAG, RW_WRITER);
	txg = spa_last_synced_txg(spa) + 1;
	if (what == SPA_CONFIG_UPDATE_POOL) {
		vdev_config_dirty(rvd);
	} else {
		/*
		 * If we have top-level vdevs that were added but have
		 * not yet been prepared for allocation, do that now.
		 * (It's safe now because the config cache is up to date,
		 * so it will be able to translate the new DVAs.)
		 * See comments in spa_vdev_add() for full details.
		 */
		for (c = 0; c < rvd->vdev_children; c++) {
			vdev_t *tvd = rvd->vdev_child[c];

			/*
			 * Explicitly skip vdevs that are indirect or
			 * log vdevs that are being removed. The reason
			 * is that both of those can have vdev_ms_array
			 * set to 0 and we wouldn't want to change their
			 * metaslab size nor call vdev_expand() on them.
			 */
			if (!vdev_is_concrete(tvd) ||
			    (tvd->vdev_islog && tvd->vdev_removing))
				continue;

			if (tvd->vdev_ms_array == 0)
				vdev_metaslab_set_size(tvd);
			vdev_expand(tvd, txg);
		}
	}
	spa_config_exit(spa, SCL_ALL, FTAG);

	/*
	 * Wait for the mosconfig to be regenerated and synced.
	 */
	txg_wait_synced(spa->spa_dsl_pool, txg);

	/*
	 * Update the global config cache to reflect the new mosconfig.
	 */
	if (!spa->spa_is_root) {
		spa_write_cachefile(spa, B_FALSE,
		    what != SPA_CONFIG_UPDATE_POOL);
	}

	if (what == SPA_CONFIG_UPDATE_POOL)
		spa_config_update(spa, SPA_CONFIG_UPDATE_VDEVS);
}
