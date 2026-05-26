# Mk RTOS Features for Porting to udynlink

## Feature 1: GNU Hash Table + Bloom Filter for Symbol Resolution

### Overview

Mk uses a **precomputed GNU hash table** (`.gnuhash`) generated at build time by a host tool called **Sym2srec**. This replaces the standard ELF `.hash` (SysV hash) with a bloom-filter-accelerated lookup for resolving symbols against the kernel's exported API.

The hash table is generated **on the host** during firmware build, linked into the firmware image at a fixed address (`0x002C0000` for the STM32F746 target), and read at runtime by the loader when resolving extern symbol references.

---

### 1.1 Host-Side Tool: Sym2srec

| Item | Detail |
|------|--------|
| Name | **Sym2srec** |
| Repository | [https://github.com/EmbSoft3/Sym2srec](https://github.com/EmbSoft3/Sym2srec) |
| Prebuilt binary (Linux) | `/tmp/Mk/Mk/Make/sym2srec` |
| Prebuilt binary (Windows) | `/tmp/Mk/Mk/Make/sym2srec.exe` |
| Source | NOT in the Mk repo — separate GitHub project |
| Build output | `.srec` (Motorola S-Record) containing firmware + symbol table |

**Invocation** (from `/tmp/Mk/Mk/Make/makefile`, line 289):
```makefile
$(SYM2SREC) $(STRIP_ARTIFACT) $(BUILD_ARTIFACT_NAME).srec $(SYMBOL_BASE_ADDR)
# Example: sym2srec Mk-Strip.elf Mk.srec 0x002C0000
```

**What Sym2srec does**:
1. Parses the input ELF file (must be 32-bit, stripped to remove local/debug symbols).
2. Extracts `.symtab` (symbol table) and `.strtab` (string table) sections.
3. Constructs a **GNU hash table** (`.gnuhash`) using the `dl_new_hash` function.
4. Builds a `SymbolsAreaHeader_t` header pointing to all three tables.
5. Copies existing loadable segments from the ELF and appends the three new sections as loadable segments at the specified `<base_address>`.
6. Writes the result as an S-Record file.

**Layout in firmware memory** (for Mk on STM32F746):

```
FLASH (0x00200000 - 0x002C0000): firmware code + data
FLASH (0x002C0000 - 0x002E0000): symbol area
  ┌─────────────────────────────────────────┐
  │ SymbolsAreaHeader_t   (at 0x002C0000)  │ ← headerSize=40 bytes
  ├─────────────────────────────────────────┤
  │ .symtab   (at symtabBaseAddr)          │ ← array of T_mkELF32SymbolTableEntry
  ├─────────────────────────────────────────┤
  │ .strtab   (at strtabBaseAddr)          │ ← null-terminated symbol name strings
  ├─────────────────────────────────────────┤
  │ .gnuhash  (at gnuhashBaseAddr)         │ ← bloom filter + buckets + chain
  └─────────────────────────────────────────┘
```

---

### 1.2 Data Structures (C)

#### SymbolsAreaHeader / T_mkInternalSymbolsAreaHeader

**File:** `/tmp/Mk/Mk/Includes/Loader/mk_loader_types.h`, lines 46-59

```c
typedef struct T_mkInternalSymbolsAreaHeader {
    uint32_t  magicNumber;      // Signature: 0x53594D42 ('SYMB')
    uint32_t  headerSize;       // Size of this header in bytes (40)
    uint32_t  reserved;         // Reserved (0xFFFFFFFF)
    uint32_t  version;          // Header version (0x00000001)
    uint32_t* symtabBaseAddr;   // Pointer to .symtab in memory
    uint32_t  symtabSize;       // Size of .symtab in bytes
    uint8_t*  strtabBaseAddr;   // Pointer to .strtab in memory
    uint32_t  strtabSize;       // Size of .strtab in bytes
    uint32_t* gnuhashBaseAddr;  // Pointer to .gnuhash in memory
    uint32_t  gnuhashSize;      // Size of .gnuhash in bytes
} T_mkInternalSymbolsAreaHeader;
```

#### GNU Hash Table / T_mkELF32GNUHashTable

**File:** `/tmp/Mk/Mk/Includes/Loader/mk_loader_elf_types.h`, lines 371-381

```c
typedef struct T_mkELF32GNUHashTable {
    uint32_t  nbuckets;      // Number of buckets in the hash table
    uint32_t  symoffset;     // Index of first non-local symbol (symoffset)
    uint32_t  bloomSize;     // Bloom filter size in 32-bit words
    uint32_t  bloomShift;    // Bloom filter shift value (typically 26 for ELF32, i.e. 5 for 32-bit arch or user-defined)
    uint32_t* bloom;         // Pointer to bloom filter array (bloomSize words)
    uint32_t* buckets;       // Pointer to bucket array (nbuckets entries)
    uint32_t* hashValues;    // Pointer to hash value chain array
} T_mkELF32GNUHashTable;
```

#### ELF32 Symbol Table Entry / T_mkELF32SymbolTableEntry

**File:** `/tmp/Mk/Mk/Includes/Loader/mk_loader_elf_types.h`, lines 162-171

```c
typedef struct T_mkELF32SymbolTableEntry {
    uint32_t  stName;    // Index into .strtab (offset of symbol name)
    uint32_t* stValue;   // Symbol address in memory (runtime address)
    uint32_t  stSize;    // Symbol size in bytes
    uint8_t   stInfo;    // Type<3:0> + Binding<7:4> (STT_* | STB_*)
    uint8_t   stOther;   // Visibility (reserved, currently 0)
    uint16_t  stShndx;   // Section header table index (SHN_UNDEF=0 for extern)
} T_mkELF32SymbolTableEntry;
```

#### Constants

**File:** `/tmp/Mk/Mk/Includes/Loader/mk_loader_elf_constants.h`, line 130

```c
#define K_MK_LOADER_ELF32_BLOOMFILTER_MASKWORDS_SIZE 32  // bits per bloom filter word
```

**File:** `/tmp/Mk/Mk/Includes/Loader/mk_loader_constants.h`

```c
#define K_MK_LOADER_MAGIC_NUMBER  0x53594D42  // 'SYMB'
#define K_MK_LOADER_MINIMAL_SIZE  40           // minimum header size
```

---

### 1.3 Hash Functions

#### GNU Hash (`dl_new_hash`)

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 81-99

```c
static uint32_t mk_loader_elf_getGnuHash ( T_str8 p_symbolName )
{
    uint32_t l_hash = 5381;
    uint8_t  l_character = ( uint8_t ) *p_symbolName;

    for ( ; l_character != '\0'; l_character = ( uint8_t ) *p_symbolName )
    {
        l_hash = ( l_hash * 33 ) + l_character;
        p_symbolName++;
    }

    return l_hash;
}
```

This is the standard `dl_new_hash` used in the ELF GNU hash specification (same as `DT_GNU_HASH`).

#### Old ELF Hash (used for external library `.hash` / DT_HASH)

**File:** Same file, lines 45-72

```c
static uint32_t mk_loader_elf_getHash ( T_str8 p_symbolName )
{
    uint32_t l_hash = 0, l_g = 0;

    while ( ( *p_symbolName ) != 0 )
    {
        l_hash = ( l_hash << 4 ) + ( uint32_t ) *p_symbolName++;
        l_g = l_hash & 0xf0000000;
        if ( l_g > 0 )
            l_hash ^= l_g >> 24;
        l_hash &= ~l_g;
    }

    return l_hash;
}
```

---

### 1.4 Initialization from Header

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 429-448

```c
// Read the 4 header fields from gnuhashBaseAddr
l_hashTable.nbuckets   = l_header->gnuhashBaseAddr[0];
l_hashTable.symoffset  = l_header->gnuhashBaseAddr[1];
l_hashTable.bloomSize  = l_header->gnuhashBaseAddr[2];
l_hashTable.bloomShift = l_header->gnuhashBaseAddr[3];

// Pointers immediately follow the 4-word header
l_hashTable.bloom      = &l_header->gnuhashBaseAddr[4];
l_hashTable.buckets    = &l_header->gnuhashBaseAddr[4 + l_hashTable.bloomSize];
l_hashTable.hashValues = &l_header->gnuhashBaseAddr[4 + l_hashTable.bloomSize + l_hashTable.nbuckets];
```

**Memory layout of `.gnuhash`:**

```
Offset  Contents
──────────────────────────────────────────
[0]     nbuckets          (uint32_t)
[1]     symoffset         (uint32_t)
[2]     bloomSize         (uint32_t) — number of 32-bit words
[3]     bloomShift        (uint32_t)
[4..4+bloomSize-1]        bloom filter word array
[4+bloomSize .. 4+bloomSize+nbuckets-1]       bucket array
[4+bloomSize+nbuckets ..]                      hash value chain array
```

---

### 1.5 Lookup Algorithm (Bloom Filter + Bucket Scan)

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 420-530

**Pseudocode:**

```c
// ─── mk_loader_elf_resolveSymbolInternal ───
// Input: symbol name string
// Output: symbol address (32-bit)

uint32_t mk_loader_elf_resolveSymbolInternal(str, *out_addr)
{
    // Step 1: Validate internal symbols area header
    header = (T_mkInternalSymbolsAreaHeader*)K_MK_INTERNAL_SYMBOLS_BASE_ADDR;
    if (header->magicNumber != 0x53594D42 || header->version == 0 || header->headerSize < 40)
        return K_MK_ERROR_UNEXPECTED;

    // Step 2: Parse GNU hash table header (4 words at gnuhashBaseAddr)
    hashTable.nbuckets   = header->gnuhashBaseAddr[0];
    hashTable.symoffset  = header->gnuhashBaseAddr[1];
    hashTable.bloomSize  = header->gnuhashBaseAddr[2];
    hashTable.bloomShift = header->gnuhashBaseAddr[3];
    hashTable.bloom      = &header->gnuhashBaseAddr[4];
    hashTable.buckets    = &header->gnuhashBaseAddr[4 + bloomSize];
    hashTable.hashValues = &header->gnuhashBaseAddr[4 + bloomSize + nbuckets];

    // Step 3: Compute GNU hash of symbol name
    hash = dl_new_hash(symbolName);  // start=5381, hash = hash*33 + c

    // Step 4: Bloom filter probe
    // Compute 2-bit mask within a 32-bit word
    mask = (1 << (hash % 32)) | (1 << ((hash >> bloomShift) % 32));
    // Select which bloom word to test
    bloomIndex = (hash / 32) & (bloomSize - 1);

    if ((bloom[bloomIndex] & mask) != mask)
        return K_MK_ERROR_UNRESOLVED;  // definitely not present (no false negatives)

    // Step 5: Bucket lookup
    symbolIndex = buckets[hash % nbuckets];

    if (symbolIndex < symoffset)
        return K_MK_ERROR_UNRESOLVED;  // only non-local symbols are indexed

    // Step 6: Walk the chain
    symbolHash = 0;
    strResult = 0;

    while ((symbolHash & 0x1) == 0 && strResult == 0)
    {
        symbolEntry = (T_mkELF32SymbolTableEntry*)
            (symtabBaseAddr + symbolIndex * sizeof(T_mkELF32SymbolTableEntry));

        symbolHash = hashValues[symbolIndex - symoffset];

        // Compare hashes (mask off chain-end bit)
        if ((symbolHash | 0x1) == (hash | 0x1))
        {
            // Full string comparison
            name = strtabBaseAddr + symbolEntry->stName;
            if (strcmp(name, symbolName) == 0)
            {
                *out_addr = symbolEntry->stValue;  // ← resolved address!
                return K_MK_OK;
            }
        }

        symbolIndex++;  // next in chain (GNU hash uses contiguous symbols)
    }

    return K_MK_ERROR_UNRESOLVED;
}
```

**Key algorithmic points:**

1. **Bloom filter**: Uses 2 hash probes per symbol — the GNU hash value `h`, and `h >> bloomShift` (typically 26 for 32-bit ELF, giving 6-bit shift). Both are modulo 32 to select a bit position within a 32-bit word. The bloom filter **has no false negatives** — if the probe fails, the symbol is guaranteed absent. False positives are resolved by the full bucket scan.

2. **Bucket structure (GNU hash)**: Unlike SysV hash which uses a linked-list chain, GNU hash stores symbols **contiguously in the symbol table**. Buckets store the starting index of the chain. The chain end is detected by checking bit 0 of the hash value — when bit 0 is set, it's the last entry in the chain.

3. **Chain walking**: Symbols in a chain are contiguous (`symbolIndex++`). No pointer chasing — just linear scan until the hash value has bit 0 set.

4. **symoffset**: Symbols with index less than `symoffset` are local and not in the hash table. The bucket chain only indexes symbols at or after this offset.

---

### 1.6 Symbol Address Resolution

Symbol addresses are stored directly in `stValue` of the `T_mkELF32SymbolTableEntry`. When a symbol is found:

```c
*p_symbolAddr = (uint32_t)l_symbolEntry->stValue;
```

`stValue` already contains the **absolute runtime address** (since Sym2srec runs on the stripped ELF whose symbols already have the final load addresses applied by the linker). The loader uses this address directly to patch GOT entries or relocation targets.

---

### 1.7 Two-Phase Resolution: External Libraries Then Internal Symbols

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 538-575

```c
T_mkCode mk_loader_elf_resolveExternalSymbols (parser, lib_list, symbolName, *symbolAddr)
{
    // Phase 1: Search external libraries (DT_NEEDED chain)
    // Uses old ELF hash (DT_HASH), not GNU hash
    result = resolveSymbolExternal(parser, lib_list, symbolName, symbolAddr);

    // Phase 2: If not found, search internal symbols (kernel API table)
    // Uses GNU hash + bloom filter
    if (result == K_MK_ERROR_UNRESOLVED)
        result = resolveSymbolInternal(symbolName, symbolAddr);

    return result;
}
```

---

## Feature 2: DT_NEEDED Dependency Tracking

### Overview

Mk's ELF loader supports `DT_NEEDED` dependencies. When loading a dynamic ELF binary (type `ET_DYN`), it:

1. Parses the `PT_DYNAMIC` segment to extract the dynamic table.
2. During relocation, for each unresolved GLOBAL/WEAK symbol, iterates the dynamic table looking for `DT_NEEDED` entries.
3. For each `DT_NEEDED` library, checks if it's already loaded (deduplication).
4. If not loaded, **recursively loads** the library from the filesystem (first in the app directory, then in `mk/libs/`).
5. Resolves the symbol within that library.
6. External library symbols use the old ELF `.hash` (DT_HASH), not the GNU bloom filter.

---

### 2.1 Dynamic Table Parsing at Load Time

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_loadRAM.c`, lines 263-313

```c
static void mk_loader_elf_parseDynamicTable (parser, lib_list)
{
    // Number of entries = PT_DYNAMIC segment size / 8
    entryCount = parser->dynEntry.pMemSize / K_MK_LOADER_ELF32_DYNAMICTABLE_ENTRY_SIZE;

    for (counter = 0; counter < entryCount; counter++)
    {
        dynEntry = parser->dynEntry.pVirtualAddr + baseAddr + counter * 8;

        if (dynEntry->dTag <= K_MK_LOADER_ELF32_DT_BIND_NOW)  // tag is supported
        {
            // Copy the entry into the parser's pre-allocated array
            copy(&parser->dynTable.entry[dynEntry->dTag], dynEntry, 8);
        }
    }

    // If loading an external library (not the main app), also copy the
    // entire dynamic table into the library's metadata
    if (lib_list->first != NULL)
        copy(&lib_list->first->dynTable, &parser->dynTable, sizeof(T_mkELF32DynamicTable));
}
```

The `T_mkELF32DynamicTable` is a fixed-size array indexed by tag value (0..K_MK_LOADER_ELF32_DT_BIND_NOW = 24):

```c
// File: /tmp/Mk/Mk/Includes/Loader/mk_loader_elf_types.h, lines 312-316
typedef struct T_mkELF32DynamicTable {
    T_mkELF32DynamicTableEntry entry[K_MK_LOADER_ELF32_DT_BIND_NOW + 1];
} T_mkELF32DynamicTable;
```

Each entry (lines 299-304):
```c
typedef struct T_mkELF32DynamicTableEntry {
    uint32_t dTag;                              // Tag: DT_NEEDED=1, DT_HASH=4, etc.
    T_mkELF32DynamicTableEntryField dField;     // dVal (int) or dAddr (pointer)
} T_mkELF32DynamicTableEntry;
```

---

### 2.2 DT_NEEDED Discovery During Symbol Resolution

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 327-412

When a symbol needs resolution, the loader iterates the **main module's** dynamic table looking for `DT_NEEDED` entries:

```c
static T_mkCode mk_loader_elf_resolveSymbolExternal(parser, lib_list, symbolName, *symbolAddr)
{
    // Number of entries in the dynamic table
    entryCount = parser->dynEntry.pMemSize / 8;

    // Address of .dynstr (string table for dynamic entries)
    strtab = parser->dynTable.entry[DT_STRTAB].dAddr + baseAddr;

    for (counter = 0; counter < entryCount &&
         result == K_MK_ERROR_UNRESOLVED; counter++)
    {
        dynEntry = parser->dynEntry.pVirtualAddr + baseAddr + counter * 8;

        if (dynEntry->dTag == K_MK_LOADER_ELF32_DT_NEEDED)
        {
            // Get library name from the dynamic string table
            libraryName = &strtab[dynEntry->dField.dVal];

            // Check if already loaded (deduplication)
            result = mk_loader_searchLibrary(lib_list, &library, libraryName);

            if (result == K_MK_ERROR_NOT_AVAILABLE)
            {
                // Recursively load the library from filesystem
                result = mk_loader_elf_loadLibrary(parser, lib_list, libraryName);

                if (result == K_MK_OK)
                {
                    // Resolve symbol in the newly loaded library
                    result = resolveSymbolExternalLibrary(
                        lib_list->first, symbolName, symbolAddr);
                }
            }
            else if (result == K_MK_OK)
            {
                // Already loaded — resolve from existing library
                result = resolveSymbolExternalLibrary(
                    library, symbolName, symbolAddr);
            }
        }
    }

    return result;
}
```

**Note:** There is a subtle bug on line 363:
```c
l_libraryName = ( T_str8 ) &l_strtab [ p_elf32Parser->dynTable.entry [ l_dynEntry->dTag ].dField.dVal ];
```
This indexes the dynTable array by `dynEntry->dTag` (which is `DT_NEEDED = 1`), not by `dynEntry->dField.dVal`. However, since the parser's dynTable stores the DT_STRTAB at index 5 and DT_NEEDED at index 1, this reads entry[1].dField.dVal, which would be the name offset stored in the original DT_NEEDED entry. The code appears to work because `dynTable.entry[DT_NEEDED]` and `dynTable.entry[DT_STRTAB]` are both set during parsing (the DT_NEEDED entry gets its dVal set to the name offset from the parsed dynamic section). Actually wait — looking at line 363 more carefully:

```c
l_libraryName = ( T_str8 ) &l_strtab [ p_elf32Parser->dynTable.entry [ l_dynEntry->dTag ].dField.dVal ];
```

`l_dynEntry` is the raw entry from the in-memory dynamic table. Its `dTag` is `DT_NEEDED = 1`. `p_elf32Parser->dynTable.entry[1]` was set during `parseDynamicTable` to the value of that DT_NEEDED entry. So `dynTable.entry[1].dField.dVal` is indeed the string table offset.

This is correct, if a bit roundabout.

---

### 2.3 Library Loading (Recursive)

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 198-252

```c
static T_mkCode mk_loader_elf_loadLibrary(parser, lib_list, libraryName)
{
    // Allocate a new T_mkExternalLibraryItem from pool
    library = pool_alloc(g_mkExternalLibraryItemPool.pool);

    // Add to head of library list
    mk_loader_addLibrary(lib_list, library);

    // First search: application directory
    result = mk_loader_elf_searchLibrary(parser, lib_list, library,
                                          parser->path,          // app path
                                          libraryName);

    // Second search: system library directory (if not found)
    if (result == K_MK_ERROR_NOT_FOUND)
        result = mk_loader_elf_searchLibrary(parser, lib_list, library,
                                              "mk/libs",           // system path
                                              libraryName);

    return result;
}
```

**Search function** (`mk_loader_elf_searchLibrary`, lines 107-190):

```c
static T_mkCode mk_loader_elf_searchLibrary(parser, lib_list, library, libraryPath, libraryName)
{
    // Build full path: libraryPath + "/" + libraryName
    strcat(library->fileBuf, libraryPath);
    strcat(library->fileBuf, "/");
    strcat(library->fileBuf, libraryName);

    // Try to open the file
    result = mk_file_open(parser->volume, &file, library->fileBuf, READ_ONLY);

    if (result == K_MK_OK)
    {
        // Record the path and name pointers
        library->filePath = &library->fileBuf[0];
        library->fileName = &library->fileBuf[pathLength + 1];

        // Allocate a memory page for this library
        mk_application_alloc(&library->baseAddr, 0);

        // Recursively load the library ELF!
        // This calls mk_loader_elf_loadRAM which will:
        //   1. Parse the ELF headers
        //   2. Load segments into the page
        //   3. Parse the PT_DYNAMIC (if ET_DYN)
        //   4. Parse the dynamic table
        //   5. Relocate (which may trigger more DT_NEEDED resolution)
        result = mk_loader_elf_loadRAM(file, library->fileName,
                                       library->baseAddr, lib_list, NULL);

        if (result != K_MK_OK)
            mk_application_free(library->baseAddr);

        mk_file_close(file);
    }

    return result;
}
```

---

### 2.4 Deduplication: Library Search

**File:** `/tmp/Mk/Mk/Sources/Loader/mk_loader_searchLibrary.c`, lines 45-96

```c
T_mkCode mk_loader_searchLibrary(lib_list, *out_item, libraryName)
{
    item = lib_list->first;

    while (item != NULL && not_found)
    {
        if (strcmp(item->fileName, libraryName) == 0)
        {
            *out_item = item;
            return K_MK_OK;         // Found!
        }
        item = item->next;
    }

    return K_MK_ERROR_NOT_AVAILABLE;  // Not loaded yet
}
```

The library list is a **singly-linked list** using `T_mkExternalLibraryItem.next`:

```c
// File: /tmp/Mk/Mk/Includes/Loader/mk_loader_types.h, lines 67-76
typedef struct T_mkExternalLibraryItem {
    int8_t   fileBuf[K_MK_FILE_MAX_NAME_LENGTH + 1];  // Full path buffer
    T_str8   fileName;                                  // Pointer to name part
    T_str8   filePath;                                  // Pointer to path part
    T_mkAddr baseAddr;                                  // Load address in RAM
    T_mkELF32DynamicTable dynTable;                     // Copied dynamic table
    T_mkExternalLibraryItem* next;                      // Linked list pointer
} T_mkExternalLibraryItem;
```

```c
// File: /tmp/Mk/Mk/Includes/Loader/mk_loader_types.h, lines 84-88
typedef struct T_mkExternalLibrariesList {
    T_mkExternalLibraryItem* first;  // Head of singly-linked list
} T_mkExternalLibrariesList;
```

New libraries are prepended to the head of the list (`mk_loader_addLibrary`, line 69: `p_list->first = p_library`).

---

### 2.5 Symbol Resolution in External Libraries

**File:** `/tmp/Mk/Mk/Sources/Loader/Elf/mk_loader_elf_resolveExternalSymbols.c`, lines 260-319

External libraries use the **old ELF SysV hash** (`DT_HASH`), not the GNU bloom filter:

```c
static T_mkCode mk_loader_elf_resolveSymbolExternalLibrary(library, symbolName, *symbolAddr)
{
    // DT_HASH table layout:
    //   [0] = nbucket (number of buckets)
    //   [1] = nchain (number of chains = number of symbols)
    //   [2 .. 2+nbucket-1] = bucket array
    //   [2+nbucket ..] = chain array

    hashTableAddr = library->dynTable.entry[DT_HASH].dAddr + library->baseAddr;
    symTableAddr  = library->dynTable.entry[DT_SYMTAB].dAddr + library->baseAddr;
    strTableAddr  = library->dynTable.entry[DT_STRTAB].dAddr + library->baseAddr;

    nbucket = hashTableAddr[0];
    bucketAddr = &hashTableAddr[2];
    chainAddr  = &hashTableAddr[2 + nbucket];

    hash = mk_loader_elf_getHash(symbolName);        // Old ELF hash
    hash = hash % nbucket;

    // Walk the chain (linked list via chain indices)
    for (index = bucketAddr[hash];
         index != 0 && !found;
         index = chainAddr[index])
    {
        entry = symTableAddr + index * sizeof(T_mkELF32SymbolTableEntry);
        name  = strTableAddr + entry->stName;

        if (strcmp(name, symbolName) == 0)
        {
            *symbolAddr = entry->stValue;   // Absolute address
            return K_MK_OK;
        }
    }

    return K_MK_ERROR_UNRESOLVED;
}
```

**Key difference from GNU hash:** The SysV hash uses a chain array where each entry's value is the **index** of the next symbol in the chain, terminating at 0 (SHN_UNDEF). This requires pointer chasing through the chain array. GNU hash uses contiguous symbols with a bit-0 chain-end marker, which is more cache-friendly.

---

### 2.6 Full Resolution Flow

```
mk_loader_elf_relocate (called from loadRAM.c:488)
  │
  └─ For each RELA/REL entry:
       │
       └─ mk_loader_elf_relocateByType
            │
            ├─ R_ARM_RELATIVE  → baseAddr + addend (local)
            ├─ R_ARM_ABS32     → stValue + baseAddr + addend (local)
            │
            └─ R_ARM_GLOB_DAT → mk_loader_elf_resolveExternalSymbols
                 │
                 ├─ mk_loader_elf_resolveSymbolExternal
                 │    │
                 │    ├─ Iterate dynamic table of MAIN module
                 │    ├─ For each DT_NEEDED:
                 │    │    ├─ mk_loader_searchLibrary → already loaded?
                 │    │    │    ├─ YES → resolve there
                 │    │    │    └─ NO  → mk_loader_elf_loadLibrary
                 │    │    │                ├─ mk_loader_elf_searchLibrary (app dir)
                 │    │    │                ├─ mk_loader_elf_searchLibrary (mk/libs/)
                 │    │    │                └─ mk_loader_elf_loadRAM (→ recursive!)
                 │    │    │                       ├─ parse dynamic table
                 │    │    │                       └─ relocate (→ may recurse further)
                 │    │    └─ mk_loader_elf_resolveSymbolExternalLibrary
                 │    │         └─ (DT_HASH / SysV hash lookup)
                 │    │
                 │    └─ Not found → K_MK_ERROR_UNRESOLVED
                 │
                 └─ mk_loader_elf_resolveSymbolInternal
                      └─ (GNU hash / bloom filter lookup)
                           └─ Found → *symbolAddr = entry->stValue
```

---

### 2.7 Error Handling for Missing Dependencies

| Condition | Error Code | Source Location |
|-----------|-----------|-----------------|
| Library file not found in app dir or mk/libs | `K_MK_ERROR_NOT_FOUND` | `searchLibrary` line 131 (if mk_file_open fails) |
| Library path too long | `K_MK_ERROR_PARAM` | `searchLibrary` line 185 |
| Symbol not found in any loaded library | `K_MK_ERROR_UNRESOLVED` | `resolveSymbolExternalLibrary` line 263 (initialized) or line 318 |
| Internal symbols header corrupted | `K_MK_ERROR_UNEXPECTED` | `resolveSymbolInternal` line 525 |
| Dynamic table missing SYMTAB or STRTAB | `K_MK_ERROR_CORRUPTED` | `relocateSymbols` line 356 (in loadRAM.c) |
| Recursive library load fails | Propagated from `mk_loader_elf_loadRAM` chain | `searchLibrary` propagates to `loadLibrary` |
| Memory allocation fails for library page | `K_MK_ERROR_MALLOC` | Propagated from `mk_application_alloc` |

**Error recovery:** When a recursive library load fails (line 151-155 in `searchLibrary`), the allocated memory page is freed. The error propagates upward. In `mk_application_loadDynamic.c` (line 180), if loading fails, all loaded libraries are flushed via `mk_loader_flushLibraries` and the SDRAM page is freed.

**There is no circular dependency detection** — if library A DT_NEEDEs library B which DT_NEEDEs library A, the deduplication check (`mk_loader_searchLibrary`) prevents infinite recursion because B will find A already in the list.

---

### 2.8 Library Lifecycle

```
Application install flow (mk_application_loadDynamic.c:90):
  1. mk_loader_elf_open → open the .elf file
  2. mk_application_alloc → allocate SDRAM page
  3. mk_loader_elf_loadRAM → load ELF segments, parse dynamic table, relocate
       ├── During relocate: DT_NEEDED resolution triggers recursive loads
       └── Each external library gets its own SDRAM page
  4. Validate T_mkApplicationDynamicHeader
  5. On success: libraries list saved in application->dynamic.libraries
  6. On failure: mk_loader_flushLibraries frees all loaded library pages
```

**Cleanup** on application uninstall: All library pages allocated during load are freed via `mk_loader_flushLibraries`, which iterates the linked list and calls `mk_application_free` on each `baseAddr`, then returns the `T_mkExternalLibraryItem` nodes to the pool.
