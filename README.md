# NodeNetwork

A small interactive **force-directed graph viewer** written in C++ with SDL2.
Inspired by [Obsidian-Node-Network](https://github.com/ngchenghow/Obsidian-Node-Network) — same `subject->predicate->object` text format, rendered as a native desktop window instead of inside Obsidian. Edges are directed and rendered with arrowheads that land on the target node's border.

![screenshot placeholder](docs/screenshot.png)

## Graph syntax

Every token on a line is a node. Consecutive tokens become an edge, so
**relationships are first-class nodes** — `bird->color->red` produces
three nodes (`bird`, `color`, `red`) connected as `bird → color → red`.
Repeating the same token (e.g. `color` in multiple lines) merges into a
single hub node.

| form | meaning |
| --- | --- |
| `a->b` | edge from `a` to `b` |
| `a->p->b` | three nodes connected as `a → p → b` |
| `a->b->c->d` | chain of edges `a → b → c → d` |
| `# ...` or `// ...` | comment |

Example:

```
bird->color->red
bird->color->black
bird->name->crow
crow->eats->seeds
```

Here `color`, `name`, and `eats` each become their own node, which lets
relationship nodes attract their participants and act as visible hubs
in the layout.

## Rules (`IF ... THEN ...`)

A graph file may also contain **inference rules**. The viewer evaluates them
to a fixpoint after parsing and renders the **derived edges in green** so
new facts are visually distinct.

```
IF
A->will->B AND C->isa->A
THEN
C->will->B
```

Syntax:

- A block starts on a line containing only `IF` (case-insensitive).
- Body lines list triple patterns of the form `s->p->o`, joined with
  `AND` (whole-word, case-insensitive). You can write all patterns on one
  line, or split them across multiple lines.
- A line containing only `THEN` switches to the consequent.
- The consequent is one or more triple patterns. A **blank line ends the
  rule**.
- In a pattern, a token whose first character is an ASCII uppercase letter
  (`A`, `Bird`, `X1`) is a **variable**. Everything else is a **constant**
  (matches a literal node id, creating it for the consequent if needed).
- Variables are shared across the antecedent patterns: the rule fires for
  every assignment that satisfies all of them.

Each successful firing produces a chain `s -> p -> o` (so hover BFS sees
it like any other chain) and marks both edges as derived. Both edges of a
derived chain render green — even if one of them already existed in the
graph — so the whole rule output stands out.

**Matching is chain-bounded.** A pattern triple matches only when
`(s, p, o)` appears as a consecutive 3-window inside some chain — either
an original source line or a chain produced by a previous rule firing.
A 2-hop path that happens to exist in the directed graph but spans two
unrelated chains at a shared hub node does *not* count, so the rule
won't invent sentences like `bird will talk` from `bird->will->fly`
plus `man->will->talk` just because both touch `will`.

Evaluation iterates until no new edge is added, capped at 16 rounds.

## Highlight directives

A graph file may contain directives that **light up part of the graph
by default**. Anything not lit is dimmed. Each directive is one line,
mixed in with the chain lines. Mouse hover always wins over the
directive-driven default.

| directive | effect |
| --- | --- |
| `highlight:a[,b,...]` | light each listed node + every chain (source line) it appears in |
| `hover:a[,b,...]` | chain-head BFS from each listed node, lighting every chain reachable by following arrows out of head nodes |
| `path:from->to` | light the shortest path between two nodes |
| `path:from->to,show:shortest` | same — union every tied-for-shortest path (default) |
| `path:from->to,shortest:one` | pick a single shortest path |
| `path:from->to,show:all` | union every simple (no repeated node) path, capped at 500 |
| `path:from->to,search:node` | path search treats any shared node as a link (default) |
| `path:from->to,search:edge` | only link chains at their head/tail nodes; from/to must themselves be head/tail of some chain |
| `path:from->to,includes:x[,includes:y,...]` | constrain the path to pass through each listed intermediate, in order |
| `join:union` / `join:intersect` | combine multiple directives by union (default) or intersection |

Example ([assets/example_directives.txt](assets/example_directives.txt)):

```
path:fly->animal,show:shortest,search:edge
highlight:crow
hover:john

man->will->talk
bird->will->fly
john->isa->man
man->isa->animal
bird->isa->animal
animal->will->die
```

Run it with:

```
./build/NodeNetwork assets/example_directives.txt
```

`hover:john` lights `john`, then transitively `isa`, `man`, `animal`,
`will`, `talk`, `die`, plus the corresponding edges. `highlight:crow`
adds nothing here because `crow` isn't in the graph. `path:fly->animal`
in `search:edge` mode lights the shortest chain-link path from `fly`
back through `bird` and out to `animal`. All three directive sets are
unioned (default `join:union`).

## Controls

| input | action |
| --- | --- |
| left-drag a node | pin and move it (release to unpin) |
| mouse hover a node | transient highlight: chain-head BFS from that node (overrides directive default) |
| middle-drag / right-drag | pan |
| mouse wheel | zoom |
| `R` | reheat — randomize positions |
| `Space` | pause / resume simulation |
| `Esc` / close window | quit |

## Building

Requires **CMake ≥ 3.16**, a C++17 compiler, and **SDL2** + **SDL2_ttf** development packages.

### Windows (MSYS2 / MinGW)

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_ttf

cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
./build/NodeNetwork.exe
```

### Linux

```bash
sudo apt install build-essential cmake libsdl2-dev libsdl2-ttf-dev
cmake -S . -B build
cmake --build build -j
./build/NodeNetwork
```

### macOS (Homebrew)

```bash
brew install cmake sdl2 sdl2_ttf
cmake -S . -B build
cmake --build build -j
./build/NodeNetwork
```

## Running

```bash
./build/NodeNetwork                  # uses assets/graph.txt
./build/NodeNetwork path/to/my.txt   # load a custom graph
```

On Windows / MinGW the build automatically copies the SDL2 + GCC runtime
DLLs next to `NodeNetwork.exe`, so you can double-click it or move the
`build/` folder anywhere. To skip the gather step, pass
`-DNODENETWORK_GATHER_DLLS=OFF` to `cmake`. To run the gather manually:

```powershell
powershell -ExecutionPolicy Bypass `
    -File scripts\gather_dlls.ps1 `
    -Exe build\NodeNetwork.exe
```

The program searches for a TTF font in this order:

1. `assets/font.ttf` (drop one here to bundle)
2. Common Windows fonts (Segoe UI, Arial, ...)
3. DejaVu Sans on Linux, Helvetica on macOS

## How it works

The layout is a basic force simulation per frame:

- **Repulsion** between every node pair: `F = k / r²`
- **Spring** along each edge pulling toward a natural length
- **Centering** force toward the canvas middle
- **Damping** on velocity each step

Pinned (dragged) nodes skip the integration step, so you can interactively
constrain part of the graph and watch the rest relax around it.

## License

MIT
