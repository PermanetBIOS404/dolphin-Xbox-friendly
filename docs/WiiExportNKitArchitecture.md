# Wii Export NKit Reconstruction Architecture

Status: N1 research/design only. NKit export remains blocked. This document is based on
`feature/wii-export-assistant` at
`2fcc8ed5527dad77326a42af763f25eb76b23979` and was prepared without opening a real NKit image.

## Decision summary

The first supported source should be a retail **Wii NKit v1 logical stream**, identified by the
exact eight bytes `NKIT v01` at disc offset `0x200`. The canonical outer files are `.nkit.iso`
(`BlobType::PLAIN`) and `.nkit.gcz` (`BlobType::GCZ`), but the NKit representation is inside the
outer container and is not itself a `BlobType`.

The reconstruction layer must produce the byte-addressed view of a conventional raw Wii disc,
not merely a decrypted file-system view. `DiscIO::WriteWbfs` copies selected raw 2 MiB ranges from
a `BlobReader`. `DiscScrubber` uses a `VolumeWii` to decide which ranges are needed, but the writer
still copies the original reader's raw bytes. The reconstructed view therefore needs conventional
disc/partition headers, original logical offsets, `0x8000` partition clusters with their `0x400`
hash areas, valid H0/H1/H2/H3 relationships, and Wii partition encryption.

The recommended sequence is:

1. **N2:** implement and test the Wii NKit v1 metadata, validation, recovery assessment,
   deterministic-junk, and gap-decoding foundation. It must remain disconnected from export/UI.
2. **N3 proof:** use that same reconstruction core to stream a temporary conventional ISO, reopen
   and validate it with `DiscIO`, then prove that `AnalyzeWbfs` accepts it. This is the safest first
   end-to-end proof, not the desired shipping design.
3. **Final architecture:** index the NKit stream once and expose an on-demand
   `NKitV1ReconstructedBlobReader`. Feed a copy of that reader to `VolumeWii`/`AnalyzeWbfs` and the
   same immutable plan to the existing native WBFS backend. Keep `WriteWbfs` unchanged.

A direct NKit-to-WBFS implementation is rejected because it would couple reconstruction to WBFS,
duplicate conventional-disc analysis, and weaken the existing validation boundary.

## 1. Current Dolphin NKit handling

### Detection and opening

The current path is:

```text
path
  -> DiscIO::CreateBlobReader (DiscIO/Blob.cpp)
  -> outer container BlobReader
  -> DiscIO::CreateDisc / TryCreateDisc (DiscIO/Volume.cpp)
  -> Wii magic at 0x18
  -> DiscIO::VolumeWii
  -> VolumeDisc::IsNKit (DiscIO/VolumeDisc.cpp)
  -> big-endian "NKIT" at logical disc offset 0x200
```

`BlobType` describes the outer storage container (`PLAIN`, `GCZ`, `WIA`, `RVZ`, and so on). There
is no `BlobType::NKIT`. `VolumeDisc::IsNKit()` reads four bytes from the already-decoded outer
container. This means the same inner NKit stream can be wrapped by different container formats.

`UICommon::GameFile` records `volume->IsNKit()` and can display the game as NKit. The Game List can
identify it as a Wii game because `TryCreateDisc` sees the normal Wii magic before NKit detection.

### What Dolphin can read today

Wii NKit v1 changes bytes `0x60` and `0x61` in the disc header to indicate that partition data has
neither Wii hash areas nor encryption. `VolumeWii` reads those flags in its constructor. With both
features absent, `VolumeWii::Read` reads the partition's compacted decrypted byte stream directly.
Consequently current Dolphin can usually:

* identify the Wii title and ID;
* display metadata and the NKit label;
* enumerate the partitions still represented in the NKit partition table;
* read the NKit-adjusted FST and files;
* boot many NKit images through the ordinary Wii volume implementation.

It cannot read the conventional raw sectors that were replaced by the NKit encoding. Raw
`BlobReader::Read` returns the compacted NKit stream, not a restored Wii ISO. Partition offsets and
file offsets observed through `VolumeWii` are the NKit-adjusted values.

The launch path intentionally displays `DolphinQt/NKitWarningDialog.cpp`. The warning notes slower
loading, deterministic-state incompatibilities, and known game failures. `DolphinTool` and the Qt
conversion dialog also state that converting an NKit input with Dolphin leaves the result as NKit.
Those conversion paths only change the outer container.

### NKit-specific behavior present in Dolphin

Current source contains only these NKit-aware behaviors:

* four-byte detection in `VolumeDisc::IsNKit()`;
* UI/Game List labeling and launch/conversion warnings;
* verifier diagnostics in `VolumeVerifier::CheckMisc()`;
* extraction cleanup in `DiscExtractor.cpp`, which clears the NKit header area from extracted
  `header.bin` and restores the Wii hash/encryption flag bytes in an extracted disc header;
* explicit Wii Export rejection.

There is no NKit metadata parser, gap decoder, junk regenerator, partition reconstructor, or
reconstructed random-access reader in current or historical Dolphin source. Repository history
locates NKit detection at commit `2e8c5b4521d7a483d9895cd4294acd51636046d4` and extraction cleanup
at `ee19ff66b4284c4849a4bec446fb5f9077f6c7d6`; `git log -S/-G` found no former reconstruction
engine.

## 2. Current Wii Export rejection path

### Exact flow

`DolphinQt::MakeWiiExportGameListEntry` accepts a valid Wii Game List item independently of NKit.
`PrepareWiiExportGameListSource` then:

1. reopens the exact `entry.source_path` with `DiscIO::CreateDisc`;
2. checks that it is still a Wii disc and that its ID matches the Game List entry;
3. calls `DiscIO::AnalyzeWbfs(*volume)`;
4. constructs `WiiExportPreparedSource` only after analysis succeeds.

`AnalyzeWbfs` checks, in order:

1. Wii platform;
2. allowed outer `BlobType` (`PLAIN` or `RVZ`);
3. `DataSizeType::Accurate`;
4. `volume.IsNKit()`;
5. conventional source size/header/geometry.

The practical results are:

| Source | First analysis result |
| --- | --- |
| `.nkit.iso` (`PLAIN`, accurate) | `WbfsAnalysisError::NKitSource` |
| NKit stream inside RVZ | `WbfsAnalysisError::NKitSource` |
| canonical `.nkit.gcz` | `WbfsAnalysisError::UnsupportedSourceFormat` before the NKit check |
| inaccurate outer reader | `WbfsAnalysisError::InaccurateSourceSize` before the NKit check |

If a synthetic prepared source bypasses this first boundary, `CreateWiiExportPlan` sets
`requires_nkit_input` and requires `WiiExportBackendCapability::NKitInput`. The native descriptor
does not advertise that capability, preflight blocks it, and
`WiiExportNativeBackend::Execute()` contains an additional explicit NKit failure. N1 must not
remove any of these defenses.

### Why the rejection is technically necessary

For a plain NKit file, `BlobReader::GetDataSize()` is accurately the length of the compacted NKit
stream; it is not the conventional Wii disc length recorded by NKit metadata. Removing only the
`IsNKit` check would make `AnalyzeWbfs` analyze NKit offsets and make `WriteWbfs` copy NKit bytes
into WBFS. The result would remain NKit internally and would not be a conventional USB-loader
disc.

`AnalyzeWbfs` does not require anything fundamentally unavailable after reconstruction. It needs:

* a Wii `VolumeDisc` over a conventional raw view;
* an accurate conventional data size;
* a stable, random-readable `BlobReader`;
* a conventional disc header and partition/file-system structure.

The existing writer can operate unchanged when those conditions are supplied. The outer-container
restriction is broader than necessary for a future reconstruction front end: a GCZ NKit input can
be decoded first and exposed as a conventional prepared reader. That is not a reason to widen the
current direct-write policy.

### Integration requirement exposed by current contracts

Preview analysis currently dies with the temporary `VolumeDisc`; execution later reopens the
original path with `CreateBlobReader`. A reconstructed source therefore needs an immutable,
repeatable preparation recipe. Preview and execution must independently rebuild the same
reconstruction plan and compare an original-source identity before the backend starts.

The original outer `BlobType` and the effective reconstructed representation also need separate
fields. The effective reader may behave as `PLAIN`, while the UI and revalidation still need to
remember that the source was, for example, GCZ plus Wii NKit v1.

## 3. NKit variants and forms

### NKit v1 disc representation

The NKit v1 source accepts exactly `NKIT v01`, not merely the four-byte marker Dolphin currently
tests. For Wii, the top-level header fields used by the reference reader/writer are:

| Offset | Meaning in Wii NKit v1 |
| --- | --- |
| `0x200..0x207` | ASCII signature/version `NKIT v01` |
| `0x208` | big-endian CRC32 of the pre-NKit source image |
| `0x20c` | CRC-forcing/patch value used by NKit v1 |
| `0x210` | original Wii image length divided by four |
| `0x214` | junk-ID/format field used by v1 readers; do not assume semantics without validation |
| `0x218` | CRC32 of an externally stored removed update partition, or zero if not removed |

Partition data has another `NKIT v01` inner header and an original partition-size field. The NKit
stream carries encoded gap descriptions, compacted file data, an adjusted FST, hash-preservation
flags, and exceptional preserved per-cluster hash material.

### Forms relevant to RWiN

| Form | Wii/GC | Preserved/changed data | Reconstructability | N1 decision |
| --- | --- | --- | --- | --- |
| Wii NKit v1, update retained | Wii | Data partitions compacted/decrypted; deterministic gaps encoded; update remains | Valid v1 should be reconstructable to its pre-NKit source from internal data | Initial target |
| Wii NKit v1, update removed (`0x218 != 0`) | Wii | Same, but update partition is externalized and represented by recovery metadata | Playable data partition can be reconstructed internally; archival-original update requires matching recovery data | Initial target with explicit fidelity state |
| `.nkit.iso` | Wii or GC | Plain outer container around v1 stream | Outer reader is straightforward | Initial Wii container |
| `.nkit.gcz` | Wii or GC | GCZ outer container around the same v1 stream | Outer GCZ reader supplies the NKit stream | Initial Wii container after reconstruction front end; not direct WBFS input |
| NKit v1 stream rewrapped as RVZ/WIA | Wii or GC | Same inner representation; Dolphin conversion preserves NKit | Algorithm is the same, but each outer format needs tests and stable random reads | Recognize; defer support claim until tested |
| GameCube NKit v1 | GameCube | Similar FST compaction/gap encoding, but no Wii partition crypto/hash layer and size metadata differs | Deterministic for valid images, with different recovery rules | Explicitly out of this Wii milestone |
| NKit v2 lossless WBFS/CISO/WIA metadata | Wii or GC | Normal container plus a separate NKit 2 header, checksums, original size, and junk-block map | Container reader can restore omitted blocks | Different format family; not detected by `VolumeDisc::IsNKit`, out of scope |
| NKit 2 scan plus deduplicated file store | Multiple | XML map plus external content store | External files are intrinsic | Not a standalone NKit disc input; out of scope |

The current NKit project documents NKit v1 output as `nkit.iso`/`nkit.gcz`, states that v1 compacted
file systems and predictable junk gaps, and states that NKit 2 reads but no longer writes that
format. NKit 2 must not be treated as a newer version of the `NKIT v01` byte stream without a
separate specification and parser.

Initial production support should be narrower than the reference program: retail Wii discs with a
valid data partition, exact v1 signature, sane single- or dual-layer size, and supported outer
reader. RVT-R/RVT-H, development/non-retail layouts, custom malformed partitions, unknown versions,
and already-damaged sources should return typed unsupported/incomplete results.

## 4. What reconstruction requires

### Transformations performed by Wii NKit v1

The NKit v1 reference implementation shows that Wii encoding is not ordinary scrubbing:

* it sets the top-level Wii header to no-hashes/no-encryption;
* it may extract and remove the update partition, recording its CRC;
* it writes partition payloads as decrypted `0x7c00` data portions without the normal `0x400`
  per-cluster hash portions;
