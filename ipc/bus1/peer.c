/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <uapi/linux/bus1.h>
#include "domain.h"
#include "message.h"
#include "peer.h"
#include "pool.h"
#include "queue.h"
#include "transaction.h"
#include "user.h"
#include "util.h"

struct bus1_peer_name {
	union {
		struct rcu_head rcu;
		struct bus1_peer_name *next;
	};
	struct bus1_peer *peer;
	struct rb_node rb;
	char name[];
};

static void bus1_peer_info_reset(struct bus1_peer_info *peer_info)
{
	struct bus1_queue_node *node, *t;
	struct bus1_message *message;

	mutex_lock(&peer_info->lock);

	rbtree_postorder_for_each_entry_safe(node, t,
					     &peer_info->queue.messages, rb) {
		if (WARN_ON(!bus1_queue_node_is_message(node)))
			continue;

		message = container_of(node, struct bus1_message, qnode);
		RB_CLEAR_NODE(&node->rb);
		if (bus1_queue_node_is_committed(node)) {
			bus1_message_deallocate_locked(message, peer_info);
			bus1_message_free(message);
		}
		/* if uncommitted, the unlink serves as removal marker */
	}
	bus1_queue_post_flush(&peer_info->queue);

	bus1_pool_flush(&peer_info->pool);

	mutex_unlock(&peer_info->lock);
}

static struct bus1_peer_info *
bus1_peer_info_free(struct bus1_peer_info *peer_info)
{
	if (!peer_info)
		return NULL;

	WARN_ON(peer_info->user);

	bus1_peer_info_reset(peer_info);

	bus1_queue_destroy(&peer_info->queue);
	bus1_pool_destroy(&peer_info->pool);
	bus1_user_quota_destroy(&peer_info->quota);

	/*
	 * Make sure the object is freed in a delayed-manner. Some
	 * embedded members (like the queue) must be accessible for an entire
	 * rcu read-side critical section.
	 */
	kfree_rcu(peer_info, rcu);

	return NULL;
}

static struct bus1_peer_info *
bus1_peer_info_new(struct bus1_cmd_connect *param)
{
	struct bus1_peer_info *peer_info;
	int r;

	if (unlikely(param->pool_size == 0 ||
		     !IS_ALIGNED(param->pool_size, PAGE_SIZE)))
		return ERR_PTR(-EINVAL);

	peer_info = kmalloc(sizeof(*peer_info), GFP_KERNEL);
	if (!peer_info)
		return ERR_PTR(-ENOMEM);

	mutex_init(&peer_info->lock);
	peer_info->user = NULL;
	bus1_user_quota_init(&peer_info->quota);
	peer_info->pool = BUS1_POOL_NULL;
	bus1_queue_init_for_peer(&peer_info->queue, peer_info);
	peer_info->map_handles_by_id = RB_ROOT;
	peer_info->map_handles_by_node = RB_ROOT;
	seqcount_init(&peer_info->seqcount);
	peer_info->handle_ids = 0;

	r = bus1_pool_create_for_peer(&peer_info->pool, peer_info,
				      param->pool_size);
	if (r < 0)
		goto error;

	return peer_info;

error:
	bus1_peer_info_free(peer_info);
	return ERR_PTR(r);
}

static struct bus1_peer_name *
bus1_peer_name_new(const char *name, struct bus1_peer *peer)
{
	struct bus1_peer_name *peer_name;
	size_t namelen;

	if (WARN_ON(!peer))
		return ERR_PTR(-EINVAL);

	namelen = strlen(name) + 1;
	if (namelen < 2 || namelen > BUS1_NAME_MAX_SIZE)
		return ERR_PTR(-EMSGSIZE);

	peer_name = kmalloc(sizeof(*peer_name) + namelen, GFP_KERNEL);
	if (!peer_name)
		return ERR_PTR(-ENOMEM);

	peer_name->next = NULL;
	peer_name->peer = peer;
	RB_CLEAR_NODE(&peer_name->rb);
	memcpy(peer_name->name, name, namelen);

	return peer_name;
}

static struct bus1_peer_name *
bus1_peer_name_free(struct bus1_peer_name *peer_name)
{
	if (!peer_name)
		return NULL;

	WARN_ON(!RB_EMPTY_NODE(&peer_name->rb));
	kfree_rcu(peer_name, rcu);

	return NULL;
}

static int bus1_peer_name_add(struct bus1_peer_name *peer_name,
			      struct bus1_domain *domain)
{
	struct rb_node *prev, **slot;
	struct bus1_peer_name *iter;
	int v;

	lockdep_assert_held(&domain->lock);
	lockdep_assert_held(&domain->seqcount);

	if (WARN_ON(!RB_EMPTY_NODE(&peer_name->rb)))
		return -EINVAL;

