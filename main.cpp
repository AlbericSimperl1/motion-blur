// Pointer trail: a motion blur for the mouse pointer, made of fading afterimages of the cursor (port of WH_mod.c).
//
// How it works:
//   tracking  every pointer move is stored with its time; samples older than the trail are dropped
//   drawing   at the last moment of each frame the fading copies along the recent path are worked out and handed to
//             the render pass; while the trail is still fading the area is damaged again, so frames keep coming after
//             the mouse stops
//   hiding    while the mouse is very fast the real cursor is hidden: its monitor is switched to a software cursor and
//             the hooked software cursor drawing skips it. Switching happens only outside rendering (on input and before
//             a frame), as it can render the hardware cursor buffer

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <cairo/cairo.h>
#include <chrono>
#include <cmath>
#include <deque>
#include <optional>

#include "TrailPassElement.hpp"

using namespace Config::Values;

// ~px of travel one copy covers at cursor size 32 (keeps the opacity independent of copy density)
constexpr double COVER_PX       = 10.0;
constexpr size_t MAX_SAMPLES    = 512;
constexpr int    MAX_COPIES     = 600; // the most copies spread along the trail in one frame (fewer show as separate pointers)
constexpr size_t COPY_BUF       = MAX_SAMPLES + MAX_COPIES;
constexpr double HOLD_MS        = 10; // how long after the last fast moment the cursor may come back
constexpr double SHOW_WINDOW_MS = 12; // the speed that brings the cursor back is measured over just this many ms

struct SSample {
    Vector2D p; // global logical coordinates
    double   t; // ms
};

struct SSprite {
    SP<Render::ITexture> tex;
    Vector2D             size, hotspot; // logical
};

static HANDLE g_handle = nullptr;

static struct {
    SP<CBoolValue>   enabled;
    SP<CIntValue>    trailMs;
    SP<CFloatValue>  opacity;
    SP<CStringValue> imagePath;
    SP<CFloatValue>  minSpeed;
    SP<CBoolValue>   hideWhenFast;
    SP<CFloatValue>  hideSpeed;
} g_cfg;

static std::deque<SSample>  g_hist;
static PHLMONITORREF        g_histMon;       // the monitor the trail is on
static bool                 g_fastEnough;    // above the minimum speed (switched only after 16 ms past the line)
static double               g_crossedAt;     // when the speed crossed the line (0: it didn't)
static bool                 g_hideWanted;    // fast enough to hide the real cursor
static double               g_lastFast;      // when the mouse was last above the hide speed
static PHLMONITORREF        g_hiddenOn;      // the monitor whose cursor is hidden (holding a software cursor lock)
static bool                 g_hidden;        // the real cursor is hidden
static CFunctionHook*       g_swCursorHook = nullptr;
static std::optional<CBox>  g_lastBox;       // the area the trail was last drawn in (global logical)
static SP<Render::ITexture> g_png;           // the configured picture, if any
static std::string          g_pngPath;
static bool                 g_pngStale = true; // the config changed: look at the picture path again (while rendering)

static CHyprSignalListener  g_onMove, g_onPre, g_onStage, g_onReload;

static double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static PHLMONITOR monitorAt(const Vector2D& p) {
    for (const auto& m : State::monitorState()->monitors()) {
        if (m->m_enabled && !m->isMirror() && m->logicalBox().containsPoint(p))
            return m;
    }
    return nullptr;
}

// Needs the GL context, so only called while rendering.
static void loadPicture() {
    g_pngStale       = false;
    std::string path = g_cfg.imagePath->value();
    if (path == "[[EMPTY]]")
        path = "";
    if (path.starts_with("~/") && getenv("HOME"))
        path = std::string{getenv("HOME")} + path.substr(1);
    if (path == g_pngPath)
        return;

    g_pngPath = path;
    g_png.reset();
    if (path.empty())
        return;

    cairo_surface_t* surface = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS)
        g_png = g_pHyprRenderer->createTexture(surface);
    else
        HyprlandAPI::addNotification(g_handle, "[pointer-trail] Can't load " + path + ", using the cursor instead", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    cairo_surface_destroy(surface);
}

// The picture the copies are made of: the configured PNG (hotspot top-left), or the cursor as it is now.
static std::optional<SSprite> sprite() {
    if (g_png)
        return SSprite{g_png, g_png->m_size, {}};

    const auto& img = Pointer::mgr()->currentCursorImage();
    auto        tex = Pointer::mgr()->getCurrentCursorTexture();
    if (!tex || img.scale <= 0)
        return std::nullopt; // the app hid its cursor: don't blur the previous one
    return SSprite{tex, img.size / img.scale, img.hotspot};
}

