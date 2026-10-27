// Feature tests: build real solids and check their volumes.
#include "io/Exchange.h"
#include "model/Document.h"
#include "sketch/SketchShape.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <filesystem>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

using namespace model;

static int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

static double volume(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(s, props);
  return props.Mass();
}

static bool near(double a, double b) { return std::abs(a - b) < 1e-6 * std::max(1.0, std::abs(b)); }

static std::unique_ptr<SketchFeature> rectangle(double x0, double y0, double x1, double y1) {
  auto f = std::make_unique<SketchFeature>();
  f->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& s = f->sketch;
  const int a = s.addPoint(x0, y0), b = s.addPoint(x1, y0), c = s.addPoint(x1, y1), d = s.addPoint(x0, y1);
  s.addLine(a, b);
  s.addLine(b, c);
  s.addLine(c, d);
  s.addLine(d, a);
  return f;
}

static std::unique_ptr<ExtrudeFeature> extrude(int sketch, double distance, Operation op = Operation::Join) {
  auto e = std::make_unique<ExtrudeFeature>();
  e->sketch = sketch;
  e->distance = distance;
  e->op = op;
  return e;
}

static void extrudeBox() {
  Document doc;
  const int s = doc.add(rectangle(0, 0, 10, 10));
  const int e = doc.add(extrude(s, 10));
  CHECK(doc.error(e).empty());
  CHECK(near(volume(doc.body()), 1000));
  CHECK(doc.isConsumed(s));
}

static void holeFromInnerLoop() {
  Document doc;
  auto sk = rectangle(0, 0, 40, 20);
  sk->sketch.addCircle(sk->sketch.addPoint(20, 10), 5);
  const int s = doc.add(std::move(sk));
  doc.add(extrude(s, 2));
  CHECK(near(volume(doc.body()), (800 - std::numbers::pi * 25) * 2));
}

static void islandInsideHoleIsSolid() {
  Document doc;
  auto sk = rectangle(0, 0, 40, 40);
  sk->sketch.addCircle(sk->sketch.addPoint(20, 20), 10);
  sk->sketch.addCircle(sk->sketch.addPoint(20, 20), 4);
  const int s = doc.add(std::move(sk));
  doc.add(extrude(s, 1));
  CHECK(near(volume(doc.body()), 1600 - std::numbers::pi * 100 + std::numbers::pi * 16));
}

static void cutAndRegenerate() {
  Document doc;
  const int s1 = doc.add(rectangle(0, 0, 10, 10));
  doc.add(extrude(s1, 10));
  const int s2 = doc.add(rectangle(2, 2, 4, 4));
  const int cut = doc.add(extrude(s2, 10, Operation::Cut));
  CHECK(doc.error(cut).empty());
  CHECK(near(volume(doc.body()), 1000 - 40));
}

static void revolveRing() {
  Document doc;
  const int s = doc.add(rectangle(10, 0, 20, 10)); // revolved around the sketch Y axis
  auto r = std::make_unique<RevolveFeature>();
  r->sketch = s;
  const int f = doc.add(std::move(r));
  CHECK(doc.error(f).empty());
  CHECK(near(volume(doc.body()), std::numbers::pi * (400 - 100) * 10));
}

static void errorsAreReported() {
  Document doc;
  auto open = std::make_unique<SketchFeature>();
  open->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  open->sketch.addLine(open->sketch.addPoint(0, 0), open->sketch.addPoint(10, 0));
  const int s = doc.add(std::move(open));
  const int e = doc.add(extrude(s, 5));
  CHECK(!doc.error(e).empty());
  CHECK(doc.body().IsNull());

  Document doc2;
  const int s2 = doc2.add(rectangle(0, 0, 1, 1));
  const int c = doc2.add(extrude(s2, 1, Operation::Cut));
  CHECK(!doc2.error(c).empty()); // nothing to cut
}

// 1-based index of an edge of the box lying along Z at x=10, y=10.
static int verticalEdgeAt(const TopoDS_Shape& body, double x, double y) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_EDGE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    GProp_GProps p;
    BRepGProp::LinearProperties(map(i), p);
    const gp_Pnt c = p.CentreOfMass();
    if (std::abs(c.X() - x) < 1e-6 && std::abs(c.Y() - y) < 1e-6) return i;
  }
  return 0;
}

