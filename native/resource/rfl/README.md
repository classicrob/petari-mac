# Native RVLFaceLib (Miis)

SMG uses RVLFaceLib (RFL, `src/RVLFaceLib`) only on the file-select screen:

- Mii icons and names: `MiiSelect`, `FileSelectFunc`, `MiiFaceIcon`.
- A 3D face on each file item: `FileSelectItem::createMii`. It is built from
  the built-in default Mii when the file uses no Mii.

`MiiFacePartsHolder` loads `RFL_Res.dat` from the disc
(`ObjectData/MiiFaceDatabase.arc`) and passes it to `RFLInitResAsync`, so RFL
never reads the resource from NAND. The only console file is the Mii Channel
database `/shared2/menu/FaceLib/RFL_DB.dat`. When it is absent, RFL boots with
no Miis: `bootloadDBopencallback_` receives `NAND_RESULT_NOEXISTS` and records
`DBNotFound` with success. That is the original no-Mii behaviour.

## Byte order and layout (this directory)

`rfl_native.h` / `rfl_native.cpp` (`petari_rfl_native`), used by the RFL
sources under `PETARI_NATIVE`. Wii builds are unchanged: each change keeps the
original code in `#else`.

- **Mii records.** `RFLiCharData` (0x4A bytes) and `RFLiHiddenCharData` (0x40)
  are big-endian 16-bit words of bitfields. CodeWarrior allocates a word's
  first field at the most significant bit; clang on arm64 at the least.
  - The codec converts a stored record to the host struct and back, bit for
    bit, including padding.
  - Names are UTF-16BE when stored.
  - `RFLiGetDefaultData` decodes the six built-in records through it.
- **Create IDs.** They stay bytes. `RFLiIsValidID`, `RFLiIsSpecialID`,
  `RFLiIsTemporaryID`, `RFLiIsSameID`, and `RFLiSetTemporaryID` treat the
  first word as big-endian. That word holds the special and temporary mask
  bits. The IDs are also not 4-byte aligned inside `RFLiCharInfo`, which UBSan
  reported.
- **RFL_Res.dat.**
  - `RFLInitResAsync` validates the whole image before use and returns
    `RFLErrcode_Fatal` with an `OSReport` if it is damaged.
  - The loader reads the archive and file tables big-endian.
  - `RFLiLoadTexture` and `RFLiLoadShpTexture` convert the copied
    `RFLiTexture` header to host order. Texels stay in GX order.
  - `RFLiInitShapeRes` converts its copy of a shape to host order: the
    Faceline transforms, the counts, and the s16 positions, normals, and
    texture coordinates. RFL's own parser then runs unchanged. Primitive
    indices are bytes.
