// MiniMenu overlay - UNTESTED. Touch/popup APIs may differ in Geode v6 / GD 2.2091.
#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include "replay.hpp"

using namespace geode::prelude;

namespace {
    constexpr char const* kKeys[3] = {"noclip", "show-hitboxes", "trajectory"};
    constexpr char const* kNames[3] = {"Noclip", "Hitboxes", "Trajectory"};
    constexpr char const* kModes[3] = {"Off", "Record", "Replay"};

    int modeIndex(std::string const& m) {
        for (int i = 0; i < 3; i++) if (m == kModes[i]) return i;
        return 0;
    }

    std::string shortName(std::string s, size_t n = 22) {
        if (s.size() > n) s = s.substr(0, n - 1) + "~";
        return s;
    }

    std::string selectedName() {
        return Mod::get()->getSavedValue<std::string>("selected-replay", "");
    }

    std::string selectorText() {
        auto s = selectedName();
        return s.empty() ? std::string("Select replay") : shortName(s);
    }

    // speed slider: position 0..1 <-> 0.1x..3.0x on a log scale (1.0x sits near 68%)
    float valueToSpeed(float v) { return 0.1f * std::pow(30.f, v); }
    float speedToValue(float s) {
        s = std::clamp(s, 0.1f, 3.f);
        return std::log(s / 0.1f) / std::log(30.f);
    }

    constexpr float kSliderW = 180.f;
}

// ---------------- settings panel ----------------
class MMPanel : public CCLayerColor {
protected:
    CCPoint m_c;

    // tabs
    CCNode* m_main = nullptr;
    CCNode* m_replay = nullptr;
    CCMenu* m_mainMenu = nullptr;
    CCMenu* m_replayMenu = nullptr;
    ButtonSprite* m_tabMainSpr = nullptr;
    ButtonSprite* m_tabReplaySpr = nullptr;

    // main tab: speed slider
    CCLabelBMFont* m_speedLabel = nullptr;
    CCLayerColor* m_fill = nullptr;
    CCLayerColor* m_thumb = nullptr;
    float m_sliderX0 = 0.f;
    float m_sliderY = 0.f;
    bool m_dragging = false;

    // replay tab
    ButtonSprite* m_modeSprite = nullptr;
    ButtonSprite* m_selSprite = nullptr;
    CCMenuItemSpriteExtra* m_selBtn = nullptr;
    CCLabelBMFont* m_infoLabel = nullptr;

    // dropdown
    CCMenu* m_dropMenu = nullptr;
    CCNode* m_dropBg = nullptr;
    CCNode* m_emptyLabel = nullptr;
    CCNode* m_pageLabel = nullptr;
    std::vector<std::string> m_names;
    int m_page = 0;

    CCMenu* makeMenu(CCNode* parent) {
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setHandlerPriority(-501); // above the panel's touch-swallowing layer
        parent->addChild(menu);
        return menu;
    }

    bool init() override {
        if (!CCLayerColor::initWithColor({0, 0, 0, 150})) return false;
        this->setTouchEnabled(true);

        auto win = CCDirector::get()->getWinSize();
        m_c = ccp(win.width / 2.f, win.height / 2.f);
        auto c = m_c;

        auto bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({280.f, 270.f});
        bg->setPosition(c);
        this->addChild(bg);

        auto title = CCLabelBMFont::create("MiniMenu", "goldFont.fnt");
        title->setScale(0.8f);
        title->setPosition(c.x, c.y + 120.f);
        this->addChild(title);

        // tab buttons + close live outside the groups so they always work
        auto tabMenu = makeMenu(this);

        m_tabMainSpr = ButtonSprite::create("Main");
        m_tabMainSpr->setScale(0.5f);
        auto tabMain = CCMenuItemSpriteExtra::create(m_tabMainSpr, this, menu_selector(MMPanel::onTabMain));
        tabMain->setPosition(c.x - 55.f, c.y + 96.f);
        tabMenu->addChild(tabMain);

        m_tabReplaySpr = ButtonSprite::create("Replay");
        m_tabReplaySpr->setScale(0.5f);
        auto tabReplay = CCMenuItemSpriteExtra::create(m_tabReplaySpr, this, menu_selector(MMPanel::onTabReplay));
        tabReplay->setPosition(c.x + 55.f, c.y + 96.f);
        tabMenu->addChild(tabReplay);

        auto closeSpr = ButtonSprite::create("Close");
        closeSpr->setScale(0.55f);
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(MMPanel::onClose));
        closeBtn->setPosition(c.x, c.y - 112.f);
        tabMenu->addChild(closeBtn);

