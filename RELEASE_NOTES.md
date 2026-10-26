## OpenCaliper 0.2.0

### What's new

- **OpenCaliper is now part of Open Workshop Tools**, a family of free,
  open-source design tools: https://github.com/open-workshop-tools.
  The project's new home is https://github.com/open-workshop-tools/Open-Caliper
  (old links redirect automatically).
- **New application ID:** `io.github.open_workshop_tools.OpenCaliper`.
  If you installed an earlier Flatpak, remove the old one first:
  `flatpak uninstall io.github.engineer948.OpenCaliper`

### Also in this release (since 0.1.0)

- **Dimensions on the part.** Hole diameters, arc radii, the sizes you gave in
  sketches and extrude depths are shown on the model. Double-click one, type a
  new value and the part rebuilds. View > Show Dimensions turns them off.
- **Simpler Windows package.** The Microsoft C++ runtime ships next to
  `OpenCaliper.exe`, so there is no separate `vc_redist.x64.exe` to install, and
  the unused Qt network libraries are no longer included.
- Windows builds with Open CASCADE 8.

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
- No undo inside a sketch yet (undo works on whole steps).
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
