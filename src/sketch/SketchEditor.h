#pragma once

#include "sketch/Sketch.h"
#include "view/Viewer.h"

#include <AIS_Shape.hxx>
#include <gp_Ax3.hxx>

#include <QObject>

// Interactive 2D sketching on a plane of the 3D view.
// Left click = current tool, right click / Esc = stop, Delete = remove selection,
// middle drag = pan, wheel = zoom.
class SketchEditor : public QObject, public InputHandler {
  Q_OBJECT
public:
  enum class Tool { Select, Line, Circle, Arc };

  explicit SketchEditor(Viewer* viewer, QObject* parent = nullptr);

  void begin(const gp_Ax3& plane, sketch::Sketch initial = {});
  sketch::Sketch finish(); // leaves edit mode and hands the sketch over
  bool isActive() const { return m_active; }
  const gp_Ax3& plane() const { return m_plane; }

  void setTool(Tool tool);
  void constrain(sketch::ConstraintKind kind); // applied to the current selection
  void dimension();                           // length, radius or distance
  void toggleFix();
  void deleteSelection();

  bool mousePress(QMouseEvent*) override;
  bool mouseMove(QMouseEvent*) override;
  bool mouseRelease(QMouseEvent*) override;
  bool keyPress(QKeyEvent*) override;

signals:
  void message(const QString& text);
  void toolChanged(SketchEditor::Tool tool);

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
  bool tryConstraints(const std::vector<sketch::Constraint>& cs);
  void cancelOperation();
  void reportState();
  void redraw();
  void redrawPreview();

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

  Handle(AIS_Shape) m_aisNormal, m_aisSelected, m_aisPreview;
};
