#pragma once

#include "model/Expression.h"
#include "sketch/Sketch.h"
#include "view/Viewer.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <gp_Ax3.hxx>

#include <QObject>

#include <functional>

// Interactive 2D sketching on a plane of the 3D view.
// Left click = current tool, right click / Esc = stop, Delete = remove selection,
// middle drag = pan, wheel = zoom.
class SketchEditor : public QObject, public InputHandler {
  Q_OBJECT
public:
  enum class Tool { Select, Line, Rectangle, Circle, Arc, Polygon, Slot, Trim, Fillet };

  explicit SketchEditor(Viewer* viewer, QObject* parent = nullptr);

  void begin(const gp_Ax3& plane, sketch::Sketch initial = {});
  sketch::Sketch finish(); // leaves edit mode and hands the sketch over
  bool isActive() const { return m_active; }
  const gp_Ax3& plane() const { return m_plane; }

  void setTool(Tool tool);
  void constrain(sketch::ConstraintKind kind); // applied to the current selection
  void dimension();                           // length, radius or distance
  void toggleFix();
  void toggleConstruction(); // helper geometry: shown dashed, never becomes a solid
  void offsetSelection();
  void mirrorSelection(); // across the last selected line
  void deleteSelection();
  // Parameter values for sizes typed as expressions ("width / 2").
  void setVariables(model::Variables vars) { m_vars = std::move(vars); }
  void setPolygonSides(int sides) { m_polygonSides = sides; }
  int polygonSides() const { return m_polygonSides; }

  // Undo inside the sketch (the model's own undo covers whole sketches).
  bool canUndo() const { return !m_undo.empty(); }
  bool canRedo() const { return !m_redo.empty(); }
  void undo();
  void redo();

  bool mousePress(QMouseEvent*) override;
  bool mouseMove(QMouseEvent*) override;
  bool mouseRelease(QMouseEvent*) override;
  bool keyPress(QKeyEvent*) override;

signals:
  void message(const QString& text);
  void toolChanged(SketchEditor::Tool tool);
  void historyChanged(); // canUndo()/canRedo() may have changed

private:
  struct Pick {
    double u = 0, v = 0;
    int point = -1; // >= 0 when snapped onto an existing point
  };
  struct Item {
    bool isPoint = false;
    int index = -1;
    bool operator==(const Item&) const = default;
  };
  struct Selected { // selection split by type
    std::vector<int> points, lines, circles;
    bool only(size_t p, size_t l, size_t c) const {
      return points.size() == p && lines.size() == l && circles.size() == c;
    }
  };

  bool pickAt(const QPointF& pos, bool snap, Pick& out) const;
  int resolve(const Pick& p); // the snapped point, or a new one
  Item hitTest(double u, double v) const;
  Selected selected() const;
  std::vector<int> usedPoints() const;

  void toolClick(const Pick& p);
  void addRectangle(const Pick& a, const Pick& b);
  void addPolygon(const Pick& center, const Pick& corner);
  void addSlot(const Pick& c1, const Pick& c2, const Pick& side);
  void remember(); // snapshot for undo, before a change
  void forget();   // drop that snapshot again (the change was refused)
  // Runs an edit as one undo step; restores the sketch if it fails or cannot be solved.
  bool applyEdit(const std::function<bool()>& edit, const QString& failure);
  // Asks for a size; accepts numbers and parameter expressions.
  bool askValue(const QString& title, const QString& label, double& value, std::string& expr, bool positive = true);
  bool tryConstraints(const std::vector<sketch::Constraint>& cs);
  void cancelOperation();
  void reportState();
  void redraw();
  void redrawPreview();
  void redrawLabels();

  Viewer* m_viewer;
  gp_Ax3 m_plane;
  bool m_active = false;
  Tool m_tool = Tool::Line;
  sketch::Sketch m_sketch;

  std::vector<Pick> m_picks; // clicks of the operation in progress
  Pick m_hover;
  bool m_hoverValid = false;
  std::vector<Item> m_selection;
  int m_dragPoint = -1;
  bool m_dragRemembered = false;
  int m_polygonSides = 6;
  double m_filletRadius = 2, m_offsetDistance = 2;
  int m_hoverEntity = -1; // trim tool: the curve under the mouse
  model::Variables m_vars;
  std::vector<sketch::Sketch> m_undo, m_redo;

  Handle(AIS_Shape) m_aisNormal, m_aisConstruction, m_aisSelected, m_aisPreview;
  std::vector<Handle(AIS_TextLabel)> m_labels; // constraint glyphs and sizes
};
