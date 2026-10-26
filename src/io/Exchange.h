#pragma once

#include <TopoDS_Shape.hxx>

#include <string>

// CAD/mesh file import and export (OCCT, no Qt). Paths are UTF-8.
// Everything throws std::runtime_error with a user-facing message on failure.
namespace io {

enum class Format { Step, Iges, Stl, Obj, ThreeMf, Brep };

// How finely curved faces are cut into triangles for STL, OBJ and 3MF.
enum class MeshQuality { Coarse, Normal, Fine };

// From the file extension (.step/.stp, .iges/.igs, .stl, .obj, .3mf, .brep); throws if unknown.
Format formatOf(const std::string& path);
bool isMesh(Format f);

void exportShape(const TopoDS_Shape& shape, const std::string& path, MeshQuality quality = MeshQuality::Normal);
// STEP, IGES or BREP. Meshes (STL/OBJ/3MF) are not imported: they are not solids.
TopoDS_Shape importShape(const std::string& path);

} // namespace io
