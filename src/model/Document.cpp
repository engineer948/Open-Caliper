#include "model/Document.h"
#include "sketch/SketchShape.h"

#include <BOPAlgo_Tools.hxx>
#include <BOPTools_AlgoTools3D.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IntTools_Context.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <Standard_Failure.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <optional>
#include <set>

#include <numbers>
#include <numeric>
#include <stdexcept>

namespace model {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

const SketchFeature& inputSketch(const Document& doc, int index) {
  const SketchFeature* s = doc.as<SketchFeature>(size_t(index));
  if (!s) fail("The referenced sketch does not exist.");
  return *s;
}

TopoDS_Shape combine(const TopoDS_Shape& body, const TopoDS_Shape& tool, Operation op, bool checkTool = true) {
  if (checkTool && !BRepCheck_Analyzer(tool).IsValid())
    fail("The resulting solid is invalid (does the profile cross the axis?).");
  if (body.IsNull()) {
    if (op == Operation::Join) return tool;
    fail("There is no body to cut or intersect yet.");
  }
  std::unique_ptr<BRepAlgoAPI_BooleanOperation> algo;
  switch (op) {
  case Operation::Join: algo = std::make_unique<BRepAlgoAPI_Fuse>(body, tool); break;
  case Operation::Cut: algo = std::make_unique<BRepAlgoAPI_Cut>(body, tool); break;
  case Operation::Intersect: algo = std::make_unique<BRepAlgoAPI_Common>(body, tool); break;
  }
  if (!algo->IsDone() || algo->HasErrors()) fail("The boolean operation failed.");
  algo->SimplifyResult(); // merge coplanar faces left over from the operation
  return algo->Shape();
}

// Sub-shapes of `body` stored as 1-based indices plus keys (see ShapeKey).
// With keys the shapes are looked up by geometry, so renumbering does not matter.
std::vector<TopoDS_Shape> bodyShapes(const TopoDS_Shape& body, const std::vector<int>& indices,
                                     const std::vector<ShapeKey>& keys, TopAbs_ShapeEnum kind) {
  const std::string what = kind == TopAbs_EDGE ? "edge" : "face";
  if (body.IsNull()) fail("There is no body yet.");
  if (indices.empty() && keys.empty()) fail("No " + what + "s selected.");
  std::vector<TopoDS_Shape> shapes;
  if (!keys.empty()) {
    for (const ShapeKey& k : keys) {
      const TopoDS_Shape s = findShape(body, k, kind);
      if (s.IsNull()) fail("A selected " + what + " no longer exists (an earlier step changed the shape).");
      if (std::find(shapes.begin(), shapes.end(), s) == shapes.end()) shapes.push_back(s);
    }
    return shapes;
  }
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, kind, map);
  for (int i : indices) {
    if (i < 1 || i > map.Extent()) fail("A selected " + what + " no longer exists (an earlier step changed the shape).");
    shapes.push_back(map(i));
  }
  return shapes;
}

std::optional<gp_Dir> outwardNormal(const TopoDS_Face& f) {
  BRepAdaptor_Surface s(f);
  if (s.GetType() != GeomAbs_Plane) return std::nullopt;
  gp_Dir n = s.Plane().Axis().Direction();
  if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
  return n;
}

