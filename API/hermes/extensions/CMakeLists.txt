# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# This CMakeLists.txt generates precompiled bytecode for JSI extensions.
# The bytecode is embedded as a C array and loaded at runtime.

if(IMPORT_HOST_COMPILERS)
  file(TO_CMAKE_PATH "${IMPORT_HOST_COMPILERS}" IMPORT_HOST_COMPILERS_CMAKE)
  include(${IMPORT_HOST_COMPILERS_CMAKE})
  set(hermesc_EXE imported-hermesc)
else()
  set(hermesc_EXE ${HERMES_TOOLS_OUTPUT_DIR}/hermesc)
endif()

# Concatenate all JS files into one source file for compilation.
# This way there is only one RuntimeModule made for them.
if(NOT CMAKE_HOST_WIN32)
  set(concatenate "cat")
else()
  set(concatenate "type")
endif()

# Header and footer are handled explicitly to ensure correct ordering.
# Extension JS files are globbed and sorted between them.
set(header_js "${CMAKE_CURRENT_SOURCE_DIR}/00-header.js")
set(footer_js "${CMAKE_CURRENT_SOURCE_DIR}/99-footer.js")

# Glob core extension JS files (excluding header 00-* and footer 99-*).
file(GLOB core_extensions_js "${CMAKE_CURRENT_SOURCE_DIR}/[1-8]*.js")

# Optionally include contrib extensions.
# Contrib JS files are appended after core extensions due to absolute path sorting
# (contrib/ sorts after numeric prefixes). Within each group, files are sorted
# by their numeric prefix.
if(HERMES_ENABLE_CONTRIB_EXTENSIONS)
  add_subdirectory(contrib)
  file(GLOB contrib_extensions_js "${CMAKE_CURRENT_SOURCE_DIR}/contrib/*.js")
  list(APPEND core_extensions_js ${contrib_extensions_js})
endif()

# Sort extension files. Due to absolute path sorting, core extensions come first,
# then contrib extensions. This ordering is intentional.
list(SORT core_extensions_js)

# Build final list: header + sorted extensions + footer
set(extensions_js_sources ${header_js} ${core_extensions_js} ${footer_js})

# Convert path from cmake style (`/`) to native style (`\` on Win and `/` elsewhere) for Win.
set(extensions_js_sources_native_path "")
foreach(cmake_path IN LISTS extensions_js_sources)
  file(TO_NATIVE_PATH ${cmake_path} native_path)
  list(APPEND extensions_js_sources_native_path ${native_path})
endforeach()

add_custom_command(
  OUTPUT ExtensionsBytecode.js
  COMMAND ${concatenate} ${extensions_js_sources_native_path} > ${CMAKE_CURRENT_BINARY_DIR}/ExtensionsBytecode.js
  DEPENDS ${extensions_js_sources}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  VERBATIM
)

set(JS_COMPILER_FLAGS "-O" "-Wno-undefined-variable" "-g0")

add_custom_command(
  OUTPUT ExtensionsBytecode.hbc
  COMMAND ${hermesc_EXE} ${JS_COMPILER_FLAGS} -emit-binary -out=${CMAKE_CURRENT_BINARY_DIR}/ExtensionsBytecode.hbc ${CMAKE_CURRENT_BINARY_DIR}/ExtensionsBytecode.js
  DEPENDS ${hermesc_EXE} ExtensionsBytecode.js
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  VERBATIM
)

# ExtensionsBytecode.inc is the hex dump of the compiled bytecode artifact that can be included
# in ExtensionsBytecode.cpp as C/C++ hex literal to initialize a byte array.
add_custom_command(
  OUTPUT ExtensionsBytecode.inc
  COMMAND ${Python_EXECUTABLE} ${PROJECT_SOURCE_DIR}/lib/InternalJavaScript/xxd.py ${CMAKE_CURRENT_BINARY_DIR}/ExtensionsBytecode.hbc > ${CMAKE_CURRENT_BINARY_DIR}/ExtensionsBytecode.inc
  DEPENDS ${PROJECT_SOURCE_DIR}/lib/InternalJavaScript/xxd.py ExtensionsBytecode.hbc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  VERBATIM
)

add_custom_target(ExtensionsBytecodeInclude ALL DEPENDS ExtensionsBytecode.inc)

# Export C++ sources to parent for linking.
set(EXTENSIONS_CPP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/Extensions.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ExtensionsBytecode.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Intrinsics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/JSIUtils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Crypto.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Dummy.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/TextEncoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/Worker.cpp
)

# Append contrib sources if enabled (set by contrib/CMakeLists.txt).
if(HERMES_ENABLE_CONTRIB_EXTENSIONS)
  list(APPEND EXTENSIONS_CPP_SOURCES ${CONTRIB_EXTENSIONS_CPP_SOURCES})
endif()

set(EXTENSIONS_CPP_SOURCES ${EXTENSIONS_CPP_SOURCES} PARENT_SCOPE)