	/* find rb-tree entry and check for possible duplicates first */
	slot = &domain->map_names.rb_node;
	prev = NULL;
	while (*slot) {
		prev = *slot;
		iter = container_of(prev, struct bus1_peer_name, rb);
		v = strcmp(peer_name->name, iter->name);
		if (!v)
			return -EISNAM;
		else if (v < 0)
			slot = &prev->rb_left;
		else /* if (v > 0) */
			slot = &prev->rb_right;
	}

	/* insert into tree */
	rb_link_node_rcu(&peer_name->rb, prev, slot);
	rb_insert_color(&peer_name->rb, &domain->map_names);

	++domain->n_names;
	return 0;
}

static void bus1_peer_name_remove(struct bus1_peer_name *peer_name,
				  struct bus1_domain *domain)
{
	lockdep_assert_held(&domain->lock);
	lockdep_assert_held(&domain->seqcount);

	if (RB_EMPTY_NODE(&peer_name->rb))
		return;

	rb_erase(&peer_name->rb, &domain->map_names);
	RB_CLEAR_NODE(&peer_name->rb);

	--domain->n_names;
}

/**
 * bus1_peer_new() - allocate new peer
 *
 * Allocate a new peer handle. The handle is *not* activated, nor linked to any
 * domain. The caller owns the only pointer to the new peer.
 *
 * Return: Pointer to peer, ERR_PTR on failure.
 */
struct bus1_peer *bus1_peer_new(void)
{
	struct bus1_peer *peer;

	peer = kmalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	init_rwsem(&peer->rwlock);
	init_waitqueue_head(&peer->waitq);
	bus1_active_init(&peer->active);
	rcu_assign_pointer(peer->info, NULL);
	peer->names = NULL;
	INIT_LIST_HEAD(&peer->link_domain);

	return peer;
}

/**
 * bus1_peer_free() - destroy peer
 * @peer:	peer to destroy, or NULL
 *
 * Destroy a peer object that was previously allocated via bus1_peer_new(). If
 * the peer object was activated, then the caller must make sure it was
 * properly torn down before destroying it.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_peer *bus1_peer_free(struct bus1_peer *peer)
{
	if (!peer)
		return NULL;

	WARN_ON(!list_empty(&peer->link_domain));
	WARN_ON(peer->names);
	WARN_ON(rcu_access_pointer(peer->info));
	bus1_active_destroy(&peer->active);
	kfree_rcu(peer, rcu);

	return NULL;
}

static void bus1_peer_cleanup(struct bus1_active *active,
			      void *userdata)
{
	struct bus1_peer *peer = container_of(active, struct bus1_peer,
					      active);
	struct bus1_domain *domain = userdata;
	struct bus1_peer_name *peer_name;
	struct bus1_peer_info *peer_info;

	/*
	 * This function is called by bus1_active_cleanup(), once all active
	 * references to the handle are drained. In that case, we know that
	 * no-one can hold a pointer to the peer, anymore. Hence, we can simply
	 * drop all the peer information and destroy the peer.
	 *
	 * During domain teardown, we avoid dropping peers from the tree, so we
	 * can safely iterate the tree and reset it afterwards.
	 */

	lockdep_assert_held(&domain->lock);
	lockdep_assert_held(&domain->seqcount);

	peer_info = rcu_dereference_protected(peer->info,
					      lockdep_is_held(&domain->lock));
	if (!peer_info)
		return;

	while ((peer_name = peer->names)) {
		peer->names = peer->names->next;
		bus1_peer_name_remove(peer_name, domain);
		bus1_peer_name_free(peer_name);
	}

	/* users reference the domain, so release with the domain locked */
	peer_info->user = bus1_user_release(peer_info->user);

	list_del_init(&peer->link_domain);
	--domain->n_peers;
}

/**
 * bus1_peer_teardown() - XXX
 */
int bus1_peer_teardown(struct bus1_peer *peer, struct bus1_domain *domain)
{
	struct bus1_peer_info *peer_info = NULL;
	int r = 0;

	/* lock against parallel CONNECT/DISCONNECT */
	down_write(&peer->rwlock);

	/* deactivate and wait for any outstanding operations */
	bus1_active_deactivate(&peer->active);
	bus1_active_drain(&peer->active, &peer->waitq);

	mutex_lock(&domain->lock);

	write_seqcount_begin(&domain->seqcount);
	/*
	 * We must not sleep on the peer->waitq, it could deadlock
	 * since we already hold the domain-lock. However, luckily all
	 * peer-releases are locked against the domain, so we wouldn't
	 * gain anything by passing the waitq in. Pass NULL instead.
	 */
	if (bus1_active_cleanup(&peer->active, NULL, bus1_peer_cleanup,
				domain)) {
		peer_info = rcu_dereference_protected(peer->info,
					lockdep_is_held(&domain->lock));
		rcu_assign_pointer(peer->info, NULL);
	} else {
		r = -ESHUTDOWN;
	}
	write_seqcount_end(&domain->seqcount);

	mutex_unlock(&domain->lock);
	up_write(&peer->rwlock);

	if (peer_info)
		bus1_peer_info_free(peer_info);

	return r;
}

