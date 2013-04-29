/*
   drbd_state.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   Thanks to Carter Burden, Bart Grantham and Gennadiy Nerubayev
   from Logicworks, Inc. for making SDP replication support possible.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/drbd_limits.h>
#include <linux/random.h>
#include "drbd_int.h"
#include "drbd_protocol.h"
#include "drbd_req.h"
#include "drbd_state_change.h"

/* in drbd_main.c */
extern void tl_abort_disk_io(struct drbd_device *device);

struct after_state_change_work {
	struct drbd_work w;
	struct drbd_state_change *state_change;
	unsigned int id;
	struct completion *done;
};

static struct drbd_state_change *alloc_state_change(struct drbd_resource *resource, gfp_t flags)
{
	struct drbd_state_change *state_change;
	unsigned int n_devices = 0, n_connections = 0, size, n;
	struct drbd_device *device;
	struct drbd_connection *connection;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, vnr)
		n_devices++;
	for_each_connection(connection, resource)
		n_connections++;
	rcu_read_unlock();

	size = sizeof(struct drbd_state_change) +
	       n_devices * sizeof(struct drbd_device_state_change) +
	       n_connections * sizeof(struct drbd_connection_state_change) +
	       n_devices * n_connections * sizeof(struct drbd_peer_device_state_change);
	state_change = kmalloc(size, flags);
	if (!state_change)
		return NULL;
	state_change->n_devices = n_devices;
	state_change->n_connections = n_connections;
	state_change->devices = (void *)(state_change + 1);
	state_change->connections = (void *)&state_change->devices[n_devices];
	state_change->peer_devices = (void *)&state_change->connections[n_connections];
	state_change->resource->resource = NULL;
	for (n = 0; n < n_devices; n++)
		state_change->devices[n].device = NULL;
	for (n = 0; n < n_connections; n++)
		state_change->connections[n].connection = NULL;
	return state_change;
}

struct drbd_state_change *remember_state_change(struct drbd_resource *resource, gfp_t gfp)
{
	struct drbd_state_change *state_change;
	struct drbd_device *device;
	unsigned int n_devices;
	struct drbd_connection *connection;
	unsigned int n_connections;
	int vnr;

	struct drbd_device_state_change *device_state_change;
	struct drbd_peer_device_state_change *peer_device_state_change;
	struct drbd_connection_state_change *connection_state_change;

retry:
	state_change = alloc_state_change(resource, gfp);
	if (!state_change)
		return NULL;

	rcu_read_lock();
	n_devices = 0;
	idr_for_each_entry(&resource->devices, device, vnr)
		n_devices++;
	n_connections = 0;
	for_each_connection(connection, resource)
		n_connections++;
	if (n_devices != state_change->n_devices ||
	    n_connections != state_change->n_connections) {
		kfree(state_change);
		rcu_read_unlock();
		goto retry;
	}

	kref_get(&resource->kref);
	state_change->resource->resource = resource;
	memcpy(state_change->resource->role,
	       resource->role, sizeof(resource->role));
	memcpy(state_change->resource->susp,
	       resource->susp, sizeof(resource->susp));
	memcpy(state_change->resource->susp_nod,
	       resource->susp_nod, sizeof(resource->susp_nod));
	memcpy(state_change->resource->susp_fen,
	       resource->susp_fen, sizeof(resource->susp_fen));
	memcpy(state_change->resource->weak,
	       resource->weak, sizeof(resource->weak));

	device_state_change = state_change->devices;
	peer_device_state_change = state_change->peer_devices;
	idr_for_each_entry(&resource->devices, device, vnr) {
		struct drbd_peer_device *peer_device;

		kobject_get(&device->kobj);
		device_state_change->device = device;
		memcpy(device_state_change->disk_state,
		       device->disk_state, sizeof(device->disk_state));

		/* The peer_devices for each device have to be enumerated in
		   the order of the connections. We may not use for_each_peer_device() here. */
		for_each_connection(connection, resource) {
			peer_device = conn_peer_device(connection, device->vnr);

			peer_device_state_change->peer_device = peer_device;
			memcpy(peer_device_state_change->disk_state,
			       peer_device->disk_state, sizeof(peer_device->disk_state));
			memcpy(peer_device_state_change->repl_state,
			       peer_device->repl_state, sizeof(peer_device->repl_state));
			memcpy(peer_device_state_change->resync_susp_user,
			       peer_device->resync_susp_user,
			       sizeof(peer_device->resync_susp_user));
			memcpy(peer_device_state_change->resync_susp_peer,
			       peer_device->resync_susp_peer,
			       sizeof(peer_device->resync_susp_peer));
			memcpy(peer_device_state_change->resync_susp_dependency,
			       peer_device->resync_susp_dependency,
			       sizeof(peer_device->resync_susp_dependency));
			memcpy(peer_device_state_change->resync_susp_other_c,
			       peer_device->resync_susp_other_c,
			       sizeof(peer_device->resync_susp_other_c));
			peer_device_state_change++;
		}
		device_state_change++;
	}

	connection_state_change = state_change->connections;
	for_each_connection(connection, resource) {
		kref_get(&connection->kref);
		connection_state_change->connection = connection;
		memcpy(connection_state_change->cstate,
		       connection->cstate, sizeof(connection->cstate));
		memcpy(connection_state_change->peer_role,
		       connection->peer_role, sizeof(connection->peer_role));
		connection_state_change++;
	}
	rcu_read_unlock();

	return state_change;
}

void forget_state_change(struct drbd_state_change *state_change)
{
	unsigned int n;

	if (!state_change)
		return;

	if (state_change->resource->resource)
		kref_put(&state_change->resource->resource->kref, drbd_destroy_resource);
	for (n = 0; n < state_change->n_devices; n++) {
		struct drbd_device *device = state_change->devices[n].device;

		if (device)
			kobject_put(&device->kobj);
	}
	for (n = 0; n < state_change->n_connections; n++) {
		struct drbd_connection *connection =
			state_change->connections[n].connection;

		if (connection)
			kref_put(&connection->kref, drbd_destroy_connection);
	}
	kfree(state_change);
}

static void print_state_change(struct drbd_resource *resource, const char *prefix);
static void finish_state_change(struct drbd_resource *, struct completion *);
STATIC int w_after_state_change(struct drbd_work *w, int unused);
static enum drbd_state_rv is_valid_soft_transition(struct drbd_resource *);
static enum drbd_state_rv is_valid_transition(struct drbd_resource *resource);
static void sanitize_state(struct drbd_resource *resource);
static enum drbd_state_rv change_peer_state(struct drbd_connection *, int, union drbd_state,
					    union drbd_state, unsigned long *);

static bool state_has_changed(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	struct drbd_device *device;
	int minor;

	if (resource->role[OLD] != resource->role[NEW] ||
	    resource->susp[OLD] != resource->susp[NEW] ||
	    resource->susp_nod[OLD] != resource->susp_nod[NEW] ||
	    resource->susp_fen[OLD] != resource->susp_fen[NEW] ||
	    resource->weak[OLD] != resource->weak[NEW])
		return true;

	for_each_connection(connection, resource) {
		if (connection->cstate[OLD] != connection->cstate[NEW] ||
		    connection->peer_role[OLD] != connection->peer_role[NEW])
			return true;
	}

	idr_for_each_entry(&resource->devices, device, minor) {
		struct drbd_peer_device *peer_device;

		if (device->disk_state[OLD] != device->disk_state[NEW])
			return true;

		for_each_peer_device(peer_device, device) {
			if (peer_device->disk_state[OLD] != peer_device->disk_state[NEW] ||
			    peer_device->repl_state[OLD] != peer_device->repl_state[NEW] ||
			    peer_device->resync_susp_user[OLD] !=
				peer_device->resync_susp_user[NEW] ||
			    peer_device->resync_susp_peer[OLD] !=
				peer_device->resync_susp_peer[NEW] ||
			    peer_device->resync_susp_dependency[OLD] !=
				peer_device->resync_susp_dependency[NEW] ||
			    peer_device->resync_susp_other_c[OLD] !=
				peer_device->resync_susp_other_c[NEW])
				return true;
		}
	}
	return false;
}

static void ___begin_state_change(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	struct drbd_device *device;
	int minor;

	resource->role[NEW] = resource->role[NOW];
	resource->susp[NEW] = resource->susp[NOW];
	resource->susp_nod[NEW] = resource->susp_nod[NOW];
	resource->susp_fen[NEW] = resource->susp_fen[NOW];
	resource->weak[NEW] = resource->weak[NOW];

	for_each_connection(connection, resource) {
		connection->cstate[NEW] = connection->cstate[NOW];
		connection->peer_role[NEW] = connection->peer_role[NOW];
		connection->peer_weak[NEW] = connection->peer_weak[NOW];
	}

	idr_for_each_entry(&resource->devices, device, minor) {
		struct drbd_peer_device *peer_device;

		device->disk_state[NEW] = device->disk_state[NOW];

		for_each_peer_device(peer_device, device) {
			peer_device->disk_state[NEW] = peer_device->disk_state[NOW];
			peer_device->repl_state[NEW] = peer_device->repl_state[NOW];
			peer_device->resync_susp_user[NEW] =
				peer_device->resync_susp_user[NOW];
			peer_device->resync_susp_peer[NEW] =
				peer_device->resync_susp_peer[NOW];
			peer_device->resync_susp_dependency[NEW] =
				peer_device->resync_susp_dependency[NOW];
			peer_device->resync_susp_other_c[NEW] =
				peer_device->resync_susp_other_c[NOW];
		}
	}
}

static void __begin_state_change(struct drbd_resource *resource)
{
	rcu_read_lock();
	___begin_state_change(resource);
}

static enum drbd_state_rv try_state_change(struct drbd_resource *resource)
{
	enum drbd_state_rv rv;

	if (!state_has_changed(resource))
		return SS_NOTHING_TO_DO;
	sanitize_state(resource);
	rv = is_valid_transition(resource);
	if (rv >= SS_SUCCESS && !(resource->state_change_flags & CS_HARD))
		rv = is_valid_soft_transition(resource);
	return rv;
}

static enum drbd_state_rv ___end_state_change(struct drbd_resource *resource, struct completion *done,
					      enum drbd_state_rv rv)
{
	enum chg_state_flags flags = resource->state_change_flags;
	struct drbd_connection *connection;
	struct drbd_device *device;
	int minor;

	if (flags & CS_ABORT)
		goto out;
	if (rv >= SS_SUCCESS)
		rv = try_state_change(resource);
	if (rv < SS_SUCCESS) {
		if (flags & CS_VERBOSE) {
			drbd_err(resource, "State change failed: %s\n", drbd_set_st_err_str(rv));
			print_state_change(resource, "Failed: ");
		}
		goto out;
	}
	if (flags & CS_PREPARE)
		goto out;

	finish_state_change(resource, done);

	resource->role[NOW] = resource->role[NEW];
	resource->susp[NOW] = resource->susp[NEW];
	resource->susp_nod[NOW] = resource->susp_nod[NEW];
	resource->susp_fen[NOW] = resource->susp_fen[NEW];
	resource->weak[NOW] = resource->weak[NEW];

	for_each_connection(connection, resource) {
		connection->cstate[NOW] = connection->cstate[NEW];
		connection->peer_role[NOW] = connection->peer_role[NEW];
		connection->peer_weak[NOW] = connection->peer_weak[NEW];
	}

	idr_for_each_entry(&resource->devices, device, minor) {
		struct drbd_peer_device *peer_device;

		device->disk_state[NOW] = device->disk_state[NEW];

		for_each_peer_device(peer_device, device) {
			peer_device->disk_state[NOW] = peer_device->disk_state[NEW];
			peer_device->repl_state[NOW] = peer_device->repl_state[NEW];
			peer_device->resync_susp_user[NOW] =
				peer_device->resync_susp_user[NEW];
			peer_device->resync_susp_peer[NOW] =
				peer_device->resync_susp_peer[NEW];
			peer_device->resync_susp_dependency[NOW] =
				peer_device->resync_susp_dependency[NEW];
			peer_device->resync_susp_other_c[NOW] =
				peer_device->resync_susp_other_c[NEW];
		}
	}
out:
	rcu_read_unlock();

	if ((flags & CS_TWOPC) && !(flags & CS_PREPARE)) {
		resource->remote_state_change = false;
		resource->twopc_reply.initiator_node_id = -1;
		resource->twopc_reply.tid = 0;
		if (resource->twopc_parent) {
			kref_put(&resource->twopc_parent->kref,
				 drbd_destroy_connection);
			resource->twopc_parent = NULL;
		}
		wake_up(&resource->twopc_wait);
	}

	return rv;
}

void state_change_lock(struct drbd_resource *resource, unsigned long *irq_flags, enum chg_state_flags flags)
{
	if ((flags & CS_SERIALIZE) && !(flags & (CS_ALREADY_SERIALIZED | CS_PREPARED)))
		down(&resource->state_sem);
	spin_lock_irqsave(&resource->req_lock, *irq_flags);
	resource->state_change_flags = flags;
}

static void __state_change_unlock(struct drbd_resource *resource, unsigned long *irq_flags, struct completion *done)
{
	enum chg_state_flags flags = resource->state_change_flags;

	resource->state_change_flags = 0;
	spin_unlock_irqrestore(&resource->req_lock, *irq_flags);
	if (done && expect(resource, current != resource->worker.task))
		wait_for_completion(done);
	if ((flags & CS_SERIALIZE) && !(flags & (CS_ALREADY_SERIALIZED | CS_PREPARE)))
		up(&resource->state_sem);
}

void state_change_unlock(struct drbd_resource *resource, unsigned long *irq_flags)
{
	__state_change_unlock(resource, irq_flags, NULL);
}

/**
 * abort_prepared_state_change
 *
 * Use when a remote state change request was prepared but neither committed
 * nor aborted; the remote state change still "holds the state mutex".
 */
void abort_prepared_state_change(struct drbd_resource *resource)
{
	up(&resource->state_sem);
}

void begin_state_change_locked(struct drbd_resource *resource, enum chg_state_flags flags)
{
	BUG_ON(flags & (CS_SERIALIZE | CS_WAIT_COMPLETE | CS_PREPARE | CS_ABORT));
	resource->state_change_flags = flags;
	__begin_state_change(resource);
}

enum drbd_state_rv end_state_change_locked(struct drbd_resource *resource)
{
	return ___end_state_change(resource, NULL, SS_SUCCESS);
}

void begin_state_change(struct drbd_resource *resource, unsigned long *irq_flags, enum chg_state_flags flags)
{
	state_change_lock(resource, irq_flags, flags);
	__begin_state_change(resource);
}

static bool all_peer_devices_connected(struct drbd_connection *connection)
{
	struct drbd_peer_device *peer_device;
	int vnr;
	bool rv = true;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		if (peer_device->repl_state[NOW] < L_ESTABLISHED) {
			rv = false;
			break;
		}
	}
	rcu_read_unlock();

	return rv;
}

static enum drbd_state_rv __end_state_change(struct drbd_resource *resource,
					     unsigned long *irq_flags,
					     enum drbd_state_rv rv)
{
	enum chg_state_flags flags = resource->state_change_flags;
	struct completion __done, *done = NULL;

	if ((flags & CS_WAIT_COMPLETE) && !(flags & (CS_PREPARE | CS_ABORT))) {
		done = &__done;
		init_completion(done);
	}
	rv = ___end_state_change(resource, done, rv);
	__state_change_unlock(resource, irq_flags, rv >= SS_SUCCESS ? done : NULL);
	return rv;
}

enum drbd_state_rv end_state_change(struct drbd_resource *resource, unsigned long *irq_flags)
{
	return __end_state_change(resource, irq_flags, SS_SUCCESS);
}

void abort_state_change(struct drbd_resource *resource, unsigned long *irq_flags)
{
	resource->state_change_flags &= ~CS_VERBOSE;
	__end_state_change(resource, irq_flags, SS_UNKNOWN_ERROR);
}

void abort_state_change_locked(struct drbd_resource *resource)
{
	resource->state_change_flags &= ~CS_VERBOSE;
	___end_state_change(resource, NULL, SS_UNKNOWN_ERROR);
}

static void begin_remote_state_change(struct drbd_resource *resource, unsigned long *irq_flags)
{
	enum chg_state_flags flags = resource->state_change_flags;

	rcu_read_unlock();
	spin_unlock_irqrestore(&resource->req_lock, *irq_flags);
        if (flags & CS_SERIALIZE)
		up(&resource->state_sem);
}

