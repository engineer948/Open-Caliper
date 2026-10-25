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
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all model tests passed\n");
  return EXIT_SUCCESS;
}
