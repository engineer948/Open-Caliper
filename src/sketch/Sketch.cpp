#include "sketch/Sketch.h"

#include <algorithm>
#include <cmath>

namespace sketch {
namespace {

constexpr double kEps = 1e-12;
constexpr double kTolerance = 1e-7; // residual accepted as "satisfied" (model units)

bool aIsEntity(ConstraintKind k) {
  switch (k) {
  case ConstraintKind::Coincident:
  case ConstraintKind::Distance:
  case ConstraintKind::PointOnLine:
  case ConstraintKind::PointOnCircle:
  case ConstraintKind::Symmetric:
    return false;
  default:
    return true;
  }
}

bool bIsEntity(ConstraintKind k) {
  switch (k) {
  case ConstraintKind::Parallel:
  case ConstraintKind::Perpendicular:
  case ConstraintKind::Equal:
  case ConstraintKind::Tangent:
  case ConstraintKind::PointOnLine:
  case ConstraintKind::PointOnCircle:
    return true;
  default:
    return false;
  }
}

double norm2(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x * x;
  return s;
}

// Solves A x = b for an n x n row-major A (Gaussian elimination, partial pivoting).
// The result replaces b.
bool solveLinear(std::vector<double>& A, std::vector<double>& b, int n) {
  for (int c = 0; c < n; ++c) {
    int piv = c;
    for (int r = c + 1; r < n; ++r)
      if (std::abs(A[r * n + c]) > std::abs(A[piv * n + c])) piv = r;
    if (std::abs(A[piv * n + c]) < kEps) return false;
    if (piv != c) {
      for (int k = 0; k < n; ++k) std::swap(A[c * n + k], A[piv * n + k]);
      std::swap(b[c], b[piv]);
    }
    for (int r = c + 1; r < n; ++r) {
      const double f = A[r * n + c] / A[c * n + c];
      if (f == 0) continue;
      for (int k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
      b[r] -= f * b[c];
    }
  }
  for (int r = n - 1; r >= 0; --r) {
    double s = b[r];
    for (int k = r + 1; k < n; ++k) s -= A[r * n + k] * b[k];
    b[r] = s / A[r * n + r];
  }
  return true;
}

int matrixRank(std::vector<double> M, int rows, int cols) {
  double maxAbs = 0;
  for (double v : M) maxAbs = std::max(maxAbs, std::abs(v));
  const double tol = 1e-6 * std::max(1.0, maxAbs);
  int rank = 0;
  for (int c = 0; c < cols && rank < rows; ++c) {
    int piv = rank;
    for (int r = rank + 1; r < rows; ++r)
      if (std::abs(M[r * cols + c]) > std::abs(M[piv * cols + c])) piv = r;
    if (std::abs(M[piv * cols + c]) <= tol) continue;
    for (int k = 0; k < cols; ++k) std::swap(M[rank * cols + k], M[piv * cols + k]);
    for (int r = rank + 1; r < rows; ++r) {
      const double f = M[r * cols + c] / M[rank * cols + c];
      for (int k = c; k < cols; ++k) M[r * cols + k] -= f * M[rank * cols + k];
    }
    ++rank;
  }
  return rank;
}

} // namespace

std::vector<int> Entity::pointList() const {
  std::vector<int> out;
  for (int q : p)
    if (q >= 0) out.push_back(q);
  for (int q : pts)
    if (std::find(out.begin(), out.end(), q) == out.end()) out.push_back(q);
  return out;
}

bool Entity::uses(int point) const {
  return std::find(std::begin(p), std::end(p), point) != std::end(p) ||
         std::find(pts.begin(), pts.end(), point) != pts.end();
}

bool Sketch::valid() const {
  const int np = int(params.size()), npt = int(points.size()), ne = int(entities.size());
  for (const Point& p : points)
    if (p.x < 0 || p.y < 0 || p.x >= np || p.y >= np) return false;
  for (const Entity& e : entities) {
    if (e.kind == EntityKind::Spline) {
      if (e.pts.size() < 2) return false;
      for (int q : e.pts)
        if (q < 0 || q >= npt) return false;
      continue;
    }
    const int needed = e.kind == EntityKind::Line ? 2 : e.kind == EntityKind::Arc ? 3 : 1;
    for (int i = 0; i < needed; ++i)
      if (e.p[i] < 0 || e.p[i] >= npt) return false;
    if (e.kind == EntityKind::Circle && (e.r < 0 || e.r >= np)) return false;
  }
  auto line = [&](int e) { return e >= 0 && e < ne && entities[e].kind == EntityKind::Line; };
  auto circular = [&](int e) { return e >= 0 && e < ne && isCircular(e); };
  auto point = [&](int p) { return p >= 0 && p < npt; };
  for (const Constraint& c : constraints) {
    bool ok = false;
    switch (c.kind) {
    case ConstraintKind::Coincident:
    case ConstraintKind::Distance: ok = point(c.a) && point(c.b); break;
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical:
    case ConstraintKind::Length: ok = line(c.a); break;
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular: ok = line(c.a) && line(c.b); break;
    case ConstraintKind::Equal: ok = (line(c.a) && line(c.b)) || (circular(c.a) && circular(c.b)); break;
    case ConstraintKind::Tangent: ok = line(c.a) && circular(c.b); break;
    case ConstraintKind::PointOnLine: ok = point(c.a) && line(c.b); break;
    case ConstraintKind::PointOnCircle: ok = point(c.a) && circular(c.b); break;
    case ConstraintKind::Radius: ok = circular(c.a); break;
    case ConstraintKind::Symmetric: ok = point(c.a) && point(c.b) && line(c.c); break;
    }
    if (!ok) return false;
  }
  return true;
}

int Sketch::addPoint(double x, double y) {
  params.push_back(x);
  params.push_back(y);
  points.push_back({int(params.size()) - 2, int(params.size()) - 1});
  return int(points.size()) - 1;
}

int Sketch::addLine(int p1, int p2) {
  Entity e{EntityKind::Line};
  e.p[0] = p1;
  e.p[1] = p2;
  entities.push_back(e);
  return int(entities.size()) - 1;
}

int Sketch::addCircle(int center, double radius) {
  params.push_back(radius);
  Entity e{EntityKind::Circle};
  e.p[0] = center;
  e.r = int(params.size()) - 1;
  entities.push_back(e);
  return int(entities.size()) - 1;
}

int Sketch::addArc(int center, int start, int end) {
  Entity e{EntityKind::Arc};
  e.p[0] = center;
  e.p[1] = start;
  e.p[2] = end;
  entities.push_back(e);
  return int(entities.size()) - 1;
}

int Sketch::addSpline(std::vector<int> through) {
  Entity e{EntityKind::Spline};
  e.pts = std::move(through);
  entities.push_back(e);
  return int(entities.size()) - 1;
}

int Sketch::addConstraint(const Constraint& c) {
  constraints.push_back(c);
  return int(constraints.size()) - 1;
}

void Sketch::removeEntity(int e) {
  std::erase_if(constraints, [e](const Constraint& c) {
    return (aIsEntity(c.kind) && c.a == e) || (bIsEntity(c.kind) && c.b == e) || c.c == e;
  });
  for (Constraint& c : constraints) {
    if (aIsEntity(c.kind) && c.a > e) --c.a;
    if (bIsEntity(c.kind) && c.b > e) --c.b;
    if (c.c > e) --c.c;
  }
  entities.erase(entities.begin() + e);
}

std::vector<int> Sketch::usedPoints() const {
  std::vector<bool> used(points.size(), false);
  for (const Entity& e : entities)
    for (int p : e.pointList()) used[p] = true;
  std::vector<int> result;
  for (int i = 0; i < int(used.size()); ++i)
    if (used[i]) result.push_back(i);
  return result;
}

void Sketch::dropDanglingConstraints() {
  std::vector<bool> used(points.size(), false);
  for (int p : usedPoints()) used[p] = true;
  std::erase_if(constraints, [&](const Constraint& c) {
    return (!aIsEntity(c.kind) && !used[c.a]) || (c.b >= 0 && !bIsEntity(c.kind) && !used[c.b]);
  });
}

void Sketch::setPoint(int pt, double x, double y) {
  params[points[pt].x] = x;
  params[points[pt].y] = y;
}

double Sketch::distance(int a, int b) const {
  return std::hypot(x(b) - x(a), y(b) - y(a));
}

double Sketch::length(int line) const {
  return distance(entities[line].p[0], entities[line].p[1]);
}

double Sketch::radius(int e) const {
  const Entity& en = entities[e];
  return en.kind == EntityKind::Circle ? params[en.r] : distance(en.p[0], en.p[1]);
}

// Parameters of live entities that are not fixed. Leftovers of deleted
// entities (orphan points, old radii) are ignored.
std::vector<int> Sketch::freeParams() const {
  std::vector<bool> used(params.size(), false);
  for (const Entity& e : entities) {
    for (int p : e.pointList())
      if (!points[p].fixed) used[points[p].x] = used[points[p].y] = true;
    if (e.r >= 0) used[e.r] = true;
  }
  std::vector<int> free;
  for (int i = 0; i < int(params.size()); ++i)
    if (used[i]) free.push_back(i);
  return free;
}

void Sketch::residuals(const std::vector<double>& p, std::vector<double>& out) const {
  out.clear();
  auto X = [&](int pt) { return p[points[pt].x]; };
  auto Y = [&](int pt) { return p[points[pt].y]; };
  auto dx = [&](int line) { return X(entities[line].p[1]) - X(entities[line].p[0]); };
  auto dy = [&](int line) { return Y(entities[line].p[1]) - Y(entities[line].p[0]); };
  auto len = [&](int line) { return std::hypot(dx(line), dy(line)); };
  auto rad = [&](int e) {
    const Entity& en = entities[e];
    if (en.kind == EntityKind::Circle) return p[en.r];
    return std::hypot(X(en.p[1]) - X(en.p[0]), Y(en.p[1]) - Y(en.p[0]));
  };
  auto center = [&](int e) { return entities[e].p[0]; };

  // Arcs keep both ends on the same circle.
  for (const Entity& e : entities)
    if (e.kind == EntityKind::Arc)
      out.push_back(std::hypot(X(e.p[2]) - X(e.p[0]), Y(e.p[2]) - Y(e.p[0])) -
                    std::hypot(X(e.p[1]) - X(e.p[0]), Y(e.p[1]) - Y(e.p[0])));

  for (const Constraint& c : constraints) {
    switch (c.kind) {
    case ConstraintKind::Coincident:
      out.push_back(X(c.a) - X(c.b));
      out.push_back(Y(c.a) - Y(c.b));
      break;
    case ConstraintKind::Horizontal:
      out.push_back(dy(c.a));
      break;
    case ConstraintKind::Vertical:
      out.push_back(dx(c.a));
      break;
    case ConstraintKind::Parallel: // normalized so shrinking a line is no way out
      out.push_back((dx(c.a) * dy(c.b) - dy(c.a) * dx(c.b)) / (len(c.a) * len(c.b) + kEps));
      break;
    case ConstraintKind::Perpendicular:
      out.push_back((dx(c.a) * dx(c.b) + dy(c.a) * dy(c.b)) / (len(c.a) * len(c.b) + kEps));
      break;
    case ConstraintKind::Equal:
      if (isCircular(c.a)) out.push_back(rad(c.a) - rad(c.b));
      else out.push_back(len(c.a) - len(c.b));
      break;
    case ConstraintKind::Tangent: {
      const Entity& line = entities[c.a];
      const Entity& curve = entities[c.b];
      const int cc = center(c.b);
      // Line and arc joined at an end (slots, rounded corners): "distance =
      // radius" has zero slope there, so the solver could not see it. Ask for
      // the line to be perpendicular to the radius at the shared point instead.
      int joint = -1;
      if (curve.kind == EntityKind::Arc)
        for (int lp : {line.p[0], line.p[1]})
          if (lp == curve.p[1] || lp == curve.p[2]) joint = lp;
      if (joint >= 0) {
        const double rx = X(joint) - X(cc), ry = Y(joint) - Y(cc);
        out.push_back((dx(c.a) * rx + dy(c.a) * ry) / (len(c.a) * std::hypot(rx, ry) + kEps));
      } else {
        const int p0 = line.p[0];
        const double d = std::abs(dx(c.a) * (Y(cc) - Y(p0)) - dy(c.a) * (X(cc) - X(p0))) / (len(c.a) + kEps);
        out.push_back(d - rad(c.b));
      }
      break;
    }
    case ConstraintKind::PointOnLine: {
      const int p0 = entities[c.b].p[0];
      out.push_back((dx(c.b) * (Y(c.a) - Y(p0)) - dy(c.b) * (X(c.a) - X(p0))) / (len(c.b) + kEps));
      break;
    }
    case ConstraintKind::PointOnCircle: {
      const int cc = center(c.b);
      out.push_back(std::hypot(X(c.a) - X(cc), Y(c.a) - Y(cc)) - rad(c.b));
      break;
    }
    case ConstraintKind::Distance:
      out.push_back(std::hypot(X(c.b) - X(c.a), Y(c.b) - Y(c.a)) - c.value);
      break;
    case ConstraintKind::Length:
      out.push_back(len(c.a) - c.value);
      break;
    case ConstraintKind::Radius:
      out.push_back(rad(c.a) - c.value);
      break;
    case ConstraintKind::Symmetric: { // midpoint on the line, a -> b across it
      const int p0 = entities[c.c].p[0];
      const double mx = (X(c.a) + X(c.b)) / 2, my = (Y(c.a) + Y(c.b)) / 2;
      out.push_back((dx(c.c) * (my - Y(p0)) - dy(c.c) * (mx - X(p0))) / (len(c.c) + kEps));
      out.push_back((dx(c.c) * (X(c.b) - X(c.a)) + dy(c.c) * (Y(c.b) - Y(c.a))) / (len(c.c) + kEps));
      break;
    }
    }
  }
}

// Central differences: plenty accurate for sketch-sized systems.
void Sketch::jacobian(const std::vector<double>& p, const std::vector<int>& free,
                      std::vector<double>& J, int& rows) const {
  std::vector<double> q = p, fPlus, fMinus;
  residuals(p, fPlus);
  rows = int(fPlus.size());
  const int n = int(free.size());
  J.assign(size_t(rows) * n, 0.0);
  for (int j = 0; j < n; ++j) {
    const int i = free[j];
    const double h = 1e-6 * std::max(1.0, std::abs(p[i]));
    q[i] = p[i] + h;
    residuals(q, fPlus);
    q[i] = p[i] - h;
    residuals(q, fMinus);
    q[i] = p[i];
    for (int r = 0; r < rows; ++r) J[size_t(r) * n + j] = (fPlus[r] - fMinus[r]) / (2 * h);
  }
}

// Levenberg-Marquardt with a plain lambda*I damping term: it also handles
// under-constrained sketches, where it prefers the smallest move.
SolveResult Sketch::solve() {
  const std::vector<int> free = freeParams();
  const int n = int(free.size());
  std::vector<double> p = params, F, Ftry;
  residuals(p, F);
  double cost = norm2(F);

  double lambda = 1e-3;
  std::vector<double> J, A, g, M, step, ptry;
  for (int iter = 0; iter < 200 && cost > kTolerance * kTolerance && n > 0; ++iter) {
    int m = 0;
    jacobian(p, free, J, m);
    A.assign(size_t(n) * n, 0.0);
    g.assign(n, 0.0);
    for (int r = 0; r < m; ++r) {
      const double* row = &J[size_t(r) * n];
      for (int i = 0; i < n; ++i) {
        if (row[i] == 0) continue;
        g[i] += row[i] * F[r];
        for (int k = 0; k < n; ++k) A[size_t(i) * n + k] += row[i] * row[k];
      }
    }

    bool improved = false;
    while (lambda < 1e12) {
      M = A;
      step.resize(n);
      for (int i = 0; i < n; ++i) {
        M[size_t(i) * n + i] += lambda;
        step[i] = -g[i];
      }
      if (solveLinear(M, step, n)) {
        ptry = p;
        for (int i = 0; i < n; ++i) ptry[free[i]] += step[i];
        residuals(ptry, Ftry);
        const double c = norm2(Ftry);
        if (c < cost) {
          p.swap(ptry);
          F.swap(Ftry);
          cost = c;
          lambda = std::max(lambda * 0.1, 1e-12);
          improved = true;
          break;
        }
      }
      lambda *= 10;
    }
    if (!improved) break;
  }

  if (cost > kTolerance * kTolerance) return SolveResult::Failed;
  params = p;
  return SolveResult::Ok;
}

int Sketch::dof() const {
  const std::vector<int> free = freeParams();
  std::vector<double> J;
  int rows = 0;
  jacobian(params, free, J, rows);
  return int(free.size()) - matrixRank(J, rows, int(free.size()));
}

} // namespace sketch
