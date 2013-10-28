/*
 * Copyright 2010 Andrea Mazzoleni. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY ANDREA MAZZOLENI AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL ANDREA MAZZOLENI OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ucl_hash.h"
#include "utlist.h"

#include <string.h> /* for memset */
#include <assert.h> /* for assert */

/******************************************************************************/
/* hashlin */

/**
 * Reallocation states.
 */
#define UCL_HASHLIN_STATE_STABLE 0
#define UCL_HASHLIN_STATE_GROW 1
#define UCL_HASHLIN_STATE_SHRINK 2

static inline unsigned
ucl_ilog2_u32 (uint32_t value)
{
#if defined(_MSC_VER)
	unsigned long count;
	_BitScanReverse(&count, value);
	return count;
#elif defined(__GNUC__)
	/*
	 * GCC implements __builtin_clz(x) as "__builtin_clz(x) = bsr(x) ^ 31"
	 *
	 * Where "x ^ 31 = 31 - x", but gcc does not optimize "31 - __builtin_clz(x)" to bsr(x),
	 * but generates 31 - (bsr(x) xor 31).
	 *
	 * So we write "__builtin_clz(x) ^ 31" instead of "31 - __builtin_clz(x)".
	 */
	return __builtin_clz(value) ^ 31;
#else
	/* Find the log base 2 of an N-bit integer in O(lg(N)) operations with multiply and lookup */
	/* from http://graphics.stanford.edu/~seander/bithacks.html */
	static const int TOMMY_DE_BRUIJN_INDEX_ILOG2[32] = {
		0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
		8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
	};

	value |= value >> 1;
	value |= value >> 2;
	value |= value >> 4;
	value |= value >> 8;
	value |= value >> 16;

	return TOMMY_DE_BRUIJN_INDEX_ILOG2[(uint32_t)(value * 0x07C4ACDDU) >> 27];
#endif
}

ucl_hashlin*
ucl_hashlin_create (void)
{
	ucl_hashlin *new;

	new = UCL_ALLOC (sizeof (ucl_hashlin));
	if (new != NULL) {
		/* fixed initial size */
		new->bucket_bit = UCL_HASHLIN_BIT;
		new->bucket_max = 1 << new->bucket_bit;
		new->bucket_mask = new->bucket_max - 1;
		new->bucket[0] = UCL_ALLOC (new->bucket_max * sizeof(ucl_hash_node_t*));
		memset (new->bucket[0], 0,
				new->bucket_max * sizeof(ucl_hash_node_t*));
		new->bucket_mac = 1;

		/* stable state */
		new->state = UCL_HASHLIN_STATE_STABLE;

		new->count = 0;
	}
	return new;
}

void ucl_hashlin_destroy (ucl_hashlin* hashlin)
{
	/* we assume to be empty, so we free only the first bucket */
	assert(hashlin->bucket_mac == 1);

	UCL_FREE (hashlin->bucket_max * sizeof(ucl_hash_node_t*), hashlin->bucket[0]);
	UCL_FREE (sizeof (ucl_hashlin), hashlin);
}

/**
 * Return the bucket at the specified pos.
 */
static inline ucl_hash_node_t**
ucl_hashlin_pos (ucl_hashlin* hashlin,
		uint32_t pos)
{
	unsigned bsr;

	/* special case for the first bucket */
	if (pos < (1 << UCL_HASHLIN_BIT)) {
		return &hashlin->bucket[0][pos];
	}

	/* get the highest bit set */
	bsr = ucl_ilog2_u32 (pos);

	/* clear the highest bit */
	pos -= 1 << bsr;

	return &hashlin->bucket[bsr - UCL_HASHLIN_BIT + 1][pos];
}

/**
 * Return the bucket to use.
 */
