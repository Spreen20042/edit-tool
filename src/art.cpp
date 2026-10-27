// Art Agent: paints your selection with AI-generated art using free services only.
//
//  * "Pollinations (no key)": downloads a generated image and samples it.
//  * "Gemini (free key)":     asks Gemini (free tier, your own key) to describe the art as
//                              simple vector shapes, then rasterizes those shapes locally.
//
// In both cases the result is reduced to a small palette; each palette color becomes a
// color channel in the level, and the selected blocks (or new small blocks inside the
// selected area) get those channels.

#include "art.hpp"

#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float kBlock = 30.f;

    double getF(char const* key) { return Mod::get()->getSettingValue<double>(key); }
    int64_t getI(char const* key) { return Mod::get()->getSettingValue<int64_t>(key); }
    std::string getS(char const* key) { return Mod::get()->getSettingValue<std::string>(key); }

    void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
        Notification::create(text, icon, 2.5f)->show();
    }

    // ---- data -------------------------------------------------------------------------------

    struct Job {
        Ref<EditorUI> ui;
        Ref<LevelEditorLayer> lel;
        std::string prompt;
        std::vector<Ref<GameObject>> objs;
        CCRect area;       // bounding box of the selection, level units
        int gw = 0;        // art canvas size in cells
        int gh = 0;
        float cellW = 15.f;
        float cellH = 15.f;
        std::vector<ccColor3B> canvas; // gw * gh, row 0 = top
    };

    geode::async::TaskHolder<web::WebResponse> s_task;
    bool s_busy = false;

    // ---- small helpers ----------------------------------------------------------------------

    std::string urlEncode(std::string const& s) {
        static char const* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        return out;
    }

    bool parseHex(std::string const& s, ccColor3B& out) {
        std::string h = s;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() != 6) return false;
        try {
            unsigned v = static_cast<unsigned>(std::stoul(h, nullptr, 16));
            out = ccColor3B{static_cast<GLubyte>((v >> 16) & 255),
                            static_cast<GLubyte>((v >> 8) & 255),
                            static_cast<GLubyte>(v & 255)};
            return true;
        } catch (...) {
            return false;
        }
    }

    bool pointInPolygon(float px, float py, std::vector<CCPoint> const& poly) {
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            CCPoint a = poly[i];
            CCPoint b = poly[j];
            if (((a.y > py) != (b.y > py)) &&
                (px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x)) {
                inside = !inside;
            }
        }
        return inside;
    }

    float colorDist2(ccColor3B a, ccColor3B b) {
        float dr = float(a.r) - float(b.r);
        float dg = float(a.g) - float(b.g);
        float db = float(a.b) - float(b.b);
        return dr * dr + dg * dg + db * db;
    }

    // k-means on colors. Returns, for every input color, the index of its palette entry.
    std::vector<int> quantize(std::vector<ccColor3B> const& cols, int k, std::vector<ccColor3B>& palette) {
        palette.clear();
        std::vector<int> assign(cols.size(), 0);
        if (cols.empty()) return assign;
        k = std::max(1, std::min<int>(k, static_cast<int>(cols.size())));

        // Farthest-point initialisation (deterministic).
        palette.push_back(cols[0]);
        while (static_cast<int>(palette.size()) < k) {
            float best = -1.f;
            size_t bestI = 0;
            for (size_t i = 0; i < cols.size(); ++i) {
                float d = 1e30f;
                for (auto const& p : palette) d = std::min(d, colorDist2(cols[i], p));
                if (d > best) { best = d; bestI = i; }
            }
            palette.push_back(cols[bestI]);
        }

        for (int iter = 0; iter < 8; ++iter) {
            for (size_t i = 0; i < cols.size(); ++i) {
                float best = 1e30f;
                for (int c = 0; c < k; ++c) {
                    float d = colorDist2(cols[i], palette[c]);
                    if (d < best) { best = d; assign[i] = c; }
                }
            }
            std::vector<double> sr(k, 0), sg(k, 0), sb(k, 0);
            std::vector<int> cnt(k, 0);
            for (size_t i = 0; i < cols.size(); ++i) {
                int c = assign[i];
                sr[c] += cols[i].r; sg[c] += cols[i].g; sb[c] += cols[i].b; cnt[c]++;
            }
            for (int c = 0; c < k; ++c) {
                if (cnt[c] == 0) continue;
                palette[c] = ccColor3B{static_cast<GLubyte>(sr[c] / cnt[c]),
                                       static_cast<GLubyte>(sg[c] / cnt[c]),
                                       static_cast<GLubyte>(sb[c] / cnt[c])};
            }
        }
        return assign;
    }

    // ---- turning a canvas into level changes ------------------------------------------------

    void defineChannel(LevelEditorLayer* lel, int id, ccColor3B color) {
        GJEffectManager* managers[2] = {
            lel->m_effectManager,
            lel->m_levelSettings ? lel->m_levelSettings->m_effectManager : nullptr
        };
        for (auto em : managers) {
            if (!em) continue;
            auto action = ColorAction::create(color, false, 0);
            action->m_colorID = id;
            em->setColorAction(action, id);
        }
    }

    ccColor3B sampleCanvas(Job const& job, float x, float y) {
        int i = static_cast<int>(std::floor((x - job.area.origin.x) / job.cellW));
        int j = static_cast<int>(std::floor((job.area.origin.y + job.area.size.height - y) / job.cellH));
        i = std::clamp(i, 0, job.gw - 1);
        j = std::clamp(j, 0, job.gh - 1);
        return job.canvas[static_cast<size_t>(j) * job.gw + i];
    }

    void setObjectChannel(GameObject* obj, int channel) {
        if (auto c = obj->m_baseColor) {
            c->m_colorID = channel;
            c->m_defaultColorID = channel;
        }
    }

    void applyCanvas(std::shared_ptr<Job> job) {
        if (!job->ui || !job->lel || !job->ui->getParent()) {
            notify("The editor was closed, art discarded", NotificationIcon::Warning);
            return;
        }

        int firstChannel = static_cast<int>(getI("art-first-channel"));
        int maxColors = static_cast<int>(getI("art-colors"));
        bool newBlocks = getS("art-mode") == "Draw new blocks in area";
        int maxObjects = static_cast<int>(getI("max-objects"));

        // Gather the colors we need to reduce.
        std::vector<ccColor3B> cols;
        std::vector<CCPoint> centers; // level position for each entry in `cols`

        if (newBlocks) {
            // Cells whose center lies inside any selected object.
            for (int j = 0; j < job->gh; ++j) {
                for (int i = 0; i < job->gw; ++i) {
                    float x = job->area.origin.x + (i + 0.5f) * job->cellW;
                    float y = job->area.origin.y + job->area.size.height - (j + 0.5f) * job->cellH;
                    bool inside = false;
                    for (auto const& o : job->objs) {
                        auto r = o->getObjectRect();
                        if (r.size.width < 1.f) r = CCRect(o->getPositionX() - 15, o->getPositionY() - 15, 30, 30);
                        if (r.containsPoint(ccp(x, y))) { inside = true; break; }
                    }
                    if (!inside) continue;
                    centers.push_back(ccp(x, y));
                    cols.push_back(job->canvas[static_cast<size_t>(j) * job->gw + i]);
                }
            }
        } else {
            for (auto const& o : job->objs) {
                centers.push_back(o->getPosition());
                cols.push_back(sampleCanvas(*job, o->getPositionX(), o->getPositionY()));
            }
        }

        if (cols.empty()) {
            notify("Nothing to paint", NotificationIcon::Warning);
            return;
        }
        if (newBlocks && static_cast<int>(cols.size()) > maxObjects) {
            notify("Too many cells, raise Art Cell Size or the object limit", NotificationIcon::Warning);
            return;
        }

        std::vector<ccColor3B> palette;
        auto assign = quantize(cols, maxColors, palette);

        for (size_t c = 0; c < palette.size(); ++c) {
            defineChannel(job->lel, firstChannel + static_cast<int>(c), palette[c]);
        }

        auto changed = CCArray::create();
        if (newBlocks) {
            float sx = job->cellW / kBlock;
            float sy = job->cellH / kBlock;
            for (size_t i = 0; i < centers.size(); ++i) {
                auto obj = job->lel->createObject(1, centers[i], false);
                if (!obj) continue;
                if (std::abs(sx - 1.f) > 0.001f) obj->updateCustomScaleX(sx);
                if (std::abs(sy - 1.f) > 0.001f) obj->updateCustomScaleY(sy);
                setObjectChannel(obj, firstChannel + assign[i]);
                changed->addObject(obj);
            }
        } else {
            for (size_t i = 0; i < job->objs.size(); ++i) {
                setObjectChannel(job->objs[i], firstChannel + assign[i]);
                changed->addObject(job->objs[i]);
            }
        }
        job->lel->updateObjectColors(changed);

        notify(fmt::format("Art done: {} colors, channels {}-{}", palette.size(), firstChannel,
                           firstChannel + static_cast<int>(palette.size()) - 1),
               NotificationIcon::Success);
    }

    void finish(std::shared_ptr<Job> job, bool ok, std::string const& error = "") {
        s_busy = false;
        if (!ok) {
            notify("Art Agent failed: " + error, NotificationIcon::Error);
            return;
        }
        applyCanvas(job);
    }

    // ---- backend 1: Pollinations (image -> canvas) -----------------------------------------

    void onImage(std::shared_ptr<Job> job, web::WebResponse res) {
        if (!res.ok()) return finish(job, false, fmt::format("server answered {}", res.code()));

        auto const& data = res.data();
        auto img = new CCImage();
        if (!img->initWithImageData(const_cast<uint8_t*>(data.data()), static_cast<int>(data.size()))) {
            img->release();
            return finish(job, false, "could not read the image (unsupported format)");
        }

        int W = static_cast<int>(img->getWidth());
        int H = static_cast<int>(img->getHeight());
        unsigned char* px = img->getData();
        int stride = img->hasAlpha() ? 4 : 3;
        if (!px || W < 2 || H < 2) {
            img->release();
            return finish(job, false, "empty image");
        }

        job->canvas.assign(static_cast<size_t>(job->gw) * job->gh, ccColor3B{0, 0, 0});
        for (int j = 0; j < job->gh; ++j) {
            for (int i = 0; i < job->gw; ++i) {
                int x0 = i * W / job->gw, x1 = std::max(x0 + 1, (i + 1) * W / job->gw);
                int y0 = j * H / job->gh, y1 = std::max(y0 + 1, (j + 1) * H / job->gh);
                double r = 0, g = 0, b = 0;
                int n = 0;
                for (int y = y0; y < y1 && y < H; ++y) {
                    for (int x = x0; x < x1 && x < W; ++x) {
                        unsigned char* p = px + (static_cast<size_t>(y) * W + x) * stride;
                        r += p[0]; g += p[1]; b += p[2]; n++;
                    }
                }
                if (n == 0) n = 1;
                job->canvas[static_cast<size_t>(j) * job->gw + i] =
                    ccColor3B{static_cast<GLubyte>(r / n), static_cast<GLubyte>(g / n), static_cast<GLubyte>(b / n)};
            }
        }
        img->release();
        finish(job, true);
    }

    void startPollinations(std::shared_ptr<Job> job) {
        // Ask for an image with the same aspect ratio as the selected area.
        float aspect = job->area.size.height / std::max(1.f, job->area.size.width);
        int w = 512;
        int h = std::clamp(static_cast<int>(512.f * aspect), 128, 1024);

        std::string full = job->prompt +
            ", flat vector illustration, simple bold shapes, solid colors, centered, no text, no watermark";
        std::string url = fmt::format(
            "https://image.pollinations.ai/prompt/{}?width={}&height={}&nologo=true&seed={}",
            urlEncode(full), w, h, static_cast<int>(std::time(nullptr) % 100000)
        );

        s_task.spawn(
            web::WebRequest().userAgent("GeodeEditorTools/0.2").timeout(std::chrono::seconds(90)).get(url),
            [job](web::WebResponse res) { onImage(job, std::move(res)); }
        );
    }

    // ---- backend 2: Gemini (JSON shapes -> canvas) ------------------------------------------

    struct Shape {
        int type = 0; // 0 rect, 1 ellipse, 2 polygon
        float a = 0, b = 0, c = 0, d = 0;
        std::vector<CCPoint> pts;
        ccColor3B color{255, 255, 255};
    };

    std::string stripFences(std::string s) {
        auto first = s.find('{');
        auto last = s.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last < first) return s;
        return s.substr(first, last - first + 1);
    }

    void onGemini(std::shared_ptr<Job> job, web::WebResponse res) {
        if (!res.ok()) {
            std::string why = fmt::format("server answered {}", res.code());
            if (res.code() == 400 || res.code() == 403) why += " (check your Gemini API key)";
            if (res.code() == 429) why += " (free quota reached, try later)";
            return finish(job, false, why);
        }

        auto js = res.json();
        if (!js) return finish(job, false, "bad response from Gemini");
        auto root = std::move(js).unwrap();

        auto& cands = root["candidates"];
        if (!cands.isArray() || cands.size() == 0) return finish(job, false, "Gemini returned no answer");
        auto& parts = cands[0]["content"]["parts"];
        if (!parts.isArray() || parts.size() == 0) return finish(job, false, "Gemini returned an empty answer");
        std::string text = parts[0]["text"].asString().unwrapOr("");

        auto parsed = matjson::parse(stripFences(text));
        if (!parsed) return finish(job, false, "Gemini did not return valid JSON");
        auto art = std::move(parsed).unwrap();

        ccColor3B background{40, 40, 50};
        parseHex(art["background"].asString().unwrapOr(""), background);

        std::vector<Shape> shapes;
        auto& list = art["shapes"];
        if (list.isArray()) {
            for (size_t i = 0; i < list.size() && shapes.size() < 80; ++i) {
                auto& s = list[i];
                Shape sh;
                if (!parseHex(s["color"].asString().unwrapOr(""), sh.color)) continue;
                std::string type = s["type"].asString().unwrapOr("");
                if (type == "rect") {
                    sh.type = 0;
                    sh.a = static_cast<float>(s["x"].asDouble().unwrapOr(0));
                    sh.b = static_cast<float>(s["y"].asDouble().unwrapOr(0));
                    sh.c = static_cast<float>(s["w"].asDouble().unwrapOr(0));
                    sh.d = static_cast<float>(s["h"].asDouble().unwrapOr(0));
                } else if (type == "ellipse") {
                    sh.type = 1;
                    sh.a = static_cast<float>(s["cx"].asDouble().unwrapOr(0));
                    sh.b = static_cast<float>(s["cy"].asDouble().unwrapOr(0));
                    sh.c = static_cast<float>(s["rx"].asDouble().unwrapOr(0));
                    sh.d = static_cast<float>(s["ry"].asDouble().unwrapOr(0));
                } else if (type == "polygon") {
                    sh.type = 2;
                    auto& pts = s["points"];
                    if (!pts.isArray()) continue;
                    for (size_t k = 0; k < pts.size(); ++k) {
                        auto& p = pts[k];
                        if (!p.isArray() || p.size() < 2) continue;
                        sh.pts.push_back(ccp(static_cast<float>(p[0].asDouble().unwrapOr(0)),
                                             static_cast<float>(p[1].asDouble().unwrapOr(0))));
                    }
                    if (sh.pts.size() < 3) continue;
                } else {
                    continue;
                }
                shapes.push_back(std::move(sh));
            }
        }
        if (shapes.empty()) return finish(job, false, "Gemini returned no shapes");

        // The canvas is 100 units wide; cells are square.
        float unit = 100.f / static_cast<float>(job->gw);
        job->canvas.assign(static_cast<size_t>(job->gw) * job->gh, background);
        for (int j = 0; j < job->gh; ++j) {
            for (int i = 0; i < job->gw; ++i) {
                float ux = (i + 0.5f) * unit;
                float uy = (j + 0.5f) * unit;
                auto& out = job->canvas[static_cast<size_t>(j) * job->gw + i];
                for (auto const& sh : shapes) {
                    bool hit = false;
                    if (sh.type == 0) {
                        hit = ux >= sh.a && ux <= sh.a + sh.c && uy >= sh.b && uy <= sh.b + sh.d;
                    } else if (sh.type == 1) {
                        if (sh.c > 0.01f && sh.d > 0.01f) {
                            float nx = (ux - sh.a) / sh.c;
                            float ny = (uy - sh.b) / sh.d;
                            hit = nx * nx + ny * ny <= 1.f;
                        }
                    } else {
                        hit = pointInPolygon(ux, uy, sh.pts);
                    }
                    if (hit) out = sh.color;
                }
            }
        }
        finish(job, true);
    }

    void startGemini(std::shared_ptr<Job> job) {
        std::string key = getS("art-gemini-key");
        if (key.empty()) {
            return finish(job, false, "paste your free Gemini API key in the mod settings");
        }
        std::string model = getS("art-gemini-model");
        if (model.empty()) model = "gemini-2.5-flash";

        float canvasH = 100.f * static_cast<float>(job->gh) / static_cast<float>(job->gw);
        std::string instruction = fmt::format(
            "You are an artist making flat vector art for a video game level. "
            "Canvas: width 100, height {:.1f}, origin at the top-left, y grows downward. "
            "Draw: \"{}\". "
            "Reply with ONLY a JSON object, no markdown, in this exact format: "
            "{{\"background\":\"#RRGGBB\",\"shapes\":[{{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"color\":\"#RRGGBB\"}},"
            "{{\"type\":\"ellipse\",\"cx\":50,\"cy\":50,\"rx\":10,\"ry\":10,\"color\":\"#RRGGBB\"}},"
            "{{\"type\":\"polygon\",\"points\":[[x,y],[x,y],[x,y]],\"color\":\"#RRGGBB\"}}]}}. "
            "Shapes are painted in order, so put large background shapes first and details last. "
            "Use at most 40 shapes and at most {} distinct colors. Fill the whole canvas.",
            canvasH, job->prompt, getI("art-colors")
        );

        std::string body = fmt::format(
            "{{\"contents\":[{{\"parts\":[{{\"text\":{}}}]}}],"
            "\"generationConfig\":{{\"responseMimeType\":\"application/json\",\"temperature\":0.9}}}}",
            matjson::Value(instruction).dump(0)
        );

        web::WebRequest req;
        req.header("x-goog-api-key", key);
        req.header("Content-Type", "application/json");
        req.userAgent("GeodeEditorTools/0.2");
        req.timeout(std::chrono::seconds(90));
        req.bodyString(body);

        std::string url = fmt::format(
            "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model
        );
        s_task.spawn(req.post(url), [job](web::WebResponse res) { onGemini(job, std::move(res)); });
    }

    // ---- entry ------------------------------------------------------------------------------

    void startJob(EditorUI* ui, std::string prompt) {
        if (s_busy) {
            notify("Art Agent is already working", NotificationIcon::Warning);
            return;
        }
        if (prompt.empty()) {
            notify("Write a description first", NotificationIcon::Warning);
            return;
        }

        auto sel = ui->getSelectedObjects();
        if (!sel || sel->count() == 0) {
            notify("Select some blocks first", NotificationIcon::Warning);
            return;
        }

        auto job = std::make_shared<Job>();
        job->ui = ui;
        job->lel = ui->m_editorLayer;
        job->prompt = std::move(prompt);

        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (auto obj : CCArrayExt<GameObject*>(sel)) {
            auto r = obj->getObjectRect();
            if (r.size.width < 1.f) r = CCRect(obj->getPositionX() - 15, obj->getPositionY() - 15, 30, 30);
            minX = std::min(minX, r.getMinX()); maxX = std::max(maxX, r.getMaxX());
            minY = std::min(minY, r.getMinY()); maxY = std::max(maxY, r.getMaxY());
            job->objs.push_back(obj);
        }
        job->area = CCRect(minX, minY, std::max(1.f, maxX - minX), std::max(1.f, maxY - minY));

        float cell = static_cast<float>(getF("art-cell"));
        job->gw = std::clamp(static_cast<int>(std::ceil(job->area.size.width / cell)), 4, 128);
        job->gh = std::clamp(static_cast<int>(std::ceil(job->area.size.height / cell)), 4, 128);
        job->cellW = job->area.size.width / job->gw;
        job->cellH = job->area.size.height / job->gh;

        s_busy = true;
        notify("Art Agent: generating, this can take up to a minute...");
        if (getS("art-backend") == "Gemini (free key)") {
            startGemini(job);
        } else {
            startPollinations(job);
        }
    }

    // ---- popup ------------------------------------------------------------------------------

    class ArtPopup : public Popup {
    protected:
        TextInput* m_input = nullptr;

        bool init(Ref<EditorUI> ui) {
            if (!Popup::init(330.f, 175.f)) return false;
            this->setTitle("Art Agent");

            m_input = TextInput::create(280.f, "Describe the art (any language)");
            m_input->setMaxCharCount(200);
            m_mainLayer->addChildAtPosition(m_input, Anchor::Center, ccp(0, 15));

            auto hint = CCLabelBMFont::create("Paints the blocks you have selected", "chatFont.fnt");
            hint->setScale(.7f);
            hint->setOpacity(160);
            m_mainLayer->addChildAtPosition(hint, Anchor::Center, ccp(0, -18));

            auto spr = ButtonSprite::create("Draw", 80, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .7f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, ui](CCMenuItemSpriteExtra*) {
                // Copy everything we need first: closing the popup destroys this lambda.
                std::string text = static_cast<std::string>(m_input->getString());
                Ref<EditorUI> editor = ui;
                this->onClose(nullptr);
                startJob(editor, text);
            });
            m_buttonMenu->addChildAtPosition(btn, Anchor::Bottom, ccp(0, 35));
            return true;
        }

    public:
        static ArtPopup* create(Ref<EditorUI> ui) {
            auto ret = new ArtPopup();
            if (ret->init(std::move(ui))) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }
    };
}

void artOpenPopup(EditorUI* ui) {
    auto sel = ui->getSelectedObjects();
    if (!sel || sel->count() == 0) {
        notify("Select the blocks you want to paint first", NotificationIcon::Warning);
        return;
    }
    if (auto popup = ArtPopup::create(ui)) popup->show();
}