ShapeKey keyOf(const TopoDS_Shape& sub, const TopTools_IndexedDataMapOfShapeListOfShape* edgeFaces) {
  ShapeKey k;
  if (sub.IsNull()) return k;
  if (sub.ShapeType() == TopAbs_EDGE) {
    BRepAdaptor_Curve c(TopoDS::Edge(sub));
    k.type = int(c.GetType());
    k.point = c.Value((c.FirstParameter() + c.LastParameter()) / 2).XYZ();
    GProp_GProps g;
    BRepGProp::LinearProperties(sub, g);
    k.size = g.Mass();
    if (c.GetType() == GeomAbs_Line) k.dir = c.Line().Direction().XYZ();
    else if (c.GetType() == GeomAbs_Circle) k.dir = c.Circle().Axis().Direction().XYZ(), k.size = c.Circle().Radius();
    if (edgeFaces)
      if (const int i = edgeFaces->FindIndex(sub); i > 0)
        for (const TopoDS_Shape& f : edgeFaces->FindFromIndex(i))
          if (const auto n = outwardNormal(TopoDS::Face(f)))
            if (std::none_of(k.normals.begin(), k.normals.end(), [&](const gp_XYZ& m) { return m.IsEqual(n->XYZ(), 1e-9); }))
              k.normals.push_back(n->XYZ());
  } else if (sub.ShapeType() == TopAbs_FACE) {
    const TopoDS_Face& f = TopoDS::Face(sub);
    BRepAdaptor_Surface s(f);
    k.type = int(s.GetType());
    GProp_GProps g;
    BRepGProp::SurfaceProperties(sub, g);
    k.point = g.CentreOfMass().XYZ();
    k.size = g.Mass();
    if (const auto n = outwardNormal(f)) k.dir = n->XYZ();
    else if (s.GetType() == GeomAbs_Cylinder) k.dir = s.Cylinder().Axis().Direction().XYZ(), k.size = s.Cylinder().Radius();
  }
  return k;
}

bool parallel(const gp_XYZ& a, const gp_XYZ& b, bool oriented) {
  if (a.Modulus() < 1e-9 || b.Modulus() < 1e-9) return a.Modulus() < 1e-9 && b.Modulus() < 1e-9;
  const double d = a.Normalized().Dot(b.Normalized());
  return (oriented ? d : std::abs(d)) > 0.999;
}

} // namespace

int edgeIndex(const TopoDS_Shape& body, const TopoDS_Shape& edge) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_EDGE, map);
  return map.FindIndex(edge);
}

int faceIndex(const TopoDS_Shape& body, const TopoDS_Shape& face) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_FACE, map);
  return map.FindIndex(face);
}

ShapeKey shapeKey(const TopoDS_Shape& body, const TopoDS_Shape& sub) {
  if (sub.IsNull()) return {};
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  if (sub.ShapeType() == TopAbs_EDGE && !body.IsNull())
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  return keyOf(sub, &edgeFaces);
}

// Same kind of curve/surface with the same direction is required. Edges
// also prefer the same neighbouring face normals (a box edge between the top
// and the right side stays that edge when the box grows), then the nearest
// position wins.
TopoDS_Shape findShape(const TopoDS_Shape& body, const ShapeKey& key, TopAbs_ShapeEnum kind) {
  if (body.IsNull() || !key.valid()) return {};
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, kind, map);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  if (kind == TopAbs_EDGE) TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  std::vector<ShapeKey> keys;
  for (int i = 1; i <= map.Extent(); ++i) keys.push_back(keyOf(map(i), &edgeFaces));

  const bool orientedDir = kind == TopAbs_FACE && key.type == int(GeomAbs_Plane); // top vs bottom face
  const bool hasRadius = key.type == (kind == TopAbs_EDGE ? int(GeomAbs_Circle) : int(GeomAbs_Cylinder));
  for (const bool strict : {true, false}) {
    double best = std::numeric_limits<double>::max();
    int found = -1;
    for (int i = 0; i < int(keys.size()); ++i) {
      const ShapeKey& c = keys[i];
      if (c.type != key.type || !parallel(c.dir, key.dir, orientedDir)) continue;
      if (strict) {
        if (c.normals.size() != key.normals.size()) continue;
        const bool same = std::all_of(key.normals.begin(), key.normals.end(), [&](const gp_XYZ& n) {
          return std::any_of(c.normals.begin(), c.normals.end(), [&](const gp_XYZ& m) { return n.Dot(m) > 0.999; });
        });
        if (!same) continue;
      }
      double score = (c.point - key.point).Modulus();
      if (hasRadius) score += std::abs(c.size - key.size);
      if (score < best) best = score, found = i;
    }
    if (found >= 0) return map(found + 1);
  }
  return {};
}

