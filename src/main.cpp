// NodeNetwork - SDL2 force-directed graph viewer.
// Inspired by Obsidian-Node-Network: parse `a->b->c` lines + directives
// (highlight, hover, path, join) and render an interactive graph where
// directive targets are lit and the rest is dimmed.
//
// Controls:
//   Left-drag a node    pin and move it (release to unpin)
//   Mouse hover         transient highlight: chain-head BFS from the hovered node
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
#include <functional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---------- graph model ----------

struct Node {
    std::string id;
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    bool pinned = false;
};

struct Edge {
    int a;       // index into nodes
    int b;
    bool derived = false; // true if added (or marked) by a fired rule
};

// ---------- rules ----------

// A term in a rule pattern is either a variable (when it starts with an
// uppercase ASCII letter) or a constant (any other token). Variables are
// captured by name and unified across all patterns in a rule.
struct Term {
    bool isVar = false;
    std::string name;
};

struct TriplePattern {
    Term s, p, o;
};

struct Rule {
    std::vector<TriplePattern> antecedent;
    std::vector<TriplePattern> consequent;
    int sourceLine = 0;
};

// A Chain is one source line, e.g. `bird->will->fly` becomes
// nodeIds=[bird,will,fly], edgeIndices=[bird->will, will->fly]. Chains are
// the unit of "lighting up" — touching any node in a chain lights the whole
// chain when used as a directive scope.
struct Chain {
    std::vector<int> nodeIds;
    std::vector<int> edgeIndices;
};

enum class Show { ShortestAll, ShortestOne, AllSimple };
enum class Search { Node, Edge };
enum class JoinMode { Union, Intersect };

struct HighlightDir { std::vector<std::string> ids; };
struct HoverDir     { std::vector<std::string> ids; };
struct PathDir {
    std::string from;
    std::string to;
    std::vector<std::string> includes;
    Show show     = Show::ShortestAll;
    Search search = Search::Node;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::unordered_map<std::string, int> idIndex;
    // Directed edge index for dedup: pack (a, b) into 64 bits.
    std::unordered_map<uint64_t, int> edgeIndexByPair;
    std::vector<Chain> chains;

    std::vector<HighlightDir> highlights;
    std::vector<HoverDir>     hovers;
    std::vector<PathDir>      paths;
    JoinMode                  joinMode = JoinMode::Union;

    std::vector<Rule>         rules;
    // Index in `chains` where rule-derived chains start. Everything before
    // this came directly from source lines.
    int                       firstDerivedChain = -1;

    std::vector<std::string>  parseErrors;

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

    int ensureEdge(int a, int b) {
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32)
                     |  static_cast<uint64_t>(static_cast<uint32_t>(b));
        auto it = edgeIndexByPair.find(key);
        if (it != edgeIndexByPair.end()) return it->second;
        int idx = static_cast<int>(edges.size());
        edges.push_back({a, b});
        edgeIndexByPair[key] = idx;
        return idx;
    }

    int findId(const std::string& id) const {
        auto it = idIndex.find(id);
        return it == idIndex.end() ? -1 : it->second;
    }
};

// ---------- parsing helpers ----------

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

static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(',', pos);
        if (c == std::string::npos) {
            std::string t = trim(s.substr(pos));
            if (!t.empty()) out.push_back(t);
            break;
        }
        std::string t = trim(s.substr(pos, c - pos));
        if (!t.empty()) out.push_back(t);
        pos = c + 1;
    }
    return out;
}

// Parse a single line, returning true if it consumed the line as a directive.
// Adds errors to g.parseErrors on malformed directives.
static bool tryDirective(Graph& g, const std::string& t, int lineNo) {
    auto colon = t.find(':');
    if (colon == std::string::npos) return false;
    std::string head = trim(t.substr(0, colon));
    std::string rest = trim(t.substr(colon + 1));
    std::string headLower = head;
    for (auto& c : headLower) c = (char)std::tolower((unsigned char)c);

    auto pushErr = [&](const std::string& m) {
        g.parseErrors.push_back("line " + std::to_string(lineNo) + ": " + m);
    };

    if (headLower == "join") {
        std::string v = rest;
        for (auto& c : v) c = (char)std::tolower((unsigned char)c);
        if (v == "union")          g.joinMode = JoinMode::Union;
        else if (v == "intersect") g.joinMode = JoinMode::Intersect;
        else pushErr("join expects 'union' or 'intersect'");
        return true;
    }
    if (headLower == "highlight") {
        HighlightDir d;
        d.ids = splitComma(rest);
        if (!d.ids.empty()) g.highlights.push_back(std::move(d));
        return true;
    }
    if (headLower == "hover") {
        HoverDir d;
        d.ids = splitComma(rest);
        if (!d.ids.empty()) g.hovers.push_back(std::move(d));
        return true;
    }
    if (headLower == "path") {
        auto segs = splitComma(rest);
        if (segs.empty()) { pushErr("path: expects <from>-><to>"); return true; }
        auto ends = splitArrow(segs[0]);
        if (ends.size() != 2 || ends[0].empty() || ends[1].empty()) {
            pushErr("path: expects <from>-><to>");
            return true;
        }
        PathDir p;
        p.from = ends[0];
        p.to   = ends[1];
        for (size_t i = 1; i < segs.size(); ++i) {
            auto kc = segs[i].find(':');
            if (kc == std::string::npos) { pushErr("path: bad modifier \"" + segs[i] + "\""); continue; }
            std::string k = trim(segs[i].substr(0, kc));
            std::string v = trim(segs[i].substr(kc + 1));
            std::string kl = k, vl = v;
            for (auto& c : kl) c = (char)std::tolower((unsigned char)c);
            for (auto& c : vl) c = (char)std::tolower((unsigned char)c);
            if (kl == "includes") {
                if (!v.empty()) p.includes.push_back(v);
            } else if (kl == "show") {
                if (vl == "all")           p.show = Show::AllSimple;
                else if (vl == "shortest") p.show = Show::ShortestAll;
                else pushErr("show: expects 'all' or 'shortest'");
            } else if (kl == "search") {
                if (vl == "node")      p.search = Search::Node;
                else if (vl == "edge") p.search = Search::Edge;
                else pushErr("search: expects 'node' or 'edge'");
            } else if (kl == "shortest") {
                if (vl == "all")      p.show = Show::ShortestAll;
                else if (vl == "one") p.show = Show::ShortestOne;
                else pushErr("shortest: expects 'all' or 'one'");
            } else {
                pushErr("path: unknown modifier \"" + segs[i] + "\"");
            }
        }
        g.paths.push_back(std::move(p));
        return true;
    }
    return false;
}

