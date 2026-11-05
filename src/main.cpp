// MiniMenu - UNTESTED. Written without access to the 2.2091 bindings or Geode v6 SDK.
// Expect to fix member names / signatures against your SDK's bindings.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>
#include "replay.hpp"

using namespace geode::prelude;

namespace {
    // ---------- settings helpers ----------
    bool opt(char const* key) { return Mod::get()->getSettingValue<bool>(key); }
    std::string botMode() { return Mod::get()->getSettingValue<std::string>("bot-mode"); }
    float speedValue() { return static_cast<float>(Mod::get()->getSettingValue<double>("speedhack")); }

    // ---------- trajectory sim state ----------
    bool s_simulating = false; // true only while we step the fake players
    bool s_simDied = false;

    // ---------- replay bot state ----------
    // One timeline of ABSOLUTE frames counted from the level start.
    // Practice checkpoints remember the frame they were made on. Loading a checkpoint
    // rewinds the frame counter to that value and drops everything recorded after it,
    // so the failed attempt vanishes and the timeline stays one clean run.
    struct Input { int frame; int button; bool down; bool p1; };
    struct CpInfo { int frame; };

    std::vector<Input> s_rec;
    int s_recLevel = 0;
    bool s_recBroken = false;      // true if a checkpoint was loaded that we never saw stored
    std::unordered_map<CheckpointObject*, CpInfo> s_cp;

    std::vector<Input> s_play;
    size_t s_cursor = 0;

    int s_frame = 0;
    bool s_cpLoaded = false;       // set inside loadFromCheckpoint during a resetLevel
    bool s_injecting = false;

    constexpr uint32_t kMagic = 0x3152414D;

    std::filesystem::path dirPath() {
        return std::filesystem::path(Mod::get()->getSaveDir().string()) / "replays";
    }

    bool writeReplay(std::filesystem::path const& p, int level, std::vector<Input> const& v) {
        std::ofstream out(p, std::ios::binary);
        if (!out) return false;
        uint32_t magic = kMagic, count = static_cast<uint32_t>(v.size());
        int32_t lvl = level;
        out.write(reinterpret_cast<char*>(&magic), 4);
        out.write(reinterpret_cast<char*>(&lvl), 4);
        out.write(reinterpret_cast<char*>(&count), 4);
        for (auto& i : v) {
            int32_t f = i.frame, b = i.button;
            uint8_t d = i.down, pl = i.p1;
            out.write(reinterpret_cast<char*>(&f), 4);
            out.write(reinterpret_cast<char*>(&b), 4);
            out.write(reinterpret_cast<char*>(&d), 1);
            out.write(reinterpret_cast<char*>(&pl), 1);
        }
        return static_cast<bool>(out);
    }

    bool readReplay(std::filesystem::path const& p, int& level, std::vector<Input>& v) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        uint32_t magic = 0, count = 0;
        int32_t lvl = 0;
        if (!in.read(reinterpret_cast<char*>(&magic), 4) || magic != kMagic) return false;
        if (!in.read(reinterpret_cast<char*>(&lvl), 4)) return false;
        if (!in.read(reinterpret_cast<char*>(&count), 4)) return false;
        if (count > 5'000'000) return false;
        v.clear();
        v.reserve(count);
        for (uint32_t n = 0; n < count; n++) {
            int32_t f, b; uint8_t d, pl;
            if (!in.read(reinterpret_cast<char*>(&f), 4)) return false;
            if (!in.read(reinterpret_cast<char*>(&b), 4)) return false;
            if (!in.read(reinterpret_cast<char*>(&d), 1)) return false;
            if (!in.read(reinterpret_cast<char*>(&pl), 1)) return false;
            v.push_back({f, b, d != 0, pl != 0});
        }
        level = lvl;
        return true;
    }

    // Replay mode: the panel's selected replay, falling back to this level's auto-saved one
    void loadForLevel(int levelID) {
        s_play.clear();
        int lvl = 0;

        auto name = Mod::get()->getSavedValue<std::string>("selected-replay", "");
        if (!name.empty() && readReplay(dirPath() / (name + ".mmr"), lvl, s_play) && lvl == levelID) {
            return;
        }

        s_play.clear();
        auto latest = fmt::format("L{}_latest", levelID);
        if (readReplay(dirPath() / (latest + ".mmr"), lvl, s_play) && lvl == levelID) {
            return;
        }

        s_play.clear();
        Notification::create(
            "No replay for this level. Record one, or pick one in the Replay tab.",
            NotificationIcon::Error
        )->show();
    }

    void resetBotTimeline() {
        s_frame = 0;
        s_cursor = 0;
        s_cp.clear();
    }
}

