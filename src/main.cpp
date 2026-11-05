// MiniMenu - UNTESTED. Written without access to the 2.2091 bindings or Geode v6 SDK.
// Expect to fix member names / signatures against your SDK's bindings.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/ui/Notification.hpp>
#include <fmod.hpp>
#include <algorithm>
#include <cctype>
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
    // Inputs (what the game logic needs) + one state snapshot per physics step
    // (used to pull a drifting replay back onto the recorded path).
    // Both are indexed by ABSOLUTE frame counted from the level start. Practice
    // checkpoints remember their frame; loading one rewinds the clock and truncates
    // inputs and snapshots, so the failed attempt vanishes from the macro.
    struct Input { int frame; int button; bool down; bool p1; };

    struct PState {
        bool on = false;          // was this player active (visible) when captured
        float x = 0.f, y = 0.f, rot = 0.f;
        double yv = 0.0;          // vertical velocity
    };
    struct Snap { PState p[2]; };

    struct CpInfo { int frame; };

    std::vector<Input> s_rec;
    std::vector<Snap> s_snaps;
    int s_recLevel = 0;
    bool s_recBroken = false;      // a checkpoint was loaded that we never saw stored
    std::unordered_map<CheckpointObject*, CpInfo> s_cp;

    std::vector<Input> s_play;
    std::vector<Snap> s_playSnaps;
    size_t s_cursor = 0;

    int s_frame = 0;
    bool s_cpLoaded = false;       // set inside loadFromCheckpoint during a resetLevel
    bool s_injecting = false;

    // drift statistics for the HUD
    float s_driftMax[2] = {0.f, 0.f};
    float s_driftNow[2] = {0.f, 0.f};
    int s_fixCount = 0;
    bool s_desync = false;
    int s_desyncFrame = -1;
    int s_desyncPlayer = 0;
    bool s_driftValid = false;
    bool s_p2Active = false;

    // checkpoint rewind statistics for the HUD
    int s_rewinds = 0;
    int s_lastRewindFrame = -1;
    int s_lastDropped = 0;

    constexpr uint32_t kMagic1 = 0x3152414D; // v1: inputs only
    constexpr uint32_t kMagic3 = 0x3352414D; // v3: inputs + per-frame snapshots

    std::filesystem::path dirPath() {
        return std::filesystem::path(Mod::get()->getSaveDir().string()) / "replays";
    }

    template <class T>
    void put(std::ofstream& o, T const& v) { o.write(reinterpret_cast<char const*>(&v), sizeof(T)); }
    template <class T>
    bool get(std::ifstream& i, T& v) { return static_cast<bool>(i.read(reinterpret_cast<char*>(&v), sizeof(T))); }

    bool writeReplay(
        std::filesystem::path const& p, int level,
        std::vector<Input> const& inputs, std::vector<Snap> const& snaps
    ) {
        std::ofstream out(p, std::ios::binary);
        if (!out) return false;
        put<uint32_t>(out, kMagic3);
        put<int32_t>(out, level);
        put<uint32_t>(out, static_cast<uint32_t>(inputs.size()));
        for (auto& i : inputs) {
            put<int32_t>(out, i.frame);
            put<int32_t>(out, i.button);
            put<uint8_t>(out, i.down);
            put<uint8_t>(out, i.p1);
        }
        put<uint32_t>(out, static_cast<uint32_t>(snaps.size()));
        for (auto& s : snaps) {
            for (auto& ps : s.p) {
                put<uint8_t>(out, ps.on);
                put<float>(out, ps.x);
                put<float>(out, ps.y);
                put<float>(out, ps.rot);
                put<double>(out, ps.yv);
            }
        }
        return static_cast<bool>(out);
    }

    bool readReplay(
        std::filesystem::path const& p, int& level,
        std::vector<Input>& inputs, std::vector<Snap>& snaps
    ) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        uint32_t magic = 0, count = 0;
        int32_t lvl = 0;
        if (!get(in, magic) || (magic != kMagic1 && magic != kMagic3)) return false;
        if (!get(in, lvl) || !get(in, count) || count > 5'000'000) return false;

        inputs.clear();
        snaps.clear();
        inputs.reserve(count);
        for (uint32_t n = 0; n < count; n++) {
            int32_t f, b; uint8_t d, pl;
            if (!get(in, f) || !get(in, b) || !get(in, d) || !get(in, pl)) return false;
            inputs.push_back({f, b, d != 0, pl != 0});
        }

        if (magic == kMagic3) {
            uint32_t sc = 0;
            if (!get(in, sc) || sc > 5'000'000) return false;
            snaps.reserve(sc);
            for (uint32_t n = 0; n < sc; n++) {
                Snap s;
                for (auto& ps : s.p) {
                    uint8_t on;
                    if (!get(in, on) || !get(in, ps.x) || !get(in, ps.y) ||
                        !get(in, ps.rot) || !get(in, ps.yv)) return false;
                    ps.on = on != 0;
                }
                snaps.push_back(s);
            }
        }
        level = lvl;
        return true;
    }

    PState capture(PlayerObject* p) {
        PState s;
        if (!p) return s;
        s.on = p->isVisible();
        s.x = p->getPositionX();
        s.y = p->getPositionY();
        s.rot = p->getRotation();
        s.yv = p->m_yVelocity;
        return s;
    }

    // pull a player back onto the recorded state; only touches it if it actually drifted
    void fixPlayer(int idx, PlayerObject* p, PState const& s) {
        if (!p || !s.on) return;
        float mag = std::max(std::abs(p->getPositionX() - s.x), std::abs(p->getPositionY() - s.y));
        s_driftValid = true;
        s_driftNow[idx] = mag;
        s_driftMax[idx] = std::max(s_driftMax[idx], mag);
        if (idx == 1) s_p2Active = true;

        if (mag > 0.001f && mag < 20.f) {
            p->setPosition({s.x, s.y});
            p->m_yVelocity = s.yv;
            s_fixCount++;
        } else if (mag >= 20.f && !s_desync) {
            // too far to be drift: the run really diverged, leave it alone
            s_desync = true;
            s_desyncFrame = s_frame;
            s_desyncPlayer = idx + 1;
        }

        float rd = std::fmod(std::abs(p->getRotation() - s.rot), 360.f);
        if (rd > 180.f) rd = 360.f - rd;
        if (rd > 0.01f && mag < 20.f) p->setRotation(s.rot);
    }

    // Replay mode: load the replay chosen in the Replay tab
    void loadForLevel(int levelID) {
        s_play.clear();
        s_playSnaps.clear();

        auto name = Mod::get()->getSavedValue<std::string>("selected-replay", "");
        if (name.empty()) {
            Notification::create(
                "No replay selected. Pick one in the Replay tab.", NotificationIcon::Error
            )->show();
            return;
        }

        int lvl = 0;
        if (!readReplay(dirPath() / (name + ".mmr"), lvl, s_play, s_playSnaps)) {
            s_play.clear();
            s_playSnaps.clear();
            Notification::create("Could not read replay " + name, NotificationIcon::Error)->show();
            return;
        }

        if (lvl != levelID) {
            if (!opt("bot-any-level")) {
                s_play.clear();
                s_playSnaps.clear();
                Notification::create(
                    "This replay was recorded on another level. Turn on Replay On Any Level.",
                    NotificationIcon::Error
                )->show();
                return;
            }
            Notification::create(
                "Replay is from another level; playing it here anyway", NotificationIcon::Warning
            )->show();
        }
    }

    void resetBotTimeline() {
        s_frame = 0;
        s_cursor = 0;
        s_cp.clear();
        s_driftMax[0] = s_driftMax[1] = 0.f;
        s_driftNow[0] = s_driftNow[1] = 0.f;
        s_fixCount = 0;
        s_desync = false;
        s_desyncFrame = -1;
        s_desyncPlayer = 0;
        s_driftValid = false;
        s_p2Active = false;
        s_rewinds = 0;
        s_lastRewindFrame = -1;
        s_lastDropped = 0;
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
        return fmt::format("Recording: {} inputs, {} frames", s_rec.size(), s_snaps.size());
    }

    // Saves the recording in memory under a name the user chose. `name` comes back cleaned.
    bool saveReplayAs(std::string& name, std::string& err) {
        if (!hasRecording()) {
            err = "Nothing to save. Record a run first.";
            return false;
        }
        std::string clean;
        for (char c : name) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') clean += c;
            else if (c == ' ') clean += '_';
        }
        if (clean.size() > 32) clean.resize(32);
        if (clean.empty()) {
            err = "Type a name first.";
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(dirPath(), ec);
        auto path = dirPath() / (clean + ".mmr");
        if (std::filesystem::exists(path, ec)) {
            err = fmt::format("A replay named \"{}\" already exists.", clean);
            return false;
        }
        if (!writeReplay(path, s_recLevel, s_rec, s_snaps)) {
            err = "Could not write the file.";
            return false;
        }
        name = clean;
        return true;
    }
}