* it compacts files, updates FST file offsets and DOL/FST header offsets, and records gaps;
* it replaces predictable gaps with typed run descriptions;
* it can remove an FST-listed file whose content is exactly deterministic junk and record enough
  information to recreate it;
* it stores flags plus exceptional hash headers for partition groups whose original hash material
  cannot be reproduced by the normal rule;
* it retains/uses original-size and CRC metadata so a reconstructed stream can be checked.

The inverse must parse and validate before trusting any encoded length or offset. At a high level:

```text
outer BlobReader
  -> validate Wii magic + exact NKit v1 metadata
  -> inventory represented partitions and recovery requirements
  -> decode compacted files and typed gap records
  -> restore original file/FST/DOL/partition offsets
  -> regenerate deterministic junk and explicit fill/literal spans
  -> restore each partition's decrypted payload length
  -> restore/preserve Wii hash structures for 64-cluster groups
  -> encrypt 0x7c00 payloads into conventional 0x8000 clusters
  -> expose a conventional raw Wii disc address space
```

`VolumeWii::HashGroup` and `VolumeWii::EncryptGroup` already implement conventional H0/H1/H2
construction and AES encryption and should be reused where their interfaces fit. The NKit hash
flag/exception stream still needs its own parser, and the reconstructed H3 relationship must be
validated. Blindly recalculating all hashes is not sufficient for exceptional/custom groups and
can conflict with the H3 table/TMD content digest.

### Requirement matrix

| Item | Classification | Evidence/consequence |
| --- | --- | --- |
| Clear top-level and inner NKit metadata | Required | `NkitReaderWii` clears `0x200..0x21b`; exported conventional view must not remain NKit |
| Restore Wii hash/encryption flags | Required | NKit writer sets `0x60/0x61` to `1`; conventional raw partitions require both back to `0` |
| Restore/normalize partition table | Required, variant-dependent | Retained-update form can restore represented entries; removed-update form needs a playable-vs-archival policy |
| Restore partition and file mappings | Required | NKit compacts files and rewrites FST, DOL, FST, partition offsets, and sizes |
| Decode typed gap records | Required | Gap records distinguish junk, scrub/fill, literal data, repeats, and junk-file removal |
| Regenerate deterministic junk | Required for playable correctness | Some FST-listed files can be removed as junk; a game may legally read them, so they cannot simply be omitted |
| Restore arbitrary non-junk gap bytes | Required when embedded | Mixed gaps carry literal spans that must be copied |
| Reproduce byte-perfect unused filler | Not required for playable WBFS; required for archival identity | The WBFS scrub map can omit proven-unused regions, but original CRC recovery needs exact bytes |
| Handle previously scrubbed regions | Required to classify; exact recovery may be impossible | v1 encodes scrub/fill states; an image made from an already lossy source does not magically contain the prior bytes |
| Restore update partition contents | Not required for game-partition playback; required for archival original when originally present | Removed-update CRC points to an external recovery partition |
| Restore channel/VC/non-game partitions | Variant-dependent | Normally retained by NKit encoding; an already-stripped source can require external partition files for archival repair |
| Restore per-cluster hashes | Required | Conventional Wii raw partition clusters contain a `0x400` hash area |
| Restore H3/TMD consistency | Required | Retained data must pass integrity validation; exceptional groups use preserved hash material |
| Restore partition encryption | Required | USB loaders and the conventional WBFS path consume raw encrypted Wii disc representation |
| Restore disc logical size/address space | Required | NKit stream length is not the conventional disc length; `0x210 * 4` supplies the Wii output size |
| External recovery database/files | Not required for a structurally valid playable subset; variant-dependent for archival exactness | The reference separates NKit expansion from a later recovery pass |
| Byte-identical Redump CRC/SHA result | Not required for playable export; required for archival-perfect status | This is a distinct product promise and must never be inferred from “boots” |

### Raw versus decrypted offsets

The final reader's address space is the conventional disc/raw space used by `BlobReader::Read`:

* non-partition areas use disc offsets directly;
* Wii partition payloads occupy `0x8000` raw clusters;
* each raw cluster contains `0x400` hash bytes plus `0x7c00` encrypted data bytes;
* `VolumeWii` translates decrypted partition offsets to those raw clusters.

An interface that exposes only the `0x7c00` decrypted stream is useful inside reconstruction but
cannot be passed directly to `WriteWbfs`.

### Open proof items, not assumptions

The following need synthetic vectors and later controlled real-image proof:

* exact behavior of every hash-preservation flag/exception combination;
* update-removed dual-layer and unusual partition layouts;
* retail Korean common-key handling through Dolphin's existing ticket code;
* NKit produced from custom, corrupt, RVT, or already-lossy inputs;
* real USB Loader GX behavior for a normalized disc that intentionally omits a missing update
  partition from the reconstructed partition table;
* cancellation granularity and cache bounds for an on-demand encrypted group generator.

## 5. External recovery data and fidelity

### Decisive answer

**No: not every Wii NKit image can be reconstructed to a byte-identical original ISO from the file
alone.** An NKit v1 image may externalize its update partition (`0x218` is the partition CRC), or it
may have been created from a source that had already lost update/channel/scrubbed data. The
reference recovery layer consumes standalone update and channel/VC partition files, Redump/custom
data, region/header candidates, and rare junk patches.

For the deliberately supported playable subset, the conclusion is different: a valid retail Wii
NKit v1 image with an intact, structurally valid data partition contains or deterministically
describes the game-partition material needed to build a conventional playable representation. A
missing update partition is not needed to launch the game. RWiN can retain the current NKit table
that omits that partition, restore the present partitions to conventional raw form, and report that
archival recovery is unavailable. This is an implementation inference supported by the reference
reader's ability to expand without the update file; it still requires N3/N4 structural and real-Wii
acceptance tests before becoming a support claim.

If game-partition reconstruction or integrity validation fails, RWiN must not publish a WBFS and
must classify the source as incomplete/unsupported. “NKit” alone is not proof that the input is
recoverable.

### Reference behavior

The v1 `Converter` uses:

* `ConvertToIso`: `NkitReaderWii` only;
* `RecoverToIso`: `NkitReaderWii`, then `RecoverReaderWii`.

When the NKit header names a removed update partition and no matching recovery file exists,
`NkitReaderWii` inserts filler, continues, and reports the result as recoverable rather than
aborting. `RecoverReaderWii` is the separate archival repair pass that can insert update/channel
partitions and attempt a known-dump match.

### Required RWiN states

The reconstruction result should keep gameplay and archival status orthogonal:

| State | Meaning | Export action |
| --- | --- | --- |
| `PlayableReady` | Conventional data partition reconstructed and validated; no known missing archival data | May proceed after NKit support is deliberately enabled |
| `PlayableReadyArchivalDataMissing` | Playable data partition validated; update/auxiliary/original filler is unavailable | May proceed with an explicit non-archival notice |
| `ArchivalExactReady` | Reconstructed bytes verified against embedded/source checksums, with any required recovery data | May proceed; still do not market WBFS as archival |
| `IncompleteOrInvalid` | Gameplay-critical data, mapping, crypto/hash consistency, or required structure cannot be proven | Block; never create final output |
| `UnsupportedVariant` | GC NKit, unknown v1 version, NKit 2/dedupe, RVT/custom form outside tested policy | Block with a precise diagnostic |

No automatic recovery-data download belongs in N2 or N3. Recovery data is also potentially
copyrighted partition content and must be user-supplied under a separate, reviewed policy if
archival recovery is ever implemented.

## 6. Reference implementations and sources

### Dolphin source/history

The authoritative evidence is the local source named throughout this document, especially:

* `Source/Core/DiscIO/Blob.h`, `Blob.cpp`, `Volume.cpp`, `VolumeDisc.cpp`, `VolumeWii.cpp`;
* `Source/Core/DiscIO/DiscScrubber.cpp`, `WbfsWriter.cpp`;
* `Source/Core/DolphinQt/GameList/WiiExportGameListPreview.cpp` and
  `WiiExportGameListExecution.cpp`;
* `Source/Core/UICommon/WiiExportPlan.cpp`, `WiiExportNativeBackend.cpp`;
* `Source/Core/DiscIO/DiscExtractor.cpp`, `VolumeVerifier.cpp`;
* commits `2e8c5b4521d` and `ee19ff66b4`.

The official Dolphin progress report for May/June 2020 documents that NKit moves files earlier on
disc, can increase emulated load times, can break games, and differs from lossless RVZ:
<https://dolphin-emu.org/blog/2020/07/05/dolphin-progress-report-may-and-june-2020/>.

### NKit v1 source

The public v1.4 source inspected was the `Ryan-Myers/NKit` fork of `Nanook/NKitv1`, commit
`61dd683b4b70273a37c4513726b87943f2e32e37`:
<https://github.com/Ryan-Myers/NKit/tree/61dd683b4b70273a37c4513726b87943f2e32e37>.

Useful components and their roles:

| Component | Role | RWiN treatment |
| --- | --- | --- |
| `Conversion/Readers/NkitReaderWii.cs` | Parses v1 metadata; expands files/gaps; restores sizes, hashes, encryption, and optional update data | Reimplement behavior behind Dolphin interfaces; use as test/oracle evidence |
| `Conversion/Writers/NkitWriterWii.cs` | Definitive inverse evidence for what v1 removes/rewrites | Use to derive invariants and fixture encoder, not ship writer code |
| `Conversion/Gaps.cs` | Typed gap/run encoding | Reimplement small format decoder with independent tests |
| `FilesAndStreams/JunkStream.cs` | Deterministic Wii junk generation | Reimplement and verify with known legal vectors |
| `Conversion/WiiHashStore.cs` | Flags and exceptional stored hash headers | Reimplement parser; reuse Dolphin hash/AES primitives |
| `DiscImage/Wii/WiiPartitionGroupEncryptionState.cs` | Hash validation/regeneration and encryption state | Reuse concepts; prefer `VolumeWii::HashGroup/EncryptGroup` |
| `Conversion/Readers/RecoverReaderWii.cs` and `Settings/RecoveryData.cs` | Separate archival repair using partition files/dat knowledge | Do not include in playable N2/N3 path |

The NKit 2 project/wiki provides useful product-level confirmation that v1 used
`nkit.iso`/`nkit.gcz`, compacted file systems, and removed predictable junk, and that NKit 2 reads
but no longer writes v1: <https://github.com/Nanook/NKit/wiki/Home>. Its processing documentation
also states that NKit ISO/GCZ is expanded to a temporary ISO before conversion:
<https://github.com/Nanook/NKit/wiki/Processing-Tasks>.

### NKit 2 / nod comparison

`encounter/nod` commit `ac12dba52493328e803a02df947d07f1fd82e168` was inspected only to avoid
conflating formats: <https://github.com/encounter/nod>. Its NKit 2 header is container metadata for
lossless WBFS/CISO/WIA, with size/digests and junk bits; it is not the inner `NKIT v01` v1 stream.
Its buffered reader/writer architecture is a useful concept, not a v1 reconstruction source.

### Licensing

* Dolphin is GPL-2.0-or-later.
* The inspected NKit v1 source is MIT licensed (copyright Nanook, 2019). MIT code is compatible with
  a GPL project, but any copied or substantially adapted portion requires retention of the MIT
  notice. The preferred path is a documented behavioral reimplementation plus independently
  generated vectors, with explicit attribution in source where an algorithm is adapted.
* `encounter/nod` is dual MIT/Apache-2.0. It is not needed for v1 and should not be imported merely
  to gain its NKit 2 container metadata.

Do not copy the C# implementation wholesale. Its forward-only pipeline, exception model, global
settings/recovery behavior, and concurrency do not match Dolphin's `BlobReader` contracts.

## 7. Architecture comparison

### Architecture A: temporary reconstructed ISO

```text
NKit -> forward reconstruction -> exclusive temporary ISO
     -> CreateDisc -> AnalyzeWbfs -> existing writer
```