// Case-insensitive equality against a single keyword.
static bool ieq(const std::string& a, const char* b) {
    size_t n = 0; while (b[n]) ++n;
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

// Split `text` on the case-insensitive token `sep` (matched as a whole word).
static std::vector<std::string> splitWord(const std::string& text, const char* sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    size_t sn = 0; while (sep[sn]) ++sn;
    while (pos <= text.size()) {
        // find next occurrence of `sep` as a whole word
        size_t hit = std::string::npos;
        for (size_t i = pos; i + sn <= text.size(); ++i) {
            bool ok = true;
            for (size_t k = 0; k < sn; ++k) {
                if (std::tolower((unsigned char)text[i + k]) != std::tolower((unsigned char)sep[k])) { ok = false; break; }
            }
            if (!ok) continue;
            // word boundaries: not preceded/followed by an alpha char
            bool leftOk  = (i == 0)               || !std::isalnum((unsigned char)text[i - 1]);
            bool rightOk = (i + sn == text.size()) || !std::isalnum((unsigned char)text[i + sn]);
            if (leftOk && rightOk) { hit = i; break; }
        }
        if (hit == std::string::npos) {
            out.push_back(trim(text.substr(pos)));
            break;
        }
        out.push_back(trim(text.substr(pos, hit - pos)));
        pos = hit + sn;
    }
    return out;
}

static Term makeTerm(const std::string& tok) {
    Term t;
    t.name = tok;
    // Variable = first character is an ASCII uppercase letter.
    if (!tok.empty() && tok[0] >= 'A' && tok[0] <= 'Z') t.isVar = true;
    return t;
}

// Parse a single `s->p->o` triple pattern. Returns true on success.
static bool parseTriplePattern(const std::string& src, TriplePattern& out, std::string& err) {
    auto parts = splitArrow(src);
    if (parts.size() != 3) { err = "expected `s->p->o`, got `" + src + "`"; return false; }
    for (auto& p : parts) if (p.empty()) { err = "empty term in `" + src + "`"; return false; }
    out.s = makeTerm(parts[0]);
    out.p = makeTerm(parts[1]);
    out.o = makeTerm(parts[2]);
    return true;
}

// Append the triple patterns parsed from one line (possibly containing `AND`)
// into `dst`. Records errors against `lineNo`.
static void parsePatternLine(Graph& g, std::vector<TriplePattern>& dst,
                             const std::string& line, int lineNo) {
    auto pieces = splitWord(line, "AND");
    for (auto& piece : pieces) {
        if (piece.empty()) continue;
        TriplePattern tp;
        std::string err;
        if (parseTriplePattern(piece, tp, err))
            dst.push_back(tp);
        else
            g.parseErrors.push_back("line " + std::to_string(lineNo) + ": " + err);
    }
}

static bool parseGraph(const std::string& text, Graph& g) {
    std::istringstream is(text);
    std::string line;
    int lineNo = 0;

    enum class RuleState { None, Antecedent, Consequent };
    RuleState rs = RuleState::None;
    Rule current;
    int currentStartLine = 0;

    auto flushRule = [&]() {
        if (rs == RuleState::None) return;
        if (current.antecedent.empty() || current.consequent.empty()) {
            g.parseErrors.push_back("line " + std::to_string(currentStartLine) +
                ": rule needs at least one antecedent and one consequent");
        } else {
            current.sourceLine = currentStartLine;
            g.rules.push_back(std::move(current));
        }
        current = Rule();
        rs = RuleState::None;
    };

    while (std::getline(is, line)) {
        ++lineNo;
        std::string t = trim(line);

        // A blank line terminates an in-progress rule but is otherwise ignored.
        if (t.empty()) { flushRule(); continue; }

        if (t[0] == '#' || (t.size() >= 2 && t[0] == '/' && t[1] == '/')) continue;

        // Keyword detection (case-insensitive).
        if (ieq(t, "IF")) {
            if (rs != RuleState::None) flushRule();
            rs = RuleState::Antecedent;
            currentStartLine = lineNo;
            continue;
        }
        if (ieq(t, "THEN")) {
            if (rs == RuleState::Antecedent) {
                rs = RuleState::Consequent;
            } else {
                g.parseErrors.push_back("line " + std::to_string(lineNo) + ": THEN without IF");
            }
            continue;
        }

        if (rs == RuleState::Antecedent) {
            parsePatternLine(g, current.antecedent, t, lineNo);
            continue;
        }
        if (rs == RuleState::Consequent) {
            parsePatternLine(g, current.consequent, t, lineNo);
            continue;
        }

        // Outside of any rule: try directives, then chain syntax.
        if (tryDirective(g, t, lineNo)) continue;

        auto parts = splitArrow(t);
        if (parts.size() < 2) {
            g.parseErrors.push_back("line " + std::to_string(lineNo) + ": expected `a->b` or `a->b->c->...`");
            continue;
        }
        bool bad = false;
        for (auto& p : parts) if (p.empty()) { bad = true; break; }
        if (bad) {
            g.parseErrors.push_back("line " + std::to_string(lineNo) + ": empty token");
            continue;
        }
        Chain c;
        for (auto& p : parts) c.nodeIds.push_back(g.getOrCreate(p));
        for (size_t i = 0; i + 1 < c.nodeIds.size(); ++i) {
            int ei = g.ensureEdge(c.nodeIds[i], c.nodeIds[i + 1]);
            c.edgeIndices.push_back(ei);
        }
        g.chains.push_back(std::move(c));
    }
    flushRule(); // EOF inside a rule block
    return g.parseErrors.empty();
}

// ---------- rule evaluation ----------

// Resolve a term to a node id under the current binding. Returns -1 if the
// term is an unbound variable or names a constant absent from the graph.
static int resolveTerm(const Graph& g, const std::unordered_map<std::string, int>& binding,
                       const Term& t) {
    if (t.isVar) {
        auto it = binding.find(t.name);
        return it == binding.end() ? -1 : it->second;
    }
    return g.findId(t.name);
}

// Attempt to unify a term with a concrete node id under the current binding.
// Returns false if the term is a constant naming a different node, or a
// variable already bound to a different node. On success, may extend the
// binding (caller is responsible for snapshot/restore on backtrack).
static bool unify(const Graph& g, const Term& t, int val,
                  std::unordered_map<std::string, int>& binding) {
    if (t.isVar) {
        auto it = binding.find(t.name);
        if (it == binding.end()) { binding[t.name] = val; return true; }
        return it->second == val;
    }
    int cId = g.findId(t.name);
    return cId == val;
}

// Find every assignment of the rule's variables that satisfies all antecedent
// triples. A triple `s->p->o` only matches if (s, p, o) appears as a
// consecutive 3-window inside some chain (source line OR a chain previously
// derived by a rule). This is stricter than walking the directed graph
// blindly: a 2-hop path that joins two source chains at a shared middle node
// is NOT a valid match. So `bird->will->talk` is not "a sentence" just
// because `bird->will` (from `bird->will->fly`) and `will->talk` (from
// `man->will->talk`) both happen to exist — there has to be a chain
// containing all three tokens in order.
static std::vector<std::unordered_map<std::string, int>>
matchAntecedent(const Graph& g, const std::vector<TriplePattern>& pats) {
    std::vector<std::unordered_map<std::string, int>> results;
    std::unordered_map<std::string, int> binding;

    std::function<void(size_t)> rec = [&](size_t idx) {
        if (idx == pats.size()) { results.push_back(binding); return; }
        const auto& pat = pats[idx];

        // Pre-pin from current binding so we can early-reject windows.
        int sFix = resolveTerm(g, binding, pat.s);
        int pFix = resolveTerm(g, binding, pat.p);
        int oFix = resolveTerm(g, binding, pat.o);

        for (const auto& c : g.chains) {
            int len = (int)c.nodeIds.size();
            if (len < 3) continue;
            for (int j = 0; j + 2 < len; ++j) {
                int sv = c.nodeIds[j];
                int pv = c.nodeIds[j + 1];
                int ov = c.nodeIds[j + 2];
                if (sFix >= 0 && sFix != sv) continue;
                if (pFix >= 0 && pFix != pv) continue;
                if (oFix >= 0 && oFix != ov) continue;
                auto saved = binding;
                if (unify(g, pat.s, sv, binding) &&
                    unify(g, pat.p, pv, binding) &&
                    unify(g, pat.o, ov, binding)) {
                    rec(idx + 1);
                }
                binding = saved;
            }
        }
    };
    rec(0);
    return results;
}

// Apply rules until no new edges are derived. Each derivation appends a
// chain to g.chains (so hover BFS sees it) and tags both edges of the
// chain as derived so the entire consequent stands out visually.
static void evaluateRules(Graph& g) {
    if (g.rules.empty()) return;
    g.firstDerivedChain = (int)g.chains.size();
    // Track derived chains already produced (canonicalized as s|p|o node ids)
    // so we don't append duplicates across fixpoint iterations.
    std::unordered_set<uint64_t> seenChains;
    auto chainKey = [](int s, int p, int o) {
        return (uint64_t)(uint32_t)s * 0x9E3779B97F4A7C15ull
             ^ ((uint64_t)(uint32_t)p << 1)
             ^ ((uint64_t)(uint32_t)o * 0xBF58476D1CE4E5B9ull);
    };
    const int MAX_ITERS = 16;
    for (int iter = 0; iter < MAX_ITERS; ++iter) {
        bool changed = false;
        for (auto& rule : g.rules) {
            auto bindings = matchAntecedent(g, rule.antecedent);
            for (auto& b : bindings) {
                for (auto& tp : rule.consequent) {
                    // Resolve / create each term to a concrete node id.
                    // Unbound variables in the consequent leave the
                    // derivation incomplete and are silently skipped.
                    auto resolveOrCreate = [&](const Term& t) -> int {
                        if (t.isVar) {
                            auto it = b.find(t.name);
                            if (it == b.end()) return -1;
                            return it->second;
                        }
                        return g.getOrCreate(t.name);
                    };
                    int s = resolveOrCreate(tp.s);
                    int p = resolveOrCreate(tp.p);
                    int o = resolveOrCreate(tp.o);
                    if (s < 0 || p < 0 || o < 0) continue;

                    uint64_t key = chainKey(s, p, o);
                    if (!seenChains.insert(key).second) continue; // already derived

                    int eSizeBefore = (int)g.edges.size();
                    int e1 = g.ensureEdge(s, p);
                    int e2 = g.ensureEdge(p, o);
                    bool addedNew = ((int)g.edges.size() > eSizeBefore);
                    // Mark both edges of the derived chain as derived so the
                    // user sees the entire consequent in the derived color.
                    g.edges[e1].derived = true;
                    g.edges[e2].derived = true;
                    Chain c;
                    c.nodeIds = {s, p, o};
                    c.edgeIndices = {e1, e2};
                    g.chains.push_back(std::move(c));
                    if (addedNew) changed = true;
                }
            }
        }
        if (!changed) break;
    }
}

static const char* kDefaultGraph =
    "# Edit assets/graph.txt to change this graph, or pass a path on the CLI\n"
    "hover:john\n"
    "man->will->talk\n"
    "bird->will->fly\n"
    "john->isa->man\n"
    "man->isa->animal\n"
    "bird->isa->animal\n"
    "animal->will->die\n";

// ---------- indices over the parsed graph ----------

struct GraphIndex {
    std::unordered_map<int, std::vector<int>> chainsByNode; // node -> chain indices
    std::unordered_map<int, std::vector<int>> chainsByHead; // node -> chains where it is the head
    std::vector<std::vector<int>> adjUndirected;            // node -> neighbor node indices

    void build(const Graph& g) {
        chainsByNode.clear();
        chainsByHead.clear();
        for (size_t ci = 0; ci < g.chains.size(); ++ci) {
            const auto& c = g.chains[ci];
            for (int nid : c.nodeIds) chainsByNode[nid].push_back((int)ci);
            if (!c.nodeIds.empty()) chainsByHead[c.nodeIds.front()].push_back((int)ci);
        }
        adjUndirected.assign(g.nodes.size(), {});
        for (auto& e : g.edges) {
            adjUndirected[e.a].push_back(e.b);
            adjUndirected[e.b].push_back(e.a);
        }
    }
};

// ---------- path algorithms (operate on a generic int-id adjacency list) ----------

static constexpr int kMaxPaths = 500;

static std::vector<int> bfsShortestPath(int from, int to,
                                        const std::vector<std::vector<int>>& adj) {
    if (from == to) return {from};
    int n = (int)adj.size();
    if (from < 0 || to < 0 || from >= n || to >= n) return {};
    std::vector<int> prev(n, -2);
    prev[from] = -1;
    std::queue<int> q;
    q.push(from);
    while (!q.empty()) {
        int cur = q.front(); q.pop();
        if (cur == to) break;
        for (int nxt : adj[cur]) {
            if (prev[nxt] != -2) continue;
            prev[nxt] = cur;
            q.push(nxt);
        }
    }
    if (prev[to] == -2) return {};
    std::vector<int> out;
    for (int c = to; c != -1; c = prev[c]) out.push_back(c);
    std::reverse(out.begin(), out.end());
    return out;
}

static std::vector<std::vector<int>> bfsAllShortestPaths(int from, int to,
                                                        const std::vector<std::vector<int>>& adj) {
    if (from == to) return {{from}};
    int n = (int)adj.size();
    if (from < 0 || to < 0 || from >= n || to >= n) return {};
    std::vector<int> dist(n, -1);
    std::vector<std::vector<int>> preds(n);
    dist[from] = 0;
    std::vector<int> frontier{from};
    while (!frontier.empty() && dist[to] == -1) {
        std::vector<int> next;
        std::vector<char> inNext(n, 0);
        for (int cur : frontier) {
            int d = dist[cur];
            for (int nxt : adj[cur]) {
                if (dist[nxt] == -1) {
                    dist[nxt] = d + 1;
                    preds[nxt].push_back(cur);
                    if (!inNext[nxt]) { inNext[nxt] = 1; next.push_back(nxt); }
                } else if (dist[nxt] == d + 1) {
                    preds[nxt].push_back(cur);
                }
            }
        }
        frontier = std::move(next);
    }
    if (dist[to] == -1) return {};
    std::vector<std::vector<int>> results;
    std::function<void(int, std::vector<int>&)> build = [&](int node, std::vector<int>& tail) {
        if ((int)results.size() >= kMaxPaths) return;
        std::vector<int> path; path.reserve(tail.size() + 1);
        path.push_back(node);
        for (int x : tail) path.push_back(x);
        if (node == from) { results.push_back(std::move(path)); return; }
        for (int p : preds[node]) {
            build(p, path);
            if ((int)results.size() >= kMaxPaths) break;
        }
    };
    std::vector<int> empty;
    build(to, empty);
    return results;
}

static std::vector<std::vector<int>> dfsAllSimplePaths(int from, int to,
                                                      const std::vector<std::vector<int>>& adj) {
    int n = (int)adj.size();
    std::vector<std::vector<int>> results;
    if (from < 0 || to < 0 || from >= n || to >= n) return results;
    std::vector<int> stack;
    std::vector<char> visited(n, 0);
    std::function<void(int)> dfs = [&](int cur) {
        if ((int)results.size() >= kMaxPaths) return;
        visited[cur] = 1; stack.push_back(cur);
        if (cur == to) {
            results.push_back(stack);
        } else {
            for (int nxt : adj[cur]) {
                if (visited[nxt]) continue;
                dfs(nxt);
                if ((int)results.size() >= kMaxPaths) break;
            }
        }
        stack.pop_back();
        visited[cur] = 0;
    };
    dfs(from);
    return results;
}

// Virtual graph for `search:edge`: each chain's middle nodes get their own
// per-chain virtual vertex, so two chains can only meet at head/tail.
// Head/tail virtual vertices keep the original node index.
struct VirtualGraph {
    std::vector<std::vector<int>> adj;
    std::vector<int> origNodeOf;                // virt index -> original node index (-1 if synthetic)
    // (virtA, virtB) -> original edge index
    std::unordered_map<uint64_t, int> edgeOfPair;
    int virtN = 0;
};

static uint64_t packPair(int a, int b) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32)
         |  static_cast<uint64_t>(static_cast<uint32_t>(b));
}

