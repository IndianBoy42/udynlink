#ifndef __UDYNLINK_TRIE_H__
#define __UDYNLINK_TRIE_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDYNLINK_TRIE_NONE  0xFFFFU
#define UDYNLINK_TRIE_FLAG_LEAF       0x0100
#define UDYNLINK_TRIE_FLAG_HAS_CHILD  0x0200

typedef struct {
    uint16_t ch_flags;
    uint16_t sibling;
    uint16_t child;
    uint16_t leaf_index;
} udynlink_trie_node_t;

typedef struct {
    size_t num_nodes;
    size_t num_leaves;
    uint16_t root_child;
    const udynlink_trie_node_t *nodes;
    const uintptr_t *leaf_addrs;
} udynlink_trie_table_t;

static void *udynlink_resolve_trie_symbol(const udynlink_trie_table_t *table, const char *name) {
    if (table->num_nodes == 0)
        return NULL;

    uint16_t next_level = table->root_child;
    uint16_t matched_idx = UDYNLINK_TRIE_NONE;

    for (const char *p = name; *p; p++) {
        uint8_t c = (uint8_t)*p;
        uint16_t cur = next_level;
        matched_idx = UDYNLINK_TRIE_NONE;

        while (cur != UDYNLINK_TRIE_NONE) {
            const udynlink_trie_node_t *node = &table->nodes[cur];
            uint8_t node_ch = node->ch_flags & 0xFF;
            if (node_ch == c) {
                matched_idx = cur;
                next_level = node->child;
                break;
            }
            if (node_ch > c)
                return NULL;
            cur = node->sibling;
        }

        if (matched_idx == UDYNLINK_TRIE_NONE)
            return NULL;
    }

    if (matched_idx != UDYNLINK_TRIE_NONE &&
        (table->nodes[matched_idx].ch_flags & UDYNLINK_TRIE_FLAG_LEAF))
        return (void *)table->leaf_addrs[table->nodes[matched_idx].leaf_index];

    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif
