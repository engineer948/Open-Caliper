## OpenCaliper 0.6.0

### What's new

**Free-form shapes**
- **Spline** (B) in sketches: click points and get a smooth curve through
  them. Right-click, Enter or Esc finishes it; click the first point again for
  a closed, smooth loop. Splines join lines and arcs at shared points, so a
  profile can mix them. Drag a point and the curve follows.
- **Loft** (Shift+L): a solid through the closed profiles of several sketches,
  for example a square at the bottom, a circle in the middle and an oval on
  top. Draw each cross-section on its own plane, click Loft and check the
  sketches to use. "Ruled" gives straight faces between the sections instead
  of a smooth skin. Lofts can join, cut, intersect and be patterned.
- **Symmetric extrude**: half of the distance to each side of the sketch, so
  a part centred on a plane no longer needs a shifted sketch.

**Fillets, chamfers and shells**
- A fillet or chamfer that cannot be made now says which edge failed (with
  its number), and suggests a smaller size.
- A fillet, chamfer or shell that "succeeded" but gave a broken solid (inside
  out or self-intersecting) is now refused, instead of being kept with a
  wrong volume.

**Dimensions on the part**
- Very gentle arcs (radius larger than the part) no longer get a radius label;
  it sat far away at the arc's centre and filled the view.

**Claude (MCP, Linux)**
- New sketch curves: `spline` (smooth, open or closed) and `arc3` (an arc
  through start, middle and end point). An `arc` whose end had to be moved
  onto its circle now says so.
- New steps: `loft`, and `symmetric` for extrudes.
- `screenshot` fits the part without the dimension labels and can take
  several views at once (`views: ["iso", "front", "top"]`).
- Claude Code started outside the Flatpak now also finds an OpenCaliper
  window running from the Flatpak.

Projects from earlier versions open unchanged. Projects with splines, lofts or
symmetric extrudes need 0.6 or newer.

---

OpenCaliper is a small, open-source parametric 3D CAD program for designing
mechanical parts and objects for 3D printing.

### Downloads

| System | File |
|---|---|
| Linux, most distributions (Ubuntu 22.04+, Mint 21+, Debian 12+, Fedora 36+, Arch, openSUSE Tumbleweed) | `OpenCaliper-x86_64.AppImage` |
| Linux, any distribution with Flatpak (also RHEL 9, openSUSE Leap, older systems) | `OpenCaliper.flatpak` |
| Windows 10 / 11, 64-bit, one file: double-click to start | `OpenCaliper-windows-x64.exe` |
| Windows 10 / 11, 64-bit, as a folder | `OpenCaliper-windows-x64.zip` |

Not sure which one? See [Which Linux file should I use?](https://github.com/open-workshop-tools/Open-Caliper#which-linux-file-should-i-use)
in the README.

### What you can do

- **Sketch** on the XY, XZ and YZ planes or on any flat face of a part:
  lines, rectangles, circles, arcs, splines, polygons and slots, snapping to the grid
  and to existing points. Trim, offset, mirror and round corners.
- **Constrain** sketches: coincident, horizontal, vertical, parallel,
  perpendicular, tangent, equal, symmetric, fix, and dimensions (length,
  radius, distance). Drag points and the sketch follows its constraints.
- **Parameters**: named sizes and formulas that drive the whole model.
- **Build solids**: extrude (one way or symmetric), revolve and loft through
  cross-sections, as a new body, joined, cut or intersected. Closed loops
  inside other loops become holes.
- **Finish** with fillets, chamfers, shells, screw holes, patterns and
  mirrors.
- **Measure** distances, angles and areas; volume and weight of the part.
- **Dimensions on the part**: hole diameters, arc radii, the sizes you gave in
  sketches and extrude depths are shown on the model. Double-click one, type a
  new value, and the part rebuilds. View > Show Dimensions turns them off.
- **Change your mind**: every step is in the timeline; double-click it to
  change a value or edit its sketch, and everything after it updates. Undo and
  redo with Ctrl+Z / Ctrl+Y.
- **Claude** (Linux): describe a part and Claude builds it step by step in the
  open window, with undo (Tools > Claude (MCP)).
- **Files**: save projects as `.ocal` (with autosave and crash recovery);
  export STEP, IGES, STL, OBJ, 3MF and BREP; import STEP, IGES and BREP.
- Clickable view cube, standard views (0/1/2/3), Help > Licenses.

### Known limitations

- Fillets, chamfers and shells find their edges and faces again by shape and
  position. Big changes to an earlier step (for example turning a box into a
  cylinder) can still make them fail; the step is then marked red in the
  timeline.
- The 3D view runs through X11 on Linux (XWayland on Wayland desktops).
- x86-64 only; no macOS build yet. The Windows build is not code-signed, so
  SmartScreen may ask for confirmation, and a few machine-learning antivirus
  scanners may flag it (false positives; see "Antivirus warnings" in the
  README).

### Requirements

A graphics driver with OpenGL 3.3 or newer. On Ubuntu 24.04+ the AppImage
needs `sudo apt install libfuse2t64`.

### License

OpenCaliper is MIT licensed. It is built on Open CASCADE Technology (LGPL 2.1
with exception) and Qt (LGPL 3); see `THIRD-PARTY-NOTICES.md` or Help >
Licenses in the program.
