// MiniMenu overlay - UNTESTED. Touch/popup/SceneManager APIs may differ in Geode v6 / GD 2.2091.
#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

namespace {
    constexpr char const* kKeys[3] = {"noclip", "show-hitboxes", "trajectory"};
    constexpr char const* kNames[3] = {"Noclip", "Hitboxes", "Trajectory"};
    constexpr char const* kModes[3] = {"Off", "Record", "Replay"};

    int modeIndex(std::string const& m) {
        for (int i = 0; i < 3; i++) if (m == kModes[i]) return i;
        return 0;
    }
}

// ---------------- settings panel ----------------
class MMPanel : public CCLayerColor {
protected:
    ButtonSprite* m_modeSprite = nullptr;

    bool init() override {
        if (!CCLayerColor::initWithColor({0, 0, 0, 150})) return false;
        this->setTouchEnabled(true);

        auto win = CCDirector::get()->getWinSize();
        auto c = win / 2;

        auto bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({260.f, 210.f});
        bg->setPosition(c);
        this->addChild(bg);

        auto title = CCLabelBMFont::create("MiniMenu", "goldFont.fnt");
        title->setScale(0.8f);
        title->setPosition(c.x, c.y + 88.f);
        this->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);

        // three bool toggles
        for (int i = 0; i < 3; i++) {
            float y = c.y + 50.f - i * 35.f;

            auto label = CCLabelBMFont::create(kNames[i], "bigFont.fnt");
            label->setScale(0.55f);
            label->setAnchorPoint({0.f, 0.5f});
            label->setPosition(c.x - 105.f, y);
            this->addChild(label);

            auto tog = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(MMPanel::onToggle), 0.7f
            );
            tog->setTag(i);
            tog->setPosition(c.x + 95.f, y);
            tog->toggle(Mod::get()->getSettingValue<bool>(kKeys[i]));
            menu->addChild(tog);
        }

        // replay bot mode cycle button
        auto mode = Mod::get()->getSettingValue<std::string>("bot-mode");
        m_modeSprite = ButtonSprite::create(
            fmt::format("Bot: {}", mode).c_str()
        );
        m_modeSprite->setScale(0.6f);
        auto modeBtn = CCMenuItemSpriteExtra::create(
            m_modeSprite, this, menu_selector(MMPanel::onMode)
        );
        modeBtn->setPosition(c.x, c.y - 55.f);
        menu->addChild(modeBtn);

        // close
        auto closeSpr = ButtonSprite::create("Close");
        closeSpr->setScale(0.6f);
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSpr, this, menu_selector(MMPanel::onClose)
        );
        closeBtn->setPosition(c.x, c.y - 88.f);
        menu->addChild(closeBtn);

        // menu must sit above the panel's touch-swallowing priority
        menu->setHandlerPriority(-501);
        return true;
    }

    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }

    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }

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
        m_btn->setPosition({28.f, win.height * 0.5f}); // fixed: left edge, middle
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

// create the overlay once, after the game is up
class $modify(MMMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        static bool created = false;
        if (!created) {
            created = true;
            if (auto o = MMOverlay::create()) {
                SceneManager::get()->keepAcrossScenes(o);
            }
        }
        return true;
    }
};
