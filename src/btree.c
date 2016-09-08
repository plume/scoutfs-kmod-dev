/*
 * Copyright (C) 2016 Zach Brown.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/rwsem.h>
#include <linux/sort.h>

#include "super.h"
#include "format.h"
#include "block.h"
#include "key.h"
#include "btree.h"

/*
 * scoutfs stores file system metadata in btrees whose items have fixed
 * sized keys and variable length values.
 *
 * Items are stored as a small header with the key followed by the
 * value.  New items are allocated from the back of the block towards
 * the front.  Deleted items can be reclaimed by packing items towards
 * the back of the block by walking them in reverse offset order.
 *
 * A dense array of item offsets after the btree block header header
 * maintains the sorted order of the items by their keys.  The array is
 * small enough that the memmoves to keep it dense involves a few cache
 * lines at most.
 *
 * Parent blocks in the btree have the same format as leaf blocks.
 * There's one key for every child reference instead of having separator
 * keys between child references.  The key in a child reference contains
 * the largest key that may be found in the child subtree.  The right
 * spine of the tree has maximal keys so that they don't have to be
 * updated if we insert an item with a key greater than everything in
 * the tree.
 *
 * btree blocks, block references, and items all have sequence numbers
 * that are set to the current dirty btree sequence number when they're
 * modified.  This lets us efficiently search a range of keys for items
 * that are newer than a given sequence number.
 *
 * Operations are performed in one pass down the tree.  This lets us
 * cascade locks from the root down to the leaves and avoids having to
 * maintain a record of the path down the tree.  Splits and merges are
 * performed as we descend.
 *
 * XXX
 *  - do we want a level in the btree header?  seems like we would?
 *  - validate structures on read?
 *  - internal bh/pos/cmp interface is clumsy.. could use cursor
 */

/* number of contiguous bytes used by the item header and val of given len */
static inline unsigned int val_bytes(unsigned int val_len)
{
	return sizeof(struct scoutfs_btree_item) + val_len;
}

/* number of contiguous bytes used by the item header its current value */
static inline unsigned int item_bytes(struct scoutfs_btree_item *item)
{
	return val_bytes(le16_to_cpu(item->val_len));
}

/* total bytes consumed by an item with given val len: offset, header, value */
static inline unsigned int all_val_bytes(unsigned int val_len)
{
	return sizeof(((struct scoutfs_btree_block *)NULL)->item_offs[0]) +
	       val_bytes(val_len);
}

/* total bytes consumed by an item with its current value */
static inline unsigned int all_item_bytes(struct scoutfs_btree_item *item)
{
	return all_val_bytes(le16_to_cpu(item->val_len));
}

/* number of contig free bytes between item offset and first item */
static inline unsigned int contig_free(struct scoutfs_btree_block *bt)
{
	return le16_to_cpu(bt->free_end) -
	       offsetof(struct scoutfs_btree_block, item_offs[bt->nr_items]);
}

/* number of contig bytes free after reclaiming free amongst items */
static inline unsigned int reclaimable_free(struct scoutfs_btree_block *bt)
{
	return contig_free(bt) + le16_to_cpu(bt->free_reclaim);
}

/* all bytes used by item offsets, headers, and values */
static inline unsigned int used_total(struct scoutfs_btree_block *bt)
{
	return SCOUTFS_BLOCK_SIZE - sizeof(struct scoutfs_btree_block) -
	       reclaimable_free(bt);
}

static inline struct scoutfs_btree_item *
off_item(struct scoutfs_btree_block *bt, __le16 off)
{
	return (void *)bt + le16_to_cpu(off);
}

static inline struct scoutfs_btree_item *
pos_item(struct scoutfs_btree_block *bt, unsigned int pos)
{
	return off_item(bt, bt->item_offs[pos]);
}

static inline struct scoutfs_key *greatest_key(struct scoutfs_btree_block *bt)
{
	return &pos_item(bt, bt->nr_items - 1)->key;
}

/*
 * Returns the sorted item position that an item with the given key
 * should occupy.
 *
 * It sets *cmp to the final comparison of the given key and the
 * position's item key.
 *
 * If the given key is greater then all items' keys then the number of
 * items can be returned.  Callers need to be careful to test for this
 * invalid index.
 */