static VirtualGraph buildVirtualGraph(const Graph& g) {
    VirtualGraph v;
    int n = (int)g.nodes.size();
    v.origNodeOf.assign(n, -1);
    for (int i = 0; i < n; ++i) v.origNodeOf[i] = i;
    v.virtN = n;
    auto addEdge = [&](int va, int vb, int origEdge) {
        if (va >= (int)v.adj.size()) v.adj.resize(va + 1);
        if (vb >= (int)v.adj.size()) v.adj.resize(vb + 1);
        v.adj[va].push_back(vb);
        v.adj[vb].push_back(va);
        v.edgeOfPair[packPair(va, vb)] = origEdge;
        v.edgeOfPair[packPair(vb, va)] = origEdge;
    };
    v.adj.resize(n);
    for (size_t ci = 0; ci < g.chains.size(); ++ci) {
        const auto& c = g.chains[ci];
        int len = (int)c.nodeIds.size();
        if (len < 2) continue;
        std::vector<int> vIds(len);
        for (int j = 0; j < len; ++j) {
            if (j == 0 || j == len - 1) {
                vIds[j] = c.nodeIds[j];
            } else {
                int newIdx = (int)v.origNodeOf.size();
                v.origNodeOf.push_back(c.nodeIds[j]);
                v.adj.emplace_back();
                vIds[j] = newIdx;
            }
        }
        for (int j = 0; j + 1 < len; ++j) addEdge(vIds[j], vIds[j + 1], c.edgeIndices[j]);
    }
    v.virtN = (int)v.origNodeOf.size();
    return v;
}

