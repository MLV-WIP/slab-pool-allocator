# Design: size-class slot-end tags + Block side table (Layout A)

**Status:** Phase 0 spec freeze (docs only). Runtime still uses today’s **front-header** protocol for all paths.  
**Version impact:** cutover is a **major** release (layout / free-protocol break).  
**Cutover style:** single hard-break branch (no long-lived dual path in production).

Related implementation narrative: [IMPLEMENTATION.md](../IMPLEMENTATION.md) (current layout today; planned locking below and in *Planned: Block map & trailer*).  
Internal planning notes (agent store) informed this freeze; **this file is the in-repo source of truth** going forward.

---

## Goals

1. Size-class **user pointer == slot base** (payload starts at the aligned Slot).  
2. Constant **8-byte trailer tax** (`N+8` footprint), independent of request alignment.  
3. Pool-level **Block ownership map** for free routing (O(log Blocks)), not per-alloc side tables.  
4. **Proxy** keeps a **front header** (protocol C), not registered in the Block map.  
5. Integrity via **generation + CRC32C** (not pointer top-byte tagging).

## Non-goals (v1 cutover)

- Pointer tagging / top-byte metadata in the user pointer (**explored and rejected** — non-portable canonical VA rules; sanitizer/hostile to raw pointer use).  
- Payload canaries / `N` in the tag; front redzones / underrun pads (deferred, off by default).  
- Merging `SlabProxy` allocations into the Block map.  
- Collapsing per-Slab `base_address_map` into the pool map (measure after map exists).  
- RCU / lock-free Block map (deferred; v1 uses `shared_mutex`).  
- Arena-as-first-class header; Pool file split beyond what Phase 1 needs.

---

## Locked decisions

| Topic | Choice |
|-------|--------|
| Size-class metadata | 8B tag at `slot_base + ElemSize - 8` (Layout **A**, validation-only) |
| User pointer (size-class) | `==` slot base |
| Footprint | `N + 8` → `resolveSlab` / ladder |
| Routing on free | `std::map<std::byte*, BlockEntry>` of **Blocks only**; `upper_bound` + predecessor + range check |
| Tag | Magic + generation + CRC32C + format nibble; **not** used to pick Slab |
| Generation | In tag **and** `BlockEntry` in v1 (VA reuse after trim) |
| Proxy | Front header (**C**); **`header_size = max(16, alignment)`** (never 8) |
| Free dispatch | Block-map hit → size-class path; else strict proxy-header probe; else fail |
| Map lock | `std::shared_mutex`: shared lookup, exclusive insert/erase |
| Publish | **Staged** grow/unpublish (see locking) |
| Observer | Pool installs registry/observer on size-class `ISlab` (API shape revisited at implement) |

### Ladder reminder

External request `N` → footprint `N+8` → smallest ladder rung with `ElemSize >= footprint`.  
Example (alignment class 16): `N=16` → 24 → `Slab<32,16>`.

---

## Current vs planned behavior

| | **Today (shipped)** | **After cutover** |
|--|---------------------|-------------------|
| Size-class user ptr | `slot_base + header_size` | `slot_base` |
| Size-class tax | `header_size = max(8, align)` | Fixed **8B trailer** |
| Size-class free | Peek front header → `resolveSlab` | Block map → tag/CRC → `deallocateItem` |
| Proxy header | `max(8, align)` (8 allowed) | `max(16, align)` only `{16,32,64}` |
| High align (32/64) | Large front pad so user ptr aligns | No front pad on size-class; slot base is aligned |

Anything that peeked **before** the size-class user pointer is broken by cutover (call out in major release notes).

---

## Data structures

```cpp
struct BlockEntry {
    ISlab*      slab;
    std::size_t elem_size;
    std::size_t block_bytes;
    uint16_t    generation;
};

// Pool:
std::map<std::byte*, BlockEntry> block_owner_;  // key = Block base only
std::shared_mutex                block_map_lock_;
```

**Lookup:** `auto it = block_owner_.upper_bound(p);` then `--it` if not `begin`; require `p ∈ [base, base+block_bytes)` and `(p - base) % elem_size == 0`.

Allocate does **not** consult this map (`resolveSlab` unchanged).

---

## Size-class tag layout (8 bytes at end of Slot)

Stored as a little-endian `uint64_t` at `slot_base + ElemSize - 8`:

| Bits | Field | Notes |
|------|-------|-------|
| 0..7 | `magic` | LIVE / FREED constants (distinct from today’s `kAllocMagic` or shared — pick at implement; document in cutover) |
| 8..23 | `generation` | Must equal `BlockEntry.generation` |
| 24..55 | `crc32c` | See coverage |
| 56..63 | `format` | `0` for v1 |

### CRC32C coverage (size-class)

CRC over this **little-endian byte concatenation** (19 bytes):