static int find_pos(struct scoutfs_btree_block *bt, struct scoutfs_key *key,
		    int *cmp)
{
	unsigned int start = 0;
	unsigned int end = bt->nr_items;
	unsigned int pos = 0;

	*cmp = -1;

	while (start < end) {
		pos = start + (end - start) / 2;

		*cmp = scoutfs_key_cmp(key, &pos_item(bt, pos)->key);
		if (*cmp < 0) {
			end = pos;
		} else if (*cmp > 0) {
			start = ++pos;
			*cmp = -1;
		} else {
			break;
		}
	}

	return pos;
}

/* move a number of contigous elements from the src index to the dst index */
#define memmove_arr(arr, dst, src, nr) \
	memmove(&(arr)[dst], &(arr)[src], (nr) * sizeof(*(arr)))

/*
 * Allocate and insert a new item into the block.  The caller has made
 * sure that there's room for everything.  The caller is responsible for
 * initializing the value.
 */
static struct scoutfs_btree_item *create_item(struct scoutfs_btree_block *bt,
					      unsigned int pos,
					      struct scoutfs_key *key,
					      unsigned int val_len)
{
	struct scoutfs_btree_item *item;

	if (pos < bt->nr_items)
		memmove_arr(bt->item_offs, pos + 1, pos, bt->nr_items - pos);

	le16_add_cpu(&bt->free_end, -val_bytes(val_len));
	bt->item_offs[pos] = bt->free_end;
	bt->nr_items++;

	item = pos_item(bt, pos);
	item->key = *key;
	item->seq = bt->hdr.seq;
	item->val_len = cpu_to_le16(val_len);

	trace_printk("pos %u off %u\n", pos, le16_to_cpu(bt->item_offs[pos]));

	return item;
}

/*
 * Delete an item from a btree block.  We record the amount of space it
 * frees to later decide if we can satisfy an insertion by compaction
 * instead of splitting.
 */
static void delete_item(struct scoutfs_btree_block *bt, unsigned int pos)
{
	struct scoutfs_btree_item *item = pos_item(bt, pos);

	trace_printk("pos %u off %u\n", pos, le16_to_cpu(bt->item_offs[pos]));

	if (pos < (bt->nr_items - 1))
		memmove_arr(bt->item_offs, pos, pos + 1,
			    bt->nr_items - 1 - pos);

	le16_add_cpu(&bt->free_reclaim, item_bytes(item));
	bt->nr_items--;

	/* wipe deleted items to avoid leaking data */
	memset(item, 0, item_bytes(item));
}

/*
 * Move items from a source block to a destination block.  The caller
 * tells us if we're moving from the tail of the source block right to
 * the head of the destination block, or vice versa.  We stop moving
 * once we've moved enough bytes of items.
 */
static void move_items(struct scoutfs_btree_block *dst,
		       struct scoutfs_btree_block *src, bool move_right,
		       int to_move)
{
	struct scoutfs_btree_item *from;
	struct scoutfs_btree_item *to;
	unsigned int t;
	unsigned int f;

	if (move_right) {
		f = src->nr_items - 1;
		t = 0;
	} else {
		f = 0;
		t = dst->nr_items;
	}

	while (f < src->nr_items && to_move > 0) {
		from = pos_item(src, f);

		to = create_item(dst, t, &from->key,
				 le16_to_cpu(from->val_len));

		memcpy(to, from, item_bytes(from));
		to_move -= all_item_bytes(from);

		delete_item(src, f);
		if (move_right)
			f--;
		else
			t++;
	}
}

static struct scoutfs_btree_block *aligned_bt(const void *ptr)
{
	unsigned long addr = (unsigned long)ptr;

	return (void *)(addr & ~((unsigned long)SCOUTFS_BLOCK_MASK));
}

static int sort_key_cmp(const void *A, const void *B)
{
	struct scoutfs_btree_block *bt = aligned_bt(A);
	const __le16 * __packed a = A;
	const __le16 * __packed b = B;

	return scoutfs_key_cmp(&off_item(bt, *a)->key, &off_item(bt, *b)->key);
}

static int sort_off_cmp(const void *A, const void *B)
{
	const __le16 * __packed a = A;
	const __le16 * __packed b = B;

	return (int)le16_to_cpu(*a) - (int)le16_to_cpu(*b);
}

static void sort_off_swap(void *A, void *B, int size)
{
	__le16 * __packed a = A;
	__le16 * __packed b = B;

	swap(*a, *b);
}

