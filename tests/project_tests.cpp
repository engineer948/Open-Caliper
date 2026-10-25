// Project file round trip: save, load, rebuild, same solid.
#include "app/ProjectFile.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
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

int main() {
  Document doc;
  auto sk = std::make_unique<SketchFeature>();
  sk->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& s = sk->sketch;
  const int a = s.addPoint(0, 0), b = s.addPoint(10, 0), c = s.addPoint(10, 10), d = s.addPoint(0, 10);
  const int l0 = s.addLine(a, b);
  s.addLine(b, c);
  s.addLine(c, d);
  s.addLine(d, a);
  s.addConstraint({sketch::ConstraintKind::Horizontal, l0});
  s.addConstraint({sketch::ConstraintKind::Length, l0, -1, 10});
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
  CHECK(!doc.hasErrors());

  const QString path = QDir::temp().filePath("roundtrip.ocal");
  project::save(doc, path);
  const Document loaded = project::load(path);
  CHECK(loaded.size() == doc.size());
  CHECK(!loaded.hasErrors());
  CHECK(std::abs(volume(loaded.body()) - volume(doc.body())) < 1e-6);
  const auto* ls = loaded.as<SketchFeature>(0);
  CHECK(ls && ls->sketch.constraints.size() == 2 && ls->sketch.points[a].fixed);
  const auto* lr = loaded.as<RevolveFeature>(3);
  CHECK(lr && lr->axis == RevolveFeature::Axis::SketchX && lr->op == Operation::Cut);

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
