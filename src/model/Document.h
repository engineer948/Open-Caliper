#pragma once

#include "model/Expression.h"
#include "sketch/Sketch.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_XYZ.hxx>

#include <map>
#include <memory>
#include <string>
#include <vector>

// Parametric model: an ordered timeline of features (plan: no DAG, a list).
// Changing a feature rebuilds it and everything after it.
namespace model {

class Document;

class Feature {
public:
  virtual ~Feature() = default;
  virtual std::unique_ptr<Feature> clone() const = 0;
  virtual std::string name() const = 0;
  // Body after this feature, given the body before it.
  // Throws std::runtime_error with a user-facing message on failure.
  virtual TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const = 0;
  // History index of the feature this one is built from (a sketch, or the
  // patterned feature for a pattern), -1 if none. Deleting that feature
  // deletes this one too.
  virtual int sketchInput() const { return -1; }
  virtual void setSketchInput(int) {}

  // Sizes given as parameter expressions ("width / 2"), by field name; the
  // document evaluates them into the fields before each build.
  std::map<std::string, std::string> expressions;
  // Sets the numeric field `field`; false if this feature has none by that name.
  virtual bool setValue(const std::string& /*field*/, double /*value*/) { return false; }
};

// CRTP helper for clone().
template <class T> class FeatureOf : public Feature {
public:
  std::unique_ptr<Feature> clone() const override { return std::make_unique<T>(static_cast<const T&>(*this)); }
};

// Where a picked edge or face was, so it can be found again after an earlier
// step changed the body and the edge/face numbering with it.
struct ShapeKey {
  int type = -1;               // GeomAbs_CurveType or GeomAbs_SurfaceType; -1 = no key
  gp_XYZ point;                // edge midpoint / face centre
  gp_XYZ dir;                  // line direction, circle or cylinder axis, plane normal (zero if none)
  double size = 0;             // circle/cylinder radius, else length/area
  std::vector<gp_XYZ> normals; // edges: outward normals of the flat faces next to it
  bool valid() const { return type >= 0; }
};
ShapeKey shapeKey(const TopoDS_Shape& body, const TopoDS_Shape& sub);
// The edge or face of `body` that best matches `key`; null if nothing fits.
TopoDS_Shape findShape(const TopoDS_Shape& body, const ShapeKey& key, TopAbs_ShapeEnum kind);

class SketchFeature : public FeatureOf<SketchFeature> {
public:
  gp_Ax3 plane;
  sketch::Sketch sketch;

  std::string name() const override { return "Sketch"; }
  TopoDS_Shape build(const Document&, const TopoDS_Shape& body) const override { return body; }

  TopoDS_Shape edges() const;
  // Closed regions as planar faces; loops inside loops become holes. Throws if none.
  TopoDS_Shape profile() const;
};

enum class Operation { Join, Cut, Intersect };

class ExtrudeFeature : public FeatureOf<ExtrudeFeature> {
public:
  int sketch = -1;
  double distance = 10; // along the sketch normal; negative goes the other way
  Operation op = Operation::Join;

  std::string name() const override { return "Extrude"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  TopoDS_Shape tool(const Document& doc) const; // the extruded solid on its own
  int sketchInput() const override { return sketch; }
  void setSketchInput(int s) override { sketch = s; }
  bool setValue(const std::string& f, double v) override { return f == "distance" && (distance = v, true); }
};

class RevolveFeature : public FeatureOf<RevolveFeature> {
public:
  enum class Axis { SketchX, SketchY };
  int sketch = -1;
  Axis axis = Axis::SketchY;
  double angle = 360; // degrees
  Operation op = Operation::Join;

  std::string name() const override { return "Revolve"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  TopoDS_Shape tool(const Document& doc) const; // the revolved solid on its own
  int sketchInput() const override { return sketch; }
  void setSketchInput(int s) override { sketch = s; }
  bool setValue(const std::string& f, double v) override { return f == "angle" && (angle = v, true); }
};

// Edges are stored by their index in the body's edge map (TopExp::MapShapes)
// and by a ShapeKey. The key finds the edge again when an earlier change
// renumbers the edges; files from 0.3 and older only have the index.
class FilletFeature : public FeatureOf<FilletFeature> {
public:
  std::vector<int> edges;
  std::vector<ShapeKey> keys; // one per edge, or empty
  double radius = 1;

  std::string name() const override { return "Fillet"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  bool setValue(const std::string& f, double v) override { return f == "radius" && (radius = v, true); }
};

class ChamferFeature : public FeatureOf<ChamferFeature> {
public:
  std::vector<int> edges;
  std::vector<ShapeKey> keys;
  double distance = 1;

  std::string name() const override { return "Chamfer"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  bool setValue(const std::string& f, double v) override { return f == "distance" && (distance = v, true); }
};

// Hollows the body out to a wall `thickness`, leaving the selected faces open
// (faces stored like fillet edges: index plus key).
class ShellFeature : public FeatureOf<ShellFeature> {
public:
  std::vector<int> faces;
  std::vector<ShapeKey> keys;
  double thickness = 2;

