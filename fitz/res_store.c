#include "fitz.h"
#include "mupdf.h"

struct fz_item_s
{
	fz_obj *key;
	fz_storable *val;
	unsigned int size;
	fz_item *next;
	fz_item *prev;
	fz_store *store;
};

struct refkey
{
	fz_store_free_fn *free;
	int num;
	int gen;
};

struct fz_store_s
{
	int refs;

	/* Every item in the store is kept in a doubly linked list, ordered
	 * by usage (so LRU entries are at the end). */
	fz_item *head;
	fz_item *tail;

	/* We have a hash table that allows to quickly find a subset of the
	 * entries (those whose keys are indirect objects). */
	fz_hash_table *hash;

	/* We keep track of the size of the store, and keep it below max. */
	unsigned int max;
	unsigned int size;
};

void
fz_new_store_context(fz_context *ctx, unsigned int max)
{
	fz_store *store;
	store = fz_malloc_struct(ctx, fz_store);
	fz_try(ctx)
	{
		store->hash = fz_new_hash_table(ctx, 4096, sizeof(struct refkey));
	}
	fz_catch(ctx)
	{
		fz_free(ctx, store);
		fz_rethrow(ctx);
	}
	store->refs = 1;
	store->head = NULL;
	store->tail = NULL;
	store->size = 0;
	store->max = max;
	ctx->store = store;
}

void *
fz_keep_storable(fz_context *ctx, fz_storable *s)
{
	if (s == NULL)
		return NULL;
	fz_lock(ctx, FZ_LOCK_ALLOC);
	if (s->refs > 0)
		s->refs++;
	fz_unlock(ctx, FZ_LOCK_ALLOC);
	return s;
}

void
fz_drop_storable(fz_context *ctx, fz_storable *s)
{
	int do_free = 0;

	if (s == NULL)
		return;
	fz_lock(ctx, FZ_LOCK_ALLOC);
	if (s->refs < 0)
	{
		/* It's a static object. Dropping does nothing. */
	}
	else if (--s->refs == 0)
	{
		/* If we are dropping the last reference to an object, then
		 * it cannot possibly be in the store (as the store always
		 * keeps a ref to everything in it, and doesn't drop via
		 * this method. So we can simply drop the storable object
		 * itself without any operations on the fz_store. */
		do_free = 1;
	}
	fz_unlock(ctx, FZ_LOCK_ALLOC);
	if (do_free)
		s->free(ctx, s);
}

static void
evict(fz_context *ctx, fz_item *item)
{
	fz_store *store = ctx->store;
	int drop;

	store->size -= item->size;
	/* Unlink from the linked list */
	if (item->next)
		item->next->prev = item->prev;
	else
		store->tail = item->prev;
	if (item->prev)
		item->prev->next = item->next;
	else
		store->head = item->next;
	/* Drop a reference to the value (freeing if required) */
	drop = (item->val->refs > 0 && --item->val->refs == 0);
	fz_unlock(ctx, FZ_LOCK_ALLOC);
	/* Remove from the hash table */
	if (fz_is_indirect(item->key))
	{
		struct refkey refkey;
		refkey.free = item->val->free;
		refkey.num = fz_to_num(item->key);
		refkey.gen = fz_to_gen(item->key);
		fz_hash_remove(ctx, store->hash, &refkey);
	}
	if (drop)
		item->val->free(ctx, item->val);
	/* Always drops the key and free the item */
	fz_drop_obj(item->key);
	fz_free(ctx, item);
	fz_lock(ctx, FZ_LOCK_ALLOC);
}

static int
ensure_space(fz_context *ctx, unsigned int tofree)
{
	fz_item *item, *prev;
	unsigned int count;
	fz_store *store = ctx->store;

	fz_assert_lock_held(ctx, FZ_LOCK_ALLOC);

	/* First check that we *can* free tofree; if not, we'd rather not
	 * cache this. */
	count = 0;
	for (item = store->tail; item; item = item->prev)
	{
		if (item->val->refs == 1)
		{
			count += item->size;
			if (count >= tofree)
				break;
		}
	}

	/* If we ran out of items to search, then we can never free enough */
	if (item == NULL)
	{
		return 0;
	}

	/* Actually free the items */
	count = 0;
	for (item = store->tail; item; item = prev)
	{
		prev = item->prev;
		if (item->val->refs == 1)
		{
			/* Free this item. Evict has to drop the lock to
			 * manage that, which could cause prev to be removed
			 * in the meantime. To avoid that we bump its reference
			 * count here. This may cause another simultaneous
			 * evict process to fail to make enough space as prev is
			 * pinned - but that will only happen if we're near to
			 * the limit anyway, and it will only cause something to
			 * not be cached. */
			count += item->size;
			if (prev)
				prev->val->refs++;
			evict(ctx, item); /* Drops then retakes lock */
			/* So the store has 1 reference to prev, as do we, so
			 * no other evict process can have thrown prev away in
			 * the meantime. So we are safe to just decrement its
			 * reference count here. */
			--prev->val->refs;

			if (count >= tofree)
				return count;
		}
	}

	return count;
}