        buildMainTab();
        buildReplayTab();
        showTab(0);
        return true;
    }

    // ================= main tab =================
    void buildMainTab() {
        auto c = m_c;
        m_main = CCNode::create();
        this->addChild(m_main);
        m_mainMenu = makeMenu(m_main);

        for (int i = 0; i < 3; i++) {
            float y = c.y + 62.f - i * 30.f;

            auto label = CCLabelBMFont::create(kNames[i], "bigFont.fnt");
            label->setScale(0.5f);
            label->setAnchorPoint({0.f, 0.5f});
            label->setPosition(c.x - 110.f, y);
            m_main->addChild(label);

            auto tog = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(MMPanel::onToggle), 0.6f
            );
            tog->setTag(i);
            tog->setPosition(c.x + 100.f, y);
            tog->toggle(Mod::get()->getSettingValue<bool>(kKeys[i]));
            m_mainMenu->addChild(tog);
        }

        // speed slider (drawn by hand; the panel's touch methods drive it)
        m_speedLabel = CCLabelBMFont::create("Speed: 1.00x", "bigFont.fnt");
        m_speedLabel->setScale(0.5f);
        m_speedLabel->setPosition(c.x, c.y - 38.f);
        m_main->addChild(m_speedLabel);

        m_sliderX0 = c.x - kSliderW / 2.f;
        m_sliderY = c.y - 64.f;

        auto track = CCLayerColor::create({60, 60, 60, 255}, kSliderW, 6.f);
        track->setPosition({m_sliderX0, m_sliderY - 3.f});
        m_main->addChild(track);

        m_fill = CCLayerColor::create({90, 230, 90, 255}, 1.f, 6.f);
        m_fill->setPosition({m_sliderX0, m_sliderY - 3.f});
        m_main->addChild(m_fill);

        m_thumb = CCLayerColor::create({255, 255, 255, 255}, 8.f, 20.f);
        m_main->addChild(m_thumb);

        auto resetSpr = ButtonSprite::create("1.0x");
        resetSpr->setScale(0.45f);
        auto resetBtn = CCMenuItemSpriteExtra::create(resetSpr, this, menu_selector(MMPanel::onSpeedReset));
        resetBtn->setPosition(c.x + 112.f, c.y - 38.f);
        m_mainMenu->addChild(resetBtn);

        refreshSlider();
    }

    void refreshSlider() {
        float s = static_cast<float>(Mod::get()->getSettingValue<double>("speedhack"));
        float v = speedToValue(s);
        m_fill->changeWidth(std::max(1.f, v * kSliderW));
        m_thumb->setPosition({m_sliderX0 + v * kSliderW - 4.f, m_sliderY - 10.f});
        m_speedLabel->setString(fmt::format("Speed: {:.2f}x", s).c_str());
    }

    void applySliderTouch(float x) {
        float v = std::clamp((x - m_sliderX0) / kSliderW, 0.f, 1.f);
        float s = valueToSpeed(v);
        if (std::abs(s - 1.f) < 0.04f) s = 1.f; // snap to normal speed
        Mod::get()->setSettingValue<double>("speedhack", static_cast<double>(s));
        refreshSlider();
    }

    void onSpeedReset(CCObject*) {
        Mod::get()->setSettingValue<double>("speedhack", 1.0);
        refreshSlider();
    }

    // ================= replay tab =================
    void buildReplayTab() {
        auto c = m_c;
        m_replay = CCNode::create();
        this->addChild(m_replay);
        m_replayMenu = makeMenu(m_replay);

        auto mode = Mod::get()->getSettingValue<std::string>("bot-mode");
        m_modeSprite = ButtonSprite::create(fmt::format("Bot: {}", mode).c_str());
        m_modeSprite->setScale(0.55f);
        auto modeBtn = CCMenuItemSpriteExtra::create(m_modeSprite, this, menu_selector(MMPanel::onMode));
        modeBtn->setPosition(c.x, c.y + 62.f);
        m_replayMenu->addChild(modeBtn);

        m_selSprite = ButtonSprite::create(
            selectorText().c_str(), 200, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.55f
        );
        m_selBtn = CCMenuItemSpriteExtra::create(m_selSprite, this, menu_selector(MMPanel::onSelector));
        m_selBtn->setPosition(c.x, c.y + 24.f);
        m_replayMenu->addChild(m_selBtn);

        char const* names[3] = {"Save", "Import", "Refresh"};
        SEL_MenuHandler handlers[3] = {
            menu_selector(MMPanel::onSave),
            menu_selector(MMPanel::onImport),
            menu_selector(MMPanel::onRefresh)
        };
        for (int i = 0; i < 3; i++) {
            auto spr = ButtonSprite::create(names[i]);
            spr->setScale(0.5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, handlers[i]);
            btn->setPosition(c.x - 85.f + i * 85.f, c.y - 12.f);
            m_replayMenu->addChild(btn);
        }

        m_infoLabel = CCLabelBMFont::create(mm::recordingInfo().c_str(), "chatFont.fnt");
        m_infoLabel->setScale(0.6f);
        m_infoLabel->setPosition(c.x, c.y - 44.f);
        m_replay->addChild(m_infoLabel);

        auto note = CCLabelBMFont::create("Finished runs auto-save as L<id>_latest", "chatFont.fnt");
        note->setScale(0.5f);
        note->setPosition(c.x, c.y - 62.f);
        m_replay->addChild(note);
    }

    // ================= tabs =================
    void showTab(int tab) {
        closeDrop();
        m_main->setVisible(tab == 0);
        m_replay->setVisible(tab == 1);
        m_tabMainSpr->setString(tab == 0 ? "[Main]" : "Main");
        m_tabReplaySpr->setString(tab == 1 ? "[Replay]" : "Replay");
        if (tab == 1) m_infoLabel->setString(mm::recordingInfo().c_str());
    }

    void onTabMain(CCObject*) { showTab(0); }
    void onTabReplay(CCObject*) { showTab(1); }

    // ================= touch =================
    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (m_main && m_main->isVisible()) {
            auto p = t->getLocation();
            if (p.x >= m_sliderX0 - 12.f && p.x <= m_sliderX0 + kSliderW + 12.f &&
                std::abs(p.y - m_sliderY) <= 18.f) {
                m_dragging = true;
                applySliderTouch(p.x);
            }
        }
        return true; // swallow everything so the scene behind never sees taps
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (m_dragging) applySliderTouch(t->getLocation().x);
    }
    void ccTouchEnded(CCTouch*, CCEvent*) override { m_dragging = false; }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { m_dragging = false; }

    // ================= dropdown =================
    void setReplayEnabled(bool on) {
        for (auto child : CCArrayExt<CCNode*>(m_replayMenu->getChildren())) {
            if (auto item = typeinfo_cast<CCMenuItem*>(child)) item->setEnabled(on);
        }
        if (!on) m_selBtn->setEnabled(true); // selector stays live so it can close the list
    }

    void clearDropLabels() {
        if (m_emptyLabel) { m_emptyLabel->removeFromParent(); m_emptyLabel = nullptr; }
        if (m_pageLabel) { m_pageLabel->removeFromParent(); m_pageLabel = nullptr; }
    }

    void closeDrop() {
        clearDropLabels();
        if (m_dropMenu) { m_dropMenu->removeFromParent(); m_dropMenu = nullptr; }
        if (m_dropBg) { m_dropBg->removeFromParent(); m_dropBg = nullptr; }
        if (m_replayMenu) setReplayEnabled(true);
    }

    void openDrop() {
        closeDrop();
        m_names = mm::listReplays();

        constexpr int kPer = 3;
        constexpr float rowH = 22.f;
        int total = static_cast<int>(m_names.size());
        int pages = std::max(1, (total + kPer - 1) / kPer);
        m_page = std::clamp(m_page, 0, pages - 1);
        int from = m_page * kPer;
        int count = std::max(0, std::min(kPer, total - from));

        int rows = std::max(count, 1) + 1; // entries (or "empty" line) + footer
        float h = rows * rowH + 10.f;
        float top = m_c.y + 24.f - 16.f;   // just under the selector

        auto bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize({220.f, h});
        bg->setAnchorPoint({0.5f, 1.f});
        bg->setPosition(m_c.x, top);
        m_replay->addChild(bg, 10);
        m_dropBg = bg;

        m_dropMenu = CCMenu::create();
        m_dropMenu->setPosition({0.f, 0.f});
        m_dropMenu->setHandlerPriority(-502);
        m_replay->addChild(m_dropMenu, 11);

        float first = top - 5.f - rowH / 2.f;
        if (count == 0) {
            auto lbl = CCLabelBMFont::create("No replays yet", "bigFont.fnt");
            lbl->setScale(0.4f);
            lbl->setPosition(m_c.x, first);
            m_replay->addChild(lbl, 12);
            m_emptyLabel = lbl;
        }
        for (int k = 0; k < count; k++) {
            auto spr = ButtonSprite::create(
                shortName(m_names[from + k]).c_str(), 190, true,
                "bigFont.fnt", "GJ_button_01.png", 20.f, 0.45f
            );
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MMPanel::onPick));
            btn->setTag(from + k);
            btn->setPosition(m_c.x, first - k * rowH);
            m_dropMenu->addChild(btn);
        }

        float fy = first - (rows - 1) * rowH;
        auto prev = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("<"), this, menu_selector(MMPanel::onPage)
        );
        prev->setTag(0);
        prev->setScale(0.45f);
        prev->setPosition(m_c.x - 70.f, fy);
        m_dropMenu->addChild(prev);

        auto next = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(">"), this, menu_selector(MMPanel::onPage)
        );
        next->setTag(1);
        next->setScale(0.45f);
        next->setPosition(m_c.x + 70.f, fy);
        m_dropMenu->addChild(next);

        auto pageLbl = CCLabelBMFont::create(
            fmt::format("{}/{}", m_page + 1, pages).c_str(), "bigFont.fnt"
        );
        pageLbl->setScale(0.4f);
        pageLbl->setPosition(m_c.x, fy);
        m_replay->addChild(pageLbl, 12);
        m_pageLabel = pageLbl;

        setReplayEnabled(false);
    }

    void onSelector(CCObject*) {
        if (m_dropMenu) closeDrop();
        else openDrop();
    }

    void onPage(CCObject* sender) {
        m_page += static_cast<CCNode*>(sender)->getTag() == 0 ? -1 : 1;
        openDrop();
    }

    void onPick(CCObject* sender) {
        int idx = static_cast<CCNode*>(sender)->getTag();
        if (idx >= 0 && idx < static_cast<int>(m_names.size())) {
            Mod::get()->setSavedValue<std::string>("selected-replay", m_names[idx]);
            m_selSprite->setString(selectorText().c_str());
        }
        closeDrop();
    }

    // ================= other callbacks =================
    void onToggle(CCObject* sender) {
        auto t = static_cast<CCMenuItemToggler*>(sender);
        // callback runs before the visual flips, so the new value is the opposite
        Mod::get()->setSettingValue<bool>(kKeys[t->getTag()], !t->isToggled());
    }

    void onMode(CCObject*) {
        int i = modeIndex(Mod::get()->getSettingValue<std::string>("bot-mode"));
        std::string next = kModes[(i + 1) % 3];
        Mod::get()->setSettingValue<std::string>("bot-mode", next);
        m_modeSprite->setString(fmt::format("Bot: {}", next).c_str());
    }

    void onSave(CCObject*) {
        std::string name;
        if (!mm::saveCurrentReplay(name)) {
            Notification::create(
                "Nothing to save. Set Bot: Record and play a level first.",
                NotificationIcon::Error
            )->show();
            return;
        }
        Mod::get()->setSavedValue<std::string>("selected-replay", name);
        m_selSprite->setString(selectorText().c_str());
        Notification::create("Saved " + name, NotificationIcon::Success)->show();
    }

    void onImport(CCObject*) {
        std::string msg = fmt::format(
            "Copy .mmr replay files into this folder, then tap Refresh:\n{}",
            mm::replayDir()
        );
        FLAlertLayer::create("Import replay", msg, "OK")->show();
    }

    void onRefresh(CCObject*) {
        auto names = mm::listReplays();
        Notification::create(
            fmt::format("{} replay(s) found", names.size()), NotificationIcon::Success
        )->show();
    }

    void onClose(CCObject*) { this->removeFromParent(); }

