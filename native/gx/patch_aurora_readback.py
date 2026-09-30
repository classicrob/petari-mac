#!/usr/bin/env python3
"""Add mid-frame EFB submissions while preserving Aurora's mapped staging lifetime."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
here = Path(__file__).resolve().parent
if source.name == 'recording.cpp':
    text = '#include "efb_snapshot.hpp"\n#include <petari/host_allocation.hpp>\n#include "../dolphin/vi/vi_internal.hpp"\n' + text
    text += '\n' + (here / 'efb_recording.inc').read_text()
elif source.name == 'frame.cpp':
    # The recorder's staging slot per frame packet (efb_frame.inc): EFB segments
    # switch it without waiting for the render worker, whose view is
    # frame.stagingBuffer. end_frame unmaps the recorder's final slot.
    for old, new in (
        ('  frame.stagingBuffer = *stagingSlot;\n',
         '  frame.stagingBuffer = *stagingSlot;\n  petariRecordingSlots[frameSlot] = *stagingSlot;\n'),
        ('  const size_t stagingSlot = frame.stagingBuffer;\n',
         '  const size_t stagingSlot = petariRecordingSlots[frameSlot];\n'),
    ):
        if text.count(old) != 1:
            raise SystemExit(f'Aurora readback patch anchor mismatch: {old.strip()}')
        text = text.replace(old, new)
    text = ('#include "efb_snapshot.hpp"\n#include <cstdlib>\n#include <future>\n'
            '#include <cstddef>\n'
            'static constexpr std::size_t petariRecordingSlotCount = 8;\n'
            'static std::size_t petariRecordingSlots[petariRecordingSlotCount];\n' + text)
    text += '\n' + (here / 'efb_frame.inc').read_text()
else:
    raise SystemExit(f'Unsupported Aurora readback patch source: {source}')
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)