static inline ucl_hash_node_t**
ucl_hashlin_bucket_ptr (ucl_hashlin* hashlin,
		uint32_t hash)
{
	unsigned pos;

	/* if we are reallocating */
	if (hashlin->state != UCL_HASHLIN_STATE_STABLE) {
		/* compute the old position */
		pos = hash & hashlin->low_mask;

		/* if we have not reallocated this position yet */
		if (pos >= hashlin->split) {

			/* use it as it was before */
			return ucl_hashlin_pos (hashlin, pos);
		}
	}

	/* otherwise operates normally */
	pos = hash & hashlin->bucket_mask;

	return ucl_hashlin_pos (hashlin, pos);
}

/**
 * Grow one step.
 */
static inline void
hashlin_grow_step (ucl_hashlin* hashlin)
{
	/* grow if more than 50% full */
	if (hashlin->state != UCL_HASHLIN_STATE_GROW
			&& hashlin->count > hashlin->bucket_max / 2) {
		/* if we are stable, setup a new grow state */
		/* otherwise continue with the already setup shrink one */
		/* but in backward direction */
		if (hashlin->state == UCL_HASHLIN_STATE_STABLE) {
			/* set the lower size */
			hashlin->low_max = hashlin->bucket_max;
			hashlin->low_mask = hashlin->bucket_mask;

			/* grow the hash size and allocate */
			++hashlin->bucket_bit;
			hashlin->bucket_max = 1 << hashlin->bucket_bit;
			hashlin->bucket_mask = hashlin->bucket_max - 1;
			hashlin->bucket[hashlin->bucket_mac] = UCL_ALLOC (hashlin->low_max * sizeof(ucl_hash_node_t*));
			++hashlin->bucket_mac;

			/* start from the beginning going forward */
			hashlin->split = 0;
		}

		/* grow state */
		hashlin->state = UCL_HASHLIN_STATE_GROW;
	}

	/* if we are growing */
	if (hashlin->state == UCL_HASHLIN_STATE_GROW) {
		/* compute the split target required to finish the reallocation before the next resize */
		unsigned split_target = 2 * hashlin->count;

		/* reallocate buckets until the split target */
		while (hashlin->split + hashlin->low_max < split_target) {
			ucl_hash_node_t** split[2];
			ucl_hash_node_t* j;
			unsigned mask;

			/* get the low bucket */
			split[0] = ucl_hashlin_pos (hashlin, hashlin->split);

			/* get the high bucket */
			/* it's always in the second half, so we can index it directly */
			/* without calling ucl_hashlin_pos() */
			split[1] =
					&hashlin->bucket[hashlin->bucket_mac - 1][hashlin->split];

			/* save the low bucket */
			j = *split[0];

			/* reinitialize the buckets */
			*split[0] = 0;
			*split[1] = 0;

			/* compute the bit to identify the bucket */
			mask = hashlin->bucket_mask & ~hashlin->low_mask;

			/* flush the bucket */
			while (j) {
				ucl_hash_node_t* j_next = j->next;
				unsigned index = (j->key & mask) != 0;
				if (*split[index]) {
					DL_APPEND (*split[index], j);
				}
				else {
					*split[index] = j;
					j->next = NULL;
				}
				j = j_next;
			}

			/* go forward */
			++hashlin->split;

			/* if we have finished, change the state */
			if (hashlin->split == hashlin->low_max) {
				hashlin->state = UCL_HASHLIN_STATE_STABLE;
				break;
			}
		}
	}
}

/**
 * Shrink one step.
 */
