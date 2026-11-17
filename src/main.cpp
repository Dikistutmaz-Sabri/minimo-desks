// minimo-desks: a Stage-Manager-like desk strip for Hyprland 0.57.2 (Minimo OS).
//   The cursor pushed against a monitor's left edge slides a strip in: one card per workspace
//   (1..3, or up to the highest one in use), with small pictures of its windows. A card pressed:
//   that desk. The cursor leaving the strip: it slides away.
//   Every desk change "swoops" (the old desk flies out on an arc, the new one comes in) and
//   moonlight ripples spread from the middle of the screen.
//   hyprctl desks swoop on|off|toggle    hyprctl desks ripple on|off|toggle
//   hyprctl desks show|hide|status       hyprctl desks click   (a press at the cursor, for tests)

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/ipc/s1/S1.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace Render;
using Clock = std::chrono::steady_clock;

static HANDLE           PHANDLE = nullptr;
static bool             g_swoopOn = true, g_rippleOn = true;
static constexpr double STRIP_W = 240, CARD_W = 196, CARD_GAP = 18, CARD_ROUND = 14; // logical px
static constexpr double SLIDE_S = 0.22, SWOOP_S = 0.55, RIPPLE_S = 1.6;              // seconds
static constexpr int    MIN_DESKS = 3;                                                  // Minimo's masaustu.lua

static double since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}
static double ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

// ── state ──
struct {
    PHLMONITORREF     mon;
    bool              open = false;
    Clock::time_point at;
    double            from = 0; // shown amount when the last slide began
    int               hover = -1;
    double shown() const {
        const double T = std::clamp(since(at) / SLIDE_S, 0.0, 1.0);
        return std::lerp(from, open ? 1.0 : 0.0, ease(T));
    }
    bool sliding() const {
        return since(at) < SLIDE_S;
    }
} g_strip;

struct SPic {
    SP<ITexture> tex;
    CBox         box; // global logical
    float        round = 0;
};
struct SSwoop {
    PHLMONITORREF     mon;
    std::vector<SPic> pics; // the old desk's windows
    double            dir = 1; // +1: to a higher desk (flies out to the left)
    Clock::time_point at;
};
struct SRipple {
    PHLMONITORREF     mon;
    Clock::time_point at;
};
static std::vector<SSwoop>                       g_swoops;
static std::vector<SRipple>                      g_ripples;
static std::unordered_map<void*, PHLWORKSPACEREF> g_lastDesk; // monitor → its desk before the change
static std::vector<CHyprSignalListener>          g_listeners;
static SP<IPC::Socket1::SCommand>                g_cmd;

// ── desks and their windows ──
// a desk's number (Minimo's desks are numbered); -1 for named/special ones
static int64_t num(const PHLWORKSPACE& ws) {
    const auto N = ws ? ws->numberedID() : std::nullopt;
    return N ? sc<int64_t>(*N) : -1;
}

static std::vector<int64_t> deskIDs() {
    int64_t top = MIN_DESKS;
    for (const auto& w : Desktop::windowState()->windows())
        if (w && w->m_workspace && w->m_workspace->type() == Workspace::eWorkspaceType::NORMAL)
            top = std::max(top, num(w->m_workspace));
    std::vector<int64_t> ids;
    for (int64_t i = 1; i <= std::min<int64_t>(top, 9); ++i)
        ids.push_back(i);
    return ids;
}

static std::vector<SPic> picsOf(std::function<bool(const PHLWORKSPACE&)> pick) {
    std::vector<SPic> pics;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->mapped() || w->isHidden() || !w->m_workspace || !pick(w->m_workspace))
            continue;
        const auto SURF = w->wlSurface() ? w->wlSurface()->resource() : nullptr;
        if (!SURF || !SURF->m_current.texture)
            continue;
        // shortcut: the main surface only (no subsurfaces/popups, no title bar); good enough for small pictures
        pics.push_back({SURF->m_current.texture, CBox{w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)},
                        w->presentation().rounding()});
    }
    return pics;
}

static PHLMONITOR monitorAt(const Vector2D& p) {
    for (const auto& m : State::monitorState()->monitors())
        if (m && CBox{m->m_position, m->m_size}.containsPoint(p))
            return m;
    return nullptr;
}