MassProperties massProperties(const TopoDS_Shape& shape) {
  MassProperties m;
  if (shape.IsNull()) return m;
  GProp_GProps vol, surf;
  BRepGProp::VolumeProperties(shape, vol);
  BRepGProp::SurfaceProperties(shape, surf);
  m.volume = vol.Mass();
  m.area = surf.Mass();
  m.center = vol.CentreOfMass();
  Bnd_Box box;
  BRepBndLib::AddOptimal(shape, box, false);
  if (!box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    m.size = gp_XYZ(x1 - x0, y1 - y0, z1 - z0);
  }
  return m;
}

namespace {

std::string mm(double v) {
  char buf[64];
  const auto end = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 3).ptr; // '.' in any language
  std::string s(buf, end); // drop trailing zeros: 12.500 -> 12.5
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  return s;
}

std::string describeOne(const TopoDS_Shape& s) {
  switch (s.ShapeType()) {
  case TopAbs_VERTEX: {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(s));
    return "Point: X " + mm(p.X()) + ", Y " + mm(p.Y()) + ", Z " + mm(p.Z()) + " mm";
  }
  case TopAbs_EDGE: {
    GProp_GProps g;
    BRepGProp::LinearProperties(s, g);
    std::string text = "Edge length: " + mm(g.Mass()) + " mm";
    BRepAdaptor_Curve c(TopoDS::Edge(s));
    if (c.GetType() == GeomAbs_Circle)
      text += "\nCircle: diameter " + mm(2 * c.Circle().Radius()) + " mm (radius " + mm(c.Circle().Radius()) + " mm)";
    return text;
  }
  case TopAbs_FACE: {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    std::string text = "Face area: " + mm(g.Mass()) + " mm²";
    BRepAdaptor_Surface f(TopoDS::Face(s));
    if (f.GetType() == GeomAbs_Cylinder) text += "\nCylinder: diameter " + mm(2 * f.Cylinder().Radius()) + " mm";
    return text;
  }
  default:
    return {};
  }
}

// Direction of a straight edge or normal of a flat face, if it has one.
std::optional<gp_Dir> direction(const TopoDS_Shape& s) {
  if (s.ShapeType() == TopAbs_EDGE) {
    BRepAdaptor_Curve c(TopoDS::Edge(s));
    if (c.GetType() == GeomAbs_Line) return c.Line().Direction();
  } else if (s.ShapeType() == TopAbs_FACE) {
    BRepAdaptor_Surface f(TopoDS::Face(s));
    if (f.GetType() == GeomAbs_Plane) return f.Plane().Axis().Direction();
  }
  return std::nullopt;
}

} // namespace

std::string measure(const std::vector<TopoDS_Shape>& picked) {
  if (picked.size() == 1) return describeOne(picked[0]);
  if (picked.size() != 2) return {};
  BRepExtrema_DistShapeShape dist(picked[0], picked[1]);
  if (!dist.IsDone()) return {};
  std::string text = "Distance: " + mm(dist.Value()) + " mm";
  const auto d0 = direction(picked[0]), d1 = direction(picked[1]);
  if (d0 && d1) {
    // Plane normals are not oriented, so report the acute angle (0-90°).
    double a = d0->Angle(*d1) * 180.0 / std::numbers::pi;
    a = std::min(a, 180 - a);
    if (picked[0].ShapeType() != picked[1].ShapeType()) a = 90 - a; // edge vs. face normal -> edge vs. face
    text += "\nAngle: " + mm(a) + "°";
  }
  const gp_Pnt a = dist.PointOnShape1(1), b = dist.PointOnShape2(1);
  text += "\nΔX " + mm(std::abs(b.X() - a.X())) + ", ΔY " + mm(std::abs(b.Y() - a.Y())) + ", ΔZ " +
          mm(std::abs(b.Z() - a.Z())) + " mm";
  return text;
}

// --- features -----------------------------------------------------------------

