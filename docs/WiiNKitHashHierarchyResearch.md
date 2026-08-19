# Wii NKit hash-hierarchy recovery research

This document records O5's source-based investigation of an unpreserved Wii NKit v1 hash
hierarchy mismatch. It is a design record, not authorization to relax the production
`HashHierarchyMismatch` block.

## Scope and evidence

The retail probe is the authorized, read-only `RK5E01` NKit v1 source. O4 found that final
partition group 2,109 has 52 physical clusters and no hash-preservation flag. Reconstruction of
the compact payload and canonical tail gap produces H3
`a3b56d1c6005a84850e3ab0cf2c80a8438fb4f17`, while the retained H3 entry is
`3516fe41bb9b04d231b98e954741c671b25a0998`. Groups 2,107 and 2,108 reconstruct and validate.

Primary implementation evidence used here:

- Dolphin `VolumeWii::CheckBlockIntegrity`, `CheckH3TableIntegrity`, `HashGroup`, and
  `EncryptGroup` in `Source/Core/DiscIO/VolumeWii.cpp`; `HashBlock` in `VolumeWii.h`;
- Dolphin `SignedBlobReader::GetSha1` and `ESCore::VerifyContainer` in
  `Source/Core/Core/IOS/ES/Formats.cpp` and `ES.cpp`, and the deliberately permissive disc
  `ESDevice::DIVerify` path in `ES.cpp`;
- pinned MIT NKit v1 commit `61dd683b4b70273a37c4513726b87943f2e32e37`, especially
  `WiiHashStore`, `WiiPartitionGroupSection::PreserveHashes`,
  `WiiPartitionGroupEncryptionState`, `NkitWriterWii`, `NkitReaderWii::repairBlocks`, and
  `RecoverReaderWii`;
- USB Loader GX commit `e25c4f3501ed957b7db73f79c51fdf00715ab2e2`, especially
  `GameBooter::BootPartition` and `WDVD_OpenPartition`/`WDVD_Read`;
- d2x cIOS commit `33ad1eeeb8f562df99e7d7ca428fdd36e0e31be7`, especially the DIP
  plug-in's emulated backing reads and `mload-module/patches.c::__Patch_EsModule`;
- Dolphin's WIA/RVZ format documentation and WiiBrew's reverse-engineered Wii disc and `/dev/di`
  documentation. WiiBrew is not official Nintendo documentation.

Repository references:

- Dolphin: `https://github.com/dolphin-emu/dolphin` and `docs/WiaAndRvz.md`;
- pinned NKit v1: `https://github.com/Nanook/NKitv1`, commit
  `61dd683b4b70273a37c4513726b87943f2e32e37`;
- current NKit publication: `https://github.com/Nanook/NKit`, commit
  `b4cdce0cc9e6fba59599aa0cbea0aa3ee245760c`;
- USB Loader GX: `https://github.com/wiidev/usbloadergx`, commit
  `e25c4f3501ed957b7db73f79c51fdf00715ab2e2`;
- d2x cIOS: `https://github.com/wiidev/d2x-cios`, commit
  `33ad1eeeb8f562df99e7d7ca428fdd36e0e31be7`;
- reverse-engineered specifications: `https://wiibrew.org/wiki/Wii_Disc` and
  `https://wiibrew.org/wiki//dev/di`.

The published current NKit repository was also checked at
`b4cdce0cc9e6fba59599aa0cbea0aa3ee245760c`. It contains documentation and release material, but
not the current NKit2 implementation source needed for an equivalent source audit. No conclusion
below depends on undocumented NKit2 behavior.

## The integrity and authenticity chains

For one encrypted 0x8000-byte Wii cluster, Dolphin's authoritative implementation establishes:

```text
31 x 0x400-byte decrypted payload slices
  -> 31 H0 SHA-1 digests (0x26c bytes)
  -> one H1 digest for that cluster

8 clusters' H1 digests
  -> one H2 digest

8 H2 digests (one 64-cluster group)
  -> one H3 digest in the partition's 0x18000-byte H3 table

SHA-1(the complete 0x18000-byte H3 table)
  -> the sole Wii-disc partition content record's TMD SHA-1 field

SHA-1(TMD signed body, beginning at the issuer)
  -> value authenticated by the TMD's Nintendo RSA signature/certificate chain
```

Each cluster's encrypted 0x400-byte header stores all 31 H0 values, all eight H1 values for its
eight-cluster subgroup, and all eight H2 values for its 64-cluster group. The H1 table is mirrored
within each subgroup and H2 is mirrored in every cluster header. H3 is stored outside partition
data in the partition H3 table. A final partial group hashes zero-filled non-present slots when
constructing its complete group hierarchy, but exposes/encrypts only its physically present
clusters.

These properties have different meanings:

- **data integrity**: H0 through H3 and the TMD content digest agree with the reconstructed bytes;
- **authenticity**: the TMD signed body is authenticated by Nintendo's RSA signature chain;
- **archival identity**: every reconstructed byte and digest matches the original mastered disc;
- **playability**: the target runtime accepts and can read the result.