static void filletAndChamfer() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  auto fillet = std::make_unique<FilletFeature>();
  fillet->edges = {verticalEdgeAt(doc.body(), 10, 10)};
  fillet->radius = 1;
  CHECK(fillet->edges[0] > 0);
  const int f = doc.add(std::move(fillet));
  CHECK(doc.error(f).empty());
  CHECK(near(volume(doc.body()), 1000 - (1 - std::numbers::pi / 4) * 10));

  auto chamfer = std::make_unique<ChamferFeature>();
  chamfer->edges = {verticalEdgeAt(doc.body(), 0, 0)};
  chamfer->distance = 1;
  const int c = doc.add(std::move(chamfer));
  CHECK(doc.error(c).empty());
  CHECK(near(volume(doc.body()), 1000 - (1 - std::numbers::pi / 4) * 10 - 5));
}

static void removeWithDependentsShiftsReferences() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  doc.add(extrude(doc.add(rectangle(20, 0, 30, 10)), 5));
  CHECK(doc.removeWithDependents(0) == 2);
  CHECK(doc.size() == 2);
  CHECK(doc.feature(1).sketchInput() == 0);
  CHECK(!doc.hasErrors());
  CHECK(near(volume(doc.body()), 500));
}

static void copyIsIndependent() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  Document copy = doc;
  auto longer = extrude(0, 20);
  copy.replace(1, std::move(longer));
  CHECK(near(volume(copy.body()), 2000));
  CHECK(near(volume(doc.body()), 1000));
}

static void exportImportRoundTrip() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  const auto dir = std::filesystem::temp_directory_path();
  for (const char* name : {"rt.step", "rt.brep", "rt.iges"}) {
    const std::string path = (dir / name).string();
    io::exportShape(doc.body(), path);
    CHECK(near(volume(io::importShape(path)), 1000));
  }
  for (const char* name : {"rt.stl", "rt.obj", "rt.3mf"}) {
    const auto path = dir / name;
    io::exportShape(doc.body(), path.string());
    CHECK(std::filesystem::file_size(path) > 100);
  }
}

// 1-based index of the face whose centre is at `c`.
static int faceAt(const TopoDS_Shape& body, gp_Pnt c) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(map(i), p);
    if (p.CentreOfMass().Distance(c) < 1e-6) return i;
  }
  return 0;
}

static void shellOpenBox() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  auto shell = std::make_unique<ShellFeature>();
  shell->faces = {faceAt(doc.body(), gp_Pnt(5, 5, 10))}; // open the top
  shell->thickness = 1;
  CHECK(shell->faces[0] > 0);
  const int s = doc.add(std::move(shell));
  CHECK(doc.error(s).empty());
  CHECK(near(volume(doc.body()), 1000 - 8 * 8 * 9));
}

static void patterns() {
  // 30 x 10 x 2 plate, one 2 x 2 hole cut through, then patterned.
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 30, 10)), 2));
  const int hole = doc.add(extrude(doc.add(rectangle(2, 4, 4, 6)), 2, Operation::Cut));
  auto lin = std::make_unique<PatternFeature>();
  lin->source = hole;
  lin->count = 3;
  lin->spacing = 10;
  const int p = doc.add(std::move(lin));
  CHECK(doc.error(p).empty());
  CHECK(near(volume(doc.body()), 600 - 3 * 8));

  // Four bosses around Z.
  Document ring;
  const int boss = ring.add(extrude(ring.add(rectangle(10, -1, 12, 1)), 5));
  auto circ = std::make_unique<PatternFeature>();
  circ->source = boss;
  circ->kind = PatternFeature::Kind::Circular;
  circ->axis = PatternFeature::Axis::Z;
  circ->count = 4;
  const int c = ring.add(std::move(circ));
  CHECK(ring.error(c).empty());
  CHECK(near(volume(ring.body()), 4 * 20));

  // Deleting the patterned feature deletes the pattern too.
  CHECK(doc.removeWithDependents(hole) == 2);
}

static void constructionIsNotSolid() {
  Document doc;
  auto sk = rectangle(0, 0, 10, 10);
  const int c = sk->sketch.addCircle(sk->sketch.addPoint(5, 5), 3);
  sk->sketch.entities[c].construction = true; // a helper circle, not a hole
  doc.add(extrude(doc.add(std::move(sk)), 1));
  CHECK(near(volume(doc.body()), 100));
}

