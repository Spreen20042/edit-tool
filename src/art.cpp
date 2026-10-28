// Art Agent: decorates your selection with AI art, using free services only.
//
// The pipeline mirrors a professional art agent:
//   1. Shape   - reads the silhouette of the selected blocks.
//   2. Plan    - (optional) a language model turns your prompt into a detailed drawing brief.
//   3. Drawing - an image model (Cloudflare FLUX, free) or a language model that draws
//                vector shapes (Gemini / Groq / OpenRouter, free) makes the picture.
//   4. Trace   - the picture is reduced to a small palette and merged into as few
//                rectangles as possible.
//   5. Place   - the rectangles become real blocks with their own color channels,
//                clipped to the shape of your selection.

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
    bool getB(char const* key) { return Mod::get()->getSettingValue<bool>(key); }
    std::string getS(char const* key) { return Mod::get()->getSettingValue<std::string>(key); }

    void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
        float seconds = icon == NotificationIcon::Error ? 8.f : 2.5f;
        Notification::create(text, icon, seconds)->show();
    }

    // ---- data -------------------------------------------------------------------------------

    // One shape of a language model's drawing, in canvas units (100 wide, y grows downward).
    struct Shape {
        int type = 0; // 0 rect, 1 ellipse, 2 polygon
        float a = 0, b = 0, c = 0, d = 0;
        std::vector<CCPoint> pts;
        ccColor3B color{255, 255, 255};
    };

    struct Job {
        std::vector<Shape> shapes;     // only filled by the language-model drawing backends
        Ref<EditorUI> ui;
        Ref<LevelEditorLayer> lel;
        std::string prompt;
        std::vector<Ref<GameObject>> objs;
        std::vector<CCRect> rects;     // area covered by the selection (plus overhang)
        CCRect area;                   // bounding box of the selection, level units
        int gw = 0;                    // canvas size in cells
        int gh = 0;
        float cellW = 7.5f;
        float cellH = 7.5f;
        std::vector<ccColor3B> canvas; // gw * gh, row 0 = top
    };

    geode::async::TaskHolder<web::WebResponse> s_task;
    geode::async::TaskHolder<web::WebResponse> s_planTask;
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

    std::vector<uint8_t> base64Decode(std::string const& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size() * 3 / 4);
        uint32_t buf = 0;
        int bits = 0;
        for (unsigned char c : in) {
            int v;
            if (c >= 'A' && c <= 'Z') v = c - 'A';
            else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
            else if (c >= '0' && c <= '9') v = c - '0' + 52;
            else if (c == '+' || c == '-') v = 62;
            else if (c == '/' || c == '_') v = 63;
            else continue; // padding, newlines
            buf = (buf << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
            }
        }
        return out;
    }

    std::string trim(std::string s) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
        return s;
    }

    // Cleans a pasted key: removes spaces, quotes and line breaks. The game's settings text box
    // drops the "_" character, so known prefixes are repaired ("cfut..." -> "cfut_...").
    std::string cleanKey(std::string k) {
        std::string out;
        for (unsigned char c : k) {
            if (std::isspace(c) || c == '"' || c == '\'') continue;
            out += static_cast<char>(c);
        }
        if (out.size() > 5 && out.rfind("cfut", 0) == 0 && out[4] != '_') out.insert(4, "_");
        if (out.size() > 4 && out.rfind("gsk", 0) == 0 && out[3] != '_') out.insert(3, "_");
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

    // Perceptual distance ("redmean"): much closer to how the eye sees skin, sky and stone tones
    // than plain RGB distance.
    float colorDist2(ccColor3B a, ccColor3B b) {
        float rmean = (float(a.r) + float(b.r)) * 0.5f;
        float dr = float(a.r) - float(b.r);
        float dg = float(a.g) - float(b.g);
        float db = float(a.b) - float(b.b);
        return (2.f + rmean / 256.f) * dr * dr + 4.f * dg * dg + (2.f + (255.f - rmean) / 256.f) * db * db;
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

    // ---- the selection's shape --------------------------------------------------------------

    bool insideSelection(Job const& job, float x, float y) {
        for (auto const& r : job.rects) {
            if (r.containsPoint(ccp(x, y))) return true;
        }
        return false;
    }

    // A small text picture of the silhouette ('#' = solid) that is shown to the language models.
    std::string silhouette(Job const& job) {
        int cols = std::min(job.gw, 40);
        int rows = std::clamp(
            static_cast<int>(std::round(cols * (job.area.size.height / job.area.size.width) * 0.5f)), 2, 20
        );
        std::string out;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                float x = job.area.origin.x + (c + 0.5f) / cols * job.area.size.width;
                float y = job.area.origin.y + job.area.size.height - (r + 0.5f) / rows * job.area.size.height;
                out += insideSelection(job, x, y) ? '#' : '.';
            }
            out += '\n';
        }
        return out;
    }

    // ---- placing things in the level --------------------------------------------------------

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

    void setObjectChannel(GameObject* obj, int channel) {
        if (auto c = obj->m_baseColor) {
            c->m_colorID = channel;
            c->m_defaultColorID = channel;
        }
    }

    ccColor3B sampleCanvas(Job const& job, float x, float y) {
        int i = static_cast<int>(std::floor((x - job.area.origin.x) / job.cellW));
        int j = static_cast<int>(std::floor((job.area.origin.y + job.area.size.height - y) / job.cellH));
        i = std::clamp(i, 0, job.gw - 1);
        j = std::clamp(j, 0, job.gh - 1);
        return job.canvas[static_cast<size_t>(j) * job.gw + i];
    }

    // Creates one stretched basic block with a color channel.
    GameObject* makeBlock(Job& job, CCPoint center, float w, float h, int channel) {
        auto obj = job.lel->createObject(1, center, false);
        if (!obj) return nullptr;
        obj->updateCustomScaleX(std::max(0.02f, w / kBlock));
        obj->updateCustomScaleY(std::max(0.02f, h / kBlock));
        setObjectChannel(obj, channel);
        return obj;
    }

    // ---- placement mode 1: smart blocks (image trace) ---------------------------------------

    struct Rect { int i0, j0, i1, j1, color; };

    // Removes speckle: every cell takes the most common color among its 3x3 neighbours.
    // Fewer, bigger blocks with cleaner edges, at almost no loss of detail.
    void majorityFilter(std::vector<int>& label, int gw, int gh, int colors, int passes) {
        if (colors <= 0) return;
        for (int pass = 0; pass < passes; ++pass) {
            std::vector<int> out = label;
            std::vector<int> counts(static_cast<size_t>(colors));
            for (int j = 0; j < gh; ++j) {
                for (int i = 0; i < gw; ++i) {
                    int cur = label[static_cast<size_t>(j) * gw + i];
                    if (cur < 0) continue;
                    std::fill(counts.begin(), counts.end(), 0);
                    for (int dj = -1; dj <= 1; ++dj) {
                        for (int di = -1; di <= 1; ++di) {
                            int ni = i + di, nj = j + dj;
                            if (ni < 0 || nj < 0 || ni >= gw || nj >= gh) continue;
                            int l = label[static_cast<size_t>(nj) * gw + ni];
                            if (l >= 0 && l < colors) counts[static_cast<size_t>(l)]++;
                        }
                    }
                    int best = cur;
                    for (int c = 0; c < colors; ++c) {
                        if (counts[static_cast<size_t>(c)] > counts[static_cast<size_t>(best)]) best = c;
                    }
                    out[static_cast<size_t>(j) * gw + i] = best;
                }
            }
            label = std::move(out);
        }
    }

    // Greedy rectangle merging: same-color neighbours become one big block.
    std::vector<Rect> mergeRects(std::vector<int> const& label, int gw, int gh) {
        std::vector<Rect> rects;
        std::vector<char> used(label.size(), 0);
        for (int j = 0; j < gh; ++j) {
            for (int i = 0; i < gw; ++i) {
                size_t idx = static_cast<size_t>(j) * gw + i;
                if (used[idx] || label[idx] < 0) continue;
                int c = label[idx];
                int i1 = i;
                while (i1 + 1 < gw && !used[idx + (i1 + 1 - i)] && label[idx + (i1 + 1 - i)] == c) i1++;
                int j1 = j;
                bool grow = true;
                while (grow && j1 + 1 < gh) {
                    for (int k = i; k <= i1; ++k) {
                        size_t n = static_cast<size_t>(j1 + 1) * gw + k;
                        if (used[n] || label[n] != c) { grow = false; break; }
                    }
                    if (grow) j1++;
                }
                for (int jj = j; jj <= j1; ++jj) {
                    for (int ii = i; ii <= i1; ++ii) used[static_cast<size_t>(jj) * gw + ii] = 1;
                }
                rects.push_back({i, j, i1, j1, c});
            }
        }
        return rects;
    }

    void applySmart(std::shared_ptr<Job> job) {
        int firstChannel = static_cast<int>(getI("art-first-channel"));
        int maxColors = static_cast<int>(getI("art-colors"));
        int maxObjects = static_cast<int>(getI("max-objects"));
        int smooth = static_cast<int>(getI("art-smooth"));
        bool clip = getB("art-clip");
        int gw = job->gw, gh = job->gh;
        float top = job->area.origin.y + job->area.size.height;

        // Cells that belong to the art, with their colors.
        std::vector<int> cellToColor(static_cast<size_t>(gw) * gh, -1);
        std::vector<ccColor3B> cols;
        for (int j = 0; j < gh; ++j) {
            for (int i = 0; i < gw; ++i) {
                float x = job->area.origin.x + (i + 0.5f) * job->cellW;
                float y = top - (j + 0.5f) * job->cellH;
                if (clip && !insideSelection(*job, x, y)) continue;
                cellToColor[static_cast<size_t>(j) * gw + i] = static_cast<int>(cols.size());
                cols.push_back(job->canvas[static_cast<size_t>(j) * gw + i]);
            }
        }
        if (cols.empty()) {
            notify("Nothing to paint inside the selection", NotificationIcon::Warning);
            return;
        }

        // Quantize and merge. If the result needs more blocks than your limit, the art is
        // simplified automatically (more smoothing first, then fewer colors) until it fits.
        std::vector<ccColor3B> palette;
        std::vector<int> label;
        std::vector<Rect> rects;
        int colorsNow = maxColors;
        int extraSmooth = 0;
        bool simplified = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            auto assign = quantize(cols, colorsNow, palette);
            label.assign(static_cast<size_t>(gw) * gh, -1);
            for (size_t c = 0; c < label.size(); ++c) {
                if (cellToColor[c] >= 0) label[c] = assign[static_cast<size_t>(cellToColor[c])];
            }
            majorityFilter(label, gw, gh, static_cast<int>(palette.size()), smooth + extraSmooth);
            rects = mergeRects(label, gw, gh);
            if (static_cast<int>(rects.size()) <= maxObjects) break;
            simplified = true;
            if (extraSmooth < 3) extraSmooth++;
            else colorsNow = std::max(3, colorsNow * 3 / 4);
        }

        if (static_cast<int>(rects.size()) > maxObjects) {
            notify(fmt::format("The art needs {} blocks (limit {}). Raise the object limit or Art Cell Size.",
                               rects.size(), maxObjects),
                   NotificationIcon::Warning);
            return;
        }
        if (simplified) {
            notify("Simplified the art to fit your block limit", NotificationIcon::Info);
        }

        for (size_t c = 0; c < palette.size(); ++c) {
            defineChannel(job->lel, firstChannel + static_cast<int>(c), palette[c]);
        }

        auto placed = CCArray::create();
        for (auto const& r : rects) {
            float w = (r.i1 - r.i0 + 1) * job->cellW;
            float h = (r.j1 - r.j0 + 1) * job->cellH;
            CCPoint center = ccp(job->area.origin.x + (r.i0 + r.i1 + 1) * 0.5f * job->cellW,
                                 top - (r.j0 + r.j1 + 1) * 0.5f * job->cellH);
            if (auto obj = makeBlock(*job, center, w, h, firstChannel + r.color)) placed->addObject(obj);
        }
        job->lel->updateObjectColors(placed);
        // The new blocks stay selected: press Delete to remove the whole drawing, or move it as one piece.
        job->ui->selectObjects(placed, true);

        notify(fmt::format("Art done: {} blocks, {} colors (channels {}-{})", rects.size(), palette.size(),
                           firstChannel, firstChannel + static_cast<int>(palette.size()) - 1),
               NotificationIcon::Success);
    }

    // ---- placement mode 2: recolor the selected blocks --------------------------------------

    void applyRecolor(std::shared_ptr<Job> job) {
        int firstChannel = static_cast<int>(getI("art-first-channel"));
        int maxColors = static_cast<int>(getI("art-colors"));

        std::vector<ccColor3B> cols;
        for (auto const& o : job->objs) cols.push_back(sampleCanvas(*job, o->getPositionX(), o->getPositionY()));
        if (cols.empty()) return;

        std::vector<ccColor3B> palette;
        auto assign = quantize(cols, maxColors, palette);
        for (size_t c = 0; c < palette.size(); ++c) {
            defineChannel(job->lel, firstChannel + static_cast<int>(c), palette[c]);
        }

        auto changed = CCArray::create();
        for (size_t i = 0; i < job->objs.size(); ++i) {
            setObjectChannel(job->objs[i], firstChannel + assign[i]);
            changed->addObject(job->objs[i]);
        }
        job->lel->updateObjectColors(changed);
        notify(fmt::format("Art done: {} colors, channels {}-{}", palette.size(), firstChannel,
                           firstChannel + static_cast<int>(palette.size()) - 1),
               NotificationIcon::Success);
    }

    // ---- placement mode 3: the model's vector shapes as stretched blocks --------------------

    void applyShapes(std::shared_ptr<Job> job) {
        int firstChannel = static_cast<int>(getI("art-first-channel"));
        int maxColors = static_cast<int>(getI("art-colors"));
        int maxObjects = static_cast<int>(getI("max-objects"));

        float canvasH = 100.f * static_cast<float>(job->gh) / static_cast<float>(job->gw);
        float sxU = job->area.size.width / 100.f;      // level units per canvas unit, x
        float syU = job->area.size.height / canvasH;   // level units per canvas unit, y
        float left = job->area.origin.x;
        float top = job->area.origin.y + job->area.size.height;
        float stripLevel = std::max(3.75f, static_cast<float>(getF("art-cell")));
        float stripU = stripLevel / syU;

        std::vector<ccColor3B> cols;
        for (auto const& s : job->shapes) cols.push_back(s.color);
        std::vector<ccColor3B> palette;
        auto assign = quantize(cols, maxColors, palette);
        for (size_t c = 0; c < palette.size(); ++c) {
            defineChannel(job->lel, firstChannel + static_cast<int>(c), palette[c]);
        }

        auto changed = CCArray::create();
        int placed = 0;
        bool limitHit = false;

        // Places one block covering [x0,x1] x [y0,y1] (canvas units, y down).
        auto place = [&](float x0, float x1, float y0, float y1, int channel) {
            if (limitHit) return;
            x0 = std::max(0.f, x0); x1 = std::min(100.f, x1);
            y0 = std::max(0.f, y0); y1 = std::min(canvasH, y1);
            if (x1 - x0 < 0.05f || y1 - y0 < 0.05f) return;
            if (placed >= maxObjects) { limitHit = true; return; }
            CCPoint center = ccp(left + (x0 + x1) * 0.5f * sxU, top - (y0 + y1) * 0.5f * syU);
            if (auto obj = makeBlock(*job, center, (x1 - x0) * sxU, (y1 - y0) * syU, channel)) {
                changed->addObject(obj);
                placed++;
            }
        };

        for (size_t si = 0; si < job->shapes.size(); ++si) {
            auto const& s = job->shapes[si];
            int channel = firstChannel + assign[si];

            if (s.type == 0) {
                place(s.a, s.a + s.c, s.b, s.b + s.d, channel);
                continue;
            }

            float y0 = 1e9f, y1 = -1e9f;
            if (s.type == 1) {
                if (s.d < 0.01f || s.c < 0.01f) continue;
                y0 = s.b - s.d; y1 = s.b + s.d;
            } else {
                for (auto const& p : s.pts) { y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
            }
            y0 = std::max(0.f, y0);
            y1 = std::min(canvasH, y1);
            if (y1 - y0 < 0.05f) continue;

            int rows = std::max(1, static_cast<int>(std::ceil((y1 - y0) / stripU)));
            float rowH = (y1 - y0) / static_cast<float>(rows);
            for (int row = 0; row < rows; ++row) {
                float ya = y0 + row * rowH;
                float yb = ya + rowH;
                float yc = (ya + yb) * 0.5f;

                if (s.type == 1) {
                    float dy = (yc - s.b) / s.d;
                    if (std::abs(dy) >= 1.f) continue;
                    float half = s.c * std::sqrt(1.f - dy * dy);
                    place(s.a - half, s.a + half, ya, yb, channel);
                } else {
                    std::vector<float> xs;
                    for (size_t i = 0, j = s.pts.size() - 1; i < s.pts.size(); j = i++) {
                        CCPoint a = s.pts[i];
                        CCPoint b = s.pts[j];
                        if ((a.y > yc) != (b.y > yc)) {
                            xs.push_back(a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y));
                        }
                    }
                    std::sort(xs.begin(), xs.end());
                    for (size_t i = 0; i + 1 < xs.size(); i += 2) place(xs[i], xs[i + 1], ya, yb, channel);
                }
            }
        }

        job->lel->updateObjectColors(changed);
        job->ui->selectObjects(changed, true);
        if (limitHit) notify("Object limit reached, the drawing is incomplete", NotificationIcon::Warning);
        notify(fmt::format("Art done: {} blocks, {} colors (channels {}-{})", placed, palette.size(),
                           firstChannel, firstChannel + static_cast<int>(palette.size()) - 1),
               NotificationIcon::Success);
    }

    void applyCanvas(std::shared_ptr<Job> job) {
        if (!job->ui || !job->lel || !job->ui->getParent()) {
            notify("The editor was closed, art discarded", NotificationIcon::Warning);
            return;
        }
        std::string mode = getS("art-mode");
        if (mode == "Recolor selected blocks") {
            applyRecolor(job);
        } else if (mode == "Shapes as real blocks" && !job->shapes.empty()) {
            applyShapes(job);
        } else {
            applySmart(job); // default, and the only option for image backends
        }
    }

    void finish(std::shared_ptr<Job> job, bool ok, std::string const& error = "") {
        s_busy = false;
        if (!ok) {
            // A popup wraps long text; a toast is cut off at the screen edges.
            log::error("Art Agent failed: {}", error);
            FLAlertLayer::create("Art Agent failed", error, "OK")->show();
            return;
        }
        applyCanvas(job);
    }

    std::string httpWhy(web::WebResponse const& res, char const* what) {
        std::string why = fmt::format("server answered {}", res.code());
        if (res.code() == 400 || res.code() == 401 || res.code() == 403) {
            why += fmt::format(" (check your {})", what);
        } else if (res.code() == 404) {
            why += " (check the URL, account ID and model name)";
        } else if (res.code() == 429) {
            why += " (free quota reached, try later or switch service)";
        }
        // Include what the server said: it usually names the exact problem.
        std::string body = res.string().unwrapOr("");
        if (!body.empty()) {
            log::error("Art Agent: server answered {}: {}", res.code(), body);

            // Cloudflare: {"result":null,"success":false,"errors":[{"code":10000,"message":"..."}]}
            // OpenAI-style: {"error":{"message":"..."}}
            std::string detail;
            if (auto parsed = matjson::parse(body)) {
                auto js = std::move(parsed).unwrap();
                if (js.isObject()) {
                    if (js.contains("errors") && js["errors"].isArray()) {
                        auto& errs = js["errors"];
                        for (size_t i = 0; i < errs.size() && i < 2; ++i) {
                            int64_t code = errs[i]["code"].asInt().unwrapOr(0);
                            detail += fmt::format("{} (code {}) ", errs[i]["message"].asString().unwrapOr("?"), code);
                            if (code == 10000) {
                                detail += "-> the API token is wrong or lacks Workers AI permission. ";
                            } else if (code == 7000 || code == 7003) {
                                detail += "-> the Account ID is wrong. ";
                            }
                        }
                    } else if (js.contains("error") && js["error"].isObject()) {
                        detail = js["error"]["message"].asString().unwrapOr("");
                    } else if (js.contains("error") && js["error"].isString()) {
                        detail = js["error"].asString().unwrapOr("");
                    }
                }
            }
            if (detail.empty()) {
                detail = body;
                if (detail.size() > 220) detail.resize(220);
            }
            why += " | " + detail;
        }
        return why;
    }

    // ---- language models (Gemini / OpenAI-compatible) ---------------------------------------

    using LLMCb = std::function<void(bool, std::string)>;

    // Sends one prompt and calls back with (ok, answer-or-error). `which` is "gemini" or "openai".
    void askLLM(geode::async::TaskHolder<web::WebResponse>& holder, std::string const& which,
                std::string const& instruction, bool wantJson, LLMCb cb) {
        web::WebRequest req;
        req.userAgent("GeodeEditorTools/0.6");
        req.timeout(std::chrono::seconds(120));
        req.header("Content-Type", "application/json");

        if (which == "gemini") {
            std::string key = cleanKey(getS("art-gemini-key"));
            if (key.empty()) return cb(false, "paste your free Gemini API key in the mod settings");
            std::string model = getS("art-gemini-model");
            if (model.empty()) model = "gemini-2.5-flash";

            std::string gen = wantJson ? "{\"responseMimeType\":\"application/json\",\"temperature\":0.9}"
                                       : "{\"temperature\":0.8}";
            req.header("x-goog-api-key", key);
            req.bodyString(fmt::format(
                "{{\"contents\":[{{\"parts\":[{{\"text\":{}}}]}}],\"generationConfig\":{}}}",
                matjson::Value(instruction).dump(0), gen
            ));
            std::string url = fmt::format(
                "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model
            );
            holder.spawn(req.post(url), [cb](web::WebResponse res) {
                if (!res.ok()) return cb(false, httpWhy(res, "Gemini API key"));
                auto js = res.json();
                if (!js) return cb(false, "bad response from Gemini");
                auto root = std::move(js).unwrap();
                auto& cands = root["candidates"];
                if (!cands.isArray() || cands.size() == 0) return cb(false, "Gemini returned no answer");
                auto& parts = cands[0]["content"]["parts"];
                if (!parts.isArray() || parts.size() == 0) return cb(false, "Gemini returned an empty answer");
                cb(true, parts[0]["text"].asString().unwrapOr(""));
            });
            return;
        }

        // OpenAI-compatible: Groq, OpenRouter, Cerebras, Mistral, Together, ...
        std::string base = getS("art-openai-url");
        std::string key = cleanKey(getS("art-openai-key"));
        std::string model = getS("art-openai-model");
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (base.empty() || model.empty()) {
            return cb(false, "set the URL and model of the AI service in the mod settings");
        }
        if (key.empty()) return cb(false, "paste your free API key for this service in the mod settings");

        req.header("Authorization", "Bearer " + key);
        req.bodyString(fmt::format(
            "{{\"model\":{},\"temperature\":0.9,\"messages\":[{{\"role\":\"user\",\"content\":{}}}]}}",
            matjson::Value(model).dump(0), matjson::Value(instruction).dump(0)
        ));
        holder.spawn(req.post(base + "/chat/completions"), [cb](web::WebResponse res) {
            if (!res.ok()) return cb(false, httpWhy(res, "API key and model"));
            auto js = res.json();
            if (!js) return cb(false, "bad response from the AI service");
            auto root = std::move(js).unwrap();
            auto& choices = root["choices"];
            if (!choices.isArray() || choices.size() == 0) return cb(false, "the AI returned no answer");
            cb(true, choices[0]["message"]["content"].asString().unwrapOr(""));
        });
    }

    // Which language model is available to plan prompts (empty = none configured).
    std::string plannerLLM() {
        std::string backend = getS("art-backend");
        if (backend == "Gemini (free key)") return "gemini";
        if (backend == "OpenAI-compatible (Groq, OpenRouter...)") return "openai";
        if (!cleanKey(getS("art-gemini-key")).empty()) return "gemini";
        if (!cleanKey(getS("art-openai-key")).empty()) return "openai";
        return "";
    }

    bool realisticStyle() { return getS("art-style") != "Flat (simple)"; }

    std::string planInstruction(Job const& job) {
        std::string style = realisticStyle()
            ? "Realistic, highly detailed game art: describe the materials (stone, wood, metal, brick, moss, glass), "
              "textures, cracks and wear, windows and doors if it is a building, a clear light source from the top left "
              "with highlights and cast shadows, and depth. Front elevation view, orthographic, no perspective, "
              "plain dark background. "
            : "Flat vector style, bold shapes, high contrast, plain simple background. ";
        return fmt::format(
            "You write prompts for an image generator. The image will decorate a video game structure "
            "with the silhouette below ('#' = solid, '.' = empty), aspect ratio about {:.1f}:1 (wide:tall).\n"
            "{}\n"
            "User request: \"{}\"\n"
            "Write ONE detailed prompt in English, max 80 words, for a decorative game-art illustration "
            "that fills the whole picture, using about {} distinct colors. {}"
            "No text, no frame, no watermark. "
            "Reply with only the prompt.",
            job.area.size.width / std::max(1.f, job.area.size.height), silhouette(job), job.prompt,
            getI("art-colors"), style
        );
    }

    std::string shapesInstruction(Job const& job) {
        float canvasH = 100.f * static_cast<float>(job.gh) / static_cast<float>(job.gw);
        return fmt::format(
            "You are an artist making flat vector decoration for a video game structure. "
            "Canvas: width 100, height {:.1f}, origin at the top-left, y grows downward. "
            "The art will be clipped to this silhouette ('#' = solid, '.' = empty):\n{}\n"
            "Draw: \"{}\". "
            "Reply with ONLY a JSON object, no markdown, in this exact format: "
            "{{\"background\":\"#RRGGBB\",\"shapes\":[{{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"color\":\"#RRGGBB\"}},"
            "{{\"type\":\"ellipse\",\"cx\":50,\"cy\":50,\"rx\":10,\"ry\":10,\"color\":\"#RRGGBB\"}},"
            "{{\"type\":\"polygon\",\"points\":[[x,y],[x,y],[x,y]],\"color\":\"#RRGGBB\"}}]}}. "
            "Shapes are painted in order, so put large background shapes first and details last. "
            "Compose it like a decorative piece: strong silhouette, a few large shapes plus small details, "
            "good contrast between neighbouring colors. "
            "Use at most 60 shapes and at most {} distinct colors. Fill the whole canvas.",
            canvasH, silhouette(job), job.prompt, getI("art-colors")
        );
    }

    std::string stripFences(std::string s) {
        auto first = s.find('{');
        auto last = s.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last < first) return s;
        return s.substr(first, last - first + 1);
    }

    // Parses the model's JSON answer into job->shapes and rasterizes it into the canvas.
    void onShapesText(std::shared_ptr<Job> job, std::string const& text, std::string const& who) {
        auto parsed = matjson::parse(stripFences(text));
        if (!parsed) return finish(job, false, who + " did not return valid JSON, try again");
        auto art = std::move(parsed).unwrap();

        ccColor3B background{40, 40, 50};
        parseHex(art["background"].asString().unwrapOr(""), background);

        std::vector<Shape> shapes;
        auto& list = art["shapes"];
        if (list.isArray()) {
            for (size_t i = 0; i < list.size() && shapes.size() < 100; ++i) {
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
        if (shapes.empty()) return finish(job, false, who + " returned no shapes, try again");

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
        job->shapes = std::move(shapes);
        finish(job, true);
    }

    void startShapes(std::shared_ptr<Job> job, std::string const& which, std::string const& who) {
        notify("Drawing...");
        askLLM(s_task, which, shapesInstruction(*job), true, [job, who](bool ok, std::string text) {
            if (!ok) return finish(job, false, text);
            onShapesText(job, text, who);
        });
    }

    // ---- image models (Cloudflare FLUX / Pollinations) --------------------------------------

    // Decodes an image and samples it into the canvas (center-cropped to the selection's aspect).
    // Returns an error message, or an empty string on success.
    std::string canvasFromImageBytes(Job& job, uint8_t const* data, size_t size) {
        if (!data || size < 16) return "empty image";
        auto img = new CCImage();
        if (!img->initWithImageData(const_cast<uint8_t*>(data), static_cast<int>(size))) {
            img->release();
            return "could not read the image (unsupported format)";
        }

        int W = static_cast<int>(img->getWidth());
        int H = static_cast<int>(img->getHeight());
        unsigned char* px = img->getData();
        int stride = img->hasAlpha() ? 4 : 3;
        if (!px || W < 2 || H < 2) {
            img->release();
            return "empty image";
        }

        // Crop the image to the same aspect ratio as the selection, so nothing is stretched.
        float targetAspect = job.area.size.width / std::max(1.f, job.area.size.height);
        float srcAspect = static_cast<float>(W) / static_cast<float>(H);
        int cropW = W, cropH = H, cropX = 0, cropY = 0;
        if (srcAspect > targetAspect) {
            cropW = std::max(2, static_cast<int>(H * targetAspect));
            cropX = (W - cropW) / 2;
        } else {
            cropH = std::max(2, static_cast<int>(W / targetAspect));
            cropY = (H - cropH) / 2;
        }

        job.canvas.assign(static_cast<size_t>(job.gw) * job.gh, ccColor3B{0, 0, 0});
        for (int j = 0; j < job.gh; ++j) {
            for (int i = 0; i < job.gw; ++i) {
                int x0 = cropX + i * cropW / job.gw;
                int x1 = std::max(x0 + 1, cropX + (i + 1) * cropW / job.gw);
                int y0 = cropY + j * cropH / job.gh;
                int y1 = std::max(y0 + 1, cropY + (j + 1) * cropH / job.gh);
                double r = 0, g = 0, b = 0;
                int n = 0;
                for (int y = y0; y < y1 && y < H; ++y) {
                    for (int x = x0; x < x1 && x < W; ++x) {
                        unsigned char* p = px + (static_cast<size_t>(y) * W + x) * stride;
                        r += p[0]; g += p[1]; b += p[2]; n++;
                    }
                }
                if (n == 0) n = 1;
                job.canvas[static_cast<size_t>(j) * job.gw + i] =
                    ccColor3B{static_cast<GLubyte>(r / n), static_cast<GLubyte>(g / n), static_cast<GLubyte>(b / n)};
            }
        }
        img->release();
        return "";
    }

    std::string imagePrompt(Job const& job) {
        if (realisticStyle()) {
            return job.prompt +
                ", highly detailed realistic game art, realistic textures and materials, soft shading, "
                "consistent lighting from the top left with highlights and shadows, front elevation view, "
                "orthographic, centered, rich but limited color palette, plain dark background, "
                "no text, no frame, no watermark";
        }
        return job.prompt +
            ", flat vector illustration, bold simple shapes, limited color palette, strong silhouette, "
            "high contrast, decorative game art, plain simple background, no text, no frame";
    }

    void startCloudflare(std::shared_ptr<Job> job) {
        std::string account = cleanKey(getS("art-cf-account"));
        std::string token = cleanKey(getS("art-cf-token"));
        if (account.empty() || token.empty()) {
            return finish(job, false, "paste your Cloudflare Account ID and API token in the mod settings");
        }

        notify("Drawing...");
        web::WebRequest req;
        req.userAgent("GeodeEditorTools/0.6");
        req.timeout(std::chrono::seconds(120));
        req.header("Authorization", "Bearer " + token);
        req.header("Content-Type", "application/json");
        req.bodyString(fmt::format("{{\"prompt\":{},\"steps\":6}}", matjson::Value(imagePrompt(*job)).dump(0)));

        std::string url = fmt::format(
            "https://api.cloudflare.com/client/v4/accounts/{}/ai/run/@cf/black-forest-labs/flux-1-schnell", account
        );
        s_task.spawn(req.post(url), [job, token, account](web::WebResponse res) {
            if (!res.ok()) {
                std::string why = httpWhy(res, "Cloudflare Account ID and API token");
                // What the mod actually sent (no secrets): helps spot a truncated or altered paste.
                std::string info = fmt::format(
                    " [token: {} chars, starts '{}', ends '{}'; account id: {} chars]",
                    token.size(), token.substr(0, 5),
                    token.size() > 3 ? token.substr(token.size() - 3) : std::string(), account.size()
                );
                if (res.code() == 401 || res.code() == 403) {
                    // Ask Cloudflare separately whether the token itself is valid and active.
                    web::WebRequest check;
                    check.userAgent("GeodeEditorTools/0.6");
                    check.timeout(std::chrono::seconds(30));
                    check.header("Authorization", "Bearer " + token);
                    s_planTask.spawn(
                        check.get("https://api.cloudflare.com/client/v4/user/tokens/verify"),
                        [job, why, info](web::WebResponse vr) {
                            std::string status;
                            if (vr.ok()) {
                                std::string st;
                                if (auto js = vr.json()) {
                                    auto root = std::move(js).unwrap();
                                    st = root["result"]["status"].asString().unwrapOr("");
                                }
                                status = st == "active"
                                    ? " | Token check: ACTIVE, so the token is valid but lacks Workers AI permission "
                                      "or belongs to another account"
                                    : " | Token check: status '" + st + "'";
                            } else {
                                status = fmt::format(" | Token check: REJECTED ({}), the token is wrong, "
                                                     "incomplete or was deleted", vr.code());
                            }
                            finish(job, false, why + status + info);
                        }
                    );
                    return;
                }
                return finish(job, false, why + info);
            }
            auto js = res.json();
            if (!js) return finish(job, false, "bad response from Cloudflare");
            auto root = std::move(js).unwrap();
            if (!root.isObject() || !root.contains("result")) {
                return finish(job, false, "Cloudflare returned no image");
            }
            std::string b64 = root["result"]["image"].asString().unwrapOr("");
            if (b64.empty()) return finish(job, false, "Cloudflare returned no image");

            notify("Tracing...");
            auto bytes = base64Decode(b64);
            auto err = canvasFromImageBytes(*job, bytes.data(), bytes.size());
            if (!err.empty()) return finish(job, false, err);
            finish(job, true);
        });
    }

    void startPollinations(std::shared_ptr<Job> job) {
        notify("Drawing...");
        float aspect = job->area.size.height / std::max(1.f, job->area.size.width);
        int w = 768;
        int h = std::clamp(static_cast<int>(768.f * aspect), 192, 1024);
        std::string url = fmt::format(
            "https://image.pollinations.ai/prompt/{}?width={}&height={}&nologo=true&seed={}",
            urlEncode(imagePrompt(*job)), w, h, static_cast<int>(std::time(nullptr) % 100000)
        );
        s_task.spawn(
            web::WebRequest().userAgent("GeodeEditorTools/0.6").timeout(std::chrono::seconds(120)).get(url),
            [job](web::WebResponse res) {
                if (!res.ok()) return finish(job, false, httpWhy(res, "connection (Pollinations now limits free use)"));
                auto const& data = res.data();
                notify("Tracing...");
                auto err = canvasFromImageBytes(*job, data.data(), data.size());
                if (!err.empty()) return finish(job, false, err);
                finish(job, true);
            }
        );
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

        // 1. Shape: the area covered by the selection, optionally with some overhang.
        float overhang = static_cast<float>(getF("art-overhang"));
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (auto obj : CCArrayExt<GameObject*>(sel)) {
            auto r = obj->getObjectRect();
            if (r.size.width < 1.f) r = CCRect(obj->getPositionX() - 15, obj->getPositionY() - 15, 30, 30);
            r = CCRect(r.origin.x - overhang, r.origin.y - overhang,
                       r.size.width + overhang * 2.f, r.size.height + overhang * 2.f);
            job->rects.push_back(r);
            minX = std::min(minX, r.getMinX()); maxX = std::max(maxX, r.getMaxX());
            minY = std::min(minY, r.getMinY()); maxY = std::max(maxY, r.getMaxY());
            job->objs.push_back(obj);
        }
        job->area = CCRect(minX, minY, std::max(1.f, maxX - minX), std::max(1.f, maxY - minY));

        float cell = std::max(3.75f, static_cast<float>(getF("art-cell")));
        job->gw = std::clamp(static_cast<int>(std::ceil(job->area.size.width / cell)), 4, 192);
        job->gh = std::clamp(static_cast<int>(std::ceil(job->area.size.height / cell)), 4, 192);
        job->cellW = job->area.size.width / job->gw;
        job->cellH = job->area.size.height / job->gh;

        s_busy = true;
        std::string backend = getS("art-backend");

        // Language-model backends draw vector shapes directly.
        if (backend == "Gemini (free key)") return startShapes(job, "gemini", "Gemini");
        if (backend == "OpenAI-compatible (Groq, OpenRouter...)") return startShapes(job, "openai", "The AI");

        // Image backends: 2. Plan (optional), then 3. Drawing.
        auto draw = [job, backend]() {
            if (backend == "Pollinations (limited, image)") startPollinations(job);
            else startCloudflare(job);
        };

        std::string planner = plannerLLM();
        if (getB("art-plan") && !planner.empty()) {
            notify("Planning the drawing...");
            askLLM(s_planTask, planner, planInstruction(*job), false, [job, draw](bool ok, std::string text) {
                text = trim(text);
                if (ok && !text.empty() && text.size() < 1200) job->prompt = text;
                draw(); // if planning fails we simply use the original prompt
            });
        } else {
            draw();
        }
    }

    // ---- popup ------------------------------------------------------------------------------

    // A small screen to paste keys. It uses a font that accepts every character (the settings
    // text box of the game drops "_"), and saves into the same mod settings.
    class KeysPopup : public Popup {
    protected:
        std::vector<std::pair<char const*, TextInput*>> m_fields;

        bool init() {
            if (!Popup::init(380.f, 280.f)) return false;
            this->setTitle("Art AI keys");

            struct Row { char const* label; char const* key; };
            Row rows[] = {
                {"Cloudflare Account ID", "art-cf-account"},
                {"Cloudflare API Token", "art-cf-token"},
                {"Groq / service API key", "art-openai-key"},
                {"Gemini API key", "art-gemini-key"},
            };
            float y = 70.f;
            for (auto const& r : rows) {
                auto lbl = CCLabelBMFont::create(r.label, "goldFont.fnt");
                lbl->setScale(.4f);
                m_mainLayer->addChildAtPosition(lbl, Anchor::Center, ccp(0, y + 20.f));

                auto in = TextInput::create(330.f, r.label, "chatFont.fnt");
                in->setMaxCharCount(300);
                in->setString(getS(r.key));
                m_mainLayer->addChildAtPosition(in, Anchor::Center, ccp(0, y));
                m_fields.push_back({r.key, in});
                y -= 48.f;
            }

            auto spr = ButtonSprite::create("Save", 80, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .7f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this](CCMenuItemSpriteExtra*) {
                for (auto const& f : m_fields) {
                    std::string v = cleanKey(static_cast<std::string>(f.second->getString()));
                    Mod::get()->setSettingValue<std::string>(f.first, v);
                }
                notify("Keys saved", NotificationIcon::Success);
                this->onClose(nullptr);
            });
            m_buttonMenu->addChildAtPosition(btn, Anchor::Bottom, ccp(0, 24));
            return true;
        }

    public:
        static KeysPopup* create() {
            auto ret = new KeysPopup();
            if (ret->init()) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }
    };

    class ArtPopup : public Popup {
    protected:
        TextInput* m_input = nullptr;

        bool init(Ref<EditorUI> ui) {
            if (!Popup::init(340.f, 175.f)) return false;
            this->setTitle("Art Agent");

            m_input = TextInput::create(290.f, "e.g. medieval stone castle with a wooden door", "chatFont.fnt");
            m_input->setMaxCharCount(200);
            m_mainLayer->addChildAtPosition(m_input, Anchor::Center, ccp(0, 15));

            auto hint = CCLabelBMFont::create("Describe materials and details (stone, wood, windows...)", "chatFont.fnt");
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

            auto keysSpr = ButtonSprite::create("Keys", 60, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
            auto keysBtn = CCMenuItemExt::createSpriteExtra(keysSpr, [](CCMenuItemSpriteExtra*) {
                if (auto popup = KeysPopup::create()) popup->show();
            });
            m_buttonMenu->addChildAtPosition(keysBtn, Anchor::Bottom, ccp(125, 35));
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
        notify("Select the blocks you want to decorate first", NotificationIcon::Warning);
        return;
    }
    if (auto popup = ArtPopup::create(ui)) popup->show();
}
