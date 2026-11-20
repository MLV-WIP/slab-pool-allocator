# Slab-pool-allocator terminology glossary

**Status:** names **locked** (2026-11-20). Code/API renames follow Phases 2–4; this file is the canonical vocabulary for docs.

**Scope:** this repository’s Pool / Slab / Block / Slot language.  
**mlvlib:** submodule (SpinLock, LifetimeObserver, helpers) — allocator-adjacent only; no storage vocabulary to rename.

---

## 1. Problem

Today **“slab”** names two different things:

| Sense | Meaning in this codebase | Examples |
|-------|--------------------------|----------|
| **(A) Object / engine** | The growable size-class container: `Slab<ElemSize, Alignment>` — one fixed slot size + alignment, freemaps, growth, reclaim | type `Slab`, registry `slabs16[]`, “per-slab lock” |
| **(B) Chunk of memory** | One mapped buffer holding a fixed count of those slots | `allocateNewSlab()`, `getBlockCount()`, `reclaimEmptyBlocks()`, `slab_data`, debug “New slab … allocated” |

Docs already partially disambiguate (IMPLEMENTATION.md calls the type an **engine** and the chunks **data blocks** / **buffers**), but APIs, members, and many comments still use sense (B). That is the primary clarity bug.

Secondary collisions: **“block”** also means lifetime metadata (`PoolControlBlock`, `PoolLifetimeToken`), and **“slot”** / **“item”** / **“buffer”** / **“engine”** overlap in prose.

---

## 2. Recommended vocabulary

Canonical stack (top → bottom):

```text
Pool
  └── Slab            (size-class engine for one ElemSize × Alignment)
        └── Block     (one mapped chunk: fixed count of slots)
              └── Slot  (one allocatable ElemSize cell)
```

Plus: **SlabProxy** for oversized / beyond-ladder allocations (no slot recycling).

### Glossary

| Term | Recommended definition | Why this name |
|------|------------------------|---------------|
| **Pool** | Named allocation **domain**: lifetime / ownership / isolation boundary; routes by footprint + alignment; owns the header protocol; owns Slabs (+ SlabProxy). | Matches existing public API and CONCEPTS framing. Not a layout strategy. |
| **Slab** | Size-class **engine**: growable container of fixed-size, fixed-alignment **Slots**, implemented as zero or more **Blocks**. Type: `Slab<ElemSize, Alignment>`. | Keep for the object — matches Bonwick/kernel “slab cache” sense (the cache, not one page of objects). Do **not** use “slab” for a single mapped chunk. |
| **Block** | One contiguous mapped allocation owned by a Slab, divided into a fixed number of Slots (`slab_alloc_size` / `ElemSize`). Growth adds Blocks; `trim` / reclaim drops empty Blocks. | Short, speaks well in APIs (`allocateNewBlock`, `getBlockCount`), and pairs cleanly as Pool → Slab → Block → Slot. See §3 for vs DataBlock / SlabBlock. |
| **Slot** | One allocatable cell of `ElemSize` bytes inside a Block (exact base required on free). Pool may place a hidden header at the slot base and return `base + header_size` to the user. | Already correct in freemap / CONCEPTS language; make it the only name for that unit. |
| **Item** | Backend allocation returned by `ISlab::allocateItem` / freed by `deallocateItem`. For `Slab`, the Item **is** the **slot base**; for `SlabProxy`, a standalone `new[]` base. Not the Pool **user pointer**. | Keep the Item API; use Slot/slot base in slab-layer prose; user pointer at Pool. |
| **SlabProxy** | Backend for large / overflow footprints: each allocate is a standalone system `new[]` (no Block/Slot freemap). Still implements the slab-backend interface. | Keep; optionally later rename interface to “backend” (see fuzzy terms). |
| **Footprint** | `user_size + header_size` used for class selection — not bare user size. | Keep; already the right teaching term. |
| **Alignment class / ladder / size class** | Alignment class (16/32/64) and its discrete ElemSize rungs. | Prefer “alignment class” over retired “axis”. |
| **Header** | Pool-owned metadata bytes at the slot base (magic, header width, footprint). | Keep; slab backends never interpret it. |

### Aliases to retire (once locked)

| Current informal term | Prefer |
|----------------------|--------|
| “slab” meaning a mapped chunk | **Block** |
| “buffer” / “data block” / “slab buffer” for that chunk | **Block** (prose may say “mapped block” once) |
| “engine” for `Slab` | **Slab** (or “size-class Slab”); “engine” OK only as teaching synonym in one glossary line) |
| “item” for a slot base / user pointer | **slot base** vs **user pointer** (see fuzzy terms) |

---

## 3. Block vs DataBlock vs SlabBlock