/**
 * bus1_peer_teardown_domain() - tear down peer
 * @peer:	peer to tear down
 * @domain:	parent domain
 *
 * This is similar to bus1_peer_teardown(), but is modified to be called during
 * domain teardown. The domain is responsible to deactivate and drain a peer
 * before calling into this. Furthermore, the domain itself must be deactivated
 * and drained already.
 *
 * This function simply cleans up the peer object and releases associated
 * resources. However, this function does *NOT* remove the peer from the
 * peer-map. This allows the caller to safely iterate the peer map and call
 * this helper on all peers.
 *
 * The caller is responsible to reset the peer-map afterwards.
 *
 * The caller must hold the domain lock and seqlock.
 *
 * This function can be called multiple times just fine. Anything but the first
 * call will be a no-op.
 */
void bus1_peer_teardown_domain(struct bus1_peer *peer,
			       struct bus1_domain *domain)
{
	struct bus1_peer_info *peer_info;

	lockdep_assert_held(&domain->lock);
	lockdep_assert_held(&domain->seqcount);

	/*
	 * We must not sleep on the peer->waitq, it could deadlock
	 * since we already hold the domain-lock. However, luckily all
	 * peer-releases are locked against the domain, so we wouldn't
	 * gain anything by passing the waitq in. Pass NULL instead.
	 */
	if (bus1_active_cleanup(&peer->active, NULL, bus1_peer_cleanup,
				domain)) {
		peer_info = rcu_dereference_protected(peer->info,
					lockdep_is_held(&domain->lock));
		rcu_assign_pointer(peer->info, NULL);
		bus1_peer_info_free(peer_info);
	}
}

/**
 * bus1_peer_acquire() - acquire active reference to peer
 * @peer:	peer to operate on, or NULL
 *
 * Acquire a new active reference to the given peer. If the peer was not
 * activated yet, or if it was already deactivated, this will fail.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: Pointer to peer, NULL on failure.
 */
struct bus1_peer *bus1_peer_acquire(struct bus1_peer *peer)
{
	if (peer && bus1_active_acquire(&peer->active))
		return peer;
	return NULL;
}

/**
 * bus1_peer_release() - release an active reference
 * @peer:	handle to release, or NULL
 *
 * This releases an active reference to a peer, acquired previously via one
 * of the lookup functions.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_peer *bus1_peer_release(struct bus1_peer *peer)
{
	if (peer)
		bus1_active_release(&peer->active, &peer->waitq);
	return NULL;
}

/**
 * bus1_peer_dereference() - dereference a peer handle
 * @peer:	handle to dereference
 *
 * Dereference a peer handle to get access to the underlying peer object. This
 * function simply returns the pointer to the linked peer information object,
 * which then can be accessed directly by the caller. The caller must hold an
 * active reference to the handle, and retain it as long as the peer object is
 * used.
 *
 * Note: If you weren't called through this handle, but rather retrieved it via
 *       other means (eg., domain lookup), you must be aware that this handle
 *       might be reset at any time. Hence, any operation you perform on the
 *       handle must be tagged by the actual peer ID (which you should have
 *       retrieved via the same means as the handle itself).
 *       If the peer is reset midway through your operation, it gets a new ID,
 *       notifies any peer that tracked it, and automatically discards any
 *       operation that was tagged with an old ID (or, if the operation wasn't
 *       finished, it will be discarded later on). A reset is a lossy operation
 *       so any pending operation is discarded silently. The origin of the
 *       operation thus gets the impression that it succeeded (and should be
 *       tracking the peer to get notified about the reset, if interested).
 *
 * Return: Pointer to the underlying peer information object is returned.
 */
struct bus1_peer_info *bus1_peer_dereference(struct bus1_peer *peer)
{
	return rcu_dereference_protected(peer->info,
					 lockdep_is_held(&peer->active));
}

/**
 * bus1_peer_wake() - wake up peer
 * @peer:		peer to wake up
 *
 * This wakes up a peer and notifies user-space about poll() events.
 */
void bus1_peer_wake(struct bus1_peer *peer)
{
	wake_up_interruptible(&peer->waitq);
}

/*
 * Check if the string is a name of the peer.
 *
 * Return: -EREMCHG if it is not, 0 if it is but is not the last name, and the
 * number of names the peer has otherwise.
 */
static ssize_t bus1_peer_name_check(struct bus1_peer *peer, const char *name)
{
	struct bus1_peer_name *peer_name;
	size_t n_names = 0;

	lockdep_assert_held(&peer->rwlock);

	for (peer_name = peer->names; peer_name; peer_name = peer_name->next) {
		++n_names;

		if (strcmp(name, peer_name->name) == 0) {
			if (peer_name->next)
				return 0;
			else
				return n_names;
		}
	}

	return -EREMCHG;
}

/*
 * Check if a nulstr contains exactly the names of the peer.
 *
 * Return: 0 if it does, -EREMCHG if it does not or -EMSGSIZE if it is
 * malformed.
 */