- **Validation.** It checks everything RFL later trusts:
  - 18 archives, in order, inside the file; monotonic file tables; the stored
    largest-file sizes;
  - textures: format, no palette, dimensions, and GX-tiled image size;
  - shapes: the per-part tag (RFL's `csHeader`), array bounds, GX primitive
    opcodes, and every vertex index against its array.

## Tests

`native/tests/rfl_tests.cpp` (`ctest -R native_rfl`: 4,153 checks, plus
479 more with `native_rfl_assets`, which runs when the extracted disc is under
`build/game-data`) covers:

- **Bit positions.** All 52 `RFLiCharData` bitfields, set by name and compared
  with bit positions transcribed from the header, in both directions. Also
  hidden records.
- **Round trips.** 2,000 random stored records, bit exact. Names are stored
  UTF-16BE; bytes are copied through.
- **Default Miis.**
  - All six pass RFL's own `RFLiCheckValidInfo` after decoding.
  - Record 0 matches a hand decode of its bytes.
  - Reading the stored bytes in place, as an unported RFL would, gives
    different fields.
- **Create IDs.** Validity, special, temporary, equality, and
  `RFLiSetTemporaryID`, at an unaligned address.
- **Synthetic resource.** A valid image, then 12 kinds of damage rejected with
  specific messages. Also host-order conversion of texture headers and shapes.
- **Disc.** `RFL_Res.dat` extracted from the real `MiiFaceDatabase.arc`
  (686,372 bytes, version 0x039D) validates. All 211 textures and 261 shapes
  convert, including empty parts. A damaged copy is rejected.

The suite is clean under ASan+UBSan, standalone and in the root build.
Mutation checks: removing the default-record decode fails RFL's own validity
check, host-order Create ID words fail the ID tests, and removing the index
check fails the synthetic rejection test.

Standalone:

```sh
cmake -S native/resource/rfl -B build/rfl -DPETARI_SANITIZERS=ON
cmake --build build/rfl && ctest --test-dir build/rfl
```

## The full library (`petari_rfl`)

All 14 RFL sources are compiled as C, as on the Wii. The library links
`petari_rfl_native`, `petari_sdk_mem` (the SDK expanded heap),
`petari_platform_nand` (async NAND), `petari_platform_os`, and `petari_core`.
Native changes beyond byte order:

- **Public opaque structs.** `RFLCharModel` (game-allocated in `MiiFaceParts`)
  and `RFLMiddleDB` are sized to hold the internal structs, which contain host
  pointers (168 and 40 bytes). The Wii sizes (0x88, 0x18) let
  `RFLInitMiddleDB` overwrite the manager's `working` flag, and RFL then
  deadlocked on its first boot. `rfl_native.cpp` asserts the sizes.
- **Work buffer.** The Wii's system heap (0x24800) fit its nine fixed
  allocations with 368 bytes to spare. Native MEM block headers are larger, and
  the second 8 KiB NAND safe buffer did not fit. The system heap and
  `RFLGetWorkSize()` grow by `RFL_NATIVE_HEAP_OVERHEAD` (0x400):
  `RFLGetWorkSize(FALSE)` is 316,624 bytes. After boot the system heap has 936
  bytes free and the temporary heap 148,864.
- **Async status.** Natively, NAND and alarm callbacks run on platform host
  threads while holding the interrupt lock, in parallel with the game thread.
  On the Wii they interrupted the one CPU. `RFLGetAsyncStatus` and
  `RFLGetLastReason` therefore read under that lock, so a poll that reports
  completion also sees everything the callback wrote. Without it, TSan found
  the races, and a poll could see "done" before the database state (arm64
  memory ordering).
- **A console `RFL_DB.dat` is refused explicitly.** If a Mii Channel database
  exists in the NAND root, RFL reports it through `OSReport` and handles it as
  it handles a database that fails its CRC check (`DBBroken`, no official
  Miis). The file is left untouched. Converting it is not implemented yet: the
  records and the CRC over stored bytes.
- **Linking.**
  - The header inline helpers are `static inline` in native C, since C99
    `inline` emits no symbol.
  - `RFL_MakeRandomFace.c` defines the `LENGTHOF` it uses; no header ever did.
- **Miis on a remote.** Native input's `WPADReadFaceData` refuses the read
  (`WPAD_ERR_INVALID`): a keyboard remote has no Mii memory. RFL reports
  `RFLErrcode_Controllerfail`. SMG never loads controller Miis.
- **Vertex arrays.** All 37 `GXSETARRAY` calls in `RFL_Model.c` pass
  little-endian (host) arrays, and every array they name is filled from
  shapes converted by `RFLiNativeShapeToHost`. The display list is recorded
  through GX calls (`GXPosition1x8` ...), not copied from the resource, so its
  FIFO bytes are whatever the GX bridge records.

## Boot tests

`native/tests/rfl_boot_tests.cpp` (`native_rfl_boot`, 47 checks; and
`native_rfl_boot_assets`, 49, which mounts the disc first as the application does, so `NANDInit` gets the title's home directory from the disc ID) boots RFL as
`MiiFacePartsHolder` does: a `RFLGetWorkSize(FALSE)` buffer,
`RFLInitResAsync`, and polling once per 16 ms. It uses a temporary native NAND
root. The runs:

- **No `RFL_DB.dat`.**
  - Boot succeeds with `DBNotFound` and nothing else broken.
  - No official Miis: none available, a saved Mii ID is not found, and no
    names.
  - The built-in default Mii 0 is readable ("no name"), as needed for
    `FileSelectItem`'s face.
  - Every fixed allocation succeeds.
  - Booting creates no database.
- **A console database present.** It is refused as described above, with no
  official Miis, and the file is unchanged byte for byte.
- **A damaged `RFL_Res.dat`.** `RFLInitResAsync` returns Fatal before
  starting. A null resource is Fatal, as on the Wii.
- **A second boot after `RFLExit`,** as each file-select scene does.

The boot tests pass under ASan+UBSan (both tests; 90 of 91 runs with the disc
resource) and TSan (6 runs, no reports). The one failed ASan run was not
captured and did not recur in 80 further runs.

## Render test (GPU)

`native/tests/rfl_render_tests.cpp` (the SDK side) and `rfl_render_aurora.cpp`
(the Aurora side) have separate headers; `rfl_render_aurora.h` carries only
plain C types. The target is `petari_rfl_render_tests`, from
`native/resource/rfl/render/CMakeLists.txt`, which root adds after
`native/gx`. The ctest `native_rfl_render` is labelled `gpu`. The test starts
like `petari_gx_probe`: Aurora with Metal, VI (`OSInit`, with the game
arenas), then JKR heaps and `GXInit`. It then:

1. Boots RFL from the disc's `RFL_Res.dat` with no Mii database. RFL's work
   buffer, the resource, the `RFLCharModel`, the model buffer, and the icon
   buffer are all in a JKR scene heap, as in `MiiFacePartsHolder`.
2. Calls `RFLInitCharModel` for the default Mii at 256 resolution. This renders
   the face texture through the EFB and `GXCopyTex`.
3. Draws the model with `RFLiMakeIcon`'s camera, light, and passes, then
   captures the EFB. Checks:
   - the corner is still background;
   - at least 20,000 face pixels;
   - the center is not background and is skin-toned (red over blue).
4. Calls `RFLMakeIcon` (128x128, direct red background), then draws a quad
   textured from the icon buffer. Aurora resolves `GXCopyTex` destinations to
   GPU textures. It captures the EFB and checks:
   - the icon corner is RFL's copy-clear red, which proves the copy reached
     the texture;
   - the icon center is the face;
   - outside the quad is background.
5. Checks the scene heap is unchanged by rendering.

It mounts the disc before `NANDInit`, as the application does. On Metal it
passed 32 checks in root's run, with no home-directory warning:

- model: 65,643 face pixels, center `ff985030` on background `ff0c1220`;
- icon: 16,384 pixels, red corner `ffdc1e1e`, skin center `ff985030`.

## Still open

- **Rendering (root's GX bridge).**
  - Faces use `GXBeginDisplayList`, `GXPosition1x8`, `GXNormal1x8`,
    `GXTexCoord1x8`, `GXEndDisplayList`, and `GXCallDisplayList`.
  - Icons (`RFLMakeIcon`) render to the EFB, then use `GXCopyTex` and
    `GXDrawDone`. They also use `GXGetScissor`, `GXGetViewportv`, and
    `GXPixModeSync`.
  - Textures are GX-order RGB5A3, IA4, and I4.
  - `RFLInitCharModel` and `RFLMakeIcon` are not exercised by these tests,
    because they need GX.
- **Converting a console `RFL_DB.dat`:** the records, the hidden database,
  and the CRC over stored bytes.
- **Two functions the decompilation does not define:** `RFLiFormatAsync`
  (hidden-database formatting) and `RFLiIsSameFaceCore` (random
  middle-database selection). SMG reaches neither. Link with `-dead_strip`,
  as the tests do, until they are implemented.
- **`RFL_MiddleDatabase.c`** still reads some fields as host-order `u32`. Only
  the controller path initialises a middle database, and SMG does not use it.