/*
 * As items are deleted they create fragmented free space.  Even if we
 * indexed free space in the block it could still get sufficiently
 * fragmented to force a split on insertion even though the two
 * resulting blocks would have less than the minimum space consumed by
 * items.
 *
 * We don't bother implementing free space indexing and addressing that
 * corner case.  Instead we track the number of bytes that could be
 * reclaimed if we compacted the item space after the free_end offset.
 * block.  If this additional free space would satisfy an insertion then
 * we compact the items instead of splitting the block.
 *
 * We move the free space to the center of the block by walking
 * backwards through the items in offset order, moving items into free
 * space between items towards the end of the block.
 *
 * We don't have specific metadata to either walk the items in offset
 * order or to update the item offsets as we move items.  We sort the
 * item offset array to achieve both ends.  First we sort it by offset
 * so we can walk in reverse order.  As we move items we update their
 * position and then sort by keys once we're done.
 *
 * Compaction is only attempted during descent as we find a block that
 * needs more or less free space.  The caller has the parent locked for
 * writing and there are no references to the items at this point so
 * it's safe to scramble the block contents.
 */
static void compact_items(struct scoutfs_btree_block *bt)
{
	struct scoutfs_btree_item *from;
	struct scoutfs_btree_item *to;
	unsigned int bytes;
	__le16 end;
	int i;

	trace_printk("free_reclaim %u\n", le16_to_cpu(bt->free_reclaim));

	sort(bt->item_offs, bt->nr_items, sizeof(bt->item_offs[0]),
	     sort_off_cmp, sort_off_swap);

	end = cpu_to_le16(SCOUTFS_BLOCK_SIZE);

	for (i = bt->nr_items - 1; i >= 0; i--) {
		from = pos_item(bt, i);

		bytes = item_bytes(from);
		le16_add_cpu(&end, -bytes);
		to = off_item(bt, end);
		bt->item_offs[i] = end;

		if (from != to)
			memmove(to, from, bytes);
	}

	bt->free_end = end;
	bt->free_reclaim = 0;

	sort(bt->item_offs, bt->nr_items, sizeof(bt->item_offs[0]),
	     sort_key_cmp, sort_off_swap);
}

/* sorting relies on masking pointers to find the containing block */
static inline struct buffer_head *check_bh_alignment(struct buffer_head *bh)
{
	struct scoutfs_btree_block *bt = bh_data(bh);

	if (!IS_ERR_OR_NULL(bh) && WARN_ON_ONCE(aligned_bt(bt) != bt)) {
		scoutfs_block_put(bh);
		return ERR_PTR(-EIO);
	}

	return bh;
}

/*
 * Allocate and initialize a new tree block. The caller adds references
 * to it.
 */
static struct buffer_head *alloc_tree_block(struct super_block *sb)
{
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;

	bh = scoutfs_block_dirty_alloc(sb);
	if (!IS_ERR(bh)) {
		bt = bh_data(bh);

		bt->free_end = cpu_to_le16(SCOUTFS_BLOCK_SIZE);
		bt->free_reclaim = 0;
		bt->nr_items = 0;
	}

	return check_bh_alignment(bh);
}

/* the caller has ensured that the free must succeed */
static void free_tree_block(struct super_block *sb, __le64 blkno)
{
	int err = scoutfs_buddy_free(sb, le64_to_cpu(blkno), 0);
	WARN_ON_ONCE(err);
}

/*
 * Allocate a new tree block and point the root at it.  The caller
 * is responsible for the items in the new root block.
 */
static struct buffer_head *grow_tree(struct super_block *sb,
				       struct scoutfs_btree_root *root)
{
	struct scoutfs_block_header *hdr;
	struct buffer_head *bh;

	bh = alloc_tree_block(sb);
	if (!IS_ERR(bh)) {
		hdr = bh_data(bh);

		root->height++;
		root->ref.blkno = hdr->blkno;
		root->ref.seq = hdr->seq;
	}

	return bh;
}

static struct buffer_head *get_block_ref(struct super_block *sb,
				         struct scoutfs_block_ref *ref,
				         bool dirty)
{
	struct buffer_head *bh;

	if (dirty)
		bh = scoutfs_block_dirty_ref(sb, ref);
	else
		bh = scoutfs_block_read_ref(sb, ref);

	return check_bh_alignment(bh);
}

/*
 * Create a new item in the parent which references the child.  The caller
 * specifies the key in the item that describes the items in the child.
 */
static void create_parent_item(struct scoutfs_btree_block *parent,
			       unsigned int pos,
			       struct scoutfs_btree_block *child,
			       struct scoutfs_key *key)
{
	struct scoutfs_btree_item *item;
	struct scoutfs_block_ref ref = {
		.blkno = child->hdr.blkno,
		.seq = child->hdr.seq,
	};

