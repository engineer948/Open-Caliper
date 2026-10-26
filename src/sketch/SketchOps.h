#pragma once

#include "sketch/Sketch.h"

#include <vector>

// Editing operations on a sketch: trim, corner fillet, mirror, offset of
// single curves. Pure 2D like Sketch itself. Each returns false (or nothing)
// when it does not apply; the sketch may then be partly changed, so callers
// keep a copy to restore (the editor's undo snapshot).
namespace sketch {

// Removes the piece of `entity` around (u, v) up to the nearest crossings
// with other curves, or the whole curve if nothing crosses it. New ends get a
// point-on-curve constraint to the curve that cut them.
bool trim(Sketch& s, int entity, double u, double v);

// Rounds the corner where exactly two lines meet at `point` with a tangent arc.
bool filletCorner(Sketch& s, int point, double radius);

// Mirror copies of `entities` across line `axis`, tied to the originals with
// Symmetric constraints (points on the axis are shared). Returns the copies.
std::vector<int> mirror(Sketch& s, const std::vector<int>& entities, int axis);

// Offset copy of one curve: lines move to their left (p0 -> p1), circles and
// arcs grow; a negative distance goes the other way. Circles and arcs share
// the centre. Returns the new entity, -1 if it would vanish.
int offsetCurve(Sketch& s, int entity, double distance);

// True when every line/arc end in `entities` meets exactly one other of them:
// the selection is made of closed loops.
bool formsClosedLoops(const Sketch& s, const std::vector<int>& entities);

} // namespace sketch
