# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Generates the inputs for the node-api libraries from a file that lists one
# Node-API function per line.

function(hermes_read_node_api_symbols symbols_file out_var)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${symbols_file})
  file(STRINGS ${symbols_file} symbols)
  set(${out_var} ${symbols} PARENT_SCOPE)
endfunction()

# Writes an ld64 -reexported_symbols_list.
function(hermes_write_node_api_reexports symbols_file output)
  hermes_read_node_api_symbols(${symbols_file} symbols)
  list(TRANSFORM symbols PREPEND "_")
  list(JOIN symbols "\n" content)
  file(CONFIGURE OUTPUT ${output} CONTENT "${content}\n")
endfunction()

# Writes C source that references every function without defining any.
function(hermes_write_node_api_refs_source symbols_file output)
  hermes_read_node_api_symbols(${symbols_file} symbols)
  set(decls "")
  set(refs "")
  foreach(symbol ${symbols})
    string(APPEND decls "void ${symbol}(void);\n")
    string(APPEND refs "  ${symbol},\n")
  endforeach()
  # retain keeps the array through --gc-sections, since nothing reads it.
  file(CONFIGURE OUTPUT ${output} CONTENT
    "${decls}\n__attribute__((used, retain)) static void (*const refs[])(void) = {\n${refs}};\n")
endfunction()

# Writes C source that defines every function with a body that traps.
function(hermes_write_node_api_stub_source symbols_file output)
  hermes_read_node_api_symbols(${symbols_file} symbols)
  set(stubs "")
  foreach(symbol ${symbols})
    string(APPEND stubs
      "__attribute__((visibility(\"default\"))) void ${symbol}(void) { __builtin_trap(); }\n")
  endforeach()
  file(CONFIGURE OUTPUT ${output} CONTENT "${stubs}")
endfunction()