// card i of n on mon, global logical, with the strip shown by `shown`
static CBox cardBox(PHLMONITOR mon, size_t i, size_t n, double shown) {
    const double H     = CARD_W * mon->m_size.y / mon->m_size.x;
    const double TOTAL = n * H + (n - 1) * CARD_GAP;
    const double X     = mon->m_position.x - STRIP_W + STRIP_W * shown + (STRIP_W - CARD_W) / 2;
    return {X, mon->m_position.y + (mon->m_size.y - TOTAL) / 2 + i * (H + CARD_GAP), CARD_W, H};
}

static int cardAt(const Vector2D& p) {
    const auto MON = g_strip.mon.lock();
    if (!MON || !g_strip.open)
        return -1;
    const auto IDS = deskIDs();
    for (size_t i = 0; i < IDS.size(); ++i)
        if (cardBox(MON, i, IDS.size(), 1).containsPoint(p))
            return i;
    return -1;
}

static void kick(PHLMONITOR mon) {
    if (mon) {
        g_pHyprRenderer->damageMonitor(mon);
        mon->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_ANIMATION);
    }
    if (Animation::mgr())
        Animation::mgr()->requestTick();
}

static void openStrip(PHLMONITOR mon, bool open) {
    if (!mon || (g_strip.open == open && g_strip.mon == mon))
        return;
    g_strip.from  = g_strip.mon == mon ? g_strip.shown() : 0.0;
    g_strip.mon   = mon;
    g_strip.open  = open;
    g_strip.at    = Clock::now();
    g_strip.hover = -1;
    kick(mon);
}

static void goToDesk(int64_t id) {
    HyprlandAPI::invokeHyprctlCommand("dispatch", std::format("hl.dsp.focus({{ workspace = \"{}\" }})", id));
}

// ── drawing (monitor-local pixels) ──
static CBox px(PHLMONITOR mon, const CBox& global) {
    return global.copy().translate(-mon->m_position).scale(mon->m_scale);
}

static UP<IPassElement> rect(const CBox& box, CHyprColor c, int round) {
    CRectPassElement::SRectData d;
    d.box   = box;
    d.color = c;
    d.round = round;
    return makeUnique<CRectPassElement>(d);
}

static UP<IPassElement> tex(PHLMONITOR mon, SP<ITexture> t, const CBox& box, float a, int round, const CBox& clip = {}) {
    CTexPassElement::SRenderData d;
    d.tex    = t;
    d.box    = box;
    d.a      = a;
    d.round  = round;
    d.clipBox = clip;
    d.damage = CRegion{0, 0, sc<int>(mon->m_transformedSize.x), sc<int>(mon->m_transformedSize.y)};
    return makeUnique<CTexPassElement>(std::move(d));
}

static UP<IPassElement> ring(const CBox& box, std::vector<CHyprColor>&& cols, int size, float a, int round = -1) {
    CBorderPassElement::SBorderData d;
    d.box        = box;
    d.grad1      = Config::CGradientValueData(std::move(cols), 0.8F);
    d.a          = a;
    d.round      = round < 0 ? sc<int>(box.w / 2) : round; // a circle unless told
    d.borderSize = size;
    return makeUnique<CBorderPassElement>(d);
}