TopoDS_Shape SketchFeature::edges() const {
  std::vector<int> real; // construction geometry is left out of profiles
  for (int i = 0; i < int(sketch.entities.size()); ++i)
    if (!sketch.entities[i].construction) real.push_back(i);
  return sketch::makeShape(sketch, plane, real, {});
}

TopoDS_Shape SketchFeature::profile() const {
  if (sketch.entities.empty()) fail("The sketch is empty.");
  TopoDS_Shape wires, faces;
  if (BOPAlgo_Tools::EdgesToWires(edges(), wires) != 0 || !BOPAlgo_Tools::WiresToFaces(wires, faces))
    fail("The sketch has no closed profile.");

  // WiresToFaces returns every region, holes included. Even-odd fill: a region
  // inside an odd number of other regions' outer loops is a hole.
  std::vector<TopoDS_Face> regions, filled;
  for (TopExp_Explorer it(faces, TopAbs_FACE); it.More(); it.Next()) {
    regions.push_back(TopoDS::Face(it.Current()));
    filled.push_back(BRepBuilderAPI_MakeFace(BRepTools::OuterWire(regions.back()), true).Face());
  }
  Handle(IntTools_Context) ctx = new IntTools_Context();
  BRep_Builder builder;
  TopoDS_Compound result;
  builder.MakeCompound(result);
  bool any = false;
  for (size_t i = 0; i < regions.size(); ++i) {
    gp_Pnt inside;
    gp_Pnt2d uv;
    if (BOPTools_AlgoTools3D::PointInFace(regions[i], inside, uv, ctx) != 0) continue;
    int depth = 0;
    for (size_t j = 0; j < regions.size(); ++j)
      if (j != i && BRepClass_FaceClassifier(filled[j], inside, 1e-7).State() == TopAbs_IN) ++depth;
    if (depth % 2 == 0) {
      builder.Add(result, regions[i]);
      any = true;
    }
  }
  if (!any) fail("The sketch has no closed profile.");
  return result;
}

TopoDS_Shape ExtrudeFeature::tool(const Document& doc) const {
  const SketchFeature& s = inputSketch(doc, sketch);
  if (std::abs(distance) < 1e-9) fail("The extrude distance is zero.");
  BRepPrimAPI_MakePrism prism(s.profile(), gp_Vec(s.plane.Direction()) * distance);
  if (!prism.IsDone()) fail("Extrude failed.");
  return prism.Shape();
}

TopoDS_Shape ExtrudeFeature::build(const Document& doc, const TopoDS_Shape& body) const {
  return combine(body, tool(doc), op);
}

TopoDS_Shape RevolveFeature::tool(const Document& doc) const {
  const SketchFeature& s = inputSketch(doc, sketch);
  if (angle <= 0 || angle > 360) fail("The revolve angle must be between 0 and 360 degrees.");
  const gp_Ax1 ax(s.plane.Location(), axis == Axis::SketchX ? s.plane.XDirection() : s.plane.YDirection());
  BRepPrimAPI_MakeRevol revol(s.profile(), ax, angle * std::numbers::pi / 180.0);
  if (!revol.IsDone()) fail("Revolve failed.");
  return revol.Shape();
}

TopoDS_Shape RevolveFeature::build(const Document& doc, const TopoDS_Shape& body) const {
  return combine(body, tool(doc), op);
}

TopoDS_Shape ShellFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (thickness <= 0) fail("The wall thickness must be positive.");
  if (body.IsNull()) fail("There is no body yet.");
  if (faces.empty() && keys.empty()) fail("Select at least one face to open.");
  TopTools_ListOfShape open;
  for (const TopoDS_Shape& f : bodyShapes(body, faces, keys, TopAbs_FACE)) open.Append(f);
  BRepOffsetAPI_MakeThickSolid shell;
  shell.MakeThickSolidByJoin(body, open, -thickness, 1e-3); // negative: walls grow inwards
  shell.Build();
  if (!shell.IsDone()) fail("Shell failed (wall too thick for this shape?).");
  return shell.Shape();
}