// ---------- lit set computation ----------

struct LitSet {
    std::unordered_set<int> nodes;
    std::unordered_set<int> edges;
    bool empty() const { return nodes.empty() && edges.empty(); }
};

static void addChainToLit(const Graph& g, int ci, LitSet& out) {
    const auto& c = g.chains[ci];
    for (int nid : c.nodeIds)    out.nodes.insert(nid);
    for (int eid : c.edgeIndices) out.edges.insert(eid);
}

static LitSet litFromHighlight(const Graph& g, const GraphIndex& gi,
                               const std::vector<std::string>& ids) {
    LitSet out;
    for (auto& id : ids) {
        int nid = g.findId(id);
        if (nid < 0) continue;
        out.nodes.insert(nid);
        auto it = gi.chainsByNode.find(nid);
        if (it != gi.chainsByNode.end())
            for (int ci : it->second) addChainToLit(g, ci, out);
    }
    return out;
}

static LitSet litFromHover(const Graph& g, const GraphIndex& gi,
                           const std::vector<std::string>& ids) {
    LitSet out;
    for (auto& id : ids) {
        int start = g.findId(id);
        if (start < 0) continue;
        out.nodes.insert(start);
        std::unordered_set<int> visitedChains;
        std::unordered_set<int> visitedNodes;
        std::queue<int> q;
        auto addNode = [&](int n) {
            if (visitedNodes.insert(n).second) q.push(n);
        };
        auto addChain = [&](int ci) {
            if (!visitedChains.insert(ci).second) return;
            for (int n : g.chains[ci].nodeIds) addNode(n);
        };
        addNode(start);
        auto seedIt = gi.chainsByNode.find(start);
        if (seedIt != gi.chainsByNode.end())
            for (int ci : seedIt->second) addChain(ci);
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            auto hit = gi.chainsByHead.find(cur);
            if (hit != gi.chainsByHead.end())
                for (int ci : hit->second) addChain(ci);
        }
        for (int ci : visitedChains) addChainToLit(g, ci, out);
    }
    return out;
}

// Light up a sequence in the original graph (used for search:node paths).
static void addSequenceToLit(const Graph& g, const std::vector<int>& seq, LitSet& out) {
    for (int n : seq) out.nodes.insert(n);
    for (size_t i = 0; i + 1 < seq.size(); ++i) {
        uint64_t key = packPair(seq[i], seq[i + 1]);
        auto it = g.edgeIndexByPair.find(key);
        if (it != g.edgeIndexByPair.end()) {
            out.edges.insert(it->second);
            continue;
        }
        // Reverse direction (paths are undirected).
        key = packPair(seq[i + 1], seq[i]);
        it = g.edgeIndexByPair.find(key);
        if (it != g.edgeIndexByPair.end()) out.edges.insert(it->second);
    }
}