static void __end_remote_state_change(struct drbd_resource *resource, enum chg_state_flags flags)
{
        if (flags & CS_SERIALIZE)
		down(&resource->state_sem);
	rcu_read_lock();
	resource->state_change_flags = flags;
	___begin_state_change(resource);
}

static void end_remote_state_change(struct drbd_resource *resource, unsigned long *irq_flags, enum chg_state_flags flags)
{
	spin_lock_irqsave(&resource->req_lock, *irq_flags);
	__end_remote_state_change(resource, flags);
}

static union drbd_state drbd_get_resource_state(struct drbd_resource *resource, enum which_state which)
{
	union drbd_state rv = { {
		.conn = C_STANDALONE,  /* really: undefined */
		/* (user_isp, peer_isp, and aftr_isp are undefined as well.) */
		.disk = D_UNKNOWN,  /* really: undefined */
		.role = resource->role[which],
		.peer = R_UNKNOWN,  /* really: undefined */
		.susp = resource->susp[which],
		.susp_nod = resource->susp_nod[which],
		.susp_fen = resource->susp_fen[which],
		.pdsk = D_UNKNOWN,  /* really: undefined */
		.weak = resource->weak[which],
	} };

	return rv;
}

union drbd_state drbd_get_device_state(struct drbd_device *device, enum which_state which)
{
	union drbd_state rv = drbd_get_resource_state(device->resource, which);

	rv.disk = device->disk_state[which];

	return rv;
}

union drbd_state drbd_get_peer_device_state(struct drbd_peer_device *peer_device, enum which_state which)
{
	struct drbd_connection *connection = peer_device->connection;
	union drbd_state rv;

	rv = drbd_get_device_state(peer_device->device, which);
	rv.user_isp = peer_device->resync_susp_user[which];
	rv.peer_isp = peer_device->resync_susp_peer[which];
	rv.aftr_isp = peer_device->resync_susp_dependency[which] || peer_device->resync_susp_other_c[which];
	rv.conn = combined_conn_state(peer_device, which);
	rv.peer = connection->peer_role[which];
	rv.pdsk = peer_device->disk_state[which];

	return rv;
}

union drbd_state drbd_get_connection_state(struct drbd_connection *connection, enum which_state which)
{
	union drbd_state rv = drbd_get_resource_state(connection->resource, which);

	rv.conn = connection->cstate[which];
	rv.peer = connection->peer_role[which];

	return rv;
}

static inline bool is_susp(union drbd_state s)
{
        return s.susp || s.susp_nod || s.susp_fen;
}

enum drbd_disk_state conn_highest_disk(struct drbd_connection *connection)
{
	enum drbd_disk_state ds = D_DISKLESS;
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		ds = max_t(enum drbd_disk_state, ds, device->disk_state[NOW]);
	}
	rcu_read_unlock();

	return ds;
}

enum drbd_disk_state conn_lowest_disk(struct drbd_connection *connection)
{
	enum drbd_disk_state ds = D_MASK;
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		ds = min_t(enum drbd_disk_state, ds, device->disk_state[NOW]);
	}
	rcu_read_unlock();

	return ds;
}

enum drbd_disk_state conn_highest_pdsk(struct drbd_connection *connection)
{
	enum drbd_disk_state ds = D_DISKLESS;
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr)
		ds = max_t(enum drbd_disk_state, ds, peer_device->disk_state[NOW]);
	rcu_read_unlock();

	return ds;
}

static enum drbd_repl_state conn_lowest_repl_state(struct drbd_connection *connection)
{
	unsigned int repl_state = -1U;
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		if (peer_device->repl_state[NOW] < repl_state)
			repl_state = peer_device->repl_state[NOW];
	}
	rcu_read_unlock();

	if (repl_state == -1U)
		return L_OFF;

	return repl_state;
}

static bool resync_suspended(struct drbd_peer_device *peer_device, enum which_state which)
{
	return peer_device->resync_susp_user[which] ||
	       peer_device->resync_susp_peer[which] ||
	       peer_device->resync_susp_dependency[which] ||
	       peer_device->resync_susp_other_c[which];
}

static void set_resync_susp_other_c(struct drbd_peer_device *peer_device, bool val, bool start)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_peer_device *p;

	for_each_peer_device(p, device) {
		if (p == peer_device)
			continue;
		p->resync_susp_other_c[NEW] = val;
		if (!val && p->repl_state[NEW] == L_PAUSED_SYNC_T && !resync_suspended(p, NEW))
			p->repl_state[NEW] = L_SYNC_TARGET;
		if (val && start &&
		    p->disk_state[NEW] == D_UP_TO_DATE && p->repl_state[NEW] == L_ESTABLISHED)
			p->repl_state[NEW] = L_PAUSED_SYNC_T;
	}
}

static int scnprintf_resync_suspend_flags(char *buffer, size_t size,
					  struct drbd_peer_device *peer_device,
					  enum which_state which)
{
	char *b = buffer, *end = buffer + size;

	if (!resync_suspended(peer_device, which))
		return scnprintf(buffer, size, "no");

	if (peer_device->resync_susp_user[which])
		b += scnprintf(b, end - b, "user,");
	if (peer_device->resync_susp_peer[which])
		b += scnprintf(b, end - b, "peer,");
	if (peer_device->resync_susp_dependency[which])
		b += scnprintf(b, end - b, "after dependency,");
	if (peer_device->resync_susp_other_c[which])
		b += scnprintf(b, end - b, "connection dependency,");
	*(--b) = 0;

	return b - buffer;
}

static bool io_suspended(struct drbd_resource *resource, enum which_state which)
{
	return resource->susp[which] ||
	       resource->susp_nod[which] ||
	       resource->susp_fen[which];
}

static int scnprintf_io_suspend_flags(char *buffer, size_t size,
				      struct drbd_resource *resource,
				      enum which_state which)
{
	char *b = buffer, *end = buffer + size;

	if (!io_suspended(resource, which))
		return scnprintf(buffer, size, "no");

	if (resource->susp[which])
		b += scnprintf(b, end - b, "user,");
	if (resource->susp_nod[which])
		b += scnprintf(b, end - b, "no-disk,");
	if (resource->susp_fen[which])
		b += scnprintf(b, end - b, "fencing,");
	*(--b) = 0;

	return b - buffer;
}

static void print_state_change(struct drbd_resource *resource, const char *prefix)
{
	char buffer[150], *b, *end = buffer + sizeof(buffer);
	struct drbd_connection *connection;
	struct drbd_device *device;
	enum drbd_role *role = resource->role;
	bool *weak = resource->weak;
	int vnr;

	b = buffer;
	if (role[OLD] != role[NEW])
		b += scnprintf(b, end - b, "role( %s -> %s ) ",
			       drbd_role_str(role[OLD]),
			       drbd_role_str(role[NEW]));
	if (io_suspended(resource, OLD) != io_suspended(resource, NEW)) {
		b += scnprintf(b, end - b, "susp-io( ");
		b += scnprintf_io_suspend_flags(b, end - b, resource, OLD);
		b += scnprintf(b, end - b, " -> ");
		b += scnprintf_io_suspend_flags(b, end - b, resource, NEW);
		b += scnprintf(b, end - b, ") ");
	}
	if (weak[OLD] != weak[NEW])
		b+= scnprintf(b, end - b, "weak( %d -> %d ) ",
			      weak[OLD], weak[NEW]);
	if (b != buffer) {
		*(b-1) = 0;
		drbd_info(resource, "%s%s\n", prefix, buffer);
	}

	for_each_connection(connection, resource) {
		enum drbd_conn_state *cstate = connection->cstate;
		enum drbd_role *peer_role = connection->peer_role;

		b = buffer;
		if (cstate[OLD] != cstate[NEW])
			b += scnprintf(b, end - b, "conn( %s -> %s ) ",
				       drbd_conn_str(cstate[OLD]),
				       drbd_conn_str(cstate[NEW]));
		if (peer_role[OLD] != peer_role[NEW])
			b += scnprintf(b, end - b, "peer( %s -> %s ) ",
				       drbd_role_str(peer_role[OLD]),
				       drbd_role_str(peer_role[NEW]));

		if (b != buffer) {
			*(b-1) = 0;
			drbd_info(connection, "%s%s\n", prefix, buffer);
		}
	}

	idr_for_each_entry(&resource->devices, device, vnr) {
		struct drbd_peer_device *peer_device;
		enum drbd_disk_state *disk_state = device->disk_state;

		if (disk_state[OLD] != disk_state[NEW])
			drbd_info(device, "%sdisk( %s -> %s )\n",
				  prefix,
				  drbd_disk_str(disk_state[OLD]),
				  drbd_disk_str(disk_state[NEW]));

		for_each_peer_device(peer_device, device) {
			enum drbd_disk_state *peer_disk_state = peer_device->disk_state;
			enum drbd_repl_state *repl_state = peer_device->repl_state;

			b = buffer;
			if (peer_disk_state[OLD] != peer_disk_state[NEW])
				b += scnprintf(b, end - b, "pdsk( %s -> %s ) ",
					       drbd_disk_str(peer_disk_state[OLD]),
					       drbd_disk_str(peer_disk_state[NEW]));
			if (repl_state[OLD] != repl_state[NEW])
				b += scnprintf(b, end - b, "repl( %s -> %s ) ",
					       drbd_repl_str(repl_state[OLD]),
					       drbd_repl_str(repl_state[NEW]));

			if (resync_suspended(peer_device, OLD) !=
			    resync_suspended(peer_device, NEW)) {
				b += scnprintf(b, end - b, "resync-susp( ");
				b += scnprintf_resync_suspend_flags(b, end - b, peer_device, OLD);
				b += scnprintf(b, end - b, " -> ");
				b += scnprintf_resync_suspend_flags(b, end - b, peer_device, NEW);
				b += scnprintf(b, end - b, " ) ");
			}

			if (b != buffer) {
				*(b-1) = 0;
				drbd_info(peer_device, "%s%s\n", prefix, buffer);
			}
		}
	}
}

static bool local_disk_may_be_outdated(struct drbd_device *device, enum which_state which)
{
	bool weak = device->resource->weak[which];
	struct drbd_peer_device *peer_device;
	int established = 0;

	if (weak)
		return true;

	for_each_peer_device(peer_device, device) {
		enum drbd_repl_state repl_state = peer_device->repl_state[which];

		switch(repl_state) {
		case L_WF_BITMAP_S:
		case L_STARTING_SYNC_S:
		case L_SYNC_SOURCE:
		case L_PAUSED_SYNC_S:
		case L_AHEAD:
			return false;
		case L_ESTABLISHED:
		case L_VERIFY_S:
		case L_VERIFY_T:
			established++;
			continue;
		case L_OFF:
			continue;
		case L_WF_SYNC_UUID:
		case L_WF_BITMAP_T:
		case L_STARTING_SYNC_T:
		case L_SYNC_TARGET:
		case L_PAUSED_SYNC_T:
		case L_BEHIND:
			return true;
		}
	}
	if (established)
		return false;

	return true;
}

static enum drbd_state_rv __is_valid_soft_transition(struct drbd_resource *resource)
{
	enum drbd_role *role = resource->role;
	bool *weak = resource->weak;
	struct drbd_connection *connection;
	struct drbd_device *device;
	int vnr;

	/* See drbd_state_sw_errors in drbd_strings.c */

	if (role[OLD] != R_PRIMARY && role[NEW] == R_PRIMARY) {
		for_each_connection(connection, resource) {
			struct net_conf *nc;

			nc = rcu_dereference(connection->net_conf);
			if (!nc || nc->two_primaries)
				continue;
			if (connection->peer_role[NEW] == R_PRIMARY)
				return SS_TWO_PRIMARIES;
		}
	}

	if (weak[NEW] && role[NEW] == R_PRIMARY)
		return SS_WEAKLY_CONNECTED;

	for_each_connection(connection, resource) {
		enum drbd_conn_state *cstate = connection->cstate;
		enum drbd_role *peer_role = connection->peer_role;
		struct net_conf *nc;
		bool two_primaries;

		if (cstate[NEW] == C_DISCONNECTING && cstate[OLD] == C_STANDALONE)
			return SS_ALREADY_STANDALONE;

		if (cstate[NEW] == C_CONNECTING && cstate[OLD] < C_UNCONNECTED)
			return SS_NO_NET_CONFIG;

		if (cstate[NEW] == C_DISCONNECTING && cstate[OLD] == C_UNCONNECTED)
			return SS_IN_TRANSIENT_STATE;

		/* While establishing a connection only allow cstate to change.
		   Delay/refuse role changes, detach attach etc... */
		if (test_bit(INITIAL_STATE_SENT, &connection->flags) &&
		    !test_bit(INITIAL_STATE_RECEIVED, &connection->flags) &&
		    !(cstate[OLD] == C_CONNECTED ||
		      (cstate[NEW] == C_CONNECTED && cstate[OLD] == C_CONNECTING)))
			return SS_IN_TRANSIENT_STATE;

		nc = rcu_dereference(connection->net_conf);
		two_primaries = nc ? nc->two_primaries : false;
		if (peer_role[NEW] == R_PRIMARY && peer_role[OLD] == R_SECONDARY &&
		    resource->open_ro_cnt && !two_primaries)
			return SS_PRIMARY_READER;
	}

	if (role[OLD] != R_SECONDARY && role[NEW] == R_SECONDARY && resource->open_rw_cnt)
		return SS_DEVICE_IN_USE;


