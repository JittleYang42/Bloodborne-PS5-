// bbport PS5: what this first console build leaves out.
//  - Online play (gpu/bbnet: shadNet, The Hunter's Dream): the runtime's offline network and
//    PSN answers serve the game, as on a PC with online play off.
//  - Video playback (AvPlayer's FFmpeg source): a movie's source does not open, so the game is
//    told the movie cannot play (the opening and the credits are skipped).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "core/libraries/avplayer/avplayer_source.h"

extern "C" {
void bbnet_configure(const char*, const char*, const char*, const char*) {}
void bbnet_initialize(void) {}
void bbnet_set_image(unsigned char*, uint64_t) {}
void bbnet_register(void) {}
int vulkan_smoke(void) {
    std::puts("PS5: --vulkan-only is not part of the console build");
    return 1;
}
}

namespace Libraries::AvPlayer {

AvPlayerSource::AvPlayerSource(AvPlayerStateCallback& state) : m_state(state) {}
AvPlayerSource::~AvPlayerSource() = default;
bool AvPlayerSource::Init(const AvPlayerInitData&, std::string_view path) {
    std::printf("PS5: movie %.*s skipped (no video decoder in this build)\n", int(path.size()), path.data());
    return false;
}
bool AvPlayerSource::FindStreams() { return false; }
s32 AvPlayerSource::GetStreamCount() { return 0; }
bool AvPlayerSource::GetStreamInfo(u32, AvPlayerStreamInfo&) { return false; }
bool AvPlayerSource::EnableStream(u32) { return false; }
void AvPlayerSource::SetLooping(bool) {}
std::optional<bool> AvPlayerSource::HasFrames(u32) { return false; }
bool AvPlayerSource::Start() { return false; }
bool AvPlayerSource::Stop() { return true; }
void AvPlayerSource::Pause() {}
void AvPlayerSource::Resume() {}
bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo&) { return false; }
bool AvPlayerSource::GetVideoData(AvPlayerFrameInfo&) { return false; }
bool AvPlayerSource::GetVideoData(AvPlayerFrameInfoEx&) { return false; }
u64 AvPlayerSource::CurrentTime() { return 0; }
bool AvPlayerSource::IsActive() { return false; }

} // namespace Libraries::AvPlayer
