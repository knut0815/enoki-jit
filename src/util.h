/*
    src/util.h -- Parallel reductions and miscellaneous utility routines.

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <enoki-jit/jit.h>
#include "cuda_api.h"

/// Descriptive names for the various reduction operations
extern const char *reduction_name[(int) ReductionType::Count];

/// Fill a device memory region with constants of a given type
extern void jit_memset_async(void *ptr, uint32_t size, uint32_t isize,
                             const void *src);

/// Reduce the given array to a single value
extern void jit_reduce(VarType type, ReductionType rtype, const void *ptr,
                       uint32_t size, void *out);

/// 'All' reduction for boolean arrays
extern uint8_t jit_all(uint8_t *values, uint32_t size);

/// 'Any' reduction for uint8_tean arrays
extern uint8_t jit_any(uint8_t *values, uint32_t size);

/// Exclusive prefix sum
extern void jit_scan_u32(const uint32_t *in, uint32_t size, uint32_t *out);

/// Mask compression
extern uint32_t jit_compress(const uint8_t *in, uint32_t size, uint32_t *out);

/// Compute a permutation to reorder an integer array into discrete groups
extern uint32_t jit_mkperm(const uint32_t *values, uint32_t size,
                           uint32_t bucket_count, uint32_t *perm,
                           uint32_t *offsets);

/// Perform a synchronous copy operation
extern void jit_memcpy(void *dst, const void *src, size_t size);

/// Perform an assynchronous copy operation
extern void jit_memcpy_async(void *dst, const void *src, size_t size);

// Compute a permutation to reorder an array of registered pointers
extern VCallBucket *jit_vcall(const char *domain, uint32_t index,
                              uint32_t *bucket_count_out);

/// Replicate individual input elements to larger blocks
extern void jit_block_copy(enum VarType type, const void *in, void *out,
                           uint32_t size, uint32_t block_size);

/// Sum over elements within blocks
extern void jit_block_sum(enum VarType type, const void *in, void *out,
                          uint32_t size, uint32_t block_size);

/// Asynchronously update a single element in memory
extern void jit_poke(void *dst, const void *src, uint32_t size);