| Candidate | Pros | Cons | Verdict |
|-----------|------|------|---------|
| **Block** | Short; clear hierarchy; already used in IMPLEMENTATION (“data blocks”) and ctor flag `preallocate_first_block`; API names read well | Collides in the same repo with **control block** / `PoolLifetimeToken` | **Recommend** as the storage-chunk term |
| **DataBlock** | Disambiguates from control-block jargon; matches current doc phrase “data block” | Longer identifiers (`allocateNewDataBlock`, `getDataBlockCount`); still two words in prose | Acceptable fallback if Michael wants zero overlap with “block” in lifetime types |
| **SlabBlock** | Signals ownership by a Slab | Re-embeds “slab” into the chunk name → readers again ask “is a SlabBlock a Slab?”; awkward next to type `Slab` | **Not recommended** |

**Recommendation: use `Block`.**

Disambiguation rule when both appear in one paragraph:

- **Block** (capital B in glossary/docs when first introduced) = storage chunk inside a Slab.  
- **control block** / `PoolControlBlock` = lifetime metadata for a Pool.  
- **`PoolLifetimeToken`** = pin on that control block (`block_refs`) — *not* a storage Block.

If that dual use of “block” feels too sharp after review, prefer renaming the **lifetime** side later (`PoolLifetimeToken` / keep `PoolControlBlock` as shared_ptr-style jargon) rather than lengthening the hot-path storage term to DataBlock. That is a separate decision; not required to lock Block for chunks.

---

## 4. Other fuzzy / imprecise terms

Surveyed in slab-pool-allocator; mlvlib called out only where it touches the same words.

| Term | Where it shows up | Issue | Suggested clarification |
|------|-------------------|-------|-------------------------|
| **buffer** | IMPLEMENTATION/CONCEPTS/API; `selectBufferSize`, comments | Synonym soup with “data block” / sense-(B) “slab” | Prefer **Block** for Slab-owned chunks; reserve “buffer” for generic byte ranges or OS mappings if needed |
| **engine** | IMPLEMENTATION routing section | Informal synonym for `Slab` | Prefer **Slab**; optional one-line alias in glossary |
| **item** | `allocateItem` / `deallocateItem`, `Pool::deallocate(item)`, many vars | Ambiguous: slot base? user pointer? any pointer? | Prefer **slot** / **slot base** in slab layer; **user pointer** at Pool; eventual API rename `allocateSlot` / `deallocateSlot` (or keep Item as “opaque backend allocation”) |
| **registry slot** | Pool array index into `slabs16[i]` | Collides with allocatable **Slot** | Say **registry index** / **class index** |
| **PoolLifetimeToken** | `pool_lifetime.hpp`, factories | “Block” ≠ storage Block | Document as lifetime pin; consider later rename to `PoolLifetimeToken` / `PoolPin` |
| **PoolControlBlock** | lifetime | Same “block” root; but matches industry “control block” | Keep name; always say **control block** in prose |
| **arena** | teardown mode vs bump-arena concept | “Arena mode” = destroy Pool first / bulk reclaim; not a bump allocator | Keep **arena teardown** / **arena path**; never “arena” alone for the Pool type |
| **ISlab** | `slab.hpp` | Backend interface (`Slab` / `SlabProxy`) | Renamed from `AbstractSlab` (Phase 4). |
| **retained_mapped_bytes** | `PoolStats` | Mapped **Block** bytes across size-class Slabs (not live client use; not SlabProxy) | Renamed from `retained_slab_bytes` (Phase 4). |
| **selectClassIndex** | Free function | Returns a **class index**, not a Slab* | `selectSlab` removed (Phase 4). |
| **Finding a pointer’s slab** (CONCEPTS heading) | docs | Often means “which **Block** owns this address?” | Retitle to “Finding a pointer’s block” (or “owning block”) |
| **chunk** | CONCEPTS (Roy “chunked pool”) | External teaching term only | Keep in further-reading mapping; do not adopt as a first-class local name |
| **mlvlib “control block”** | `LifetimeObserver` | Same phrase as Pool control block; different type | Fine — different library; say `LifetimeObserver` control block when needed |

mlvlib itself has **no** Pool/Slab/Slot/Block storage vocabulary worth renaming for this project. Only shared English: control block, “block” as in “block the thread” (SpinLock).

---

## 5. Current conflation & rename hotspots

### Already partly clear (docs)

- README / CONCEPTS / API: Pool ≠ Slab (object sense) — good.  
- IMPLEMENTATION.md § Pool routing: explicit note that older APIs still say “slab” for a data block — rename backlog acknowledged.

### Hotspots where “slab” = Block (sense B)

| Kind | Location | Examples |
|------|----------|----------|
| Methods | `spallocator/slab.hpp` | `allocateNewSlab`, `destroySlabAt`, `getBlockCount`, `reclaimEmptyBlocks`, `findBlockForSlot` |
| Members | `slab.hpp` | `slab_data`, `slab_map`, `slab_available_map`, `slab_alloc_size`, `max_slabs`, `alloc_cursor_slab_` |
| Knobs | `slab.hpp` / configure | `kSlabTargetBytes`, `kSlabTargetMultiplier`, `--with-slab-target-*` (these size **Blocks**, not the Slab type) |
| Messages | `slab.hpp` | “New slab … allocated”, “remaining slabs=”, “no corresponding slab found” |
| Tests | `test/tester.cpp` | Many `getBlockCount` / `reclaimEmptyBlocks` expectations; comments mix “data block” and count-as-slab |
| Docs | API / IMPLEMENTATION / CONCEPTS | “slab buffers”, `reclaimEmptyBlocks`, “Finding a pointer’s slab”, trim “per size class” buffer language |

