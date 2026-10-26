// Solver tests. Run: ctest --test-dir build  (or ./build/sketch_tests)
#include "sketch/Sketch.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace sketch;
using K = ConstraintKind;

static int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

static void freeEntitiesDof() {
  Sketch s;
  s.addLine(s.addPoint(0, 0), s.addPoint(10, 0));
  CHECK(s.dof() == 4);
  s.addCircle(s.addPoint(5, 5), 3);
  CHECK(s.dof() == 7);
}

static void rectangle() {
  Sketch s;
  const int p0 = s.addPoint(0, 0), p1 = s.addPoint(9, 1), p2 = s.addPoint(11, 7), p3 = s.addPoint(-1, 6);
  const int l0 = s.addLine(p0, p1), l1 = s.addLine(p1, p2), l2 = s.addLine(p2, p3), l3 = s.addLine(p3, p0);
  s.points[p0].fixed = true;
  s.addConstraint({K::Horizontal, l0});
  s.addConstraint({K::Horizontal, l2});
  s.addConstraint({K::Vertical, l1});
  s.addConstraint({K::Vertical, l3});
  s.addConstraint({K::Length, l0, -1, 40});
  s.addConstraint({K::Length, l1, -1, 20});
  CHECK(s.solve() == SolveResult::Ok);
  CHECK(near(s.x(p2), 40) && near(s.y(p2), 20));
  CHECK(near(s.x(p3), 0) && near(s.y(p3), 20));
  CHECK(s.dof() == 0);
}

static void tangentLine() {
  Sketch s;
  const int c = s.addPoint(0, 0);
  s.points[c].fixed = true;
  const int circle = s.addCircle(c, 10);
  const int line = s.addLine(s.addPoint(-20, 13), s.addPoint(20, 12));
  s.addConstraint({K::Horizontal, line});
  s.addConstraint({K::Radius, circle, -1, 10});
  s.addConstraint({K::Tangent, line, circle});
  CHECK(s.solve() == SolveResult::Ok);
  CHECK(near(std::abs(s.y(s.entities[line].p[0])), 10));
}

static void conflictLeavesSketchUnchanged() {
  Sketch s;
  const int a = s.addPoint(0, 0), b = s.addPoint(10, 0);
  const int line = s.addLine(a, b);
  s.addConstraint({K::Length, line, -1, 10});
  s.addConstraint({K::Distance, a, b, 20});
  const auto before = s.params;
  CHECK(s.solve() == SolveResult::Failed);
  CHECK(s.params == before);
}

static void arcEndsStayOnCircle() {
  Sketch s;
  const int c = s.addPoint(0, 0), st = s.addPoint(10, 0), en = s.addPoint(0, 7);
  const int arc = s.addArc(c, st, en);
  s.addConstraint({K::Radius, arc, -1, 5});
  CHECK(s.solve() == SolveResult::Ok);
  CHECK(near(s.distance(c, st), 5) && near(s.distance(c, en), 5));
}

static void parallelAndPerpendicular() {
  Sketch s;
  const int l0 = s.addLine(s.addPoint(0, 0), s.addPoint(10, 1));
  const int l1 = s.addLine(s.addPoint(0, 5), s.addPoint(9, 7));
  const int l2 = s.addLine(s.addPoint(20, 0), s.addPoint(21, 9));
  s.addConstraint({K::Parallel, l0, l1});
  s.addConstraint({K::Perpendicular, l0, l2});
  CHECK(s.solve() == SolveResult::Ok);
  auto dir = [&](int l, double& dx, double& dy) {
    dx = s.x(s.entities[l].p[1]) - s.x(s.entities[l].p[0]);
    dy = s.y(s.entities[l].p[1]) - s.y(s.entities[l].p[0]);
  };
  double ax, ay, bx, by, cx, cy;
  dir(l0, ax, ay);
  dir(l1, bx, by);
  dir(l2, cx, cy);
  // sin / cos of the angle between the lines
  CHECK(std::abs(ax * by - ay * bx) / (std::hypot(ax, ay) * std::hypot(bx, by)) < 1e-6);
  CHECK(std::abs(ax * cx + ay * cy) / (std::hypot(ax, ay) * std::hypot(cx, cy)) < 1e-6);
}

static void removeEntityReindexesConstraints() {
  Sketch s;
  const int l0 = s.addLine(s.addPoint(0, 0), s.addPoint(1, 0));
  const int l1 = s.addLine(s.addPoint(0, 1), s.addPoint(1, 2));
  const int l2 = s.addLine(s.addPoint(0, 3), s.addPoint(1, 5));
  s.addConstraint({K::Horizontal, l0});
  s.addConstraint({K::Parallel, l1, l2});
  s.addConstraint({K::Vertical, l2});
  s.removeEntity(l1);
  CHECK(s.entities.size() == 2);
  CHECK(s.constraints.size() == 2);
  CHECK(s.constraints[1].kind == K::Vertical && s.constraints[1].a == 1);
}

static void deletedEntityFreesItsDof() {
  Sketch s;
  s.addLine(s.addPoint(0, 0), s.addPoint(10, 0));
  const int circle = s.addCircle(s.addPoint(5, 5), 3);
  CHECK(s.dof() == 7);
  s.removeEntity(circle);
  CHECK(s.dof() == 4);
}

// A slot: two arcs joined to two lines at their ends. Only the two centres
// and the radius may stay free, and dragging must keep the ends tangent.
static void slotIsTangentAtJoints() {
  Sketch s;
  const double r = 3;
  const int c1 = s.addPoint(0, 0), c2 = s.addPoint(10, 0);
  const int a1 = s.addPoint(0, r), b1 = s.addPoint(0, -r), a2 = s.addPoint(10, r), b2 = s.addPoint(10, -r);
  const int top = s.addLine(a1, a2), bottom = s.addLine(b1, b2);
  const int end2 = s.addArc(c2, b2, a2), end1 = s.addArc(c1, a1, b1);
  for (int line : {top, bottom})
    for (int arc : {end1, end2}) s.addConstraint({K::Tangent, line, arc});
  s.addConstraint({K::Equal, end1, end2});
  CHECK(s.dof() == 5);

  s.points[c2].fixed = true;
  s.setPoint(c2, 10, 6); // tilt the slot
  CHECK(s.solve() == SolveResult::Ok);
  const double dx = s.x(a2) - s.x(a1), dy = s.y(a2) - s.y(a1);
  const double rx = s.x(a1) - s.x(c1), ry = s.y(a1) - s.y(c1);
  CHECK(std::abs(dx * rx + dy * ry) / (std::hypot(dx, dy) * std::hypot(rx, ry)) < 1e-6);
}

int main() {
  freeEntitiesDof();
  rectangle();
  tangentLine();
  conflictLeavesSketchUnchanged();
  arcEndsStayOnCircle();
  parallelAndPerpendicular();
  removeEntityReindexesConstraints();
  deletedEntityFreesItsDof();
  slotIsTangentAtJoints();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all sketch tests passed\n");
  return EXIT_SUCCESS;
}
