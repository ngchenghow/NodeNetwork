// NodeNetwork - SDL2 force-directed graph viewer.
// Inspired by Obsidian-Node-Network: parse simple `a->predicate->b` lines
// and render an interactive force-directed graph.
//
// Controls:
//   Left-drag a node    pin and move it (release to unpin)
//   R                   reheat / randomize positions
//   Space               pause / resume simulation
//   Scroll              zoom
//   Middle-drag         pan
//   Esc / window close  quit

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Node {
    std::string id;
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    bool pinned = false;
};

struct Edge {
    int a;             // index into nodes
    int b;
    std::string label; // empty if no predicate
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::unordered_map<std::string, int> idIndex;

    int getOrCreate(const std::string& id) {
        auto it = idIndex.find(id);
        if (it != idIndex.end()) return it->second;
        int idx = static_cast<int>(nodes.size());
        Node n;
        n.id = id;
        nodes.push_back(n);
        idIndex[id] = idx;
        return idx;
    }
};

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> splitArrow(const std::string& line) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= line.size()) {
        size_t arrow = line.find("->", pos);
        if (arrow == std::string::npos) {
            parts.push_back(trim(line.substr(pos)));
            break;
        }
        parts.push_back(trim(line.substr(pos, arrow - pos)));
        pos = arrow + 2;
    }
    return parts;
}

static bool parseGraph(const std::string& text, Graph& g, std::string& err) {
    std::istringstream is(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(is, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#' || (t.size() >= 2 && t[0] == '/' && t[1] == '/')) continue;

        auto parts = splitArrow(t);
        if (parts.size() < 2) {
            err = "line " + std::to_string(lineNo) + ": expected `a->b` or `a->p->b`";
            return false;
        }
        for (auto& p : parts) {
            if (p.empty()) {
                err = "line " + std::to_string(lineNo) + ": empty token";
                return false;
            }
        }
        if (parts.size() == 2) {
            int a = g.getOrCreate(parts[0]);
            int b = g.getOrCreate(parts[1]);
            g.edges.push_back({a, b, ""});
        } else if (parts.size() == 3) {
            int a = g.getOrCreate(parts[0]);
            int b = g.getOrCreate(parts[2]);
            g.edges.push_back({a, b, parts[1]});
        } else {
            // chain a->b->c->d : treat consecutive pairs as edges (no labels)
            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                int a = g.getOrCreate(parts[i]);
                int b = g.getOrCreate(parts[i + 1]);
                g.edges.push_back({a, b, ""});
            }
        }
    }
    return true;
}

static const char* kDefaultGraph =
    "# Edit assets/graph.txt to change this graph, or pass a path on the CLI\n"
    "bird->color->red\n"
    "bird->color->black\n"
    "bird->name->crow\n"
    "bird->name->sparrow\n"
    "bird->habitat->forest\n"
    "bird->habitat->city\n"
    "crow->eats->seeds\n"
    "sparrow->eats->seeds\n"
    "sparrow->eats->insects\n"
    "forest->contains->trees\n"
    "trees->produce->seeds\n";

// ---------- rendering ----------

static void fillCircle(SDL_Renderer* r, int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = (int)std::floor(std::sqrt((float)(radius * radius - dy * dy)));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void strokeCircle(SDL_Renderer* r, int cx, int cy, int radius) {
    int x = radius, y = 0, err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(r, cx + x, cy + y);
        SDL_RenderDrawPoint(r, cx + y, cy + x);
        SDL_RenderDrawPoint(r, cx - y, cy + x);
        SDL_RenderDrawPoint(r, cx - x, cy + y);
        SDL_RenderDrawPoint(r, cx - x, cy - y);
        SDL_RenderDrawPoint(r, cx - y, cy - x);
        SDL_RenderDrawPoint(r, cx + y, cy - x);
        SDL_RenderDrawPoint(r, cx + x, cy - y);
        if (err <= 0) { ++y; err += 2 * y + 1; }
        if (err > 0)  { --x; err -= 2 * x + 1; }
    }
}

// Thicken a line by drawing parallel offsets. Cheap, looks OK at small widths.
static void drawThickLine(SDL_Renderer* r, float x1, float y1, float x2, float y2, int thickness) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    float nx = -dy / len, ny = dx / len;
    for (int i = -thickness / 2; i <= thickness / 2; ++i) {
        SDL_RenderDrawLine(r,
            (int)std::round(x1 + nx * i), (int)std::round(y1 + ny * i),
            (int)std::round(x2 + nx * i), (int)std::round(y2 + ny * i));
    }
}

