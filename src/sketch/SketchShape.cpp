#include "sketch/SketchShape.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <cmath>
#include <numbers>

namespace sketch {
namespace {
constexpr double kMinSize = 1e-9;

gp_Circ circleOn(const gp_Ax3& plane, double cu, double cv, double r) {
  return gp_Circ(gp_Ax2(toWorld(plane, cu, cv), plane.Direction(), plane.XDirection()), r);
}
} // namespace

gp_Pnt toWorld(const gp_Ax3& plane, double u, double v) {
  return plane.Location().Translated(gp_Vec(plane.XDirection()) * u + gp_Vec(plane.YDirection()) * v);
}

TopoDS_Shape makeSegment(const gp_Ax3& plane, double u0, double v0, double u1, double v1) {
  if (std::hypot(u1 - u0, v1 - v0) < kMinSize) return {};
  return BRepBuilderAPI_MakeEdge(toWorld(plane, u0, v0), toWorld(plane, u1, v1)).Edge();
}

TopoDS_Shape makeCircle(const gp_Ax3& plane, double cu, double cv, double r) {
  if (r < kMinSize) return {};
  return BRepBuilderAPI_MakeEdge(circleOn(plane, cu, cv, r)).Edge();
}

TopoDS_Shape makeArc(const gp_Ax3& plane, double cu, double cv, double su, double sv, double eu, double ev) {
  const double r = std::hypot(su - cu, sv - cv);
  if (r < kMinSize) return {};
  // Circle parameter = angle from the plane's X axis, counter-clockwise.
  const double a1 = std::atan2(sv - cv, su - cu);
  double a2 = std::atan2(ev - cv, eu - cu);
  while (a2 <= a1 + kMinSize) a2 += 2 * std::numbers::pi;
  return BRepBuilderAPI_MakeEdge(circleOn(plane, cu, cv, r), a1, a2).Edge();
}

TopoDS_Shape makeEntity(const Sketch& s, const gp_Ax3& plane, int entity) {
  const Entity& e = s.entities[entity];
  switch (e.kind) {
  case EntityKind::Line:
    return makeSegment(plane, s.x(e.p[0]), s.y(e.p[0]), s.x(e.p[1]), s.y(e.p[1]));
  case EntityKind::Circle:
    return makeCircle(plane, s.x(e.p[0]), s.y(e.p[0]), s.radius(entity));
  case EntityKind::Arc:
    return makeArc(plane, s.x(e.p[0]), s.y(e.p[0]), s.x(e.p[1]), s.y(e.p[1]), s.x(e.p[2]), s.y(e.p[2]));
  }
  return {};
}

TopoDS_Compound makeShape(const Sketch& s, const gp_Ax3& plane,
                          const std::vector<int>& entities, const std::vector<int>& points) {
  BRep_Builder builder;
  TopoDS_Compound result;
  builder.MakeCompound(result);
  for (int e : entities) {
    const TopoDS_Shape edge = makeEntity(s, plane, e);
    if (!edge.IsNull()) builder.Add(result, edge);
  }
  for (int p : points)
    builder.Add(result, BRepBuilderAPI_MakeVertex(toWorld(plane, s.x(p), s.y(p))).Vertex());
  return result;
}

} // namespace sketch
