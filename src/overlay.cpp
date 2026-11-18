// MiniMenu overlay - UNTESTED. Touch/popup APIs may differ in Geode v6 / GD 2.2091.
#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif
#include <Geode/binding/EndLevelLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/TextInput.hpp>
#include <algorithm>
#include <cmath>
#include "replay.hpp"

using namespace geode::prelude;

namespace {
    constexpr char const* kKeys[4] = {"noclip", "show-hitboxes", "trajectory", "death-delay-custom"};
    constexpr char const* kNames[4] = {"Noclip", "Hitboxes", "Trajectory", "Death Delay"};
    constexpr char const* kHitStyles[3] = {"Outline + Fill", "Outline only", "Fill only"};
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
    std::vector<CCMenu*> m_menus;
    Ref<CCMenuItem> m_pressed;

    // "name your replay" dialog
    CCNode* m_dlg = nullptr;
    CCMenu* m_dlgMenu = nullptr;
    TextInput* m_nameInput = nullptr;

    // tabs
    CCNode* m_main = nullptr;
    CCNode* m_replay = nullptr;
    CCMenu* m_mainMenu = nullptr;
    CCMenu* m_replayMenu = nullptr;
    ButtonSprite* m_tabMainSpr = nullptr;
    ButtonSprite* m_tabReplaySpr = nullptr;

    // main tab: speed slider
    CCLabelBMFont* m_speedLabel = nullptr;
    CCLabelBMFont* m_delayLabel = nullptr;
    ButtonSprite* m_hitSprite = nullptr;
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
    CCLabelBMFont* m_replayInfoLabel = nullptr;

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
        parent->addChild(menu);
        m_menus.push_back(menu); // taps are routed to these menus' items by the panel itself
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
        closeBtn->setPosition(c.x, c.y - 120.f);
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

        for (int i = 0; i < 4; i++) {
            float y = c.y + 72.f - i * 25.f;

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
        m_speedLabel->setPosition(c.x, c.y - 51.f);
        m_main->addChild(m_speedLabel);

        m_sliderX0 = c.x - kSliderW / 2.f;
        m_sliderY = c.y - 72.f;

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
        resetBtn->setPosition(c.x + 112.f, c.y - 51.f);
        m_mainMenu->addChild(resetBtn);

        // custom death delay: value with - / + buttons (the toggle above switches it on)
        m_delayLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_delayLabel->setScale(0.45f);
        m_delayLabel->setPosition(c.x - 25.f, c.y - 27.f);
        m_main->addChild(m_delayLabel);

        auto minusSpr = ButtonSprite::create("-");
        minusSpr->setScale(0.5f);
        auto minus = CCMenuItemSpriteExtra::create(minusSpr, this, menu_selector(MMPanel::onDelayDown));
        minus->setPosition(c.x + 70.f, c.y - 27.f);
        m_mainMenu->addChild(minus);

        auto plusSpr = ButtonSprite::create("+");
        plusSpr->setScale(0.5f);
        auto plus = CCMenuItemSpriteExtra::create(plusSpr, this, menu_selector(MMPanel::onDelayUp));
        plus->setPosition(c.x + 105.f, c.y - 27.f);
        m_mainMenu->addChild(plus);

        // hitbox style: Outline + Fill / Outline only / Fill only
        auto hitStyle = Mod::get()->getSettingValue<std::string>("hitbox-style");
        m_hitSprite = ButtonSprite::create(("Hitbox: " + hitStyle).c_str());
        m_hitSprite->setScale(0.45f);
        auto hitBtn = CCMenuItemSpriteExtra::create(m_hitSprite, this, menu_selector(MMPanel::onHitStyle));
        hitBtn->setPosition(c.x, c.y - 94.f);
        m_mainMenu->addChild(hitBtn);

        refreshSlider();
        refreshDelay();
    }

    void refreshDelay() {
        double d = Mod::get()->getSettingValue<double>("death-delay");
        m_delayLabel->setString(fmt::format("Delay: {:.1f}s", d).c_str());
    }

    void adjustDelay(double step) {
        double d = Mod::get()->getSettingValue<double>("death-delay");
        d = std::clamp(std::round((d + step) * 10.0) / 10.0, 0.0, 5.0);
        Mod::get()->setSettingValue<double>("death-delay", d);
        refreshDelay();
    }

    void onDelayDown(CCObject*) { adjustDelay(-0.1); }
    void onDelayUp(CCObject*) { adjustDelay(0.1); }