public:
    static MMPanel* create() {
        auto ret = new (std::nothrow) MMPanel();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------------- floating button ----------------
class MMOverlay : public CCNode {
protected:
    CCMenuItemSpriteExtra* m_btn = nullptr;

    bool init() override {
        if (!CCNode::init()) return false;

        auto win = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);

        auto spr = ButtonSprite::create("MM");
        spr->setScale(0.7f);
        spr->setOpacity(200);
        m_btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(MMOverlay::onOpen)
        );
        m_btn->setPosition({28.f, win.height * 0.5f + 35.f});
        menu->addChild(m_btn);

        this->schedule(schedule_selector(MMOverlay::tick));
        return true;
    }

    // hide inside levels and the editor
    void tick(float) {
        bool inLevel = PlayLayer::get() || LevelEditorLayer::get();
        m_btn->setVisible(!inLevel);
        m_btn->setEnabled(!inLevel);
        if (inLevel) {
            if (auto p = this->getChildByID("mm-panel")) p->removeFromParent();
        }
    }

    void onOpen(CCObject*) {
        if (this->getChildByID("mm-panel")) return;
        auto panel = MMPanel::create();
        panel->setID("mm-panel");
        this->addChild(panel, 100);
    }

public:
    static MMOverlay* create() {
        auto ret = new (std::nothrow) MMOverlay();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// keep one overlay alive and move it into each new scene
class $modify(MMDirector, CCDirector) {
    void willSwitchToScene(CCScene* scene) {
        CCDirector::willSwitchToScene(scene);

        static MMOverlay* overlay = nullptr;
        if (!overlay) {
            overlay = MMOverlay::create();
            if (overlay) overlay->retain();
        }
        if (!overlay || !scene) return;

        overlay->removeFromParentAndCleanup(false);
        scene->addChild(overlay, 100000);
    }
};
