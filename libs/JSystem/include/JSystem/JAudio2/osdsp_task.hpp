#pragma once

#include "JSystem/JAudio2/dspproc.hpp"
#include <revolution/dsp.h>

extern DSPTaskInfo* DSP_prior_task;

void DsyncFrame2(u32 param_0, JASDspAddr param_1, JASDspAddr param_2);
void DsyncFrame3(u32 param_0, JASDspAddr param_1, JASDspAddr param_2, JASDspAddr param_3, JASDspAddr param_4);