struct TextCache {
    SDL_Renderer* renderer;
    TTF_Font* font;
    struct Entry { SDL_Texture* tex; int w, h; };
    std::unordered_map<std::string, Entry> map;

    ~TextCache() {
        for (auto& kv : map) SDL_DestroyTexture(kv.second.tex);
    }

    Entry get(const std::string& s, SDL_Color color) {
        // Cache key folds color in so we can reuse for dim labels too.
        char key[256];
        std::snprintf(key, sizeof(key), "%02x%02x%02x|%s", color.r, color.g, color.b, s.c_str());
        auto it = map.find(key);
        if (it != map.end()) return it->second;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), color);
        if (!surf) return {nullptr, 0, 0};
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
        Entry e{tex, surf->w, surf->h};
        SDL_FreeSurface(surf);
        map[key] = e;
        return e;
    }
};

static void drawText(SDL_Renderer* r, TextCache& cache, const std::string& s, int x, int y, SDL_Color color, bool centerX = true, bool centerY = true) {
    auto e = cache.get(s, color);
    if (!e.tex) return;
    SDL_Rect dst;
    dst.x = centerX ? x - e.w / 2 : x;
    dst.y = centerY ? y - e.h / 2 : y;
    dst.w = e.w;
    dst.h = e.h;
    SDL_RenderCopy(r, e.tex, nullptr, &dst);
}

// ---------- font discovery ----------

static const char* candidateFonts[] = {
    "assets/font.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
    "C:/Windows/Fonts/calibri.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    nullptr,
};

static TTF_Font* openAnyFont(int size) {
    for (int i = 0; candidateFonts[i]; ++i) {
        TTF_Font* f = TTF_OpenFont(candidateFonts[i], size);
        if (f) {
            std::printf("font: %s\n", candidateFonts[i]);
            return f;
        }
    }
    return nullptr;
}

