// Solver tests. Run: ctest --test-dir build  (or ./build/sketch_tests)
#include "sketch/Sketch.h"
#include "sketch/SketchOps.h"

#include <numbers>

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

static void symmetricAndMirror() {
  Sketch s;
  const int axis = s.addLine(s.addPoint(0, -10), s.addPoint(0, 10));
  s.addConstraint({K::Vertical, axis});
  s.points[s.entities[axis].p[0]].fixed = true;
  const int a = s.addPoint(2, 0), b = s.addPoint(6, 1), c = s.addPoint(0, 5); // c is on the axis
  const int l1 = s.addLine(a, b), l2 = s.addLine(b, c);
  const int circle = s.addCircle(s.addPoint(4, -4), 1.5);
  const int before = s.dof();
  const std::vector<int> copies = mirror(s, {l1, l2, circle, axis}, axis);
  CHECK(copies.size() == 3);
  CHECK(s.dof() == before); // the copy follows the original: no new freedom
  CHECK(s.entities[copies[1]].p[1] == c); // shared point on the axis
  // Move the original: the mirror image follows.
  s.points[a].fixed = true;
  s.setPoint(a, 3, 2);
  CHECK(s.solve() == SolveResult::Ok);
  const int ma = s.entities[copies[0]].p[0];
  CHECK(near(s.x(ma), -3) && near(s.y(ma), 2));
  CHECK(near(s.radius(copies[2]), s.radius(circle)));
}

static int lineCount(const Sketch& s) {
  int n = 0;
  for (const Entity& e : s.entities) n += e.kind == EntityKind::Line;
  return n;
}

static void trimming() {
  // A plus sign: trimming the right arm of the horizontal line stops at the vertical one.
  Sketch s;
  const int h = s.addLine(s.addPoint(-10, 0), s.addPoint(10, 0));
  const int v = s.addLine(s.addPoint(0, -10), s.addPoint(0, 10));
  CHECK(trim(s, h, 6, 0));
  CHECK(near(s.x(s.entities[h].p[1]), 0));
  CHECK(s.solve() == SolveResult::Ok);
  // Middle of a line cut twice: two pieces remain.
  Sketch m;
  const int long_ = m.addLine(m.addPoint(0, 0), m.addPoint(30, 0));
  m.addConstraint({K::Horizontal, long_});
  m.addLine(m.addPoint(10, -5), m.addPoint(10, 5));
  m.addLine(m.addPoint(20, -5), m.addPoint(20, 5));
  CHECK(trim(m, long_, 15, 0));
  CHECK(lineCount(m) == 4);
  CHECK(m.solve() == SolveResult::Ok);
  // A circle crossed by a line becomes an arc; with nothing crossing it disappears.
  Sketch c;
  const int circle = c.addCircle(c.addPoint(0, 0), 5);
  c.addLine(c.addPoint(-10, 0), c.addPoint(10, 0));
  CHECK(trim(c, circle, 0, -5)); // remove the lower half
  CHECK(c.entities[circle].kind == EntityKind::Arc);
  const Entity& arc = c.entities[circle];
  CHECK(c.y(arc.p[1]) > -1e-6 && near(std::abs(c.x(arc.p[1])), 5)); // starts on the line, upper half kept
  CHECK(c.solve() == SolveResult::Ok);
  Sketch lone;
  lone.addCircle(lone.addPoint(0, 0), 5);
  CHECK(trim(lone, 0, 5, 0) && lone.entities.empty());
  (void)v;
}

