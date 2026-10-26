cmake_minimum_required(VERSION 3.21)
project(OpenCaliper VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(OpenCASCADE CONFIG REQUIRED)

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# 2D sketch core (no Qt, no OCCT): shared by everything, tested on its own.
add_library(sketchcore STATIC src/sketch/Sketch.cpp)
target_include_directories(sketchcore PUBLIC src)

# Geometry and the parametric model (OCCT, no Qt).
# OCCT libraries are listed one by one: the full component lists pull in
# optional parts (e.g. TKIVtk needs VTK) that are not always installed.
add_library(cadcore STATIC
  src/sketch/SketchShape.cpp
  src/model/Document.cpp
  src/io/Exchange.cpp
)
target_include_directories(cadcore PUBLIC src ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cadcore PUBLIC
  sketchcore
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep
  TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKShHealing TKFillet TKMesh
  TKDE TKXSBase TKDESTEP TKDEIGES TKDESTL
)

add_executable(opencaliper
  src/main.cpp
  src/app/MainWindow.cpp
  src/app/Icons.cpp
  src/app/ProjectFile.cpp
  src/app/LicensesDialog.cpp
  src/app/Dimensions.cpp
  src/view/Viewer.cpp
  src/sketch/SketchEditor.cpp
)
target_link_libraries(opencaliper PRIVATE
  cadcore
  Qt6::Widgets
  Qt6::OpenGLWidgets
  TKService TKV3d TKOpenGl
)

target_compile_definitions(opencaliper PRIVATE OPENCALIPER_VERSION="${PROJECT_VERSION}")
# License texts are compiled in, so Help > Licenses works for every package type.
qt_add_resources(opencaliper licenses
  PREFIX /licenses
  FILES
    LICENSE
    THIRD-PARTY-NOTICES.md
    LICENSES/LGPL-2.1-only.txt
    LICENSES/OCCT-exception-1.0.txt
    LICENSES/LGPL-3.0-only.txt
    LICENSES/GPL-3.0-only.txt
    LICENSES/FTL.txt
)

if(WIN32)
  target_link_libraries(opencaliper PRIVATE opengl32)
  set_target_properties(opencaliper PROPERTIES WIN32_EXECUTABLE ON)
endif()

enable_testing()
add_executable(sketch_tests tests/sketch_tests.cpp)
target_link_libraries(sketch_tests PRIVATE sketchcore)
add_test(NAME sketch_tests COMMAND sketch_tests)
add_executable(model_tests tests/model_tests.cpp)
target_link_libraries(model_tests PRIVATE cadcore)
add_test(NAME model_tests COMMAND model_tests)
add_executable(project_tests tests/project_tests.cpp src/app/ProjectFile.cpp)
target_link_libraries(project_tests PRIVATE cadcore Qt6::Core)
add_test(NAME project_tests COMMAND project_tests)
add_executable(dimension_tests tests/dimension_tests.cpp src/app/Dimensions.cpp)
target_link_libraries(dimension_tests PRIVATE cadcore TKService TKV3d)
add_test(NAME dimension_tests COMMAND dimension_tests)

include(GNUInstallDirs)
install(TARGETS opencaliper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE THIRD-PARTY-NOTICES.md README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY LICENSES DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(UNIX AND NOT APPLE)
  set(APP_ID io.github.open_workshop_tools.OpenCaliper)
  install(FILES packaging/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/${APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
  install(FILES packaging/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  install(FILES packaging/${APP_ID}.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
endif()
