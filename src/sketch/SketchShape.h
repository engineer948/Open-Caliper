#pragma once

#include "sketch/Sketch.h"

#include <Geom2d_BSplineCurve.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>

#include <utility>
#include <vector>

// Sketch (2D, plane-local u/v) -> OCCT edges on a 3D plane.
namespace sketch {

gp_Pnt toWorld(const gp_Ax3& plane, double u, double v);

// Null shape when degenerate (zero length / radius).
TopoDS_Shape makeSegment(const gp_Ax3& plane, double u0, double v0, double u1, double v1);
TopoDS_Shape makeCircle(const gp_Ax3& plane, double cu, double cv, double r);
TopoDS_Shape makeArc(const gp_Ax3& plane, double cu, double cv, double su, double sv, double eu, double ev);

// Smooth curve through (u, v) points in sketch coordinates; periodic when
// `closed` (then the first point is not repeated). Null if it cannot be made.
Handle(Geom2d_BSplineCurve) interpolate(const std::vector<std::pair<double, double>>& points, bool closed);
Handle(Geom2d_BSplineCurve) splineCurve(const Sketch& s, int entity);
TopoDS_Shape makeSpline(const gp_Ax3& plane, const Handle(Geom2d_BSplineCurve)& curve);

TopoDS_Shape makeEntity(const Sketch& s, const gp_Ax3& plane, int entity);

// Edges of `entities` plus vertices of `points`.
TopoDS_Compound makeShape(const Sketch& s, const gp_Ax3& plane,
                          const std::vector<int>& entities, const std::vector<int>& points);

// Offset copy of closed loops made of `entities` (lines and arcs), with sharp
// corners; positive distance grows the loops. Returns the new entities,
// empty if OCCT cannot offset them.
std::vector<int> offsetLoops(Sketch& s, const std::vector<int>& entities, double distance);

} // namespace sketch
