/* Module-facing API for multi-region memory placement.
 *
 * These macros tag module objects and functions into named placement
 * sections (.udynlink.sec.<name>) which mkmodule collects into dedicated
 * linker regions when the module is built with --section.  At load time the
 * host resolves each section to an address through its allocator callbacks,
 * so a DMA buffer can live in non-cacheable RAM, a hot function in tightly
 * coupled memory, and so on.
 *
 * This header is deliberately self-contained (no includes) so module builds
 * need just one -I entry pointing at the udynlink include dir — module
 * sources must not be forced to include the host-facing udynlink.h.
 *
 * The <name> declared here must match a --section <name> passed to
 * mkmodule; an undeclared section is dropped from the image.
 *
 * Copyright (c) 2026 udynlink contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __UDYNLINK_SECTION_H__
#define __UDYNLINK_SECTION_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Module writer API ─────────────────────────────────────────────── */

/**
 * Place an object or function into the named placement section.
 *
 * @param name section tag, as a string literal (must be declared to
 *             mkmodule via --section <name>)
 *
 * Works on both data objects and functions; whether the section counts as
 * code or data is derived from what it contains, not from this attribute.
 */
#define UDYNLINK_SECTION(name)             __attribute__((section(".udynlink.sec." name)))

/**
 * Place an object or function into the named placement section and raise
 * its alignment requirement to @p al bytes (power of two).
 *
 * @param name section tag, as a string literal (must be declared to
 *             mkmodule via --section <name>)
 * @param al   alignment in bytes, power of two; the host must place the
 *             section at an address at least this aligned
 *
 * Note: inside the default .data/.bss sections aligned(N) with N > 4 is
 * not honored at runtime; guaranteed alignment requires a tagged section.
 */
#define UDYNLINK_SECTION_ALIGNED(name, al) __attribute__((section(".udynlink.sec." name), aligned(al)))

#ifdef __cplusplus
}
#endif

#endif /* __UDYNLINK_SECTION_H__ */