	idr_for_each_entry(&resource->devices, device, vnr) {
		enum drbd_disk_state *disk_state = device->disk_state;
		struct drbd_peer_device *peer_device;
		bool one_peer_disk_up_to_date[2];
		enum which_state which;

		if (disk_state[NEW] > D_ATTACHING && disk_state[OLD] == D_DISKLESS)
			return SS_IS_DISKLESS;

		if (disk_state[NEW] == D_OUTDATED && disk_state[OLD] < D_OUTDATED &&
		    disk_state[OLD] != D_ATTACHING) {
			/* Do not allow outdate of inconsistent or diskless.
			   But we have to allow Inconsistent -> Outdated if a resync
			   finishes over one connection, and is paused on other connections */

			for_each_peer_device(peer_device, device) {
				enum drbd_repl_state *repl_state = peer_device->repl_state;
				if (repl_state[OLD] == L_SYNC_TARGET && repl_state[NEW] == L_ESTABLISHED)
					goto allow;
			}
			return SS_LOWER_THAN_OUTDATED;
		}
		allow:

		for (which = OLD; which <= NEW; which++) {
			one_peer_disk_up_to_date[which] = false;
			for_each_peer_device(peer_device, device) {
				enum drbd_disk_state *peer_disk_state = peer_device->disk_state;

				if (peer_disk_state[which] == D_UP_TO_DATE) {
					one_peer_disk_up_to_date[which] = true;
					break;
				}
			}
		}
		if (!(role[OLD] == R_PRIMARY && disk_state[OLD] < D_UP_TO_DATE && !one_peer_disk_up_to_date[OLD]) &&
		    (role[NEW] == R_PRIMARY && disk_state[NEW] < D_UP_TO_DATE && !one_peer_disk_up_to_date[OLD]))
			return SS_NO_UP_TO_DATE_DISK;

		for_each_peer_device(peer_device, device) {
			enum drbd_disk_state *peer_disk_state = peer_device->disk_state;
			enum drbd_repl_state *repl_state = peer_device->repl_state;

			if (peer_device->connection->fencing_policy >= FP_RESOURCE &&
			    !(role[OLD] == R_PRIMARY && repl_state[OLD] < L_ESTABLISHED && !(peer_disk_state[OLD] <= D_OUTDATED)) &&
			     (role[NEW] == R_PRIMARY && repl_state[NEW] < L_ESTABLISHED && !(peer_disk_state[NEW] <= D_OUTDATED)))
				return SS_PRIMARY_NOP;

			if (!(repl_state[OLD] > L_ESTABLISHED && disk_state[OLD] < D_INCONSISTENT) &&
			     (repl_state[NEW] > L_ESTABLISHED && disk_state[NEW] < D_INCONSISTENT))
				return SS_NO_LOCAL_DISK;

			if (!(repl_state[OLD] > L_ESTABLISHED && peer_disk_state[OLD] < D_INCONSISTENT) &&
			     (repl_state[NEW] > L_ESTABLISHED && peer_disk_state[NEW] < D_INCONSISTENT))
				return SS_NO_REMOTE_DISK;

			if (!(repl_state[OLD] > L_ESTABLISHED && disk_state[OLD] < D_UP_TO_DATE && peer_disk_state[OLD] < D_UP_TO_DATE) &&
			     (repl_state[NEW] > L_ESTABLISHED && disk_state[NEW] < D_UP_TO_DATE && peer_disk_state[NEW] < D_UP_TO_DATE))
				return SS_NO_UP_TO_DATE_DISK;

			if (!(disk_state[OLD] == D_OUTDATED && !local_disk_may_be_outdated(device, OLD)) &&
			    (disk_state[NEW] == D_OUTDATED && !local_disk_may_be_outdated(device, NEW)))
				return SS_CONNECTED_OUTDATES;

			if (!(repl_state[OLD] == L_VERIFY_S || repl_state[OLD] == L_VERIFY_T) &&
			     (repl_state[NEW] == L_VERIFY_S || repl_state[NEW] == L_VERIFY_T)) {
				struct net_conf *nc = rcu_dereference(peer_device->connection->net_conf);

				if (!nc || nc->verify_alg[0] == 0)
					return SS_NO_VERIFY_ALG;
			}

			if (!(repl_state[OLD] == L_VERIFY_S || repl_state[OLD] == L_VERIFY_T) &&
			     (repl_state[NEW] == L_VERIFY_S || repl_state[NEW] == L_VERIFY_T) &&
				  peer_device->connection->agreed_pro_version < 88)
				return SS_NOT_SUPPORTED;

			if (!(repl_state[OLD] >= L_ESTABLISHED && peer_disk_state[OLD] == D_UNKNOWN) &&
			     (repl_state[NEW] >= L_ESTABLISHED && peer_disk_state[NEW] == D_UNKNOWN))
				return SS_CONNECTED_OUTDATES;

			if (repl_state[NEW] != repl_state[OLD] &&
			    (repl_state[NEW] == L_STARTING_SYNC_T || repl_state[NEW] == L_STARTING_SYNC_S) &&
			    repl_state[OLD] > L_ESTABLISHED )
				return SS_RESYNC_RUNNING;

			/* if (repl_state[NEW] == repl_state[OLD] && repl_state[NEW] == L_OFF)
				return SS_IN_TRANSIENT_STATE; */

			if ((repl_state[NEW] == L_VERIFY_S || repl_state[NEW] == L_VERIFY_T) && repl_state[OLD] < L_ESTABLISHED)
				return SS_NEED_CONNECTION;

			if ((repl_state[NEW] == L_VERIFY_S || repl_state[NEW] == L_VERIFY_T) &&
			    repl_state[NEW] != repl_state[OLD] && repl_state[OLD] > L_ESTABLISHED)
				return SS_RESYNC_RUNNING;

			if ((repl_state[NEW] == L_STARTING_SYNC_S || repl_state[NEW] == L_STARTING_SYNC_T) &&
			    repl_state[OLD] < L_ESTABLISHED)
				return SS_NEED_CONNECTION;

			if ((repl_state[NEW] == L_SYNC_TARGET || repl_state[NEW] == L_SYNC_SOURCE)
			    && repl_state[OLD] < L_OFF)
				return SS_NEED_CONNECTION; /* No NetworkFailure -> SyncTarget etc... */
		}
	}

	return SS_SUCCESS;
}

/**
 * is_valid_soft_transition() - Returns an SS_ error code if state[NEW] is not valid
 *
 * "Soft" transitions are voluntary state changes which drbd may decline, such
 * as a user request to promote a resource to primary.  Opposed to that are
 * involuntary or "hard" transitions like a network connection loss.
 *
 * When deciding if a "soft" transition should be allowed, "hard" transitions
 * may already have forced the resource into a critical state.  It may take
 * several "soft" transitions to get the resource back to normal.  To allow
 * those, rather than checking if the desired new state is valid, we can only
 * check if the desired new state is "at least as good" as the current state.
 */
static enum drbd_state_rv is_valid_soft_transition(struct drbd_resource *resource)
{
	enum drbd_state_rv rv;

	rcu_read_lock();
	rv = __is_valid_soft_transition(resource);
	rcu_read_unlock();

	return rv;
}

STATIC enum drbd_state_rv
is_valid_conn_transition(enum drbd_conn_state oc, enum drbd_conn_state nc)
{
	/* no change -> nothing to do, at least for the connection part */
	if (oc == nc)
		return SS_NOTHING_TO_DO;

	/* disconnect of an unconfigured connection does not make sense */
	if (oc == C_STANDALONE && nc == C_DISCONNECTING)
		return SS_ALREADY_STANDALONE;

	/* from C_STANDALONE, we start with C_UNCONNECTED */
	if (oc == C_STANDALONE && nc != C_UNCONNECTED)
		return SS_NEED_CONNECTION;

	/* After a network error only C_UNCONNECTED or C_DISCONNECTING may follow. */
	if (oc >= C_TIMEOUT && oc <= C_TEAR_DOWN && nc != C_UNCONNECTED && nc != C_DISCONNECTING)
		return SS_IN_TRANSIENT_STATE;

	/* After C_DISCONNECTING only C_STANDALONE may follow */
	if (oc == C_DISCONNECTING && nc != C_STANDALONE)
		return SS_IN_TRANSIENT_STATE;

	return SS_SUCCESS;
}


/**
 * is_valid_transition() - Returns an SS_ error code if the state transition is not possible
 * This limits hard state transitions. Hard state transitions are facts there are
 * imposed on DRBD by the environment. E.g. disk broke or network broke down.
 * But those hard state transitions are still not allowed to do everything.
 */
static enum drbd_state_rv is_valid_transition(struct drbd_resource *resource)
{
	enum drbd_state_rv rv;
	struct drbd_connection *connection;
	struct drbd_device *device;
	struct drbd_peer_device *peer_device;
	int vnr;

	for_each_connection(connection, resource) {
		rv = is_valid_conn_transition(connection->cstate[OLD], connection->cstate[NEW]);
		if (rv < SS_SUCCESS)
			return rv;

		idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
			/* When establishing a connection we need to go through C_CONNECTED!
			   Necessary to do the right thing upon invalidate-remote on a disconnected
			   resource */
			if (connection->cstate[OLD] < C_CONNECTED &&
			    peer_device->repl_state[NEW] >= L_ESTABLISHED)
				return SS_NEED_CONNECTION;
		}
	}

	idr_for_each_entry(&resource->devices, device, vnr) {
		/* we cannot fail (again) if we already detached */
		if (device->disk_state[NEW] == D_FAILED && device->disk_state[OLD] == D_DISKLESS) {
			return SS_IS_DISKLESS;
		}
	}

	return SS_SUCCESS;
}

bool drbd_calc_weak(struct drbd_resource *resource)
{
	enum drbd_role *role = resource->role;
	struct drbd_connection *connection;
	u64 primaries = 0, direct_primaries = 0, direct_secondaries = 0;

	for_each_connection(connection, resource) {
		if (connection->cstate[NEW] >= C_CONNECTED) {
			const int peer_node_id = connection->net_conf->peer_node_id;
			const u64 peer_node_mask = NODE_MASK(peer_node_id);
			primaries |= connection->primary_mask;
			if (peer_node_mask & connection->primary_mask)
				direct_primaries |= peer_node_mask;
			else
				direct_secondaries |= peer_node_mask;
		}
	}

	/* We might see temporarily false primaries because P_PRI_REACHABLE packets
	   come in in arbitrary order. If a directly connected neighbor considers
	   himself as secondary, trust that more than what other nodes say. */
	primaries = primaries & ~direct_secondaries;

	return role[NEW] == R_SECONDARY && primaries && primaries != direct_primaries;
}

static void sanitize_state(struct drbd_resource *resource)
{
	enum drbd_role *role = resource->role;
	bool *weak = resource->weak;
	struct drbd_connection *connection;
	struct drbd_device *device;
	bool primary_visible = false;
	int vnr;

	rcu_read_lock();
	for_each_connection(connection, resource) {
		enum drbd_conn_state *cstate = connection->cstate;

		if (cstate[NEW] < C_CONNECTED) {
			connection->peer_role[NEW] = R_UNKNOWN;
			if (connection->primary_mask > 0)
				weak[NEW] = drbd_calc_weak(resource);
				/* One might be tempted to reset primary_mask to 0 here, but
				   it is not clear if this state transition will be committed.
				   The primary_mask gets set to 0 in finish_state_change() */
		}

		if (connection->peer_role[NEW] == R_PRIMARY)
			primary_visible = true;
	}

	idr_for_each_entry(&resource->devices, device, vnr) {
		struct drbd_peer_device *peer_device;
		enum drbd_disk_state *disk_state = device->disk_state;
		bool lost_connection = false;
		int good_data_count[2] = { };

		if ((resource->state_change_flags & CS_IGN_OUTD_FAIL) &&
		    disk_state[OLD] < D_OUTDATED && disk_state[NEW] == D_OUTDATED)
			disk_state[NEW] = disk_state[OLD];

		for_each_peer_device(peer_device, device) {
			enum drbd_repl_state *repl_state = peer_device->repl_state;
			enum drbd_disk_state *peer_disk_state = peer_device->disk_state;
			struct drbd_connection *connection = peer_device->connection;
			enum drbd_conn_state *cstate = connection->cstate;
			bool *peer_weak = connection->peer_weak;
			enum drbd_disk_state min_disk_state, max_disk_state;
			enum drbd_disk_state min_peer_disk_state, max_peer_disk_state;

			if (repl_state[NEW] < L_ESTABLISHED) {
				peer_device->resync_susp_peer[NEW] = false;
				if (peer_disk_state[NEW] > D_UNKNOWN ||
				    peer_disk_state[NEW] < D_INCONSISTENT)
					peer_disk_state[NEW] = D_UNKNOWN;
			}
			if (repl_state[OLD] >= L_ESTABLISHED && repl_state[NEW] < L_ESTABLISHED)
				lost_connection = true;

			/* Clear the aftr_isp when becoming unconfigured */
			if (cstate[NEW] == C_STANDALONE &&
			    disk_state[NEW] == D_DISKLESS &&
			    role[NEW] == R_SECONDARY)
				peer_device->resync_susp_dependency[NEW] = false;

			/* Abort resync if a disk fails/detaches */
			if (repl_state[NEW] > L_ESTABLISHED &&
			    (disk_state[NEW] <= D_FAILED ||
			     peer_disk_state[NEW] <= D_FAILED))
				repl_state[NEW] = L_ESTABLISHED;

			/* D_CONSISTENT and D_OUTDATED vanish when we get connected */
			if (repl_state[NEW] >= L_ESTABLISHED && repl_state[NEW] < L_AHEAD) {
				if (disk_state[NEW] == D_CONSISTENT ||
				    disk_state[NEW] == D_OUTDATED)
					disk_state[NEW] = D_UP_TO_DATE;
				if (peer_disk_state[NEW] == D_CONSISTENT ||
				    peer_disk_state[NEW] == D_OUTDATED)
					peer_disk_state[NEW] = D_UP_TO_DATE;
			}

			if (weak[NEW] && disk_state[NEW] > D_OUTDATED)
				disk_state[NEW] = D_OUTDATED;

			if (peer_weak[NEW] && peer_disk_state[NEW] > D_OUTDATED)
				peer_disk_state[NEW] = D_OUTDATED;

			/* Start to resync from reachable secondaries, if I loose the
			   weak bit and have no primary in reach (== there are no primaries reachable) */
			if (weak[OLD] && !weak[NEW] && !primary_visible &&
			    repl_state[NEW] == L_ESTABLISHED && peer_disk_state[NEW] == D_UP_TO_DATE) {
				const int my_node_id = resource->res_opts.node_id;
				if (peer_device->bitmap_uuids[my_node_id] == drbd_current_uuid(device))
					repl_state[NEW] = L_WF_BITMAP_T;
				else
					drbd_err(peer_device, "ASSERT FAILED "
						 "bitmap of peer (%llX) == my current (%llX)",
						 peer_device->bitmap_uuids[my_node_id],
						 drbd_current_uuid(device));
			}

			/* Implications of the repl state on the disk states */
			min_disk_state = D_DISKLESS;
			max_disk_state = D_UP_TO_DATE;
			min_peer_disk_state = D_INCONSISTENT;
			max_peer_disk_state = D_UNKNOWN;
			switch (repl_state[NEW]) {
			case L_OFF:
				/* values from above */
				break;
			case L_WF_BITMAP_T:
			case L_PAUSED_SYNC_T:
			case L_STARTING_SYNC_T:
			case L_WF_SYNC_UUID:
			case L_BEHIND:
				min_disk_state = D_INCONSISTENT;
				max_disk_state = D_OUTDATED;
				min_peer_disk_state = D_UP_TO_DATE;
				max_peer_disk_state = D_UP_TO_DATE;
				break;
			case L_VERIFY_S:
			case L_VERIFY_T:
				min_disk_state = D_UP_TO_DATE;
				max_disk_state = D_UP_TO_DATE;
				min_peer_disk_state = D_UP_TO_DATE;
				max_peer_disk_state = D_UP_TO_DATE;
				break;
			case L_ESTABLISHED:
				min_disk_state = D_DISKLESS;
				max_disk_state = D_UP_TO_DATE;
				min_peer_disk_state = D_DISKLESS;
				max_peer_disk_state = D_UP_TO_DATE;
				break;
			case L_WF_BITMAP_S:
			case L_PAUSED_SYNC_S:
			case L_STARTING_SYNC_S:
			case L_AHEAD:
				min_disk_state = D_UP_TO_DATE;
				max_disk_state = D_UP_TO_DATE;
				min_peer_disk_state = D_INCONSISTENT;
				max_peer_disk_state = D_CONSISTENT; /* D_OUTDATED would be nice. But explicit outdate necessary*/
				break;
			case L_SYNC_TARGET:
				min_disk_state = D_INCONSISTENT;
				max_disk_state = D_INCONSISTENT;
				min_peer_disk_state = D_UP_TO_DATE;
				max_peer_disk_state = D_UP_TO_DATE;
				break;
			case L_SYNC_SOURCE:
				min_disk_state = D_UP_TO_DATE;
				max_disk_state = D_UP_TO_DATE;
				min_peer_disk_state = D_INCONSISTENT;
				max_peer_disk_state = D_INCONSISTENT;
				break;
			}

			/* Implications of the repl state on the disk states */
			if (disk_state[NEW] > max_disk_state)
				disk_state[NEW] = max_disk_state;

			if (disk_state[NEW] < min_disk_state)
				disk_state[NEW] = min_disk_state;

			if (peer_disk_state[NEW] > max_peer_disk_state)
				peer_disk_state[NEW] = max_peer_disk_state;

			if (peer_disk_state[NEW] < min_peer_disk_state)
				peer_disk_state[NEW] = min_peer_disk_state;

			/* Suspend IO while fence-peer handler runs (peer lost) */
			if (connection->fencing_policy == FP_STONITH &&
			    (role[NEW] == R_PRIMARY &&
			     repl_state[NEW] < L_ESTABLISHED &&
			     peer_disk_state[NEW] == D_UNKNOWN) &&
			    (role[OLD] != R_PRIMARY ||
			     peer_disk_state[OLD] != D_UNKNOWN))
				resource->susp_fen[NEW] = true;

			/* Count access to good data */
			if (peer_disk_state[OLD] == D_UP_TO_DATE)
				++good_data_count[OLD];
			if (peer_disk_state[NEW] == D_UP_TO_DATE)
				++good_data_count[NEW];

			if (resync_suspended(peer_device, NEW)) {
				if (repl_state[NEW] == L_SYNC_SOURCE)
					repl_state[NEW] = L_PAUSED_SYNC_S;
				if (repl_state[NEW] == L_SYNC_TARGET)
					repl_state[NEW] = L_PAUSED_SYNC_T;
			} else {
				if (repl_state[NEW] == L_PAUSED_SYNC_S)
					repl_state[NEW] = L_SYNC_SOURCE;
				if (repl_state[NEW] == L_PAUSED_SYNC_T)
					repl_state[NEW] = L_SYNC_TARGET;
			}

			/* This needs to be after the previous block, since we should not set
			   the bit if we are paused ourself */
			if (repl_state[OLD] != L_SYNC_TARGET && repl_state[NEW] == L_SYNC_TARGET)
				set_resync_susp_other_c(peer_device, true, false);
			if (repl_state[OLD] == L_SYNC_TARGET && repl_state[NEW] != L_SYNC_TARGET)
				set_resync_susp_other_c(peer_device, false, false);

			/* Implication of the repl state on other peer's repl state */
			if (repl_state[OLD] != L_STARTING_SYNC_T && repl_state[NEW] == L_STARTING_SYNC_T)
				set_resync_susp_other_c(peer_device, true, true);
		}
		if (disk_state[OLD] == D_UP_TO_DATE)
			++good_data_count[OLD];
		if (disk_state[NEW] == D_UP_TO_DATE)
			++good_data_count[NEW];

		/* Suspend IO if we have no accessible data available.
		 * Policy may be extended later to be able to suspend
		 * if redundancy falls below a certain level. */
		if (resource->res_opts.on_no_data == OND_SUSPEND_IO &&
		    (role[NEW] == R_PRIMARY && good_data_count[NEW] == 0) &&
		   !(role[OLD] == R_PRIMARY && good_data_count[OLD] == 0))
			resource->susp_nod[NEW] = true;
		if (lost_connection && disk_state[NEW] == D_NEGOTIATING &&
		    get_ldev_if_state(device, D_NEGOTIATING)) {
			disk_state[NEW] = disk_state_from_md(device);
			put_ldev(device);
		}
	}
	rcu_read_unlock();
}