// ---------- API used by the panel ----------
namespace mm {
    std::string replayDir() { return dirPath().string(); }

    std::vector<std::string> listReplays() {
        std::vector<std::string> out;
        std::error_code ec;
        std::filesystem::create_directories(dirPath(), ec);
        for (auto& e : std::filesystem::directory_iterator(dirPath(), ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".mmr") {
                out.push_back(e.path().stem().string());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool hasRecording() { return !s_rec.empty() && s_recLevel != 0 && !s_recBroken; }

    std::string recordingInfo() {
        if (s_recBroken) return "Recording lost sync (unknown checkpoint)";
        if (s_rec.empty() || s_recLevel == 0) return "No recording in memory";
        return fmt::format("Recording: level {}, {} inputs", s_recLevel, s_rec.size());
    }

    bool saveCurrentReplay(std::string& outName) {
        if (!hasRecording()) return false;
        std::error_code ec;
        std::filesystem::create_directories(dirPath(), ec);
        for (int n = 1;; n++) {
            outName = fmt::format("L{}_{}", s_recLevel, n);
            if (!std::filesystem::exists(dirPath() / (outName + ".mmr"), ec)) break;
        }
        return writeReplay(dirPath() / (outName + ".mmr"), s_recLevel, s_rec);
    }
}

// ============================================================
// Speedhack: scale the time the scheduler hands to everything
// ============================================================
class $modify(MMScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt * speedValue());
    }
};

// ============================================================
// PlayLayer: noclip, hitboxes, trajectory, checkpoint-aware recording
// ============================================================
class $modify(MMPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* hitNode = nullptr;
        CCDrawNode* trajNode = nullptr;
        Ref<PlayerObject> sim[2];
        CCLabelBMFont* debugLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;

        auto f = m_fields.self();
        f->hitNode = CCDrawNode::create();
        f->trajNode = CCDrawNode::create();
        m_objectLayer->addChild(f->hitNode, 10000);
        m_objectLayer->addChild(f->trajNode, 10000);

        f->debugLabel = CCLabelBMFont::create("", "chatFont.fnt");
        f->debugLabel->setAnchorPoint({0.f, 1.f});
        f->debugLabel->setScale(0.5f);
        f->debugLabel->setPosition({70.f, CCDirector::get()->getWinSize().height - 10.f});
        this->addChild(f->debugLabel, 1000);

        for (auto& s : f->sim) {
            s = PlayerObject::create(1, 1, this, m_objectLayer, false);
        }

        resetBotTimeline();
        auto mode = botMode();
        if (mode == "Record") {
            s_rec.clear();
            s_recLevel = level->m_levelID;
            s_recBroken = false;
        } else if (mode == "Replay") {
            loadForLevel(level->m_levelID);
            m_isTestMode = true; // bot runs never submit
        }
        return true;
    }