static std::optional<CBox> trailBox(const SSprite& sp) {
    if (g_hist.empty())
        return std::nullopt;
    double minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
    for (const auto& s : g_hist) {
        minX = std::min(minX, s.p.x - sp.hotspot.x);
        minY = std::min(minY, s.p.y - sp.hotspot.y);
        maxX = std::max(maxX, s.p.x - sp.hotspot.x + sp.size.x);
        maxY = std::max(maxY, s.p.y - sp.hotspot.y + sp.size.y);
    }
    return CBox{minX, minY, maxX - minX, maxY - minY}.expand(2);
}

// Damages where the trail was and where it is now (both are needed: the old area must be cleared).
static void damageTrail(std::optional<CBox> box) {
    if (g_lastBox)
        g_pHyprRenderer->damageBox(*g_lastBox);
    if (box)
        g_pHyprRenderer->damageBox(*box);
    g_lastBox = box;
}

static void clearTrail() {
    damageTrail(std::nullopt);
    g_hist.clear();
    g_fastEnough = false;
    g_crossedAt  = 0;
    g_hideWanted = false;
}

typedef void (*origRenderSoftwareCursorsFor)(void*, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);
static void hkRenderSoftwareCursorsFor(void* thisptr, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool screencopy,
                                       bool forceRender) {
    if (g_hidden && !screencopy && pMonitor == g_hiddenOn.lock())
        return;
    ((origRenderSoftwareCursorsFor)g_swCursorHook->m_original)(thisptr, pMonitor, now, damage, overridePos, screencopy, forceRender);
}

// Hides or shows the real cursor as wanted. Never called while rendering: switching to a software cursor and back
// updates the cursor plane, which can render the hardware cursor buffer.
static void applyHide() {
    const bool want = g_hideWanted && g_swCursorHook && g_cfg.enabled->value();
    const auto mon  = g_histMon.lock();
    if (g_hidden && (!want || g_hiddenOn.lock() != mon)) {
        if (const auto old = g_hiddenOn.lock())
            Pointer::mgr()->unlockSoftwareForMonitor(old);
        g_hidden = false;
        g_hiddenOn.reset();
    }
    if (want && !g_hidden && mon) {
        Pointer::mgr()->lockSoftwareForMonitor(mon);
        g_hidden   = true;
        g_hiddenOn = mon;
    }
}

// Drops samples older than the trail (always keeping the newest), then decides whether the trail is drawn and whether
// the real cursor is hidden. Speed over the trail window: drawn above the minimum speed and stopped below half of it,
// each only once the speed has stayed past that line for 16 ms (mouse reports arrive unevenly, so a single reading can be
// a third off). The real cursor is hidden when very fast; bringing it back looks at only the last few ms (from sample j
// on), so it returns as soon as the mouse stops or slows.
static void ageTrail(double now) {
    const double trail = g_cfg.trailMs->value();
    while (g_hist.size() > 1 && now - g_hist.front().t > trail)
        g_hist.pop_front();

    const double dt   = g_hist.empty() ? 0 : g_hist.back().t - g_hist.front().t;
    size_t       j    = g_hist.empty() ? 0 : g_hist.size() - 1;
    while (j > 0 && now - g_hist[j - 1].t <= SHOW_WINDOW_MS)
        j--;
    double path = 0, recentPath = 0;
    for (size_t i = 1; i < g_hist.size(); i++) {
        const double d = g_hist[i - 1].p.distance(g_hist[i].p);
        path += d;
        if (i > j)
            recentPath += d;
    }

    if (!g_cfg.hideWhenFast->value() || g_hist.empty())
        g_hideWanted = false;
    else if (dt >= 8) {
        const double hide     = g_cfg.hideSpeed->value();
        const double speed    = path * 1000.0 / dt;
        const double recentDt = now - g_hist[j].t;
        const double recent   = recentDt >= 4 ? recentPath * 1000.0 / recentDt : speed;
        if (speed > hide) {
            g_hideWanted = true;
            g_lastFast   = now;
        } else if (g_hideWanted && recent < hide * 0.5 && now - g_lastFast > HOLD_MS)
            g_hideWanted = false;
    }
    if (g_hideWanted && now - g_hist.back().t > HOLD_MS + SHOW_WINDOW_MS) // stopped
        g_hideWanted = false;

    const double minSpeed = g_cfg.minSpeed->value();
    const double speed    = dt >= 4 ? path * 1000.0 / dt : 0;
    const double drawAt   = minSpeed * (g_fastEnough ? 0.5 : 1);
    const bool   want     = g_hist.size() > 1 && (minSpeed == 0 || (dt >= 4 && speed >= drawAt));
    if (want == g_fastEnough || g_hist.size() < 2 || minSpeed == 0) {
        g_crossedAt  = 0;
        g_fastEnough = want;
    } else if (!g_crossedAt)
        g_crossedAt = now;
    else if (now - g_crossedAt >= 16) {
        g_crossedAt  = 0;
        g_fastEnough = want;
    }
}