static int bus1_peer_names_check(struct bus1_peer *peer, const char *names,
				 size_t names_len)
{
	size_t n, n_names = 0, n_names_old = 0;
	ssize_t r;

	lockdep_assert_held(&peer->rwlock);

	if (names_len == 0 && peer->names)
		return -EREMCHG;

	while (names_len > 0) {
		n = strnlen(names, names_len);
		if (n == 0 || n == names_len)
			return -EMSGSIZE;

		r = bus1_peer_name_check(peer, names);
		if (r < 0)
			return r;
		if (r > 0)
			n_names_old = r;

		names += n + 1;
		names_len -= n + 1;
		++n_names;
	}

	if (n_names != n_names_old)
		return -EREMCHG;

	return 0;
}

static int bus1_peer_connect_new(struct bus1_peer *peer,
				 struct bus1_domain *domain,
				 kuid_t uid,
				 struct bus1_cmd_connect *param)
{
	struct bus1_peer_name *peer_name, *names = NULL;
	struct bus1_peer_info *peer_info;
	size_t n, remaining;
	const char *name;
	int r;

	/*
	 * Connect a new peer. We first allocate the peer object, then
	 * lock the whole domain and link the names and the peer
	 * itself. If either fails, revert everything we did so far and
	 * bail out.
	 */

	lockdep_assert_held(&domain->active);
	lockdep_assert_held(&peer->rwlock);

	/* cannot connect a peer that is already connected */
	if (!bus1_active_is_new(&peer->active)) {
		struct bus1_peer_info *peer_info;

		/*
		 * If the peer is already connected, we return -EISCONN if the
		 * passed in parameters match, or -EREMCHG if they do not (but
		 * are otherwise valid).
		 */

		/*
		 * We hold a domain-reference and peer-lock, the caller already
		 * verified we're not disconnected. Barriers guarantee that the
		 * peer is accessible, and both the domain teardown and
		 * peer-disconnect have to wait for us to finish. However, to
		 * be safe, check for NULL anyway.
		 */
		peer_info = rcu_dereference_protected(peer->info,
					lockdep_is_held(&domain->active) &&
					lockdep_is_held(&peer->rwlock));
		if (WARN_ON(!peer_info))
			return -ESHUTDOWN;

		if (param->pool_size != peer_info->pool.size)
			return -EREMCHG;

		r = bus1_peer_names_check(peer, param->names, param->size - sizeof(*param));
		if (r < 0)
			return r;

		return -EISCONN;
	}

	/*
	 * The domain-reference and peer-lock guarantee that no other
	 * connect, disconnect, or teardown can race us (they wait for us). We
	 * also verified that the peer is NEW. Hence, peer->info must be
	 * NULL. We still verify it, just to be safe.
	 */
	if (WARN_ON(rcu_dereference_protected(peer->info,
					lockdep_is_held(&domain->active) &&
					lockdep_is_held(&peer->rwlock))))
		return -EISCONN;

	/* allocate new peer_info object */
	peer_info = bus1_peer_info_new(param);
	if (IS_ERR(peer_info))
		return PTR_ERR(peer_info);

	/* pin a user object */
	peer_info->user = bus1_user_acquire_by_uid(domain, uid);
	if (IS_ERR(peer_info->user)) {
		r = PTR_ERR(peer_info->user);
		peer_info->user = NULL;
		goto error;
	}

	/* allocate names */
	name = param->names;
	remaining = param->size - sizeof(*param);
	while (remaining > 0) {
		n = strnlen(name, remaining);
		if (n == 0 || n == remaining) {
			r = -EMSGSIZE;
			goto error;
		}

		peer_name = bus1_peer_name_new(name, peer);
		if (IS_ERR(peer_name)) {
			r = PTR_ERR(peer_name);
			goto error;
		}

		/* insert into names list */
		peer_name->next = names;
		names = peer_name;

		name += n + 1;
		remaining -= n + 1;
	}

	mutex_lock(&domain->lock);
	write_seqcount_begin(&domain->seqcount);

	/* link into names rbtree */
	for (peer_name = names; peer_name; peer_name = peer_name->next) {
		r = bus1_peer_name_add(peer_name, domain);
		if (r < 0)
			goto error_unlock;
	}

	peer->names = names;
	list_add(&peer->link_domain, &domain->list_peers);
	++domain->n_peers;
	rcu_assign_pointer(peer->info, peer_info);
	bus1_active_activate(&peer->active);

	write_seqcount_end(&domain->seqcount);
	mutex_unlock(&domain->lock);

	return 0;

error_unlock:
	for (peer_name = names; peer_name; peer_name = peer_name->next)
		bus1_peer_name_remove(peer_name, domain);
	write_seqcount_end(&domain->seqcount);
	mutex_unlock(&domain->lock);
error:
	while ((peer_name = names)) {
		names = names->next;
		bus1_peer_name_free(peer_name);
	}
	peer_info->user = bus1_user_release(peer_info->user);
	bus1_peer_info_free(peer_info);
	return r;
}

