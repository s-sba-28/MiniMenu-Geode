#pragma once
#include <string>
#include <vector>

// Shared between main.cpp (storage / bot) and overlay.cpp (panel UI).
namespace mm {
    std::string replayDir();                       // folder holding *.mmr files
    std::vector<std::string> listReplays();        // names without extension, sorted
    bool hasRecording();                           // is there a recording with inputs in memory?
    bool recordingBroken();                        // did it lose sync with a checkpoint?
    std::string recordingInfo();                   // short status text for the panel
    // save the recording under a user-chosen name; `name` is cleaned in place, `err` explains a failure
    bool saveReplayAs(std::string& name, std::string& err);
}