void drbd_resume_al(struct drbd_device *device)
{
	if (test_and_clear_bit(AL_SUSPENDED, &device->flags))
		drbd_info(device, "Resumed AL updates\n");
}

static void set_ov_position(struct drbd_peer_device *peer_device,
			    enum drbd_repl_state repl_state)
{
	struct drbd_device *device = peer_device->device;
	if (peer_device->connection->agreed_pro_version < 90)
		peer_device->ov_start_sector = 0;
	peer_device->rs_total = drbd_bm_bits(device);
	peer_device->ov_position = 0;
	if (repl_state == L_VERIFY_T) {
		/* starting online verify from an arbitrary position
		 * does not fit well into the existing protocol.
		 * on L_VERIFY_T, we initialize ov_left and friends
		 * implicitly in receive_DataRequest once the
		 * first P_OV_REQUEST is received */
		peer_device->ov_start_sector = ~(sector_t)0;
	} else {
		unsigned long bit = BM_SECT_TO_BIT(peer_device->ov_start_sector);
		if (bit >= peer_device->rs_total) {
			peer_device->ov_start_sector =
				BM_BIT_TO_SECT(peer_device->rs_total - 1);
			peer_device->rs_total = 1;
		} else
			peer_device->rs_total -= bit;
		peer_device->ov_position = peer_device->ov_start_sector;
	}
	peer_device->ov_left = peer_device->rs_total;
}

static void queue_after_state_change_work(struct drbd_resource *resource,
					  struct completion *done, gfp_t gfp)
{
	struct after_state_change_work *work;

	work = kmalloc(sizeof(*work), gfp);
	if (work) {
		work->state_change = remember_state_change(resource, gfp);
		work->id = atomic_inc_return(&drbd_notify_id);
	}
	if (work && work->state_change) {
		work->w.cb = w_after_state_change;
		work->done = done;
		drbd_queue_work(&resource->work, &work->w);
	} else {
		if (work)
			forget_state_change(work->state_change);
		drbd_err(resource, "Could not allocate after state change work\n");
	}
}

static void initialize_resync(struct drbd_peer_device *peer_device)
{
	unsigned long tw = drbd_bm_total_weight(peer_device);
	unsigned long now = jiffies;
	int i;

	peer_device->rs_failed = 0;
	peer_device->rs_paused = 0;
	peer_device->rs_same_csum = 0;
	peer_device->rs_last_events = 0;
	peer_device->rs_last_sect_ev = 0;
	peer_device->rs_total = tw;
	peer_device->rs_start = now;
	for (i = 0; i < DRBD_SYNC_MARKS; i++) {
		peer_device->rs_mark_left[i] = tw;
		peer_device->rs_mark_time[i] = now;
	}

	drbd_rs_controller_reset(peer_device);
}

/**
 * finish_state_change  -  carry out actions triggered by a state change
 */
static void finish_state_change(struct drbd_resource *resource, struct completion *done)
{
	enum drbd_role *role = resource->role;
	struct drbd_device *device;
	struct drbd_connection *connection;
	bool *weak = resource->weak;
	int vnr;

	print_state_change(resource, "");

	if (role[OLD] == R_PRIMARY && role[NEW] == R_SECONDARY && resource->peer_ack_req) {
		resource->last_peer_acked_dagtag = resource->peer_ack_req->dagtag_sector;
		drbd_queue_peer_ack(resource->peer_ack_req);
		resource->peer_ack_req = NULL;
	}

	idr_for_each_entry(&resource->devices, device, vnr) {
		enum drbd_disk_state *disk_state = device->disk_state;
		struct drbd_peer_device *peer_device;

		/* if we are going -> D_FAILED or D_DISKLESS, grab one extra reference
		 * on the ldev here, to be sure the transition -> D_DISKLESS resp.
		 * drbd_ldev_destroy() won't happen before our corresponding
		 * w_after_state_change works run, where we put_ldev again. */
		if ((disk_state[OLD] != D_FAILED && disk_state[NEW] == D_FAILED) ||
		    (disk_state[OLD] != D_DISKLESS && disk_state[NEW] == D_DISKLESS))
			atomic_inc(&device->local_cnt);

		if (disk_state[OLD] == D_ATTACHING && disk_state[NEW] >= D_NEGOTIATING)
			drbd_info(device, "attached to current UUID: %016llX\n", device->ldev->md.current_uuid);

		for_each_peer_device(peer_device, device) {
			enum drbd_repl_state *repl_state = peer_device->repl_state;
			struct drbd_connection *connection = peer_device->connection;

			/* Wake up role changes, that were delayed because of connection establishing */
			if (repl_state[OLD] == L_OFF && repl_state[NEW] != L_OFF &&
			    all_peer_devices_connected(connection))
				clear_bit(INITIAL_STATE_SENT, &connection->flags);
		}

		wake_up(&device->misc_wait);
		wake_up(&device->resource->state_wait);

		for_each_peer_device(peer_device, device) {
			enum drbd_repl_state *repl_state = peer_device->repl_state;

			if (repl_state[OLD] == L_SYNC_TARGET &&
			    !(repl_state[NEW] == L_SYNC_TARGET || repl_state[NEW] == L_PAUSED_SYNC_T))
				clear_bit(WEAK_WHILE_RESYNC, &device->flags);
		}

		for_each_peer_device(peer_device, device) {
			enum drbd_repl_state *repl_state = peer_device->repl_state;
			enum drbd_disk_state *peer_disk_state = peer_device->disk_state;
			struct drbd_connection *connection = peer_device->connection;
			enum drbd_role *peer_role = connection->peer_role;

			if (weak[OLD] && !weak[NEW] && repl_state[NEW] == L_WF_BITMAP_T)
				drbd_info(peer_device, "Resync because leaving weak state\n");

			if (resource->weak[NEW] &&
			    (repl_state[NEW] == L_SYNC_TARGET || repl_state[NEW] == L_PAUSED_SYNC_T))
				set_bit(WEAK_WHILE_RESYNC, &device->flags);

			/* Aborted verify run, or we reached the stop sector.
			 * Log the last position, unless end-of-device. */
			if ((repl_state[OLD] == L_VERIFY_S || repl_state[OLD] == L_VERIFY_T) &&
			    repl_state[NEW] <= L_ESTABLISHED) {
				peer_device->ov_start_sector =
					BM_BIT_TO_SECT(drbd_bm_bits(device) - peer_device->ov_left);
				if (peer_device->ov_left)
					drbd_info(peer_device, "Online Verify reached sector %llu\n",
						  (unsigned long long)peer_device->ov_start_sector);
			}

			if ((repl_state[OLD] == L_PAUSED_SYNC_T || repl_state[OLD] == L_PAUSED_SYNC_S) &&
			    (repl_state[NEW] == L_SYNC_TARGET  || repl_state[NEW] == L_SYNC_SOURCE)) {
				drbd_info(peer_device, "Syncer continues.\n");
				peer_device->rs_paused += (long)jiffies
						  -(long)peer_device->rs_mark_time[peer_device->rs_last_mark];
				if (repl_state[NEW] == L_SYNC_TARGET)
					mod_timer(&peer_device->resync_timer, jiffies);

				device->bm_resync_fo &= ~BM_BLOCKS_PER_BM_EXT_MASK;
				/* Setting the find_offset back is necessary when switching resync from
				   one peer to the other. Since in the bitmap of the new peer, there
				   might be bits before the current find_offset. Since the peer is
				   notified about the resync progress in BM_EXT sized chunks. */
			}

			if ((repl_state[OLD] == L_SYNC_TARGET  || repl_state[OLD] == L_SYNC_SOURCE) &&
			    (repl_state[NEW] == L_PAUSED_SYNC_T || repl_state[NEW] == L_PAUSED_SYNC_S)) {
				drbd_info(peer_device, "Resync suspended\n");
				peer_device->rs_mark_time[peer_device->rs_last_mark] = jiffies;
			}


			if (repl_state[OLD] > L_ESTABLISHED && repl_state[NEW] <= L_ESTABLISHED) {
				clear_bit(RECONCILIATION_RESYNC, &peer_device->flags);
				peer_device->max_size = 0;
			}

			if (repl_state[OLD] == L_ESTABLISHED &&
			    (repl_state[NEW] == L_VERIFY_S || repl_state[NEW] == L_VERIFY_T)) {
				unsigned long now = jiffies;
				int i;

				set_ov_position(peer_device, repl_state[NEW]);
				peer_device->rs_start = now;
				peer_device->rs_last_events = 0;
				peer_device->rs_last_sect_ev = 0;
				peer_device->ov_last_oos_size = 0;
				peer_device->ov_last_oos_start = 0;

				for (i = 0; i < DRBD_SYNC_MARKS; i++) {
					peer_device->rs_mark_left[i] = peer_device->ov_left;
					peer_device->rs_mark_time[i] = now;
				}

				drbd_rs_controller_reset(peer_device);

				if (repl_state[NEW] == L_VERIFY_S) {
					drbd_info(peer_device, "Starting Online Verify from sector %llu\n",
							(unsigned long long)peer_device->ov_position);
					mod_timer(&peer_device->resync_timer, jiffies);
				}
			} else if (!(repl_state[OLD] >= L_SYNC_SOURCE && repl_state[OLD] <= L_PAUSED_SYNC_T) &&
				   (repl_state[NEW] >= L_SYNC_SOURCE && repl_state[NEW] <= L_PAUSED_SYNC_T)) {
				initialize_resync(peer_device);
			}

			if (disk_state[NEW] != D_NEGOTIATING && get_ldev(device)) {
				if (peer_device->bitmap_index != -1) {
					u32 mdf = device->ldev->md.peers[peer_device->bitmap_index].flags;
					mdf &= ~(MDF_PEER_CONNECTED | MDF_PEER_OUTDATED | MDF_PEER_FENCING);
					if (repl_state[NEW] > L_OFF)
						mdf |= MDF_PEER_CONNECTED;
					if (peer_device->disk_state[NEW] <= D_OUTDATED &&
					    peer_device->disk_state[NEW] >= D_INCONSISTENT)
						mdf |= MDF_PEER_OUTDATED;
					if (peer_device->connection->fencing_policy != FP_DONT_CARE)
						mdf |= MDF_PEER_FENCING;
					if (mdf != device->ldev->md.peers[peer_device->bitmap_index].flags) {
						device->ldev->md.peers[peer_device->bitmap_index].flags = mdf;
						drbd_md_mark_dirty(device);
					}
				}

				/* Peer was forced D_UP_TO_DATE & R_PRIMARY, consider to resync */
				if (disk_state[OLD] == D_INCONSISTENT && peer_disk_state[OLD] == D_INCONSISTENT &&
				    peer_role[OLD] == R_SECONDARY && peer_role[NEW] == R_PRIMARY)
					set_bit(CONSIDER_RESYNC, &peer_device->flags);

				/* Resume AL writing if we get a connection */
				if (repl_state[OLD] < L_ESTABLISHED && repl_state[NEW] >= L_ESTABLISHED)
					drbd_resume_al(device);
				put_ldev(device);
			}
		}

		if (disk_state[NEW] != D_NEGOTIATING && get_ldev(device)) {
			u32 mdf = device->ldev->md.flags & ~(MDF_CONSISTENT|MDF_PRIMARY_IND|
							 MDF_WAS_UP_TO_DATE|MDF_CRASHED_PRIMARY);
			mdf &= ~MDF_AL_CLEAN;
			if (test_bit(CRASHED_PRIMARY, &device->flags))
				mdf |= MDF_CRASHED_PRIMARY;
			if (device->resource->role[NEW] == R_PRIMARY)
				mdf |= MDF_PRIMARY_IND;
			if (disk_state[NEW] > D_INCONSISTENT)
				mdf |= MDF_CONSISTENT;
			if (disk_state[NEW] > D_OUTDATED)
				mdf |= MDF_WAS_UP_TO_DATE;
			if (mdf != device->ldev->md.flags) {
				device->ldev->md.flags = mdf;
				drbd_md_mark_dirty(device);
			}
			if (disk_state[OLD] < D_CONSISTENT && disk_state[NEW] >= D_CONSISTENT)
				drbd_set_exposed_data_uuid(device, device->ldev->md.current_uuid);
			put_ldev(device);
		}

		/* remember last attach time so request_timer_fn() won't
		 * kill newly established sessions while we are still trying to thaw
		 * previously frozen IO */
		if ((disk_state[OLD] == D_ATTACHING || disk_state[OLD] == D_NEGOTIATING) &&
		    disk_state[NEW] > D_NEGOTIATING)
			device->last_reattach_jif = jiffies;
	}

	for_each_connection(connection, resource) {
		enum drbd_conn_state *cstate = connection->cstate;

		wake_up(&connection->ping_wait);

		/* Receiver should clean up itself */
		if (cstate[OLD] != C_DISCONNECTING && cstate[NEW] == C_DISCONNECTING)
			drbd_thread_stop_nowait(&connection->receiver);

		/* Now the receiver finished cleaning up itself, it should die */
		if (cstate[OLD] != C_STANDALONE && cstate[NEW] == C_STANDALONE)
			drbd_thread_stop_nowait(&connection->receiver);

		/* Upon network failure, we need to restart the receiver. */
		if (cstate[OLD] > C_CONNECTING &&
		    cstate[NEW] <= C_TEAR_DOWN && cstate[NEW] >= C_TIMEOUT)
			drbd_thread_restart_nowait(&connection->receiver);

		if (cstate[NEW] < C_CONNECTED) {
			clear_bit(INITIAL_STATE_SENT, &connection->flags);
			clear_bit(INITIAL_STATE_RECEIVED, &connection->flags);
			if (cstate[OLD] >= C_CONNECTED)
				connection->primary_mask = 0;
		}

		/* remember last connect time so request_timer_fn() won't
		 * kill newly established sessions while we are still trying to thaw
		 * previously frozen IO */
		if (cstate[OLD] < C_CONNECTED && cstate[NEW] == C_CONNECTED)
			connection->last_reconnect_jif = jiffies;
	}

	queue_after_state_change_work(resource, done, GFP_ATOMIC);
}

static void abw_start_sync(struct drbd_device *device,
			   struct drbd_peer_device *peer_device, int rv)
{
	struct drbd_peer_device *pd;

	if (rv) {
		drbd_err(device, "Writing the bitmap failed not starting resync.\n");
		stable_change_repl_state(peer_device, L_ESTABLISHED, CS_VERBOSE);
		return;
	}

	switch (peer_device->repl_state[NOW]) {
	case L_STARTING_SYNC_T:
		/* Since the number of set bits changed and the other peer_devices are
		   lready in L_PAUSED_SYNC_T state, we need to set rs_total here */
		for_each_peer_device(pd, device)
			initialize_resync(pd);

		if (peer_device->connection->agreed_pro_version < 110)
			stable_change_repl_state(peer_device, L_WF_SYNC_UUID, CS_VERBOSE);
		else
			drbd_start_resync(peer_device, L_SYNC_TARGET);
		break;
	case L_STARTING_SYNC_S:
		drbd_start_resync(peer_device, L_SYNC_SOURCE);
		break;
	default:
		break;
	}
}