// Light a virtual-graph sequence (search:edge mode). Each virt id maps to an
// original node; edges map via the virtual graph's edge-of-pair table.
static void addVirtualSequenceToLit(const VirtualGraph& v, const std::vector<int>& vSeq, LitSet& out) {
    for (int vid : vSeq) {
        if (vid >= 0 && vid < (int)v.origNodeOf.size())
            out.nodes.insert(v.origNodeOf[vid]);
    }
    for (size_t i = 0; i + 1 < vSeq.size(); ++i) {
        auto it = v.edgeOfPair.find(packPair(vSeq[i], vSeq[i + 1]));
        if (it != v.edgeOfPair.end()) out.edges.insert(it->second);
    }
}

static bool litFromPath(const Graph& g, const GraphIndex& gi, const VirtualGraph* virtMaybe,
                       const PathDir& p, LitSet& out) {
    int from = g.findId(p.from);
    int to   = g.findId(p.to);
    if (from < 0 || to < 0) return false;
    for (auto& w : p.includes) if (g.findId(w) < 0) return false;

    if (p.search == Search::Edge) {
        // Need a virtual graph; from/to must be head/tail of some chain.
        if (!virtMaybe) return false;
        const VirtualGraph& v = *virtMaybe;
        // In the virtual graph, head/tail vertices have origNodeOf == themselves.
        // Validate from/to are original head/tail by checking that vertex `from`
        // has origNodeOf[from] == from and that it actually has neighbors.
        if (from >= v.virtN || to >= v.virtN) return false;

        if (p.show == Show::AllSimple) {
            auto all = dfsAllSimplePaths(from, to, v.adj);
            if (all.empty()) return false;
            std::unordered_set<int> required;
            for (auto& w : p.includes) required.insert(g.findId(w));
            bool any = false;
            for (auto& seq : all) {
                if (!required.empty()) {
                    std::unordered_set<int> origs;
                    for (int vid : seq) if (vid < (int)v.origNodeOf.size()) origs.insert(v.origNodeOf[vid]);
                    bool ok = true;
                    for (int r : required) if (!origs.count(r)) { ok = false; break; }
                    if (!ok) continue;
                }
                addVirtualSequenceToLit(v, seq, out);
                any = true;
            }
            return any;
        }

        std::vector<int> waypoints;
        waypoints.push_back(from);
        for (auto& w : p.includes) waypoints.push_back(g.findId(w));
        waypoints.push_back(to);
        for (size_t i = 0; i + 1 < waypoints.size(); ++i) {
            if (p.show == Show::ShortestOne) {
                auto seg = bfsShortestPath(waypoints[i], waypoints[i + 1], v.adj);
                if (seg.empty()) return false;
                addVirtualSequenceToLit(v, seg, out);
            } else {
                auto segs = bfsAllShortestPaths(waypoints[i], waypoints[i + 1], v.adj);
                if (segs.empty()) return false;
                for (auto& s : segs) addVirtualSequenceToLit(v, s, out);
            }
        }
        return true;
    }

    // search:node — operate directly on the original undirected adjacency.
    if (p.show == Show::AllSimple) {
        auto all = dfsAllSimplePaths(from, to, gi.adjUndirected);
        if (all.empty()) return false;
        std::unordered_set<int> required;
        for (auto& w : p.includes) required.insert(g.findId(w));
        bool any = false;
        for (auto& seq : all) {
            if (!required.empty()) {
                std::unordered_set<int> in(seq.begin(), seq.end());
                bool ok = true;
                for (int r : required) if (!in.count(r)) { ok = false; break; }
                if (!ok) continue;
            }
            addSequenceToLit(g, seq, out);
            any = true;
        }
        return any;
    }
    std::vector<int> waypoints;
    waypoints.push_back(from);
    for (auto& w : p.includes) waypoints.push_back(g.findId(w));
    waypoints.push_back(to);
    for (size_t i = 0; i + 1 < waypoints.size(); ++i) {
        if (p.show == Show::ShortestOne) {
            auto seg = bfsShortestPath(waypoints[i], waypoints[i + 1], gi.adjUndirected);
            if (seg.empty()) return false;
            addSequenceToLit(g, seg, out);
        } else {
            auto segs = bfsAllShortestPaths(waypoints[i], waypoints[i + 1], gi.adjUndirected);
            if (segs.empty()) return false;
            for (auto& s : segs) addSequenceToLit(g, s, out);
        }
    }
    return true;
}

// Combine directive lit-sets via the join mode, returning the final default
// lit-set or std::nullopt if no directives produced anything.
struct DefaultLit {
    LitSet set;
    bool active = false;
};

static DefaultLit computeDefaultLit(const Graph& g, const GraphIndex& gi) {
    std::vector<LitSet> sets;
    for (auto& h : g.highlights) {
        auto s = litFromHighlight(g, gi, h.ids);
        if (!s.empty()) sets.push_back(std::move(s));
    }
    for (auto& h : g.hovers) {
        auto s = litFromHover(g, gi, h.ids);
        if (!s.empty()) sets.push_back(std::move(s));
    }
    if (!g.paths.empty()) {
        // Build the virtual graph lazily; cheap enough to always build when paths exist.
        VirtualGraph v = buildVirtualGraph(g);
        for (auto& p : g.paths) {
            LitSet s;
            if (litFromPath(g, gi, &v, p, s) && !s.empty()) sets.push_back(std::move(s));
        }
    }
    DefaultLit dl;
    if (sets.empty()) return dl;
    if (sets.size() == 1) { dl.set = std::move(sets[0]); dl.active = true; return dl; }
    if (g.joinMode == JoinMode::Intersect) {
        LitSet acc = sets[0];
        for (size_t i = 1; i < sets.size(); ++i) {
            LitSet nx;
            for (int n : acc.nodes) if (sets[i].nodes.count(n)) nx.nodes.insert(n);
            for (int e : acc.edges) if (sets[i].edges.count(e)) nx.edges.insert(e);
            acc = std::move(nx);
        }
        dl.set = std::move(acc);
    } else {
        LitSet acc;
        for (auto& s : sets) {
            for (int n : s.nodes) acc.nodes.insert(n);
            for (int e : s.edges) acc.edges.insert(e);
        }
        dl.set = std::move(acc);
    }
    dl.active = true;
    return dl;
}

// ---------- rendering primitives ----------

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