static void propertiesAndMeasure() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 20)), 5));
  const MassProperties m = massProperties(doc.body());
  CHECK(near(m.volume, 1000));
  CHECK(near(m.area, 2 * (200 + 50 + 100)));
  CHECK(near(m.center.X(), 5) && near(m.center.Y(), 10) && near(m.center.Z(), 2.5));
  CHECK(std::abs(m.size.X() - 10) < 1e-3 && std::abs(m.size.Z() - 5) < 1e-3);

  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(doc.body(), TopAbs_FACE, faces);
  const std::string text =
      measure({faces(faceAt(doc.body(), gp_Pnt(5, 10, 0))), faces(faceAt(doc.body(), gp_Pnt(5, 10, 5)))});
  CHECK(text.find("Distance: 5 mm") != std::string::npos);
  CHECK(text.find("Angle: 0") != std::string::npos);
  const std::string one = measure({faces(faceAt(doc.body(), gp_Pnt(5, 10, 5)))});
  CHECK(one.find("Face area: 200") != std::string::npos);
}

static bool roughly(double a, double b) { return std::abs(a - b) < 1e-3 * std::max(1.0, std::abs(b)); }

static void expressions() {
  CHECK(near(evaluate("2 + 3 * 4"), 14));
  CHECK(near(evaluate("-(2^3) + 10 / 4"), -5.5));
  CHECK(near(evaluate("sqrt(16) + max(1, 2) + sin(30)"), 6.5));
  CHECK(near(evaluate("w / 2 + 5", {{"w", 50}}), 30));
  CHECK(near(evaluate("w * 0.75 + .5", {{"w", 40}}), 30.5));
  CHECK(near(evaluate("2,5 * 2"), 5) && near(evaluate("max(1, 2.5)"), 2.5) && isPlainNumber("2,5"));
  CHECK(isPlainNumber("12.5") && isPlainNumber(" -3 ") && !isPlainNumber("w") && !isPlainNumber("1e3"));
  CHECK(isValidName("wall_2") && !isValidName("2x") && !isValidName("sqrt") && !isValidName("pi"));
  bool threw = false;
  try { evaluate("2 +"); } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { evaluate("nope * 2"); } catch (const UnknownName& e) { threw = e.name == "nope"; }
  CHECK(threw);
  // Any order, but no cycles.
  const Variables v = evaluateParameters({{"b", "a * 2"}, {"a", "3"}});
  CHECK(near(v.at("b"), 6));
  threw = false;
  try { evaluateParameters({{"a", "b"}, {"b", "a + 1"}}); } catch (const std::runtime_error&) { threw = true; }
  CHECK(threw);
  CHECK((namesIn("w/2 + sqrt(h)") == std::vector<std::string>{"w", "h"}));
}

// A fully constrained rectangle whose width and height come from parameters,
// extruded by another one.
static void parametersDriveSizes() {
  Document doc;
  doc.setParameters({{"w", "20"}, {"h", "w / 2"}});
  auto sk = rectangle(0, 0, 10, 10);
  sketch::Sketch& s = sk->sketch;
  s.points[0].fixed = true;
  s.addConstraint({sketch::ConstraintKind::Horizontal, 0});
  s.addConstraint({sketch::ConstraintKind::Vertical, 1});
  s.addConstraint({sketch::ConstraintKind::Horizontal, 2});
  s.addConstraint({sketch::ConstraintKind::Vertical, 3});
  s.addConstraint({sketch::ConstraintKind::Length, 0, -1, 10, "w"});
  s.addConstraint({sketch::ConstraintKind::Length, 1, -1, 10, "h"});
  const int si = doc.add(std::move(sk));
  auto e = extrude(si, 1);
  e->expressions["distance"] = "h";
  const int ei = doc.add(std::move(e));
  CHECK(doc.error(ei).empty());
  CHECK(near(volume(doc.body()), 20 * 10 * 10));

  doc.setParameters({{"w", "30"}, {"h", "w / 2"}});
  CHECK(!doc.hasErrors());
  CHECK(near(volume(doc.body()), 30 * 15 * 15));

  doc.setParameters({{"w", "30"}}); // h removed: both steps that use it fail
  CHECK(!doc.error(si).empty());
  CHECK(!doc.error(ei).empty());
  doc.setParameters({{"w", "30"}, {"h", "5"}});
  CHECK(!doc.hasErrors());
  CHECK(near(volume(doc.body()), 30 * 5 * 5));
}