TopoDS_Shape PatternFeature::build(const Document& doc, const TopoDS_Shape& body) const {
  if (count < 2) fail("A pattern needs at least 2 copies.");
  if (count > 500) fail("At most 500 copies.");
  TopoDS_Shape original;
  Operation op = Operation::Join;
  if (const auto* e = doc.as<ExtrudeFeature>(size_t(source))) original = e->tool(doc), op = e->op;
  else if (const auto* r = doc.as<RevolveFeature>(size_t(source))) original = r->tool(doc), op = r->op;
  else if (const auto* h = doc.as<HoleFeature>(size_t(source))) original = h->tool(doc, body), op = Operation::Cut;
  else fail("A pattern repeats an extrude, a revolve or a hole.");

  const gp_Dir dir = axis == Axis::X ? gp::DX() : axis == Axis::Y ? gp::DY() : gp::DZ();
  // A full circle has no copy on top of the original: 360/count; an arc ends on its last copy.
  const bool full = std::abs(angle - 360) < 1e-9;
  const double stepAngle = angle * std::numbers::pi / 180.0 / (full ? count : count - 1);
  TopoDS_Shape result = body;
  for (int i = 1; i < count; ++i) { // copy 0 is the original feature itself
    gp_Trsf t;
    if (kind == Kind::Linear) t.SetTranslation(gp_Vec(dir) * (spacing * i));
    else t.SetRotation(gp_Ax1(gp::Origin(), dir), stepAngle * i);
    result = combine(result, BRepBuilderAPI_Transform(original, t, true).Shape(), op);
  }
  return result;
}

bool PatternFeature::setValue(const std::string& f, double v) {
  if (f == "count") count = int(std::lround(v));
  else if (f == "spacing") spacing = v;
  else if (f == "angle") angle = v;
  else return false;
  return true;
}

bool HoleFeature::setValue(const std::string& f, double v) {
  if (f == "diameter") diameter = v;
  else if (f == "depth") depth = v;
  else if (f == "bore-diameter") boreDiameter = v;
  else if (f == "bore-depth") boreDepth = v;
  else if (f == "sink-diameter") sinkDiameter = v;
  else if (f == "sink-angle") sinkAngle = v;
  else return false;
  return true;
}

