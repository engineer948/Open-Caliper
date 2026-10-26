## OpenCaliper 0.3.0

### What's new

**Sketching**
- **Rectangle**, **polygon** (any number of sides, always regular) and **slot**
  tools. Shortcuts: R, G, K.
- **Construction geometry** (X): turn curves into dashed helper lines and
  circles that guide the sketch but never become part of the solid.
- **Constraint labels**: H, V, //, 90°, =, T, the sizes you gave and F for
  fixed points are shown next to the geometry.
- **Undo and redo inside a sketch** with Ctrl+Z / Ctrl+Y.

**Modelling**
- **Shell** (Shift+H): hollow the part out to a wall thickness, leaving the
  faces you clicked open. Perfect for boxes and enclosures.
- **Pattern** (Shift+P): repeat an extrude or revolve, such as a hole, in a
  row along X, Y or Z, or around an axis.

**Measuring**
- **Measure** (M): click one or two points, edges or faces for lengths,
  areas, diameters, distances and angles.
- **Properties** (Shift+M): volume, surface area, size, centre of mass and
  weight in PLA, PETG, ABS, nylon, aluminium, steel, brass or wood.

**Fixes**
- Lines joined to arcs at their ends now really stay tangent when the sketch
  changes.

Projects from 0.1 and 0.2 open unchanged.

---

OpenCaliper is a small, open-source parametric 3D CAD program for designing
mechanical parts and objects for 3D printing.

### Downloads

| System | File |
|---|---|
| Linux, most distributions (Ubuntu 22.04+, Mint 21+, Debian 12+, Fedora 36+, Arch, openSUSE Tumbleweed) | `OpenCaliper-x86_64.AppImage` |
| Linux, any distribution with Flatpak (also RHEL 9, openSUSE Leap, older systems) | `OpenCaliper.flatpak` |
| Windows 10 / 11, 64-bit | `OpenCaliper-windows-x64.zip` |

Not sure which one? See [Which Linux file should I use?](https://github.com/open-workshop-tools/Open-Caliper#which-linux-file-should-i-use)
in the README.

### What you can do

- **Sketch** on the XY, XZ and YZ planes or on any flat face of a part:
  lines, circles and arcs, snapping to the grid and to existing points.
- **Constrain** sketches: coincident, horizontal, vertical, parallel,
  perpendicular, tangent, equal, fix, and dimensions (length, radius,
  distance). Drag points and the sketch follows its constraints.
- **Build solids**: extrude and revolve, as a new body, joined, cut or
  intersected. Closed loops inside other loops become holes.
- **Finish edges** with fillets (rounded) and chamfers (bevelled).
- **Dimensions on the part**: hole diameters, arc radii, the sizes you gave in
  sketches and extrude depths are shown on the model. Double-click one, type a
  new value, and the part rebuilds. View > Show Dimensions turns them off.
- **Change your mind**: every step is in the timeline; double-click it to
  change a value or edit its sketch, and everything after it updates. Undo and
  redo with Ctrl+Z / Ctrl+Y.
- **Files**: save projects as `.ocal`; export STEP, IGES, STL, OBJ and BREP;
  import STEP, IGES and BREP.
- Clickable view cube, standard views (0/1/2/3), Help > Licenses.

### Known limitations

- Fillets and chamfers remember edges by number. Changing an earlier step
  that adds or removes edges can make them fail (marked red in the timeline)
  or round a different edge.
- The 3D view runs through X11 on Linux (XWayland on Wayland desktops).
- x86-64 only; no macOS build yet. The Windows build is not code-signed, so
  SmartScreen may ask for confirmation.

### Requirements

A graphics driver with OpenGL 3.3 or newer. On Ubuntu 24.04+ the AppImage
needs `sudo apt install libfuse2t64`.

### License

OpenCaliper is MIT licensed. It is built on Open CASCADE Technology (LGPL 2.1
with exception) and Qt (LGPL 3); see `THIRD-PARTY-NOTICES.md` or Help >
Licenses in the program.