static TopoDS_Shape edgeShape(const TopoDS_Shape& body, int index) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, TopAbs_EDGE, map);
  return map(index);
}

// The fillet keeps rounding the same corner when an earlier step adds edges
// (renumbering them) or makes the box bigger.
static void keysSurviveRenumbering() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  auto fillet = std::make_unique<FilletFeature>();
  fillet->edges = {verticalEdgeAt(doc.body(), 10, 10)};
  fillet->keys = {shapeKey(doc.body(), edgeShape(doc.body(), fillet->edges[0]))};
  fillet->radius = 1;
  const int f = doc.add(std::move(fillet));
  CHECK(doc.error(f).empty());
  const double corner = (1 - std::numbers::pi / 4) * 10;

  auto withHole = rectangle(0, 0, 10, 10);
  withHole->sketch.addCircle(withHole->sketch.addPoint(3, 3), 2);
  doc.replace(0, std::move(withHole));
  CHECK(doc.error(f).empty());
  CHECK(near(volume(doc.body()), 1000 - std::numbers::pi * 4 * 10 - corner));

  doc.replace(0, rectangle(0, 0, 20, 20));
  CHECK(doc.error(f).empty());
  CHECK(near(volume(doc.body()), 4000 - corner));
}

static std::unique_ptr<SketchFeature> circlesOnTop(double z, std::vector<std::pair<double, double>> centres) {
  auto f = std::make_unique<SketchFeature>();
  f->plane = gp_Ax3(gp_Pnt(0, 0, z), gp::DZ(), gp::DX());
  for (const auto& [x, y] : centres) f->sketch.addCircle(f->sketch.addPoint(x, y), 1);
  return f;
}

static void holes() {
  const double pi = std::numbers::pi;
  auto plate = [] {
    Document doc;
    doc.add(extrude(doc.add(rectangle(0, 0, 40, 40)), 10));
    return doc;
  };
  {
    Document doc = plate();
    auto h = std::make_unique<HoleFeature>();
    h->sketch = doc.add(circlesOnTop(10, {{10, 10}, {30, 30}}));
    h->diameter = 4; // through all
    const int i = doc.add(std::move(h));
    CHECK(doc.error(i).empty());
    CHECK(near(volume(doc.body()), 16000 - 2 * pi * 4 * 10));
  }
  {
    Document doc = plate();
    auto h = std::make_unique<HoleFeature>();
    h->sketch = doc.add(circlesOnTop(10, {{20, 20}}));
    h->kind = HoleFeature::Kind::Counterbore;
    h->diameter = 4;
    h->depth = 6;
    h->boreDiameter = 8;
    h->boreDepth = 2;
    const int i = doc.add(std::move(h));
    CHECK(doc.error(i).empty());
    CHECK(near(volume(doc.body()), 16000 - pi * 4 * 6 - pi * (16 - 4) * 2));
  }
  {
    Document doc = plate();
    auto h = std::make_unique<HoleFeature>();
    h->sketch = doc.add(circlesOnTop(10, {{20, 20}}));
    h->kind = HoleFeature::Kind::Countersink;
    h->diameter = 4;
    h->sinkDiameter = 8; // 90°: cone 2 deep
    const int i = doc.add(std::move(h));
    CHECK(doc.error(i).empty());
    const double cone = pi * 2 / 3 * (16 + 8 + 4) - pi * 4 * 2; // frustum minus the hole inside it
    CHECK(roughly(volume(doc.body()), 16000 - pi * 4 * 10 - cone));

    auto p = std::make_unique<PatternFeature>(); // the hole, three times along X
    p->source = i;
    p->count = 3;
    p->spacing = 8;
    const int pi2 = doc.add(std::move(p));
    CHECK(doc.error(pi2).empty());
    CHECK(roughly(volume(doc.body()), 16000 - 3 * (pi * 4 * 10 + cone)));
  }
  {
    Document doc = plate();
    auto h = std::make_unique<HoleFeature>();
    h->sketch = doc.add(circlesOnTop(10, {}));
    const int i = doc.add(std::move(h));
    CHECK(!doc.error(i).empty()); // no circles, no holes
  }
}