A result can be internally self-consistent and playable under a permissive cIOS without being
Nintendo-authentic or archival-identical.

## Verification behavior

### Dolphin

`VolumeWii::CheckBlockIntegrity` performs the complete H0/H1/H2/H3 check described above.
`CheckH3TableIntegrity` hashes the complete H3 table and compares it with the sole TMD content
record. `ESCore::VerifyContainer` performs real RSA verification when invoked.

Ordinary `VolumeWii::Read` decrypts data but does not invoke either integrity method. Consequently,
a filesystem read alone is not integrity proof. During Dolphin's emulated disc boot,
`Boot_BS2Emu` calls `ESDevice::DIVerify`; that function validates basic TMD/ticket structure and
title identity but intentionally skips their cryptographic verification so custom/patched games
continue to work. Therefore the O5 synthetic experiment establishes that a fully regenerated
hierarchy and TMD content digest is readable and internally valid in DiscIO, and Dolphin's HLE boot
path is designed to tolerate the now-invalid retail signature. It does not constitute a full
synthetic retail-program boot test.

### Stock IOS and `/dev/di`

The reverse-engineered `/dev/di` contract describes partition opening as authenticating the TMD
and its H3-table commitment, followed by per-read verification of the cluster hierarchy. Thus a
retained mismatching H3 fails an accessed block; a regenerated H3 table with an unchanged TMD
fails the partition content commitment; and a changed TMD cannot pass an unmodified stock IOS RSA
check without Nintendo's private signing key. Historical Trucha signature behavior is IOS-version
dependent and is not a portable production target.

### USB Loader GX and d2x

USB Loader GX's `GameBooter::BootPartition` finds the data partition, calls
`WDVD_OpenPartition`, then runs the apploader. `WDVD_OpenPartition` and `WDVD_Read` issue normal
`/dev/di` requests; the loader does not implement a second H0-H3 or TMD verifier and does not make
inconsistent metadata safe by itself.

d2x's DIP plug-in supplies raw disc bytes from WBFS/fragment/DVD backings to the base IOS DIP
command path, which retains partition crypto/hash processing. Its ES patch explicitly overwrites
two signature-check sites. The source therefore supports this expectation for the common
USB Loader GX+d2x target: a regenerated, internally consistent H0-H3 chain and matching TMD content
digest should pass the data-integrity path, while the cIOS ES patch accepts the modified TMD's
invalid retail signature. This remains an **expected real-hardware result**, not a result proven by
O5. Other cIOS builds/configurations are not covered automatically.

## Canonical NKit v1 behavior and information loss

The pinned NKit v1 writer preserves the full 0x400 hash header for every present cluster in a
flagged group. `WiiPartitionGroupSection::PreserveHashes` flags mixed scrub states, file-overlapping
scrub, heterogeneous all-scrubbed data, or any group for which `FastHashIsValid` is false.
`WiiHashStore` serializes a per-group bit plus the corresponding 0x400 headers. Therefore a zero
flag means the compact file contains no exceptional lower-level hash bytes for that group.

`NkitReaderWii` regenerates hashes for an unflagged group. `repairBlocks` returns the result of
`IsValid(true)`, whose recalculation compares SHA-1(H2) with retained H3, but the main conversion
loop does not make that Boolean fatal. It writes the regenerated encrypted clusters while leaving
the retained H3 table and TMD in place; final CRC/match reporting can label the conversion invalid.
That is a permissive conversion artifact, not a self-consistent repair.

`RecoverReaderWii` is different: it applies externally supplied junk patches and update recovery
material in pursuit of the original image, counts H3 errors, and warns that unresolved errors make
the result corrupt. Neither path regenerates the H3 table or changes the TMD content digest.

For Kirby group 2,109, all 264 flag bytes are zero. The group contains a compact source-backed
file tail beginning around `0xdcc6bd10` for `0x18ffe0` bytes and a tail gap record at
`0xdcdfbcf0`; the canonical decoded gap is `0x3020` bytes of deterministic junk with 28 leading
zero bytes. Independent NKit-v1 reconstruction gives the same regenerated H3 as Dolphin. Tested
tail/padding variants do not give the retained value.

The neighboring comparison is:

| Group | Geometry and structure | Retained/regenerated result |
|---|---|---|
| 2,107 | full 64 clusters, unflagged | retained H3 `4d14a5855a3ea2d877a9c56df6460b3a293274f7`; canonical regeneration matches |
| 2,108 | full 64 clusters, unflagged, contains the same terminal file continued by 2,109 | retained H3 `48ed1bd2ac3f4073fb4be84f00ee9d2ffc4e89df`; canonical regeneration matches |
| 2,109 | final 52 clusters, unflagged, terminal file tail plus decoded final junk/slack | retained H3 `3516fe41bb9b04d231b98e954741c671b25a0998`; canonical regeneration is `a3b56d1c6005a84850e3ab0cf2c80a8438fb4f17` |

