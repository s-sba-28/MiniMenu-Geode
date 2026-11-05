#pragma once
#include <string>
#include <vector>

// Shared between main.cpp (storage / bot) and overlay.cpp (panel UI).
namespace mm {
    std::string replayDir();                       // folder holding *.mmr files
    std::vector<std::string> listReplays();        // names without extension, sorted
    bool hasRecording();                           // usable recording in memory?
    std::string recordingInfo();                   // short status text for the panel
    bool saveCurrentReplay(std::string& outName);  // writes it to replayDir(), returns its name
}