int drbd_bitmap_io_from_worker(struct drbd_device *device,
		int (*io_fn)(struct drbd_device *, struct drbd_peer_device *),
		char *why, enum bm_flag flags,
		struct drbd_peer_device *peer_device)
{
	int rv;

	D_ASSERT(device, current == device->resource->worker.task);

	/* open coded non-blocking drbd_suspend_io(device); */
	atomic_inc(&device->suspend_cnt);

	if (flags & BM_LOCK_SINGLE_SLOT)
		drbd_bm_slot_lock(peer_device, why, flags);
	else
		drbd_bm_lock(device, why, flags);
	rv = io_fn(device, peer_device);
	if (flags & BM_LOCK_SINGLE_SLOT)
		drbd_bm_slot_unlock(peer_device);
	else
		drbd_bm_unlock(device);

	drbd_resume_io(device);

	return rv;
}

static union drbd_state state_change_word(struct drbd_state_change *state_change,
					  unsigned int n_device, int n_connection,
					  enum which_state which)
{
	struct drbd_resource_state_change *resource_state_change =
		&state_change->resource[0];
	struct drbd_device_state_change *device_state_change =
		&state_change->devices[n_device];
	union drbd_state state = { {
		.role = R_UNKNOWN,
		.peer = R_UNKNOWN,
		.conn = C_STANDALONE,
		.disk = D_UNKNOWN,
		.pdsk = D_UNKNOWN,
	} };

	state.role = resource_state_change->role[which];
	state.susp = resource_state_change->susp[which];
	state.susp_nod = resource_state_change->susp_nod[which];
	state.susp_fen = resource_state_change->susp_fen[which];
	state.weak = resource_state_change->weak[which];
	state.disk = device_state_change->disk_state[which];
	if (n_connection != -1) {
		struct drbd_connection_state_change *connection_state_change =
			&state_change->connections[n_connection];
		struct drbd_peer_device_state_change *peer_device_state_change =
			&state_change->peer_devices[n_device * state_change->n_connections + n_connection];

		state.peer = connection_state_change->peer_role[which];
		state.conn = peer_device_state_change->repl_state[which];
		if (state.conn <= L_OFF)
			state.conn = connection_state_change->cstate[which];
		state.pdsk = peer_device_state_change->disk_state[which];
		state.aftr_isp = peer_device_state_change->resync_susp_dependency[which] ||
			peer_device_state_change->resync_susp_other_c[which];
		state.peer_isp = peer_device_state_change->resync_susp_peer[which];
		state.user_isp = peer_device_state_change->resync_susp_user[which];
	}
	return state;
}

void notify_resource_state_change(struct sk_buff *skb,
				  unsigned int seq,
				  struct drbd_resource_state_change *resource_state_change,
				  enum which_state which, enum drbd_notification_type type,
				  unsigned int id)
{
	struct drbd_resource *resource = resource_state_change->resource;
	struct resource_info resource_info = {
		.res_role = resource_state_change->role[which],
		.res_susp = resource_state_change->susp[which],
		.res_susp_nod = resource_state_change->susp_nod[which],
		.res_susp_fen = resource_state_change->susp_fen[which],
	};

	notify_resource_state(skb, seq, resource, &resource_info, type, id);
}

void notify_connection_state_change(struct sk_buff *skb,
				    unsigned int seq,
				    struct drbd_connection_state_change *connection_state_change,
				    enum which_state which,
				    enum drbd_notification_type type, unsigned int id)
{
	struct drbd_connection *connection = connection_state_change->connection;
	struct connection_info connection_info = {
		.conn_connection_state = connection_state_change->cstate[which],
		.conn_role = connection_state_change->peer_role[which],
	};

	notify_connection_state(skb, seq, connection, &connection_info, type, id);
}

void notify_device_state_change(struct sk_buff *skb,
				unsigned int seq,
				struct drbd_device_state_change *device_state_change,
				enum which_state which,
				enum drbd_notification_type type,
				unsigned int id)
{
	struct drbd_device *device = device_state_change->device;
	struct device_info device_info = {
		.dev_disk_state = device_state_change->disk_state[which],
	};

	notify_device_state(skb, seq, device, &device_info, type, id);
}

void notify_peer_device_state_change(struct sk_buff *skb,
				     unsigned int seq,
				     struct drbd_peer_device_state_change *p,
				     enum which_state which,
				     enum drbd_notification_type type,
				     unsigned int id)
{
	struct drbd_peer_device *peer_device = p->peer_device;
	struct peer_device_info peer_device_info = {
		.peer_repl_state = p->repl_state[which],
		.peer_disk_state = p->disk_state[which],
		.peer_resync_susp_user = p->resync_susp_user[which],
		.peer_resync_susp_peer = p->resync_susp_peer[which],
		.peer_resync_susp_dependency = p->resync_susp_dependency[which] || p->resync_susp_other_c[which],
	};

	notify_peer_device_state(skb, seq, peer_device, &peer_device_info, type, id);
}

static void broadcast_state_change(struct drbd_state_change *state_change, unsigned int id)
{
	struct drbd_resource_state_change *resource_state_change = &state_change->resource[0];
	bool resource_state_has_changed;
	unsigned int n_device, n_connection, n_peer_device, n_peer_devices;
	void (*last_func)(struct sk_buff *, unsigned int, void *, enum which_state,
			  enum drbd_notification_type, unsigned int) = NULL;
	void *uninitialized_var(last_arg);

#define HAS_CHANGED(state) ((state)[OLD] != (state)[NEW])
#define FINAL_STATE_CHANGE(type, id) \
	({ if (last_func) \
		last_func(NULL, 0, last_arg, NEW, type, id); \
	})
#define REMEMBER_STATE_CHANGE(func, arg, type, id) \
	({ FINAL_STATE_CHANGE(type | NOTIFY_CONTINUES, id); \
	   last_func = (typeof(last_func))func; \
	   last_arg = arg; \
	 })


	resource_state_has_changed =
	    HAS_CHANGED(resource_state_change->role) ||
	    HAS_CHANGED(resource_state_change->susp) ||
	    HAS_CHANGED(resource_state_change->susp_nod) ||
	    HAS_CHANGED(resource_state_change->susp_fen);

	if (resource_state_has_changed)
		REMEMBER_STATE_CHANGE(notify_resource_state_change,
				      resource_state_change, NOTIFY_CHANGE, id);

	for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
		struct drbd_connection_state_change *connection_state_change =
				&state_change->connections[n_connection];

		if (HAS_CHANGED(connection_state_change->peer_role) ||
		    HAS_CHANGED(connection_state_change->cstate))
			REMEMBER_STATE_CHANGE(notify_connection_state_change,
					      connection_state_change, NOTIFY_CHANGE, id);
	}

	for (n_device = 0; n_device < state_change->n_devices; n_device++) {
		struct drbd_device_state_change *device_state_change =
			&state_change->devices[n_device];

		if (HAS_CHANGED(device_state_change->disk_state))
			REMEMBER_STATE_CHANGE(notify_device_state_change,
					      device_state_change, NOTIFY_CHANGE, id);
	}

	n_peer_devices = state_change->n_devices * state_change->n_connections;
	for (n_peer_device = 0; n_peer_device < n_peer_devices; n_peer_device++) {
		struct drbd_peer_device_state_change *p =
			&state_change->peer_devices[n_peer_device];

		if (HAS_CHANGED(p->disk_state) ||
		    HAS_CHANGED(p->repl_state) ||
		    HAS_CHANGED(p->resync_susp_user) ||
		    HAS_CHANGED(p->resync_susp_peer) ||
		    HAS_CHANGED(p->resync_susp_dependency) ||
		    HAS_CHANGED(p->resync_susp_other_c))
			REMEMBER_STATE_CHANGE(notify_peer_device_state_change,
					      p, NOTIFY_CHANGE, id);
	}

	FINAL_STATE_CHANGE(NOTIFY_CHANGE, id);

#undef HAS_CHANGED
#undef FINAL_STATE_CHANGE
#undef REMEMBER_STATE_CHANGE
}

static void send_new_state_to_all_peer_devices(struct drbd_state_change *state_change, unsigned int n_device)
{
	unsigned int n_connection;

	for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
		struct drbd_peer_device_state_change *peer_device_state_change =
			&state_change->peer_devices[n_device * state_change->n_connections + n_connection];
		struct drbd_peer_device *peer_device = peer_device_state_change->peer_device;
		union drbd_state new_state = state_change_word(state_change, n_device, n_connection, NEW);

		if (new_state.conn >= C_CONNECTED)
			drbd_send_state(peer_device, new_state);
	}
}

static void notify_peers_lost_primary(struct drbd_connection *lost_peer)
{
	struct drbd_resource *resource = lost_peer->resource;
	struct drbd_connection *connection;

	for_each_connection(connection, resource) {
		if (connection == lost_peer)
			continue;
		if (connection->cstate[NOW] == C_CONNECTED) {
			struct drbd_peer_device *peer_device;
			int vnr;

			idr_for_each_entry(&connection->peer_devices, peer_device, vnr)
				drbd_send_current_uuid(peer_device, drbd_current_uuid(peer_device->device));

			drbd_send_peer_dagtag(connection, lost_peer);
		}
	}
}

/*
 * Perform after state change actions that may sleep.
 */
