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

#include <algorithm>
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

// --- features -----------------------------------------------------------------

TopoDS_Shape SketchFeature::edges() const {
  std::vector<int> all(sketch.entities.size());
  std::iota(all.begin(), all.end(), 0);
  return sketch::makeShape(sketch, plane, all, {});
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

TopoDS_Shape ExtrudeFeature::build(const Document& doc, const TopoDS_Shape& body) const {
  const SketchFeature& s = inputSketch(doc, sketch);
  if (std::abs(distance) < 1e-9) fail("The extrude distance is zero.");
  BRepPrimAPI_MakePrism prism(s.profile(), gp_Vec(s.plane.Direction()) * distance);
  if (!prism.IsDone()) fail("Extrude failed.");
  return combine(body, prism.Shape(), op);
}

TopoDS_Shape RevolveFeature::build(const Document& doc, const TopoDS_Shape& body) const {
  const SketchFeature& s = inputSketch(doc, sketch);
  if (angle <= 0 || angle > 360) fail("The revolve angle must be between 0 and 360 degrees.");
  const gp_Ax1 ax(s.plane.Location(), axis == Axis::SketchX ? s.plane.XDirection() : s.plane.YDirection());
  BRepPrimAPI_MakeRevol revol(s.profile(), ax, angle * std::numbers::pi / 180.0);
  if (!revol.IsDone()) fail("Revolve failed.");
  return combine(body, revol.Shape(), op);
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