// A list of Hyprland's own elements, made fresh each frame.
class CDesksElement : public IPassElement {
  public:
    explicit CDesksElement(std::function<std::vector<UP<IPassElement>>(PHLMONITOR)> make) : m_make(std::move(make)) {}
    std::vector<UP<IPassElement>> draw(CRenderContext& ctx) override {
        const auto MON = ctx.m_data.pMonitor.lock();
        return MON ? m_make(MON) : std::vector<UP<IPassElement>>{};
    }
    bool needsLiveBlur(CRenderContext&) override {
        return false;
    }
    bool needsPrecomputeBlur(CRenderContext&) override {
        return false;
    }
    bool disableSimplification(CRenderContext&) override {
        return true;
    }
    const char* passName() override {
        return "CDesksElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    std::function<std::vector<UP<IPassElement>>(PHLMONITOR)> m_make;
};

static std::vector<UP<IPassElement>> drawSwoops(PHLMONITOR mon) {
    std::vector<UP<IPassElement>> out;
    for (const auto& s : g_swoops) {
        if (s.mon != mon)
            continue;
        const double P = ease(since(s.at) / SWOOP_S);
        // an arc: up and sideways, then down and away, shrinking towards the strip's side
        const Vector2D CTR   = mon->m_position + mon->m_size / 2;
        const double   SCALE = 1.0 - 0.5 * P;
        const Vector2D OFF   = {-s.dir * mon->m_size.x * 0.62 * P, -mon->m_size.y * 0.28 * std::sin(std::numbers::pi * P) + mon->m_size.y * 0.12 * P * P};
        for (const auto& p : s.pics) {
            const CBox B{CTR + (p.box.pos() - CTR) * SCALE + OFF, p.box.size() * SCALE};
            out.push_back(tex(mon, p.tex, px(mon, B), sc<float>(1.0 - P * P), sc<int>(p.round * SCALE * mon->m_scale)));
        }
    }
    return out;
}

static std::vector<UP<IPassElement>> drawRipples(PHLMONITOR mon) {
    std::vector<UP<IPassElement>> out;
    const double S = mon->m_scale;
    const double R = 0.6 * std::hypot(mon->m_size.x, mon->m_size.y);
    for (const auto& r : g_ripples) {
        if (r.mon != mon)
            continue;
        const double T = since(r.at);
        for (int k = 0; k < 4; ++k) {
            const double Q = std::clamp((T - k * 0.14) / (RIPPLE_S - 0.42), 0.0, 1.0);
            if (Q <= 0 || Q >= 1)
                continue;
            const double   RAD     = 12 + R * (1 - std::pow(1 - Q, 2.2)); // fast, then slower, like water
            const double   SHIMMER = 0.85 + 0.15 * std::sin(T * 23 + k * 1.7);
            const float    A       = sc<float>(std::pow(1 - Q, 1.6) * SHIMMER * (k == 0 ? 0.55 : 0.4));
            const Vector2D C       = mon->m_position + mon->m_size / 2;
            const CBox     BOX     = px(mon, {C.x - RAD, C.y - RAD, RAD * 2, RAD * 2});
            // a wide faint glow, then a thin bright silver-blue line
            out.push_back(ring(BOX.copy().expand(-6 * S), {CHyprColor{0.55, 0.70, 1.0, 1.0}, CHyprColor{0.75, 0.85, 1.0, 1.0}}, sc<int>(14 * S), A * 0.22F));
            out.push_back(ring(BOX, {CHyprColor{0.92, 0.95, 1.0, 1.0}, CHyprColor{0.62, 0.76, 0.98, 1.0}, CHyprColor{0.85, 0.90, 1.0, 1.0}}, sc<int>(std::max(1.0, 2 * S)), A));
        }
    }
    return out;
}

static std::vector<UP<IPassElement>> drawStrip(PHLMONITOR mon) {
    std::vector<UP<IPassElement>> out;
    const double SHOWN = g_strip.shown();
    if (g_strip.mon != mon || SHOWN <= 0.001)
        return out;
    const double S = mon->m_scale;
    out.push_back(rect(px(mon, {mon->m_position.x - STRIP_W + STRIP_W * SHOWN, mon->m_position.y, STRIP_W, mon->m_size.y}), CHyprColor{0.07, 0.09, 0.11, 0.80 * SHOWN}, 0));

    const auto IDS    = deskIDs();
    const auto ACTIVE = num(mon->m_activeWorkspace);
    for (size_t i = 0; i < IDS.size(); ++i) {
        const CBox CARD = cardBox(mon, i, IDS.size(), SHOWN);
        out.push_back(rect(px(mon, CARD), i == sc<size_t>(g_strip.hover) ? CHyprColor{0.24, 0.29, 0.34, 0.97} : CHyprColor{0.15, 0.18, 0.21, 0.95}, sc<int>(CARD_ROUND * S)));
        // its windows, where they are on the screen, small
        const double K = CARD.w / mon->m_size.x;
        for (const auto& p : picsOf([&](const PHLWORKSPACE& ws) { return num(ws) == IDS[i]; })) {
            const CBox B{CARD.x + (p.box.x - mon->m_position.x) * K, CARD.y + (p.box.y - mon->m_position.y) * K, p.box.w * K, p.box.h * K}; // NOLINT
            out.push_back(tex(mon, p.tex, px(mon, B), 1.F, sc<int>(4 * S), px(mon, CARD))); // kept inside its card
        }
        if (IDS[i] == ACTIVE)
            out.push_back(ring(px(mon, CARD), {CHyprColor{0.62, 0.84, 0.74, 1.0}}, sc<int>(2 * S), 1.F, sc<int>(CARD_ROUND * S))); // Minimo's mint
    }
    return out;
}

// ── events ──
static void onTick() {
    std::vector<PHLMONITOR> mons;
    for (const auto& s : g_swoops)
        mons.push_back(s.mon.lock());
    for (const auto& r : g_ripples)
        mons.push_back(r.mon.lock());
    if (g_strip.sliding())
        mons.push_back(g_strip.mon.lock());
    if (mons.empty())
        return;
    std::erase_if(g_swoops, [](const SSwoop& s) { return since(s.at) > SWOOP_S || !s.mon; });
    std::erase_if(g_ripples, [](const SRipple& r) { return since(r.at) > RIPPLE_S || !r.mon; });
    for (auto& m : mons)
        kick(m); // also once after the last frame, to wipe it
}

static void onRenderStage(const Event::SRenderStageEvent& ev) {
    if (!ev.context || !ev.monitor)
        return;
    // the old desk flies over the new one's windows; ripples and the strip over everything
    if (ev.stage == RENDER_POST_WINDOWS && !g_swoops.empty())
        IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CDesksElement>(drawSwoops));
    if (ev.stage == RENDER_LAST_MOMENT && (!g_ripples.empty() || g_strip.mon == ev.monitor))
        IHyprRenderer::addPassElement(ev.context->get(), makeUnique<CDesksElement>([](PHLMONITOR m) {
                                          auto out = drawRipples(m);
                                          for (auto& e : drawStrip(m))
                                              out.push_back(std::move(e));
                                          return out;
                                      }));
}