	item = create_item(parent, pos, key, sizeof(ref));
	memcpy(&item->val, &ref, sizeof(ref));
}

/*
 * See if we need to split this block while descending for insertion so
 * that we have enough space to insert.
 *
 * Parent blocks need enough space for a new item and child ref if a
 * child block splits.  Leaf blocks need enough space to insert the new
 * item with its value.
 *
 * We split to the left so that the greatest key in the existing block
 * doesn't change so we don't have to update the key in its parent item.
 *
 * If the search key falls in the new split block then we return it
 * to the caller to walk through.
 *
 * The locking in the case where we add the first parent is a little wonky.
 * We're creating a parent block that the walk doesn't know about.  It
 * holds the tree mutex while we add the parent ref and then will lock
 * the child that we return.  It's skipping locking the new parent as it
 * descends but that's fine.
 */
static struct buffer_head *try_split(struct super_block *sb,
				       struct scoutfs_btree_root *root,
				       int level, struct scoutfs_key *key,
				       unsigned int val_len,
				       struct scoutfs_btree_block *parent,
				       unsigned int parent_pos,
				       struct buffer_head *right_bh)
{
	struct scoutfs_btree_block *right = bh_data(right_bh);
	struct scoutfs_btree_block *left;
	struct buffer_head *left_bh;
	struct buffer_head *par_bh = NULL;
	struct scoutfs_key maximal;
	unsigned int all_bytes;

	if (level)
		val_len = sizeof(struct scoutfs_block_ref);
	all_bytes = all_val_bytes(val_len);

	if (contig_free(right) >= all_bytes)
		return right_bh;

	if (reclaimable_free(right) >= all_bytes) {
		compact_items(right);
		return right_bh;
	}

	/* alloc split neighbour first to avoid unwinding tree growth */
	left_bh = alloc_tree_block(sb);
	if (IS_ERR(left_bh)) {
		scoutfs_block_put(right_bh);
		return left_bh;
	}
	left = bh_data(left_bh);

	if (!parent) {
		par_bh = grow_tree(sb, root);
		if (IS_ERR(par_bh)) {
			free_tree_block(sb, left->hdr.blkno);
			scoutfs_block_put(left_bh);
			scoutfs_block_put(right_bh);
			return par_bh;
		}

		parent = bh_data(par_bh);
		parent_pos = 0;

		scoutfs_set_max_key(&maximal);
		create_parent_item(parent, parent_pos, right, &maximal);
	}

	move_items(left, right, false, used_total(right) / 2);
	create_parent_item(parent, parent_pos, left, greatest_key(left));
	parent_pos++; /* not that anything uses it again :P */

	if (scoutfs_key_cmp(key, greatest_key(left)) <= 0) {
		/* insertion will go to the new left block */
		scoutfs_block_put(right_bh);
		right_bh = left_bh;
	} else {
		scoutfs_block_put(left_bh);

		/* insertion will still go through us, might need to compact */
		if (contig_free(right) < all_bytes)
			compact_items(right);
	}

	scoutfs_block_put(par_bh);

	return right_bh;
}

/*
 * This is called during descent for deletion when we have a parent and
 * might need to merge items from a sibling block if this block has too
 * much free space.  Eventually we'll be able to fit all of the
 * sibling's items in our free space which lets us delete the sibling
 * block.
 *
 * The error handling here is a little weird.  We're returning an
 * ERR_PTR buffer to match splitting so that the walk can handle errors
 * from both easily.  We have to unlock and release our buffer to return
 * an error.
 *
 * The caller only has the parent locked.  They'll lock whichever
 * block we return.
 *
 * We free sibling or parent btree block blknos if we drain them of items.
 * They're dirtied either by descent or before we start migrating items
 * so freeing their blkno must succeed.
 *
 * XXX this could more cleverly chose a merge candidate sibling
 */
static struct buffer_head *try_merge(struct super_block *sb,
				     struct scoutfs_btree_root *root,
				     struct scoutfs_btree_block *parent,
				     unsigned int pos,
				     struct buffer_head *bh)
{
	struct scoutfs_btree_block *bt = bh_data(bh);
	struct scoutfs_btree_item *sib_item;
	struct scoutfs_btree_block *sib_bt;
	struct buffer_head *sib_bh;
	unsigned int sib_pos;
	bool move_right;
	int to_move;

