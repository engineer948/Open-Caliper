#pragma once

#include "model/Document.h"

#include <PrsDim_Dimension.hxx>

#include <memory>
#include <string>
#include <utility>
#include <vector>

// 3D dimension labels for the values that drive the model: hole and circle
// diameters, arc radii, sketch lengths/distances and extrude depths.
// Double-clicking one in the view edits that value (see MainWindow).
struct DimensionRef {
  enum class Kind { SketchCurve, SketchConstraint, ExtrudeDepth };
  Kind kind;
  int feature = -1; // history index
  int index = -1;   // entity (SketchCurve) or constraint (SketchConstraint)
  bool diameter = false; // value is shown and edited as a diameter
  double value = 0;      // what the label shows
};

using DimensionList = std::vector<std::pair<Handle(PrsDim_Dimension), DimensionRef>>;

DimensionList buildDimensions(const model::Document& doc);

// The feature that replaces ref.feature with `newValue` applied, or null and
// `error` set (for example when the new size conflicts with other constraints).
std::unique_ptr<model::Feature> applyDimension(const model::Document& doc, const DimensionRef& ref,
                                               double newValue, std::string& error);
