#pragma once

#include <TopoDS_Shape.hxx>

#include <string>

// CAD/mesh file import and export (OCCT, no Qt). Paths are UTF-8.
// Everything throws std::runtime_error with a user-facing message on failure.
namespace io {

enum class Format { Step, Iges, Stl, Obj, Brep };

// From the file extension (.step/.stp, .iges/.igs, .stl, .obj, .brep); throws if unknown.
Format formatOf(const std::string& path);

void exportShape(const TopoDS_Shape& shape, const std::string& path);
// STEP, IGES or BREP. Meshes (STL/OBJ) are not imported: they are not solids.
TopoDS_Shape importShape(const std::string& path);

} // namespace io