Advantages:

* closest to the proven v1 forward reader and easiest to compare against an oracle;
* normal Dolphin classes validate the exact boundary before WBFS is involved;
* random reads, source copies, and revalidation become ordinary ISO operations;
* failures are easy to inspect with synthetic fixtures;
* best early proof of partition remapping, hashing, and encryption.

Costs/risks:

* requires roughly 4.7 GiB (single layer) or 8.5 GiB (dual layer) of temporary space in addition to
  WBFS staging/final space;
* writes every reconstructed byte even when WBFS will omit most unused blocks;
* adds a full extra sequential I/O pass;
* cancellation and crash cleanup must cover an exclusive temporary file;
* preview cannot cheaply call conventional `AnalyzeWbfs` unless reconstruction happens before the
  user chooses Export;
* poor final UX on space-constrained destinations.

Requirements for the N3 proof: create with no-replace semantics under a disposable test root,
check cancellation between bounded chunks/groups, use `ScopeGuard` cleanup, flush/close before
reopening, validate `!IsNKit()`, `HasWiiHashes()`, `HasWiiEncryption()`, partition/FST reads, and
`AnalyzeWbfs`, then delete the temporary ISO. Never publish it as a user output.

Assessment: **recommended N3 proof architecture; not recommended final architecture**.

### Architecture B: on-demand reconstructed DiscIO reader

```text
outer NKit BlobReader
  -> immutable parsed/indexed NKitV1ReconstructionPlan
  -> NKitV1ReconstructedBlobReader
  -> VolumeWii / AnalyzeWbfs / WriteWbfs
```

The reader must implement:

* `GetDataSize()` as the conventional size from validated NKit metadata;
* `GetDataSizeType()` as `Accurate` only after a complete valid plan is built;
* a prepared/effective conventional type (normally `PLAIN`), while retaining the original outer
  type separately for UI/revalidation;
* `GetRawSize()` consistently for source matching (prefer the virtual conventional size, with an
  independent original-source fingerprint outside `WbfsAnalysis`);
* `CopyReader()` with an independent outer reader and caches but the same immutable plan;
* arbitrary, overflow-safe `Read(offset, size)` across copied, generated, hashed, encrypted, and
  zero/fill spans.

The forward v1 encoding first needs a bounded scan that validates every encoded record and builds
an output-to-input span/group index. Partition groups align naturally with the 2 MiB raw group size
(`64 * 0x8000`), also the current WBFS block size. Cache one or a small bounded number of fully
reconstructed/encrypted groups. Reads spanning regions stitch header/filler/group data. The reader
itself remains non-thread-safe, like `BlobReader`; `CopyReader` provides parallel independence.

Cancellation does not belong in `BlobReader::Read`, so plan construction and explicit prewarming
need cancellable APIs. Individual group reconstruction must remain bounded. Avoid spawning
unbounded nested async tasks when using `VolumeWii::HashGroup/EncryptGroup`; select a conservative
worker policy in the calling reconstruction service.

Advantages:

* no multi-gigabyte ISO;
* Analyze and WBFS writing share one normal reader abstraction;
* WBFS reads only selected blocks after analysis;
* supports preview and execution revalidation without materializing an output;
* reconstruction remains reusable by future validation or conversion code.

Costs/risks:

* hardest mapping/caching implementation;
* an initial index scan is unavoidable for variable-length gap/file records;
* analysis may read decrypted file-system ranges in a pattern different from sequential WBFS
  writing, so caching must avoid repeated crypto work;
* original-source identity and virtual-reader identity must be modeled separately;
* malformed lengths, integer overflow, and adversarial random reads require strict testing.

Assessment: **recommended final architecture**.

### Architecture C: direct NKit-aware WBFS adapter

```text
NKit -> special reconstruction/export adapter -> WBFS writer
```

If the adapter implements a conventional random-readable interface, it is Architecture B under a
different name. If it writes WBFS directly, it must duplicate or bypass `DiscScrubber`, block-map
analysis, source matching, temporary-output validation, and normal `VolumeWii` validation. It also
makes NKit reconstruction unusable outside WBFS and encourages assumptions that omitted NKit bytes
can map directly to omitted WBFS blocks.

Assessment: **reject**. Keep one WBFS implementation.

### Summary

| Criterion | A: temporary ISO | B: on-demand reader | C: direct adapter |
| --- | --- | --- | --- |
| First proof correctness | Best | Harder | Poor boundary |
| Disk cost | High | Low | Low |
| Implementation complexity | Medium | High | Medium initially, high long-term |
| Existing C2-C6 reuse | Complete after ISO | Complete | Partial/duplicated |
| Cancellation/cleanup | File cleanup plus two passes | Plan/group cancellation, no ISO | Coupled to output |
| Testability | Excellent oracle artifact | Excellent after span tests | WBFS-specific |
| Final maintenance | Duplicate intermediate path | Clean DiscIO abstraction | Tight coupling |

## 8. Recommended integration architecture

### N3 proof

Build a forward-only `NKitV1SequentialReconstructor` over the N2 parser/gap primitives. Its sole N3
consumer writes an exclusive temporary ISO in a controlled test/proof API. Reopen it through
`CreateBlobReader`/`CreateDisc`, validate it as conventional Wii, and call `AnalyzeWbfs`. Do not add
Game List support or a production conversion button in N3.

### Final production path

Build `NKitV1ReconstructedBlobReader` on the same immutable plan and reconstruction primitives.
Introduce a source-preparation seam before `AnalyzeWbfs`:

```text
original path
  -> open outer reader + original VolumeWii
  -> exact NKit v1 analysis
  -> original-source fingerprint + reconstruction plan
  -> reconstructed reader (effective conventional PLAIN)
  -> CreateDisc(reconstructed_reader->CopyReader())
  -> existing AnalyzeWbfs
  -> existing planner / preview / execution contracts
  -> execution recreates and revalidates original fingerprint + plan
  -> existing WriteWbfs(reconstructed_reader, analysis)
```

Only after this path passes synthetic and controlled real validation should the native descriptor
advertise `NKitInput`. `AnalyzeWbfs`'s NKit rejection remains correct for unreconstructed input.
`WriteWbfs`, its split writer, final-output validation, collision policy, and C6 lifecycle should
remain unchanged.

The prepared-source model will eventually need:

* original outer blob type and NKit version;
* effective reconstructed blob type;
* original-file identity/fingerprint;
* immutable reconstruction-plan identity;
* playable and archival assessments;
* expected WBFS analysis over the reconstructed reader.

## 9. Exact N2 implementation boundary

N2 is a production-quality, read-only reconstruction **foundation**, not an exporter.

### Files and types

Add:

* `Source/Core/DiscIO/NKitV1.h`
* `Source/Core/DiscIO/NKitV1.cpp`
* `Source/Core/DiscIO/NKitV1Reconstruction.h`
* `Source/Core/DiscIO/NKitV1Reconstruction.cpp`
* `Source/UnitTests/Core/NKitV1Test.cpp`
* the minimal `DiscIO` and unit-test CMake list entries.

Proposed public types:

```text
NKitV1Metadata
NKitV1PartitionMetadata
NKitV1Analysis
NKitV1Error
NKitV1RecoveryRequirement
NKitV1PlayableAssessment
NKitV1ArchivalAssessment
NKitV1ReconstructionPlan
NKitV1GapSpan
NKitV1GapDecodeResult
```

Proposed functions/classes:

```text
AnalyzeWiiNKitV1(BlobReader&)
BuildWiiNKitV1ReconstructionPlan(BlobReader&, const NKitV1Analysis&)
DecodeNKitV1Gap(...)
NKitV1JunkGenerator
```

Keep format-detail helpers private unless N3 needs them. Do not add a new `BlobType`; NKit v1 is an
inner representation.

### Responsibilities

`AnalyzeWiiNKitV1`:

* require Wii magic and exact `NKIT v01`;
* parse all known top-level fields as big-endian;
* validate `original_size_quads * 4` without overflow, size/alignment, source bounds, ID, partition
  table bounds/counts/ordering, and presence of one supported data partition;
* distinguish update-preserved and update-removed forms;
* explicitly reject GameCube, unknown versions, NKit 2, RVT/custom unsupported layouts, truncated
  metadata, and impossible sizes.

`NKitV1ReconstructionPlan`:

* be immutable after construction;
* retain original outer type/size and a source-header fingerprint;
* contain conventional output size, represented partition inventory, recovery requirements, and
  separate playable/archival assessments;
* synthesize the normalized conventional top-level disc header in memory. For playable mode with a
  removed update, keep only represented partitions rather than creating an invalid table entry;
* never claim `DataSizeType::Accurate` for a future reader until the whole v1 record scan validates.

`DecodeNKitV1Gap` and `NKitV1JunkGenerator`:

* decode all-junk, all-scrub/fill, mixed, repeat, literal, extended-length, and junk-file forms with
  checked arithmetic and strict input/output limits;
* produce typed output spans or reconstruct a caller-bounded memory range;
* support deterministic seeking by disc/partition ID, disc number, logical length, and output
  offset;
* never write a file and never mutate the input reader.

### Typed errors

At minimum distinguish:

```text
ReadFailed
NotWiiDisc
NotNKit
UnsupportedVersion
UnsupportedPlatformOrVariant
TruncatedHeader
InvalidOriginalSize
InvalidGameId
InvalidPartitionTable
MissingDataPartition
MalformedGapRecord
SpanOverflow
UnexpectedEndOfInput
RecoveryDataRequiredForArchival
GameplayDataIncomplete
```

Errors must include a safe byte offset/partition index where applicable, but no game-data dump.

### What N2 actually proves

N2 will perform three real inverse operations entirely in memory on legal synthetic bytes:

1. normalize a v1 top-level Wii disc header, clearing NKit metadata and restoring conventional
   hash/encryption flags;
2. decode v1 gap/run records into bounded literal/fill/junk output spans;
3. regenerate deterministic Wii junk at arbitrary offsets and match fixed test vectors.

It will also produce a validated reconstruction/recovery plan for update-present and
update-removed synthetic headers. It will **not** yet rebuild partition payloads, hash/encrypt full
groups, create an ISO/WBFS, alter Game List behavior, or advertise backend capability.

### Initially recognized input

N2 recognizes exact retail Wii `NKIT v01` metadata through an accurate, random-readable outer
`BlobReader`. Tests must cover PLAIN and a synthetic GCZ-equivalent reader type at the parser layer,
but no outer container is advertised until an on-disk test exists. GameCube v1 is detected and
returned as unsupported, not accidentally parsed with Wii size semantics.

## 10. N3 expected proof target

N3 should extend the foundation just far enough to reconstruct one fully synthetic minimal retail
Wii NKit v1 image to a temporary conventional ISO. The synthetic image must include a data
partition, adjusted FST/file positions, at least one encoded deterministic gap, one literal/fill
case, and enough ticket/key/hash structure to exercise group hashing and encryption.

N3 succeeds only if:

* the source bytes remain unchanged;
* cancellation removes the temporary ISO;
* the reconstructed size and normalized header match the plan;
* `CreateDisc` returns `VolumeWii`, `IsNKit()` is false, and hashes/encryption are enabled;
* the data partition and synthetic FST/file can be read;
* retained groups pass block/H3 integrity checks used by the fixture;
* `AnalyzeWbfs` succeeds on the reconstructed ISO;
* malformed and recovery-required cases fail before finalization.

Do not connect N3 to the Game List and do not process a real image unless a later prompt supplies an
explicit controlled path.

## 11. Legal synthetic fixture strategy

No fixture may contain Nintendo/game files, keys copied from a title, or recovery partitions.

### N2 fixtures

Build byte vectors in the test itself:

* a synthetic alphanumeric Wii ID and Wii magic;
* exact/incorrect/truncated `NKIT v01` strings;
* valid single-layer and dual-layer size fields plus overflow/unaligned/out-of-range values;
* minimal bounded partition tables with synthetic offsets/types;
* update CRC zero/nonzero;
* each gap type, mixed run, repeat, literal payload, extended length, and malformed/truncated run;
* deterministic junk generated from a made-up ID/disc number, with fixed expected byte hashes or
  small byte vectors checked into the test;