static int bus1_peer_connect_reset(struct bus1_peer *peer,
				   struct bus1_domain *domain,
				   struct bus1_cmd_connect *param)
{
	struct bus1_peer_info *peer_info;

	/*
	 * If a RESET is requested, we atomically DISCONNECT and
	 * CONNECT the peer. Luckily, all we have to do is allocate a
	 * new ID and re-add it to the rb-tree. Then we tell the peer
	 * itself to flush any pending data. There might be operations
	 * in-flight, that finish after we reset the peer. All those
	 * operations must be tagged with the old id, though (see
	 * bus1_peer_dereference() for details). Therefore, those
	 * operations can be silently ignored and will be gc'ed later
	 * on if their tag is outdated.
	 */

	lockdep_assert_held(&domain->active);
	lockdep_assert_held(&peer->rwlock);

	/* cannot reset a peer that was never connected */
	if (bus1_active_is_new(&peer->active))
		return -ENOTCONN;

	/* verify pool-size is unset and no names are appended */
	if (param->pool_size != 0 || param->size > sizeof(*param))
		return -EINVAL;

	/*
	 * We hold domain reference and peer-lock, hence domain/peer teardown
	 * must wait for us. Our caller already verified we haven't been torn
	 * down, yet. We verified that the peer is not NEW. Hence, the peer
	 * pointer must be valid.
	 * Be safe and verify it anyway.
	 */
	peer_info = rcu_dereference_protected(peer->info,
					lockdep_is_held(&domain->active) &&
					lockdep_is_held(&peer->rwlock));
	if (WARN_ON(!peer_info))
		return -ESHUTDOWN;

	/* provide information for caller */
	param->pool_size = peer_info->pool.size;

	/* safe to call outside of domain-lock; we still hold the peer-lock */
	bus1_peer_info_reset(peer_info);

	return 0;
}

static int bus1_peer_connect_query(struct bus1_peer *peer,
				   struct bus1_domain *domain,
				   struct bus1_cmd_connect *param)
{
	struct bus1_peer_info *peer_info;

	lockdep_assert_held(&domain->active);
	lockdep_assert_held(&peer->rwlock);

	/* cannot query a peer that was never connected */
	if (bus1_active_is_new(&peer->active))
		return -ENOTCONN;

	/*
	 * We hold a domain-reference and peer-lock, the caller already
	 * verified we're not disconnected. Barriers guarantee that the peer is
	 * accessible, and both the domain teardown and peer-disconnect have to
	 * wait for us to finish. However, to be safe, check for NULL anyway.
	 */
	peer_info = rcu_dereference_protected(peer->info,
					lockdep_is_held(&domain->active) &&
					lockdep_is_held(&peer->rwlock));
	if (WARN_ON(!peer_info))
		return -ESHUTDOWN;

	param->pool_size = peer_info->pool.size;

	return 0;
}