    void onHitStyle(CCObject*) {
        auto cur = Mod::get()->getSettingValue<std::string>("hitbox-style");
        int i = 0;
        for (int k = 0; k < 3; k++) if (cur == kHitStyles[k]) i = k;
        std::string next = kHitStyles[(i + 1) % 3];
        Mod::get()->setSettingValue<std::string>("hitbox-style", next);
        m_hitSprite->setString(("Hitbox: " + next).c_str());
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

        m_replayInfoLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_replayInfoLabel->setScale(0.6f);
        m_replayInfoLabel->setPosition(c.x, c.y - 82.f);
        m_replay->addChild(m_replayInfoLabel);
        refreshReplayInfo();

        auto note = CCLabelBMFont::create("Record a run, then Save and name it", "chatFont.fnt");
        note->setScale(0.5f);
        note->setPosition(c.x, c.y - 62.f);
        m_replay->addChild(note);
    }

    // what the currently selected replay file contains (checked before you enter a level)
    void refreshReplayInfo() {
        if (!m_replayInfoLabel) return;
        auto name = selectedName();
        std::string text = name.empty()
            ? std::string("Selected: none")
            : "Selected: " + shortName(name, 18) + " - " + mm::replayInfo(name);
        m_replayInfoLabel->setString(text.c_str());
    }

    // ================= tabs =================
    void showTab(int tab) {
        closeDrop();
        m_main->setVisible(tab == 0);
        m_replay->setVisible(tab == 1);
        m_tabMainSpr->setString(tab == 0 ? "[Main]" : "Main");
        m_tabReplaySpr->setString(tab == 1 ? "[Replay]" : "Replay");
        if (tab == 1) {
            m_infoLabel->setString(mm::recordingInfo().c_str());
            refreshReplayInfo();
        }
    }

    void onTabMain(CCObject*) { showTab(0); }
    void onTabReplay(CCObject*) { showTab(1); }

