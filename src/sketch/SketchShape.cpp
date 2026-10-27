#include "sketch/SketchShape.h"

#include <BOPAlgo_Tools.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <GeomAPI.hxx>
#include <Geom_Curve.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Pln.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
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

Handle(Geom2d_BSplineCurve) interpolate(const std::vector<std::pair<double, double>>& points, bool closed) {
  if (points.size() < (closed ? 3u : 2u)) return {};
  for (size_t i = 1; i < points.size(); ++i) // the same point twice in a row has no direction
    if (std::hypot(points[i].first - points[i - 1].first, points[i].second - points[i - 1].second) < kMinSize) return {};
  Handle(TColgp_HArray1OfPnt2d) pts = new TColgp_HArray1OfPnt2d(1, int(points.size()));
  for (int i = 0; i < int(points.size()); ++i) pts->SetValue(i + 1, gp_Pnt2d(points[i].first, points[i].second));
  try {
    Geom2dAPI_Interpolate interp(pts, closed, kMinSize);
    interp.Perform();
    if (!interp.IsDone()) return {};
    return interp.Curve();
  } catch (const Standard_Failure&) {
    return {};
  }
}

Handle(Geom2d_BSplineCurve) splineCurve(const Sketch& s, int entity) {
  const Entity& e = s.entities[entity];
  const bool closed = e.closedSpline();
  std::vector<std::pair<double, double>> points;
  for (size_t i = 0; i < e.pts.size() - (closed ? 1 : 0); ++i) points.emplace_back(s.x(e.pts[i]), s.y(e.pts[i]));
  return interpolate(points, closed);
}

TopoDS_Shape makeSpline(const gp_Ax3& plane, const Handle(Geom2d_BSplineCurve)& curve) {
  if (curve.IsNull()) return {};
  return BRepBuilderAPI_MakeEdge(GeomAPI::To3d(curve, gp_Pln(plane))).Edge();
}

TopoDS_Shape makeEntity(const Sketch& s, const gp_Ax3& plane, int entity) {
  const Entity& e = s.entities[entity];
  switch (e.kind) {
  case EntityKind::Spline:
    return makeSpline(plane, splineCurve(s, entity));
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

std::vector<int> offsetLoops(Sketch& s, const std::vector<int>& entities, double distance) {
  const gp_Ax3 plane; // sketch coordinates are the XY plane here
  BRep_Builder builder;
  TopoDS_Compound edges;
  builder.MakeCompound(edges);
  for (int e : entities)
    if (const TopoDS_Shape edge = makeEntity(s, plane, e); !edge.IsNull()) builder.Add(edges, edge);

  std::vector<int> added, newPoints;
  auto point = [&](const gp_Pnt& p) { // reuse the ends shared by neighbouring pieces
    for (int q : newPoints)
      if (std::hypot(s.x(q) - p.X(), s.y(q) - p.Y()) < 1e-7) return q;
    newPoints.push_back(s.addPoint(p.X(), p.Y()));
    return newPoints.back();
  };
  try {
    TopoDS_Shape wires;
    if (BOPAlgo_Tools::EdgesToWires(edges, wires) != 0) return {};
    for (TopExp_Explorer w(wires, TopAbs_WIRE); w.More(); w.Next()) {
      BRepBuilderAPI_MakeFace face(TopoDS::Wire(w.Current()), true);
      if (!face.IsDone()) return {};
      BRepOffsetAPI_MakeOffset offset(face.Face(), GeomAbs_Intersection);
      offset.Perform(distance);
      if (!offset.IsDone()) return {};
      for (TopExp_Explorer it(offset.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(it.Current()));
        const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (c.GetType() == GeomAbs_Line) {
          if (a.Distance(b) > 1e-9) added.push_back(s.addLine(point(a), point(b)));
        } else if (c.GetType() == GeomAbs_Circle) {
          const gp_Circ circ = c.Circle();
          const gp_Pnt centre = circ.Location();
          if (c.LastParameter() - c.FirstParameter() > 2 * std::numbers::pi - 1e-9) {
            added.push_back(s.addCircle(s.addPoint(centre.X(), centre.Y()), circ.Radius()));
          } else { // counter-clockwise in the sketch when the circle's axis points up
            const bool up = circ.Axis().Direction().Z() > 0;
            added.push_back(s.addArc(s.addPoint(centre.X(), centre.Y()), point(up ? a : b), point(up ? b : a)));
          }
        } else {
          return {}; // splines have no offset in the sketch
        }
      }
    }
  } catch (const Standard_Failure&) {
    return {};
  }
  return added;
}

} // namespace sketch