static SDL_Color hsv2rgb(float h, float s, float v, Uint8 alpha = 255) {
    h = std::fmod(h, 360.f);
    if (h < 0) h += 360.f;
    float c = v * s;
    float h60 = h / 60.f;
    float x = c * (1.f - std::fabs(std::fmod(h60, 2.f) - 1.f));
    float r1 = 0, g1 = 0, b1 = 0;
    if (h60 < 1)      { r1 = c; g1 = x; b1 = 0; }
    else if (h60 < 2) { r1 = x; g1 = c; b1 = 0; }
    else if (h60 < 3) { r1 = 0; g1 = c; b1 = x; }
    else if (h60 < 4) { r1 = 0; g1 = x; b1 = c; }
    else if (h60 < 5) { r1 = x; g1 = 0; b1 = c; }
    else              { r1 = c; g1 = 0; b1 = x; }
    float m = v - c;
    auto u8 = [](float f) { return (Uint8)std::clamp(int(std::round((f) * 255.f)), 0, 255); };
    return { u8(r1 + m), u8(g1 + m), u8(b1 + m), alpha };
}

// Distinct color per chain index. Uses the golden angle (~137.508°) for
// nicely spread hues; derived chains nudge into a slightly different
// saturation/value band so the eye can still pick them out at a glance.
static SDL_Color chainColor(int ci, bool derived) {
    float hue = std::fmod(ci * 137.508f, 360.f);
    return derived ? hsv2rgb(hue, 0.55f, 0.85f) : hsv2rgb(hue, 0.78f, 0.95f);
}

static void fillTriangle(SDL_Renderer* r, float ax, float ay, float bx, float by,
                         float cx, float cy, SDL_Color color) {
    SDL_Vertex v[3];
    v[0].position = {ax, ay}; v[0].color = color; v[0].tex_coord = {0, 0};
    v[1].position = {bx, by}; v[1].color = color; v[1].tex_coord = {0, 0};
    v[2].position = {cx, cy}; v[2].color = color; v[2].tex_coord = {0, 0};
    SDL_RenderGeometry(r, nullptr, v, 3, nullptr, 0);
}

static void drawArrow(SDL_Renderer* r, float x1, float y1, float x2, float y2,
                      float tipInset, int shaftThickness, float headLen,
                      float headHalfWidth, SDL_Color color) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < tipInset + 1.f) return;
    float ux = dx / len, uy = dy / len;
    float px = -uy,      py = ux;
    float tipX = x2 - ux * tipInset;
    float tipY = y2 - uy * tipInset;
    float baseX = tipX - ux * headLen;
    float baseY = tipY - uy * headLen;
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    drawThickLine(r, x1, y1, baseX, baseY, shaftThickness);
    fillTriangle(r,
        tipX, tipY,
        baseX + px * headHalfWidth, baseY + py * headHalfWidth,
        baseX - px * headHalfWidth, baseY - py * headHalfWidth,
        color);
}

struct TextCache {
    SDL_Renderer* renderer;
    TTF_Font* font;
    struct Entry { SDL_Texture* tex; int w, h; };
    std::unordered_map<std::string, Entry> map;

    ~TextCache() { for (auto& kv : map) SDL_DestroyTexture(kv.second.tex); }

