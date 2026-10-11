#include "monitor/model.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace vulkanscope::monitor;

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void android16Names() {
    const std::string name = "SurfaceView[com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity](BLAST)#279";
    const std::string wrapper = "RequestedLayerState{" + name + " parentId=278 z=-2}";
    auto names = Surfaces(wrapper + "\n", "com.tencent.tmgp.sgame", "");
    require(names.size() == 1 && names.front() == name,
            "Android 16 list wrappers must become exact latency names while retaining sequence IDs");
    require(Surfaces(wrapper, "com.tencent.tmgp.sgame", wrapper) == names &&
                Surfaces(wrapper, "com.tencent.tmgp.sgame", name) == names &&
                Surfaces(name, "com.tencent.tmgp.sgame", wrapper) == names,
            "manual overrides accept both copied Android 16 list records and normalized names");
    require(Surfaces(wrapper + "\r\n" + name + "\n", "com.tencent.tmgp.sgame", "") == names,
            "deduplicate normalized and legacy names and accept CRLF records");
    const std::string unusual = "SurfaceView[com.game.app/.Main] name parentId=12 #not-an-id (BLAST)#501";
    names = Surfaces("RequestedLayerState{" + unusual +
                         " parentId=50 relativeParentId=51 mirrorId={52,53,} !handle z=-1 layerStack=2}\n",
                     "com.game.app", "");
    require(names.size() == 1 && names.front() == unusual,
            "preserve spaces and property-like text in names while stripping all AOSP debug properties");
    require(Surfaces("RequestedLayerState{com.game.app/.Main#42}", "com.game.app", "") ==
                std::vector<std::string>{"com.game.app/.Main#42"},
            "Android 16 wrappers without optional attributes are valid");
    for (const auto &malformed : {
             "RequestedLayerState{com.game.app/.Main}",
             "RequestedLayerState{com.game.app/.Main#42",
             "RequestedLayerState{com.game.app/.Main#42 parentId=}",
             "RequestedLayerState{com.game.app/.Main#42 z=-}",
             "RequestedLayerState{com.game.app/.Main#42 mirrorId={1,2}}",
             "RequestedLayerState{com.game.app/.Main#42 mirrorId={}}",
             "RequestedLayerState{com.game.app/.Main#42 futureField=1}"}) {
        require(Surfaces(malformed, "com.game.app", "").empty(),
                "malformed or unknown debug wrappers must not become guessed latency identities");
        require(Surfaces("com.game.app/.Main#42", "com.game.app", malformed).empty(),
                "malformed manual wrappers must not silently fall back to automatic selection");
    }
}

void candidateIdentity() {
    const std::string parent = "SurfaceView[com.game.app/.Main]#10";
    const std::string buffer = "SurfaceView[com.game.app/.Main](BLAST)#11";
    const auto names = Surfaces("com.game.app.clone/.Main#1\n"
                                "other.com.game.app/.Main#2\n"
                                "com.game.app_2/.Main#3\n"
                                "com.game.application/.Main#4\n"
                                "dev.snowflake.vulkan/com.game.app#5\n" +
                                    parent + "\n" + buffer + "\n" + parent + "\n",
                                "com.game.app", "");
    require(names == std::vector<std::string>{buffer, parent},
            "enforce package boundaries, exclude the monitor, deduplicate, and prioritize BLAST buffers");
    require(Surfaces(parent + "\n" + buffer, "com.game.app", parent) ==
                std::vector<std::string>{parent},
            "manual selection uses an exact layer identity even when another layer ranks higher");
    require(Surfaces("com.other.app/.Main#1", "com.game.app", "com.other.app/.Main#1").empty(),
            "manual selection cannot escape the current game package");
    require(Surfaces("RequestedLayerState{com.other.app/.Main#1 parentId=2}", "com.game.app",
                     "RequestedLayerState{com.other.app/.Main#1 parentId=2}").empty(),
            "normalized manual selection cannot escape the current game package");
    require(Surfaces(parent, "", "").empty(), "an empty package cannot select arbitrary system layers");
}

void candidateBounds() {
    std::string list;
    for (int i = 0; i < 10; ++i)
        list += "SurfaceView[com.game.app/.Main](BLAST)#" + std::to_string(i) + "\n";
    auto names = Surfaces(list, "com.game.app", "");
    require(names.size() == 10 && names[5].find("#5") != std::string::npos,
            "preserve candidates beyond the old four-layer cutoff for rotating collection");
    list.clear();
    for (std::size_t i = 0; i < MaxSurfaceCandidates + 20; ++i)
        list += "com.game.app/.Main#" + std::to_string(i) + "\n";
    const std::string lateBuffer = "SurfaceView[com.game.app/.Main](BLAST)#999";
    list += lateBuffer + "\n";
    names = Surfaces(list, "com.game.app", "");
    require(names.size() == MaxSurfaceCandidates && names.front() == lateBuffer &&
                names.back() == "com.game.app/.Main#126",
            "keep a bounded stable candidate set while allowing later buffers to replace low-ranked layers");
    require(Surfaces(list, "com.game.app", "com.game.app/.Main#140") ==
                std::vector<std::string>{"com.game.app/.Main#140"},
            "exact manual selection is applied before the candidate count bound");
    std::string longest = "com.game.app/";
    longest += std::string(MaxSurfaceNameBytes - longest.size() - 2, 'a');
    longest += "#1";
    require(Surfaces(longest, "com.game.app", "") == std::vector<std::string>{longest},
            "accept layer names at the documented byte bound");
    require(Surfaces(longest + "a", "com.game.app", "").empty() &&
                Surfaces(longest, "com.game.app", longest + "a").empty(),
            "reject overlong legacy names and manual selections");
    require(Surfaces("RequestedLayerState{" + longest + " parentId=2}", "com.game.app", "") ==
                std::vector<std::string>{longest},
            "enforce the length bound on the actual name rather than its debug wrapper");
    require(Surfaces("com.game.app/" + std::string(1, '\0') + "Main#1", "com.game.app", "").empty(),
            "embedded NUL bytes must never become shell arguments");
}
}

int main() {
    try {
        android16Names();
        candidateIdentity();
        candidateBounds();
        std::puts("Surface identity normalization, selection and bounds passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