static void onDeskChanged(PHLWORKSPACE ws) {
    if (!ws || ws->type() != Workspace::eWorkspaceType::NORMAL)
        return;
    const auto MON = ws->m_monitor.lock();
    if (!MON)
        return;
    const auto OLD     = g_lastDesk[MON.get()].lock();
    g_lastDesk[MON.get()] = ws;
    if (!OLD || OLD == ws || OLD->m_monitor != MON)
        return;

    const double DIR = num(ws) > num(OLD) ? 1.0 : -1.0;
    if (g_swoopOn) {
        g_swoops.push_back({MON, picsOf([&](const PHLWORKSPACE& w) { return w == OLD; }), DIR, Clock::now()});
        // Hyprland's own slide of the old desk ends now: our pictures fly instead
        OLD->m_renderOffset->warp();
        OLD->m_alpha->warp();
        // the new desk comes in from the other side, low, fading in (Hyprland's "workspaces" curve and speed)
        ws->m_renderOffset->setValueAndWarp(Vector2D{DIR * MON->m_size.x * 0.30, MON->m_size.y * 0.10});
        *ws->m_renderOffset = Vector2D{};
        ws->m_alpha->setValueAndWarp(0.F);
        *ws->m_alpha = 1.F;
    }
    if (g_rippleOn)
        g_ripples.push_back({MON, Clock::now()});
    // quick changes in a row: only the latest two of each, so it does not turn into noise
    while (g_swoops.size() > 2)
        g_swoops.erase(g_swoops.begin());
    while (g_ripples.size() > 2)
        g_ripples.erase(g_ripples.begin());
    kick(MON);
}

static void onMouseMove(const Vector2D& pos) {
    const auto MON = monitorAt(pos);
    if (!MON)
        return;
    if (!g_strip.open && pos.x <= MON->m_position.x + 1)
        openStrip(MON, true);
    else if (g_strip.open && (MON != g_strip.mon || pos.x > MON->m_position.x + STRIP_W + 8))
        openStrip(g_strip.mon.lock(), false);
    if (g_strip.open) {
        const int H = cardAt(pos);
        if (H != g_strip.hover) {
            g_strip.hover = H;
            kick(MON);
        }
    }
}