* mismatched platform magic, ID, version, and partition count.

Generate expected vectors with a tiny independent script or the MIT reference implementation,
record their provenance in test comments, and validate them in at least two implementations before
freezing them. Do not run or bundle the external reference at test time.

### N3 fixture builder

A meaningful full Wii fixture is larger in structure than the existing filesystem-free
`WbfsWriterTest` fake. Create a reusable test-only builder that:

1. generates an entirely synthetic conventional Wii data partition in memory;
2. uses a test-owned AES key/ticket-like structure accepted by the fixture boundary;
3. uses Dolphin's hash/encryption primitives to make conventional groups;
4. encodes that known disc into a minimal NKit v1 stream using a small test-only encoder that is
   deliberately independent from the production decoder;
5. retains the original conventional bytes as the oracle;
6. compares sequential and random reconstructed reads against the oracle before WBFS analysis.

If constructing a fully valid signed Wii partition is unnecessary for the first decoder tests,
inject an abstract partition-group source and test mapping/hash/encryption separately. Do not make a
fake so shallow that it proves only the four-byte marker; N3 must cross the real raw/decrypted
partition boundary.

Cancellation tests should stop during plan scan, gap generation, group reconstruction, and
temporary writing. Malformed input tests should include arithmetic overflow, overlapping/backward
spans, impossible output lengths, truncated preserved-hash data, and an incorrect synthetic ID.

## 12. Risks and open questions

The three largest risks before N2/N3 are:

1. **Format completeness and exceptional hashes.** The public v1 code is the best available
   behavior specification but is complex and lightly unit-tested. Hash-preservation, scrubbed
   mixed groups, and odd/custom discs can silently produce structurally plausible bad data unless
   every boundary is checked.
2. **Playable missing-update policy.** Omitting an unavailable update partition from the normalized
   table is cleaner than the reference reader's invalid filler placeholder for a WBFS scrub pass,
   but it needs synthetic validation and a later real USB Loader GX acceptance test before being
   called supported.
3. **Random-access performance/correctness.** Turning a forward variable-length format into a
   deterministic `BlobReader` requires a complete safe index, bounded encrypted-group cache, stable
   `CopyReader` behavior, original-source identity, and cancellation outside `Read`.

Additional questions:

* Should the final reader report effective `BlobType::PLAIN`, or should `WbfsAnalysis` be decoupled
  from `BlobType` through a prepared-source descriptor? Do not add `BlobType::NKIT`.
* How should the source fingerprint cover a compressed outer file without hashing a multi-gigabyte
  source during every preview? At minimum use validated metadata, outer raw/logical sizes, header,
  and stable filesystem identity, then recheck before execution.
* Can `VolumeWii::EncryptGroup` be adapted without its current internal async fan-out causing
  excessive reconstruction threads?
* Which outer containers should the first real acceptance matrix include after ISO and GCZ?
* What exact integrity checks define `PlayableReady` for a deliberately normalized, non-archival
  disc?

## 13. Explicitly out of scope

N1/N2/N3 do not include:

* enabling NKit in Game List export or advertising `NKitInput`;
* removing any current NKit blocker;
* production NKit-to-ISO or NKit-to-WBFS conversion;
* real user-image discovery, opening, hashing, or modification;
* recovery-data downloads, databases, or bundled recovery partitions;
* GameCube NKit export;
* NKit 2 lossless WBFS/CISO/WIA or deduped scans;
* external NKit/WIT tool execution;
* overwrite, batch, release, C7, or C8 work.

## 14. Recommended N2 objective

> Implement the read-only Wii NKit v1 reconstruction foundation defined in
> `docs/WiiExportNKitArchitecture.md`: exact metadata/partition validation, typed playable and
> archival recovery assessments, immutable reconstruction planning, normalized in-memory disc
> header generation, bounded v1 gap decoding, and deterministic random-offset Wii junk generation,
> with comprehensive legal synthetic tests. Recognize only exact supported retail Wii `NKIT v01`
> inputs, keep all Game List/backend NKit blockers intact, write no ISO/WBFS, use no real game data,
> and stop before partition group reconstruction or export integration.

## 15. N2 implementation status

N2 implements the bounded read-only foundation on branch
`feature/wii-export-n2-nkit-foundation`, based on N1 commit
`67f2d7cf222a7b6c8c06bc2c4b57d7c44b022aba`. It remains disconnected from Wii Export capability
advertising and execution.

### Production and test files

Production code is in:

* `Source/Core/DiscIO/NKitV1.h`
* `Source/Core/DiscIO/NKitV1.cpp`
* `Source/Core/DiscIO/NKitV1Reconstruction.h`
* `Source/Core/DiscIO/NKitV1Reconstruction.cpp`
* `Source/Core/DiscIO/CMakeLists.txt`
* `Source/Core/DolphinLib.props`

Legal synthetic coverage is in `Source/UnitTests/Core/NKitV1Test.cpp`, registered by
`Source/UnitTests/Core/CMakeLists.txt`. The test fixture builds its fixed headers, partition tables,
inner metadata, gap records, and made-up IDs entirely in memory; no binary game fixture is stored.

### Public foundation API

`AnalyzeWiiNKitV1(BlobReader&)` returns `NKitV1Result<NKitV1Analysis>`. Valid analyses contain
getter-only `NKitV1Metadata`, `NKitV1PartitionMetadata`, `NKitV1RecoveryAssessment`, and a SHA-1
fingerprint of the fixed source header. `DecodeNKitV1Gap` returns bounded getter-only
`NKitV1GapSpan` instructions. `NKitV1JunkGenerator` generates a caller-provided byte range.
`BuildWiiNKitV1ReconstructionPlan` is the only plan construction path. Validated metadata,
analyses, spans, decode results, junk generators, and plans cannot be default-constructed into a
half-valid state.

The typed error model is `NKitV1ErrorCode` plus safe source offset and partition index fields. It
distinguishes identification/version/platform failures, inaccurate or failed reads, truncated
metadata, invalid IDs/sizes/tables/ranges/geometry, arithmetic overflow, malformed/truncated or
over-limit gaps, overlap, external recovery requirements, and unsupported reconstruction features.
No source-controlled text is returned.

### Initially supported subset and validation

The parser consumes Dolphin's already-decoded logical `BlobReader` stream, so PLAIN and compressed
outer containers retain their existing `BlobType`; NKit is not a new blob type. The supported
subset requires an accurate logical size, Wii magic, exact `NKIT v01`, alphanumeric synthetic-safe
ID fields, both NKit decrypted/no-hash flags set, an exact retail single- or dual-layer original
size, a bounded conventional four-group partition table, and at least one represented data
partition. Each represented partition must have a bounded, non-overlapping, `0x8000`-aligned source
range, the v1 `0x20000` compacted-data offset, an exact inner `NKIT v01`, and valid raw/decrypted Wii
cluster geometry.

Every offset, multiplication, addition, table range, source read, literal range, reconstructed
range, and plan range is checked before use. Counts are capped by the fixed v1 partition-table
layout, reads and allocations have fixed upper bounds, gap instruction count is caller-bounded,
and a generated gap never allocates its reconstructed length. GameCube, version-like inner strings
such as `NKIT v02`, unknown partition types, unsupported flag combinations, malformed layouts, and
inaccurate logical streams fail with typed errors. In accordance with the N1 finding, `NKIT v02` is
reported as an unsupported inner version and is not mislabeled as NKit 2: actual NKit 2 is separate
outer-container metadata that this v1 logical-stream parser neither detects nor supports.

### Recovery, normalization, gaps, and junk

The reconstruction readiness state is deliberately named
`FoundationValidatedPartitionReconstructionPending`: N2 does not infer or claim playability before
partition reconstruction. A retained update reports `NoExternalRecoveryIndicated`; a nonzero
removed-update CRC reports `ExternalUpdateRecoveryRequired` with requirement
`RemovedUpdatePartition`. N2 does not locate, load, or download recovery material.

Plan construction copies the validated fixed `0x50000` header in memory, clears `0x200..0x21b`,
restores header bytes `0x60` and `0x61` to conventional hash/encryption values, preserves disc
identity and represented partition inventory, and records original output geometry, source-header
fingerprint, recovery assessment, and validated gap spans. Partition table offset remapping is not
performed until N3, so this normalized header is not yet exposed as a complete disc.

The gap decoder implements the v1 all-junk, all-scrubbed zero-fill, mixed junk/fill/literal,
repeat, extended-length, and junk-file forms. It reports consumed encoded bytes and coalesced typed
instructions in disc or decrypted-partition address space. Truncation, zero-progress mixed records,
repeat-without-predecessor, range overflow, output overflow, invalid address domains, and excessive
instruction counts are rejected.

Gap semantics and Wii junk seed derivation were behaviorally adapted from Nanook/NKit commit
`61dd683b4b70273a37c4513726b87943f2e32e37` under its MIT license; the full applicable notice is
retained in `NKitV1Reconstruction.cpp`. The recurrence itself reuses Dolphin's existing CC0
`LaggedFibonacciGenerator`. Junk is independently seeded for each `0x8000` segment and can generate
any caller-bounded range. Tests pin an independently calculated fixed vector and prove a nonzero
cross-segment slice matches full generation.

### N3 boundary and N1 evidence status

N2 does not reconstruct Wii `0x8000` raw clusters, generate their `0x400` hash areas or H0/H1/H2/H3
trees, encrypt partitions, remap compacted file systems, create a random-readable complete disc,
write a temporary ISO, write WBFS, or enable NKit in Wii Export. Existing `VolumeDisc::IsNKit()` and
`WbfsAnalysisError::NKitSource` behavior remain covered by regression tests.

No N1 architectural conclusion was contradicted. Source verification refined two implementation
details without changing the architecture: top-level bytes `0x214..0x217` remain intentionally
opaque because the v1 reader/writer does not require stronger semantics here, and Dolphin's
existing lagged-Fibonacci implementation can safely supply the recurrence after NKit-specific seed
derivation.

## 16. N3 implementation status

N3 adds the first complete synthetic conventional-Wii reconstruction proof on branch
`feature/wii-export-n3-nkit-reconstruction-proof`, based exactly on N2 commit
`9ecee0a966e322f8cf73b0a971dcee470d30a7f9`. It remains a DiscIO proof component: it neither
advertises NKit as an export input nor connects reconstruction to `AnalyzeWbfs`, `WriteWbfs`,
`ExecuteWiiExport`, or DolphinQt.

### Production and test files

The production implementation is in:

* `Source/Core/DiscIO/NKitV1SequentialReconstructor.h`
* `Source/Core/DiscIO/NKitV1SequentialReconstructor.cpp`
* `Source/Core/DiscIO/NKitV1.h`
* `Source/Core/DiscIO/VolumeWii.h`
* `Source/Core/DiscIO/VolumeWii.cpp`
* `Source/Core/DiscIO/CMakeLists.txt`
* `Source/Core/DolphinLib.props`

Legal synthetic coverage is entirely programmatic in
`Source/UnitTests/Core/NKitV1SequentialReconstructorTest.cpp`, registered by
`Source/UnitTests/Core/CMakeLists.txt`. No binary fixture, Nintendo asset, title-derived key, real
disc image, or recovery file is stored or opened.

### Sequential reconstruction architecture

`BuildWiiNKitV1SequentialReconstructionPlan` consumes the immutable N2 plan plus the same
read-only `BlobReader`. It completes a bounded validation scan before any output is published and
returns a getter-only `NKitV1SequentialReconstructionPlan`. Its disc and decrypted-partition
instructions are immutable `NKitV1SequentialSpan` values of kind source, fill, or deterministic
junk. `NKitV1SequentialPartition` holds the validated remapping, rebuilt partition header, and
decrypted group instructions.