	if (reclaimable_free(bt) <= SCOUTFS_BTREE_FREE_LIMIT)
		return bh;

	/* move items right into our block if we have a left sibling */
	if (pos) {
		sib_pos = pos - 1;
		move_right = true;
	} else {
		sib_pos = pos + 1;
		move_right = false;
	}
	sib_item = pos_item(parent, sib_pos);

	sib_bh = get_block_ref(sb, (void *)sib_item->val, true);
	if (IS_ERR(sib_bh)) {
		/* XXX do we need to unlock this?  don't think so */
		scoutfs_block_put(bh);
		return sib_bh;
	}
	sib_bt = bh_data(sib_bh);

	if (used_total(sib_bt) <= reclaimable_free(bt))
		to_move = used_total(sib_bt);
	else
		to_move = reclaimable_free(bt) - SCOUTFS_BTREE_FREE_LIMIT;

	if (contig_free(bt) < to_move)
		compact_items(bt);

	trace_printk("sib_pos %d move_right %u to_move %u\n",
		     sib_pos, move_right, to_move);

	move_items(bt, sib_bt, move_right, to_move);

	/* update our parent's ref if we changed our greatest key */
	if (!move_right)
		pos_item(parent, pos)->key = *greatest_key(bt);

	/* delete an empty sib or update if we changed its greatest key */
	if (sib_bt->nr_items == 0) {
		delete_item(parent, sib_pos);
		free_tree_block(sb, sib_bt->hdr.blkno);
	} else if (move_right) {
		sib_item->key = *greatest_key(sib_bt);
	}

	/* and finally shrink the tree if our parent is the root with 1 */
	if (parent->nr_items == 1) {
		root->height--;
		root->ref.blkno = bt->hdr.blkno;
		root->ref.seq = bt->hdr.seq;
		free_tree_block(sb, parent->hdr.blkno);
	}

	return bh;
}

enum {
	WALK_INSERT = 1,
	WALK_DELETE,
	WALK_NEXT,
	WALK_NEXT_SEQ,
	WALK_DIRTY,
};

static inline void lock_root(struct scoutfs_sb_info *sbi, bool dirty)
{
	if (dirty)
		down_write(&sbi->btree_rwsem);
	else
		down_read(&sbi->btree_rwsem);
}

static inline void unlock_root(struct scoutfs_sb_info *sbi, bool dirty)
{
	if (dirty)
		up_write(&sbi->btree_rwsem);
	else
		up_read(&sbi->btree_rwsem);
}

/*
 * As we descend we lock parent blocks (or the root), then lock the child,
 * then unlock the parent.
 */
static inline void lock_block(struct scoutfs_sb_info *sbi,
			      struct buffer_head *bh, bool dirty)
{
	if (bh == NULL)
		lock_root(sbi, dirty);
	else
		lock_buffer(bh);
}

static inline void unlock_block(struct scoutfs_sb_info *sbi,
				struct buffer_head *bh, bool dirty)
{
	if (bh == NULL)
		unlock_root(sbi, dirty);
	else
		unlock_buffer(bh);
}

static u64 item_block_ref_seq(struct scoutfs_btree_item *item)
{
	struct scoutfs_block_ref *ref = (void *)item->val;

	return le64_to_cpu(ref->seq);
}

/*
 * Return true if we should skip this item while iterating by sequence
 * number.  If it's a parent then we test the block ref's seq, if it's a
 * leaf item then we check the item's seq.
 */
static bool skip_pos_seq(struct scoutfs_btree_block *bt, unsigned int pos,
			 int level, u64 seq, int op)
{
	struct scoutfs_btree_item *item;

	if (op != WALK_NEXT_SEQ || pos >= bt->nr_items)
	       return false;

	item = pos_item(bt, pos);

	return ((level > 0 && item_block_ref_seq(item) < seq) ||
	        (level == 0 && le64_to_cpu(item->seq) < seq));
}

/*
 * Return the next sorted item position, possibly skipping those with
 * sequence numbers less than the desired sequence number.
 */
static unsigned int next_pos_seq(struct scoutfs_btree_block *bt,
				 unsigned int pos, int level, u64 seq, int op)
{
	do {
		pos++;
	} while (skip_pos_seq(bt, pos, level, seq, op));

	return pos;
}

/*
 * Return the first item after the given key, possibly skipping those
 * with sequence numbers less than the desired sequence number.
 */