    void resetLevel() {
        s_cpLoaded = false;
        PlayLayer::resetLevel();

        auto mode = botMode();
        if (mode == "Replay") {
            // replay always restarts from frame 0
            resetBotTimeline();
        } else if (!s_cpLoaded) {
            // respawned at the very start: begin a fresh timeline
            resetBotTimeline();
            if (mode == "Record") {
                s_rec.clear();
                s_recBroken = false;
            }
        }
        // (if a checkpoint was loaded, loadFromCheckpoint already rewound the timeline)
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        if (botMode() == "Record" && checkpoint) {
            s_cp[checkpoint] = {s_frame};
        }
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        s_cpLoaded = true;
        if (botMode() != "Record") return;

        auto it = s_cp.find(checkpoint);
        if (it == s_cp.end()) {
            s_recBroken = true; // checkpoint we never saw: timeline can't be trusted
            return;
        }
        int n = it->second.frame;

        // 1. drop the failed attempt: everything recorded at or after the checkpoint frame
        auto cut = std::lower_bound(
            s_rec.begin(), s_rec.end(), n,
            [](Input const& in, int frame) { return in.frame < frame; }
        );
        s_rec.erase(cut, s_rec.end());

        // 2. the game releases all buttons on respawn; make the timeline say the same
        bool held[2][4] = {};
        for (auto const& in : s_rec) {
            if (in.button >= 1 && in.button <= 3) held[in.p1 ? 0 : 1][in.button] = in.down;
        }
        for (int p = 0; p < 2; p++) {
            for (int b = 1; b <= 3; b++) {
                if (held[p][b]) s_rec.push_back({n, b, false, p == 0});
            }
        }

        // 3. rewind the clock
        s_frame = n;
        s_cursor = 0;
    }

    void levelComplete() {
        if (botMode() == "Record" && mm::hasRecording()) {
            auto path = dirPath() / fmt::format("L{}_latest.mmr", s_recLevel);
            std::error_code ec;
            std::filesystem::create_directories(dirPath(), ec);
            if (writeReplay(path, s_recLevel, s_rec)) {
                Notification::create(
                    fmt::format("Replay saved: L{}_latest", s_recLevel), NotificationIcon::Success
                )->show();
            }
        }
        PlayLayer::levelComplete();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (s_simulating) { s_simDied = true; return; } // fake player hit something
        if (opt("noclip") && obj != m_anticheatSpike) {
            m_isTestMode = true; // keep noclip runs from being submitted
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (speedValue() != 1.f) m_isTestMode = true; // speedhack runs never submit
        drawHitboxes();
        updateHud(drawTrajectory());
    }

    // ---- HUD text ----
    void updateHud(std::string text) {
        if (opt("bot-debug")) {
            auto mode = botMode();
            if (mode == "Record") {
                text += fmt::format(
                    "REC frame={} inputs={} checkpoints={}{}\n",
                    s_frame, s_rec.size(), s_cp.size(), s_recBroken ? " LOST SYNC" : ""
                );
            } else if (mode == "Replay") {
                text += fmt::format("PLAY frame={} input {}/{}\n", s_frame, s_cursor, s_play.size());
            }
        }
        m_fields->debugLabel->setString(text.c_str());
    }

    // ---- hitboxes ----
    void drawHitboxes() {
        auto node = m_fields->hitNode;
        node->clear();
        if (!opt("show-hitboxes") || !m_player1) return;

        float px = m_player1->getPositionX();
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj || std::abs(obj->getPositionX() - px) > 500.f) continue;

            ccColor4F border;
            switch (obj->m_objectType) {
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard: border = {1.f, 0.f, 0.f, 1.f}; break;
                case GameObjectType::Solid:          border = {0.f, 0.4f, 1.f, 1.f}; break;
                default: continue;
            }
            ccColor4F fill = {border.r, border.g, border.b, 0.15f};
            auto r = obj->getObjectRect();
            node->drawRect(
                ccp(r.getMinX(), r.getMinY()), ccp(r.getMaxX(), r.getMaxY()),
                fill, 0.5f, border
            );
        }