static void mirror() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  auto m = std::make_unique<MirrorFeature>();
  m->plane = MirrorFeature::Plane::YZ;
  const int i = doc.add(std::move(m));
  CHECK(doc.error(i).empty());
  CHECK(near(volume(doc.body()), 2000));
  CHECK(near(massProperties(doc.body()).size.X(), 20));

  Document doc2;
  doc2.add(extrude(doc2.add(rectangle(0, 0, 10, 10)), 10));
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(doc2.body(), TopAbs_FACE, faces);
  auto onFace = std::make_unique<MirrorFeature>();
  onFace->plane = MirrorFeature::Plane::Face;
  onFace->face = faceAt(doc2.body(), gp_Pnt(5, 5, 10)); // the top
  onFace->key = shapeKey(doc2.body(), faces(onFace->face));
  const int j = doc2.add(std::move(onFace));
  CHECK(doc2.error(j).empty());
  CHECK(near(volume(doc2.body()), 2000));
  CHECK(near(massProperties(doc2.body()).size.Z(), 20));
}

// Offset of a closed square: a bigger square with sharp corners.
static void offsetLoop() {
  sketch::Sketch s;
  const int a = s.addPoint(0, 0), b = s.addPoint(10, 0), c = s.addPoint(10, 10), d = s.addPoint(0, 10);
  const std::vector<int> loop{s.addLine(a, b), s.addLine(b, c), s.addLine(c, d), s.addLine(d, a)};
  const std::vector<int> out = sketch::offsetLoops(s, loop, 2);
  CHECK(out.size() == 4);
  double minX = 1e9, maxX = -1e9;
  for (int e : out)
    for (int k : {0, 1}) {
      minX = std::min(minX, s.x(s.entities[e].p[k]));
      maxX = std::max(maxX, s.x(s.entities[e].p[k]));
    }
  CHECK(near(minX, -2) && near(maxX, 12));
  CHECK(s.usedPoints().size() == 8); // corners shared, not duplicated
  CHECK(s.solve() == sketch::SolveResult::Ok);
}

static std::unique_ptr<SketchFeature> square(double z, double half) {
  auto f = rectangle(-half, -half, half, half);
  f->plane = gp_Ax3(gp_Pnt(0, 0, z), gp::DZ(), gp::DX());
  return f;
}

static void splines() {
  // A closed spline through 8 points of a circle is very nearly that circle.
  Document doc;
  auto round = std::make_unique<SketchFeature>();
  round->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  std::vector<int> pts;
  for (int i = 0; i < 8; ++i)
    pts.push_back(round->sketch.addPoint(10 * std::cos(i * std::numbers::pi / 4), 10 * std::sin(i * std::numbers::pi / 4)));
  pts.push_back(pts.front());
  round->sketch.addSpline(pts);
  CHECK(round->sketch.valid());
  CHECK(round->sketch.dof() == 16); // just its points
  const int s = doc.add(std::move(round));
  const int e = doc.add(extrude(s, 5));
  CHECK(doc.error(e).empty());
  CHECK(std::abs(volume(doc.body()) - std::numbers::pi * 100 * 5) < 0.01 * std::numbers::pi * 100 * 5);

  // Smooth sides take a fillet all round the top.
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(doc.body(), TopAbs_EDGE, edges);
  auto fillet = std::make_unique<FilletFeature>();
  for (int i = 1; i <= edges.Extent(); ++i) {
    GProp_GProps p;
    BRepGProp::LinearProperties(edges(i), p);
    if (std::abs(p.CentreOfMass().Z() - 5) < 1e-6 && p.Mass() > 1) fillet->edges.push_back(i);
  }
  CHECK(fillet->edges.size() == 1);
  fillet->radius = 1;
  const int f = doc.add(std::move(fillet));
  CHECK(doc.error(f).empty());
  CHECK(volume(doc.body()) < std::numbers::pi * 100 * 5);

  // An open spline closed off by lines; its ends are ordinary shared points.
  Document doc2;
  auto d = std::make_unique<SketchFeature>();
  d->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& k = d->sketch;
  const int a = k.addPoint(0, 0), b = k.addPoint(20, 0), c = k.addPoint(20, 20), q = k.addPoint(0, 20);
  k.addLine(a, b);
  k.addSpline({b, k.addPoint(26, 10), c});
  k.addLine(c, q);
  k.addLine(q, a);
  const int s2 = doc2.add(std::move(d));
  doc2.add(extrude(s2, 1));
  CHECK(!doc2.hasErrors());
  CHECK(volume(doc2.body()) > 450 && volume(doc2.body()) < 560);
}

