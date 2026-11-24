# Major release notes: size-class end tags

**Status:** draft for the eventual PR that lands the end-tag work on `main`.  
**Impact:** **major** version (layout / free-protocol break).  
**Spec:** [slot-end-tag.md](slot-end-tag.md).

## Summary

Size-class allocations no longer place a front header before the user pointer.
The user pointer **is** the Slot base; an 8-byte end tag (magic + generation +
CRC32C + format) sits at `slot_base + ElemSize - 8`. Free routes through a Pool
**Block ownership map**, then validates the tag. `SlabProxy` (oversize) keeps a
front header with `header_size = max(16, alignment)` and is **not** in the map.

Byte-wise layouts and design rationale:
[IMPLEMENTATION.md — Memory layout](../IMPLEMENTATION.md#memory-layout).

## Breaking changes

| Before | After |
|--------|--------|
| Size-class user ptr = `slot_base + header_size` | Size-class user ptr = `slot_base` |
| Footprint `N + max(8, align)` | Footprint `N + 8` |
| Free peeked bytes **before** the user pointer | Free uses Block map + end tag (do not peek before size-class ptrs) |
| Proxy `header_size` could be 8 | Proxy `header_size ∈ {16,32,64}` only |
| No CRC on metadata | CRC32C on size-class tags and proxy headers |

Anything that assumed metadata live immediately before a size-class user
pointer, or that `allocate(N, 8)` used an 8-byte front header, is broken.

## Compatibility notes

- Request alignment contract unchanged: powers of two in `[4, 64]`.  
- `kDefaultAlignment` still defaults `allocate(N)`.  
- Smart-pointer / `PoolAllocator` / `PoolMemoryResource` APIs unchanged; only
  the bytes behind the pointer protocol changed.  
- Interior / garbage / double-free still fail closed (`deallocate` throws;
  `try_deallocate` returns false; noexcept deleters abort).

## Migration

1. Stop reading or writing bytes before size-class user pointers.  
2. Expect some sizes to land in a **lower** ladder rung (smaller tax).  
3. Re-run apps under AddressSanitizer / ThreadSanitizer.  
4. Optional locality check:
   `make bench BUILD_TYPE=release BENCH_ARGS='--scenario=chase --backend=new,pool --align=64'`.

## Deferred (not in this major)

- Payload canaries / `N` in the tag; front redzones.  
- Collapsing per-Slab `base_address_map` into the Pool Block map.  
- RCU / lock-free Block map.

## Verification

- `make test` green (size-class tag + proxy dispatch tests included).  
- Chase sample: [IMPLEMENTATION — Chase](../IMPLEMENTATION.md#chase-ws-pointer-chase-post-cutover).