static void cornerFillet() {
  Sketch s;
  const int p0 = s.addPoint(0, 0), p1 = s.addPoint(10, 0), p2 = s.addPoint(10, 10);
  const int a = s.addLine(p0, p1), b = s.addLine(p1, p2);
  s.addConstraint({K::Horizontal, a});
  s.addConstraint({K::Vertical, b});
  CHECK(filletCorner(s, p1, 3));
  CHECK(s.entities.size() == 3 && s.entities[2].kind == EntityKind::Arc);
  CHECK(s.solve() == SolveResult::Ok);
  CHECK(near(s.x(s.entities[a].p[1]), 7) && near(s.y(s.entities[b].p[0]), 3));
  CHECK(near(s.radius(2), 3));
  const int c = s.entities[2].p[0];
  CHECK(near(s.x(c), 7) && near(s.y(c), 3));
  CHECK(!filletCorner(s, p0, 1)); // only one line ends there
  Sketch big;
  const int q = big.addPoint(5, 0);
  big.addLine(big.addPoint(0, 0), q);
  big.addLine(q, big.addPoint(5, 5));
  CHECK(!filletCorner(big, q, 50)); // larger than the lines
}

static void offsets() {
  Sketch s;
  const int l = s.addLine(s.addPoint(0, 0), s.addPoint(10, 0));
  const int o = offsetCurve(s, l, 2);
  CHECK(near(s.y(s.entities[o].p[0]), 2)); // to the left of 0 -> 10
  const int c = s.addCircle(s.addPoint(0, 0), 5);
  CHECK(near(s.radius(offsetCurve(s, c, -1)), 4));
  CHECK(offsetCurve(s, c, -6) < 0);
  CHECK(s.solve() == SolveResult::Ok);
  Sketch r;
  const int a = r.addPoint(0, 0), b = r.addPoint(1, 0), cc = r.addPoint(1, 1), d = r.addPoint(0, 1);
  const std::vector<int> loop{r.addLine(a, b), r.addLine(b, cc), r.addLine(cc, d), r.addLine(d, a)};
  CHECK(formsClosedLoops(r, loop));
  CHECK(!formsClosedLoops(r, {loop[0], loop[1]}));
}

static void splineOps() {
  Sketch s;
  const int a = s.addPoint(0, 0), b = s.addPoint(10, 5), c = s.addPoint(20, 0);
  const int sp = s.addSpline({a, b, c});
  CHECK(s.valid());
  CHECK(!s.isCircular(sp));
  CHECK(s.usedPoints().size() == 3);
  CHECK(s.dof() == 6);
  // A line closes it; the spline's ends count as loop ends.
  const int l = s.addLine(c, a);
  CHECK(formsClosedLoops(s, {sp, l}));
  CHECK(!formsClosedLoops(s, {sp}));
  // Splines are not trimmed or offset, and take no line/circle constraints.
  CHECK(!trim(s, sp, 10, 5));
  CHECK(offsetCurve(s, sp, 1) < 0);
  Sketch bad = s;
  bad.addConstraint({K::Horizontal, sp});
  CHECK(!bad.valid());
  // Mirror across the closing line copies every point.
  const std::vector<int> copies = mirror(s, {sp}, l);
  CHECK(copies.size() == 1 && s.entities[copies[0]].kind == EntityKind::Spline);
  const Entity& m = s.entities[copies[0]];
  CHECK(m.pts.size() == 3 && m.pts[0] == a && m.pts[2] == c && near(s.y(m.pts[1]), -5));
  CHECK(s.solve() == SolveResult::Ok);
  // Points of a spline can be fixed and dragged by the solver like any other.
  s.points[a].fixed = true;
  s.addConstraint({K::Distance, a, b, 15});
  CHECK(s.solve() == SolveResult::Ok);
  CHECK(near(s.distance(a, b), 15));
  // Removing it frees its middle point.
  Sketch t;
  t.addSpline({t.addPoint(0, 0), t.addPoint(1, 1), t.addPoint(2, 0)});
  t.removeEntity(0);
  CHECK(t.usedPoints().empty() && t.dof() == 0);
  // Closed spline: first point repeated at the end.
  Sketch u;
  const int p = u.addPoint(0, 0);
  const int closed = u.addSpline({p, u.addPoint(5, 5), u.addPoint(10, 0), p});
  CHECK(u.entities[closed].closedSpline());
  CHECK(u.entities[closed].pointList().size() == 3);
  CHECK(!formsClosedLoops(u, {closed})); // handled on its own, like a circle
}

int main() {
  splineOps();
  symmetricAndMirror();
  trimming();
  cornerFillet();
  offsets();
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