STATIC int w_after_state_change(struct drbd_work *w, int unused)
{
	struct after_state_change_work *work =
		container_of(w, struct after_state_change_work, w);
	struct drbd_state_change *state_change = work->state_change;
	struct drbd_resource_state_change *resource_state_change = &state_change->resource[0];
	struct drbd_resource *resource = resource_state_change->resource;
	enum drbd_role *role = resource_state_change->role;
	bool *susp_nod = resource_state_change->susp_nod;
	bool *susp_fen = resource_state_change->susp_fen;
	bool *weak = resource_state_change->weak;
	int n_device, n_connection;
	bool still_connected = false;

	broadcast_state_change(state_change, work->id);

	for (n_device = 0; n_device < state_change->n_devices; n_device++) {
		struct drbd_device_state_change *device_state_change = &state_change->devices[n_device];
		struct drbd_device *device = device_state_change->device;
		enum drbd_disk_state *disk_state = device_state_change->disk_state;
		bool effective_disk_size_determined = false;
		bool one_peer_disk_up_to_date[2] = { };

		if (disk_state[NEW] == D_UP_TO_DATE)
			effective_disk_size_determined = true;

		for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
			struct drbd_peer_device_state_change *peer_device_state_change =
				&state_change->peer_devices[
					n_device * state_change->n_connections + n_connection];
			enum drbd_disk_state *peer_disk_state = peer_device_state_change->disk_state;
			enum which_state which;

			for (which = OLD; which <= NEW; which++) {
				if (peer_disk_state[which] == D_UP_TO_DATE)
					one_peer_disk_up_to_date[which] = true;
			}
		}

		for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
			struct drbd_connection_state_change *connection_state_change = &state_change->connections[n_connection];
			struct drbd_connection *connection = connection_state_change->connection;
			enum drbd_conn_state *cstate = connection_state_change->cstate;
			enum drbd_role *peer_role = connection_state_change->peer_role;
			struct drbd_peer_device_state_change *peer_device_state_change =
				&state_change->peer_devices[
					n_device * state_change->n_connections + n_connection];
			struct drbd_peer_device *peer_device = peer_device_state_change->peer_device;
			enum drbd_repl_state *repl_state = peer_device_state_change->repl_state;
			enum drbd_disk_state *peer_disk_state = peer_device_state_change->disk_state;
			bool *resync_susp_user = peer_device_state_change->resync_susp_user;
			bool *resync_susp_peer = peer_device_state_change->resync_susp_peer;
			bool *resync_susp_dependency = peer_device_state_change->resync_susp_dependency;
			bool *resync_susp_other_c = peer_device_state_change->resync_susp_other_c;
			union drbd_state new_state =
				state_change_word(state_change, n_device, n_connection, NEW);
			bool create_new_uuid;
			bool send_state = false;

			if (peer_disk_state[NEW] == D_UP_TO_DATE)
				effective_disk_size_determined = true;

			if (repl_state[OLD] != L_ESTABLISHED && repl_state[NEW] == L_ESTABLISHED) {
				clear_bit(CRASHED_PRIMARY, &device->flags);
				if (peer_device->uuids_received)
					peer_device->uuid_flags &= ~((u64)UUID_FLAG_CRASHED_PRIMARY);
			}

			if (!(role[OLD] == R_PRIMARY && disk_state[OLD] < D_UP_TO_DATE && !one_peer_disk_up_to_date[OLD]) &&
			     (role[NEW] == R_PRIMARY && disk_state[NEW] < D_UP_TO_DATE && !one_peer_disk_up_to_date[NEW]))
				drbd_khelper(device, connection, "pri-on-incon-degr");

			if (susp_nod[NEW]) {
				enum drbd_req_event what = NOTHING;

				if (repl_state[OLD] < L_ESTABLISHED &&
				    conn_lowest_repl_state(connection) >= L_ESTABLISHED)
					what = RESEND;

#if 0
/* FIXME currently broken.
 * RESTART_FROZEN_DISK_IO may need a (temporary?) dedicated kernel thread */
				if ((disk_state[OLD] == D_ATTACHING || disk_state[OLD] == D_NEGOTIATING) &&
				    conn_lowest_disk(connection) > D_NEGOTIATING)
					what = RESTART_FROZEN_DISK_IO;
#endif

				if (what != NOTHING) {
					unsigned long irq_flags;

					/* Is this too early?  We should only
					 * resume after the iteration over all
					 * connections?
					 */
					begin_state_change(resource, &irq_flags, CS_VERBOSE);
					if (what == RESEND)
						connection->todo.req_next = TL_NEXT_REQUEST_RESEND;
					__change_io_susp_no_data(resource, false);
					end_state_change(resource, &irq_flags);
				}
			}

			/* Became sync source.  With protocol >= 96, we still need to send out
			 * the sync uuid now. Need to do that before any drbd_send_state, or
			 * the other side may go "paused sync" before receiving the sync uuids,
			 * which is unexpected. */
			if (!(repl_state[OLD] == L_SYNC_SOURCE || repl_state[OLD] == L_PAUSED_SYNC_S) &&
			     (repl_state[NEW] == L_SYNC_SOURCE || repl_state[NEW] == L_PAUSED_SYNC_S) &&
			    connection->agreed_pro_version >= 96 && connection->agreed_pro_version < 110 &&
			    get_ldev(device)) {
				drbd_gen_and_send_sync_uuid(peer_device);
				put_ldev(device);
			}

			/* Do not change the order of the if above and the two below... */
			if (peer_disk_state[OLD] == D_DISKLESS &&
			    peer_disk_state[NEW] > D_DISKLESS && peer_disk_state[NEW] != D_UNKNOWN) {      /* attach on the peer */
				/* we probably will start a resync soon.
				 * make sure those things are properly reset. */
				peer_device->rs_total = 0;
				peer_device->rs_failed = 0;
				atomic_set(&peer_device->rs_pending_cnt, 0);
				drbd_rs_cancel_all(peer_device);

				drbd_send_uuids(peer_device, 0, 0);
				drbd_send_state(peer_device, new_state);
			}
			/* No point in queuing send_bitmap if we don't have a connection
			 * anymore, so check also the _current_ state, not only the new state
			 * at the time this work was queued. */
			if (repl_state[OLD] != L_WF_BITMAP_S && repl_state[NEW] == L_WF_BITMAP_S &&
			    peer_device->repl_state[NOW] == L_WF_BITMAP_S)
				drbd_queue_bitmap_io(device, &drbd_send_bitmap, NULL,
						"send_bitmap (WFBitMapS)",
						BM_LOCK_SET | BM_LOCK_CLEAR | BM_LOCK_BULK | BM_LOCK_SINGLE_SLOT,
						peer_device);

			create_new_uuid = false;
			/* Lost contact to peer's copy of the data */
			if (!(peer_disk_state[OLD] < D_INCONSISTENT ||
			      peer_disk_state[OLD] == D_UNKNOWN ||
			      peer_disk_state[OLD] == D_OUTDATED) &&
			    (peer_disk_state[NEW] < D_INCONSISTENT ||
			     peer_disk_state[NEW] == D_UNKNOWN ||
			     peer_disk_state[NEW] == D_OUTDATED)) {

				if (role[NEW] == R_PRIMARY)
					create_new_uuid = true;

				if (connection->agreed_pro_version < 110 &&
				    peer_role[NEW] == R_PRIMARY &&
				    disk_state[NEW] >= D_UP_TO_DATE)
					create_new_uuid = true;
			}

			if (create_new_uuid && get_ldev(device)) {
				if (drbd_suspended(device))
					set_bit(NEW_CUR_UUID, &device->flags);
				else
					drbd_uuid_new_current(device);
				put_ldev(device);
			}

			if (peer_disk_state[NEW] < D_INCONSISTENT && get_ldev(device)) {
				/* D_DISKLESS Peer becomes secondary */
				if (peer_role[OLD] == R_PRIMARY && peer_role[NEW] == R_SECONDARY)
					/* We may still be Primary ourselves.
					 * No harm done if the bitmap still changes,
					 * redirtied pages will follow later. */
					drbd_bitmap_io_from_worker(device, &drbd_bm_write,
						"demote diskless peer", BM_LOCK_CLEAR | BM_LOCK_BULK,
						NULL);
				put_ldev(device);
			}

			/* Write out all changed bits on demote.
			 * Though, no need to da that just yet
			 * if there is a resync going on still */
			if (role[OLD] == R_PRIMARY && role[NEW] == R_SECONDARY &&
				peer_device->repl_state[NOW] <= L_ESTABLISHED && get_ldev(device)) {
				/* No changes to the bitmap expected this time, so assert that,
				 * even though no harm was done if it did change. */
				drbd_bitmap_io_from_worker(device, &drbd_bm_write,
						"demote", BM_LOCK_SET | BM_LOCK_CLEAR | BM_LOCK_BULK,
						NULL);
				put_ldev(device);
			}

			/* Last part of the attaching process ... */
			if (repl_state[NEW] >= L_ESTABLISHED &&
			    disk_state[OLD] == D_ATTACHING && disk_state[NEW] == D_NEGOTIATING) {
				drbd_send_sizes(peer_device, 0, 0);  /* to start sync... */
				drbd_send_uuids(peer_device, 0, 0);
				drbd_send_state(peer_device, new_state);
			}

			/* We want to pause/continue resync, tell peer. */
			if (repl_state[NEW] >= L_ESTABLISHED &&
			     ((resync_susp_dependency[OLD] != resync_susp_dependency[NEW]) ||
			      (resync_susp_other_c[OLD] != resync_susp_other_c[NEW]) ||
			      (resync_susp_user[OLD] != resync_susp_user[NEW])))
				send_state = true;

			/* finished resync, tell sync source */
			if ((repl_state[OLD] == L_SYNC_TARGET || repl_state[OLD] == L_PAUSED_SYNC_T) &&
			    repl_state[NEW] == L_ESTABLISHED)
				send_state = true;

			/* In case one of the isp bits got set, suspend other devices. */
			if (!(resync_susp_dependency[OLD] || resync_susp_peer[OLD] || resync_susp_user[OLD]) &&
			     (resync_susp_dependency[NEW] || resync_susp_peer[NEW] || resync_susp_user[NEW]))
				suspend_other_sg(device);

			/* Make sure the peer gets informed about eventual state
			   changes (ISP bits) while we were in L_OFF. */
			if (repl_state[OLD] == L_OFF && repl_state[NEW] >= L_ESTABLISHED)
				send_state = true;

			if (repl_state[OLD] != L_AHEAD && repl_state[NEW] == L_AHEAD)
				send_state = true;

			if (weak[OLD] && !weak[NEW] && repl_state[NEW] == L_WF_BITMAP_T)
				send_state = true;

			/* We are in the progress to start a full sync. SyncTarget sets all slots. */
			if (repl_state[OLD] != L_STARTING_SYNC_T && repl_state[NEW] == L_STARTING_SYNC_T)
				drbd_queue_bitmap_io(device,
					&drbd_bmio_set_all_n_write, &abw_start_sync,
					"set_n_write from StartingSync",
					BM_LOCK_SET | BM_LOCK_CLEAR | BM_LOCK_BULK,
					peer_device);

			/* We are in the progress to start a full sync. SyncSource one slot. */
			if (repl_state[OLD] != L_STARTING_SYNC_S && repl_state[NEW] == L_STARTING_SYNC_S)
				drbd_queue_bitmap_io(device,
					&drbd_bmio_set_n_write, &abw_start_sync,
					"set_n_write from StartingSync",
					BM_LOCK_SET | BM_LOCK_CLEAR | BM_LOCK_BULK,
					peer_device);

			/* We are invalidating our self... */
			if (repl_state[OLD] < L_ESTABLISHED && repl_state[NEW] < L_ESTABLISHED &&
			    disk_state[OLD] > D_INCONSISTENT && disk_state[NEW] == D_INCONSISTENT)
				/* other bitmap operation expected during this phase */
				drbd_queue_bitmap_io(device, &drbd_bmio_set_n_write, NULL,
					"set_n_write from invalidate", BM_LOCK_ALL,
					peer_device);

			/* Disks got bigger while they were detached */
			if (disk_state[NEW] > D_NEGOTIATING && peer_disk_state[NEW] > D_NEGOTIATING &&
			    test_and_clear_bit(RESYNC_AFTER_NEG, &peer_device->flags)) {
				if (repl_state[NEW] == L_ESTABLISHED)
					resync_after_online_grow(peer_device);
			}

			/* A resync finished or aborted, wake paused devices... */
			if ((repl_state[OLD] > L_ESTABLISHED && repl_state[NEW] <= L_ESTABLISHED) ||
			    (resync_susp_peer[OLD] && !resync_susp_peer[NEW]) ||
			    (resync_susp_user[OLD] && !resync_susp_user[NEW]))
				resume_next_sg(device);

			/* sync target done with resync. Explicitly notify all peers. Our sync
			   source should even know by himself, but the others need that info. */
			if (disk_state[OLD] < D_UP_TO_DATE && repl_state[OLD] >= L_SYNC_SOURCE && repl_state[NEW] == L_ESTABLISHED)
				send_new_state_to_all_peer_devices(state_change, n_device);

			/* Outdated myself because became weak, tell peers */
			if (disk_state[OLD] > D_OUTDATED && disk_state[NEW] == D_OUTDATED &&
			    repl_state[NEW] >= L_ESTABLISHED)
				send_state = true;

			/* This triggers bitmap writeout of potentially still unwritten pages
			 * if the resync finished cleanly, or aborted because of peer disk
			 * failure, or because of connection loss.
			 * For resync aborted because of local disk failure, we cannot do
			 * any bitmap writeout anymore.
			 * No harm done if some bits change during this phase.
			 */
			if (repl_state[OLD] > L_ESTABLISHED && repl_state[NEW] <= L_ESTABLISHED && get_ldev(device)) {
				drbd_queue_bitmap_io(device, &drbd_bm_write_copy_pages, NULL,
					"write from resync_finished", BM_LOCK_BULK,
					NULL);
				put_ldev(device);
			}

			/* Verify finished, or reached stop sector.  Peer did not know about
			 * the stop sector, and we may even have changed the stop sector during
			 * verify to interrupt/stop early.  Send the new state. */
			if (repl_state[OLD] == L_VERIFY_S && repl_state[NEW] == L_ESTABLISHED
			    && verify_can_do_stop_sector(peer_device))
				send_new_state_to_all_peer_devices(state_change, n_device);

			if (disk_state[NEW] == D_DISKLESS &&
			    cstate[NEW] == C_STANDALONE &&
			    role[NEW] == R_SECONDARY) {
				if (resync_susp_dependency[OLD] != resync_susp_dependency[NEW])
					resume_next_sg(device);
			}

			if (send_state)
				drbd_send_state(peer_device, new_state);
		}

		/* Make sure the effective disk size is stored in the metadata
		 * if a local disk is attached and either the local disk state
		 * or a peer disk state is D_UP_TO_DATE.  */
		if (effective_disk_size_determined && get_ldev(device)) {
			sector_t size = drbd_get_capacity(device->this_bdev);
			if (device->ldev->md.effective_size != size) {
				char ppb[10];

				drbd_info(device, "size = %s (%llu KB)\n", ppsize(ppb, size >> 1),
				     (unsigned long long)size >> 1);
				device->ldev->md.effective_size = size;
				drbd_md_mark_dirty(device);
			}
			put_ldev(device);
		}

		/* first half of local IO error, failure to attach,
		 * or administrative detach */
		if (disk_state[OLD] != D_FAILED && disk_state[NEW] == D_FAILED) {
			enum drbd_io_error_p eh = EP_PASS_ON;
			int was_io_error = 0;

			/*
			 * finish_state_change() has grabbed a reference on
			 * ldev in this case.
			 *
			 * our cleanup here with the transition to D_DISKLESS.
			 * But is is still not save to dreference ldev here, since
			 * we might come from an failed Attach before ldev was set. */
			if (device->ldev) {
				rcu_read_lock();
				eh = rcu_dereference(device->ldev->disk_conf)->on_io_error;
				rcu_read_unlock();

				was_io_error = test_and_clear_bit(WAS_IO_ERROR, &device->flags);

				if (was_io_error && eh == EP_CALL_HELPER)
					drbd_khelper(device, NULL, "local-io-error");

				/* Immediately allow completion of all application IO,
				 * that waits for completion from the local disk,
				 * if this was a force-detach due to disk_timeout
				 * or administrator request (drbdsetup detach --force).
				 * Do NOT abort otherwise.
				 * Aborting local requests may cause serious problems,
				 * if requests are completed to upper layers already,
				 * and then later the already submitted local bio completes.
				 * This can cause DMA into former bio pages that meanwhile
				 * have been re-used for other things.
				 * So aborting local requests may cause crashes,
				 * or even worse, silent data corruption.
				 */
				if (test_and_clear_bit(FORCE_DETACH, &device->flags))
					tl_abort_disk_io(device);

				/* current state still has to be D_FAILED,
				 * there is only one way out: to D_DISKLESS,
				 * and that may only happen after our put_ldev below. */
				if (device->disk_state[NOW] != D_FAILED)
					drbd_err(device,
						 "ASSERT FAILED: disk is %s during detach\n",
						 drbd_disk_str(device->disk_state[NOW]));

				send_new_state_to_all_peer_devices(state_change, n_device);

				for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
					struct drbd_peer_device_state_change *peer_device_state_change =
						&state_change->peer_devices[
							n_device * state_change->n_connections + n_connection];
					struct drbd_peer_device *peer_device = peer_device_state_change->peer_device;
					drbd_rs_cancel_all(peer_device);
				}

				/* In case we want to get something to stable storage still,
				 * this may be the last chance.
				 * Following put_ldev may transition to D_DISKLESS. */
				drbd_md_sync(device);
			}
			put_ldev(device);
		}

		/* second half of local IO error, failure to attach,
		 * or administrative detach,
		 * after local_cnt references have reached zero again */
		if (disk_state[OLD] != D_DISKLESS && disk_state[NEW] == D_DISKLESS) {
			/* We must still be diskless,
			 * re-attach has to be serialized with this! */
			if (device->disk_state[NOW] != D_DISKLESS)
				drbd_err(device,
					"ASSERT FAILED: disk is %s while going diskless\n",
					drbd_disk_str(device->disk_state[NOW]));

			send_new_state_to_all_peer_devices(state_change, n_device);
			/*
			 * finish_state_change() has grabbed a reference on
			 * ldev in this case.
			 */
			put_ldev(device);
		}

		/* Notify peers that I had a local IO error and did not detach. */
		if (disk_state[OLD] == D_UP_TO_DATE && disk_state[NEW] == D_INCONSISTENT)
			send_new_state_to_all_peer_devices(state_change, n_device);

		drbd_md_sync(device);
	}

	for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
		struct drbd_connection_state_change *connection_state_change = &state_change->connections[n_connection];
		struct drbd_connection *connection = connection_state_change->connection;
		enum drbd_conn_state *cstate = connection_state_change->cstate;
		enum drbd_role *peer_role = connection_state_change->peer_role;

		/* Upon network configuration, we need to start the receiver */
		if (cstate[OLD] == C_STANDALONE && cstate[NEW] == C_UNCONNECTED)
			drbd_thread_start(&connection->receiver);

		if (susp_fen[NEW]) {
			bool all_peer_disks_outdated = true;
			bool all_peer_disks_connected = true;

			/* Iterate over all peer devices on this connection.  */
			for (n_device = 0; n_device < state_change->n_devices; n_device++) {
				struct drbd_peer_device_state_change *peer_device_state_change =
					&state_change->peer_devices[n_device * state_change->n_connections + n_connection];
				enum drbd_repl_state *repl_state = peer_device_state_change->repl_state;
				enum drbd_disk_state *peer_disk_state = peer_device_state_change->disk_state;

				if (peer_disk_state[NEW] > D_OUTDATED)
					all_peer_disks_outdated = false;
				if (repl_state[NEW] < L_ESTABLISHED)
					all_peer_disks_connected = false;
			}

			/* case1: The outdate peer handler is successful: */
			if (all_peer_disks_outdated) {
				struct drbd_peer_device *peer_device;
				unsigned long irq_flags;
				int vnr;

				rcu_read_lock();
				idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
					struct drbd_device *device = peer_device->device;
					if (test_bit(NEW_CUR_UUID, &device->flags)) {
						drbd_uuid_new_current(device);
						clear_bit(NEW_CUR_UUID, &device->flags);
					}
				}
				rcu_read_unlock();
				begin_state_change(resource, &irq_flags, CS_VERBOSE);
				_tl_restart(connection, CONNECTION_LOST_WHILE_PENDING);
				__change_io_susp_fencing(resource, false);
				end_state_change(resource, &irq_flags);
			}
			/* case2: The connection was established again: */
			if (all_peer_disks_connected) {
				struct drbd_peer_device *peer_device;
				unsigned long irq_flags;
				int vnr;

				rcu_read_lock();
				idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
					struct drbd_device *device = peer_device->device;
					clear_bit(NEW_CUR_UUID, &device->flags);
				}
				rcu_read_unlock();
				begin_state_change(resource, &irq_flags, CS_VERBOSE);
				_tl_restart(connection, RESEND);
				__change_io_susp_fencing(resource, false);
				end_state_change(resource, &irq_flags);
			}
		}

		if (peer_role[OLD] == R_PRIMARY &&
		    cstate[OLD] == C_CONNECTED && cstate[NEW] < C_CONNECTED) {
			/* A connection to a primary went down, notify other peers about that */
			notify_peers_lost_primary(connection);
		}
	}

	/* reachability changes must go out after notify_peer_lost_primary() */
	for (n_connection = 0; n_connection < state_change->n_connections; n_connection++) {
		struct drbd_connection_state_change *connection_state_change = &state_change->connections[n_connection];
		struct drbd_connection *connection = connection_state_change->connection;
		enum drbd_conn_state *cstate = connection_state_change->cstate;

		if (cstate[NEW] == C_CONNECTED || cstate[NEW] == C_CONNECTING) {
			still_connected = true;
			drbd_propagate_reachability(connection);
		}
	}

	if (!still_connected)
		mod_timer_pending(&resource->twopc_timer, jiffies);

	if (work->done)
		complete(work->done);
	forget_state_change(state_change);
	kfree(work);

	return 0;
}

static inline bool local_state_change(enum chg_state_flags flags)
{
	return flags & (CS_HARD | CS_LOCAL_ONLY);
}

static enum drbd_state_rv
__peer_request(struct drbd_connection *connection, int vnr,
	       union drbd_state mask, union drbd_state val)
{
	enum drbd_state_rv rv = SS_SUCCESS;

	if (connection->cstate[NOW] == C_CONNECTED) {
		enum drbd_packet cmd = (vnr == -1) ? P_CONN_ST_CHG_REQ : P_STATE_CHG_REQ;
		if (!conn_send_state_req(connection, vnr, cmd, mask, val)) {
			set_bit(TWOPC_PREPARED, &connection->flags);
			rv = SS_CW_SUCCESS;
		}
	}
	return rv;
}

static enum drbd_state_rv __peer_reply(struct drbd_connection *connection)
{
	if (test_and_clear_bit(TWOPC_NO, &connection->flags))
		return SS_CW_FAILED_BY_PEER;
	if (test_and_clear_bit(TWOPC_YES, &connection->flags) ||
	    !test_bit(TWOPC_PREPARED, &connection->flags))
		return SS_CW_SUCCESS;
	return SS_UNKNOWN_ERROR;
}