    // ================= touch =================
    void registerWithTouchDispatcher() override {
        // ahead of FLAlertLayer (-500) so the panel works on top of the pause / complete screens
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -510, true);
    }

    // The panel swallows every touch at priority -500 and hands taps to its own
    // buttons by hand. This avoids touching the dispatcher's per-menu priorities,
    // which crashed on this Geode build.
    bool itemHit(CCMenuItem* item, CCPoint const& p) {
        if (!item->isEnabled()) return false;
        for (CCNode* n = item; n; n = n->getParent()) {
            if (!n->isVisible()) return false;
        }
        auto local = item->convertToNodeSpace(p);
        auto size = item->getContentSize();
        return local.x >= 0.f && local.y >= 0.f && local.x <= size.width && local.y <= size.height;
    }

    CCMenuItem* itemAt(CCPoint const& p) {
        std::vector<CCMenu*> order;
        if (m_dlgMenu) {
            order.push_back(m_dlgMenu); // the dialog is modal: nothing else is clickable
        } else {
            if (m_dropMenu) order.push_back(m_dropMenu); // dropdown sits on top
            for (auto it = m_menus.rbegin(); it != m_menus.rend(); ++it) order.push_back(*it);
        }
        for (auto menu : order) {
            auto kids = menu->getChildren();
            if (!kids) continue;
            for (auto child : CCArrayExt<CCNode*>(kids)) {
                if (auto item = typeinfo_cast<CCMenuItem*>(child)) {
                    if (itemHit(item, p)) return item;
                }
            }
        }
        return nullptr;
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        auto p = t->getLocation();
        if (auto item = itemAt(p)) {
            m_pressed = item;
            item->selected();
            return true;
        }
        if (m_dlg) {
            // tapping the text box focuses it, tapping elsewhere in the dialog releases it
            if (m_nameInput) {
                if (m_nameInput->boundingBox().containsPoint(p)) m_nameInput->focus();
                else m_nameInput->unfocus();
            }
            return true;
        }
        if (m_main && m_main->isVisible()) {
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

    void ccTouchEnded(CCTouch* t, CCEvent*) override {
        m_dragging = false;
        if (m_pressed) {
            Ref<CCMenuItem> item = m_pressed;
            m_pressed = nullptr;
            item->unselected();
            if (itemHit(item, t->getLocation())) item->activate(); // may remove this panel
        }
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_dragging = false;
        if (m_pressed) {
            m_pressed->unselected();
            m_pressed = nullptr;
        }
    }

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
            refreshReplayInfo();
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
        if (next != "Off") {
            Notification::create(
                "Restart the level from the start for " + next + " to take effect",
                NotificationIcon::Info
            )->show();
        }
    }

    // ---- naming dialog ----
    void onSave(CCObject*) {
        if (!mm::hasRecording()) {
            bool recording = Mod::get()->getSettingValue<std::string>("bot-mode") == "Record";
            Notification::create(
                recording
                    ? "No inputs recorded yet. Play the level with a click first."
                    : "Bot is not on Record, so nothing was recorded.",
                NotificationIcon::Error
            )->show();
            return;
        }
        openNameDialog();
    }

    void openNameDialog() {
        if (m_dlg) return;
        closeDrop();

        auto c = m_c;
        auto dlg = CCLayerColor::create({0, 0, 0, 170});
        this->addChild(dlg, 50);
        m_dlg = dlg;

        auto bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({240.f, 130.f});
        bg->setPosition(c);
        dlg->addChild(bg);

        auto title = CCLabelBMFont::create("Name your replay", "goldFont.fnt");
        title->setScale(0.6f);
        title->setPosition(c.x, c.y + 42.f);
        dlg->addChild(title);

        m_nameInput = TextInput::create(190.f, "name", "bigFont.fnt");
        m_nameInput->setMaxCharCount(32);
        m_nameInput->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ");
        m_nameInput->setPosition(c.x, c.y + 8.f);
        dlg->addChild(m_nameInput);

        m_dlgMenu = CCMenu::create();
        m_dlgMenu->setPosition({0.f, 0.f});
        dlg->addChild(m_dlgMenu);

        auto okSpr = ButtonSprite::create("Save");
        okSpr->setScale(0.55f);
        auto ok = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(MMPanel::onDlgOk));
        ok->setPosition(c.x - 50.f, c.y - 38.f);
        m_dlgMenu->addChild(ok);

        auto cancelSpr = ButtonSprite::create("Cancel");
        cancelSpr->setScale(0.55f);
        auto cancel = CCMenuItemSpriteExtra::create(cancelSpr, this, menu_selector(MMPanel::onDlgCancel));
        cancel->setPosition(c.x + 50.f, c.y - 38.f);
        m_dlgMenu->addChild(cancel);
    }

    void closeNameDialog() {
        if (m_nameInput) m_nameInput->unfocus();
        if (m_dlg) m_dlg->removeFromParent();
        m_dlg = nullptr;
        m_dlgMenu = nullptr;
        m_nameInput = nullptr;
    }

    void onDlgOk(CCObject*) {
        if (!m_nameInput) return;
        std::string name = m_nameInput->getString();
        std::string err;
        if (!mm::saveReplayAs(name, err)) {
            Notification::create(err, NotificationIcon::Error)->show();
            return; // keep the dialog open so the name can be fixed
        }
        Mod::get()->setSavedValue<std::string>("selected-replay", name);
        m_selSprite->setString(selectorText().c_str());
        Notification::create("Saved \"" + name + "\"", NotificationIcon::Success)->show();
        if (mm::recordingBroken()) {
            Notification::create(
                "This run lost sync at a checkpoint, so it may not replay correctly.",
                NotificationIcon::Warning
            )->show();
        }
        if (m_infoLabel) m_infoLabel->setString(mm::recordingInfo().c_str());
        refreshReplayInfo();
        closeNameDialog();
    }

    void onDlgCancel(CCObject*) { closeNameDialog(); }

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
        refreshReplayInfo();
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
// A touch layer rather than a menu: it only claims touches that start on the
// button, so everything else passes through, and it can tell a tap from a drag.
class MMOverlay : public CCLayer {
protected:
    ButtonSprite* m_btn = nullptr;
    bool m_down = false;
    bool m_dragged = false;
    CCPoint m_start;
    CCPoint m_grab;
    int m_hold = 0; // frames to stay hidden after gameplay

    static CCPoint clampPos(CCPoint p) {
        auto win = CCDirector::get()->getWinSize();
        p.x = std::clamp(p.x, 20.f, win.width - 20.f);
        p.y = std::clamp(p.y, 20.f, win.height - 20.f);
        return p;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        this->setTouchEnabled(true);

        auto win = CCDirector::get()->getWinSize();
        m_btn = ButtonSprite::create("MM");
        m_btn->setScale(0.7f);
        m_btn->setOpacity(200);
        float x = Mod::get()->getSavedValue<float>("btn-x", 28.f);
        float y = Mod::get()->getSavedValue<float>("btn-y", win.height * 0.5f + 35.f);
        m_btn->setPosition(clampPos(ccp(x, y)));
        this->addChild(m_btn);

        this->schedule(schedule_selector(MMOverlay::tick));
        s_instance = this;
        updateVisibility(CCDirector::get()->getRunningScene());
        return true;
    }

    // is the pause menu or the level-complete screen open?
    static bool levelMenuOpen() {
        auto scene = CCDirector::get()->getRunningScene();
        if (scene) {
            if (scene->getChildByType<PauseLayer>(0)) return true;
            if (scene->getChildByType<EndLevelLayer>(0)) return true;
        }
        if (auto pl = PlayLayer::get()) {
            if (pl->getChildByType<EndLevelLayer>(0)) return true;
        }
        return false;
    }