// The minimum speed only decides drawing; hiding goes by its own speed. A hidden cursor always gets its trail, so the
// pointer never disappears.
static bool drawing() {
    return g_fastEnough || g_hideWanted || g_hidden;
}

static void onMove() {
    if (!g_cfg.enabled->value())
        return;

    const Vector2D pos = Pointer::mgr()->position();
    const auto     mon = monitorAt(pos);
    if (mon != g_histMon.lock()) { // drop the trail so it doesn't smear across the gap between monitors
        clearTrail();
        g_histMon = mon;
    }

    const double now     = nowMs();
    const bool   changed = g_hist.empty() || g_hist.back().p != pos;
    if (!changed)
        return;
    if (!g_hist.empty() && now - g_hist.back().t < 1) // several reports within a ms: keep the newest
        g_hist.back().p = pos;
    else {
        if (g_hist.size() == MAX_SAMPLES)
            g_hist.pop_front();
        g_hist.push_back({pos, now});
    }

    ageTrail(now);
    applyHide();
    if (const auto sp = sprite(); sp && drawing())
        damageTrail(trailBox(*sp));
}

// The fading copies along the path for one monitor: dense, faint copies read as a smooth blur.
static std::vector<CPointerTrailPass::SCopy> buildCopies(const SSprite& sp, PHLMONITOR mon, double now) {
    std::vector<CPointerTrailPass::SCopy> copies;

    double total = 0;
    for (size_t i = 1; i < g_hist.size(); i++)
        total += g_hist[i - 1].p.distance(g_hist[i].p);

    const double step  = total / MAX_COPIES > 1 ? total / MAX_COPIES : 1;
    const double cover = COVER_PX * sp.size.x / 32.0, peak = g_cfg.opacity->value(), trail = g_cfg.trailMs->value();
    for (size_t i = 1; i < g_hist.size() && copies.size() < COPY_BUF; i++) {
        const auto&    a = g_hist[i - 1];
        const auto&    b = g_hist[i];
        const Vector2D delta = b.p - a.p;
        const double   d     = a.p.distance(b.p);
        if (d < 0.5)
            continue;
        const int k = std::max(1, sc<int>(d / step));
        // the copies stop just short of b: the real cursor is drawn there
        for (int c = 0; c < k && copies.size() < COPY_BUF; c++) {
            const double t   = sc<double>(c) / k;
            const double age = (now - (a.t + (b.t - a.t) * t)) / trail; // 0 at the pointer, 1 at the end of the trail
            if (age >= 1)
                continue;
            const double life = 1.0 - age * age; // nearly full strength behind the pointer, fading out toward the end
            // About cover/step copies overlap at each point; layered on top of each other they add up to the target
            // opacity when each has 1 - (1 - target)^(step/cover).
            const double target = peak * life, overlap = cover / step;
            const double alpha  = overlap > 1 ? 1 - std::pow(1 - target, 1 / overlap) : target;
            if (alpha * 255 < 0.5)
                continue;
            const Vector2D topLeft = a.p + delta * t - sp.hotspot - mon->m_position;
            copies.push_back({CBox{topLeft, sp.size}.scale(mon->m_scale).round(), sc<float>(std::min(alpha, 1.0))});
        }
    }
    return copies;
}

// Before a frame, outside rendering: the trail ages once per frame, on the monitor it is on, and the real cursor is
// hidden or brought back.
static void onRenderPre(PHLMONITOR mon) {
    if (!mon || mon != g_histMon.lock())
        return;
    if (!g_cfg.enabled->value())
        g_hideWanted = false;
    else
        ageTrail(nowMs());
    applyHide();
}