  std::string name() const override { return "Shell"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  bool setValue(const std::string& f, double v) override { return f == "thickness" && (thickness = v, true); }
};

// Drilled holes at the centre of every circle of a sketch, into the body
// (against the sketch normal, or along it when flipped). Simple, counterbored
// for socket head screws, or countersunk for flat head screws.
class HoleFeature : public FeatureOf<HoleFeature> {
public:
  enum class Kind { Simple, Counterbore, Countersink };
  int sketch = -1;
  Kind kind = Kind::Simple;
  double diameter = 3.4;
  double depth = 0; // 0 = through all
  double boreDiameter = 6.5, boreDepth = 3.3; // counterbore
  double sinkDiameter = 6.3, sinkAngle = 90;  // countersink
  bool flip = false;

  std::string name() const override { return "Hole"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  TopoDS_Shape tool(const Document& doc, const TopoDS_Shape& body) const; // the drill shape on its own
  int sketchInput() const override { return sketch; }
  void setSketchInput(int s) override { sketch = s; }
  bool setValue(const std::string& f, double v) override;
};

// Mirrors the whole body across an origin plane or a flat face and joins the copy.
class MirrorFeature : public FeatureOf<MirrorFeature> {
public:
  enum class Plane { XY, XZ, YZ, Face };
  Plane plane = Plane::YZ;
  int face = -1; // Plane::Face: face index and key, as with shells
  ShapeKey key;

  std::string name() const override { return "Mirror"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
};

// Repeats an extrude, revolve or hole: `count` copies in total (the original
// included), in a row along a world axis or around a world axis through the origin.
class PatternFeature : public FeatureOf<PatternFeature> {
public:
  enum class Kind { Linear, Circular };
  enum class Axis { X, Y, Z };
  int source = -1; // history index of the extrude/revolve/hole to repeat
  Kind kind = Kind::Linear;
  Axis axis = Axis::X;
  int count = 3;
  double spacing = 20; // linear: distance between copies (mm)
  double angle = 360;  // circular: total angle the copies are spread over (deg)

  std::string name() const override { return "Pattern"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
  int sketchInput() const override { return source; }
  void setSketchInput(int s) override { source = s; }
  bool setValue(const std::string& f, double v) override;
};

// A shape from a STEP/IGES/BREP file, joined to the body.
class ImportFeature : public FeatureOf<ImportFeature> {
public:
  TopoDS_Shape shape;
  std::string source; // file name, for display

  std::string name() const override { return "Import"; }
  TopoDS_Shape build(const Document& doc, const TopoDS_Shape& body) const override;
};

// 1-based index of `edge` in `body`'s edge map, 0 if not found.
int edgeIndex(const TopoDS_Shape& body, const TopoDS_Shape& edge);
// Same for faces.
int faceIndex(const TopoDS_Shape& body, const TopoDS_Shape& face);

struct MassProperties {
  double volume = 0; // mm^3
  double area = 0;   // mm^2
  gp_Pnt center;     // centre of mass
  gp_XYZ size;       // bounding box extents along X, Y, Z
};
MassProperties massProperties(const TopoDS_Shape& shape);

// Sketch plane on a flat face: normal out of the material, origin where the
// world origin projects onto it, X kept horizontal on walls. False if not flat.
bool sketchPlaneOnFace(const TopoDS_Shape& face, gp_Ax3& plane);

// Human-readable measurement of one or two picked sub-shapes (vertex, edge,
// face): lengths, areas, diameters, distances and angles. Empty if nothing fits.
std::string measure(const std::vector<TopoDS_Shape>& picked);

class Document {
public:
  Document() = default;
  Document(const Document& other);
  Document& operator=(const Document& other);
  Document(Document&&) = default;
  Document& operator=(Document&&) = default;

  // Appends and builds the feature; returns its index. Check error() afterwards.
  int add(std::unique_ptr<Feature> feature);
  void removeLast();
  // Replaces feature i and rebuilds from there.
  void replace(size_t i, std::unique_ptr<Feature> feature);
  // Removes feature i and every feature built from it (recursively); returns how many.
  int removeWithDependents(size_t i);
  void regenerate(size_t from = 0);

  size_t size() const { return m_history.size(); }
  const Feature& feature(size_t i) const { return *m_history[i]; }
  template <class T> const T* as(size_t i) const {
    return i < m_history.size() ? dynamic_cast<const T*>(m_history[i].get()) : nullptr;
  }
  TopoDS_Shape body() const { return m_bodies.empty() ? TopoDS_Shape() : m_bodies.back(); }
  // Body as it was right before feature i (what i is built on).
  TopoDS_Shape bodyBefore(size_t i) const { return i == 0 ? TopoDS_Shape() : m_bodies[i - 1]; }
  const std::string& error(size_t i) const { return m_errors[i]; }
  bool hasErrors() const;
  bool isConsumed(size_t sketchIndex) const; // used by some later feature

  // User parameters. Changing them rebuilds everything.
  const std::vector<Parameter>& parameters() const { return m_parameters; }
  void setParameters(std::vector<Parameter> params);
  // Current values; empty with parameterError() set when they do not evaluate.
  const Variables& values() const { return m_values; }
  const std::string& parameterError() const { return m_parameterError; }
  // Evaluates an expression with the current parameter values (throws).
  double evaluate(const std::string& expression) const { return model::evaluate(expression, m_values); }

private:
  void applyExpressions(Feature& f) const; // throws
  void evaluateParameters();

  std::vector<Parameter> m_parameters;
  Variables m_values;
  std::string m_parameterError;
  std::vector<std::unique_ptr<Feature>> m_history;
  std::vector<TopoDS_Shape> m_bodies; // body after each feature (cache)
  std::vector<std::string> m_errors;  // empty when the feature built fine
};

} // namespace model