// true: the press was the strip's
static bool onPress(const Vector2D& pos) {
    const auto MON = g_strip.mon.lock();
    if (!g_strip.open || !MON || pos.x > MON->m_position.x + STRIP_W || !MON->logicalBox().containsPoint(pos))
        return false;
    const int  I   = cardAt(pos);
    const auto IDS = deskIDs();
    if (I >= 0 && I < sc<int>(IDS.size()))
        goToDesk(IDS[I]);
    return true;
}

static std::string onCommand(const std::string& line) {
    std::istringstream is(line);
    std::string        cmd, verb, arg;
    is >> cmd >> verb >> arg;
    const auto SET = [&](bool& v) {
        v = arg == "on" ? true : arg == "off" ? false : arg == "toggle" ? !v : v;
        return std::string{v ? "on" : "off"};
    };
    if (verb == "swoop")
        return "swoop " + SET(g_swoopOn);
    if (verb == "ripple")
        return "ripple " + SET(g_rippleOn);
    if (verb == "show" || verb == "hide") {
        const auto MON = monitorAt(g_pInputManager->getMouseCoordsInternal());
        openStrip(verb == "show" ? MON : g_strip.mon.lock(), verb == "show");
        return "ok";
    }
    if (verb == "click")
        return onPress(g_pInputManager->getMouseCoordsInternal()) ? "ok" : "not on the strip";
    if (verb == "status")
        return std::format("swoop {} ripple {} strip {}", g_swoopOn ? "on" : "off", g_rippleOn ? "on" : "off", g_strip.open ? "open" : "closed");
    return "usage: hyprctl desks swoop|ripple on|off|toggle, desks show|hide|status|click";
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[minimo-desks] built for another Hyprland: rebuild it (make)", CHyprColor{1.0, 0.3, 0.3, 1.0}, 8000);
        throw std::runtime_error("[minimo-desks] version mismatch");
    }

    // the desks there are now, so the first change already knows where it came from
    for (const auto& m : State::monitorState()->monitors())
        if (m && m->m_activeWorkspace)
            g_lastDesk[m.get()] = m->m_activeWorkspace;

    auto& E = Event::bus()->m_events;
    g_listeners.push_back(E.tick.listen([] { onTick(); }));
    g_listeners.push_back(E.render.stage.listen([](const Event::SRenderStageEvent& ev) { onRenderStage(ev); }));
    g_listeners.push_back(E.workspace.active.listen([](PHLWORKSPACE ws) { onDeskChanged(ws); }));
    g_listeners.push_back(E.input.mouse.move.listen([](const Vector2D& pos, Event::SCallbackInfo&) { onMouseMove(pos); }));
    g_listeners.push_back(E.input.mouse.button.listen([](const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        const auto POS = g_pInputManager->getMouseCoordsInternal();
        const auto MON = g_strip.mon.lock();
        if (!g_strip.open || !MON || POS.x > MON->m_position.x + STRIP_W)
            return;
        info.cancelled = true; // the strip's, not the window's under it
        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            onPress(POS);
    }));

    g_cmd = HyprlandAPI::registerHyprCtlCommand(PHANDLE, IPC::Socket1::SCommand{
                                                             .name    = "desks",
                                                             .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
                                                             .handler = [](const IPC::Socket1::SRequest& r) -> IPC::Socket1::SResponse { return onCommand(r.command); },
                                                         });

    return {"minimo-desks", "Stage-Manager-like desk strip, swoop and moonlight ripples", "minimo", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();
    if (g_cmd)
        HyprlandAPI::unregisterHyprCtlCommand(PHANDLE, g_cmd);
    g_swoops.clear();
    g_ripples.clear();
    g_lastDesk.clear();
    const auto MON = g_strip.mon.lock();
    g_strip.open   = false;
    g_strip.mon.reset();
    if (MON)
        g_pHyprRenderer->damageMonitor(MON);
}