static unsigned int find_pos_after_seq(struct scoutfs_btree_block *bt,
				       struct scoutfs_key *key, int level,
				       u64 seq, int op)
{
	unsigned int pos;
	int cmp;

	pos = find_pos(bt, key, &cmp);
	if (skip_pos_seq(bt, pos, level, seq, op))
		pos = next_pos_seq(bt, pos, level, seq, op);

	return pos;
}

/*
 * Return the leaf block that should contain the given key.  The caller
 * is responsible for searching the leaf block and performing their
 * operation.  The block is returned locked for either reading or
 * writing depending on the operation.
 *
 * As we descend through parent items we set next_key to the first key
 * in the next sibling's block.  This is used by iteration to advance to
 * the next block when they're done with the block this returns.
 */
static struct buffer_head *btree_walk(struct super_block *sb,
					struct scoutfs_key *key,
					struct scoutfs_key *next_key,
					unsigned int val_len, u64 seq, int op)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_btree_block *parent = NULL;
	struct scoutfs_btree_root *root;
	struct buffer_head *par_bh = NULL;
	struct buffer_head *bh = NULL;
	struct scoutfs_btree_item *item = NULL;
	struct scoutfs_block_ref *ref;
	unsigned int level;
	unsigned int pos = 0;
	const bool dirty = op == WALK_INSERT || op == WALK_DELETE ||
			   op == WALK_DIRTY;

	/* no sibling blocks if we don't have parent blocks */
	if (next_key)
		scoutfs_set_max_key(next_key);

	lock_block(sbi, par_bh, dirty);

	/* XXX one for now */
	root = &sbi->super.btree_root;
	ref = &root->ref;
	level = root->height;

	if (!root->height) {
		if (op == WALK_INSERT) {
			bh = ERR_PTR(-ENOENT);
		} else {
			bh = grow_tree(sb, root);
			if (!IS_ERR(bh))
				lock_block(sbi, bh, dirty);
		}
		unlock_block(sbi, par_bh, dirty);
		return bh;
	}


	/* skip the whole tree if the root ref's seq is old */
	if (op == WALK_NEXT_SEQ && le64_to_cpu(ref->seq) < seq) {
		unlock_block(sbi, par_bh, dirty);
		return ERR_PTR(-ENOENT);
	}

	while (level--) {
		/* XXX hmm, need to think about retry */
		bh = get_block_ref(sb, ref, dirty);
		if (IS_ERR(bh))
			break;

		if (op == WALK_INSERT)
			bh = try_split(sb, root, level, key, val_len, parent,
				       pos, bh);
		if ((op == WALK_DELETE) && parent)
			bh = try_merge(sb, root, parent, pos, bh);
		if (IS_ERR(bh))
			break;

		lock_block(sbi, bh, dirty);

		if (!level)
			break;

		/* unlock parent before searching so others can use it */
		unlock_block(sbi, par_bh, dirty);
		scoutfs_block_put(par_bh);
		par_bh = bh;
		parent = bh_data(par_bh);

		/*
		 * Find the parent item that references the next child
		 * block to search.  If we're skipping items with old
		 * seqs then we might not have any child items to
		 * search.
		 */
		pos = find_pos_after_seq(parent, key, level, seq, op);
		if (pos >= parent->nr_items) {
			/* current block dropped as parent below */
			if (op == WALK_NEXT_SEQ)
				bh = ERR_PTR(-ENOENT);
			else
				bh = ERR_PTR(-EIO);
			break;
		}

		/* XXX verify sane length */
		item = pos_item(parent, pos);
		ref = (void *)item->val;

		/*
		 * Update the next key an iterator should read from.
		 * Keep in mind that iteration is read only so the
		 * parent item won't be changed splitting or merging.
		 */
		if (next_key) {
			*next_key = item->key;
			scoutfs_inc_key(next_key);
		}
	}

	unlock_block(sbi, par_bh, dirty);
	scoutfs_block_put(par_bh);

	return bh;
}

static void set_cursor(struct scoutfs_btree_cursor *curs,
		       struct buffer_head *bh, unsigned int pos, bool write)
{
	struct scoutfs_btree_block *bt = bh_data(bh);
	struct scoutfs_btree_item *item = pos_item(bt, pos);

	curs->bh = bh;
	curs->pos = pos;
	curs->write = write;

	curs->key = &item->key;
	curs->seq = le64_to_cpu(item->seq);
	curs->val = item->val;
	curs->val_len = le16_to_cpu(item->val_len);
}

