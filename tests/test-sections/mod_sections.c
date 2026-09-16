/* Module exercising multi-region placement: a 32-byte-aligned data buffer
 * tagged into the "alt" section and two functions tagged into the "altcode"
 * section. Both tagged sections are host-placed (one malloc callback call
 * each, named by section); every access from module code goes through the
 * LOT, so the module is placement-agnostic — only the host observes where
 * the sections landed. */
#include <stdint.h>
#include <stdio.h>
#include "udynlink_section.h"

#define ALT_BUF_WORDS 16

/* aligned(32) is the alignment the section table declares; the host pool
 * must honor it (today's untagged path cannot — that gap is the point of
 * the feature). */
UDYNLINK_SECTION_ALIGNED("alt", 32) uint32_t alt_buf[ALT_BUF_WORDS];

/* Functions in a second region. All calls (host -> wrapper, test -> body)
 * go through the LOT / same-region bl, never a cross-region bl. */
UDYNLINK_SECTION("altcode") int alt_add(int x) {
    for (int i = 0; i < ALT_BUF_WORDS; i++)
        alt_buf[i] = (uint32_t)(x + i);
    return x;
}

UDYNLINK_SECTION("altcode") uint32_t alt_sum(void) {
    uint32_t s = 0;
    for (int i = 0; i < ALT_BUF_WORDS; i++)
        s += alt_buf[i];
    return s;
}

int test(void) { printf("Running test 'mod_sections'\n"); return 1; }
