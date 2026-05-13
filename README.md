# NodeNetwork

A small interactive **force-directed graph viewer** written in C++ with SDL2.
Inspired by [Obsidian-Node-Network](https://github.com/ngchenghow/Obsidian-Node-Network) — same `subject->predicate->object` text format, rendered as a native desktop window instead of inside Obsidian.

![screenshot placeholder](docs/screenshot.png)

## Graph syntax

Each line in `assets/graph.txt` is one of:

| form | meaning |
| --- | --- |
| `a->p->b` | edge from `a` to `b` labeled `p` |
| `a->b`   | unlabeled edge from `a` to `b` |
| `a->b->c->d` | chain — consecutive pairs become edges |
| `# ...` or `// ...` | comment |

Example:

```
bird->color->red
bird->name->crow
crow->eats->seeds
```

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