`ReconstructWiiNKitV1Sequential` rechecks source type, sizes, and the N2 fixed-header fingerprint,
then sends monotonically increasing bytes to `NKitV1SequentialOutput::Write` or the semantically
equivalent `WriteZeros`. The latter lets a test file sink create a sparse zero tail without a disc-
sized allocation. The result reports output bytes, groups rebuilt, and the bounded working-set
estimate. A cancellation callback is checked before group materialization, between input spans,
and before each output operation. Output, cancellation, integrity, and layout failures use the N2
typed-error channel.

The N3 subset fixes its working set to one decrypted 64-cluster group, one encrypted raw group,
the partition hash material, and a 64 KiB disc-span buffer: less than 5 MiB in the focused proof.
Source-controlled compacted payload, FST, file, and prefix sizes are capped before allocation;
every offset addition, multiplication, decoded range, and output boundary is checked. Large zero
regions are never materialized.

### Supported N3 subset and remapping

The proof accepts exactly one represented data partition with no external recovery requirement,
one conventional `0x200000`-byte raw group (`64 * 0x8000`) representing `0x1f0000` decrypted
bytes (`64 * 0x7c00`), one root FST plus one regular file, and ordinary regenerated hashes. The
four-byte v1 exceptional-hash flags must all be zero. Multiple groups/files/partitions, preserved
hash exceptions, canonical filesystem-driven gap variants, removed update partitions, and
external recovery are explicitly rejected as unsupported or recovery-required.

The scanner decodes the compact disc prefix to derive the conventional partition offset, rewrites
the corresponding conventional partition-table entry in the N2 normalized `0x50000` header, and
restores the conventional partition data offset/size. Within decrypted partition data it copies
the header/FST prefix, clears the inner `0x200..0x21b` NKit metadata, restores the FST file offset,
decodes the pre-file and post-file gaps, and copies the aligned file bytes. The partition header's
H3 table is regenerated and its data-size field is restored before output.

### Raw groups, hashes, and encryption

N3 reuses Dolphin's authoritative `VolumeWii::HashGroup` H0/H1/H2 implementation and
`Common::SHA1` for H3 and the TMD content digest. For the single supported group, each conventional
raw cluster is rebuilt as the encrypted `0x400`-byte hash area followed by the encrypted
`0x7c00`-byte payload, for a `0x8000`-byte cluster. The group H3 is SHA-1 over H2; the remainder of
the fixed H3 table is zero in this one-group fixture. Planning independently verifies the generated
H3 table against the compact partition header and verifies the TMD content digest before output.

`VolumeWii::EncryptGroup` now also accepts an already-materialized decrypted group and an optional
single-threaded mode. Its default blob-backed behavior is preserved. N3 selects that bounded mode
so its resource bound is independent of host CPU count; both paths reuse Dolphin's AES context, conventional hash-
area zero-IV encryption, and payload IV at raw-header offset `0x3d0`. A synthetic title key is
wrapped into the synthetic ticket at test runtime through Dolphin's existing IOSC and recovered by
the normal `TicketReader` path. No Wii key bytes are added by N3.

### Independent synthetic oracle and NKit fixture

`BuildSyntheticConventionalWiiDisc` constructs a sparse, legal single-layer Wii byte oracle with
invented ID `RN3P01`, one data partition, a synthetic ticket/TMD, a minimal DOL/FST, and
`proof.bin` containing fixed invented bytes. It independently lays out the decrypted group,
generates H0/H1/H2/H3, encrypts all 64 raw clusters, and retains every meaningful conventional byte
through the partition end; the remaining conventional disc tail is defined as zero without being
allocated.

The test-only `BuildSyntheticNKitV1Fixture` starts from normal DiscIO decrypted reads of that
oracle and independently emits the selected compact v1 layout. It sets exact `NKIT v01` metadata,
applies top-level and inner size transformations, relocates the represented partition, adjusts the
compacted FST file offset, emits zero normal-hash flags, and encodes explicit mixed
junk/fill/literal gaps plus partition/disc zero tails. It does not invoke the reconstructor in
reverse. Both disc and partition gaps include deterministic junk, a nonzero fill, literal bytes,
and offsets that exercise partial random-offset junk generation.

### Disposable ISO, normal DiscIO, and WBFS-analysis proof

The only ISO writer is `SparseIsoOutput` inside the test translation unit. It accepts only the path
created by that test's `File::CreateTempDir`, uses sparse forward seeks only for semantic zero
writes, finalizes the expected synthetic size, and is removed by test teardown. There is no
production filename/path API.

The focused proof compares the complete stored conventional prefix through the raw partition end
against the independent byte oracle, including the normalized header, remapped table, partition
metadata, all 64 encrypted clusters, mixed gaps, FST, and file. It then reopens the disposable ISO
with normal `CreateDisc`, requires Wii detection, the synthetic ID, conventional hash/encryption
flags, `IsNKit() == false`, the expected data partition, valid block/H3 integrity, and byte-exact
`proof.bin`. Finally it calls only `AnalyzeWbfs`; the reconstructed conventional source is accepted
with nonzero used/output planning, while the compact NKit source remains rejected with
`WbfsAnalysisError::NKitSource`.

### Recovery boundary, refined evidence, and N4 boundary

Any N2 plan whose recovery requirement is not `None` is rejected before sequential-plan
construction. N3 does not load recovery files or silently downgrade an archival requirement into
the self-contained proof path.

Inspection of the exact public v1 reference used by N2 refined one format assumption: the N2 gap
decoder correctly exposes its raw bounded records, but the reference reader also applies
filesystem-context-dependent leading-null handling around some canonical junk gaps. N3 therefore
supports only the explicit gap records generated by its independent fixture and does not claim
general canonical v1 compatibility. This narrows the proof subset without changing the N1/N2
architecture.

N4 should turn the proven validated layout/group transform into a production random-access
reconstructed `BlobReader`: build a complete source-to-output index, handle multiple groups/files
and normal v1 gap context, add a bounded decrypted/encrypted group cache and stable `CopyReader`,
propagate cancellation outside `Read`, preserve source identity across preview/execution, and pass
that conventional virtual source to the existing WBFS analysis/writer pipeline. N4 must retain the
recovery policy and all current Game List/backend NKit blockers until that production path has its
own acceptance matrix. It should not require or expose a user-visible temporary ISO.

### N3 validation checkpoint

The configured Ninja `RelWithDebInfo` build has tests and Qt enabled. The aggregate `tests` target
and production `dolphin-emu` target both link successfully at `-j2`. Focused validation is 10/10
N3 reconstruction tests, 43/43 N2 foundation regressions, 6/6 relevant SHA-1/TMD/DiscIO/WBFS-
analysis regressions, and 5/5 no-output Wii Export bridge tests. The full Dolphin test suite was
not run. No validation step called `WriteWbfs` or `ExecuteWiiExport`.

## 17. N4 implementation status

N4 implements the production reconstructed-source boundary on branch
`feature/wii-export-n4-nkit-random-access`, based exactly on N3 commit
`df69aa3b69b9177408dcc56cb79d3af31f539b6c`. It remains disconnected from DolphinQt, Game List
capability advertising, and `ExecuteWiiExport`; an original compact NKit source still follows the
existing `AnalyzeWbfs` rejection path.

### Production files and API

The random-access implementation is in:

* `Source/Core/DiscIO/NKitV1ReconstructedBlob.h`
* `Source/Core/DiscIO/NKitV1ReconstructedBlob.cpp`
* `Source/Core/DiscIO/NKitV1SequentialReconstructor.h`
* `Source/Core/DiscIO/NKitV1SequentialReconstructor.cpp`
* `Source/Core/DiscIO/NKitV1.h`
* the DiscIO CMake and Visual Studio source lists.

`TryCreateWiiNKitV1ReconstructedReader` owns an already-decoded compact `BlobReader`, performs N2
analysis and foundation planning, completes the N3-derived filesystem/group scan, builds an
immutable `NKitV1ReconstructionIndex`, and returns a `NKitV1ReconstructedBlobReader` only when the
entire supported address space validates. Failures retain the typed `NKitV1Error` channel.
`PrewarmGroup` is the cancellable N5 execution seam; ordinary `BlobReader::Read` remains
deterministic because the base interface has no cancellation channel.

The effective reader reports `BlobType::PLAIN`, identical accurate raw/data sizes from validated
original-size metadata, block size zero, no compression metadata, and a conventional byte view.
It does not alter the original compact reader's container type or NKit marker.

### Immutable reconstruction index and random reads

The sorted index completely covers `[0, reconstructed_size)` without overlap or implicit holes.
Its range kinds are generated disc header, source-backed disc literal, fill/zero, deterministic
junk, generated partition header, and reconstructed raw partition group. Source and output bounds,
range adjacency, group count, H3 capacity, FST counts, file sizes, compact offsets, arithmetic, and
the final conventional geometry are checked before the reader is returned. `Read` binary-searches
this index and stitches arbitrary subranges; it never reparses compact metadata.

N4 generalizes the shared group transform to multiple complete 64-cluster groups. A requested
group materializes only its `0x1f0000` decrypted bytes from indexed source/fill/junk spans, clears
inner NKit metadata, applies every restored FST offset, validates the regenerated group H3 against
the planned H3 table, and reuses `VolumeWii::EncryptGroup` for the conventional encrypted 2 MiB
result. N3 sequential output now iterates the same transform, preserving the disposable-ISO proof
as an independent regression.

### FST and canonical gap context

The supported filesystem scanner accepts a bounded root/FST with multiple named, positive-size
regular files, including an FST entry order different from compacted data order. It validates the
bounded name table, sorts by validated compacted offset, decodes the preceding canonical gap,
reconstructs the conventional offset, and records a patch for the original FST field. Additional
directory entries remain outside this deliberately narrow subset and return a typed unsupported-
context result.

Reference v1 behavior establishes that leading nulls are filesystem context rather than gap-record
bits. N4 restores up to `0x1c` leading zero bytes when a junk span begins the first gap after the
FST or the final gap, and after an ordinary file when the internal gap is smaller than `0x40000`.
An internal gap of at least `0x40000` receives no leading-null substitution. This decision uses
validated file/gap position, never neighboring byte heuristics. Junk-file removal, zero-length
overlap conventions, malformed/overlapping compact offsets, and non-filesystem fallback streams
return typed unsupported-context errors.

### Cache, copies, and source identity

Each reader has a deterministic two-entry least-recently-used encrypted-group cache. Resident
derived bytes are capped at 4 MiB; group construction adds one bounded decrypted and one candidate
encrypted buffer, and no allocation scales with the conventional disc size. Hits, misses, builds,
evictions, resident groups, and resident bytes are observable for focused tests.

`CopyReader` copies the underlying source reader, shares only the immutable index/plan, and starts
with an independent empty cache and error state. Destroying either copy cannot invalidate the
other. This follows the base `BlobReader` contract, which explicitly makes one instance
non-thread-safe and uses copies for parallel ownership.

The plan retains the original outer type, logical/raw sizes, disc ID/metadata, and SHA-1 of the
bounded fixed NKit header. Creation and explicit revalidation compare all of them, and ordinary
reads revalidate the bounded header before returning virtual bytes. Partition payload changes are
also caught when regenerated H3 differs from the immutable plan. This is a stable reconstruction
identity check, not a claim to hash an entire multi-gigabyte outer image.

### Synthetic direct-reader and WBFS bridge proof

The N4 coverage extends `NKitV1SequentialReconstructorTest.cpp` with an entirely programmatic
three-group conventional oracle and independent compact v1 encoder. The fixture uses invented ID
`RN4P01`, synthetic ticket/title-key material, three differently sized files, nontrivial FST/data
ordering, canonical leading-null and non-leading junk contexts, mixed literal/fill/junk gaps, a
three-entry H3 table, and the normal Dolphin hash/AES path. No binary fixture or real game material
is used.

