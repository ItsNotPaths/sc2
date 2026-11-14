// The contract's rules, written once for libsc2 and the host.
#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "sc2.h"

// In sc2_screen, fields from key_transitions to nodes are input, written through incoming.
// From nodes to the end they are the program's, committed at each tick.
#define INPUT_START  offsetof(sc2_screen, key_transitions)
#define INPUT_SIZE   (offsetof(sc2_screen, nodes) - INPUT_START)
#define OUTPUT_START offsetof(sc2_screen, nodes)
#define OUTPUT_SIZE  (sizeof(sc2_screen) - OUTPUT_START)
#define NODE_COUNT   ((int)(sizeof(((sc2_screen *)0)->nodes) / sizeof(sc2_node)))

// A region's sequence is odd while its one writer writes it.
static inline void sequence_write_begin(uint32_t *sequence) {
    _Atomic uint32_t *atomic_sequence = (_Atomic uint32_t *)sequence;
    atomic_store_explicit(atomic_sequence, atomic_load_explicit(atomic_sequence, memory_order_relaxed) + 1,
                          memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static inline void sequence_write_end(uint32_t *sequence) {
    _Atomic uint32_t *atomic_sequence = (_Atomic uint32_t *)sequence;
    atomic_store_explicit(atomic_sequence, atomic_load_explicit(atomic_sequence, memory_order_relaxed) + 1,
                          memory_order_release);
}

static inline uint32_t sequence_read_begin(const uint32_t *sequence) {
    return atomic_load_explicit((const _Atomic uint32_t *)sequence, memory_order_acquire);
}

// True when everything read since sequence_read_begin came from one whole write.
static inline bool sequence_read_valid(const uint32_t *sequence, uint32_t begun) {
    atomic_thread_fence(memory_order_acquire);
    return begun % 2 == 0 && atomic_load_explicit((const _Atomic uint32_t *)sequence, memory_order_relaxed) == begun;
}

// Cells a cell node uses; 0 for other kinds and for grids that run past SC2_CELL_CAPACITY.
static inline uint32_t node_cell_count(const sc2_node *node) {
    uint32_t count = (uint32_t)node->content_width * node->content_height;
    if (node->kind != SC2_NODE_CELLS || node->storage_offset > SC2_CELL_CAPACITY
        || count > SC2_CELL_CAPACITY - node->storage_offset)
        return 0;
    return count;
}

// Bytes a pixel node reads; 0 for other kinds, for an offset not a multiple of 4, and for images that run past SC2_PIXEL_CAPACITY.
static inline uint32_t node_pixel_size(const sc2_node *node) {
    uint64_t size = (uint64_t)node->content_width * node->content_height * 4;
    if (node->kind != SC2_NODE_PIXELS || node->storage_offset % 4 || node->storage_offset > SC2_PIXEL_CAPACITY
        || size > SC2_PIXEL_CAPACITY - node->storage_offset)
        return 0;
    return (uint32_t)size;
}