TopoDS_Shape HoleFeature::tool(const Document& doc, const TopoDS_Shape& body) const {
  const SketchFeature& s = inputSketch(doc, sketch);
  if (diameter <= 0) fail("The hole diameter must be positive.");
  if (depth < 0) fail("The hole depth must be positive (0 = through all).");
  if (kind == Kind::Counterbore && (boreDiameter <= diameter || boreDepth <= 0))
    fail("The counterbore must be wider than the hole and have a depth.");
  if (kind == Kind::Countersink && (sinkDiameter <= diameter || sinkAngle <= 0 || sinkAngle >= 180))
    fail("The countersink must be wider than the hole, with an angle between 0 and 180°.");

  gp_Dir down = s.plane.Direction(); // into the material for a sketch on a face
  if (!flip) down.Reverse();
  double length = depth;
  if (length == 0) { // through all: past the far side of the body from any hole
    Bnd_Box box;
    if (!body.IsNull()) BRepBndLib::Add(body, box);
    if (box.IsVoid()) fail("There is no body to drill through yet.");
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    length = 2 * (std::sqrt(box.SquareExtent()) + s.plane.Location().Distance(gp_Pnt((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)));
  }
  constexpr double lift = 0.01; // start just above the face: no coplanar faces in the cut

  BRep_Builder builder;
  TopoDS_Compound holes;
  builder.MakeCompound(holes);
  bool any = false;
  for (int e = 0; e < int(s.sketch.entities.size()); ++e) {
    const sketch::Entity& en = s.sketch.entities[e];
    if (en.kind != sketch::EntityKind::Circle || en.construction) continue;
    const gp_Pnt top = sketch::toWorld(s.plane, s.sketch.x(en.p[0]), s.sketch.y(en.p[0])).Translated(gp_Vec(down) * -lift);
    const gp_Ax2 ax(top, down);
    TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(ax, diameter / 2, length + lift).Shape();
    if (kind == Kind::Counterbore)
      hole = BRepAlgoAPI_Fuse(hole, BRepPrimAPI_MakeCylinder(ax, boreDiameter / 2, boreDepth + lift).Shape()).Shape();
    if (kind == Kind::Countersink) {
      const double slope = std::tan(sinkAngle / 2 * std::numbers::pi / 180); // radius change per depth
      const double h = (sinkDiameter - diameter) / 2 / slope;
      hole = BRepAlgoAPI_Fuse(hole, BRepPrimAPI_MakeCone(ax, sinkDiameter / 2 + lift * slope, diameter / 2, h + lift).Shape()).Shape();
    }
    builder.Add(holes, hole);
    any = true;
  }
  if (!any) fail("Draw a circle in the sketch where each hole goes.");
  return holes;
}

TopoDS_Shape HoleFeature::build(const Document& doc, const TopoDS_Shape& body) const {
  if (body.IsNull()) fail("There is no body to drill into yet.");
  return combine(body, tool(doc, body), Operation::Cut, false);
}

TopoDS_Shape MirrorFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (body.IsNull()) fail("There is no body to mirror yet.");
  gp_Ax2 mirror;
  switch (plane) {
  case Plane::XY: mirror = gp_Ax2(gp::Origin(), gp::DZ()); break;
  case Plane::XZ: mirror = gp_Ax2(gp::Origin(), gp::DY()); break;
  case Plane::YZ: mirror = gp_Ax2(gp::Origin(), gp::DX()); break;
  case Plane::Face: {
    const std::vector<TopoDS_Shape> f =
        bodyShapes(body, face >= 0 ? std::vector<int>{face} : std::vector<int>{},
                   key.valid() ? std::vector<ShapeKey>{key} : std::vector<ShapeKey>{}, TopAbs_FACE);
    BRepAdaptor_Surface surface(TopoDS::Face(f[0]));
    if (surface.GetType() != GeomAbs_Plane) fail("The mirror face is not flat.");
    mirror = gp_Ax2(surface.Plane().Location(), surface.Plane().Axis().Direction());
    break;
  }
  }
  gp_Trsf t;
  t.SetMirror(mirror);
  return combine(body, BRepBuilderAPI_Transform(body, t, true).Shape(), Operation::Join, false);
}

TopoDS_Shape FilletFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (radius <= 0) fail("The fillet radius must be positive.");
  BRepFilletAPI_MakeFillet fillet(body);
  for (const TopoDS_Shape& e : bodyShapes(body, edges, keys, TopAbs_EDGE)) fillet.Add(radius, TopoDS::Edge(e));
  fillet.Build();
  if (!fillet.IsDone()) fail("Fillet failed (radius too large for the selected edges?).");
  return fillet.Shape();
}

TopoDS_Shape ChamferFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (distance <= 0) fail("The chamfer distance must be positive.");
  BRepFilletAPI_MakeChamfer chamfer(body);
  for (const TopoDS_Shape& e : bodyShapes(body, edges, keys, TopAbs_EDGE)) chamfer.Add(distance, TopoDS::Edge(e));
  chamfer.Build();
  if (!chamfer.IsDone()) fail("Chamfer failed (distance too large for the selected edges?).");
  return chamfer.Shape();
}

TopoDS_Shape ImportFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (shape.IsNull()) fail("The imported file contained no shape.");
  return combine(body, shape, Operation::Join, false); // files are not always strictly valid
}

// --- document -----------------------------------------------------------------

Document::Document(const Document& other)
    : m_parameters(other.m_parameters), m_values(other.m_values), m_parameterError(other.m_parameterError),
      m_bodies(other.m_bodies), m_errors(other.m_errors) {
  for (const auto& f : other.m_history) m_history.push_back(f->clone());
}