1. `uint64_t` user pointer bits (canonical address as integer)  
2. `uint64_t` `elem_size`  
3. `uint16_t` `generation`  
4. `uint8_t` `magic`  

Computed on allocate; verified on free **after** map copy supplies `elem_size` / `generation` (do not trust tag alone for those fields when checking CRC — use map entry + tag magic/gen for the pack).

Algorithm: **CRC32C** (Castagnoli); use HW intrinsics when available, software fallback otherwise. Not a cryptographic MAC.

---

## Proxy front header (protocol C)

`header_size = max(16, alignment)` ∈ `{16, 32, 64}`.

Metadata cluster remains immediately before the user pointer; CRC occupies four bytes in the mandatory 16B minimum pad:

```text
user_ptr - header_size .. user_ptr - 17 : alignment pad (when header_size > 16); zeros
user_ptr - 16 .. user_ptr - 13          : CRC32C (uint32 LE)
user_ptr - 12 .. user_ptr - 7           : reserved (zeros)
user_ptr - 6                            : magic
user_ptr - 5                            : header_size
user_ptr - 4 .. user_ptr - 1            : alloc_size (uint32 LE)
```

### CRC32C coverage (proxy)

LE concat:

1. `uint64_t` user pointer  
2. `uint32_t` `alloc_size`  
3. `uint8_t` `header_size`  
4. `uint8_t` `magic`  

---

## Free dispatch (planned)

```text
1. Block-map shared lookup → copy BlockEntry → unlock
2. If hit: validate tag (magic, gen, CRC) → slab->deallocateItem(p)
3. Else: strict proxy front-header probe (header_size ∈ {16,32,64}, magic, size, CRC)
4. Else: fail closed
```

---

## Locking & staged publish / unpublish

See also [IMPLEMENTATION — Planned Block map locking](../IMPLEMENTATION.md#planned-block-map--trailer-locking).

### Invariants

1. No live user pointer into a Block until it is **map-published** and **slab-published**.  
2. Map erase only when freemap shows **zero live slots**, and **before** `delete[]` of the VA.  
3. Free hot path: **map (shared) → unlock → tag/CRC → slab lock** — never hold map while taking slab, and never take map while holding slab on that path.  
4. Freemap occupancy is the **epoch** bridging the map→slab gap for live frees vs trim.  
5. Generation (+ CRC) covers VA reuse and wild pointers.

### Grow (staged publish)

```text
[slab lock]  new[] + append structures; mark UNPUBLISHED (not in alloc scan)
[unlock]
[map exclusive]  insert BlockEntry
[unlock]
[slab lock]  mark PUBLISHED (allocatable)
[unlock]
```

### Trim / reclaim (staged unpublish)

```text
[slab lock]  only if freemap.none() (and keep_min); mark DYING; detach from slab structures
[unlock]
[map exclusive]  erase(base)
[unlock]
delete[] VA
```

### Notification

Size-class `ISlab` notifies Pool’s Block registry at the staged points (observer / registry pointer installed at Slab construction). Exact virtual surface revisited at Phase 1 implement time.

---

## Locality A/B: chase / working-set bench (Phase 0b)

**Shipped** (bench-only; no Pool hot-path changes) as `--scenario=chase` in
`bench/bench_allocators.cpp` (not part of `--scenario=all`):

- Dependent pointer chase (`p = p->next`) over rings; sweep working-set sizes.  
- Default WS list: **sysfs cache sizes** when readable, else fixed presets; override with `--ws-bytes=`.  
- Modes: **calibrate** (one `new[]` buffer), **pool** spatial/random, **`::new`** spatial/random.  
- Flags: `--stride=64`, `--link=spatial|random|all`, `--chase-loads=N`, `--align=16|64`.  

```bash
make bench BUILD_TYPE=release \
  BENCH_ARGS='--scenario=chase --backend=new,pool --align=64'
```

Use before/after trailer cutover (especially `--align=64`, payload at offset 0).

---

## Suggested implementation order

1. Header layout PR (`spallocator/impl/`, slab split) — before Block registry code  
2. This spec + IMPLEMENTATION locking (Phase 0 — **this document**)  
3. Chase bench baseline (0b, parallel OK)  
4. Block registry + staged publish (header path still live)  
5. Hard-break cutover: trailer + proxy `header_size≥16` + CRC + dispatch  
6. Re-measure chase  

---

## Rejected alternative: pointer top-byte tagging

Packing metadata into the high bits of a 64-bit user pointer was considered and **rejected**:

- x86-64 canonical addressing faults (#GP) without LAM; ARM TBI is not universal.  
- Breaks sanitizers, debuggers, and any code that compares/hashes raw pointers.  
- Does not remove the need for Block ownership on free.

Side table + end tag (this design) is the adopted direction.