The retained 160-bit H3 is only SHA-1 of the missing 160-byte H2 table. It does not encode the H2
preimage, the 64 H1 tables, the 64 x 31 H0 values, or the payload bytes that created them. Reversing
SHA-1 to obtain those missing values is a preimage problem, and even a hypothetical preimage would
not identify the unique original payload. Image CRCs and record CRCs are checks, not sufficient
recovery data. The removed update partition is unrelated to this retained data-partition group.
Archival recovery would require the original differing payload/junk bytes, preserved 0x400 hash
headers, or an equivalent recovery patch/full group from an external database.

Because the NKit writer should have preserved a hierarchy that was already anomalous in its input,
the zero flag makes deliberately anomalous original hash bytes an unlikely explanation. Its
validity test is applied before compacted junk/slack is later regenerated, however, so it cannot
guarantee that discarded bytes will be reproduced exactly. The strongest evidence-based diagnosis
is original final-group slack/junk that the canonical generator does not reproduce (the kind of
difference represented by NKit recovery `JunkPatches`), or a related NKit v1 compact/rebuild defect.
The exact original byte difference cannot be identified from the compact source and retained
digest alone.

## Policy analysis

| Policy | Integrity and authenticity | Target behavior | O5 result |
|---|---|---|---|
| A. Regenerated data/H0-H2, retained H3/TMD | H3 table still matches its signed TMD, but the accessed group's H2 does not match H3 | Dolphin ordinary reads may appear fine; integrity verifier and IOS cluster read fail | Reject |
| B. Regenerated H0-H3, retained TMD | Per-cluster/group chain is consistent; complete H3 table no longer matches TMD | Dolphin ordinary reads may appear fine; H3-table verifier and IOS partition authentication fail | Reject |
| C. Regenerated H0-H3 and TMD content digest | Entire data-integrity chain is consistent; changed TMD signed body invalidates retail RSA signature | Dolphin HLE is compatible; stock IOS rejects; USB Loader GX+d2x is expected compatible because d2x patches ES signatures | Candidate for a narrowly scoped O6 plus hardware acceptance |
| D. Modified/fakesigned TMD | A real Nintendo signature cannot be produced; Trucha-style forgery is version-dependent | Unnecessary for d2x and not portable to stock IOS | Do not use as generic policy |
| E. External recovery | Can restore original hierarchy and Nintendo-authentic metadata if the original missing bytes/hash headers are supplied | Archival-correct path | Required for archival identity |
| F. Preserve O4 fail-closed | Avoids emitting a hierarchy known to be inconsistent | Safe current production behavior | Keep until O6 is implemented and accepted |

Policy C is not an integrity bypass: it constructs a complete consistent integrity chain. Its cost
is authenticity—the modified TMD no longer has Nintendo's signature—and archival identity. The
feature therefore must be explicitly limited to playable WBFS output for a proven permissive
cIOS target, must retain truthful archival/recovery status, and must never be represented as a
restored original disc.

## Synthetic laboratory experiment

`NKitV1O5Research.HashPolicyVariantsSeparateDiscIntegrityFromOrdinaryReads` uses only the existing
invented synthetic disc. It changes one invented decrypted payload byte and constructs three
variants independently of production NKit reconstruction policy:

1. retained H3/TMD;
2. regenerated H3 with retained TMD;
3. regenerated H3 plus SHA-1 of the complete H3 table written to the TMD content record.

Normal DiscIO opens all three, reads the known file, and reads the deliberately changed byte from
the affected decrypted cluster, proving that ordinary reads do not validate hashes. Variant 1
fails `CheckBlockIntegrity` but passes `CheckH3TableIntegrity`. Variant 2 passes block integrity
but fails H3-table integrity. Variant 3 passes both. Its TMD signed-body SHA-1 differs from the
original while its signature bytes remain unchanged, directly demonstrating the authenticity
consequence. No permissive behavior was added to production code.

## O5 decision and O6 boundary

O5 recommends **O6B: scoped playable hierarchy and TMD-digest regeneration for USB Loader GX with
d2x**, followed by a controlled real-hardware acceptance gate. The label is O6B rather than an
unqualified O6A because changing the TMD's signed body is essential; this is not hierarchy-only
regeneration.

O6 should:

1. retain O4's default fail-closed behavior;
2. add an explicit playable-WBFS repair policy only for an unpreserved hierarchy mismatch whose
   decrypted group bytes are otherwise fully reconstructable;
3. regenerate H0/H1/H2, the affected H3 entries, and the complete-H3-table TMD content digest;
4. preserve and report invalidated retail-signature and archival-recovery status;
5. prove every used block, H3-table integrity, Dolphin DiscIO, WBFS reopen, and synthetic files;
6. require one controlled USB Loader GX+d2x real-hardware test before calling the policy supported.

Because the H3 table and TMD are read before arbitrary partition groups, O6 must derive a stable
repaired H3/TMD view during bounded execution preflight (collecting every used group's regenerated
H3) and then create an immutable output reader. It must not mutate header metadata opportunistically
as random reads discover mismatches.

O5 cannot prove stock-IOS acceptance (the source evidence predicts rejection), actual USB Loader
GX+d2x hardware playability, behavior of every cIOS variant, or a full synthetic retail-program
boot. Those are explicit acceptance boundaries, not assumptions.
