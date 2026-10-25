// Dimensions in the view: which ones appear, and editing them rebuilds the part.
#include "app/Dimensions.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

using namespace model;
using K = sketch::ConstraintKind;

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

// 40 x 20 plate (bottom edge dimensioned), hole of radius 5, extruded 10.
static Document plate() {
  auto f = std::make_unique<SketchFeature>();
  f->plane = gp_Ax3(gp::Origin(), gp::DZ(), gp::DX());
  sketch::Sketch& s = f->sketch;
  const int a = s.addPoint(0, 0), b = s.addPoint(40, 0), c = s.addPoint(40, 20), d = s.addPoint(0, 20);
  const int bottom = s.addLine(a, b);
  s.addLine(b, c);
  s.addLine(c, d);
  s.addLine(d, a);
  s.addCircle(s.addPoint(20, 10), 5);
  s.addConstraint({K::Length, bottom, -1, 40});
  Document doc;
  const int sk = doc.add(std::move(f));
  auto ex = std::make_unique<ExtrudeFeature>();
  ex->sketch = sk;
  ex->distance = 10;
  doc.add(std::move(ex));
  return doc;
}

static const DimensionRef* find(const DimensionList& dims, DimensionRef::Kind kind) {
  for (const auto& [dim, ref] : dims)
    if (ref.kind == kind) return &ref;
  return nullptr;
}

int main() {
  Document doc = plate();
  const double pi = std::numbers::pi;
  CHECK(near(volume(doc.body()), (800 - pi * 25) * 10));

  const DimensionList dims = buildDimensions(doc);
  CHECK(dims.size() == 3); // hole diameter, bottom length, extrude depth
  const DimensionRef* hole = find(dims, DimensionRef::Kind::SketchCurve);
  const DimensionRef* depth = find(dims, DimensionRef::Kind::ExtrudeDepth);
  const DimensionRef* length = find(dims, DimensionRef::Kind::SketchConstraint);
  CHECK(hole && hole->diameter && near(hole->value, 10));
  CHECK(depth && near(depth->value, 10));
  CHECK(length && near(length->value, 40));

  // Hole 10 -> 4 mm: the sketch gets a radius constraint and the solid updates.
  std::string error;
  auto changed = applyDimension(doc, *hole, 4, error);
  CHECK(changed && error.empty());
  doc.replace(hole->feature, std::move(changed));
  CHECK(!doc.hasErrors());
  CHECK(near(volume(doc.body()), (800 - pi * 4) * 10));

  // Depth 10 -> 25 mm.
  changed = applyDimension(doc, *find(buildDimensions(doc), DimensionRef::Kind::ExtrudeDepth), 25, error);
  CHECK(changed);
  doc.replace(1, std::move(changed));
  CHECK(near(volume(doc.body()), (800 - pi * 4) * 25));

  // A size that contradicts the sketch is refused and nothing changes.
  Document fixed = plate();
  auto sk = std::make_unique<SketchFeature>(*fixed.as<SketchFeature>(0));
  sk->sketch.addConstraint({K::Distance, 0, 1, 40}); // the same edge, fixed a second way
  fixed.replace(0, std::move(sk));
  const DimensionRef* len = find(buildDimensions(fixed), DimensionRef::Kind::SketchConstraint);
  CHECK(len);
  CHECK(!applyDimension(fixed, *len, 30, error) && !error.empty());

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all dimension tests passed\n");
  return EXIT_SUCCESS;
}