// ============================================================
// Speedhack: scale game time AND audio
// ============================================================
namespace {
    // Pitch of the master channel group scales music and sound effects together,
    // so the song stays in sync with the sped-up / slowed-down game.
    void applyAudioSpeed(float speed) {
        static float last = 1.f;
        if (speed == 1.f && last == 1.f) return; // never touch FMOD unless speedhack is in use
        last = speed;
        auto engine = FMODAudioEngine::sharedEngine();
        if (!engine || !engine->m_system) return;
        FMOD::ChannelGroup* master = nullptr;
        if (engine->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
            master->setPitch(speed);
        }
    }
}

class $modify(MMScheduler, CCScheduler) {
    void update(float dt) {
        float s = speedValue();
        applyAudioSpeed(s);
        CCScheduler::update(dt * s);
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
            s_snaps.clear();
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
            resetBotTimeline(); // replay always restarts from frame 0
        } else if (!s_cpLoaded) {
            resetBotTimeline(); // respawned at the very start: fresh timeline
            if (mode == "Record") {
                s_rec.clear();
                s_snaps.clear();
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

    // does the recorded state at `frame` match the player the game just restored?
    bool snapMatches(int frame) {
        if (frame < 0 || static_cast<size_t>(frame) >= s_snaps.size() || !m_player1) return false;
        auto const& st = s_snaps[frame].p[0];
        return std::abs(st.x - m_player1->getPositionX()) < 0.05f &&
               std::abs(st.y - m_player1->getPositionY()) < 0.05f;
    }

    // latest recorded frame whose state equals the restored one (used if the stored frame is missing/wrong)
    int findMatchingFrame() {
        for (int i = static_cast<int>(s_snaps.size()) - 1; i >= 0; i--) {
            if (snapMatches(i)) return i;
        }
        return -1;
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::loadFromCheckpoint(checkpoint);
        s_cpLoaded = true;
        if (botMode() != "Record") return;

        // 1. the frame remembered when the checkpoint was stored, if it agrees with the restored state
        int n = -1;
        char const* how = "checkpoint";
        auto it = s_cp.find(checkpoint);
        if (it != s_cp.end() && snapMatches(it->second.frame)) n = it->second.frame;

        // 2. otherwise find the recorded frame the restored state belongs to
        if (n < 0) {
            n = findMatchingFrame();
            how = "position";
        }
        if (n < 0) {
            s_recBroken = true; // nothing in the timeline matches: it can't be trusted
            if (opt("bot-debug")) {
                Notification::create("Macro lost sync: checkpoint not found", NotificationIcon::Error)->show();
            }
            return;
        }

        // drop the failed attempt: inputs and snapshots at or after the checkpoint frame
        size_t before = s_rec.size();
        auto cut = std::lower_bound(
            s_rec.begin(), s_rec.end(), n,
            [](Input const& in, int frame) { return in.frame < frame; }
        );
        s_rec.erase(cut, s_rec.end());
        if (s_snaps.size() > static_cast<size_t>(n)) s_snaps.resize(n);
        s_lastDropped = static_cast<int>(before - s_rec.size());

        // some setups keep nothing held across a respawn; make the timeline say the same
        if (opt("bot-release-on-rewind")) {
            bool held[2][4] = {};
            for (auto const& in : s_rec) {
                if (in.button >= 1 && in.button <= 3) held[in.p1 ? 0 : 1][in.button] = in.down;
            }
            for (int pl = 0; pl < 2; pl++) {
                for (int b = 1; b <= 3; b++) {
                    if (held[pl][b]) s_rec.push_back({n, b, false, pl == 0});
                }
            }
        }

        // rewind the clock
        s_frame = n;
        s_cursor = 0;
        s_rewinds++;
        s_lastRewindFrame = n;
        if (opt("bot-debug")) {
            Notification::create(
                fmt::format("Macro rewound to frame {} ({}), dropped {} inputs", n, how, s_lastDropped),
                NotificationIcon::Info
            )->show();
        }
    }

    void levelComplete() {
        // nothing is saved automatically: use Save in the Replay tab to name the recording
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
                    "REC frame={} inputs={} frames={} checkpoints={}{}\n",
                    s_frame, s_rec.size(), s_snaps.size(), s_cp.size(),
                    s_recBroken ? " LOST SYNC" : ""
                );
                text += fmt::format(
                    "rewinds={} last->frame {} dropped {}\n",
                    s_rewinds, s_lastRewindFrame, s_lastDropped
                );
            } else if (mode == "Replay") {
                text += fmt::format("PLAY frame={} input {}/{}\n", s_frame, s_cursor, s_play.size());
                if (s_driftValid) {
                    text += fmt::format(
                        "p1 drift now={:.2f} max={:.2f}\n", s_driftNow[0], s_driftMax[0]
                    );
                    if (s_p2Active) {
                        text += fmt::format(
                            "p2 drift now={:.2f} max={:.2f}\n", s_driftNow[1], s_driftMax[1]
                        );
                    }
                    text += fmt::format("corrected={}", s_fixCount);
                    if (s_desync) text += fmt::format(" DESYNC p{} @frame {}", s_desyncPlayer, s_desyncFrame);
                    text += "\n";
                }
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
        // the fake player runs the real collision code, which can flip real orb/portal
        // state; that would desync a recording, so it stays off while the bot is active
        if (botMode() != "Off" && !opt("trajectory-with-bot")) return "";

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
        if (pl && !s_simulating && m_player1) {
            auto mode = botMode();
            size_t frame = static_cast<size_t>(s_frame);

            if (mode == "Record") {
                // one snapshot per step, taken before the step runs
                if (s_snaps.size() > frame) s_snaps.resize(frame);
                if (s_snaps.size() == frame) {
                    Snap sn;
                    sn.p[0] = capture(m_player1);
                    sn.p[1] = capture(m_player2);
                    // a parked / hidden player 2 must not count (dual mode keeps both close together)
                    if (sn.p[1].on && std::abs(sn.p[1].x - sn.p[0].x) > 600.f) sn.p[1].on = false;
                    s_snaps.push_back(sn);
                }
            } else if (mode == "Replay") {
                // 1. pull the players back onto the recorded path if they drifted
                if (opt("bot-frame-fix") && frame < s_playSnaps.size()) {
                    fixPlayer(0, m_player1, s_playSnaps[frame].p[0]);
                    fixPlayer(1, m_player2, s_playSnaps[frame].p[1]);
                }
                // 2. feed this step's inputs
                while (s_cursor < s_play.size() && s_play[s_cursor].frame <= s_frame) {
                    auto& in = s_play[s_cursor++];
                    s_injecting = true;
                    this->handleButton(in.down, in.button, in.p1);
                    s_injecting = false;
                }
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