Document& Document::operator=(const Document& other) {
  if (this != &other) *this = Document(other);
  return *this;
}

void Document::replace(size_t i, std::unique_ptr<Feature> feature) {
  m_history[i] = std::move(feature);
  regenerate(i);
}

int Document::removeWithDependents(size_t i) {
  std::set<int> doomed{int(i)};
  for (size_t k = i + 1; k < m_history.size(); ++k)
    if (doomed.count(m_history[k]->sketchInput())) doomed.insert(int(k));
  for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) m_history.erase(m_history.begin() + *it);
  for (auto& f : m_history) // shift sketch references past the removed entries
    if (int s = f->sketchInput(); s >= 0)
      f->setSketchInput(s - int(std::count_if(doomed.begin(), doomed.end(), [s](int d) { return d < s; })));
  regenerate(i);
  return int(doomed.size());
}

bool Document::hasErrors() const {
  return std::any_of(m_errors.begin(), m_errors.end(), [](const std::string& e) { return !e.empty(); });
}

int Document::add(std::unique_ptr<Feature> feature) {
  m_history.push_back(std::move(feature));
  regenerate(m_history.size() - 1);
  return int(m_history.size()) - 1;
}

void Document::removeLast() {
  m_history.pop_back();
  m_bodies.resize(m_history.size());
  m_errors.resize(m_history.size());
}

void Document::setParameters(std::vector<Parameter> params) {
  m_parameters = std::move(params);
  regenerate(0);
}

void Document::evaluateParameters() {
  try {
    m_values = model::evaluateParameters(m_parameters);
    m_parameterError.clear();
  } catch (const std::exception& e) {
    m_values.clear();
    m_parameterError = e.what();
  }
}

void Document::applyExpressions(Feature& f) const {
  auto* s = dynamic_cast<SketchFeature*>(&f);
  const bool sketchUses = s && std::any_of(s->sketch.constraints.begin(), s->sketch.constraints.end(),
                                           [](const sketch::Constraint& c) { return !c.expr.empty(); });
  if (!m_parameterError.empty() && (sketchUses || !f.expressions.empty()))
    fail("Parameters: " + m_parameterError);
  for (const auto& [field, expr] : f.expressions)
    if (!f.setValue(field, model::evaluate(expr, m_values))) fail("Unknown size '" + field + "'.");
  if (!sketchUses) return;
  bool changed = false;
  for (sketch::Constraint& c : s->sketch.constraints) {
    if (c.expr.empty()) continue;
    const double v = model::evaluate(c.expr, m_values);
    if (v <= 0) fail("\"" + c.expr + "\" must be a positive size.");
    if (v != c.value) c.value = v, changed = true;
  }
  if (changed && s->sketch.solve() == sketch::SolveResult::Failed)
    fail("With these parameter values the sketch has no solution.");
}

void Document::regenerate(size_t from) {
  m_bodies.resize(m_history.size());
  m_errors.resize(m_history.size());
  evaluateParameters();
  for (size_t i = from; i < m_history.size(); ++i) {
    const TopoDS_Shape before = i == 0 ? TopoDS_Shape() : m_bodies[i - 1];
    m_bodies[i] = before; // a failed feature leaves the body as it was
    try {
      applyExpressions(*m_history[i]);
      m_bodies[i] = m_history[i]->build(*this, before);
      m_errors[i].clear();
    } catch (const Standard_Failure& e) { // first: in OCCT 8 it is a std::exception
      m_errors[i] = std::string("Geometry kernel error: ") + e.GetMessageString();
    } catch (const std::exception& e) {
      m_errors[i] = e.what();
    }
  }
}

bool Document::isConsumed(size_t sketchIndex) const {
  for (size_t i = sketchIndex + 1; i < m_history.size(); ++i)
    if (m_history[i]->sketchInput() == int(sketchIndex)) return true;
  return false;
}

} // namespace model
