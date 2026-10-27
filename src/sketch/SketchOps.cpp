#include "sketch/SketchOps.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <utility>

namespace sketch {
namespace {

using K = ConstraintKind;
constexpr double kPi = std::numbers::pi;
constexpr double kEps = 1e-9;

struct P {
  double x, y;
};

P at(const Sketch& s, int pt) { return {s.x(pt), s.y(pt)}; }

// Counter-clockwise angle from `from` to `to`, in [0, 2pi).
double ccw(double from, double to) {
  double d = std::fmod(to - from, 2 * kPi);
  return d < 0 ? d + 2 * kPi : d;
}

// Where a curve runs: a segment, or a circle with an angular range.
struct Curve {
  bool line = true;
  P a{}, b{};             // segment ends
  P c{};                  // centre
  double r = 0;           // radius
  double start = 0, span = 2 * kPi; // arc: from angle `start`, ccw over `span`
  bool full = false;      // whole circle
};

Curve curveOf(const Sketch& s, int e) {
  const Entity& en = s.entities[e];
  Curve c;
  if (en.kind == EntityKind::Line) {
    c.a = at(s, en.p[0]);
    c.b = at(s, en.p[1]);
    return c;
  }
  c.line = false;
  c.c = at(s, en.p[0]);
  c.r = s.radius(e);
  if (en.kind == EntityKind::Circle) {
    c.full = true;
    return c;
  }
  const P p1 = at(s, en.p[1]), p2 = at(s, en.p[2]);
  c.start = std::atan2(p1.y - c.c.y, p1.x - c.c.x);
  c.span = ccw(c.start, std::atan2(p2.y - c.c.y, p2.x - c.c.x));
  if (c.span < kEps) c.span = 2 * kPi;
  return c;
}

// Position along the curve: 0..1 on a segment, the ccw angle from the start on a circle/arc.
double paramOf(const Curve& c, P p) {
  if (c.line) {
    const double dx = c.b.x - c.a.x, dy = c.b.y - c.a.y, len2 = dx * dx + dy * dy;
    return len2 > 0 ? ((p.x - c.a.x) * dx + (p.y - c.a.y) * dy) / len2 : 0;
  }
  return ccw(c.start, std::atan2(p.y - c.c.y, p.x - c.c.x));
}

P pointAt(const Curve& c, double t) {
  if (c.line) return {c.a.x + t * (c.b.x - c.a.x), c.a.y + t * (c.b.y - c.a.y)};
  const double a = c.start + t;
  return {c.c.x + c.r * std::cos(a), c.c.y + c.r * std::sin(a)};
}

bool contains(const Curve& c, P p, double tol) {
  const double t = paramOf(c, p);
  if (c.line) {
    const double len = std::hypot(c.b.x - c.a.x, c.b.y - c.a.y);
    return t >= -tol / std::max(len, kEps) && t <= 1 + tol / std::max(len, kEps);
  }
  if (c.full) return true;
  return t <= c.span + tol / c.r || t >= 2 * kPi - tol / c.r;
}

// Crossings of the full line/circle carriers; filtered by `contains` afterwards.
std::vector<P> crossings(const Curve& p, const Curve& q) {
  std::vector<P> out;
  if (p.line && q.line) {
    const double rx = p.b.x - p.a.x, ry = p.b.y - p.a.y, sx = q.b.x - q.a.x, sy = q.b.y - q.a.y;
    const double den = rx * sy - ry * sx;
    if (std::abs(den) < kEps) return out; // parallel
    const double t = ((q.a.x - p.a.x) * sy - (q.a.y - p.a.y) * sx) / den;
    out.push_back({p.a.x + t * rx, p.a.y + t * ry});
  } else if (p.line != q.line) {
    const Curve& l = p.line ? p : q;
    const Curve& c = p.line ? q : p;
    const double dx = l.b.x - l.a.x, dy = l.b.y - l.a.y, fx = l.a.x - c.c.x, fy = l.a.y - c.c.y;
    const double A = dx * dx + dy * dy, B = 2 * (fx * dx + fy * dy), C = fx * fx + fy * fy - c.r * c.r;
    const double disc = B * B - 4 * A * C;
    if (A < kEps || disc < -kEps) return out;
    const double root = std::sqrt(std::max(0.0, disc));
    for (double t : {(-B - root) / (2 * A), (-B + root) / (2 * A)}) out.push_back({l.a.x + t * dx, l.a.y + t * dy});
  } else {
    const double dx = q.c.x - p.c.x, dy = q.c.y - p.c.y, d = std::hypot(dx, dy);
    if (d < kEps || d > p.r + q.r + kEps || d < std::abs(p.r - q.r) - kEps) return out;
    const double a = (p.r * p.r - q.r * q.r + d * d) / (2 * d), h = std::sqrt(std::max(0.0, p.r * p.r - a * a));
    const double mx = p.c.x + a * dx / d, my = p.c.y + a * dy / d;
    out.push_back({mx - h * dy / d, my + h * dx / d});
    out.push_back({mx + h * dy / d, my - h * dx / d});
  }
  return out;
}

// A new point at p, constrained onto curve `cutter`.
int pointOn(Sketch& s, P p, int cutter) {
  const int pt = s.addPoint(p.x, p.y);
  s.addConstraint({s.isCircular(cutter) ? K::PointOnCircle : K::PointOnLine, pt, cutter});
  return pt;
}

} // namespace

bool trim(Sketch& s, int entity, double u, double v) {
  if (entity < 0 || entity >= int(s.entities.size())) return false;
  if (s.entities[entity].kind == EntityKind::Spline) return false; // only lines, circles and arcs
  const Curve me = curveOf(s, entity);
  const double scale = me.line ? std::hypot(me.b.x - me.a.x, me.b.y - me.a.y) : me.r;
  const double tol = 1e-7 * std::max(1.0, scale);

  // Crossings as (position along this curve, cutting curve).
  std::vector<std::pair<double, int>> cuts;
  for (int o = 0; o < int(s.entities.size()); ++o) {
    if (o == entity || s.entities[o].kind == EntityKind::Spline) continue;
    const Curve other = curveOf(s, o);
    for (const P& p : crossings(me, other)) {
      if (!contains(me, p, tol) || !contains(other, p, tol)) continue;
      double t = paramOf(me, p);
      const double end = me.line ? 1 : me.span;
      const double minGap = me.line ? tol / std::max(scale, kEps) : tol / me.r;
      if (!me.full) {
        if (t > end + minGap) t -= 2 * kPi; // just before an arc's start
        if (t <= minGap || t >= end - minGap) continue; // at an end: not a cut
      }
      cuts.emplace_back(t, o);
    }
  }
  std::sort(cuts.begin(), cuts.end());

  const double click = paramOf(me, {u, v});
  Entity& en = s.entities[entity];
  if (me.full) { // a circle needs two cuts; what is left becomes an arc
    if (cuts.size() < 2) {
      s.removeEntity(entity);
      s.dropDanglingConstraints();
      return true;
    }
    auto hi = std::upper_bound(cuts.begin(), cuts.end(), std::make_pair(click, -1));
    const auto& after = hi == cuts.end() ? cuts.front() : *hi;                    // first cut ccw of the click
    const auto& before = hi == cuts.begin() ? cuts.back() : *std::prev(hi);       // last cut before it
    const int a = pointOn(s, pointAt(me, after.first), after.second);
    const int b = pointOn(s, pointAt(me, before.first), before.second);
    en.kind = EntityKind::Arc;
    en.p[1] = a; // keep from `after` round to `before`
    en.p[2] = b;
    en.r = -1;
    return true;
  }

  const auto hi = std::upper_bound(cuts.begin(), cuts.end(), std::make_pair(click, -1));
  const bool hasAfter = hi != cuts.end(), hasBefore = hi != cuts.begin();
  if (!hasAfter && !hasBefore) {
    s.removeEntity(entity);
    s.dropDanglingConstraints();
    return true;
  }
  const int last = me.line ? 1 : 2; // index of the end point in Entity::p
  const int first = me.line ? 0 : 1;
  // A changed line has a new length; its old Length constraint would fight that.
  std::erase_if(s.constraints, [&](const Constraint& c) { return c.kind == K::Length && c.a == entity; });
  if (hasBefore && hasAfter) { // middle piece: split in two
    const int oldEnd = en.p[last];
    const int cutA = pointOn(s, pointAt(me, std::prev(hi)->first), std::prev(hi)->second);
    const int cutB = pointOn(s, pointAt(me, hi->first), hi->second);
    s.entities[entity].p[last] = cutA;
    if (me.line) {
      const int piece = s.addLine(cutB, oldEnd);
      std::vector<ConstraintKind> keep; // a horizontal line stays horizontal in both halves
      for (const Constraint& c : s.constraints)
        if ((c.kind == K::Horizontal || c.kind == K::Vertical) && c.a == entity) keep.push_back(c.kind);
      for (ConstraintKind k : keep) s.addConstraint({k, piece});
      if (keep.empty()) s.addConstraint({K::Parallel, entity, piece});
    } else {
      s.addConstraint({K::Equal, entity, s.addArc(s.entities[entity].p[0], cutB, oldEnd)});
    }
    s.dropDanglingConstraints();
    return true;
  }
  if (hasAfter) en.p[first] = pointOn(s, pointAt(me, hi->first), hi->second);   // cut off the start
  else en.p[last] = pointOn(s, pointAt(me, std::prev(hi)->first), std::prev(hi)->second); // cut off the end
  s.dropDanglingConstraints();
  return true;
}

bool filletCorner(Sketch& s, int point, double radius) {
  if (radius <= 0) return false;
  std::vector<std::pair<int, int>> lines; // (entity, which end is the corner)
  for (int e = 0; e < int(s.entities.size()); ++e) {
    const Entity& en = s.entities[e];
    if (en.kind != EntityKind::Line) {
      if (en.uses(point)) return false;
      continue;
    }
    for (int k : {0, 1})
      if (en.p[k] == point) lines.emplace_back(e, k);
  }
  if (lines.size() != 2) return false;

  const P c = at(s, point);
  P dir[2];
  double len[2];
  for (int i = 0; i < 2; ++i) {
    const Entity& en = s.entities[lines[i].first];
    const P far = at(s, en.p[1 - lines[i].second]);
    len[i] = std::hypot(far.x - c.x, far.y - c.y);
    if (len[i] < kEps) return false;
    dir[i] = {(far.x - c.x) / len[i], (far.y - c.y) / len[i]};
  }
  const double angle = std::acos(std::clamp(dir[0].x * dir[1].x + dir[0].y * dir[1].y, -1.0, 1.0));
  if (angle < 1e-6 || angle > kPi - 1e-6) return false; // straight or folded back
  const double t = radius / std::tan(angle / 2);         // corner to tangent point
  if (t >= len[0] || t >= len[1]) return false;          // radius too large for the lines
  P bis{dir[0].x + dir[1].x, dir[0].y + dir[1].y};
  const double bl = std::hypot(bis.x, bis.y);
  bis = {bis.x / bl, bis.y / bl};
  const double cd = radius / std::sin(angle / 2);

  const int t0 = s.addPoint(c.x + dir[0].x * t, c.y + dir[0].y * t);
  const int t1 = s.addPoint(c.x + dir[1].x * t, c.y + dir[1].y * t);
  const int centre = s.addPoint(c.x + bis.x * cd, c.y + bis.y * cd);
  s.entities[lines[0].first].p[lines[0].second] = t0;
  s.entities[lines[1].first].p[lines[1].second] = t1;
  // Counter-clockwise over the short way round.
  const double cross = (s.x(t0) - s.x(centre)) * (s.y(t1) - s.y(centre)) - (s.y(t0) - s.y(centre)) * (s.x(t1) - s.x(centre));
  const int arc = cross > 0 ? s.addArc(centre, t0, t1) : s.addArc(centre, t1, t0);
  std::erase_if(s.constraints, [&](const Constraint& k) {
    return k.kind == K::Length && (k.a == lines[0].first || k.a == lines[1].first);
  });
  s.dropDanglingConstraints(); // the old corner point is gone
  s.addConstraint({K::Tangent, lines[0].first, arc});
  s.addConstraint({K::Tangent, lines[1].first, arc});
  s.addConstraint({K::Radius, arc, -1, radius});
  return true;
}

std::vector<int> mirror(Sketch& s, const std::vector<int>& entities, int axis) {
  if (axis < 0 || axis >= int(s.entities.size()) || s.entities[axis].kind != EntityKind::Line) return {};
  const P a = at(s, s.entities[axis].p[0]), b = at(s, s.entities[axis].p[1]);
  const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
  if (len2 < kEps) return {};
  std::map<int, int> image; // point -> mirrored point
  auto mirrored = [&](int pt) {
    if (const auto it = image.find(pt); it != image.end()) return it->second;
    const P p = at(s, pt);
    const double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    const P foot{a.x + t * dx, a.y + t * dy};
    int m = pt;
    if (std::hypot(p.x - foot.x, p.y - foot.y) > 1e-9 * std::max(1.0, std::sqrt(len2))) {
      m = s.addPoint(2 * foot.x - p.x, 2 * foot.y - p.y);
      s.addConstraint({K::Symmetric, pt, m, 0, {}, axis});
    }
    image[pt] = m;
    return m;
  };
  std::vector<int> copies;
  for (int e : entities) {
    if (e == axis || e < 0 || e >= int(s.entities.size())) continue;
    const Entity en = s.entities[e];
    int copy = -1;
    switch (en.kind) {
    case EntityKind::Line: copy = s.addLine(mirrored(en.p[0]), mirrored(en.p[1])); break;
    case EntityKind::Circle:
      copy = s.addCircle(mirrored(en.p[0]), s.radius(e));
      s.addConstraint({K::Equal, e, copy});
      break;
    case EntityKind::Arc: copy = s.addArc(mirrored(en.p[0]), mirrored(en.p[2]), mirrored(en.p[1])); break; // turns the other way
    case EntityKind::Spline: {
      std::vector<int> pts;
      for (int p : en.pts) pts.push_back(mirrored(p));
      copy = s.addSpline(pts);
      break;
    }
    }
    s.entities[copy].construction = en.construction;
    copies.push_back(copy);
  }
  return copies;
}

int offsetCurve(Sketch& s, int entity, double distance) {
  const Entity en = s.entities[entity];
  if (en.kind == EntityKind::Spline) return -1;
  if (en.kind == EntityKind::Line) {
    const P a = at(s, en.p[0]), b = at(s, en.p[1]);
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (len < kEps) return -1;
    const double nx = -(b.y - a.y) / len * distance, ny = (b.x - a.x) / len * distance;
    const int line = s.addLine(s.addPoint(a.x + nx, a.y + ny), s.addPoint(b.x + nx, b.y + ny));
    s.addConstraint({K::Parallel, entity, line});
    return line;
  }
  const double r = s.radius(entity), nr = r + distance;
  if (nr <= kEps) return -1;
  if (en.kind == EntityKind::Circle) return s.addCircle(en.p[0], nr);
  const P c = at(s, en.p[0]);
  auto scaled = [&](int pt) {
    const P p = at(s, pt);
    return s.addPoint(c.x + (p.x - c.x) * nr / r, c.y + (p.y - c.y) * nr / r);
  };
  return s.addArc(en.p[0], scaled(en.p[1]), scaled(en.p[2]));
}

bool formsClosedLoops(const Sketch& s, const std::vector<int>& entities) {
  std::map<int, int> degree;
  bool anyOpen = false;
  for (int e : entities) {
    const Entity& en = s.entities[e];
    if (en.kind == EntityKind::Circle || en.closedSpline()) continue;
    anyOpen = true;
    if (en.kind == EntityKind::Spline) {
      ++degree[en.pts.front()];
      ++degree[en.pts.back()];
      continue;
    }
    ++degree[en.kind == EntityKind::Line ? en.p[0] : en.p[1]];
    ++degree[en.kind == EntityKind::Line ? en.p[1] : en.p[2]];
  }
  if (!anyOpen) return false; // circles alone are handled one by one
  return std::all_of(degree.begin(), degree.end(), [](const auto& d) { return d.second == 2; });
}

} // namespace sketch