The reconstructed reader is compared byte-for-byte with the oracle across its complete stored
prefix and targeted random/cross-range/cross-group reads. Normal `CreateDisc` opens a conventional
Wii volume directly over `CopyReader`, reports `IsNKit() == false`, validates all three group/H3
relationships, and reads all three files. `AnalyzeWbfs` accepts that direct conventional view. A
test then passes the same production reader to the unchanged `WriteWbfs`, reopens the temporary
synthetic WBFS through normal DiscIO, verifies ID/FST/file bytes, and checks success/cancellation
cleanup. No ISO participates in this N4 production path, while the N3 temporary-ISO test remains.

### N4 support and rejection matrix

| Case | N4 result |
| --- | --- |
| Exact Wii retail `NKIT v01`, PLAIN accurate source, one retained data partition, single-layer geometry, one or more complete normal-hash groups, supported regular-file contexts | Supported |
| Multiple root-level regular files, reordered compact data, canonical leading-null and large internal junk gaps | Supported within fixed scan/file limits |
| Retained update/additional partitions | Unsupported but recognized |
| Removed-update CRC/recovery requirement | Recovery required; factory refuses self-contained reader |
| Preserved/exceptional hash flags or scrub/hash forms | Unsupported but recognized |
| Additional directory entries, junk-file removal, overlapping/zero-length-file context, non-filesystem fallback | Unsupported gap context |
| Malformed tables, ranges, FST, gaps, hashes, identity, or arithmetic | Malformed/invalid; no reader |
| GCZ/RVZ/WIA compact outer source or dual-layer Wii geometry | Out of current tested subset |
| GameCube NKit v1 | Recognized GameCube and rejected |
| `NKIT v02`-like inner version | Unsupported v1 version; not mislabeled as NKit 2 |
| NKit 2 container/deduplicated forms | Out of format and out of scope |

### Exact N5 boundary

N5 should introduce a prepared-source recipe in the Wii Export Assistant that preserves original
outer identity separately from this effective conventional reader, recreates and revalidates the
factory result for execution, and enables only the N4-supported matrix in preview/preflight/native
capability handling. Direct compact NKit must remain blocked, recovery/unsupported cases must stay
explicitly unavailable, and the existing `AnalyzeWbfs`/`WriteWbfs` staging, validation,
cancellation, collision, and publication code must remain authoritative. Controlled real-user
testing, additional outer containers, dual-layer coverage, exceptional hashes, and recovery data
remain later milestones.

### N4 validation checkpoint

The N4 checkpoint is configured as Ninja `RelWithDebInfo` with tests and Qt enabled. Focused N4,
N3, N2, relevant DiscIO, and Wii Export capability results are recorded in the N4 review report;
the complete Dolphin suite is intentionally not part of the default milestone validation.

## 18. N5 implementation status

N5 connects the N4 conventional reconstructed view to the existing Wii Export Assistant without
advertising compact NKit bytes as a native-writer input. It is based exactly on N4 commit
`f6b4d541b94f647f9c955fcc541eccae29f7d7ae`. The integration remains synthetic-only; no real NKit
image, user storage, everyday Dolphin installation, recovery service, overwrite path, or batch
workflow participates in this milestone.

### Prepared source recipe and capability assessment

`WiiExportPreparedSource` now carries a `WiiExportSourceRecipe` in addition to its effective
`WiiExportSource` and immutable `WbfsAnalysis`. `DirectDisc` retains the established ISO/RVZ path.
`ReconstructedNKitV1` records the compact outer type and accurate sizes, reconstructed size and
group count, the N2 fixed-header fingerprint, and a SHA-1 identity of the complete validated N4
reconstruction recipe. That recipe identity covers generated disc and partition headers (including
regenerated H3), sorted reconstruction ranges, decrypted spans, and FST offset patches. It is a
bounded validated-plan identity, not an unsupported claim to hash an entire retail image.

`PrepareWiiExportGameListSource` first opens the selected disc normally. A non-NKit Wii disc keeps
the old analysis path. An NKit-marked Wii disc is instead reopened as a compact `BlobReader`, passed
to `TryCreateWiiNKitV1ReconstructedReader`, opened as a conventional Wii `VolumeDisc`, checked for
the selected ID6 and `IsNKit() == false`, and analyzed by the unchanged `AnalyzeWbfs`. The effective
source supplied to planning is therefore accurate PLAIN and non-NKit. The original compact path and
identity remain solely in the recipe. The native descriptor still omits `NKitInput`, and the broad
direct `AnalyzeWbfs` compact-NKit rejection is unchanged.

N5 adds product-facing support classifications for supported v1, recovery required, unsupported
layout, unsupported FST/gap context, exceptional hash/scrub form, additional partitions,
compressed outer source, dual layer, GameCube, unsupported version, malformed source, source
change, cancellation, and generic reconstruction failure. DiscIO adds narrow typed error values for
outer-container, dual-layer, additional-partition, partition-layout, and exceptional-hash decisions
which N4 previously grouped under its conservative unsupported result. These refine reporting only;
they do not broaden reconstruction.

### Preview and execution-time recreation

For a supported recipe the preview shows `Source format: NKit v1` and `Reconstruction: Supported —
reconstructed during export`, while retaining the normal USB Loader GX WBFS layout, destination,
collision, free-space, and Ready/Ready-with-warnings logic. Its wording describes a playable WBFS
and explicitly avoids an archival-perfect ISO claim. ISO and RVZ presentations are unchanged.
Unsupported cases fail before a Ready dialog and use specific recovery/layout/context/hash/
container/version/corruption wording.

Preview never retains a reconstructed reader. `CreateWiiExportGameListExecutionRequest` snapshots
the immutable recipe. On Export, the worker rebuilds source preparation from the original compact
path with cancellation enabled, reruns the factory and normal `AnalyzeWbfs`, compares the compact
recipe identity, conventional WBFS fingerprint, ID6, support classification, destination facts,
collisions, free space, and every planned field. A mismatch requires a new preview. The default
backend factory then independently recreates the reader once more and compares the same recipe and
analysis before transferring sole ownership to `WiiExportNativeBackend`; no preview-local reader,
cache, cursor, or dangling reference crosses the worker boundary.

After this boundary, the existing `ExecuteWiiExport` and `WiiExportNativeBackend` remain
authoritative for no-overwrite planning, split naming, staging, `WriteWbfs`, progress validation,
structural WBFS validation, publication, cleanup, and result handling. There is no second writer or
NKit-specific output implementation. Direct compact NKit can therefore never reach `WriteWbfs` in
the integrated path.

### Cancellation, progress, and synthetic end-to-end proof

The existing worker-thread boundary performs all NKit planning and group work off the GUI thread.
The execution cancellation query is passed into both fresh source preparation and backend reader
recreation; N4 checks it during the expensive plan/group scan, and the existing writer continues
cooperative cancellation afterward. A cancelled preparation returns the normal cancelled result
without invoking a backend or committing output. The UI adds only a truthful indeterminate
`Preparing NKit reconstruction...` event before fresh reconstruction; writer byte progress and the
existing Validating/Finalizing/Completed stages are unchanged. It does not invent a reconstruction
percentage or promise interruption inside one bounded operation.

The focused N5 fixture uses the independent N4 three-group, three-file synthetic NKit encoder. It
exercises Game List source preparation, Ready planning, execution-time factory recreation, the
normal execution contract and native backend, temporary WBFS output, normal DiscIO reopen, and
byte-exact `alpha.bin`, `beta.bin`, and `charlie.bin` reads. The test also verifies that the compact
source remains `NKitSource` when analyzed directly, reconstructed planning requires PLAIN rather
than `NKitInput`, success leaves only the synthetic source and final WBFS, changed header/recipe and
changed support classification block before writer invocation, and cancellation commits no output.

### N5 support/rejection matrix

| Case | N5 preview/export result |
| --- | --- |
| Exact N4-supported Wii `NKIT v01` PLAIN/accurate/single-layer/one-data-partition/normal-hash/root-file subset | Ready via reconstructed conventional PLAIN source |
| One or multiple complete groups and multiple supported root-level regular files | Ready; covered by N4 recipe and synthetic end-to-end proof |
| Removed-update recovery requirement | Blocked: external recovery required |
| Retained additional partition or unsupported partition layout | Blocked with additional-partition/layout reason |
| Exceptional hash/scrub flags | Blocked with hash/scrub reason |
| Nested directory, junk-file, or unsupported canonical gap/FST context | Blocked with filesystem/gap-context reason |
| GCZ/RVZ/WIA or other compressed compact outer source | Blocked as compressed outer NKit |
| Dual-layer metadata | Blocked as unproven dual-layer reconstruction |
| Malformed, truncated, corrupt, overflowed, or identity-mismatched source | Blocked; no reader/backend |
| GameCube NKit | Ineligible/blocked; no GameCube support |
| Unsupported v1 marker such as `NKIT v02` and actual NKit 2 | Blocked; no NKit 2 claim |

### Exact N6 boundary

N6 may launch only the isolated N5 binary and inspect one legally owned Wii NKit image at a time.
It should first observe the support classification without export. Only a source classified inside
the exact N4/N5 supported matrix may proceed through preview, WBFS export, output reopen/validation,
and controlled boot/play testing. Any recovery, layout, hash, FST/gap, outer-container, dual-layer,
or version rejection is an acceptance result to record, not permission to weaken the boundary.
N6 must not enable additional variants, download recovery data, install over everyday Dolphin, or
touch unrelated SD/RWIN-GAMR storage.

### N5 validation checkpoint

The isolated Ninja `RelWithDebInfo` configuration has Qt and the aggregate tests enabled. Both the
`tests` target and production `dolphin-emu` target link successfully with every build invocation
limited to `-j2`. Validation is 6/6 N5-focused integration/UX tests, 8/8 N4 random-access tests,
10/10 N3 sequential/ISO proofs, 43/43 N2 foundation tests, 185/185 tests across the complete
affected Wii Export planner, execution, native-backend, writer, preview, Game List, and progress
families, and 3/3 representative Physical SD smoke tests. The full Dolphin suite was intentionally
not run because the bounded affected matrix was green. All N5 WBFS artifacts were synthetic,
temporary, reopened through normal DiscIO, and automatically cleaned; no real NKit/game or user
storage participated.

## 19. O1 retail compatibility: removed update partitions

O1 begins the retail-compatibility arc from N5 commit
`ba703fd1386a07b6d2b7e5f01af85b67293ec652`. Unlike N1-N5, O1 used one explicitly authorized,
legally owned retail image as a bounded read-only diagnostic input: *Kirby's Epic Yarn* USA,
ID6 `RK5E01`. No retail bytes are committed, and no ISO or WBFS was written from that source.

### Real diagnosis and canonical behavior

Kirby is a PLAIN, accurate, single-layer Wii `NKIT v01` source. Its top-level metadata has a
nonzero removed-update CRC32 (`0xafd3a24a`) at `0x218`, and its compact partition table retains one
data partition at source offset `0x58000`. The 32 KiB range at `0x50000` is the canonical removed-
update placeholder: its first `0x100` bytes preserve an original two-entry partition table with an
update partition at conventional offset `0x50000` and the data partition at `0x0f800000`; the rest
is zero padding. The retained data-partition header begins at `0x58000`, and compact decrypted data
begins at `0x78000`.

The old block was therefore exact but over-broad. `AnalyzeWiiNKitV1` classified the nonzero CRC as
`ExternalUpdateRecoveryRequired` / `RemovedUpdatePartition`, and
`BuildWiiNKitV1SequentialReconstructionPlan` rejected every non-`None` recovery requirement as
`ExternalRecoveryRequired` before inspecting the placeholder or retained filesystem.

Reference NKit behavior was checked at the already-pinned MIT-licensed commit
`61dd683b4b70273a37c4513726b87943f2e32e37`. `NkitWriterWii` and
`WiiPartitionPlaceHolder` preserve the original partition-table bytes in this 32 KiB placeholder
when removing an update. `NkitReaderWii` uses the saved data-partition offset and, when no recovery
partition is available, inserts filler through that offset and continues ordinary ISO conversion.
`RecoverReaderWii` is a separate archival-recovery pass. Thus external update data is needed for
byte-identical archival recovery, not for the retained game partition's playable conventional
view. O1 behavior is adapted from that documented algorithm and retains the existing MIT
provenance notice in the reconstruction implementation.

