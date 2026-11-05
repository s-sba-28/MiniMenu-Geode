#pragma once
#include <string>
#include <vector>

namespace mm {
    std::string replayDir();
    std::vector<std::string> listReplays();
    bool hasRecording();
    bool saveCurrentReplay(std::string& outName);
}