static bool when_done_lock(struct drbd_resource *resource,
			   unsigned long *irq_flags)
{
	spin_lock_irqsave(&resource->req_lock, *irq_flags);
	if (!resource->remote_state_change)
		return true;
	spin_unlock_irqrestore(&resource->req_lock, *irq_flags);
	return false;
}

/**
 * complete_remote_state_change  -  Wait for other remote state changes to complete
 */
static void complete_remote_state_change(struct drbd_resource *resource,
					 unsigned long *irq_flags)
{
	if (resource->remote_state_change) {
		enum chg_state_flags flags = resource->state_change_flags;

		begin_remote_state_change(resource, irq_flags);
		wait_event(resource->twopc_wait,
			   when_done_lock(resource, irq_flags));
		__end_remote_state_change(resource, flags);
	}
}

static enum drbd_state_rv
change_peer_state(struct drbd_connection *connection, int vnr,
		  union drbd_state mask, union drbd_state val, unsigned long *irq_flags)
{
	struct drbd_resource *resource = connection->resource;
	enum chg_state_flags flags = resource->state_change_flags | CS_TWOPC;
	enum drbd_state_rv rv;

	if (!expect(resource, flags & CS_SERIALIZE))
		return SS_CW_FAILED_BY_PEER;

	complete_remote_state_change(resource, irq_flags);

	resource->remote_state_change = true;
	resource->twopc_reply.initiator_node_id = resource->res_opts.node_id;
	resource->twopc_reply.tid = 0;
	begin_remote_state_change(resource, irq_flags);
	rv = __peer_request(connection, vnr, mask, val);
	if (rv == SS_CW_SUCCESS) {
		wait_event(resource->state_wait,
			((rv = __peer_reply(connection)) != SS_UNKNOWN_ERROR));
		clear_bit(TWOPC_PREPARED, &connection->flags);
	}
	end_remote_state_change(resource, irq_flags, flags);
	return rv;
}

static enum drbd_state_rv
__cluster_wide_request(struct drbd_resource *resource, int vnr, enum drbd_packet cmd,
		       struct p_twopc_request *request, u64 reach_immediately)
{
	struct drbd_connection *connection;
	enum drbd_state_rv rv = SS_SUCCESS;

	rcu_read_lock();
	for_each_connection(connection, resource) {
		u64 mask = NODE_MASK(connection->net_conf->peer_node_id);
		if (reach_immediately & mask)
			set_bit(TWOPC_PREPARED, &connection->flags);
		else
			clear_bit(TWOPC_PREPARED, &connection->flags);
	}
	for_each_connection(connection, resource) {
		struct twopc_reply *reply = &resource->twopc_reply;
		u64 primary_nodes, weak_nodes;

		if (!test_bit(TWOPC_PREPARED, &connection->flags))
			continue;
		kref_get(&connection->kref);
		rcu_read_unlock();

		/* If the cluster is still connected after this transaction,
		 * all nodes will receive the same primary_nodes and weak_nodes
		 * masks (set by the caller).  Otherwise, there will be two
		 * cluster segments after this transaction.
		 */
		primary_nodes = reply->primary_nodes;
		if (be32_to_cpu(request->initiator_node_id) ==
		    resource->res_opts.node_id &&
		    be32_to_cpu(request->target_node_id) ==
		    connection->net_conf->peer_node_id) {
			primary_nodes &= reply->target_reachable_nodes;
			weak_nodes = reply->target_weak_nodes;
		} else {
			primary_nodes &= reply->reachable_nodes;
			weak_nodes = reply->weak_nodes;
		}
		request->primary_nodes = cpu_to_be64(primary_nodes);
		request->weak_nodes = cpu_to_be64(weak_nodes);

		clear_bit(TWOPC_YES, &connection->flags);
		clear_bit(TWOPC_NO, &connection->flags);
		clear_bit(TWOPC_RETRY, &connection->flags);

		if (!conn_send_twopc_request(connection, vnr, cmd, request))
			rv = SS_CW_SUCCESS;
		else {
			clear_bit(TWOPC_PREPARED, &connection->flags);
			wake_up(&resource->work.q_wait);
		}
		rcu_read_lock();
		kref_put(&connection->kref, drbd_destroy_connection);
	}
	rcu_read_unlock();
	return rv;
}

bool cluster_wide_reply_ready(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	bool ready = true;

	rcu_read_lock();
	for_each_connection(connection, resource) {
		if (!test_bit(TWOPC_PREPARED, &connection->flags))
			continue;
		if (!(test_bit(TWOPC_YES, &connection->flags) ||
		      test_bit(TWOPC_NO, &connection->flags) ||
		      test_bit(TWOPC_RETRY, &connection->flags))) {
			drbd_debug(connection, "Reply not ready yet\n");
			ready = false;
			break;
		}
	}
	rcu_read_unlock();
	return ready;
}

static enum drbd_state_rv get_cluster_wide_reply(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	enum drbd_state_rv rv = SS_CW_SUCCESS;

	rcu_read_lock();
	for_each_connection(connection, resource) {
		if (!test_bit(TWOPC_PREPARED, &connection->flags))
			continue;
		if (test_bit(TWOPC_NO, &connection->flags))
			rv = SS_CW_FAILED_BY_PEER;
		if (test_bit(TWOPC_RETRY, &connection->flags)) {
			rv = SS_CONCURRENT_ST_CHG;
			break;
		}
	}
	rcu_read_unlock();
	return rv;
}

static bool supports_two_phase_commit(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	bool supported = true;

	rcu_read_lock();
	for_each_connection(connection, resource) {
		if (connection->cstate[NOW] != C_CONNECTED)
			continue;
		if (connection->agreed_pro_version < 110) {
			supported = false;
			break;
		}
	}
	rcu_read_unlock();

	return supported;
}

static struct drbd_connection *get_first_connection(struct drbd_resource *resource)
{
	struct drbd_connection *connection = NULL;

	rcu_read_lock();
	if (!list_empty(&resource->connections)) {
		connection = first_connection(resource);
		kref_get(&connection->kref);
	}
	rcu_read_unlock();
	return connection;
}

static int twopc_initiator_work(struct drbd_work *work, int cancel)
{
	struct drbd_resource *resource =
		container_of(work, struct drbd_resource, twopc_work);

	wake_up(&resource->state_wait);
	return 0;
}

u64 directly_connected_nodes(struct drbd_resource *resource)
{
	u64 directly_connected = 0;
	struct drbd_connection *connection;

	for_each_connection(connection, resource) {
		if (connection->cstate[NOW] < C_CONNECTED)
			continue;
		directly_connected |=
			NODE_MASK(connection->net_conf->peer_node_id);
	}
	return directly_connected;
}


/* Think: Can this be replaced by a call to __is_valid_soft_transition() */
static enum drbd_state_rv primary_nodes_allowed(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	enum drbd_state_rv rv = SS_SUCCESS;

	rcu_read_lock();
	for_each_connection_rcu(connection, resource) {
		u64 mask;

		/* If this peer is primary as well, the config must allow it. */
		mask = NODE_MASK(connection->net_conf->peer_node_id);
		if ((resource->twopc_reply.primary_nodes & mask) &&
		    !(connection->net_conf->two_primaries)) {
			rv = SS_TWO_PRIMARIES;
			break;
		}
	}
	rcu_read_unlock();

	/* We must be directly connected to all primary nodes. */
	if (rv == SS_SUCCESS &&
	    (resource->twopc_reply.weak_nodes &
	      NODE_MASK(resource->res_opts.node_id)))
		rv = SS_WEAKLY_CONNECTED;

	return rv;
}

long twopc_retry_timeout(struct drbd_resource *resource)
{
	struct drbd_connection *connection;
	int connections = 0;
	long timeout = 0;

	rcu_read_lock();
	for_each_connection(connection, resource) {
		if (connection->cstate[NOW] < C_CONNECTING)
			continue;
		connections++;
	}
	rcu_read_unlock();

	if (connections > 1) {
		timeout = resource->res_opts.twopc_retry_timeout *
			  HZ / 10 * (connections - 1);
		timeout = random32() % timeout;
	}
	return timeout;
}

/**
 * change_cluster_wide_state  -  Cluster-wide two-phase commit
 *
 * Perform a two-phase commit transaction among all (reachable) nodes in the
 * cluster.  In our transaction model, the initiator of a transaction is also
 * the coordinator.
 *
 * In phase one of the transaction, the coordinator sends all nodes in the
 * cluster a P_TWOPC_PREPARE packet.  Each node replies with either P_TWOPC_YES
 * if it consents or with P_TWOPC_NO if it denies the transaction.  Once all
 * replies have been received, the coordinator sends all nodes in the cluster a
 * P_TWOPC_COMMIT or P_TWOPC_ABORT packet to finish the transaction.
 *
 * When a node in the cluster is busy with another transaction, it replies with
 * P_TWOPC_NO.  The coordinator is then responsible for retrying the
 * transaction.
 *
 * Since a cluster is not guaranteed to always be fully connected, some nodes
 * will not be directly reachable from other nodes.  In order to still reach
 * all nodes in the cluster, participants will forward requests to nodes which
 * haven't received the request yet:
 *
 * The nodes_to_reach field in requests indicates which nodes have received the
 * request already.  Before forwarding a request to a peer, a node removes
 * itself from nodes_to_reach; it then sends the request to all directly
 * connected nodes in nodes_to_reach.
 *
 * If there are redundant paths in the cluster, requests will reach some nodes
 * more than once.  Nodes remember when they are taking part in a transaction;
 * they detect duplicate requests and reply to them with P_TWOPC_YES packets.
 * (Transactions are identified by the node id of the initiator and a random,
 * unique-enough transaction identifier.)
 *
 * A configurable timeout determines how long a coordinator or participant will
 * wait for a transaction to finish.  A transaction that times out is assumed
 * to have aborted.
 */
static enum drbd_state_rv
change_cluster_wide_state(struct drbd_resource *resource, int vnr,
			  union drbd_state mask, union drbd_state val,
			  unsigned long *irq_flags,
			  int target_node_id)
{
	struct p_twopc_request request;
	struct twopc_reply *reply = &resource->twopc_reply;
	enum chg_state_flags flags = resource->state_change_flags | CS_TWOPC;
	struct drbd_connection *connection;
	enum drbd_state_rv rv;
	u64 reach_immediately;
	int attempts = 5;
	unsigned long start_time;

	if (!supports_two_phase_commit(resource)) {
		connection = get_first_connection(resource);
		rv = SS_SUCCESS;
		if (connection) {
			rv = change_peer_state(connection, vnr, mask, val, irq_flags);
			kref_put(&connection->kref, drbd_destroy_connection);
		}
		goto out;
	}

	if (!expect(resource, flags & CS_SERIALIZE)) {
		rv = SS_CW_FAILED_BY_PEER;
		goto out;
	}

	complete_remote_state_change(resource, irq_flags);
	start_time = jiffies;

    retry:
	reach_immediately = directly_connected_nodes(resource);
	if (target_node_id != -1) {
		struct drbd_connection *connection;

		/* Fail if the target node is no longer directly reachable. */
		for_each_connection(connection, resource) {
			if (connection->net_conf->peer_node_id == target_node_id)
				goto found_target_node;
		}
		rv = SS_CW_FAILED_BY_PEER;
		goto out;

	    found_target_node:
		if (!(connection->cstate[NOW] == C_CONNECTED ||
		      (connection->cstate[NOW] == C_CONNECTING &&
		       mask.conn == conn_MASK &&
		       val.conn == C_CONNECTED))) {
			rv = SS_CW_FAILED_BY_PEER;
			goto out;
		}

		/* For connect transactions, add the target node id. */
		reach_immediately |= NODE_MASK(target_node_id);
	}

	do
		reply->tid = random32();
	while (!reply->tid);

	request.tid = cpu_to_be32(reply->tid);
	request.initiator_node_id = cpu_to_be32(resource->res_opts.node_id);
	request.target_node_id = cpu_to_be32(target_node_id);
	request.nodes_to_reach = cpu_to_be64(
		~(reach_immediately | NODE_MASK(resource->res_opts.node_id)));
	request.primary_nodes = 0;  /* Computed in phase 1. */
	request.weak_nodes = 0;  /* Computed in phase 1. */
	request.mask = cpu_to_be32(mask.i);
	request.val = cpu_to_be32(val.i);

	drbd_debug(resource, "Preparing cluster-wide state change %u\n",
		   be32_to_cpu(request.tid));
	resource->remote_state_change = true;
	reply->initiator_node_id = resource->res_opts.node_id;
	reply->target_node_id = target_node_id;
	reply->primary_nodes = 0;
	reply->target_weak_nodes = 0;
	reply->weak_nodes = 0;

	reply->reachable_nodes = directly_connected_nodes(resource) |
				       NODE_MASK(resource->res_opts.node_id);
	if (mask.conn == conn_MASK && val.conn == C_CONNECTED) {
		reply->reachable_nodes |= NODE_MASK(target_node_id);
		reply->target_reachable_nodes = reply->reachable_nodes;
	} else if (mask.conn == conn_MASK && val.conn == C_DISCONNECTING) {
		reply->target_reachable_nodes = NODE_MASK(target_node_id);
		reply->reachable_nodes &= ~reply->target_reachable_nodes;
	} else {
		reply->target_reachable_nodes = reply->reachable_nodes;
	}

	resource->twopc_work.cb = twopc_initiator_work;
	begin_remote_state_change(resource, irq_flags);
	rv = __cluster_wide_request(resource, vnr, P_TWOPC_PREPARE,
				    &request, reach_immediately);
	if (rv == SS_CW_SUCCESS) {
		if (wait_event_timeout(resource->state_wait,
				       cluster_wide_reply_ready(resource),
				       twopc_timeout(resource)))
			rv = get_cluster_wide_reply(resource);
		else
			rv = SS_TIMEOUT;

		if (rv == SS_CW_SUCCESS) {
			u64 directly_reachable =
				directly_connected_nodes(resource) |
				NODE_MASK(resource->res_opts.node_id);

			if (mask.conn == conn_MASK) {
				if (val.conn == C_CONNECTED)
					directly_reachable |= NODE_MASK(target_node_id);
				if (val.conn == C_DISCONNECTING)
					directly_reachable &= ~NODE_MASK(target_node_id);
			}
			if ((mask.role == role_MASK && val.role == R_PRIMARY) ||
			    (mask.role != role_MASK && resource->role[NOW] == R_PRIMARY)) {
				reply->primary_nodes |=
					NODE_MASK(resource->res_opts.node_id);
				reply->weak_nodes |= ~directly_reachable;
			}

			if (mask.role == role_MASK && val.role == R_PRIMARY)
				rv = primary_nodes_allowed(resource);
			if (mask.conn == conn_MASK && val.conn == C_CONNECTED) {
				/* This is a request to establish a connection. */
				/* Establishing the connection is only allowed
				 * if the resulting cluster contains no primary
				 * nodes or all primary nodes are connected to each
				 * other (i.e., they are in strongly_connected_nodes).  */
				if (reply->primary_nodes & reply->weak_nodes)
					rv = SS_WEAKLY_CONNECTED;
				/* FIXME: Where do we check against the
				 * configuration if the set of primaries is allowed? */
			}
			if (!(mask.conn == conn_MASK && val.conn == C_DISCONNECTING) ||
			    (reply->reachable_nodes & reply->target_reachable_nodes)) {
				/* The cluster is still connected after this
				 * transaction: either this transaction does
				 * not disconnect a connection, or there are
				 * redundant connections.  */

				u64 m;

				m = reply->reachable_nodes | reply->target_reachable_nodes;
				reply->reachable_nodes = m;
				reply->target_reachable_nodes = m;

				m = reply->weak_nodes | reply->target_weak_nodes;
				reply->weak_nodes = m;
				reply->target_weak_nodes = m;
			} else {
				for_each_connection(connection, resource) {
					int node_id = connection->net_conf->peer_node_id;

					if (node_id == target_node_id) {
						drbd_info(connection, "Cluster is now split");
						break;
					}
				}
			}
		}
		if (rv >= SS_SUCCESS) {
			drbd_debug(resource, "Committing cluster-wide state change %u (%ums)\n",
				   be32_to_cpu(request.tid),
				   jiffies_to_msecs(jiffies - start_time));

			rv = __cluster_wide_request(resource, vnr, P_TWOPC_COMMIT,
						    &request, reach_immediately);
			if (rv != SS_CW_SUCCESS) {
				/* FIXME: disconnect all peers? */
			}
			flags |= CS_WEAK_NODES;
		} else {
			drbd_debug(resource, "Aborting cluster-wide state change %u (%ums)\n",
				   be32_to_cpu(request.tid),
				   jiffies_to_msecs(jiffies - start_time));
			__cluster_wide_request(resource, vnr, P_TWOPC_ABORT,
					       &request, reach_immediately);
		}

		rcu_read_lock();
		for_each_connection(connection, resource)
			clear_bit(TWOPC_PREPARED, &connection->flags);
		rcu_read_unlock();
	}
	if ((rv == SS_TIMEOUT || rv == SS_CONCURRENT_ST_CHG) &&
	    --attempts > 0) {
		long timeout = twopc_retry_timeout(resource);
		drbd_debug(resource, "Retrying cluster-wide state change after %ums\n",
			   jiffies_to_msecs(timeout));
		schedule_timeout(timeout);
		end_remote_state_change(resource, irq_flags, flags);
		goto retry;
	}

	end_remote_state_change(resource, irq_flags, flags);

	if (rv >= SS_SUCCESS) {
		__change_weak(resource,
			reply->weak_nodes &
			NODE_MASK(resource->res_opts.node_id));
	}

    out:
	return rv;
}

