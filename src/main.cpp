// Editor Tools - free Geometry Dash editor helpers for Geode.
// Original code. Not affiliated with, and does not use code from, any other editor mod.

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

#include "art.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <random>
#include <vector>

using namespace geode::prelude;

namespace {
    enum class Tool { None, Scatter, Polygon, Lasso };

    constexpr float kBlock = 30.f;           // size of a basic block in editor units
    constexpr float kPi = 3.14159265358979f;

    std::mt19937& rng() {
        static std::mt19937 gen{std::random_device{}()};
        return gen;
    }

    float randf(float a, float b) {
        if (b <= a) return a;
        return std::uniform_real_distribution<float>(a, b)(rng());
    }

    double getF(char const* key) { return Mod::get()->getSettingValue<double>(key); }
    int64_t getI(char const* key) { return Mod::get()->getSettingValue<int64_t>(key); }

    float dist(CCPoint a, CCPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }

    void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 1.8f)->show();
    }

    // Chaikin corner cutting on a closed polygon: each pass rounds the corners.
    std::vector<CCPoint> chaikin(std::vector<CCPoint> pts, int iterations) {
        for (int k = 0; k < iterations && pts.size() >= 3; ++k) {
            std::vector<CCPoint> out;
            out.reserve(pts.size() * 2);
            for (size_t i = 0; i < pts.size(); ++i) {
                CCPoint a = pts[i];
                CCPoint b = pts[(i + 1) % pts.size()];
                out.push_back(ccp(a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f));
                out.push_back(ccp(a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f));
            }
            pts = std::move(out);
        }
        return pts;
    }

    // Keep only points that are at least `minDist` apart (used to clean up the lasso path).
    std::vector<CCPoint> resample(std::vector<CCPoint> const& pts, float minDist) {
        std::vector<CCPoint> out;
        for (auto const& p : pts) {
            if (out.empty() || dist(out.back(), p) >= minDist) out.push_back(p);
        }
        return out;
    }

    bool pointInPolygon(CCPoint p, std::vector<CCPoint> const& poly) {
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            CCPoint a = poly[i];
            CCPoint b = poly[j];
            if (((a.y > p.y) != (b.y > p.y)) &&
                (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
                inside = !inside;
            }
        }
        return inside;
    }

    GameObject* spawn(LevelEditorLayer* lel, int id, CCPoint pos, float sx, float sy, float rot) {
        auto obj = lel->createObject(id, pos, false); // false = register in the undo list
        if (!obj) return nullptr;
        if (std::abs(sx - 1.f) > 0.001f) obj->updateCustomScaleX(sx);
        if (std::abs(sy - 1.f) > 0.001f) obj->updateCustomScaleY(sy);
        if (std::abs(rot) > 0.001f) obj->setRotation(rot);
        return obj;
    }

    // The object that gets sprayed / used for outlines and fills:
    // the first selected object in the editor, or a basic block (ID 1) if nothing is selected.
    int pickObjectID(EditorUI* ui) {
        if (auto sel = ui->getSelectedObjects()) {
            if (sel->count() > 0) {
                if (auto obj = typeinfo_cast<GameObject*>(sel->objectAtIndex(0))) {
                    return obj->m_objectID;
                }
            }
        }
        return 1;
    }
}

class $modify(ETEditorUI, EditorUI) {
    struct Fields {
        Tool tool = Tool::None;
        CCTouch* active = nullptr;
        CCDrawNode* preview = nullptr;
        CCMenu* menu = nullptr;      // always visible: the "Tools" toggle and the Done button
        CCMenu* listMenu = nullptr;  // the drop-down list, hidden until opened
        ButtonSprite* mainSpr = nullptr;
        bool open = false;
        CCMenuItemSpriteExtra* doneBtn = nullptr;
        std::array<ButtonSprite*, 3> toolSprites{nullptr, nullptr, nullptr};
        std::vector<CCPoint> pts;
        CCPoint lastScatter = ccp(0, 0);
        int templateID = 1;
        int placed = 0;
    };

