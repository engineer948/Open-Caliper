#include "app/Dimensions.h"
#include "sketch/SketchShape.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Prs3d_ArrowAspect.hxx>
#include <Prs3d_DimensionAspect.hxx>
#include <Prs3d_TextAspect.hxx>
#include <PrsDim_DiameterDimension.hxx>
#include <PrsDim_LengthDimension.hxx>
#include <PrsDim_RadiusDimension.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <cmath>

using namespace model;
using sketch::ConstraintKind;
using sketch::EntityKind;

namespace {

constexpr double kMin = 1e-6;

// Arrows and offsets scale with the part so labels look the same at any size.
Handle(Prs3d_DimensionAspect) makeAspect(double modelSize) {
  Handle(Prs3d_DimensionAspect) a = new Prs3d_DimensionAspect();
  a->SetCommonColor(Quantity_Color(0.10, 0.30, 0.75, Quantity_TOC_sRGB));
  a->MakeText3d(false); // screen-sized text, readable when zoomed out
  a->TextAspect()->SetHeight(17);
  a->ArrowAspect()->SetLength(modelSize * 0.025);
  a->SetExtensionSize(modelSize * 0.02);
  a->SetValueStringFormat("%g");
  return a;
}

double modelSize(const Document& doc) {
  Bnd_Box box;
  if (!doc.body().IsNull()) BRepBndLib::Add(doc.body(), box);
  for (size_t i = 0; i < doc.size(); ++i)
    if (const auto* s = doc.as<SketchFeature>(i); s && !s->sketch.entities.empty())
      BRepBndLib::Add(s->edges(), box);
  return box.IsVoid() ? 100.0 : std::max(1.0, std::sqrt(box.SquareExtent()));
}

} // namespace

DimensionList buildDimensions(const Document& doc) {
  DimensionList out;
  const double size = modelSize(doc);
  const Handle(Prs3d_DimensionAspect) aspect = makeAspect(size);
  auto add = [&](const Handle(PrsDim_Dimension)& dim, const DimensionRef& ref, double flyout) {
    if (!dim->IsValid()) return;
    dim->SetDimensionAspect(aspect);
    if (flyout != 0) dim->SetFlyout(flyout);
    out.emplace_back(dim, ref);
  };

  for (size_t f = 0; f < doc.size(); ++f) {
    if (doc.error(f).size()) continue; // a broken step has nothing sensible to show
    if (const auto* sf = doc.as<SketchFeature>(f)) {
      const sketch::Sketch& s = sf->sketch;
      // A sketch pulled up into a solid would have its labels on the hidden
      // bottom face; draw them on the top face instead, where the holes show.
      gp_Ax3 at = sf->plane;
      for (size_t k = f + 1; k < doc.size(); ++k)
        if (const auto* ex = doc.as<ExtrudeFeature>(k); ex && ex->sketch == int(f)) {
          if (ex->op == Operation::Join && ex->distance > 0)
            at.Translate(gp_Vec(sf->plane.Direction()) * ex->distance);
          break;
        }
      const gp_Pln plane(at);
      auto world = [&](int pt) { return sketch::toWorld(at, s.x(pt), s.y(pt)); };

      // Every circle and arc: holes are what people want to resize most.
      for (int e = 0; e < int(s.entities.size()); ++e) {
        const sketch::Entity& en = s.entities[e];
        if (en.kind == EntityKind::Line) continue;
        const double r = s.radius(e);
        if (r < kMin) continue;
        const gp_Circ circ(gp_Ax2(world(en.p[0]), at.Direction(), at.XDirection()), r);
        if (en.kind == EntityKind::Circle)
          add(new PrsDim_DiameterDimension(circ, plane), {DimensionRef::Kind::SketchCurve, int(f), e, true, 2 * r}, 0);
        else
          add(new PrsDim_RadiusDimension(circ, world(en.p[1])), {DimensionRef::Kind::SketchCurve, int(f), e, false, r}, 0);
      }
      // Lengths and distances the user dimensioned in the sketch.
      for (int c = 0; c < int(s.constraints.size()); ++c) {
        const sketch::Constraint& con = s.constraints[c];
        int a = -1, b = -1;
        if (con.kind == ConstraintKind::Length) a = s.entities[con.a].p[0], b = s.entities[con.a].p[1];
        else if (con.kind == ConstraintKind::Distance) a = con.a, b = con.b;
        else continue;
        if (world(a).Distance(world(b)) < kMin) continue;
        add(new PrsDim_LengthDimension(world(a), world(b), plane),
            {DimensionRef::Kind::SketchConstraint, int(f), c, false, con.value}, size * 0.06);
      }
    } else if (const auto* ex = doc.as<ExtrudeFeature>(f)) {
      // Depth, drawn along the sketch normal from the sketch's first point.
      const auto* sf = doc.as<SketchFeature>(size_t(ex->sketch));
      if (!sf || sf->sketch.entities.empty() || std::abs(ex->distance) < kMin) continue;
      const sketch::Sketch& s = sf->sketch;
      const int p = s.entities[0].p[0];
      const gp_Pnt from = sketch::toWorld(sf->plane, s.x(p), s.y(p));
      const gp_Pnt to = from.Translated(gp_Vec(sf->plane.Direction()) * ex->distance);
      // A plane that contains the extrude direction, so the label stands up.
      const gp_Pln side(gp_Ax3(from, sf->plane.YDirection(), sf->plane.XDirection()));
      add(new PrsDim_LengthDimension(from, to, side),
          {DimensionRef::Kind::ExtrudeDepth, int(f), -1, false, std::abs(ex->distance)}, -size * 0.06);
    }
  }
  return out;
}

std::unique_ptr<Feature> applyDimension(const Document& doc, const DimensionRef& ref, double newValue,
                                        std::string& error) {
  if (newValue <= 0) {
    error = "The value must be positive.";
    return nullptr;
  }
  if (ref.kind == DimensionRef::Kind::ExtrudeDepth) {
    const auto* ex = doc.as<ExtrudeFeature>(ref.feature);
    if (!ex) return nullptr;
    auto copy = std::make_unique<ExtrudeFeature>(*ex);
    copy->distance = ex->distance < 0 ? -newValue : newValue; // keep the direction
    return copy;
  }

  const auto* sf = doc.as<SketchFeature>(ref.feature);
  if (!sf) return nullptr;
  auto copy = std::make_unique<SketchFeature>(*sf);
  sketch::Sketch& s = copy->sketch;
  if (ref.kind == DimensionRef::Kind::SketchCurve) {
    const double r = ref.diameter ? newValue / 2 : newValue;
    bool found = false;
    for (sketch::Constraint& c : s.constraints)
      if (c.kind == ConstraintKind::Radius && c.a == ref.index) c.value = r, found = true;
    if (!found) s.addConstraint({ConstraintKind::Radius, ref.index, -1, r}); // now it is a driving size
  } else {
    s.constraints[ref.index].value = newValue;
  }
  if (s.solve() == sketch::SolveResult::Failed) {
    error = "This size conflicts with the sketch's other constraints.";
    return nullptr;
  }
  return copy;
}
