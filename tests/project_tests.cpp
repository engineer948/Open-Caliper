// Project file round trip: save, load, rebuild, same solid.
#include "app/ProjectFile.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <GProp_GProps.hxx>

#include <QDir>
#include <QFile>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

// 0.4: parameters, holes, mirrors and fillets that find their edges by key.
static void newFeatures() {
  Document doc;
  doc.setParameters({{"t", "4"}, {"d", "t - 1"}});
  auto sk = std::make_unique<SketchFeature>();
  sk->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& s = sk->sketch;
  const int a = s.addPoint(0, 0), b = s.addPoint(20, 0), c = s.addPoint(20, 20), d = s.addPoint(0, 20);
  s.addLine(a, b);
  s.addLine(b, c);
  s.addLine(c, d);
  s.addLine(d, a);
  doc.add(std::move(sk));
  auto ex = std::make_unique<ExtrudeFeature>();
  ex->sketch = 0;
  ex->expressions["distance"] = "t * 2";
  doc.add(std::move(ex));

  auto top = std::make_unique<SketchFeature>();
  top->plane = gp_Ax3(gp_Pnt(0, 0, 8), gp::DZ(), gp::DX());
  top->sketch.addCircle(top->sketch.addPoint(10, 10), 1);
  doc.add(std::move(top));
  auto hole = std::make_unique<HoleFeature>();
  hole->sketch = 2;
  hole->kind = HoleFeature::Kind::Counterbore;
  hole->expressions["diameter"] = "d";
  doc.add(std::move(hole));

  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(doc.body(), TopAbs_EDGE, edges);
  auto fillet = std::make_unique<FilletFeature>();
  fillet->edges = {1};
  fillet->keys = {shapeKey(doc.body(), edges(1))};
  fillet->radius = 0.5;
  doc.add(std::move(fillet));
  auto mirror = std::make_unique<MirrorFeature>();
  mirror->plane = MirrorFeature::Plane::XZ;
  doc.add(std::move(mirror));
  CHECK(!doc.hasErrors());

  const QString path = QDir::temp().filePath("roundtrip04.ocal");
  project::save(doc, path);
  const Document loaded = project::load(path);
  CHECK(loaded.size() == doc.size());
  CHECK(!loaded.hasErrors());
  CHECK(std::abs(volume(loaded.body()) - volume(doc.body())) < 1e-6);
  CHECK(loaded.parameters().size() == 2 && loaded.values().at("d") == 3);
  const auto* lh = loaded.as<HoleFeature>(3);
  CHECK(lh && lh->kind == HoleFeature::Kind::Counterbore && lh->diameter == 3 && lh->expressions.at("diameter") == "d");
  const auto* lf = loaded.as<FilletFeature>(4);
  CHECK(lf && lf->keys.size() == 1 && lf->keys[0].valid() && lf->keys[0].normals.size() == 2);
  const auto* lm = loaded.as<MirrorFeature>(5);
  CHECK(lm && lm->plane == MirrorFeature::Plane::XZ);
}

static void splinesAndLofts() {
  Document doc;
  auto bottom = std::make_unique<SketchFeature>();
  bottom->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& s = bottom->sketch;
  const int p = s.addPoint(10, 0);
  s.addSpline({p, s.addPoint(0, 10), s.addPoint(-10, 0), s.addPoint(0, -10), p});
  doc.add(std::move(bottom));
  auto top = std::make_unique<SketchFeature>();
  top->plane = gp_Ax3(gp_Pnt(0, 0, 20), gp::DZ(), gp::DX());
  top->sketch.addCircle(top->sketch.addPoint(0, 0), 4);
  doc.add(std::move(top));
  auto loft = std::make_unique<LoftFeature>();
  loft->sketches = {0, 1};
  loft->ruled = true;
  doc.add(std::move(loft));
  auto rim = std::make_unique<SketchFeature>();
  rim->plane = gp_Ax3(gp::Origin(), gp::DY(), gp::DX());
  const int a = rim->sketch.addPoint(-2, -2), b = rim->sketch.addPoint(2, -2), c = rim->sketch.addPoint(2, 2),
            d = rim->sketch.addPoint(-2, 2);
  rim->sketch.addLine(a, b);
  rim->sketch.addLine(b, c);
  rim->sketch.addLine(c, d);
  rim->sketch.addLine(d, a);
  doc.add(std::move(rim));
  auto ex = std::make_unique<ExtrudeFeature>();
  ex->sketch = 3;
  ex->distance = 30;
  ex->symmetric = true;
  ex->op = Operation::Cut;
  doc.add(std::move(ex));
  CHECK(!doc.hasErrors());

  const QString path = QDir::temp().filePath("roundtrip06.ocal");
  project::save(doc, path);
  const Document loaded = project::load(path);
  QFile::remove(path);
  CHECK(!loaded.hasErrors());
  CHECK(std::abs(volume(loaded.body()) - volume(doc.body())) < 1e-6);
  const auto* ls = loaded.as<SketchFeature>(0);
  CHECK(ls && ls->sketch.entities[0].kind == sketch::EntityKind::Spline && ls->sketch.entities[0].closedSpline());
  const auto* ll = loaded.as<LoftFeature>(2);
  CHECK(ll && ll->ruled && ll->sketches == std::vector<int>({0, 1}));
  const auto* le = loaded.as<ExtrudeFeature>(4);
  CHECK(le && le->symmetric && le->op == Operation::Cut);
}