static void twopc_end_nested(struct drbd_resource *resource, enum drbd_packet cmd)
{
	struct drbd_connection *twopc_parent;
	struct twopc_reply twopc_reply;

	spin_lock_irq(&resource->req_lock);
	twopc_parent = resource->twopc_parent;
	resource->twopc_parent = NULL;
	spin_unlock_irq(&resource->req_lock);

	if (!expect(resource, twopc_parent))
		return;

	drbd_debug(twopc_parent, "Nested state change %u result: %s\n",
		   resource->twopc_reply.tid,
		   cmdname(cmd));
	twopc_reply = resource->twopc_reply;

	if (cmd == P_TWOPC_NO) {
		del_timer(&resource->twopc_timer);
		abort_nested_twopc_work(&resource->twopc_work, false);
	}

	drbd_send_twopc_reply(twopc_parent, cmd, &twopc_reply);
	kref_put(&twopc_parent->kref, drbd_destroy_connection);
}

static int nested_twopc_work(struct drbd_work *work, int cancel)
{
	struct drbd_resource *resource =
		container_of(work, struct drbd_resource, twopc_work);
	enum drbd_state_rv rv;
	enum drbd_packet cmd;

	rv = get_cluster_wide_reply(resource);
	if (rv >= SS_SUCCESS)
		cmd = P_TWOPC_YES;
	else if (rv == SS_CONCURRENT_ST_CHG)
		cmd = P_TWOPC_RETRY;
	else
		cmd = P_TWOPC_NO;
	twopc_end_nested(resource, cmd);
	return 0;
}

enum drbd_state_rv
nested_twopc_request(struct drbd_resource *resource, int vnr, enum drbd_packet cmd,
		     struct p_twopc_request *request)
{
	enum drbd_state_rv rv;
	u64 nodes_to_reach, reach_immediately;

	spin_lock_irq(&resource->req_lock);
	nodes_to_reach = be64_to_cpu(request->nodes_to_reach);
	reach_immediately = directly_connected_nodes(resource) & nodes_to_reach;
	nodes_to_reach &= ~(reach_immediately | NODE_MASK(resource->res_opts.node_id));
	request->nodes_to_reach = cpu_to_be64(nodes_to_reach);
	resource->twopc_work.cb = nested_twopc_work;
	spin_unlock_irq(&resource->req_lock);

	rv = __cluster_wide_request(resource, vnr, cmd, request, reach_immediately);
	if (cmd == P_TWOPC_PREPARE) {
		if (rv <= SS_SUCCESS) {
			cmd = (rv == SS_SUCCESS) ? P_TWOPC_YES : P_TWOPC_NO;
			twopc_end_nested(resource, cmd);
		}
	}
	return rv;
}

static bool has_up_to_date_peer_disks(struct drbd_device *device)
{
	struct drbd_peer_device *peer_device;

	for_each_peer_device(peer_device, device)
		if (peer_device->disk_state[NEW] == D_UP_TO_DATE)
			return true;
	return false;
}

void __change_role(struct drbd_resource *resource, enum drbd_role role,
		   bool force)
{
	resource->role[NEW] = role;

	if (role == R_PRIMARY && force) {
		struct drbd_device *device;
		int vnr;

		rcu_read_lock();
		idr_for_each_entry(&resource->devices, device, vnr) {
			if (device->disk_state[NEW] < D_UP_TO_DATE &&
			    device->disk_state[NEW] >= D_INCONSISTENT &&
			    !has_up_to_date_peer_disks(device))
				device->disk_state[NEW] = D_UP_TO_DATE;
		}
		rcu_read_unlock();
	}
}

enum drbd_state_rv change_role(struct drbd_resource *resource,
			       enum drbd_role role,
			       enum chg_state_flags flags,
			       bool force)
{
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, flags | CS_SERIALIZE | CS_LOCAL_ONLY);
	if (!local_state_change(flags) &&
	    resource->role[NOW] != R_PRIMARY && role == R_PRIMARY) {
		enum drbd_state_rv rv;

		__change_role(resource, role, force);
		rv = try_state_change(resource);
		if (rv == SS_SUCCESS)
			rv = change_cluster_wide_state(resource, -1,
				NS(role, role), &irq_flags, -1);
		if (rv < SS_SUCCESS) {
			abort_state_change(resource, &irq_flags);
			return rv;
		}
	}
	__change_role(resource, role, force);
	return end_state_change(resource, &irq_flags);
}

void __change_io_susp_user(struct drbd_resource *resource, bool value)
{
	resource->susp[NEW] = value;
}

enum drbd_state_rv change_io_susp_user(struct drbd_resource *resource,
				       bool value,
				       enum chg_state_flags flags)
{
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, flags);
	__change_io_susp_user(resource, value);
	return end_state_change(resource, &irq_flags);
}

void __change_io_susp_no_data(struct drbd_resource *resource, bool value)
{
	resource->susp_nod[NEW] = value;
}

void __change_io_susp_fencing(struct drbd_resource *resource, bool value)
{
	resource->susp_fen[NEW] = value;
}

void __change_weak(struct drbd_resource *resource, bool value)
{
	resource->weak[NEW] = value;
}

void __change_disk_state(struct drbd_device *device, enum drbd_disk_state disk_state)
{
	device->disk_state[NEW] = disk_state;
}

void __change_disk_states(struct drbd_resource *resource, enum drbd_disk_state disk_state)
{
	struct drbd_device *device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, vnr)
		__change_disk_state(device, disk_state);
	rcu_read_unlock();
}

static bool device_has_connected_peer_devices(struct drbd_device *device)
{
	struct drbd_peer_device *peer_device;

	for_each_peer_device(peer_device, device)
		if (peer_device->repl_state[NOW] >= L_ESTABLISHED)
			return true;
	return false;
}

enum drbd_state_rv change_disk_state(struct drbd_device *device,
				     enum drbd_disk_state disk_state,
				     enum chg_state_flags flags)
{
	struct drbd_resource *resource = device->resource;
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, flags | CS_SERIALIZE | CS_LOCAL_ONLY);
	if (!local_state_change(flags) &&
	    device->disk_state[NOW] != D_FAILED && disk_state == D_FAILED &&
	    device_has_connected_peer_devices(device)) {
		enum drbd_state_rv rv;

		__change_disk_state(device, disk_state);
		rv = try_state_change(resource);
		if (rv == SS_SUCCESS)
			rv = change_cluster_wide_state(resource, device->vnr,
				NS(disk, disk_state), &irq_flags, -1);
		if (rv < SS_SUCCESS) {
			abort_state_change(resource, &irq_flags);
			return rv;
		}
	}
	__change_disk_state(device, disk_state);
	return end_state_change(resource, &irq_flags);
}

void __change_cstate(struct drbd_connection *connection, enum drbd_conn_state cstate)
{
	if (cstate == C_DISCONNECTING)
		set_bit(DISCONNECT_SENT, &connection->flags);

	connection->cstate[NEW] = cstate;
	if (cstate < C_CONNECTED) {
		struct drbd_peer_device *peer_device;
		int vnr;

		rcu_read_lock();
		idr_for_each_entry(&connection->peer_devices, peer_device, vnr)
			__change_repl_state(peer_device, L_OFF);
		rcu_read_unlock();
	}
}

static bool connection_has_connected_peer_devices(struct drbd_connection *connection)
{
	struct drbd_peer_device *peer_device;
	int vnr;

	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		if (peer_device->repl_state[NOW] >= L_ESTABLISHED)
			return true;
	}
	return false;
}

enum outdate_what { OUTDATE_NOTHING, OUTDATE_DISKS, OUTDATE_PEER_DISKS };

static enum outdate_what outdate_on_disconnect(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;

	if (connection->fencing_policy >= FP_RESOURCE &&
	    resource->role[NOW] != connection->peer_role[NOW]) {
		if (resource->role[NOW] == R_PRIMARY)
			return OUTDATE_PEER_DISKS;
		if (connection->peer_role[NOW] != R_PRIMARY)
			return OUTDATE_DISKS;
	}
	return OUTDATE_NOTHING;
}

static void __change_cstate_and_outdate(struct drbd_connection *connection,
					enum drbd_conn_state cstate,
					enum outdate_what outdate_what)
{
	__change_cstate(connection, cstate);
	switch(outdate_what) {
		case OUTDATE_DISKS:
			__change_disk_states(connection->resource, D_OUTDATED);
			break;
		case OUTDATE_PEER_DISKS:
			__change_peer_disk_states(connection, D_OUTDATED);
			break;
		case OUTDATE_NOTHING:
			break;
	}
}

enum drbd_state_rv connect_transaction(struct drbd_connection *connection)
{
	int target_node_id = connection->net_conf->peer_node_id;
	struct drbd_resource *resource = connection->resource;
	unsigned long irq_flags;
	enum drbd_state_rv rv;

	begin_state_change(resource, &irq_flags, CS_SERIALIZE | CS_LOCAL_ONLY);
	if (connection->cstate[NOW] != C_CONNECTING)
		rv = SS_IN_TRANSIENT_STATE;
	else {
		rv = change_cluster_wide_state(resource, -1,
			NS(conn, C_CONNECTED), &irq_flags,
			target_node_id);
	}
	__change_cstate(connection, C_CONNECTED);
	end_state_change(resource, &irq_flags);
	return rv;
}

/**
 * change_cstate()  -  change the connection state of a connection
 *
 * When disconnecting from a peer, we may also need to outdate the local or
 * peer disks depending on the fencing policy.  This cannot easily be split
 * into two state changes.
 */
enum drbd_state_rv change_cstate(struct drbd_connection *connection,
				 enum drbd_conn_state cstate,
				 enum chg_state_flags flags)
{
	struct drbd_resource *resource = connection->resource;
	enum outdate_what outdate_what = OUTDATE_NOTHING;
	enum drbd_state_rv rv = SS_SUCCESS;
	unsigned long irq_flags;

	/*
	 * Hard connection state changes like a protocol error or forced
	 * disconnect may occur while we are holding resource->state_sem.  In
	 * that case, omit CS_SERIALIZE so that we don't deadlock trying to
	 * grab that mutex again.
	 */
	if (!(flags & CS_HARD))
		flags |= CS_SERIALIZE;

	begin_state_change(resource, &irq_flags, flags | CS_LOCAL_ONLY);
	if (!local_state_change(flags) &&
		   cstate == C_DISCONNECTING &&
		   connection_has_connected_peer_devices(connection)) {
		outdate_what = outdate_on_disconnect(connection);
		__change_cstate_and_outdate(connection, cstate, outdate_what);
		rv = try_state_change(resource);
		if (rv == SS_SUCCESS) {
			union drbd_state mask = {}, val = {};
			int target_node_id = connection->net_conf->peer_node_id;

			mask.conn = conn_MASK;
			val.conn = cstate;

			switch(outdate_what) {
			case OUTDATE_DISKS:
				mask.disk = disk_MASK;
				val.disk = D_OUTDATED;
				break;
			case OUTDATE_PEER_DISKS:
				mask.pdsk = pdsk_MASK;
				val.pdsk = D_OUTDATED;
				break;
			case OUTDATE_NOTHING:
				break;
			}

			rv = change_cluster_wide_state(resource, -1, mask, val,
					&irq_flags, target_node_id);
		}
	}
	__change_cstate_and_outdate(connection, cstate, outdate_what);
	if (rv < SS_SUCCESS) {
		abort_state_change(resource, &irq_flags);
		return rv;
	}
	return end_state_change(resource, &irq_flags);
}

void __change_peer_role(struct drbd_connection *connection, enum drbd_role peer_role)
{
	connection->peer_role[NEW] = peer_role;
}

void __change_peer_weak(struct drbd_connection *connection, bool peer_weak)
{
	connection->peer_weak[NEW] = peer_weak;
}

void __change_repl_state(struct drbd_peer_device *peer_device, enum drbd_repl_state repl_state)
{
	peer_device->repl_state[NEW] = repl_state;
	if (repl_state > L_OFF)
		peer_device->connection->cstate[NEW] = C_CONNECTED;
}

enum drbd_state_rv change_repl_state(struct drbd_peer_device *peer_device,
				     enum drbd_repl_state new_repl_state,
				     enum chg_state_flags flags)
{
	struct drbd_resource *resource = peer_device->device->resource;
	enum drbd_repl_state *repl_state = peer_device->repl_state;
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, flags | CS_SERIALIZE | CS_LOCAL_ONLY);
	if (!local_state_change(flags) && repl_state[NOW] != new_repl_state &&
	    ((repl_state[NOW] >= L_ESTABLISHED &&
	      (new_repl_state == L_STARTING_SYNC_S || new_repl_state == L_STARTING_SYNC_T)) ||
	     (repl_state[NOW] == L_ESTABLISHED &&
	      (new_repl_state == L_VERIFY_S || new_repl_state == L_OFF)))) {
		enum drbd_state_rv rv;

		__change_repl_state(peer_device, new_repl_state);
		rv = try_state_change(resource);
		if (rv == SS_SUCCESS)
			rv = change_peer_state(peer_device->connection, peer_device->device->vnr,
					       NS(conn, new_repl_state), &irq_flags);
		if (rv < SS_SUCCESS) {
			abort_state_change(resource, &irq_flags);
			return rv;
		}
	}
	__change_repl_state(peer_device, new_repl_state);
	return end_state_change(resource, &irq_flags);
}

enum drbd_state_rv stable_change_repl_state(struct drbd_peer_device *peer_device,
					    enum drbd_repl_state repl_state,
					    enum chg_state_flags flags)
{
	return stable_state_change(peer_device->device->resource,
		change_repl_state(peer_device, repl_state, flags));
}

void __change_peer_disk_state(struct drbd_peer_device *peer_device, enum drbd_disk_state disk_state)
{
	peer_device->disk_state[NEW] = disk_state;
}

void __change_peer_disk_states(struct drbd_connection *connection,
			       enum drbd_disk_state disk_state)
{
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr)
		__change_peer_disk_state(peer_device, disk_state);
	rcu_read_unlock();
}

enum drbd_state_rv change_peer_disk_state(struct drbd_peer_device *peer_device,
					  enum drbd_disk_state disk_state,
					  enum chg_state_flags flags)
{
	struct drbd_resource *resource = peer_device->device->resource;
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, flags);
	__change_peer_disk_state(peer_device, disk_state);
	return end_state_change(resource, &irq_flags);
}

void __change_resync_susp_user(struct drbd_peer_device *peer_device,
				       bool value)
{
	peer_device->resync_susp_user[NEW] = value;
}

enum drbd_state_rv change_resync_susp_user(struct drbd_peer_device *peer_device,
						   bool value,
						   enum chg_state_flags flags)
{
	struct drbd_resource *resource = peer_device->device->resource;
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, flags);
	__change_resync_susp_user(peer_device, value);
	return end_state_change(resource, &irq_flags);
}

void __change_resync_susp_peer(struct drbd_peer_device *peer_device,
				       bool value)
{
	peer_device->resync_susp_peer[NEW] = value;
}

void __change_resync_susp_dependency(struct drbd_peer_device *peer_device,
					     bool value)
{
	peer_device->resync_susp_dependency[NEW] = value;
}