    // ---- setup ------------------------------------------------------------------------------

    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;

        auto f = m_fields.self();

        f->preview = CCDrawNode::create();
        lel->m_objectLayer->addChild(f->preview, 9999);

        f->menu = CCMenu::create();
        f->menu->setPosition(ccp(0, 0));
        this->addChild(f->menu, 100);

        float x = static_cast<float>(getI("menu-x"));
        float y = static_cast<float>(getI("menu-y"));
        float const step = 26.f;

        // One small "Tools" button is all that is on screen by default.
        f->mainSpr = ButtonSprite::create("Tools", 62, true, "bigFont.fnt", "GJ_button_04.png", 22.f, .5f);
        auto mainBtn = CCMenuItemExt::createSpriteExtra(f->mainSpr, [this](CCMenuItemSpriteExtra*) {
            this->etSetOpen(!m_fields->open);
        });
        mainBtn->setPosition(ccp(x, y));
        f->menu->addChild(mainBtn);

        // The list drops down under it and is hidden again after picking something.
        f->listMenu = CCMenu::create();
        f->listMenu->setPosition(ccp(0, 0));
        f->listMenu->setVisible(false);
        this->addChild(f->listMenu, 100);

        auto addToolBtn = [&](char const* text, Tool tool, int slot, float yy) {
            auto spr = ButtonSprite::create(text, 62, true, "bigFont.fnt", "GJ_button_04.png", 22.f, .5f);
            f->toolSprites[slot] = spr;
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, tool](CCMenuItemSpriteExtra*) {
                this->etSetOpen(false);
                this->etSetTool(tool);
            });
            btn->setPosition(ccp(x, yy));
            f->listMenu->addChild(btn);
        };
        addToolBtn("Scatter", Tool::Scatter, 0, y - step);
        addToolBtn("Polygon", Tool::Polygon, 1, y - step * 2);
        addToolBtn("Lasso", Tool::Lasso, 2, y - step * 3);

        auto shotSpr = ButtonSprite::create("Shot", 62, true, "bigFont.fnt", "GJ_button_04.png", 22.f, .5f);
        auto shotBtn = CCMenuItemExt::createSpriteExtra(shotSpr, [this](CCMenuItemSpriteExtra*) {
            this->etSetOpen(false);
            this->etScreenshot();
        });
        shotBtn->setPosition(ccp(x, y - step * 4));
        f->listMenu->addChild(shotBtn);

        auto artSpr = ButtonSprite::create("Art AI", 62, true, "bigFont.fnt", "GJ_button_04.png", 22.f, .5f);
        auto artBtn = CCMenuItemExt::createSpriteExtra(artSpr, [this](CCMenuItemSpriteExtra*) {
            this->etSetOpen(false);
            artOpenPopup(this);
        });
        artBtn->setPosition(ccp(x, y - step * 5));
        f->listMenu->addChild(artBtn);

        // "Done" for the polygon tool sits next to the Tools button, only while it is needed.
        auto doneSpr = ButtonSprite::create("Done", 62, true, "bigFont.fnt", "GJ_button_01.png", 22.f, .5f);
        f->doneBtn = CCMenuItemExt::createSpriteExtra(doneSpr, [this](CCMenuItemSpriteExtra*) {
            this->etFinishPolygon();
        });
        f->doneBtn->setPosition(ccp(x + 68.f, y));
        f->doneBtn->setVisible(false);
        f->menu->addChild(f->doneBtn);

        return true;
    }

    void etSetOpen(bool open) {
        auto f = m_fields.self();
        f->open = open;
        f->listMenu->setVisible(open);
        this->etRefreshMain();
    }

    // The Tools button is blue while open, green while a tool is active, gray otherwise.
    void etRefreshMain() {
        auto f = m_fields.self();
        char const* bg = f->open ? "GJ_button_02.png"
                                 : (f->tool != Tool::None ? "GJ_button_01.png" : "GJ_button_04.png");
        f->mainSpr->updateBGImage(bg);
    }

    // ---- tool state -------------------------------------------------------------------------

    void etSetTool(Tool tool) {
        auto f = m_fields.self();
        f->tool = (f->tool == tool) ? Tool::None : tool;
        f->pts.clear();
        f->active = nullptr;
        f->preview->clear();

        for (int i = 0; i < 3; ++i) {
            bool on = (static_cast<int>(f->tool) == i + 1);
            if (f->toolSprites[i]) {
                f->toolSprites[i]->updateBGImage(on ? "GJ_button_01.png" : "GJ_button_04.png");
            }
        }
        f->doneBtn->setVisible(f->tool == Tool::Polygon);
        this->etRefreshMain();

        switch (f->tool) {
            case Tool::Scatter: notify("Scatter Brush: select an object, then drag"); break;
            case Tool::Polygon: notify("Polygon: tap points, then press Done"); break;
            case Tool::Lasso: notify("Lasso Fill: draw a closed outline"); break;
            default: break;
        }
    }

    CCPoint etLevelPoint(CCTouch* touch) {
        return m_editorLayer->m_objectLayer->convertToNodeSpace(touch->getLocation());
    }

    bool etLimitReached(int extra = 1) {
        auto f = m_fields.self();
        return f->placed + extra > static_cast<int>(getI("max-objects"));
    }

    // ---- scatter brush ----------------------------------------------------------------------

    void etScatterAt(CCPoint p) {
        auto f = m_fields.self();
        float radius = static_cast<float>(getF("scatter-radius"));
        int count = static_cast<int>(getI("scatter-count"));
        float smin = static_cast<float>(getF("scatter-min-scale"));
        float smax = static_cast<float>(getF("scatter-max-scale"));
        bool rotate = Mod::get()->getSettingValue<bool>("scatter-rotate");

        for (int i = 0; i < count; ++i) {
            if (this->etLimitReached()) {
                notify("Object limit reached for this stroke", NotificationIcon::Warning);
                return;
            }
            float ang = randf(0.f, 2.f * kPi);
            float rad = radius * std::sqrt(randf(0.f, 1.f));
            CCPoint pos = ccp(p.x + std::cos(ang) * rad, p.y + std::sin(ang) * rad);
            float s = randf(smin, smax);
            float rot = rotate ? randf(0.f, 360.f) : 0.f;
            if (spawn(m_editorLayer, f->templateID, pos, s, s, rot)) f->placed++;
        }
    }

    // ---- polygon / fill ---------------------------------------------------------------------

    void etDrawPreview(bool closed) {
        auto f = m_fields.self();
        f->preview->clear();
        ccColor4F line{1.f, 0.85f, 0.2f, 0.9f};
        ccColor4F dot{1.f, 1.f, 1.f, 1.f};
        for (size_t i = 0; i < f->pts.size(); ++i) {
            if (i + 1 < f->pts.size()) f->preview->drawSegment(f->pts[i], f->pts[i + 1], 1.2f, line);
            if (f->tool == Tool::Polygon) f->preview->drawDot(f->pts[i], 3.f, dot);
        }
        if (closed && f->pts.size() > 2) {
            f->preview->drawSegment(f->pts.back(), f->pts.front(), 1.2f, ccColor4F{1.f, 0.85f, 0.2f, 0.4f});
        }
    }

    void etFillPolygon(std::vector<CCPoint> const& poly, int id, float step) {
        auto f = m_fields.self();
        if (poly.size() < 3 || step < 1.f) return;

        float minX = poly[0].x, maxX = poly[0].x, minY = poly[0].y, maxY = poly[0].y;
        for (auto const& p : poly) {
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }

        // Cells are centered on a grid aligned with the editor grid, so neighbouring fills line up.
        int ix0 = static_cast<int>(std::floor(minX / step));
        int ix1 = static_cast<int>(std::floor(maxX / step));
        int iy0 = static_cast<int>(std::floor(minY / step));
        int iy1 = static_cast<int>(std::floor(maxY / step));
        long long cells = static_cast<long long>(ix1 - ix0 + 1) * (iy1 - iy0 + 1);
        if (cells > 400000) {
            notify("Area too big: raise Fill Cell Size in the mod settings", NotificationIcon::Warning);
            return;
        }

        float scale = step / kBlock;
        for (int iy = iy0; iy <= iy1; ++iy) {
            for (int ix = ix0; ix <= ix1; ++ix) {
                CCPoint c = ccp((ix + 0.5f) * step, (iy + 0.5f) * step);
                if (!pointInPolygon(c, poly)) continue;
                if (this->etLimitReached()) {
                    notify("Object limit reached, fill is incomplete", NotificationIcon::Warning);
                    return;
                }
                if (spawn(m_editorLayer, id, c, scale, scale, 0.f)) f->placed++;
            }
        }
    }

    void etBuildOutline(std::vector<CCPoint> const& poly, int id, float thickness) {
        auto f = m_fields.self();
        for (size_t i = 0; i < poly.size(); ++i) {
            CCPoint a = poly[i];
            CCPoint b = poly[(i + 1) % poly.size()];
            float len = dist(a, b);
            if (len < 0.5f) continue;
            if (this->etLimitReached()) {
                notify("Object limit reached, outline is incomplete", NotificationIcon::Warning);
                return;
            }
            CCPoint mid = ccp((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            // GD rotation is clockwise, math angle is counter-clockwise.
            float angle = -std::atan2(b.y - a.y, b.x - a.x) * 180.f / kPi;
            // A little overlap (+thickness) avoids gaps at the joints.
            float sx = (len + thickness * 0.5f) / kBlock;
            float sy = thickness / kBlock;
            if (spawn(m_editorLayer, id, mid, sx, sy, angle)) f->placed++;
        }
    }

    void etFinishPolygon() {
        auto f = m_fields.self();
        if (f->tool != Tool::Polygon) return;
        if (f->pts.size() < 3) {
            notify("Need at least 3 points", NotificationIcon::Warning);
            return;
        }

        int id = pickObjectID(this);
        f->placed = 0;
        auto poly = chaikin(f->pts, static_cast<int>(getI("polygon-smoothing")));

        if (Mod::get()->getSettingValue<std::string>("polygon-mode") == "Filled") {
            this->etFillPolygon(poly, id, static_cast<float>(getF("fill-step")));
        }
        this->etBuildOutline(poly, id, static_cast<float>(getF("polygon-thickness")));

        notify(fmt::format("Polygon created ({} objects)", f->placed), NotificationIcon::Success);
        f->pts.clear();
        f->preview->clear();
    }

    void etFinishLasso() {
        auto f = m_fields.self();
        auto path = resample(f->pts, 4.f);
        f->pts.clear();
        f->preview->clear();
        if (path.size() < 3) return;

        // Rough / Smooth / Smoother / Smoothest.
        int smoothing = static_cast<int>(getI("lasso-smoothing"));
        if (path.size() > 150) path = resample(path, 12.f);
        path = chaikin(path, smoothing);

        int id = pickObjectID(this);
        f->placed = 0;
        this->etFillPolygon(path, id, static_cast<float>(getF("fill-step")));
        notify(fmt::format("Lasso fill done ({} objects)", f->placed), NotificationIcon::Success);
    }

    // ---- screenshot -------------------------------------------------------------------------

    void etScreenshot() {
        auto f = m_fields.self();
        auto lel = m_editorLayer;
        auto win = CCDirector::get()->getWinSize();

        auto rt = CCRenderTexture::create(static_cast<int>(win.width), static_cast<int>(win.height));
        if (!rt) {
            notify("Screenshot failed", NotificationIcon::Error);
            return;
        }

        bool gridWasVisible = lel->m_drawGridLayer ? lel->m_drawGridLayer->isVisible() : false;
        if (lel->m_drawGridLayer) lel->m_drawGridLayer->setVisible(false);
        f->preview->setVisible(false);

        rt->begin();
        lel->visit();
        rt->end();

        if (lel->m_drawGridLayer) lel->m_drawGridLayer->setVisible(gridWasVisible);
        f->preview->setVisible(true);

        auto dir = Mod::get()->getSaveDir() / "screenshots";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        auto path = dir / fmt::format("screenshot-{}.png", stamp);

        if (rt->saveToFile(path.string().c_str())) {
            notify("Screenshot saved to the mod's screenshots folder", NotificationIcon::Success);
            log::info("Screenshot saved: {}", path.string());
        } else {
            notify("Could not save the screenshot", NotificationIcon::Error);
        }
    }

    // ---- input ------------------------------------------------------------------------------

    bool etOnToolbar(CCTouch* touch) {
        return touch->getLocation().y <= m_toolbarHeight;
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        auto f = m_fields.self();
        if (f->tool == Tool::None || f->active || this->etOnToolbar(touch)) {
            return EditorUI::ccTouchBegan(touch, event);
        }

        f->active = touch;
        CCPoint p = this->etLevelPoint(touch);

        switch (f->tool) {
            case Tool::Scatter:
                f->templateID = pickObjectID(this);
                f->placed = 0;
                f->lastScatter = p;
                this->etScatterAt(p);
                break;
            case Tool::Polygon:
                f->pts.push_back(p);
                this->etDrawPreview(true);
                break;
            case Tool::Lasso:
                f->pts.clear();
                f->pts.push_back(p);
                this->etDrawPreview(true);
                break;
            default: break;
        }
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent* event) {
        auto f = m_fields.self();
        if (f->tool == Tool::None || f->active != touch) {
            return EditorUI::ccTouchMoved(touch, event);
        }

        CCPoint p = this->etLevelPoint(touch);
        switch (f->tool) {
            case Tool::Scatter:
                if (dist(p, f->lastScatter) >= static_cast<float>(getF("scatter-spacing"))) {
                    f->lastScatter = p;
                    this->etScatterAt(p);
                }
                break;
            case Tool::Polygon:
                // Drag to fine-tune the point you just placed.
                if (!f->pts.empty()) f->pts.back() = p;
                this->etDrawPreview(true);
                break;
            case Tool::Lasso:
                if (f->pts.empty() || dist(f->pts.back(), p) >= 3.f) f->pts.push_back(p);
                this->etDrawPreview(true);
                break;
            default: break;
        }
    }

    void ccTouchEnded(CCTouch* touch, CCEvent* event) {
        auto f = m_fields.self();
        if (f->tool == Tool::None || f->active != touch) {
            return EditorUI::ccTouchEnded(touch, event);
        }
        f->active = nullptr;
        if (f->tool == Tool::Lasso) this->etFinishLasso();
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* event) {
        auto f = m_fields.self();
        if (f->tool == Tool::None || f->active != touch) {
            return EditorUI::ccTouchCancelled(touch, event);
        }
        f->active = nullptr;
        if (f->tool == Tool::Lasso) {
            f->pts.clear();
            f->preview->clear();
        }
    }

    void keyDown(enumKeyCodes key, double timestamp) {
        auto f = m_fields.self();
        auto name = Mod::get()->getSettingValue<std::string>("screenshot-key");

        enumKeyCodes shotKey = KEY_F2;
        if (name == "F3") shotKey = KEY_F3;
        else if (name == "F4") shotKey = KEY_F4;
        else if (name == "F8") shotKey = KEY_F8;
        else if (name == "P") shotKey = KEY_P;
        else if (name == "O") shotKey = KEY_O;

        if (key == shotKey) {
            this->etScreenshot();
            return;
        }
        if (key == KEY_Enter && f->tool == Tool::Polygon && !f->pts.empty()) {
            this->etFinishPolygon();
            return;
        }
        EditorUI::keyDown(key, timestamp);
    }
};