### Playable and archival policy refinement

`NKitV1RecoveryAssessment` now carries an independent `NKitV1PlayableAssessment`. The two proven
states are self-contained and synthetic-non-game-regions-required. A removed-update CRC continues
to report `ExternalUpdateRecoveryRequired` and `RemovedUpdatePartition` for archival truth, while
its playable assessment reports `SyntheticNonGameRegionsRequired`. Wii Export similarly carries
`requires_external_archival_recovery` separately from `is_nkit`: the reconstructed conventional
view remains non-NKit writer input, but planning no longer falsely reports that archival recovery
is unnecessary.

The sequential planner accepts only the format-driven canonical placeholder form inside O1's
existing one-data-partition subset. It requires exactly 32 KiB, zero padding after the saved
`0x100` partition-table region, one update entry at `0x50000`, one aligned retained data entry, no
other descriptors or partition types, and fully bounded offsets. It takes the conventional data-
partition offset from that saved table and emits one zero-fill index range from `0x50000` to the
retained data partition. The normalized conventional header continues to expose only the playable
retained data partition. Malformed placeholders, extra saved partitions, and absent retained game
data fail with typed errors; no title or ID-specific branch exists.

### Synthetic proof and real read-only reassessment

The N4 independent three-group/three-file fixture now has an O1 variant containing invented data,
a synthetic removed-update CRC, and an independently built canonical saved partition table. Tests
prove that playable and archival assessments differ, the production random-access reader builds,
the non-game region is zero-filled, representative partition bytes remain oracle-identical, normal
DiscIO validates hashes and all files, `AnalyzeWbfs` accepts the conventional view, and the existing
`WriteWbfs` produces a temporary synthetic WBFS which reopens with all known files. The compact
fixture remains directly blocked as `NKitSource`. Separate malformed-placeholder and missing-data-
partition fixtures remain blocked, and the N5 preview/execution test reaches Ready only for the
validated playable case while retaining the archival assessment.

After the change, Kirby no longer fails as external recovery required. Its read-only assessment
reports playable reconstruction with a synthetic non-game region and external archival update
recovery still required. The factory then reaches the next independent retail construct and stops:
the retained data partition has raw size `0x107ba0000` (4,424,597,504 bytes), consisting of 2,109
complete 2 MiB groups plus a 52-cluster (`0x1a0000`) final partial group. N4 currently requires the
raw size to be an exact complete-group multiple, so Kirby now returns `UnsupportedPartitionLayout`
at compact data offset `0x78000`. It does not yet reach Ready, and O1 deliberately does not combine
partial-final-group support with the removed-update policy fix.

### Exact O2 boundary

O2 should implement canonical partial-final-group reconstruction generically: validate the NKit
stored geometry and final-group hash/H3 semantics, reconstruct and encrypt only the represented
final clusters while providing the correct conventional partition-size view, add a legal
synthetic 2,109-complete-group-plus-52-cluster shape without allocating retail-sized test data, and
reassess Kirby read-only. It must not relax exceptional hash/scrub, additional-partition,
compressed-outer, dual-layer, nested-directory, GameCube, NKit 2, or recovery-download boundaries.

## 20. O2 retail compatibility: partial final groups and retail-scale indexing

O2 is based exactly on O1 commit `d56ff1cecbb181505b90377f448a1f9c0d5b037c`. It
generalizes the proven one-partition reader to a canonical partial final Wii hash group and removes
the whole-compacted-partition planning allocation. The compatibility change is entirely
format-driven; production code contains no title/ID-specific branch, and the one authorized retail
source remained a read-only diagnostic input.

### Canonical partial-group semantics

The prior rejection occurred in `BuildWiiNKitV1SequentialReconstructionPlan`: group count used
integer division by `VolumeWii::GROUP_TOTAL_SIZE`, raw partition size had to have zero remainder,
and decrypted size had to equal that full-group count times `GROUP_DATA_SIZE`. Fixed 2 MiB
assumptions then propagated through group materialization, sequential output, reconstruction-index
ranges, and cache copies.

Canonical NKit v1 behavior was rechecked at the pinned MIT reference commit
`61dd683b4b70273a37c4513726b87943f2e32e37`. `NkitReaderWii` chooses
`min(64, remaining / 0x7c00)` present blocks, clears unused slots in its 64-block group buffer, and
writes only `present_blocks * 0x8000`. `WiiPartitionGroupEncryptionState` marks only those blocks
used but calculates the full 64-slot hierarchy: unused blocks contribute 31 SHA-1 digests of a
zero `0x400` data sector, H1 covers each eight-block H0 set, H2 covers the eight H1 sets, and H3 is
SHA-1 of the complete H2 table. Encryption likewise operates over the zero-padded group, but only
physically present encrypted clusters are emitted. Dolphin's existing `VolumeWii::HashGroup` and
`EncryptGroup` already implement that hierarchy and AES/IV behavior when the missing decrypted
slots are zero-filled, so O2 reuses them unchanged.

`NKitV1PartitionGroupGeometry` now records group index, first cluster, present cluster count, exact
raw/decrypted offsets, and exact raw/decrypted sizes. Geometry is derived from a cluster-aligned
partition size. Every non-final descriptor must contain exactly 64 clusters; only the final
descriptor may contain 1 through 63. The materializer clears its bounded 64-block working buffer,
fills only the present decrypted range, hashes/encrypts the full canonical zero-padded hierarchy,
compares the generated H3 with the retained entry, and returns the exact valid encrypted size.
Sequential output, index ranges, and random reads expose only that returned size, so no fictional
cluster is readable past the declared partition end.

### Retail-scale plan, index, and cache

The O1 planning ceiling was caused by reading `source_stored_size` into one vector capped at 16
MiB, followed by eagerly materializing and hashing every partition group. O2 instead reads only the
fixed `0x440` payload header, the bounded prefix through the FST, the compact hash-flag table, and
one bounded encoded gap at a time. Regular file bodies remain source-backed spans regardless of
file size, alignment padding is checked in fixed 64 KiB chunks, and the compact partition tail is
decoded through bounded windows. The retained 0x18000-byte H3 table is checked against the TMD
content digest once during planning; each group's data-to-H3 relationship is checked lazily when
that group is requested. Thus factory cost is proportional to compact metadata/instructions and
descriptor count, not reconstructed bytes or the complete compact payload.

The immutable random-access index gives each final partial group its exact raw length and validates
group order, cluster counts, first-cluster/raw/decrypted offsets, sizes, complete address-space
coverage, and source bounds. Lookup remains binary search over sorted ranges. At Kirby-class
geometry, 2,110 group descriptors consume 118,160 bytes (56 bytes each); the corresponding 2,114
synthetic index ranges consume 101,472 bytes (48 bytes each). This is linear in group/range count,
bounded by legal Wii/H3 geometry, and independent of the roughly 4.4 GiB partition size.

The existing deterministic two-entry LRU cache remains capped at two full candidate buffers (4
MiB resident). Each cached entry now records its valid byte count. A partial entry is keyed by the
same logical group index, copies only within that valid size, and cannot expose zero-padding or
stale bytes from another full entry. Full/partial hits, misses, repeated reads, and eviction are
covered by the focused tests; `CopyReader` behavior is unchanged.

### Synthetic proof

The small oracle/independent-encoder fixture now supports two full groups plus a 52-cluster final
group. Its exact final `52 * 0x8000` encrypted bytes match the independently generated conventional
oracle, including H0/H1/H2/H3 and AES output. Boundary reads cover full-to-partial crossing, first
and last final-group bytes, partition end, the following defined disc range, reverse/repeated cache
access, and rejection of a nonexistent group. Normal DiscIO reports Wii, the invented ID, and
`IsNKit() == false`; it validates the last present block and complete H3 table, reads all known
files, and is accepted by unchanged `AnalyzeWbfs`. Unchanged `WriteWbfs` produces a temporary
synthetic WBFS which reopens with the same files; existing writer cancellation and staging cleanup
tests remain authoritative.

A second fixture represents 2,109 complete groups plus a 52-cluster final group without a 4 GiB
allocation or file. It stores about 20 MiB of invented compact data, including a source-backed file
larger than the old 16 MiB whole-payload cap, then describes thousands of reconstructed zero groups
with one gap and canonical repeated zero-group H3 entries. Planning performs no read larger than
the fixed 0x50000 NKit header and less than 4 MiB of total source reads before group prewarming. It
builds all 2,110 descriptors, reconstructs arbitrary middle/final groups into the bounded cache,
opens through DiscIO, reads the known file prefix, and passes `AnalyzeWbfs`.

Negative coverage rejects zero or non-cluster-aligned raw geometry, inconsistent inner sizes,
truncated compact input, retained-H3/TMD mismatch, and mutated final-group payload. Non-final
groups are full by construction and are revalidated by the index, while group counts exceeding
the H3/legal Wii bound, arithmetic overflow, exceptional hash/scrub flags, and gameplay-critical
recovery loss remain covered by the existing foundation/random-access matrix.

### Kirby read-only reassessment and exact O3 boundary

The authorized Kirby source still identifies as ID6 `RK5E01`, exact Wii `NKIT v01`, PLAIN,
single-layer, one retained data partition, playable with a synthetic non-game region, and requiring
external update data only for archival recovery. Its retained raw/decrypted sizes now validate as
2,109 complete groups plus one 52-cluster final group, and its 3,705,667,584-byte compact source no
longer hits the former 16 MiB planning limit.

Factory construction now advances to the next independent generic boundary:
`UnsupportedGapContext` at source offset `0x9a070c`. The compact FST begins at payload-relative
`0x928700` (source `0x9a0700`), has 2,964 entries, and entry 1 is a directory named `bggimmick`
with parent index 0 and subtree-end index 352. O2 deliberately retains N4's root-level-regular-file
restriction, so no reconstructed reader, DiscIO conventional view, `AnalyzeWbfs` result, or Ready
preview is produced for Kirby yet. No retail ISO/WBFS was written.

O3 should implement validated nested-directory FST traversal and the corresponding canonical
directory-aware file/gap ordering. It must preserve directory records while collecting and sorting
only regular-file compact data, patch each file offset correctly, add synthetic nested/subtree and
malformed-directory coverage, and then reassess Kirby read-only. Junk-file removal or any further
exceptional context should remain a separate later construct unless it is inseparable from the
canonical nested-directory behavior proven by that reassessment.

### O2 validation checkpoint

The isolated Ninja `RelWithDebInfo` configuration has tests and Qt enabled. O2 focused coverage is
4/4; O1 is 7/7; N5 is 6/6; N4 is 8/8; N3 is 10/10; N2 is 43/43; the complete affected Wii Export
family is 186/186; and representative Physical SD smoke is 3/3. The full Dolphin suite was not run
because the bounded affected matrix was green. Both aggregate tests and the production
`dolphin-emu` target are required to link with every build invocation limited to `-j2`.

## 21. O3 retail compatibility: nested-directory FST traversal

O2 stopped on Kirby's first non-root FST record: entry 1 at compact source `0x9a070c` is the
directory `bggimmick` (parent 0, exclusive subtree end 352), while the production planner required
every non-root record to be a regular file. That byte-type check returned
`UnsupportedGapContext` before any regular files could be planned. O3 replaces the root-files-only
scan with a validated iterative FST parser; it does not ignore, flatten, or synthesize directory
records.

### Validated directory model and canonical ordering

Dolphin's `FileSystemGCWii` implementation is the primary authority for the 12-byte Wii FST
record. The high byte of word 0 identifies a directory and its low 24 bits index the NUL-terminated
name table. For a regular file, words 1 and 2 are the word-scaled file offset and byte length. For
a directory, word 1 is the parent directory entry and word 2 is the exclusive entry index after
the directory's complete preorder subtree. Root is directory entry 0, has parent 0, and its end
field supplies the complete entry count. Empty directories therefore have end `index + 1`;
sibling and nested directories are delimited by their exclusive end indexes.

