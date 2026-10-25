#include "io/Exchange.h"

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace io {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

// Display-independent mesh for STL/OBJ: ~0.05% of the part size, 0.3 rad.
void meshForExport(const TopoDS_Shape& shape) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  const double size = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
  BRepMesh_IncrementalMesh(shape, std::max(1e-4, size * 5e-4), false, 0.3);
}

void writeObj(const TopoDS_Shape& shape, const std::string& path) {
  meshForExport(shape);
  std::ofstream out(path);
  if (!out) fail("Cannot write " + path);
  out << "# exported by OpenCaliper\n";
  int base = 1; // OBJ vertex numbers are 1-based and global
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face& face = TopoDS::Face(it.Current());
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    for (int i = 1; i <= tri->NbNodes(); ++i) {
      const gp_Pnt p = tri->Node(i).Transformed(loc.Transformation());
      out << "v " << p.X() << ' ' << p.Y() << ' ' << p.Z() << '\n';
    }
    const bool reversed = face.Orientation() == TopAbs_REVERSED; // keep normals outward
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      out << "f " << base + a - 1 << ' ' << base + b - 1 << ' ' << base + c - 1 << '\n';
    }
    base += tri->NbNodes();
  }
  if (!out) fail("Cannot write " + path);
}

} // namespace

Format formatOf(const std::string& path) {
  std::string ext = path.substr(path.find_last_of('.') + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (ext == "step" || ext == "stp") return Format::Step;
  if (ext == "iges" || ext == "igs") return Format::Iges;
  if (ext == "stl") return Format::Stl;
  if (ext == "obj") return Format::Obj;
  if (ext == "brep") return Format::Brep;
  fail("Unknown file type: ." + ext);
}

void exportShape(const TopoDS_Shape& shape, const std::string& path) {
  if (shape.IsNull()) fail("There is nothing to export yet.");
  switch (formatOf(path)) {
  case Format::Step: {
    STEPControl_Writer writer;
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone || writer.Write(path.c_str()) != IFSelect_RetDone)
      fail("STEP export failed.");
    break;
  }
  case Format::Iges: {
    IGESControl_Controller::Init();
    IGESControl_Writer writer("MM", 1); // 1 = B-Rep mode (solids, not just surfaces)
    writer.AddShape(shape);
    writer.ComputeModel();
    if (!writer.Write(path.c_str())) fail("IGES export failed.");
    break;
  }
  case Format::Stl: {
    meshForExport(shape);
    StlAPI_Writer writer;
    writer.ASCIIMode() = false; // binary: ~5x smaller
    if (!writer.Write(shape, path.c_str())) fail("STL export failed.");
    break;
  }
  case Format::Obj:
    writeObj(shape, path);
    break;
  case Format::Brep:
    if (!BRepTools::Write(shape, path.c_str())) fail("BREP export failed.");
    break;
  }
}

TopoDS_Shape importShape(const std::string& path) {
  switch (formatOf(path)) {
  case Format::Step: {
    STEPControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) fail("Cannot read STEP file.");
    reader.TransferRoots();
    return reader.OneShape();
  }
  case Format::Iges: {
    IGESControl_Controller::Init();
    IGESControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) fail("Cannot read IGES file.");
    reader.TransferRoots();
    return reader.OneShape();
  }
  case Format::Brep: {
    TopoDS_Shape shape;
    if (!BRepTools::Read(shape, path.c_str(), BRep_Builder())) fail("Cannot read BREP file.");
    return shape;
  }
  case Format::Stl:
  case Format::Obj:
    break;
  }
  fail("STL/OBJ are meshes and cannot be imported as solids.");
}

} // namespace io
