## OpenCaliper 0.4.2

### Fixed

**Windows**
- A few antivirus programs flagged the one-file download
  (`OpenCaliper-windows-x64.exe`) as suspicious. These were guesses by
  machine-learning scanners, not a real detection: the exe only unpacks
  OpenCaliper to `%LOCALAPPDATA%` and starts it, and its source is in
  `packaging/windows/`. It is now built the way installers usually are (the
  program stored as a Windows resource, with a proper application manifest),
  which such scanners are less likely to misread. The zip is unchanged.

## OpenCaliper 0.4.1

### What's new

**Windows**
- **One-file download**: `OpenCaliper-windows-x64.exe`. Double-click it and
  OpenCaliper starts, with nothing to unzip or install. The first start takes
  a few seconds while it unpacks itself to
  `%LOCALAPPDATA%\open-workshop-tools\OpenCaliper\app`; later starts are
  immediate, and a newer version cleans up the old copy. The zip is still
  there for anyone who prefers a plain folder.
- OpenCaliper now has its own icon in Explorer and the taskbar.

## OpenCaliper 0.4.0

**Parameters**
- **Parameters** (Ctrl+P): give sizes a name once, such as `width = 50` or
  `wall = 2`, and use them in any size field: `width / 2 + 5`. Change a
  parameter and every step that uses it rebuilds. Formulas also work in
  sketch dimensions and when you double-click a dimension on the part.
  `+ - * / ^`, parentheses, `pi`, `sqrt`, `min`, `max`, `round`, `sin`,
  `cos` and `tan` (degrees) are supported.

**Steadier history**
- Fillets, chamfers and shells now remember *which* edge or face you picked
  by its shape and position, not only by its number. Making the part bigger
  or adding a hole in an earlier step no longer moves a fillet to another
  edge or breaks it.

**Sketching**
- **Trim** (W): click the piece of a curve to cut away, up to where it
  crosses other curves.
- **Fillet** (N): click a corner where two lines meet to round it.
- **Offset** (O): parallel copy of curves; a closed shape grows or shrinks
  with sharp corners.
- **Mirror** (U): mirror curves across a line. The copy stays linked to the
  original through the new **Symmetric** constraint (Y).

**Modelling**
- **Hole** (Shift+D): draw circles on a face where the holes go, then pick
  a screw size from M2 to M12. Simple, counterbored (socket head) or
  countersunk (flat head), blind or through all. Holes can be patterned.
- **Mirror** (Shift+U): mirror the whole part across the XY, XZ or YZ plane
  or across a flat face, and join the copy.

**3D printing**
- **3MF export**, the format slicers prefer: millimetres, watertight mesh.
- **Mesh quality** (coarse, normal, fine) when exporting STL, OBJ or 3MF.

**Comfort**
- **Autosave**: unsaved work is backed up every minute. After a crash or
  power cut, OpenCaliper offers to recover it on the next start.
- **File > Open Recent**.

**Fixes**
- Measure showed broken numbers ("5,") on systems with a decimal comma.

Projects from 0.1 to 0.3 open unchanged. Projects saved by 0.4 need 0.4 or
newer for parameters, holes and mirrors.

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
  lines, rectangles, circles, arcs, polygons and slots, snapping to the grid
  and to existing points. Trim, offset, mirror and round corners.
- **Constrain** sketches: coincident, horizontal, vertical, parallel,
  perpendicular, tangent, equal, symmetric, fix, and dimensions (length,
  radius, distance). Drag points and the sketch follows its constraints.
- **Parameters**: named sizes and formulas that drive the whole model.
- **Build solids**: extrude and revolve, as a new body, joined, cut or
  intersected. Closed loops inside other loops become holes.
- **Finish** with fillets, chamfers, shells, screw holes, patterns and
  mirrors.
- **Measure** distances, angles and areas; volume and weight of the part.
- **Dimensions on the part**: hole diameters, arc radii, the sizes you gave in
  sketches and extrude depths are shown on the model. Double-click one, type a
  new value, and the part rebuilds. View > Show Dimensions turns them off.
- **Change your mind**: every step is in the timeline; double-click it to
  change a value or edit its sketch, and everything after it updates. Undo and
  redo with Ctrl+Z / Ctrl+Y.
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
  SmartScreen may ask for confirmation.

### Requirements

A graphics driver with OpenGL 3.3 or newer. On Ubuntu 24.04+ the AppImage
needs `sudo apt install libfuse2t64`.

### License

OpenCaliper is MIT licensed. It is built on Open CASCADE Technology (LGPL 2.1
with exception) and Qt (LGPL 3); see `THIRD-PARTY-NOTICES.md` or Help >
Licenses in the program.
