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
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <Standard_Failure.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
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
#include <cstdio>
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

// Edge list of `body` by stored 1-based indices.
std::vector<TopoDS_Edge> bodyEdges(const TopoDS_Shape& body, const std::vector<int>& indices) {
  if (body.IsNull()) fail("There is no body yet.");
  if (indices.empty()) fail("No edges selected.");
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_EDGE, map);
  std::vector<TopoDS_Edge> edges;
  for (int i : indices) {
    if (i < 1 || i > map.Extent()) fail("A selected edge no longer exists (an earlier step changed the shape).");
    edges.push_back(TopoDS::Edge(map(i)));
  }
  return edges;
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
  std::snprintf(buf, sizeof buf, "%.3f", v);
  std::string s = buf; // drop trailing zeros: 12.500 -> 12.5
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
  if (faces.empty()) fail("Select at least one face to open.");
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_FACE, map);
  TopTools_ListOfShape open;
  for (int i : faces) {
    if (i < 1 || i > map.Extent()) fail("A selected face no longer exists (an earlier step changed the shape).");
    open.Append(map(i));
  }
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
  else fail("A pattern repeats an extrude or a revolve.");

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

TopoDS_Shape FilletFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (radius <= 0) fail("The fillet radius must be positive.");
  BRepFilletAPI_MakeFillet fillet(body);
  for (const TopoDS_Edge& e : bodyEdges(body, edges)) fillet.Add(radius, e);
  fillet.Build();
  if (!fillet.IsDone()) fail("Fillet failed (radius too large for the selected edges?).");
  return fillet.Shape();
}

TopoDS_Shape ChamferFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (distance <= 0) fail("The chamfer distance must be positive.");
  BRepFilletAPI_MakeChamfer chamfer(body);
  for (const TopoDS_Edge& e : bodyEdges(body, edges)) chamfer.Add(distance, e);
  chamfer.Build();
  if (!chamfer.IsDone()) fail("Chamfer failed (distance too large for the selected edges?).");
  return chamfer.Shape();
}

TopoDS_Shape ImportFeature::build(const Document&, const TopoDS_Shape& body) const {
  if (shape.IsNull()) fail("The imported file contained no shape.");
  return combine(body, shape, Operation::Join, false); // files are not always strictly valid
}

// --- document -----------------------------------------------------------------

Document::Document(const Document& other) : m_bodies(other.m_bodies), m_errors(other.m_errors) {
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

void Document::regenerate(size_t from) {
  m_bodies.resize(m_history.size());
  m_errors.resize(m_history.size());
  for (size_t i = from; i < m_history.size(); ++i) {
    const TopoDS_Shape before = i == 0 ? TopoDS_Shape() : m_bodies[i - 1];
    m_bodies[i] = before; // a failed feature leaves the body as it was
    try {
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