static void lofts() {
  Document doc;
  const int a = doc.add(square(0, 10)), b = doc.add(square(10, 5));
  auto loft = std::make_unique<LoftFeature>();
  loft->sketches = {a, b};
  loft->ruled = true;
  const int l = doc.add(std::move(loft));
  CHECK(doc.error(l).empty());
  CHECK(std::abs(volume(doc.body()) - 10.0 / 3 * (400 + 100 + 200)) < 1e-3); // frustum
  CHECK(doc.isConsumed(a) && doc.isConsumed(b));

  // Smooth through three sections, square -> circle -> square.
  Document doc2;
  const int s0 = doc2.add(square(0, 10));
  auto circle = std::make_unique<SketchFeature>();
  circle->plane = gp_Ax3(gp_Pnt(0, 0, 15), gp::DZ(), gp::DX());
  circle->sketch.addCircle(circle->sketch.addPoint(0, 0), 6);
  const int s1 = doc2.add(std::move(circle));
  const int s2 = doc2.add(square(30, 8));
  auto smooth = std::make_unique<LoftFeature>();
  smooth->sketches = {s0, s1, s2};
  const int l2 = doc2.add(std::move(smooth));
  CHECK(doc2.error(l2).empty());
  CHECK(volume(doc2.body()) > 30 * 100 && volume(doc2.body()) < 30 * 400);

  // Deleting a section deletes the loft; deleting something before shifts it.
  Document doc3;
  doc3.add(rectangle(50, 0, 60, 10));
  const int x = doc3.add(square(0, 10)), y = doc3.add(square(10, 5));
  auto l3 = std::make_unique<LoftFeature>();
  l3->sketches = {x, y};
  doc3.add(std::move(l3));
  Document doc4 = doc3;
  CHECK(doc3.removeWithDependents(0) == 1);
  CHECK(!doc3.hasErrors());
  const auto* moved = doc3.as<LoftFeature>(2);
  CHECK(moved && moved->sketches == std::vector<int>({0, 1}));
  CHECK(doc4.removeWithDependents(2) == 2);
  CHECK(doc4.size() == 2);

  // Sections must be single loops.
  Document doc5;
  auto ring = rectangle(-10, -10, 10, 10);
  ring->sketch.addCircle(ring->sketch.addPoint(0, 0), 3);
  const int r0 = doc5.add(std::move(ring)), r1 = doc5.add(square(10, 5));
  auto bad = std::make_unique<LoftFeature>();
  bad->sketches = {r0, r1};
  const int lb = doc5.add(std::move(bad));
  CHECK(!doc5.error(lb).empty());
}

static void symmetricExtrude() {
  Document doc;
  auto e = extrude(doc.add(rectangle(0, 0, 10, 10)), 10);
  e->symmetric = true;
  doc.add(std::move(e));
  CHECK(!doc.hasErrors());
  CHECK(near(volume(doc.body()), 1000));
  const MassProperties m = massProperties(doc.body());
  CHECK(std::abs(m.center.Z()) < 1e-6 && std::abs(m.size.Z() - 10) < 1e-6);
}

static void filletErrorsNameTheEdge() {
  Document doc;
  doc.add(extrude(doc.add(rectangle(0, 0, 10, 10)), 10));
  auto fillet = std::make_unique<FilletFeature>();
  const int edge = verticalEdgeAt(doc.body(), 10, 10);
  fillet->edges = {edge};
  fillet->radius = 20;
  const int f = doc.add(std::move(fillet));
  CHECK(!doc.error(f).empty());
  CHECK(doc.error(f).find("Fillet failed") != std::string::npos);
  CHECK(near(volume(doc.body()), 1000)); // left as it was
}

int main() {
  std::setlocale(LC_ALL, "tr_TR.UTF-8"); // decimal comma: formulas must still read "0.75"
  offsetLoop();
  expressions();
  parametersDriveSizes();
  keysSurviveRenumbering();
  holes();
  mirror();
  extrudeBox();
  holeFromInnerLoop();
  islandInsideHoleIsSolid();
  cutAndRegenerate();
  revolveRing();
  errorsAreReported();
  filletAndChamfer();
  removeWithDependentsShiftsReferences();
  copyIsIndependent();
  exportImportRoundTrip();
  shellOpenBox();
  patterns();
  constructionIsNotSolid();
  propertiesAndMeasure();
  splines();
  lofts();
  symmetricExtrude();
  filletErrorsNameTheEdge();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all model tests passed\n");
  return EXIT_SUCCESS;
}