/*
 * Point the caller's cursor at the item if it's found.  It can't be
 * modified.  -ENOENT is returned if the key isn't found in the tree.
 */
int scoutfs_btree_lookup(struct super_block *sb, struct scoutfs_key *key,
			 struct scoutfs_btree_cursor *curs)
{
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;
	unsigned int pos;
	int cmp;
	int ret;

	BUG_ON(curs->bh);

	bh = btree_walk(sb, key, NULL, 0, 0, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	bt = bh_data(bh);

	pos = find_pos(bt, key, &cmp);
	if (cmp == 0) {
		set_cursor(curs, bh, pos, false);
		ret = 0;
	} else {
		unlock_block(NULL, bh, false);
		scoutfs_block_put(bh);
		ret = -ENOENT;
	}

	return ret;
}

/*
 * Insert a new item in the tree and point the caller's cursor at it.
 * The caller is responsible for setting the value.
 *
 * -EEXIST is returned if the key is already present in the tree.
 *
 * XXX this walks the treap twice, which isn't great
 */
int scoutfs_btree_insert(struct super_block *sb, struct scoutfs_key *key,
			 unsigned int val_len,
			 struct scoutfs_btree_cursor *curs)
{
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;
	int pos;
	int cmp;
	int ret;

	BUG_ON(curs->bh);

	bh = btree_walk(sb, key, NULL, val_len, 0, WALK_INSERT);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	bt = bh_data(bh);

	pos = find_pos(bt, key, &cmp);
	if (cmp) {
		create_item(bt, pos, key, val_len);
		set_cursor(curs, bh, pos, true);
		ret = 0;
	} else {
		unlock_block(NULL, bh, true);
		scoutfs_block_put(bh);
		ret = -EEXIST;
	}

	return ret;
}

/*
 * Delete an item from the tree.  -ENOENT is returned if the key isn't
 * found.
 */
int scoutfs_btree_delete(struct super_block *sb, struct scoutfs_key *key)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_btree_root *root;
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;
	int pos;
	int cmp;
	int ret;

	bh = btree_walk(sb, key, NULL, 0, 0, WALK_DELETE);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	bt = bh_data(bh);

	pos = find_pos(bt, key, &cmp);
	if (cmp == 0) {
		delete_item(bt, pos);
		ret = 0;

		/* XXX this locking is broken.. hold root rwsem? */

		/* delete the final block in the tree */
		if (bt->nr_items == 0) {
			root = &sbi->super.btree_root;

			root->height = 0;
			root->ref.blkno = 0;
			root->ref.seq = 0;

			free_tree_block(sb, bt->hdr.blkno);
		}
	} else {
		ret = -ENOENT;
	}

	unlock_block(NULL, bh, true);
	scoutfs_block_put(bh);

	return ret;
}

/*
 * Iterate over items in the tree starting with first and ending with
 * last.  We point the cursor at each item and return to the caller.
 * The caller continues the search with the cursor.
 *
 * The caller can limit results to items with a sequence number greater
 * than or equal to their sequence number.
 *
 * When there isn't an item in the cursor then we walk the btree to the
 * leaf that should contain the key and look for items from there.  When
 * we exhaust leaves we search the tree again from the next key that was
 * increased past the leaf's parent's item.
 *
 * Returns > 0 when the cursor has an item, 0 when done, and -errno on error.
 */
static int btree_next(struct super_block *sb, struct scoutfs_key *first,
		      struct scoutfs_key *last, u64 seq, int op,
		      struct scoutfs_btree_cursor *curs)
{
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;
	struct scoutfs_key key = *first;
	struct scoutfs_key next_key;
	int ret;

	if (scoutfs_key_cmp(first, last) > 0)
		return 0;

	/* find the next item after the cursor, releasing if we're done */
	if (curs->bh) {
		bt = bh_data(curs->bh);
		key = *curs->key;
		scoutfs_inc_key(&key);

		curs->pos = next_pos_seq(bt, curs->pos, 0, seq, op);
		if (curs->pos < bt->nr_items)
			set_cursor(curs, curs->bh, curs->pos, curs->write);
		else
			scoutfs_btree_release(curs);
	}