static void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_LAST_MOMENT)
        return;

    const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!mon || mon->isMirror())
        return;

    if (g_pngStale)
        loadPicture();

    if (!g_cfg.enabled->value()) {
        if (!g_hist.empty() || g_lastBox)
            clearTrail();
        if (g_hidden) // one more frame, so the cursor comes back before the next one
            g_pHyprRenderer->damageMonitor(mon);
        return;
    }

    const auto   sp  = sprite();
    const double now = nowMs();
    if (mon == g_histMon.lock()) {
        // Hyprland hiding the cursor (inactivity, typing) ends the trail too.
        const bool alive = sp && drawing() && g_hist.size() > 1 && g_pHyprRenderer->shouldRenderCursor() && now - g_hist.back().t < g_cfg.trailMs->value();
        // still fading: damaging it again brings the next frame; once it has faded, clear its area one last time
        if (alive)
            damageTrail(trailBox(*sp));
        else {
            if (g_lastBox)
                damageTrail(std::nullopt);
            if (g_hidden) // one more frame, so the cursor comes back before it
                g_pHyprRenderer->damageBox(Pointer::mgr()->getCursorBoxGlobal().expand(2));
            g_fastEnough = false;
            g_crossedAt  = 0;
            g_hideWanted = false;
            return;
        }
    }

    if (!sp || !g_lastBox || !g_lastBox->overlaps(mon->logicalBox()))
        return;

    auto copies = buildCopies(*sp, mon, now);
    if (!copies.empty())
        g_pHyprRenderer->m_renderPass.add(makeUnique<CPointerTrailPass>(sp->tex, std::move(copies)));
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(handle, "[pointer-trail] Built for a different Hyprland version, not loading", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[pointer-trail] Version mismatch");
    }

    g_cfg.enabled   = makeShared<CBoolValue>("plugin:pointer_trail:enabled", "draw the trail", true);
    g_cfg.trailMs   = makeShared<CIntValue>("plugin:pointer_trail:trail_length", "how long the trail is, in ms", 120, SIntValueOptions{.min = 10, .max = 1000});
    g_cfg.opacity   = makeShared<CFloatValue>("plugin:pointer_trail:opacity", "peak opacity of the trail, 0 - 1", 0.5F, SFloatValueOptions{.min = 0.F, .max = 1.F});
    g_cfg.imagePath = makeShared<CStringValue>("plugin:pointer_trail:cursor_image_path", "PNG to draw instead of the cursor (empty: the cursor)", "");
    g_cfg.minSpeed  = makeShared<CFloatValue>("plugin:pointer_trail:min_speed", "speed in logical px/s below which no trail is drawn (0: always)", 0.F,
                                              SFloatValueOptions{.min = 0.F});
    g_cfg.hideWhenFast = makeShared<CBoolValue>("plugin:pointer_trail:hide_when_fast", "hide the real cursor while the mouse is very fast", true);
    g_cfg.hideSpeed    = makeShared<CFloatValue>("plugin:pointer_trail:hide_speed", "speed in logical px/s above which the real cursor is hidden", 3000.F,
                                                 SFloatValueOptions{.min = 100.F});
    for (const auto& v :
         std::initializer_list<SP<IValue>>{g_cfg.enabled, g_cfg.trailMs, g_cfg.opacity, g_cfg.imagePath, g_cfg.minSpeed, g_cfg.hideWhenFast, g_cfg.hideSpeed})
        HyprlandAPI::addConfigValueV2(handle, v);

    for (const auto& fn : HyprlandAPI::findFunctionsByName(handle, "renderSoftwareCursorsFor")) {
        if (fn.demangled.contains("CPointerManager::renderSoftwareCursorsFor")) {
            g_swCursorHook = HyprlandAPI::createFunctionHook(handle, fn.address, (void*)&hkRenderSoftwareCursorsFor);
            break;
        }
    }
    if (!g_swCursorHook || !g_swCursorHook->hook()) {
        g_swCursorHook = nullptr;
        HyprlandAPI::addNotification(handle, "[pointer-trail] Can't hook the cursor drawing, the cursor won't be hidden when fast", CHyprColor{1.0, 0.6, 0.2, 1.0}, 5000);
    }

    g_onMove   = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { onMove(); });
    g_onPre    = Event::bus()->m_events.render.pre.listen([](PHLMONITOR mon) { onRenderPre(mon); });
    g_onStage  = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
    g_onReload = Event::bus()->m_events.config.reloaded.listen([] { g_pngStale = true; });

    return {"pointer-trail", "A motion blur for the mouse pointer, made of fading afterimages of your cursor", "alberic", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_onMove.reset();
    g_onPre.reset();
    g_onStage.reset();
    g_onReload.reset();
    g_pHyprRenderer->m_renderPass.removeAllOfType("CPointerTrailPass");
    clearTrail();
    applyHide(); // never leave the user with an invisible cursor
    if (g_swCursorHook)
        HyprlandAPI::removeFunctionHook(g_handle, g_swCursorHook);
    g_swCursorHook = nullptr;
    g_png.reset();
}