The pinned MIT NKit-v1 reference (`NKit/FilesAndStreams/FileSystem.cs`,
`NKit/Conversion/NkitFormat.cs`, and `NKit/Conversion/Readers/NkitReaderWii.cs`) confirms that
reconstruction traverses the FST to discover regular files, then orders those files by compact
physical offset and length. Directory entries consume no compact body bytes. The conventional FST
is preserved in place and only the offset word of each regular-file entry is patched. Thus FST
preorder may differ from compact/physical file order without changing the hierarchy.

`NKitV1FstEntry` is the immutable compact production descriptor: entry index/type, validated
parent and exclusive subtree end, name offset/length, directory depth, and regular-file compact
offset/size. `NKitV1SequentialPartition` retains the descriptors in original FST order plus
directory/file/depth statistics. Preview/execution recipe identity now hashes this complete
directory-aware representation.

Validation uses an active-directory index stack rather than recursion. It rejects an invalid root
type/parent/count, unknown entry type, parent mismatch or non-ancestor parent, non-forward or
out-of-parent subtree end, impossible nesting, entry/name-table arithmetic overflow, out-of-table
or unterminated/empty non-root names, truncated entry/string tables, and file offsets or extents
outside validated compact and reconstructed bounds. Only regular files enter the physical sort or
source-span sequence. Complexity is `O(n + f log f)` time and `O(n + f + depth)` descriptor
memory; traversal itself has no recursive stack or quadratic subtree walk.

O3 also verified one directory-aware gap boundary exposed immediately after the directory fix.
Kirby first advanced from `0x9a070c` to `UnexpectedEndOfInput` at `0x9fd460`. Metadata showed that
the preceding aligned regular file ends at compact offset `0x985460` relative to partition data
and the next file begins at exactly that offset. Canonical `NkitReaderWii.writeGap` returns zero
without reading a record when its calculated gap length is zero. Production now applies that
format-wide rule: adjacent aligned regular files consume no gap record and add no reconstructed
span. Nonzero gaps retain all existing bounded decoding, leading-null, junk-file, padding, and
malformed-record checks.

### Synthetic proof and malformed matrix

The independent small oracle contains the invented `RO3P01` hierarchy:

```text
/
|-- root.bin
|-- dir_a/
|   |-- alpha.bin
|   `-- dir_b/
|       `-- beta.bin
|-- dir_c/
|   `-- gamma.bin
`-- empty_dir/
```

Its FST traversal order differs deliberately from compact physical order
(`gamma`, `root`, `beta`, `alpha`), and `beta`/`alpha` are physically adjacent with no encoded gap.
The test-only encoder preserves directory records and independently compacts only regular files.
The reconstructed FST matches the conventional oracle byte-for-byte: root, parents, subtree ends,
name table, file sizes, and restored file offsets. Normal DiscIO exposes all directories including
the empty directory and reads the root, nested, and deepest file bytes exactly. Random/cache reads,
unchanged `AnalyzeWbfs`, unchanged `WriteWbfs`, WBFS reopen, nested file reads, cancellation, and
staging cleanup all pass.

A second generated fixture contains exactly 3,000 FST entries (2,996 directories including root,
four regular files, and depth 2) while reusing tiny generated bodies. Its final immutable entry
vector is 144,000 bytes (3,000 descriptors at 48 bytes); planning remains bounded by the existing
partition-prefix cap and does not allocate per-file bodies. The focused malformed matrix covers
root, parent, subtree, type, filename offset/termination, truncated table/string data, compact file
range/overlap, and nested-gap failures.

### Kirby read-only reassessment and next boundary

The authorized Kirby source validates with 2,964 FST entries: 160 directories, 2,804 regular
files, maximum directory depth 4, and largest observed directory subtree span 1,264 entries. The
production plan contains all 2,110 partition groups and 2,114 reconstructed index ranges. Reader
creation succeeds; normal DiscIO opens ID6 `RK5E01`, reports `IsNKit() == false`, and unchanged
`AnalyzeWbfs` returns success. N5 preview preparation classifies the source as supported and reaches
Ready. The source remained read-only and no retail ISO or WBFS was created.

O3 therefore reveals no independent O4 blocker for Kirby. The next action is the separately
controlled manual Kirby NKit-to-WBFS acceptance run after review. A future O4 should be opened only
from concrete evidence produced by that acceptance run or another authorized retail probe—for
example junk-file removal, exceptional scrub/hash state, or another partition/layout construct—and
must again implement the generic format rule rather than a title-specific exception.

### O3 validation checkpoint

The isolated Ninja `RelWithDebInfo` configuration has tests and Qt enabled. O3 focused coverage is
3/3; O2 is 4/4; O1 is 7/7; N5 is 6/6; N4 is 8/8; N3 is 10/10; and N2 is 43/43. The complete
affected NKit/Wii Export/WBFS matrix is 260/260 (the O2 checkpoint's 257 plus the three O3 tests),
and representative Physical SD smoke is 3/3. The full Dolphin suite was not run because the
bounded affected matrix is green. Aggregate tests and the production `dolphin-emu` executable
link successfully, with all compilation limited to `-j2` and final single-process link steps below
that ceiling. Kirby's before/after SHA-256, size, and nanosecond mtime match exactly.

## 22. O4 retail compatibility: late full-write hash validation

The first controlled retail export after O3 reached the native writer and failed at about 75%
with `WbfsWriteStatus::SourceReadFailed`; existing WBFS rollback removed every staging and final
file. The requested `/tmp/dolphin-rwin-kirby-diagnostic.log` did not contain the temporary NKit
messages (only three unrelated null-pixmap warnings), so the failure was reproduced without output
using a disposable read-only traversal linked to the exact committed O3 code.

That traversal identified WBFS logical block 2,233 (used-block ordinal 1,825), whose read starts at
disc offset `0x117200000`. Group 2,108 materializes successfully. When the same block crosses the
next index range at `0x117220000`, final partial group 2,109 (52 clusters) fails. Committed O3
returned numeric error 30, `SourceIdentityMismatch`, with error-offset field `0x83d` and partition
0. The field was carrying the group index, not a failed compact read address. The error originates
in `ReconstructWiiNKitV1PartitionGroup` after decrypted-span materialization and
`VolumeWii::HashGroup`: SHA-1 over the regenerated H2 table does not equal the retained group H3.
It is therefore not a source-file identity mutation, source-backed read failure, gap parse,
encryption failure, WBFS bug, or index/cache overrun.

The contributing final-group compact file data begins at absolute source offset `0xdcc6bd10` and
runs for `0x18ffe0` bytes. The canonical tail record is at `0xdcdfbcf0` and describes the final
`0x3020`-byte all-junk gap, including the normal 28-byte leading-null rule. The compact stream ends
exactly at `0xdce00000`. The retained expected H3 is
`3516fe41bb9b04d231b98e954741c671b25a0998`; the canonical regenerated H3 is
`a3b56d1c6005a84850e3ab0cf2c80a8438fb4f17`. All 264 bytes of group hash-preservation flags are
zero, including group 2,109. Independent reconstruction using the pinned MIT NKit-v1 junk and Wii
hash rules produced the same regenerated value, and alternative leading-null/zero-tail and unused
hash-slot variants did not produce the retained value.

### Classification and canonical boundary

This is a class-B retail construct: an **unpreserved reconstructed-hash/H3 mismatch**, not an O1,
O2, or O3 implementation regression. Neighboring group 2,108 uses the same final file and passes;
the final group has no exceptional-hash payload from which the retained hierarchy can be restored.
An H3 digest is not sufficient to recover the missing bytes or hash areas.

The pinned reference regenerates normal hashes for an unflagged group. Its `repairBlocks` result can
report that the regenerated group is inconsistent with retained H3, but the conversion loop does
not make that result fatal and can emit the regenerated group. Dolphin cannot copy that behavior
blindly: retaining H3 fails the conventional H3 chain, while replacing H3 also changes the TMD
content digest protected by the retail signature. The architecture has always required H3/TMD
consistency, so O4 does not fabricate zeros, ignore integrity, or rewrite signed metadata merely to
let the writer finish. Recovery or a separately proven playable repair policy is required before
this class can be exported.

### Generic production behavior

O4 adds the distinct typed error `HashHierarchyMismatch` (numeric 34) and a stable error-name
formatter; `SourceIdentityMismatch` remains reserved for actual source identity changes. The
random-access reader now retains the precise reconstructed logical offset at which a read failed.

`ValidateWiiNKitV1WbfsSourceReads` consumes an already successful `WbfsAnalysis` and materializes
every used logical WBFS block through the production reconstructed reader without creating output.
It returns the WBFS block, exact failing logical offset, reconstructed group when applicable, and
the original typed NKit error. It uses one fixed 2 MiB traversal buffer plus the reader's existing
two-group/4 MiB cache, is cancellation-aware, and scales with used blocks rather than allocating a
disc image.

The N5 native backend receives the concrete reconstructed-reader type for reconstructed recipes
and runs this validation during the existing indeterminate Preparing stage, before creating output
directories or invoking `WriteWbfs`. A failure logs the enum name/numeric code, logical offset,
WBFS block, group, error field, and partition. The user-facing hash case says that the image
contains partition hash data which cannot be reconstructed and explicitly confirms that no output
was created. Direct ISO/RVZ sources and direct compact-NKit rejection are unchanged. Preview is not
made to perform this full-disc traversal, so Kirby can still show Ready; execution now fails early
and precisely during Preparing rather than after a partial multi-gigabyte write.

### Synthetic and retail read-only proof

The synthetic late-failure fixture changes an invented final used group's retained H3 and updates
the synthetic TMD digest so planning and `AnalyzeWbfs` remain successful, while deliberately
providing no preservation flag/data capable of regenerating that H3. This reproduces committed
O3's later `WriteWbfs` `SourceReadFailed`. O4's full-used-block validator identifies the exact
late group before output; the integrated backend never invokes the writer and leaves no directory,
final file, or staging residue. A valid fixture traverses every used block, writes and reopens its
WBFS with exact files, and the validation cancellation path remains clean.

The final authorized Kirby read-only pass confirms 2,110 groups, 52 clusters in the final group,
2,114 index ranges, and 1,826 used WBFS blocks. Groups 2,107 and 2,108 reconstruct; group 2,109
still returns `HashHierarchyMismatch`. Unchanged `AnalyzeWbfs` succeeds, but the complete used-block
pass stops only at block 2,233 / logical offset `0x117220000`. This is the intended fail-closed
result: O4 diagnoses and prevents the unsafe export but does not claim to have recovered absent
hash-critical material. Kirby remains preview-Ready because preview is intentionally bounded; it
is not currently safe to convert.

### Next compatibility boundary

The next retail-compatibility investigation should determine whether authoritative recovery data
can supply the missing final-group hierarchy or whether a Wii-verified playable repair exists that
preserves the signed TMD trust chain. Until that is proven with synthetic integrity and boot-path
evidence, `HashHierarchyMismatch` remains blocked. The next manual acceptance check should use the
isolated O4 binary only to confirm that Kirby now stops during Preparing with the precise hash
diagnostic and creates no output; it should not retry a full conversion.

### O4 validation checkpoint

The isolated Ninja `RelWithDebInfo` configuration has tests and Qt enabled. O4 focused coverage is
3/3; O3 is 3/3; O2 is 4/4; O1 is 7/7; the current N5 slice is 7/7; N4 is 8/8; N3 is 10/10; and
N2 is 43/43. The complete affected NKit/Wii Export/WBFS matrix is 263/263, and representative
Physical SD smoke is 3/3. The full suite was not run because the bounded matrix was green. Both
`tests` and `dolphin-emu` link successfully, every build invocation used `-j2`, and Kirby's
before/after SHA-256, size, and nanosecond mtime match exactly.