static int bus1_peer_ioctl_connect(struct bus1_peer *peer,
				   struct bus1_domain *domain,
				   const struct file *file,
				   unsigned long arg)
{
	struct bus1_cmd_connect __user *uparam = (void __user *)arg;
	struct bus1_cmd_connect *param;
	int r;

	/*
	 * The domain-active-reference guarantees that a domain teardown waits
	 * for us, before it starts the force-disconnect on all clients.
	 */
	lockdep_assert_held(&domain->active);

	param = bus1_import_dynamic_ioctl(arg, sizeof(*param));
	if (IS_ERR(param))
		return PTR_ERR(param);

	/* check for validity of all flags */
	if (param->flags & ~(BUS1_CONNECT_FLAG_PEER |
			     BUS1_CONNECT_FLAG_MONITOR |
			     BUS1_CONNECT_FLAG_QUERY |
			     BUS1_CONNECT_FLAG_RESET))
		return -EINVAL;
	/* only one mode can be specified */
	if (!!(param->flags & BUS1_CONNECT_FLAG_PEER) +
	    !!(param->flags & BUS1_CONNECT_FLAG_MONITOR) +
	    !!(param->flags & BUS1_CONNECT_FLAG_RESET) > 1)
		return -EINVAL;
	/* only root can claim names */
	if (!file_ns_capable(file, domain->info->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/* lock against parallel CONNECT/DISCONNECT */
	down_write(&peer->rwlock);

	if (bus1_active_is_deactivated(&peer->active)) {
		/* all fails, if the peer was already disconnected */
		r = -ESHUTDOWN;
	} else if (param->flags & (BUS1_CONNECT_FLAG_PEER |
				   BUS1_CONNECT_FLAG_MONITOR)) {
		/* fresh connect of a new peer */
		r = bus1_peer_connect_new(peer, domain, file->f_cred->uid,
					  param);
	} else if (param->flags & BUS1_CONNECT_FLAG_RESET) {
		/* reset of the peer requested */
		r = bus1_peer_connect_reset(peer, domain, param);
	} else if (param->flags & BUS1_CONNECT_FLAG_QUERY) {
		/* fallback: no special operation specified, just query */
		r = bus1_peer_connect_query(peer, domain, param);
	} else {
		r = -EINVAL; /* no mode specified */
	}

	up_write(&peer->rwlock);

	/*
	 * QUERY can be combined with any CONNECT operation. On success, it
	 * causes the peer information to be copied back to user-space.
	 * All handlers above must provide that information in @param for this
	 * to copy it back.
	 */
	if (r >= 0 && (param->flags & BUS1_CONNECT_FLAG_QUERY)) {
		if (put_user(param->pool_size, &uparam->pool_size))
			r = -EFAULT; /* Don't care.. keep what we did so far */
	}

	kfree(param);
	return r;
}

static int bus1_peer_ioctl_resolve(struct bus1_peer *peer,
				   struct bus1_domain *domain,
				   unsigned long arg)
{
	struct bus1_cmd_resolve __user *uparam = (void __user *)arg;
	struct bus1_cmd_resolve *param;
	struct bus1_peer_name *peer_name;
	struct rb_node *n;
	size_t namelen;
	unsigned seq;
	int r, v;

	lockdep_assert_held(&domain->active);

	param = bus1_import_dynamic_ioctl(arg, sizeof(*param));
	if (IS_ERR(param))
		return PTR_ERR(param);

	/* no flags are known at this time */
	if (param->flags) {
		r = -EINVAL;
		goto exit;
	}

	/* result must be cleared by caller */
	if (param->id != 0) {
		r = -EINVAL;
		goto exit;
	}

	/* reject overlong/short names early */
	namelen = param->size - sizeof(*param);
	if (namelen < 2 || namelen > BUS1_NAME_MAX_SIZE) {
		r = -ENXIO;
		goto exit;
	}

	/* name must be zero-terminated */
	if (param->name[namelen - 1] != 0) {
		r = -EINVAL;
		goto exit;
	}

	/* find unique-id of named peer */
	seq = raw_seqcount_begin(&domain->seqcount);
	do {
		rcu_read_lock();
		n = rcu_dereference(domain->map_names.rb_node);
		while (n) {
			peer_name = container_of(n, struct bus1_peer_name, rb);
			v = strcmp(param->name, peer_name->name);
			if (v == 0) {
				if (bus1_active_is_active(&peer_name->peer->active))
					param->id = 0; /* XXX: handle id */
				break;
			} else if (v < 0) {
				n = rcu_dereference(n->rb_left);
			} else /* if (v > 0) */ {
				n = rcu_dereference(n->rb_right);
			}
		}
		rcu_read_unlock();
	} while (!n &&
		 read_seqcount_retry(&domain->seqcount, seq) &&
		 ((seq = read_seqcount_begin(&domain->seqcount)), true));

	if (!n)
		r = -ENXIO; /* not found, or deactivated */
	else if (put_user(param->id, &uparam->id))
		r = -EFAULT;
	else
		r = 0;

exit:
	kfree(param);
	return r;
}

static int bus1_peer_ioctl_slice_release(struct bus1_peer *peer,
					 unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	u64 offset;
	int r;

	r = bus1_import_fixed_ioctl(&offset, arg, sizeof(offset));
	if (r < 0)
		return r;

	mutex_lock(&peer_info->lock);
	r = bus1_pool_release_user(&peer_info->pool, offset);
	mutex_unlock(&peer_info->lock);

	return r;
}

static int bus1_peer_ioctl_send(struct bus1_peer *peer,
				struct bus1_domain *domain,
				unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	struct bus1_transaction *transaction = NULL;
	/* Use a stack-allocated buffer for the transaction object if it fits */
	u8 buf[512];
	const u64 __user *ptr_dest;
	struct bus1_cmd_send param;
	u64 destination;
	size_t i;
	int r;

	lockdep_assert_held(&peer->active);

	r = bus1_import_fixed_ioctl(&param, arg, sizeof(param));
	if (r < 0)
		return r;

	if (unlikely(param.flags & ~(BUS1_SEND_FLAG_IGNORE_UNKNOWN |
				     BUS1_SEND_FLAG_CONVEY_ERRORS)))
		return -EINVAL;

	/* check basic limits; avoids integer-overflows later on */
	if (unlikely(param.n_vecs > BUS1_VEC_MAX) ||
	    unlikely(param.n_fds > BUS1_FD_MAX))
		return -EMSGSIZE;

	/* 32bit pointer validity checks */
	if (unlikely(param.ptr_destinations !=
		     (u64)(unsigned long)param.ptr_destinations) ||
	    unlikely(param.ptr_vecs !=
		     (u64)(unsigned long)param.ptr_vecs) ||
	    unlikely(param.ptr_ids !=
		     (u64)(unsigned long)param.ptr_ids) ||
	    unlikely(param.ptr_fds !=
		     (u64)(unsigned long)param.ptr_fds))
		return -EFAULT;

	/* peer is pinned, hence domain_info and ID can be accessed freely */
	transaction = bus1_transaction_new_from_user(peer_info, domain, &param,
						     buf, sizeof(buf),
						     bus1_in_compat_syscall());
	if (IS_ERR(transaction))
		return PTR_ERR(transaction);

	ptr_dest = (const u64 __user *)(unsigned long)param.ptr_destinations;
	if (param.n_destinations == 1) { /* Fastpath: unicast */
		if (get_user(destination, ptr_dest)) {
			r = -EFAULT; /* faults are always fatal */
			goto exit;
		}

		r = bus1_transaction_commit_for_id(transaction,
						   peer->info->user,
						   destination,
						   param.flags);
		if (r < 0)
			goto exit;
	} else { /* Slowpath: any message */
		for (i = 0; i < param.n_destinations; ++i) {
			if (get_user(destination, ptr_dest + i)) {
				r = -EFAULT; /* faults are always fatal */
				goto exit;
			}

			r = bus1_transaction_instantiate_for_id(transaction,
							peer->info->user,
							destination,
							param.flags);
			if (r < 0)
				goto exit;
		}

		bus1_transaction_commit(transaction);
	}

	r = 0;

exit:
	bus1_transaction_free(transaction, transaction != (void*)buf);
	return r;
}

static int bus1_peer_ioctl_recv(struct bus1_peer *peer, unsigned long arg)
{
	struct bus1_peer_info *peer_info = bus1_peer_dereference(peer);
	struct bus1_cmd_recv __user *uparam = (void __user *)arg;
	struct bus1_queue_node *node;
	struct bus1_message *message;
	struct bus1_cmd_recv param;
	size_t wanted_fds, n_fds = 0;
	int r, *t, *fds = NULL;
	struct kvec vec;

	r = bus1_import_fixed_ioctl(&param, arg, sizeof(param));
	if (r < 0)
		return r;

	if (unlikely(param.flags & ~(BUS1_RECV_FLAG_PEEK)))
		return -EINVAL;

	if (unlikely(param.msg_offset != BUS1_OFFSET_INVALID) ||
	    unlikely(param.msg_size != 0) ||
	    unlikely(param.msg_ids != 0) ||
	    unlikely(param.msg_fds != 0))
		return -EINVAL;

	/*
	 * Peek at the first message to fetch the FD count. We need to
	 * pre-allocate FDs, to avoid dropping messages due to FD exhaustion.
	 * If no entry is queued, we can bail out early.
	 * Note that this is just a fast-path optimization. Anyone might race
	 * us for message retrieval, so we have to check it again below.
	 */
	rcu_read_lock();
	node = bus1_queue_peek_rcu(&peer_info->queue);
	if (node) {
		WARN_ON(!bus1_queue_node_is_message(node));
		message = bus1_message_from_node(node);
		wanted_fds = message->n_files;
	}
	rcu_read_unlock();
	if (!node)
		return -EAGAIN;

	/*
	 * Deal with PEEK first. This is simple. Just look at the first queued
	 * message, publish the slice and return the information to user-space.
	 * Keep the entry queued, so it can be peeked multiple times, and
	 * received later on.
	 * We do not install any FDs for PEEK, but provide the number in
	 * msg_fds, anyway.
	 */
	if (param.flags & BUS1_RECV_FLAG_PEEK) {
		mutex_lock(&peer_info->lock);
		node = bus1_queue_peek(&peer_info->queue);
		if (node) {
			message = bus1_message_from_node(node);
			bus1_pool_publish(&peer_info->pool, message->slice,
					  &param.msg_offset, &param.msg_size);
			param.msg_fds = message->n_files;
		}
		mutex_unlock(&peer_info->lock);

		if (!node)
			return -EAGAIN;

		r = 0;
		goto exit;
	}

	/*
	 * So there is a message queued with 'wanted_fds' attached FDs.
	 * Allocate a temporary buffer to store them, then dequeue the message.
	 * In case someone raced us and the message changed, re-allocate the
	 * temporary buffer and retry.
	 */

	do {
		if (wanted_fds > n_fds) {
			t = krealloc(fds, wanted_fds * sizeof(*fds),
				     GFP_TEMPORARY);
			if (!t) {
				r = -ENOMEM;
				goto exit;
			}

			fds = t;
			for ( ; n_fds < wanted_fds; ++n_fds) {
				r = get_unused_fd_flags(O_CLOEXEC);
				if (r < 0)
					goto exit;

				fds[n_fds] = r;
			}
		}

		mutex_lock(&peer_info->lock);
		node = bus1_queue_peek(&peer_info->queue);
		message = node ? bus1_message_from_node(node) : NULL;
		if (!node) {
			/* nothing to do, caught below */
		} else if (message->n_files > n_fds) {
			/* re-allocate FD array and retry */
			wanted_fds = message->n_files;
		} else {
			bus1_queue_remove(&peer_info->queue, node);
			bus1_pool_publish(&peer_info->pool, message->slice,
					  &param.msg_offset, &param.msg_size);
			param.msg_fds = message->n_files;

			/*
			 * Fastpath: If no FD is transmitted, we can avoid the
			 *           second lock below. Directly release the
			 *           slice.
			 */
			if (message->n_files == 0)
				bus1_message_deallocate_locked(message,
							       peer_info);
		}
		mutex_unlock(&peer_info->lock);
	} while (wanted_fds > n_fds);

	if (!node) {
		r = -EAGAIN;
		goto exit;
	}

	while (n_fds > message->n_files)
		put_unused_fd(fds[--n_fds]);

	if (n_fds > 0) {
		/*
		 * We dequeued the message, we already fetched enough FDs, all
		 * we have to do is copy the FD numbers into the slice and link
		 * the FDs.
		 * The only reason this can fail, is if writing the pool fails,
		 * which itself can only happen during OOM. In that case, we
		 * don't support reverting the operation, but you rather lose
		 * the message. We cannot put it back on the queue (would break
		 * ordering), and we don't want to perform the copy-operation
		 * while holding the queue-lock.
		 * We treat this OOM as if the actual message transaction OOMed
		 * and simply drop the message.
		 */

		vec.iov_base = fds;
		vec.iov_len = n_fds * sizeof(*fds);

		r = bus1_pool_write_kvec(&peer_info->pool, message->slice,
					 message->slice->size - vec.iov_len,
					 &vec, 1, vec.iov_len);

		mutex_lock(&peer_info->lock);
		bus1_message_deallocate_locked(message, peer_info);
		mutex_unlock(&peer_info->lock);

		/* on success, install FDs; on error, see fput() in `exit:' */
		if (r >= 0) {
			for ( ; n_fds > 0; --n_fds)
				fd_install(fds[n_fds - 1],
					   get_file(message->files[n_fds - 1]));
		} else {
			/* XXX: convey error, just like in transactions */
		}
	} else {
		/* slice is already released, nothing to do */
		r = 0;
	}

	bus1_message_free(message);

exit:
	if (r >= 0) {
		if (put_user(param.msg_offset, &uparam->msg_offset) ||
		    put_user(param.msg_size, &uparam->msg_size) ||
		    put_user(param.msg_ids, &uparam->msg_ids) ||
		    put_user(param.msg_fds, &uparam->msg_fds))
			r = -EFAULT; /* Don't care.. keep what we did so far */
	}
	while (n_fds > 0)
		put_unused_fd(fds[--n_fds]);
	kfree(fds);
	return r;
}

/**
 * bus1_peer_ioctl() - handle peer ioctl
 * @peer:		peer to work on
 * @domain:		parent domain
 * @file:		file this ioctl is called on
 * @cmd:		ioctl command
 * @arg:		ioctl argument
 *
 * This handles the given ioctl (cmd+arg) on the passed peer. @domain must be
 * the parent domain of @peer. The caller must not hold an active reference to
 * either.
 *
 * Multiple ioctls can be called in parallel just fine. No locking is needed.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_peer_ioctl(struct bus1_peer *peer,
		    struct bus1_domain *domain,
		    const struct file *file,
		    unsigned int cmd,
		    unsigned long arg)
{
	int r = -ENOTTY;

	switch (cmd) {
	case BUS1_CMD_CONNECT:
	case BUS1_CMD_RESOLVE:
		/* lock against domain shutdown */
		if (!bus1_domain_acquire(domain))
			return -ESHUTDOWN;

		if (cmd == BUS1_CMD_CONNECT)
			r = bus1_peer_ioctl_connect(peer, domain, file, arg);
		else if (cmd == BUS1_CMD_RESOLVE)
			r = bus1_peer_ioctl_resolve(peer, domain, arg);

		bus1_domain_release(domain);
		break;

	case BUS1_CMD_DISCONNECT:
		/* no arguments allowed, it behaves like the last close() */
		if (arg != 0)
			return -EINVAL;

		return bus1_peer_teardown(peer, domain);

	case BUS1_CMD_SLICE_RELEASE:
	case BUS1_CMD_SEND:
	case BUS1_CMD_RECV:
		down_read(&peer->rwlock);
		if (!bus1_peer_acquire(peer)) {
			r = -ESHUTDOWN;
		} else {
			if (cmd == BUS1_CMD_SLICE_RELEASE)
				r = bus1_peer_ioctl_slice_release(peer, arg);
			else if (cmd == BUS1_CMD_SEND)
				r = bus1_peer_ioctl_send(peer, domain, arg);
			else if (cmd == BUS1_CMD_RECV)
				r = bus1_peer_ioctl_recv(peer, arg);
			bus1_peer_release(peer);
		}
		up_read(&peer->rwlock);
		break;
	}

	return r;
}