    // Is this node, or something just below it, a level? It looks through transition scenes too,
    // so the button is already hidden while a level loads or fades out, moments when
    // PlayLayer::get() is not reliable.
    static bool nodeHasLevel(CCNode* n, int depth = 0) {
        if (!n || depth > 2) return false;
        if (typeinfo_cast<PlayLayer*>(n) || typeinfo_cast<LevelEditorLayer*>(n)) return true;
        auto kids = n->getChildren();
        if (!kids) return false;
        for (auto k : CCArrayExt<CCNode*>(kids)) {
            if (k != s_instance && nodeHasLevel(k, depth + 1)) return true;
        }
        return false;
    }

    // Android: visible in menus, on the pause menu and on the level-complete screen; hidden
    // everywhere else in a level (gameplay, loading, fading out, the editor).
    // Windows: never shown, Tab opens the menu instead.
    void updateVisibility(CCScene* scene) {
        if (!m_btn) return;
        bool inLevel = PlayLayer::get() || LevelEditorLayer::get() || nodeHasLevel(scene);
        bool menuOpen = inLevel && levelMenuOpen();

        // stay hidden for a short moment after gameplay ends, so a one-frame gap in the checks
        // above (level start / exit) can never flash the button
        bool gameplay = inLevel && !menuOpen;
        if (gameplay) m_hold = 20;
        else if (!menuOpen && m_hold > 0) m_hold--;

        bool show = !gameplay && (menuOpen || m_hold == 0);
#ifdef GEODE_IS_WINDOWS
        show = false;
#endif
        auto panel = this->getChildByID("mm-panel");
        if (panel) show = false;
        m_btn->setVisible(show);
#ifndef GEODE_IS_WINDOWS
        // never leave the panel up over live gameplay on Android
        if (panel && gameplay) panel->removeFromParent();
#endif
    }

    void tick(float) { updateVisibility(CCDirector::get()->getRunningScene()); }

    void registerWithTouchDispatcher() override {
        // ahead of FLAlertLayer (-500): the pause and complete screens would otherwise swallow our taps
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -505, true);
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!m_btn->isVisible()) return false;
        auto p = t->getLocation();
        auto r = m_btn->boundingBox();
        r.origin.x -= 8.f;
        r.origin.y -= 8.f;
        r.size.width += 16.f;
        r.size.height += 16.f;
        if (!r.containsPoint(p)) return false; // not ours: let the game have it

        m_down = true;
        m_dragged = false;
        m_start = p;
        m_grab = ccp(m_btn->getPositionX() - p.x, m_btn->getPositionY() - p.y);
        m_btn->setScale(0.8f);
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (!m_down) return;
        auto p = t->getLocation();
        if (!m_dragged) {
            float dx = p.x - m_start.x, dy = p.y - m_start.y;
            if (dx * dx + dy * dy > 100.f) m_dragged = true; // moved more than 10 units
        }
        if (m_dragged) m_btn->setPosition(clampPos(ccp(p.x + m_grab.x, p.y + m_grab.y)));
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (!m_down) return;
        m_down = false;
        m_btn->setScale(0.7f);
        if (m_dragged) {
            Mod::get()->setSavedValue<float>("btn-x", m_btn->getPositionX());
            Mod::get()->setSavedValue<float>("btn-y", m_btn->getPositionY());
        } else {
            openPanel();
        }
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_down = false;
        m_btn->setScale(0.7f);
    }

    void openPanel() {
        if (this->getChildByID("mm-panel")) return;
        auto panel = MMPanel::create();
        panel->setID("mm-panel");
        this->addChild(panel, 100);
    }

public:
    inline static MMOverlay* s_instance = nullptr;

    // called as a scene is about to be shown, before the next tick runs
    void refresh(CCScene* scene) { updateVisibility(scene); }

    // Windows: Tab opens / closes the menu everywhere, including gameplay
    static void togglePanel() {
        if (!s_instance) return;
        if (auto p = s_instance->getChildByID("mm-panel")) p->removeFromParent();
        else s_instance->openPanel();
    }

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
        overlay->refresh(scene); // hide at once if this scene is a level
    }
};

#ifdef GEODE_IS_WINDOWS
// Tab toggles the menu on Windows, in menus and in levels
class $modify(MMKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool isKeyRepeat, double timestamp) {
        if (key == KEY_Tab && down && !isKeyRepeat) {
            MMOverlay::togglePanel();
            return true; // consumed
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, isKeyRepeat, timestamp);
    }
};
#endif
