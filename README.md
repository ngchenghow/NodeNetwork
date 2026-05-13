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

In the example above `color`, `name`, and `eats` each become their own
node, which lets relationship nodes attract their participants and act
as visible hubs in the layout.

## Controls

| input | action |
| --- | --- |
| left-drag a node | pin and move it (release to unpin) |
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
