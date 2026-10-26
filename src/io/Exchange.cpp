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
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace io {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

// Display-independent mesh for STL/OBJ/3MF. Normal: deviation ~0.05% of the
// part size, 0.3 rad between neighbouring facets.
void meshForExport(const TopoDS_Shape& shape, MeshQuality quality) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  const double size = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
  double deflection = 5e-4, angle = 0.3;
  if (quality == MeshQuality::Coarse) deflection = 2e-3, angle = 0.5;
  if (quality == MeshQuality::Fine) deflection = 1e-4, angle = 0.1;
  BRepTools::Clean(shape); // drop a mesh made earlier with other settings
  BRepMesh_IncrementalMesh(shape, std::max(1e-4, size * deflection), false, angle);
}

// One triangle mesh with shared vertices: faces meet on identical edge nodes,
// so merging equal coordinates gives a closed (watertight) mesh.
struct Mesh {
  std::vector<gp_Pnt> nodes;
  std::vector<std::array<int, 3>> triangles; // 0-based, counter-clockwise seen from outside
};

Mesh collectMesh(const TopoDS_Shape& shape) {
  Mesh mesh;
  std::map<std::tuple<double, double, double>, int> index;
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face& face = TopoDS::Face(it.Current());
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    std::vector<int> local(tri->NbNodes() + 1);
    for (int i = 1; i <= tri->NbNodes(); ++i) {
      const gp_Pnt p = tri->Node(i).Transformed(loc.Transformation());
      const auto [pos, added] = index.try_emplace({p.X(), p.Y(), p.Z()}, int(mesh.nodes.size()));
      if (added) mesh.nodes.push_back(p);
      local[i] = pos->second;
    }
    const bool reversed = face.Orientation() == TopAbs_REVERSED; // keep normals outward
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      if (local[a] == local[b] || local[b] == local[c] || local[a] == local[c]) continue; // degenerate
      mesh.triangles.push_back({local[a], local[b], local[c]});
    }
  }
  return mesh;
}

// --- 3MF: an OPC zip with three small XML files ---------------------------------

uint32_t crc32(const std::string& data) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// Minimal zip writer: entries stored without compression (allowed by 3MF).
class ZipWriter {
public:
  void add(const std::string& name, const std::string& data) {
    Entry e{name, crc32(data), uint32_t(data.size()), uint32_t(m_out.size())};
    put32(0x04034b50);
    put16(20); // version needed
    put16(0);  // flags
    put16(0);  // stored
    put16(0);  // time
    put16(0x21); // date: 1980-01-01
    put32(e.crc);
    put32(e.size);
    put32(e.size);
    put16(uint16_t(name.size()));
    put16(0);
    m_out += name;
    m_out += data;
    m_entries.push_back(e);
  }
  std::string finish() {
    const uint32_t start = uint32_t(m_out.size());
    for (const Entry& e : m_entries) {
      put32(0x02014b50);
      put16(20);
      put16(20);
      put16(0);
      put16(0);
      put16(0);
      put16(0x21);
      put32(e.crc);
      put32(e.size);
      put32(e.size);
      put16(uint16_t(e.name.size()));
      put16(0);
      put16(0);
      put16(0);
      put16(0);
      put32(0);
      put32(e.offset);
      m_out += e.name;
    }
    const uint32_t size = uint32_t(m_out.size()) - start;
    put32(0x06054b50);
    put16(0);
    put16(0);
    put16(uint16_t(m_entries.size()));
    put16(uint16_t(m_entries.size()));
    put32(size);
    put32(start);
    put16(0);
    return std::move(m_out);
  }

private:
  struct Entry {
    std::string name;
    uint32_t crc, size, offset;
  };
  void put16(uint16_t v) { m_out += char(v & 0xFF), m_out += char(v >> 8); }
  void put32(uint32_t v) { put16(uint16_t(v & 0xFFFF)), put16(uint16_t(v >> 16)); }
  std::string m_out;
  std::vector<Entry> m_entries;
};

void write3mf(const TopoDS_Shape& shape, const std::string& path, MeshQuality quality) {
  meshForExport(shape, quality);
  const Mesh mesh = collectMesh(shape);
  if (mesh.triangles.empty()) fail("3MF export failed: the part has no faces.");
  std::ostringstream model;
  model.imbue(std::locale::classic());
  model.precision(9);
  model << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<model unit=\"millimeter\" xml:lang=\"en-US\" "
           "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
        << " <metadata name=\"Application\">OpenCaliper</metadata>\n"
        << " <resources>\n  <object id=\"1\" type=\"model\">\n   <mesh>\n    <vertices>\n";
  for (const gp_Pnt& p : mesh.nodes)
    model << "     <vertex x=\"" << p.X() << "\" y=\"" << p.Y() << "\" z=\"" << p.Z() << "\"/>\n";
  model << "    </vertices>\n    <triangles>\n";
  for (const auto& t : mesh.triangles)
    model << "     <triangle v1=\"" << t[0] << "\" v2=\"" << t[1] << "\" v3=\"" << t[2] << "\"/>\n";
  model << "    </triangles>\n   </mesh>\n  </object>\n </resources>\n"
        << " <build>\n  <item objectid=\"1\"/>\n </build>\n</model>\n";

  ZipWriter zip;
  zip.add("[Content_Types].xml",
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
          " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
          " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
          "</Types>\n");
  zip.add("_rels/.rels",
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
          " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
          "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
          "</Relationships>\n");
  zip.add("3D/3dmodel.model", model.str());
  const std::string data = zip.finish();
  std::ofstream out(path, std::ios::binary);
  if (!out || !out.write(data.data(), std::streamsize(data.size()))) fail("Cannot write " + path);
}

void writeObj(const TopoDS_Shape& shape, const std::string& path, MeshQuality quality) {
  meshForExport(shape, quality);
  std::ofstream out(path);
  if (!out) fail("Cannot write " + path);
  out.imbue(std::locale::classic()); // '.' decimal point whatever the language
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
  if (ext == "3mf") return Format::ThreeMf;
  if (ext == "brep") return Format::Brep;
  fail("Unknown file type: ." + ext);
}

bool isMesh(Format f) { return f == Format::Stl || f == Format::Obj || f == Format::ThreeMf; }

void exportShape(const TopoDS_Shape& shape, const std::string& path, MeshQuality quality) {
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
    meshForExport(shape, quality);
    StlAPI_Writer writer;
    writer.ASCIIMode() = false; // binary: ~5x smaller
    if (!writer.Write(shape, path.c_str())) fail("STL export failed.");
    break;
  }
  case Format::Obj:
    writeObj(shape, path, quality);
    break;
  case Format::ThreeMf:
    write3mf(shape, path, quality);
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
  case Format::ThreeMf:
    break;
  }
  fail("STL/OBJ/3MF are meshes and cannot be imported as solids.");
}

} // namespace io