### Hotspots where naming is OK but should stay consistent

| Kind | Examples | Note |
|------|----------|------|
| Type / registry | `Slab`, `slabs16`/`32`/`64`, `large_slab` | Sense (A) — keep “slab” |
| Pool routing | `resolveSlab`, `makePoolSlab` | Resolve/create a **Slab** engine — OK |
| Slot layer | `SlotFreemap`, `slots_per_buffer`, `alloc_cursor_slot_` | Slot is good; `*_buffer` → `*_block` when renaming |

### Lifetime “block” (do not casually rename with storage Blocks)

- `PoolControlBlock`, `PoolLifetimeToken`, `block_refs`, `lifetime_token()`  
- Document side-by-side with storage **Block**; optional later lifetime rename

---

## 6. Rename impact sketch (proposal only)

Order of operations after Michael locks names — **not started this round**.

### Phase 0 — Docs glossary lock (this file)

- Agree: Pool / Slab / **Block** / Slot (+ SlabProxy, footprint, header).  
- Agree: whether lifetime types keep “Block” in the name.

### Phase 1 — Documentation pass — **committed** (`640ab02`)

1. Repo copy under `docs/terminology-glossary.md` + README / CONCEPTS pointers.  
2. CONCEPTS: retitled “Finding a pointer’s owning Block”; Block in growth/reclaim; Pool≠Slab kept.  
3. IMPLEMENTATION: **Block** preferred over buffer/data-block; locked Slab/Block/Slot note.  
4. API.md: Block vocabulary; ISlab slot base vs user pointer.

### Phase 2 — Internal / private code identifiers — **committed** (`cdd6cdb`)

Hard renames (no aliases): `allocateNewBlock`, `destroyBlockAt`, `block_bases`,
`block_freemaps`, `block_available_map`, `block_bytes`, `max_blocks`,
`slots_per_block`, `alloc_cursor_block_`, `selectBlockSize`, local `new_block`.

Compile knobs (`SPALLOCATOR_SLAB_TARGET_*` / `kSlabTarget*`) kept; comment notes they size Blocks.

### Phase 3 — Public / test-facing API — **done in working tree** (awaiting review; uncommitted)

Hard renames (no aliases):

| Was | Now |
|-----|-----|
| `getSlabCount()` | `getBlockCount()` |
| `reclaimEmptySlabs` | `reclaimEmptyBlocks` |
| `findSlabForItem` | `findBlockForSlot` |
| `PoolBlockToken` | `PoolLifetimeToken` |
| `block_token()` | `lifetime_token()` |

Kept: `PoolControlBlock`. Tests / ISlab / docs updated.

### Phase 4 — Clarity renames — **done in working tree** (awaiting review; uncommitted)

- Keep Item API; document Item (backend) vs Slot (internal) vs user pointer.  
- `retained_slab_bytes` → `retained_mapped_bytes`.  
- CONCEPTS/API diagrams: Pool → Slab → Block → Slot.  
- Retire `selectSlab`; `selectClassIndex` only.  
- `AbstractSlab` → `ISlab`.  
- Retire “axis” → alignment class (`nativeAlignmentClass`, etc.).


### Out of scope unless requested

- Renaming the repository or product (“slab-pool-allocator”) — still accurate (pool of slabs).  
- mlvlib renames.  
- Mass comment churn unrelated to these terms.

---

## 7. Locked decisions (Michael, 2026-11-20)

1. **Chunk term:** **Block**
2. **Keep type name `Slab`** for the size-class engine: **yes**
3. **Lifetime side:** rename `PoolBlockToken` → **`PoolLifetimeToken`** in Phase 3 (done in working tree); keep **`PoolControlBlock`** unchanged
4. **Slot** stays for freemap cells: **yes**
5. **Phase order:** as in §6 (docs → private identifiers → public API + LifetimeToken → optional clarity) — **approved**

---

## 8. This round’s deliverable status

| Item | Done? |
|------|-------|
| Survey code + docs + mlvlib for conflations | Yes |
| Glossary with recommended names | Yes (this file) |
| Block vs DataBlock vs SlabBlock recommendation | **Block** |
| Other fuzzy terms flagged | Yes (§4) |
| Rename impact sketch | Yes (§6) — proposal only |
| Code renames | **None** |
| Git commit / PR | **None** (awaiting explicit approval) |

**Repo path:** `docs/terminology-glossary.md` (synced from the Project locked glossary).