	/* find the leaf that contains the next item after the key */
	while (!curs->bh && scoutfs_key_cmp(&key, last) <= 0) {

		bh = btree_walk(sb, &key, &next_key, 0, seq, op);

		/* next seq walks can terminate in parents with old seqs */
		if (op == WALK_NEXT_SEQ && bh == ERR_PTR(-ENOENT)) {
			key = next_key;
			continue;
		}

		if (IS_ERR(bh)) {
			if (bh == ERR_PTR(-ENOENT))
				break;
			return PTR_ERR(bh);
		}
		bt = bh_data(bh);

		/* keep trying leaves until next_key passes last */
		curs->pos = find_pos_after_seq(bt, &key, 0, seq, op);
		if (curs->pos >= bt->nr_items) {
			key = next_key;
			unlock_block(NULL, bh, false);
			scoutfs_block_put(bh);
			continue;
		}

		set_cursor(curs, bh, curs->pos, false);
		break;
	}

	/* only return the next item if it's within last */
	if (curs->bh && scoutfs_key_cmp(curs->key, last) <= 0) {
		ret = 1;
	} else {
		scoutfs_btree_release(curs);
		ret = 0;
	}

	return ret;
}

int scoutfs_btree_next(struct super_block *sb, struct scoutfs_key *first,
		       struct scoutfs_key *last,
		       struct scoutfs_btree_cursor *curs)
{
	return btree_next(sb, first, last, 0, WALK_NEXT, curs);
}

int scoutfs_btree_since(struct super_block *sb, struct scoutfs_key *first,
		        struct scoutfs_key *last, u64 seq,
		        struct scoutfs_btree_cursor *curs)
{
	return btree_next(sb, first, last, seq, WALK_NEXT_SEQ, curs);
}

/*
 * Ensure that the blocks that lead to the item with the given key are
 * dirty.  caller can hold a transaction to pin the dirty blocks and
 * guarantee that later updates of the item will succeed.
 *
 * <0 is returned on error, including -ENOENT if the key isn't present.
 */
int scoutfs_btree_dirty(struct super_block *sb, struct scoutfs_key *key)
{
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;
	int cmp;
	int ret;

	bh = btree_walk(sb, key, NULL, 0, 0, WALK_DIRTY);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	bt = bh_data(bh);

	find_pos(bt, key, &cmp);
	if (cmp == 0) {
		ret = 0;
	} else {
		ret = -ENOENT;
	}

	unlock_block(NULL, bh, true);
	scoutfs_block_put(bh);

	return ret;
}

/*
 * This is guaranteed not to fail if the caller has already dirtied the
 * block that contains the item in the current transaction.
 */
int scoutfs_btree_update(struct super_block *sb, struct scoutfs_key *key,
			 struct scoutfs_btree_cursor *curs)
{
	struct scoutfs_btree_item *item;
	struct scoutfs_btree_block *bt;
	struct buffer_head *bh;
	int pos;
	int cmp;
	int ret;

	BUG_ON(curs->bh);

	bh = btree_walk(sb, key, NULL, 0, 0, WALK_DIRTY);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	bt = bh_data(bh);

	pos = find_pos(bt, key, &cmp);
	if (cmp == 0) {
		item = pos_item(bt, pos);
		item->seq = bt->hdr.seq;
		set_cursor(curs, bh, pos, true);
		ret = 0;
	} else {
		unlock_block(NULL, bh, true);
		scoutfs_block_put(bh);
		ret = -ENOENT;
	}

	return ret;
}

void scoutfs_btree_release(struct scoutfs_btree_cursor *curs)
{
	if (curs->bh) {
		unlock_block(NULL, curs->bh, curs->write);
		scoutfs_block_put(curs->bh);
	}
	curs->bh = NULL;
}

/*
 * Find the first missing key between the caller's keys, inclusive.  Set
 * the caller's hole key and return 0 if we find a missing key.  Return
 * -ENOSPC if all the keys in the range were present or -errno on errors.
 *
 * The caller ensures that it's safe for us to be walking this region
 * of the tree.
 */
int scoutfs_btree_hole(struct super_block *sb, struct scoutfs_key *first,
		       struct scoutfs_key *last, struct scoutfs_key *hole)
{
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	int ret;

	*hole = *first;
	while ((ret = scoutfs_btree_next(sb, first, last, &curs)) > 0) {
		/* return our expected hole if we skipped it */
		if (scoutfs_key_cmp(hole, curs.key) < 0)
			break;

		*hole = *curs.key;
		scoutfs_inc_key(hole);
	}
	scoutfs_btree_release(&curs);

	if (ret >= 0) {
		if (scoutfs_key_cmp(hole, last) <= 0)
			ret = 0;
		else
			ret = -ENOSPC;
	}

	return ret;
}