void
fz_store_item(fz_context *ctx, fz_obj *key, void *val_, unsigned int itemsize)
{
	fz_item *item = NULL;
	unsigned int size;
	fz_storable *val = (fz_storable *)val_;
	fz_store *store = ctx->store;
	int indirect;
	struct refkey refkey;

	if (!store)
		return;

	fz_var(item);

	/* Form the key before we take the lock */ 
	indirect = fz_is_indirect(key);
	if (indirect)
	{
		refkey.free = val->free;
		refkey.num = fz_to_num(key);
		refkey.gen = fz_to_gen(key);
	}

	/* If we fail for any reason, we swallow the exception and continue.
	 * All that the above program will see is that we failed to store
	 * the item. */
	fz_try(ctx)
	{
		item = fz_malloc_struct(ctx, fz_item);
	}
	fz_catch(ctx)
	{
		return;
	}

	fz_lock(ctx, FZ_LOCK_ALLOC);
	if (store->max != FZ_STORE_UNLIMITED)
	{
		size = store->size + itemsize;
		while (size > store->max)
		{
			/* ensure_space may drop, then retake the lock */
			if (ensure_space(ctx, size - store->max) == 0)
			{
				/* Failed to free any space */
				fz_unlock(ctx, FZ_LOCK_ALLOC);
				fz_free(ctx, item);
				return;
			}
		}
	}
	store->size += itemsize;

	item->key = fz_keep_obj(key);
	item->val = val;
	item->size = itemsize;
	item->next = NULL;

	/* If we can index it fast, put it into the hash table */
	if (indirect)
	{
		fz_unlock(ctx, FZ_LOCK_ALLOC);
		fz_try(ctx)
		{
			fz_hash_insert(ctx, store->hash, &refkey, item);
		}
		fz_catch(ctx)
		{
			fz_free(ctx, item);
			fz_lock(ctx, FZ_LOCK_ALLOC);
			store->size -= itemsize;
			fz_unlock(ctx, FZ_LOCK_ALLOC);
			return;
		}
		fz_lock(ctx, FZ_LOCK_ALLOC);
	}
	/* Now we can never fail, bump the ref */
	if (val->refs > 0)
		val->refs++;
	/* Regardless of whether it's indexed, it goes into the linked list */
	item->next = store->head;
	if (item->next)
		item->next->prev = item;
	else
		store->tail = item;
	store->head = item;
	item->prev = NULL;
	fz_unlock(ctx, FZ_LOCK_ALLOC);
}

void *
fz_find_item(fz_context *ctx, fz_store_free_fn *free, fz_obj *key)
{
	struct refkey refkey;
	fz_item *item;
	fz_store *store = ctx->store;
	int indirect;

	if (!store)
		return NULL;

	if (!key)
		return NULL;

	/* Form the key before we take the lock */
	indirect = fz_is_indirect(key);
	if (indirect)
	{
		refkey.free = free;
		refkey.num = fz_to_num(key);
		refkey.gen = fz_to_gen(key);
	}

	fz_lock(ctx, FZ_LOCK_ALLOC);
	if (indirect)
	{
		/* We can find objects keyed on indirected objects quickly */
		item = fz_hash_find(ctx, store->hash, &refkey);
	}
	else
	{
		/* Others we have to hunt for slowly */
		for (item = store->head; item; item = item->next)
		{
			if (item->val->free == free && !fz_objcmp(item->key, key))
				break;
		}
	}
	if (item)
	{
		/* LRU: Move the block to the front */
		/* Unlink from present position */
		if (item->next)
			item->next->prev = item->prev;
		else
			store->tail = item->prev;
		if (item->prev)
			item->prev->next = item->next;
		else
			store->head = item->next;
		/* Insert at head */
		item->next = store->head;
		if (item->next)
			item->next->prev = item;
		else
			store->tail = item;
		item->prev = NULL;
		store->head = item;
		/* And bump the refcount before returning */
		if (item->val->refs > 0)
			item->val->refs++;
		fz_unlock(ctx, FZ_LOCK_ALLOC);
		return (void *)item->val;
	}
	fz_unlock(ctx, FZ_LOCK_ALLOC);

	return NULL;
}

void
fz_remove_item(fz_context *ctx, fz_store_free_fn *free, fz_obj *key)
{
	struct refkey refkey;
	fz_item *item;
	fz_store *store = ctx->store;
	int drop, indirect;

	/* Form the key before we take the lock */
	indirect = fz_is_indirect(key);
	if (indirect)
	{
		refkey.free = free;
		refkey.num = fz_to_num(key);
		refkey.gen = fz_to_gen(key);
	}

	fz_lock(ctx, FZ_LOCK_ALLOC);
	if (indirect)
	{
		/* We can find objects keyed on indirect objects quickly */
		item = fz_hash_find(ctx, store->hash, &refkey);
		if (item)
			fz_hash_remove(ctx, store->hash, &refkey);
	}
	else
	{
		/* Others we have to hunt for slowly */
		for (item = store->head; item; item = item->next)
			if (item->val->free == free && !fz_objcmp(item->key, key))
				break;
	}
	if (item)
	{
		if (item->next)
			item->next->prev = item->prev;
		else
			store->tail = item->prev;
		if (item->prev)
			item->prev->next = item->next;
		else
			store->head = item->next;
		drop = (item->val->refs > 0 && --item->val->refs == 0);
		fz_unlock(ctx, FZ_LOCK_ALLOC);
		if (drop)
			item->val->free(ctx, item->val);
		fz_drop_obj(item->key);
		fz_free(ctx, item);
	}
	else
		fz_unlock(ctx, FZ_LOCK_ALLOC);
}

