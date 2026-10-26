// Feature tests: build real solids and check their volumes.
#include "io/Exchange.h"
#include "model/Document.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <filesystem>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

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
  for (const char* name : {"rt.stl", "rt.obj"}) {
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

int main() {
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
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all model tests passed\n");
  return EXIT_SUCCESS;
}