        for (auto p : {m_player1, m_player2}) {
            if (!p) continue;
            auto r = p->getObjectRect();
            ccColor4F c = {1.f, 1.f, 0.f, 1.f};
            node->drawRect(
                ccp(r.getMinX(), r.getMinY()), ccp(r.getMaxX(), r.getMaxY()),
                {1.f, 1.f, 0.f, 0.15f}, 0.5f, c
            );
        }
    }

    // ---- trajectory ----
    // physics + solid landings come from the real collision code on a fake player;
    // deaths are detected with our own rectangle tests because the damage-free
    // collision mode never reports them.
    std::string drawTrajectory() {
        auto f = m_fields.self();
        auto node = f->trajNode;
        node->clear();
        if (!opt("trajectory") || !m_player1 || m_player1->m_isDead) return "";

        float scale = static_cast<float>(Mod::get()->getSettingValue<double>("trajectory-time-scale"));
        int maxSteps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("trajectory-steps"));
        if (scale == 1.f) scale = 60.f;       // migrate old saved default
        if (maxSteps == 150) maxSteps = 600;  // migrate old saved default
        bool debug = opt("trajectory-debug");
        bool solidDeath = opt("trajectory-solid-death");

        // nearby hazards / solids, collected once per frame
        std::vector<CCRect> hazards, solids;
        float px = m_player1->getPositionX();
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            float ox = obj->getPositionX();
            if (ox < px - 80.f || ox > px + 1800.f) continue;
            switch (obj->m_objectType) {
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard: hazards.push_back(obj->getObjectRect()); break;
                case GameObjectType::Solid:          solids.push_back(obj->getObjectRect()); break;
                default: break;
            }
        }

        auto base = m_player1->getObjectRect();
        float pw = base.size.width, ph = base.size.height;

        auto overlap = [](float cx, float cy, float w, float h, CCRect const& r, float& ox, float& oy) {
            float l = std::max(cx - w / 2.f, r.getMinX());
            float rt = std::min(cx + w / 2.f, r.getMaxX());
            float b = std::max(cy - h / 2.f, r.getMinY());
            float t = std::min(cy + h / 2.f, r.getMaxY());
            ox = rt - l;
            oy = t - b;
            return ox > 0.f && oy > 0.f;
        };

        std::string report;
        for (int i = 0; i < 2; i++) {
            bool hold = (i == 1);
            auto fake = f->sim[i].data();
            if (!fake) continue;

            fake->copyAttributes(m_player1);
            fake->setPosition(m_player1->getPosition());
            if (hold) fake->pushButton(PlayerButton::Jump);

            s_simulating = true;
            s_simDied = false;

            ccColor4F col = hold ? ccColor4F{0.f, 1.f, 0.f, 1.f} : ccColor4F{1.f, 0.4f, 0.f, 1.f};
            float dt = (1.f / 240.f) * scale;
            CCPoint start = fake->getPosition();
            CCPoint last = start;

            int s = 0;
            for (; s < maxSteps && !s_simDied; s++) {
                fake->update(dt);
                this->checkCollisions(fake, dt, true);
                auto now = fake->getPosition();
                node->drawSegment(last, now, 1.2f, col);
                last = now;

                float ox, oy;
                // hazards use a shrunken box, like the game's small hitbox
                for (auto const& h : hazards) {
                    if (overlap(now.x, now.y, pw * 0.4f, ph * 0.4f, h, ox, oy)) { s_simDied = true; break; }
                }
                // hitting a solid deeply (not just resting on it) is a crash
                if (!s_simDied && solidDeath) {
                    for (auto const& r : solids) {
                        if (overlap(now.x, now.y, pw, ph, r, ox, oy) && ox > 3.f && oy > 3.f) {
                            s_simDied = true;
                            break;
                        }
                    }
                }
            }
            s_simulating = false;

            if (s_simDied) node->drawDot(last, 3.f, ccColor4F{1.f, 0.f, 0.f, 1.f});

            report += fmt::format(
                "{}: steps={} dx={:.1f} died={}\n",
                hold ? "hold" : "idle", s, last.x - start.x, s_simDied
            );
        }

        return debug ? report : "";
    }
};

// ============================================================
// GJBaseGameLayer: replay bot record / playback
// ============================================================
class $modify(MMBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && !s_simulating && botMode() == "Replay") {
            while (s_cursor < s_play.size() && s_play[s_cursor].frame <= s_frame) {
                auto& in = s_play[s_cursor++];
                s_injecting = true;
                this->handleButton(in.down, in.button, in.p1);
                s_injecting = false;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (pl) s_frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (PlayLayer::get() && !s_injecting && !s_simulating) {
            auto mode = botMode();
            if (mode == "Replay") return; // manual input is ignored during playback
            if (mode == "Record") s_rec.push_back({s_frame, button, down, isPlayer1});
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