void
fz_empty_store(fz_context *ctx)
{
	fz_store *store = ctx->store;

	if (store == NULL)
		return;

	fz_lock(ctx, FZ_LOCK_ALLOC);
	/* Run through all the items in the store */
	while (store->head)
	{
		evict(ctx, store->head); /* Drops then retakes lock */
	}
	fz_unlock(ctx, FZ_LOCK_ALLOC);
}

fz_store *
fz_store_keep(fz_context *ctx)
{
	if (ctx == NULL || ctx->store == NULL)
		return NULL;
	fz_lock(ctx, FZ_LOCK_ALLOC);
	ctx->store->refs++;
	fz_unlock(ctx, FZ_LOCK_ALLOC);
	return ctx->store;
}

void
fz_free_store_context(fz_context *ctx)
{
	int refs;
	if (ctx == NULL || ctx->store == NULL)
		return;
	fz_lock(ctx, FZ_LOCK_ALLOC);
	refs = --ctx->store->refs;
	fz_unlock(ctx, FZ_LOCK_ALLOC);
	if (refs != 0)
		return;

	fz_empty_store(ctx);
	fz_free_hash(ctx, ctx->store->hash);
	fz_free(ctx, ctx->store);
	ctx->store = NULL;
}

void
fz_debug_store(fz_context *ctx)
{
	fz_item *item, *next;
	fz_store *store = ctx->store;

	printf("-- resource store contents --\n");

	fz_lock(ctx, FZ_LOCK_ALLOC);
	for (item = store->head; item; item = next)
	{
		next = item->next;
		next->val->refs++;
		printf("store[*][refs=%d][size=%d] ", item->val->refs, item->size);
		fz_unlock(ctx, FZ_LOCK_ALLOC);
		if (fz_is_indirect(item->key))
		{
			printf("(%d %d R) ", fz_to_num(item->key), fz_to_gen(item->key));
		} else
			fz_debug_obj(item->key);
		printf(" = %p\n", item->val);
		fz_lock(ctx, FZ_LOCK_ALLOC);
		next->val->refs--;
	}
	fz_unlock(ctx, FZ_LOCK_ALLOC);
}

/* This is now an n^2 algorithm - not ideal, but it'll only be bad if we are
 * actually managing to scavenge lots of blocks back. */
static int
scavenge(fz_context *ctx, unsigned int tofree)
{
	fz_store *store = ctx->store;
	unsigned int count = 0;
	fz_item *item, *prev;

	/* Free the items */
	for (item = store->tail; item; item = prev)
	{
		prev = item->prev;
		if (item->val->refs == 1)
		{
			/* Free this item */
			count += item->size;
			evict(ctx, item); /* Drops then retakes lock */

			if (count >= tofree)
				break;

			/* Have to restart search again, as prev may no longer
			 * be valid due to release of lock in evict. */
			prev = store->tail;
		}
	}
	/* Success is managing to evict any blocks */
	return count != 0;
}

int fz_store_scavenge(fz_context *ctx, unsigned int size, int *phase)
{
	fz_store *store;
	unsigned int max;

	if (ctx == NULL)
		return 0;
	store = ctx->store;
	if (store == NULL)
		return 0;

#ifdef DEBUG_SCAVENGING
	printf("Scavenging: store=%d size=%d phase=%d\n", store->size, size, *phase);
	fz_debug_store(ctx);
	Memento_stats();
#endif
	do
	{
		unsigned int tofree;

		/* Calculate 'max' as the maximum size of the store for this phase */
		if (*phase >= 16)
			max = 0;
		else if (store->max != FZ_STORE_UNLIMITED)
			max = store->max / 16 * (16 - *phase);
		else
			max = store->size / (16 - *phase) * (15 - *phase);
		(*phase)++;

		/* Slightly baroque calculations to avoid overflow */
		if (size > UINT_MAX - store->size)
			tofree = UINT_MAX - max;
		else if (size + store->size > max)
			continue;
		else
			tofree = size + store->size - max;

		if (scavenge(ctx, tofree))
		{
#ifdef DEBUG_SCAVENGING
			printf("scavenged: store=%d\n", store->size);
			fz_debug_store(ctx);
			Memento_stats();
#endif
			return 1;
		}
	}
	while (max > 0);

#ifdef DEBUG_SCAVENGING
	printf("scavenging failed\n");
	fz_debug_store(ctx);
	Memento_listBlocks();
#endif
	return 0;
}