int main() {
  newFeatures();
  splinesAndLofts();
  Document doc;
  doc.setParameters({{"len", "10"}});
  auto sk = std::make_unique<SketchFeature>();
  sk->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& s = sk->sketch;
  const int a = s.addPoint(0, 0), b = s.addPoint(10, 0), c = s.addPoint(10, 10), d = s.addPoint(0, 10);
  const int l0 = s.addLine(a, b);
  s.addLine(b, c);
  s.addLine(c, d);
  s.addLine(d, a);
  s.addConstraint({sketch::ConstraintKind::Horizontal, l0});
  s.entities[s.addCircle(s.addPoint(5, 5), 2)].construction = true;
  s.addConstraint({sketch::ConstraintKind::Length, l0, -1, 10, "len"});
  s.points[a].fixed = true;
  doc.add(std::move(sk));

  auto ex = std::make_unique<ExtrudeFeature>();
  ex->sketch = 0;
  ex->distance = 5;
  doc.add(std::move(ex));

  auto im = std::make_unique<ImportFeature>();
  im->shape = BRepPrimAPI_MakeBox(gp_Pnt(20, 0, 0), 2, 2, 2).Shape();
  im->source = "box.step";
  doc.add(std::move(im));

  auto rev = std::make_unique<RevolveFeature>();
  rev->sketch = 0;
  rev->axis = RevolveFeature::Axis::SketchX;
  rev->angle = 90;
  rev->op = Operation::Cut;
  doc.add(std::move(rev));

  auto pat = std::make_unique<PatternFeature>();
  pat->source = 1;
  pat->kind = PatternFeature::Kind::Circular;
  pat->axis = PatternFeature::Axis::Z;
  pat->count = 3;
  pat->angle = 90;
  doc.add(std::move(pat));
  CHECK(!doc.hasErrors());

  const QString path = QDir::temp().filePath("roundtrip.ocal");
  project::save(doc, path);
  const Document loaded = project::load(path);
  CHECK(loaded.size() == doc.size());
  CHECK(!loaded.hasErrors());
  CHECK(std::abs(volume(loaded.body()) - volume(doc.body())) < 1e-6);
  const auto* ls = loaded.as<SketchFeature>(0);
  CHECK(ls && ls->sketch.constraints.size() == 2 && ls->sketch.points[a].fixed);
  CHECK(ls && ls->sketch.constraints[1].expr == "len");
  const auto* lr = loaded.as<RevolveFeature>(3);
  CHECK(lr && lr->axis == RevolveFeature::Axis::SketchX && lr->op == Operation::Cut);
  const auto* lp = loaded.as<PatternFeature>(4);
  CHECK(lp && lp->kind == PatternFeature::Kind::Circular && lp->axis == PatternFeature::Axis::Z && lp->count == 3);
  CHECK(ls && ls->sketch.entities.back().construction);

  // A damaged file must be rejected, not crash.
  QFile f(path);
  CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(R"({"format":"opencaliper-project","version":1,"features":[{"type":"sketch","plane":{"origin":[0,0,0],"normal":[0,0,1],"xdir":[1,0,0]},"params":[0,0],"points":[{"x":0,"y":1}],"entities":[{"kind":"line","p":[0,7,-1],"r":-1}],"constraints":[]}]})");
  f.close();
  bool rejected = false;
  try {
    project::load(path);
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all project tests passed\n");
  return EXIT_SUCCESS;
}
