// Native (Mach-O arm64) counterpart of BCSVInclude.s: embeds the BCSV tables that the
// Wii build links into main.dol .rodata. Assemble with the repository root as an include
// directory (-I <repo>) so the .incbin paths resolve. Tables stay big-endian; JMapInfo
// decodes them on read.

    .section __TEXT,__const

    .globl _StoryEventBCSV
    .p2align 5
_StoryEventBCSV:
    .incbin "src/Game/System/StoryEvent.bcsv"

    .globl _GalaxyIDBCSV
    .p2align 5
_GalaxyIDBCSV:
    .incbin "src/Game/System/GalaxyID.bcsv"
