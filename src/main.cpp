// MiniMenu - UNTESTED. Written without access to the 2.209 bindings or Geode v6 SDK.
// Expect to fix member names / signatures against your SDK's bindings.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <fstream>
#include <vector>

using namespace geode::prelude;

namespace {
    // ---------- settings helpers ----------
    bool opt(char const* key) { return Mod::get()->getSettingValue<bool>(key); }
    std::string botMode() { return Mod::get()->getSettingValue<std::string>("bot-mode"); }

    // ---------- trajectory sim state ----------
    bool s_simulating = false; // true only while we step the fake players
    bool s_simDied = false;

    // ---------- replay bot state ----------
    struct Input { int frame; int button; bool down; bool p1; };
    std::vector<Input> s_inputs;
    size_t s_cursor = 0;
    int s_frame = 0;
    bool s_injecting = false;

    std::string replayPath(int levelID) {
        return fmt::format("{}/replay_{}.bin", Mod::get()->getSaveDir().string(), levelID);
    }

    void saveReplay(int levelID) {
        std::ofstream out(replayPath(levelID), std::ios::binary);
        for (auto& i : s_inputs) {
            int32_t f = i.frame, b = i.button;
            uint8_t d = i.down, p = i.p1;
            out.write(reinterpret_cast<char*>(&f), 4);
            out.write(reinterpret_cast<char*>(&b), 4);
            out.write(reinterpret_cast<char*>(&d), 1);
            out.write(reinterpret_cast<char*>(&p), 1);
        }
    }

    void loadReplay(int levelID) {
        s_inputs.clear();
        std::ifstream in(replayPath(levelID), std::ios::binary);
        while (in) {
            int32_t f, b; uint8_t d, p;
            if (!in.read(reinterpret_cast<char*>(&f), 4)) break;
            in.read(reinterpret_cast<char*>(&b), 4);
            in.read(reinterpret_cast<char*>(&d), 1);
            in.read(reinterpret_cast<char*>(&p), 1);
            s_inputs.push_back({f, b, d != 0, p != 0});
        }
    }
}

// ============================================================
// PlayLayer: noclip, hitboxes, trajectory, replay load/save
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

        auto mode = botMode();
        if (mode == "Replay") {
            loadReplay(level->m_levelID);
            m_isTestMode = true; // bot runs never submit
        }
        s_frame = 0;
        s_cursor = 0;
        if (mode == "Record") s_inputs.clear();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        s_frame = 0;
        s_cursor = 0;
    }

    void levelComplete() {
        if (botMode() == "Record") saveReplay(m_level->m_levelID);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (botMode() == "Record") saveReplay(m_level->m_levelID);
        PlayLayer::onQuit();
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
        drawHitboxes();
        drawTrajectory();
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
    // Least reliable part: collision handling on the fake player can touch real
    // object state (orbs/portals). Test on a plain level first.
    void drawTrajectory() {
        auto f = m_fields.self();
        auto node = f->trajNode;
        node->clear();
        f->debugLabel->setString("");
        if (!opt("trajectory") || !m_player1 || m_player1->m_isDead) return;

        float scale = static_cast<float>(Mod::get()->getSettingValue<double>("trajectory-time-scale"));
        int maxSteps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("trajectory-steps"));
        bool debug = opt("trajectory-debug");
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
                this->checkCollisions(fake, dt, false);
                auto now = fake->getPosition();
                node->drawSegment(last, now, 1.2f, col);
                last = now;
            }
            s_simulating = false;

            if (s_simDied) node->drawDot(last, 3.f, ccColor4F{1.f, 0.f, 0.f, 1.f});

            report += fmt::format(
                "{}: steps={} dx={:.1f} died={}\n",
                hold ? "hold" : "idle", s, last.x - start.x, s_simDied
            );
        }

        if (debug) f->debugLabel->setString(report.c_str());
    }
};

// ============================================================
// GJBaseGameLayer: replay bot record / playback
// ============================================================
class $modify(MMBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (PlayLayer::get() && botMode() == "Replay") {
            while (s_cursor < s_inputs.size() && s_inputs[s_cursor].frame <= s_frame) {
                auto& in = s_inputs[s_cursor++];
                s_injecting = true;
                this->handleButton(in.down, in.button, in.p1);
                s_injecting = false;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (PlayLayer::get()) s_frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (PlayLayer::get() && !s_injecting) {
            auto mode = botMode();
            if (mode == "Replay") return; // ignore manual input during playback
            if (mode == "Record") s_inputs.push_back({s_frame, button, down, isPlayer1});
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
