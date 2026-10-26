#pragma once

#include "app/Dimensions.h"
#include "model/Document.h"
#include "sketch/SketchEditor.h"

#include <AIS_Shape.hxx>
#include <TopoDS_Face.hxx>

#include <QMainWindow>

#include <map>
#include <memory>
#include <vector>

class QAction;
class QLabel;
class QListWidget;
class QLockFile;
class QMenu;
class QToolBar;
class Viewer;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  Viewer* viewer() const { return m_viewer; }
  bool openProject(const QString& path);
  // Offers work left behind by a session that crashed; call once at startup.
  void recoverAutosave();

protected:
  void closeEvent(QCloseEvent* e) override;

private:
  // UI construction
  QAction* action(const QString& text, const QString& icon, const QKeySequence& key, const QString& tip = {});
  void buildFileMenu();
  void buildEditMenu();
  void buildViewMenu();
  void buildSketchUi();
  void buildModelUi();
  void buildTimeline();
  void buildHelpMenu();

  // sketches
  void startSketch(const gp_Ax3& plane);
  void sketchOnFace();
  void finishSketch();
  int chosenSketch() const; // selected sketch in the view, else the newest unused one

  // features
  void extrude(int editIndex = -1);
  void revolve(int editIndex = -1);
  void filletOrChamfer(bool fillet, int editIndex = -1);
  void shell(int editIndex = -1);
  void pattern(int editIndex = -1);
  void hole(int editIndex = -1);
  void mirror(int editIndex = -1);
  void editParameters();
  void measure();
  void showProperties();
  void editFeature(int index);
  void deleteFeature(int index);
  std::vector<int> selectedEdges() const;
  TopoDS_Face selectedFace() const;
  std::vector<int> selectedFaces() const;
  std::vector<TopoDS_Shape> selectedShapes() const; // picked vertices, edges, faces of the body
  // Keys of the body's edges/faces with these indices, to find them again later.
  std::vector<model::ShapeKey> keysOf(const std::vector<int>& indices, TopAbs_ShapeEnum kind) const;

  // document changes: every change goes through these, for undo
  bool addFeature(std::unique_ptr<model::Feature> feature);
  bool replaceFeature(int index, std::unique_ptr<model::Feature> feature);
  void pushUndo();
  void rollback();
  void documentChanged();
  void undo();
  void redo();

  // files
  bool confirmDiscard();
  void newProject();
  void openProjectDialog();
  bool save();
  bool saveAs();
  void importFile();
  void exportFile();
  void setProjectPath(const QString& path);
  void addRecent(const QString& path);
  void fillRecentMenu();
  void autosave();
  void clearAutosave();

  // display
  void refreshScene();
  void clearDimensions();
  void editDimension(); // the dimension under the mouse (double-click)
  void refreshTimeline();
  void updateActions();
  void hint(const QString& text);
  QString describe(size_t index) const;

  Viewer* m_viewer = nullptr;
  SketchEditor* m_editor = nullptr;
  model::Document m_doc;
  std::vector<model::Document> m_undo, m_redo;
  QString m_path;
  int m_editingSketch = -1; // history index of the sketch being edited, -1 = new sketch

  QList<QAction*> m_modelActions; // enabled outside sketch mode
  QList<QAction*> m_sketchActions; // enabled inside sketch mode
  std::map<SketchEditor::Tool, QAction*> m_toolActions;
  QAction* m_undoAction = nullptr;
  QAction* m_redoAction = nullptr;
  QToolBar* m_drawBar = nullptr;
  QToolBar* m_constraintBar = nullptr;
  QListWidget* m_timeline = nullptr;
  QLabel* m_hint = nullptr;
  QMenu* m_recentMenu = nullptr;
  QString m_autosavePath; // this window's crash backup
  std::unique_ptr<QLockFile> m_autosaveLock; // held while running: a backup without a holder is left from a crash

  Handle(AIS_Shape) m_bodyAis;
  std::vector<std::pair<Handle(AIS_Shape), int>> m_sketchAis; // shown sketch -> history index
  DimensionList m_dimensions;
  bool m_showDimensions = true;
};
