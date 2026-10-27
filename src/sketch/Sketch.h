#pragma once

#include <string>
#include <vector>

// Pure 2D sketch: geometry, constraints and the constraint solver.
// No Qt, no OCCT, so it can be unit-tested on its own.
namespace sketch {

// Coordinates live in Sketch::params so the solver can move them.
struct Point {
  int x, y;           // indices into Sketch::params
  bool fixed = false; // solver never moves a fixed point
};

enum class EntityKind { Line, Circle, Arc, Spline };

// Line:   p[0] -> p[1]
// Circle: center p[0], radius params[r]
// Arc:    center p[0], counter-clockwise from p[1] to p[2] (both kept on one circle)
// Spline: smooth curve through the points `pts` in order; closed when the
//         first point is repeated at the end
struct Entity {
  EntityKind kind;
  int p[3] = {-1, -1, -1};
  int r = -1;
  bool construction = false; // helper geometry: constrains, but never becomes a solid
  std::vector<int> pts;      // Spline only

  std::vector<int> pointList() const; // every point the entity uses (no repeats)
  bool uses(int point) const;
  bool closedSpline() const { return pts.size() > 2 && pts.front() == pts.back(); }
};

enum class ConstraintKind {
  Coincident,    // points a, b
  Horizontal,    // line a
  Vertical,      // line a
  Parallel,      // lines a, b
  Perpendicular, // lines a, b
  Equal,         // lines a, b (length) or circles/arcs a, b (radius)
  Tangent,       // line a, circle/arc b
  PointOnLine,   // point a, line b
  PointOnCircle, // point a, circle/arc b
  Distance,      // points a, b at `value`
  Length,        // line a is `value` long
  Radius,        // circle/arc a has radius `value`
  Symmetric,     // points a, b mirror images across line c
};

struct Constraint {
  ConstraintKind kind;
  int a = -1, b = -1;
  double value = 0;
  std::string expr; // when set, `value` comes from this parameter expression
  int c = -1;       // third reference (Symmetric: the mirror line)
};

enum class SolveResult { Ok, Failed };

class Sketch {
public:
  std::vector<double> params;
  std::vector<Point> points;
  std::vector<Entity> entities;
  std::vector<Constraint> constraints;

  int addPoint(double x, double y);
  int addLine(int p1, int p2);
  int addCircle(int center, double radius);
  int addArc(int center, int start, int end);
  int addSpline(std::vector<int> through); // repeat the first point at the end for a closed one
  int addConstraint(const Constraint& c);
  void removeEntity(int e); // also drops the constraints that use it
  // Drops constraints on points that no curve uses any more (after trimming).
  void dropDanglingConstraints();
  std::vector<int> usedPoints() const;

  double x(int pt) const { return params[points[pt].x]; }
  double y(int pt) const { return params[points[pt].y]; }
  void setPoint(int pt, double x, double y);
  double distance(int a, int b) const;
  double length(int line) const;
  double radius(int e) const;
  bool isCircular(int e) const { return entities[e].kind == EntityKind::Circle || entities[e].kind == EntityKind::Arc; }
  // All indices in range and constraints refer to the right kind of entity
  // (for data from files).
  bool valid() const;

  // Moves free parameters as little as possible until all constraints hold.
  // On failure (conflicting constraints) nothing is changed.
  SolveResult solve();
  // Remaining degrees of freedom; 0 means fully constrained.
  int dof() const;

private:
  void residuals(const std::vector<double>& p, std::vector<double>& out) const;
  std::vector<int> freeParams() const;
  void jacobian(const std::vector<double>& p, const std::vector<int>& free,
                std::vector<double>& J, int& rows) const;
};

} // namespace sketch