static inline void
hashlin_shrink_step (ucl_hashlin* hashlin)
{
	/* shrink if less than 12.5% full */
	if (hashlin->state != UCL_HASHLIN_STATE_SHRINK
			&& hashlin->count < hashlin->bucket_max / 8) {
		/* avoid to shrink the first bucket */
		if (hashlin->bucket_bit > UCL_HASHLIN_BIT) {
			/* if we are stable, setup a new shrink state */
			/* otherwise continue with the already setup grow one */
			/* but in backward direction */
			if (hashlin->state == UCL_HASHLIN_STATE_STABLE) {
				/* set the lower size */
				hashlin->low_max = hashlin->bucket_max / 2;
				hashlin->low_mask = hashlin->bucket_mask / 2;

				/* start from the half going backward */
				hashlin->split = hashlin->low_max;
			}

			/* start reallocation */
			hashlin->state = UCL_HASHLIN_STATE_SHRINK;
		}
	}

	/* if we are shrinking */
	if (hashlin->state == UCL_HASHLIN_STATE_SHRINK) {
		/* compute the split target required to finish the reallocation before the next resize */
		unsigned split_target = 8 * hashlin->count;

		/* reallocate buckets until the split target */
		while (hashlin->split + hashlin->low_max > split_target) {
			ucl_hash_node_t** split[2];

			/* go backward position */
			--hashlin->split;

			/* get the low bucket */
			split[0] = ucl_hashlin_pos (hashlin, hashlin->split);

			/* get the high bucket */
			/* it's always in the second half, so we can index it directly */
			/* without calling ucl_hashlin_pos() */
			split[1] =
					&hashlin->bucket[hashlin->bucket_mac - 1][hashlin->split];

			/* concat the high bucket into the low one */
			DL_CONCAT (*split[0], *split[1]);

			/* if we have finished, clean up and change the state */
			if (hashlin->split == 0) {
				hashlin->state = UCL_HASHLIN_STATE_STABLE;

				/* shrink the hash size */
				--hashlin->bucket_bit;
				hashlin->bucket_max = 1 << hashlin->bucket_bit;
				hashlin->bucket_mask = hashlin->bucket_max - 1;

				/* free the last segment */
				--hashlin->bucket_mac;
				UCL_FREE (hashlin->low_max * sizeof(ucl_hash_node_t*),
						hashlin->bucket[hashlin->bucket_mac]);
				break;
			}
		}
	}
}

void
ucl_hashlin_insert (ucl_hashlin* hashlin, ucl_hash_node_t* node,
		void* data, uint32_t hash)
{
	ucl_hash_node_t* bucket;

	bucket = ucl_hashlin_bucket (hashlin, hash);
	DL_APPEND (bucket, node);

	node->data = data;
	node->key = hash;

	++hashlin->count;

	hashlin_grow_step (hashlin);
}

void*
ucl_hashlin_remove_existing (ucl_hashlin* hashlin, ucl_hash_node_t* node)
{
	ucl_hash_node_t* bucket;

	bucket = ucl_hashlin_bucket (hashlin, node->key);

	DL_DELETE (bucket, node);

	--hashlin->count;

	hashlin_shrink_step (hashlin);

	return node->data;
}

ucl_hash_node_t*
ucl_hashlin_bucket (ucl_hashlin* hashlin, uint32_t hash)
{
	return *ucl_hashlin_bucket_ptr (hashlin, hash);
}

void*
ucl_hashlin_remove (ucl_hashlin* hashlin, ucl_hash_cmp_func* cmp,
		const void* cmp_arg, uint32_t hash)
{
	ucl_hash_node_t** let_ptr = ucl_hashlin_bucket_ptr (hashlin, hash);
	ucl_hash_node_t* i = *let_ptr;

	while (i) {
		/* we first check if the hash matches, as in the same bucket we may have multiples hash values */
		if (i->key == hash && cmp (cmp_arg, i->data) == 0) {
			DL_DELETE (*let_ptr, i);

			--hashlin->count;

			hashlin_shrink_step (hashlin);

			return i->data;
		}
		i = i->next;
	}

	return 0;
}

size_t
ucl_hashlin_memory_usage (ucl_hashlin* hashlin)
{
	return hashlin->bucket_max * (size_t) sizeof(hashlin->bucket[0][0])
			+ hashlin->count * (size_t) sizeof(ucl_hash_node_t);
}
