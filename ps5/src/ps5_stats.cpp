// bbport PS5: the GPU library's counters for the heartbeat (ps5/src/ps5_main.c).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "bbport_toggles.h"

extern "C" int bb_ps5_gpu_stats(char* out, unsigned long size) {
    return std::snprintf(out, size,
                         "gpu: submissions=%llu draws=%llu dispatches=%llu frames=%llu images=%llu "
                         "uploads(img %llu MiB, buf %llu MiB) tracker_faults=%llu signal_faults=%llu",
                         (unsigned long long)BbStats::submissions.load(),
                         (unsigned long long)BbStats::draws.load(),
                         (unsigned long long)BbStats::dispatches.load(),
                         (unsigned long long)BbStats::gpu_frames.load(),
                         (unsigned long long)BbStats::images_registered.load(),
                         (unsigned long long)(BbStats::image_upload_bytes.load() >> 20),
                         (unsigned long long)(BbStats::buffer_upload_bytes.load() >> 20),
                         (unsigned long long)BbStats::tracker_faults.load(),
                         (unsigned long long)BbStats::gpu_signal_faults.load());
}