// ---------- main ----------

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        std::fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    int winW = 1100, winH = 720;
    SDL_Window* win = SDL_CreateWindow("NodeNetwork",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        winW, winH, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        std::fprintf(stderr, "CreateWindow failed: %s\n", SDL_GetError());
        TTF_Quit(); SDL_Quit();
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        std::fprintf(stderr, "CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    TTF_Font* fontNode = openAnyFont(16);
    TTF_Font* fontEdge = openAnyFont(12);
    if (!fontNode || !fontEdge) {
        std::fprintf(stderr, "No usable TTF font found. Drop one at assets/font.ttf\n");
    }

    // Load graph: CLI path > assets/graph.txt > built-in default.
    std::string source;
    {
        std::string path;
        if (argc >= 2) path = argv[1];
        else path = "assets/graph.txt";
        std::ifstream f(path);
        if (f) {
            std::stringstream ss; ss << f.rdbuf();
            source = ss.str();
            std::printf("loaded graph: %s\n", path.c_str());
        } else {
            std::printf("graph file not found, using default\n");
            source = kDefaultGraph;
        }
    }

    Graph g;
    std::string err;
    if (!parseGraph(source, g, err)) {
        std::fprintf(stderr, "parse error: %s\n", err.c_str());
        // fall back to default so the window still shows something
        g = Graph();
        parseGraph(kDefaultGraph, g, err);
    }

    // initial layout: spread nodes on a circle
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> jitter(-30.f, 30.f);
    auto seedLayout = [&]() {
        int n = (int)g.nodes.size();
        if (n == 0) return;
        float cx = winW * 0.5f, cy = winH * 0.5f;
        float r = std::min(winW, winH) * 0.3f;
        for (int i = 0; i < n; ++i) {
            float a = (float)i / std::max(1, n) * 6.2831853f;
            g.nodes[i].x = cx + r * std::cos(a) + jitter(rng);
            g.nodes[i].y = cy + r * std::sin(a) + jitter(rng);
            g.nodes[i].vx = g.nodes[i].vy = 0;
            g.nodes[i].pinned = false;
        }
    };
    seedLayout();

    // Simulation parameters - tuned for ~50 nodes at 1100x720.
    struct {
        float repel = 6500.f;       // charge strength
        float springK = 0.04f;      // spring constant
        float springLen = 110.f;    // natural edge length
        float centerK = 0.005f;     // pull to center
        float damping = 0.85f;      // velocity decay per step
        float maxSpeed = 30.f;
        int nodeRadius = 14;
    } sim;

    float zoom = 1.0f;
    float panX = 0, panY = 0;
    bool paused = false;
    int draggingNode = -1;
    float dragOffX = 0, dragOffY = 0;
    bool panning = false;
    int panStartX = 0, panStartY = 0;
    float panStartPx = 0, panStartPy = 0;

    auto screenToWorld = [&](int sx, int sy, float& wx, float& wy) {
        wx = (sx - panX) / zoom;
        wy = (sy - panY) / zoom;
    };
    auto worldToScreen = [&](float wx, float wy, float& sx, float& sy) {
        sx = wx * zoom + panX;
        sy = wy * zoom + panY;
    };

    auto pickNode = [&](int sx, int sy) -> int {
        float wx, wy; screenToWorld(sx, sy, wx, wy);
        float bestD2 = 1e30f; int best = -1;
        float rr = (float)sim.nodeRadius * (float)sim.nodeRadius;
        for (int i = 0; i < (int)g.nodes.size(); ++i) {
            float dx = g.nodes[i].x - wx, dy = g.nodes[i].y - wy;
            float d2 = dx * dx + dy * dy;
            if (d2 < rr && d2 < bestD2) { bestD2 = d2; best = i; }
        }
        return best;
    };

    TextCache nodeText{ren, fontNode, {}};
    TextCache edgeText{ren, fontEdge, {}};

    SDL_Color colBg     = {18, 20, 24, 255};
    SDL_Color colEdge   = {110, 120, 140, 200};
    SDL_Color colEdgeLbl= {170, 180, 200, 220};
    SDL_Color colNode   = {130, 170, 240, 255};
    SDL_Color colNodeBd = {220, 230, 250, 255};
    SDL_Color colNodeLbl= {245, 248, 255, 255};
    SDL_Color colNodeHi = {255, 200, 110, 255};

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                    else if (ev.key.keysym.sym == SDLK_r) { seedLayout(); }
                    else if (ev.key.keysym.sym == SDLK_SPACE) paused = !paused;
                    break;
                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        winW = ev.window.data1;
                        winH = ev.window.data2;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        int idx = pickNode(ev.button.x, ev.button.y);
                        if (idx >= 0) {
                            draggingNode = idx;
                            g.nodes[idx].pinned = true;
                            float wx, wy; screenToWorld(ev.button.x, ev.button.y, wx, wy);
                            dragOffX = g.nodes[idx].x - wx;
                            dragOffY = g.nodes[idx].y - wy;
                        }
                    } else if (ev.button.button == SDL_BUTTON_MIDDLE || ev.button.button == SDL_BUTTON_RIGHT) {
                        panning = true;
                        panStartX = ev.button.x;
                        panStartY = ev.button.y;
                        panStartPx = panX;
                        panStartPy = panY;
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        if (draggingNode >= 0) {
                            g.nodes[draggingNode].pinned = false;
                            g.nodes[draggingNode].vx = 0;
                            g.nodes[draggingNode].vy = 0;
                            draggingNode = -1;
                        }
                    } else if (ev.button.button == SDL_BUTTON_MIDDLE || ev.button.button == SDL_BUTTON_RIGHT) {
                        panning = false;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (draggingNode >= 0) {
                        float wx, wy; screenToWorld(ev.motion.x, ev.motion.y, wx, wy);
                        g.nodes[draggingNode].x = wx + dragOffX;
                        g.nodes[draggingNode].y = wy + dragOffY;
                        g.nodes[draggingNode].vx = 0;
                        g.nodes[draggingNode].vy = 0;
                    }
                    if (panning) {
                        panX = panStartPx + (ev.motion.x - panStartX);
                        panY = panStartPy + (ev.motion.y - panStartY);
                    }
                    break;
                case SDL_MOUSEWHEEL: {
                    int mx, my; SDL_GetMouseState(&mx, &my);
                    float wx, wy; screenToWorld(mx, my, wx, wy);
                    float step = (ev.wheel.y > 0) ? 1.15f : (1.0f / 1.15f);
                    zoom *= step;
                    zoom = std::clamp(zoom, 0.2f, 4.0f);
                    panX = mx - wx * zoom;
                    panY = my - wy * zoom;
                    break; }
            }
        }

        // ---------- physics step ----------
        if (!paused && !g.nodes.empty()) {
            int n = (int)g.nodes.size();
            std::vector<float> fx(n, 0.f), fy(n, 0.f);

            // Repulsion (O(n^2) - fine up to a few hundred nodes)
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    float dx = g.nodes[i].x - g.nodes[j].x;
                    float dy = g.nodes[i].y - g.nodes[j].y;
                    float d2 = dx * dx + dy * dy + 0.01f;
                    float invD = 1.0f / std::sqrt(d2);
                    float f = sim.repel / d2;
                    fx[i] += f * dx * invD; fy[i] += f * dy * invD;
                    fx[j] -= f * dx * invD; fy[j] -= f * dy * invD;
                }
            }
            // Spring (edges)
            for (auto& e : g.edges) {
                float dx = g.nodes[e.b].x - g.nodes[e.a].x;
                float dy = g.nodes[e.b].y - g.nodes[e.a].y;
                float d = std::sqrt(dx * dx + dy * dy) + 1e-3f;
                float f = sim.springK * (d - sim.springLen);
                fx[e.a] += f * dx / d; fy[e.a] += f * dy / d;
                fx[e.b] -= f * dx / d; fy[e.b] -= f * dy / d;
            }
            // Centering
            float cx = winW * 0.5f, cy = winH * 0.5f;
            for (int i = 0; i < n; ++i) {
                fx[i] += (cx - g.nodes[i].x) * sim.centerK;
                fy[i] += (cy - g.nodes[i].y) * sim.centerK;
            }
            // Integrate
            for (int i = 0; i < n; ++i) {
                if (g.nodes[i].pinned) continue;
                g.nodes[i].vx = (g.nodes[i].vx + fx[i]) * sim.damping;
                g.nodes[i].vy = (g.nodes[i].vy + fy[i]) * sim.damping;
                float sp = std::sqrt(g.nodes[i].vx * g.nodes[i].vx + g.nodes[i].vy * g.nodes[i].vy);
                if (sp > sim.maxSpeed) {
                    g.nodes[i].vx *= sim.maxSpeed / sp;
                    g.nodes[i].vy *= sim.maxSpeed / sp;
                }
                g.nodes[i].x += g.nodes[i].vx;
                g.nodes[i].y += g.nodes[i].vy;
            }
        }

        // ---------- render ----------
        SDL_SetRenderDrawColor(ren, colBg.r, colBg.g, colBg.b, colBg.a);
        SDL_RenderClear(ren);

        int hovered = -1;
        {
            int mx, my; SDL_GetMouseState(&mx, &my);
            hovered = pickNode(mx, my);
        }

        // edges
        SDL_SetRenderDrawColor(ren, colEdge.r, colEdge.g, colEdge.b, colEdge.a);
        for (auto& e : g.edges) {
            float ax, ay, bx, by;
            worldToScreen(g.nodes[e.a].x, g.nodes[e.a].y, ax, ay);
            worldToScreen(g.nodes[e.b].x, g.nodes[e.b].y, bx, by);
            drawThickLine(ren, ax, ay, bx, by, 2);
        }
        // edge labels
        for (auto& e : g.edges) {
            if (e.label.empty()) continue;
            float ax, ay, bx, by;
            worldToScreen(g.nodes[e.a].x, g.nodes[e.a].y, ax, ay);
            worldToScreen(g.nodes[e.b].x, g.nodes[e.b].y, bx, by);
            int mx = (int)((ax + bx) * 0.5f);
            int my = (int)((ay + by) * 0.5f);
            if (fontEdge) drawText(ren, edgeText, e.label, mx, my, colEdgeLbl);
        }

        // nodes
        for (int i = 0; i < (int)g.nodes.size(); ++i) {
            float sx, sy;
            worldToScreen(g.nodes[i].x, g.nodes[i].y, sx, sy);
            int radius = (int)std::round(sim.nodeRadius * zoom);
            if (radius < 4) radius = 4;
            bool hi = (i == hovered || i == draggingNode);
            SDL_Color fill = hi ? colNodeHi : colNode;
            SDL_SetRenderDrawColor(ren, fill.r, fill.g, fill.b, fill.a);
            fillCircle(ren, (int)sx, (int)sy, radius);
            SDL_SetRenderDrawColor(ren, colNodeBd.r, colNodeBd.g, colNodeBd.b, colNodeBd.a);
            strokeCircle(ren, (int)sx, (int)sy, radius);
            if (fontNode) drawText(ren, nodeText, g.nodes[i].id, (int)sx, (int)sy - radius - 12, colNodeLbl);
        }

        // status line
        if (fontEdge) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "nodes:%d  edges:%d  zoom:%.2f  %s  [R reheat] [Space pause] [scroll zoom] [middle-drag pan]",
                (int)g.nodes.size(), (int)g.edges.size(), zoom, paused ? "PAUSED" : "running");
            drawText(ren, edgeText, buf, 10, winH - 18, colEdgeLbl, false, false);
        }

        SDL_RenderPresent(ren);
    }

    if (fontNode) TTF_CloseFont(fontNode);
    if (fontEdge) TTF_CloseFont(fontEdge);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