    Entry get(const std::string& s, SDL_Color color) {
        char key[256];
        std::snprintf(key, sizeof(key), "%02x%02x%02x%02x|%s",
                      color.r, color.g, color.b, color.a, s.c_str());
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

static void drawText(SDL_Renderer* r, TextCache& cache, const std::string& s,
                     int x, int y, SDL_Color color, bool centerX = true, bool centerY = true) {
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

// CJK-capable fonts are listed first so Chinese / Japanese / Korean node
// labels render with real glyphs instead of blank boxes. Microsoft YaHei
// and friends also carry full Latin coverage, so the rest of the UI still
// looks fine. Drop your own font at assets/font.ttf to override everything.
static const char* candidateFonts[] = {
    "assets/font.ttf",
    // Windows CJK
    "C:/Windows/Fonts/msyh.ttc",     // Microsoft YaHei  (Simplified Chinese)
    "C:/Windows/Fonts/msjh.ttc",     // Microsoft JhengHei (Traditional Chinese)
    "C:/Windows/Fonts/simhei.ttf",   // SimHei
    "C:/Windows/Fonts/simsun.ttc",   // SimSun
    "C:/Windows/Fonts/malgun.ttf",   // Malgun Gothic (Korean, has CJK)
    "C:/Windows/Fonts/msgothic.ttc", // MS Gothic (Japanese)
    // Linux CJK
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    // macOS CJK
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    // Latin-only fallbacks (no CJK glyphs)
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

    std::string source;
    {
        std::string path = (argc >= 2) ? argv[1] : "assets/graph.txt";
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
    parseGraph(source, g);
    for (auto& e : g.parseErrors) std::fprintf(stderr, "parse: %s\n", e.c_str());
    int edgesBeforeRules  = (int)g.edges.size();
    int chainsBeforeRules = (int)g.chains.size();
    evaluateRules(g);
    int derivedEdges  = (int)g.edges.size()  - edgesBeforeRules;
    int derivedChains = (int)g.chains.size() - chainsBeforeRules;
    std::printf("graph: %d nodes, %d edges, %d chains (rules:%d, derived %d chains/%d new edges) "
                "directives: h=%d hv=%d p=%d\n",
        (int)g.nodes.size(), (int)g.edges.size(), (int)g.chains.size(),
        (int)g.rules.size(), derivedChains, derivedEdges,
        (int)g.highlights.size(), (int)g.hovers.size(), (int)g.paths.size());

    GraphIndex gi;
    gi.build(g);
    DefaultLit defaultLit = computeDefaultLit(g, gi);

    // Precompute one color per chain and, for each edge, how many chains
    // claim it. We render each chain's edges in its own color, offsetting
    // shared edges perpendicularly so all participating chains stay visible.
    std::vector<SDL_Color> chainColors(g.chains.size());
    for (size_t ci = 0; ci < g.chains.size(); ++ci) {
        bool derived = (g.firstDerivedChain >= 0 && (int)ci >= g.firstDerivedChain);
        chainColors[ci] = chainColor((int)ci, derived);
    }
    std::vector<int> chainsPerEdge(g.edges.size(), 0);
    for (auto& c : g.chains)
        for (int ei : c.edgeIndices) chainsPerEdge[ei]++;

    // Sidebar lives on the right; graph area is everything to its left.
    const int sidebarW = 320;
    auto graphAreaW = [&]() { return std::max(200, winW - sidebarW); };

    // initial layout: spread nodes on a circle inside the graph area
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> jitter(-30.f, 30.f);
    auto seedLayout = [&]() {
        int n = (int)g.nodes.size();
        if (n == 0) return;
        float cx = graphAreaW() * 0.5f, cy = winH * 0.5f;
        float r = std::min(graphAreaW(), winH) * 0.3f;
        for (int i = 0; i < n; ++i) {
            float a = (float)i / std::max(1, n) * 6.2831853f;
            g.nodes[i].x = cx + r * std::cos(a) + jitter(rng);
            g.nodes[i].y = cy + r * std::sin(a) + jitter(rng);
            g.nodes[i].vx = g.nodes[i].vy = 0;
            g.nodes[i].pinned = false;
        }
    };
    seedLayout();

    struct {
        float repel = 6500.f;
        float springK = 0.04f;
        float springLen = 110.f;
        float centerK = 0.005f;
        float damping = 0.85f;
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

    SDL_Color colBg      = {18, 20, 24, 255};
    SDL_Color colEdgeLbl = {170, 180, 200, 220};
    SDL_Color colNode   = {130, 170, 240, 255};
    SDL_Color colNodeBd = {220, 230, 250, 255};
    SDL_Color colNodeDim= {70, 80, 100, 120};
    SDL_Color colNodeHi = {255, 180, 90, 255};
    SDL_Color colNodeBdHi = {255, 235, 170, 255};
    SDL_Color colNodeLbl= {245, 248, 255, 255};
    SDL_Color colNodeLblDim = {150, 158, 175, 200};

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

        // physics step
        if (!paused && !g.nodes.empty()) {
            int n = (int)g.nodes.size();
            std::vector<float> fx(n, 0.f), fy(n, 0.f);
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
            for (auto& e : g.edges) {
                float dx = g.nodes[e.b].x - g.nodes[e.a].x;
                float dy = g.nodes[e.b].y - g.nodes[e.a].y;
                float d = std::sqrt(dx * dx + dy * dy) + 1e-3f;
                float f = sim.springK * (d - sim.springLen);
                fx[e.a] += f * dx / d; fy[e.a] += f * dy / d;
                fx[e.b] -= f * dx / d; fy[e.b] -= f * dy / d;
            }
            float cx = graphAreaW() * 0.5f, cy = winH * 0.5f;
            for (int i = 0; i < n; ++i) {
                fx[i] += (cx - g.nodes[i].x) * sim.centerK;
                fy[i] += (cy - g.nodes[i].y) * sim.centerK;
            }
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

        // Compute sidebar layout up front so we can both pick a hovered row
        // and render it consistently below. Keep this in sync with the
        // sidebar render block.
        int sbX = winW - sidebarW;
        int sbPadX = sbX + 14;
        int sbHeaderH = (fontNode ? 26 : 0) + (fontEdge ? 22 : 0) + 14;
        int sbRowH    = fontEdge ? TTF_FontLineSkip(fontEdge) + 2 : 18;
        int sbRowsTop = sbHeaderH;
        int sbRowsBot = winH - 24;
        int sbVisibleRows = std::max(0, (sbRowsBot - sbRowsTop) / sbRowH);

        int mouseX = 0, mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);

        // Pick which sentence row, if any, the mouse is over. Returns -1 if
        // not over a row or if the mouse is outside the sidebar.
        int hoveredSidebarChain = -1;
        if (mouseX >= sbX && mouseY >= sbRowsTop && mouseY < sbRowsBot) {
            int idx = (mouseY - sbRowsTop) / sbRowH;
            if (idx >= 0 && idx < (int)g.chains.size() && idx < sbVisibleRows)
                hoveredSidebarChain = idx;
        }

        // Pick a graph node only when the mouse is in the graph area.
        int hovered = -1;
        if (mouseX < sbX) hovered = pickNode(mouseX, mouseY);

        // Effective lit set. Priority:
        //   1. mouse over a sidebar row  → only that chain
        //   2. mouse over a graph node   → chain-head BFS from that node
        //   3. directive default         → join of highlight/hover/path
        //   4. nothing                   → everything lit
        const LitSet* activeLit = nullptr;
        LitSet sidebarLit, hoverLit;
        if (hoveredSidebarChain >= 0) {
            const auto& c = g.chains[hoveredSidebarChain];
            for (int n : c.nodeIds)    sidebarLit.nodes.insert(n);
            for (int e : c.edgeIndices) sidebarLit.edges.insert(e);
            activeLit = &sidebarLit;
        } else if (hovered >= 0) {
            hoverLit = litFromHover(g, gi, {g.nodes[hovered].id});
            activeLit = &hoverLit;
        } else if (defaultLit.active) {
            activeLit = &defaultLit.set;
        }
        auto nodeLit = [&](int i) { return !activeLit || activeLit->nodes.count(i); };
        // A chain is "lit" when every one of its edges is in the active lit
        // set. This is the unit of highlighting for edges: a shared edge
        // that appears in chain A and chain B will only light up via the
        // chain that's actually selected — its parallel copy from the
        // unselected chain stays dim.
        auto chainIsLit = [&](int ci) {
            if (!activeLit) return true;
            const auto& c = g.chains[ci];
            if (c.edgeIndices.empty()) return false;
            for (int e : c.edgeIndices)
                if (!activeLit->edges.count(e)) return false;
            return true;
        };

        // render
        SDL_SetRenderDrawColor(ren, colBg.r, colBg.g, colBg.b, colBg.a);
        SDL_RenderClear(ren);

        // edges: draw each chain's edges in its own color. Shared edges
        // (the same edge index appearing in multiple chains) are offset
        // perpendicularly so every participating chain stays visible.
        {
            float inset      = sim.nodeRadius * zoom + 1.f;
            float headLen    = std::max(8.f, 9.f * zoom);
            float headHalfW  = std::max(4.f, 5.f * zoom);
            int   shaftThick = std::max(1, (int)std::round(2.f * std::min(zoom, 1.5f)));
            float perpStride = std::max(3.5f, 4.5f * std::min(zoom, 1.5f));

            std::vector<int> drawIdx(g.edges.size(), 0);
            for (int ci = 0; ci < (int)g.chains.size(); ++ci) {
                const auto& chain = g.chains[ci];
                SDL_Color base = chainColors[ci];
                bool lit = chainIsLit(ci);
                for (size_t j = 0; j + 1 < chain.nodeIds.size(); ++j) {
                    int ei = chain.edgeIndices[j];
                    int sNode = chain.nodeIds[j];
                    int oNode = chain.nodeIds[j + 1];
                    SDL_Color col = base;
                    col.a = lit ? 240 : 55;

                    int total = chainsPerEdge[ei];
                    int idx   = drawIdx[ei]++;
                    float off = (idx - (total - 1) * 0.5f) * perpStride;

                    float ax, ay, bx, by;
                    worldToScreen(g.nodes[sNode].x, g.nodes[sNode].y, ax, ay);
                    worldToScreen(g.nodes[oNode].x, g.nodes[oNode].y, bx, by);

                    float dx = bx - ax, dy = by - ay;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.1f && std::fabs(off) > 0.01f) {
                        float px = -dy / len, py = dx / len;
                        ax += px * off; ay += py * off;
                        bx += px * off; by += py * off;
                    }

                    drawArrow(ren, ax, ay, bx, by, inset, shaftThick,
                              headLen, headHalfW, col);
                }
            }
        }

        // nodes
        for (int i = 0; i < (int)g.nodes.size(); ++i) {
            float sx, sy;
            worldToScreen(g.nodes[i].x, g.nodes[i].y, sx, sy);
            int radius = (int)std::round(sim.nodeRadius * zoom);
            if (radius < 4) radius = 4;
            bool isDrag = (i == draggingNode);
            bool lit    = nodeLit(i);
            SDL_Color fill = lit ? (activeLit ? colNodeHi : colNode) : colNodeDim;
            SDL_Color bd   = lit ? (activeLit ? colNodeBdHi : colNodeBd) : colNodeDim;
            if (isDrag) fill = colNodeHi;
            SDL_SetRenderDrawColor(ren, fill.r, fill.g, fill.b, fill.a);
            fillCircle(ren, (int)sx, (int)sy, radius);
            SDL_SetRenderDrawColor(ren, bd.r, bd.g, bd.b, bd.a);
            strokeCircle(ren, (int)sx, (int)sy, radius);
            if (fontNode) {
                SDL_Color lblCol = lit ? colNodeLbl : colNodeLblDim;
                drawText(ren, nodeText, g.nodes[i].id, (int)sx, (int)sy - radius - 12, lblCol);
            }
        }

        // ---------- sidebar: chains rendered as sentences ----------
        {
            // Background panel + left divider line.
            SDL_SetRenderDrawColor(ren, 24, 26, 32, 255);
            SDL_Rect sbRect{sbX, 0, sidebarW, winH};
            SDL_RenderFillRect(ren, &sbRect);
            SDL_SetRenderDrawColor(ren, 60, 66, 78, 255);
            SDL_RenderDrawLine(ren, sbX, 0, sbX, winH);

            int y = 14;
            if (fontNode) {
                drawText(ren, nodeText, "Sentences",
                         sbPadX, y, SDL_Color{230, 235, 245, 255}, false, false);
                y += 26;
            }
            if (fontEdge) {
                char sub[160];
                std::snprintf(sub, sizeof(sub),
                    "%d total  (%d original, %d derived)",
                    (int)g.chains.size(),
                    g.firstDerivedChain < 0 ? (int)g.chains.size() : g.firstDerivedChain,
                    g.firstDerivedChain < 0 ? 0 : (int)g.chains.size() - g.firstDerivedChain);
                drawText(ren, edgeText, sub, sbPadX, y, colEdgeLbl, false, false);
                y += 22;
            }
            // y now equals sbRowsTop; double-check so the picker stays consistent.

            SDL_Color colSentence    = {230, 235, 245, 255};
            SDL_Color colSentenceDim = {115, 122, 138, 230};

            for (int ci = 0; ci < (int)g.chains.size(); ++ci) {
                if (y + sbRowH > sbRowsBot) break; // leave room for status line
                const auto& c = g.chains[ci];
                bool derived = (g.firstDerivedChain >= 0 && ci >= g.firstDerivedChain);
                bool lit = chainIsLit(ci);
                bool rowHovered = (ci == hoveredSidebarChain);

                // Row hover background — strip the row in the chain's color
                // at low alpha so you can see exactly which sentence is
                // currently driving the graph highlight.
                if (rowHovered) {
                    SDL_Color hi = chainColors[ci];
                    hi.a = 70;
                    SDL_SetRenderDrawColor(ren, hi.r, hi.g, hi.b, hi.a);
                    SDL_Rect rowRect{sbX + 4, y - 1, sidebarW - 8, sbRowH};
                    SDL_RenderFillRect(ren, &rowRect);
                }

                // Build the sentence: join the chain's node ids with spaces.
                std::string sentence;
                for (size_t j = 0; j < c.nodeIds.size(); ++j) {
                    if (j) sentence += " ";
                    sentence += g.nodes[c.nodeIds[j]].id;
                }
                if (derived) sentence += "   (derived)";

                // Bullet in this chain's color so the user can match a row
                // in the sidebar against the matching trace in the graph.
                SDL_Color bullet = chainColors[ci];
                if (!lit) { bullet.a = 110; }
                SDL_SetRenderDrawColor(ren, bullet.r, bullet.g, bullet.b, bullet.a);
                fillCircle(ren, sbPadX + 2, y + sbRowH / 2 - 2, 5);
                if (derived) {
                    // Hollow ring overlaid on the bullet marks derived chains.
                    SDL_SetRenderDrawColor(ren, 30, 32, 38, lit ? 255 : 180);
                    fillCircle(ren, sbPadX + 2, y + sbRowH / 2 - 2, 2);
                }

                SDL_Color col = lit ? colSentence : colSentenceDim;
                if (fontEdge) drawText(ren, edgeText, sentence,
                                       sbPadX + 16, y, col, false, false);
                y += sbRowH;
            }

            // Footer legend
            if (fontEdge) {
                int legendY = winH - 56;
                drawText(ren, edgeText, "○ derived chain",
                         sbPadX, legendY, colEdgeLbl, false, false);
                drawText(ren, edgeText, "hover a row to highlight its chain",
                         sbPadX, legendY + 18, colEdgeLbl, false, false);
            }
        }

        // status line (along the bottom of the graph area, NOT the sidebar)
        if (fontEdge) {
            const char* mode = (hoveredSidebarChain >= 0) ? "sentence"
                : (hovered >= 0) ? "hover"
                : (defaultLit.active ? "directive" : "all");
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "nodes:%d  edges:%d  chains:%d  zoom:%.2f  lit:%s  %s  "
                "[R reheat] [Space pause] [scroll zoom] [middle-drag pan]",
                (int)g.nodes.size(), (int)g.edges.size(), (int)g.chains.size(),
                zoom, mode, paused ? "PAUSED" : "running");
            drawText(ren, edgeText, buf, 10, winH - 18, colEdgeLbl, false, false);

            // Directive summary in the top-left of the graph area.
            if (defaultLit.active || !g.parseErrors.empty() || !g.rules.empty()) {
                char top[320];
                std::snprintf(top, sizeof(top),
                    "directives: %d highlight / %d hover / %d path  rules: %d  (join:%s)",
                    (int)g.highlights.size(), (int)g.hovers.size(), (int)g.paths.size(),
                    (int)g.rules.size(),
                    g.joinMode == JoinMode::Intersect ? "intersect" : "union");
                drawText(ren, edgeText, top, 10, 8, colEdgeLbl, false, false);
                int yErr = 26;
                for (auto& er : g.parseErrors) {
                    SDL_Color errC{255, 120, 120, 240};
                    drawText(ren, edgeText, er, 10, yErr, errC, false, false);
                    yErr += 16;
                    if (yErr > 100) break;
                }
            }
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
