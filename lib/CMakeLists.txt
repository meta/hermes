# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

hermes_enable_werror_undef()

add_hermes_library(hermesOptimizer
  Optimizer/PassManager/PassManager.cpp
  Optimizer/PassManager/Pipeline.cpp
  Optimizer/Scalar/SimplifyCFG.cpp
  Optimizer/Scalar/CSE.cpp
  Optimizer/Scalar/CodeMotion.cpp
  Optimizer/Scalar/DCE.cpp
  Optimizer/Scalar/MetroRequireOpt.cpp
  Optimizer/Scalar/LowerGeneratorFunction.cpp
  Optimizer/Scalar/Mem2Reg.cpp
  Optimizer/Scalar/SimpleMem2Reg.cpp
  Optimizer/Scalar/FrameLoadStoreOpts.cpp
  Optimizer/Scalar/LowerBuiltinCalls.cpp
  Optimizer/Scalar/ScopeElimination.cpp
  Optimizer/Scalar/ScopeHoisting.cpp
  Optimizer/Scalar/TypeInference.cpp
  Optimizer/Scalar/SimpleStackPromotion.cpp
  Optimizer/Scalar/ObjectMergeNewStores.cpp
  Optimizer/Scalar/ObjectStackPromotion.cpp
  Optimizer/Scalar/InstSimplify.cpp
  Optimizer/Scalar/Auditor.cpp
  Optimizer/Scalar/ResolveStaticRequire.cpp
  Optimizer/Scalar/FuncSigOpts.cpp
  Optimizer/Scalar/Utils.cpp
  Optimizer/Scalar/FunctionAnalysis.cpp
  Optimizer/Scalar/Inlining.cpp
  Optimizer/Scalar/InstructionEscapeAnalysis.cpp
  Optimizer/Scalar/TDZDedup.cpp
  Optimizer/Scalar/PrivateBrandCheckDedup.cpp
  Optimizer/Scalar/StripDebugInsts.cpp
  LINK_OBJLIBS hermesFrontend
)

add_hermes_library(hermesFrontend
  IRGen/IRGen.cpp
  IRGen/ESTreeIRGen.cpp IRGen/ESTreeIRGen.h
  IRGen/ESTreeIRGen-expr.cpp
  IRGen/ESTreeIRGen-stmt.cpp
  IRGen/ESTreeIRGen-func.cpp
  IRGen/ESTreeIRGen-except.cpp
  IRGen/ESTreeIRGen-typed-class.cpp
  IRGen/ESTreeIRGen-legacy-class.cpp
  IR/Analysis.cpp
  IR/IR.cpp
  IR/TypeContext.cpp
  IR/IREval.cpp
  IR/IRHash.cpp
  IR/CFG.cpp
  IR/IRBuilder.cpp
  IR/IRVerifier.cpp
  IR/Instrs.cpp
  IR/IRUtils.cpp
  Utils/Dumper.cpp
  LINK_OBJLIBS hermesSupport hermesFrontEndDefs hermesAST hermesSema hermesParser
)

add_subdirectory(VM)
add_subdirectory(Inst)
add_subdirectory(FrontEndDefs)
add_subdirectory(CompilerDriver)
add_subdirectory(DependencyExtractor)
add_subdirectory(ADT)
add_subdirectory(AST)
add_subdirectory(AST2JS)
add_subdirectory(Parser)
add_subdirectory(SourceMap)
add_subdirectory(Support)
add_subdirectory(BCGen)
add_subdirectory(ConsoleHost)
add_subdirectory(Regex)
add_subdirectory(Platform)
add_subdirectory(InternalJavaScript)
add_subdirectory(TypedLib)
add_subdirectory(Sema)

# Define combined libraries from all components. Components are defined as object
# libraries. It is tricky.
#
# The problem is that most of the componenents have dependencies on other components.
# We can't simply add them as LINK_LIBRARIES because that transfers their tranisitive
# dependency metadata to the combined library. The combined library ends up
# containing the dependency and simultaneously depending on it.
# So, we are forced to include just dependencies using $<TARGET_OBJECTS:component>.
# But then theere is anothet problem: the TARGET_OBJECTS approach doesn't transfer
# any metadata at all (include paths, external dependencies, etc).
# Fortunately most components don't have external dependencies or include paths,
# but some do.
#
# Our solution. If a component has external dependencies or other metadata we
# need, we make sure it has not internal dependencies and add it to LINK_LIBRARIES
# (this could make it harder to use in tests, but such is life).
# The rest of the components we add with the TARGET_OBJECTS approach.

add_library(hermescompiler STATIC)
target_link_libraries(hermescompiler PUBLIC
  LLVHSupport_obj
  ${BOOST_CONTEXT_LIB}
  dtoa_obj
  hermesADT_obj
  hermesAST2JS_obj
  hermesAST_obj
  hermesBackend_obj
  hermesFrontEndDefs_obj
  hermesFrontend_obj
  hermesHBCBackend_obj
  hermesInst_obj
  hermesOptimizer_obj
  hermesParser_obj
  hermesPlatformUnicode_obj
  hermesPlatform_obj
  hermesPublic_obj
  hermesRegex_obj
  hermesSema_obj
  hermesSHBackend_obj
  hermesSourceMap_obj
  hermesSupport_obj
  hermesTypedLib_obj
  zip_obj
  )

set(HERMESVM_PUBLIC_INCLUDE_DIRS
  ${PROJECT_SOURCE_DIR}/public
  ${HERMES_COMMON_DIR}/public
  ${PROJECT_SOURCE_DIR}/API
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_BINARY_DIR}/include
  ${PROJECT_BINARY_DIR}/lib/config
  ${PROJECT_SOURCE_DIR}/external
  ${PROJECT_SOURCE_DIR}/external/llvh/include
  ${PROJECT_SOURCE_DIR}/external/llvh/gen/include
  ${PROJECT_BINARY_DIR}/external/llvh/include
  ${HERMES_JSI_DIR}
)

add_library(hermesvm_a STATIC)
target_include_directories(hermesvm_a PUBLIC ${HERMESVM_PUBLIC_INCLUDE_DIRS})
target_link_libraries(hermesvm_a PUBLIC
  LLVHSupport_obj
  ${BOOST_CONTEXT_LIB}
  dtoa_obj
  hermesapi_obj
  hermesADT_obj
  hermesAST2JS_obj
  hermesAST_obj
  hermesBackend_obj
  hermesFrontEndDefs_obj
  hermesFrontend_obj
  hermesHBCBackend_obj
  hermesInst_obj
  hermesInstrumentation_obj
  hermesInternalJavaScript_obj
  hermesOptimizer_obj
  hermesParser_obj
  hermesPlatformUnicode_obj
  hermesPlatform_obj
  hermesPublic_obj
  hermesRegex_obj
  hermesSema_obj
  hermesSHBackend_obj
  hermesSourceMap_obj
  hermesSupport_obj
  hermesTypedLib_obj
  hermesVMRuntime_obj
  zip_obj
  )

add_library(hermesvmlean_a STATIC)
target_include_directories(hermesvmlean_a PUBLIC ${HERMESVM_PUBLIC_INCLUDE_DIRS})
target_link_libraries(hermesvmlean_a PUBLIC
  LLVHSupport_obj
  ${BOOST_CONTEXT_LIB}
  dtoa_obj
  hermesapi_obj
  hermesADT_obj
  hermesFrontEndDefs_obj # needed by JIT
  hermesHBCBackendLean_obj
  hermesInst_obj
  hermesInstrumentation_obj
  hermesInternalJavaScript_obj
  hermesPlatformUnicode_obj
  hermesPlatform_obj
  hermesPublic_obj
  hermesRegex_obj
  hermesSupport_obj
  hermesVMRuntime_obj
  zip_obj
  )

# CMake requires at least one file for the target, so create an empty file.
add_library(hermesvm SHARED dummy.cpp)
add_library(hermesvmlean SHARED dummy.cpp)

# Define the deployment target in the framework's metadata.
function(hermes_set_framework_min_os_version target)
  if(HERMES_APPLE_TARGET_PLATFORM MATCHES "iphone|appletv|xr")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :MinimumOSVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:${target}>/Info.plist
    )
  elseif(HERMES_APPLE_TARGET_PLATFORM MATCHES "catalyst|macos")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:${target}>/Resources/Info.plist
    )
  endif()
endfunction()

if(APPLE AND HERMES_BUILD_APPLE_FRAMEWORK)
  set_target_properties(hermesvm PROPERTIES
    FRAMEWORK TRUE
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION}
    FRAMEWORK_VERSION ${PROJECT_VERSION_MAJOR}
    MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_FRAMEWORK_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_FRAMEWORK_IDENTIFIER dev.hermesengine.${HERMES_APPLE_TARGET_PLATFORM}
  )
  if(HERMES_APPLE_TARGET_PLATFORM MATCHES "xr" AND CMAKE_VERSION VERSION_LESS 3.28.4)
    message("VisionOS Simulator requires CMake version >= 3.28.4")
  endif()
  hermes_set_framework_min_os_version(hermesvm)
endif()

# Force the entire hermesvm_a library to be used by hermesvm. There is
# no cross platform way to do this (force_load is for macOS). Once we upgrade
# CMake 3.24, we can use the LINK_LIBRARY generator expression to do this.
# We use private dependencies for the shared library target, in order to avoid
# having its dependencies added to the link line of things that depend on it.
# That is, we do not want libraries that depend on hermesvm to link in an extra
# copy of any static libraries linked into it.
if(APPLE)
  target_link_libraries(hermesvm PRIVATE -force_load hermesvm_a)
  target_link_libraries(hermesvmlean PRIVATE -force_load hermesvmlean_a)
elseif(MSVC)
  target_link_libraries(hermesvm PRIVATE hermesvm_a)
  target_link_options(hermesvm PRIVATE /WHOLEARCHIVE:hermesvm_a)
  target_link_libraries(hermesvmlean PRIVATE hermesvmlean_a)
  target_link_options(hermesvmlean PRIVATE /WHOLEARCHIVE:hermesvmlean_a)
else()
  target_link_libraries(hermesvm PRIVATE
    -Wl,--whole-archive
    hermesvm_a
    -Wl,--no-whole-archive
  )
  target_link_libraries(hermesvmlean PRIVATE
    -Wl,--whole-archive
    hermesvmlean_a
    -Wl,--no-whole-archive
  )
endif()
if(HERMES_ENABLE_NAPI)
  if(APPLE)
    target_link_libraries(hermesvm PRIVATE -force_load hermesNapi)
    target_link_libraries(hermesvmlean PRIVATE -force_load hermesNapi)
  elseif(MSVC)
    target_link_libraries(hermesvm PRIVATE hermesNapi)
    target_link_options(hermesvm PRIVATE /WHOLEARCHIVE:hermesNapi)
    target_link_libraries(hermesvmlean PRIVATE hermesNapi)
    target_link_options(hermesvmlean PRIVATE /WHOLEARCHIVE:hermesNapi)
  else()
    target_link_libraries(hermesvm PRIVATE
      -Wl,--whole-archive hermesNapi -Wl,--no-whole-archive)
    target_link_libraries(hermesvmlean PRIVATE
      -Wl,--whole-archive hermesNapi -Wl,--no-whole-archive)
  endif()
endif()

# A library with a stable name for Node-API addons to link against, which
# forwards only the Node-API functions to hermesvm.
if(HERMES_ENABLE_NAPI AND (APPLE OR HERMES_IS_ANDROID))
  include(HermesNodeApi)
  set(NODE_API_SYMBOLS_FILE ${PROJECT_SOURCE_DIR}/API/napi/node-api.symbols)

  if(APPLE)
    set(NODE_API_REEXPORTS ${CMAKE_CURRENT_BINARY_DIR}/node-api.reexports)
    hermes_write_node_api_reexports(${NODE_API_SYMBOLS_FILE} ${NODE_API_REEXPORTS})

    add_library(node-api SHARED dummy.cpp)
    if(HERMES_BUILD_APPLE_FRAMEWORK)
      set_target_properties(node-api PROPERTIES
        FRAMEWORK TRUE
        # Part of the install name addons record, so it can't follow the Hermes version.
        FRAMEWORK_VERSION A
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_FRAMEWORK_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_FRAMEWORK_IDENTIFIER dev.hermesengine.node-api.${HERMES_APPLE_TARGET_PLATFORM}
      )
      hermes_set_framework_min_os_version(node-api)
    endif()
    target_link_options(node-api PRIVATE
      "LINKER:-reexported_symbols_list,${NODE_API_REEXPORTS}"
      ${HERMES_EXTRA_LINKER_FLAGS}
    )
    set_property(TARGET node-api APPEND PROPERTY LINK_DEPENDS ${NODE_API_REEXPORTS})
  elseif(HERMES_IS_ANDROID)
    # bionic searches libnode-api.so before libhermesvm.so, so this defines
    # nothing. Referencing every symbol fails the link if hermesvm lacks one.
    set(NODE_API_REFS_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/node-api-refs.c)
    hermes_write_node_api_refs_source(${NODE_API_SYMBOLS_FILE} ${NODE_API_REFS_SOURCE})
    add_library(node-api SHARED ${NODE_API_REFS_SOURCE})

    # lld doesn't resolve symbols through libnode-api.so's DT_NEEDED, so addons
    # link against this libnode-api.so instead.
    set(HERMES_NODE_API_STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/node-api-stub CACHE PATH
      "Directory for the libnode-api.so that Node-API addons link against")
    set(NODE_API_STUB_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/node-api-stub.c)
    hermes_write_node_api_stub_source(${NODE_API_SYMBOLS_FILE} ${NODE_API_STUB_SOURCE})
    add_library(node-api-stub SHARED ${NODE_API_STUB_SOURCE})
    set_target_properties(node-api-stub PROPERTIES
      OUTPUT_NAME node-api
      LIBRARY_OUTPUT_DIRECTORY ${HERMES_NODE_API_STUB_DIR}/${ANDROID_ABI}
    )
    add_dependencies(node-api node-api-stub)
  endif()

  target_link_libraries(node-api PRIVATE hermesvm)
  target_include_directories(node-api PUBLIC ${PROJECT_SOURCE_DIR}/include/hermes/napi)
endif()

# Since we use private dependencies above, we need to enumerate all the headers
# that should be available to consumers of hermesvm. Note that we only expose
# the headers which produce visible symbols.
target_include_directories(hermesvm PUBLIC ${HERMESVM_PUBLIC_INCLUDE_DIRS})
target_include_directories(hermesvmlean PUBLIC ${HERMESVM_PUBLIC_INCLUDE_DIRS})

# Private dependencies also mean that JSI will not get propagated to libraries
# that depend on hermesvm. If JSI is built as a shared library, ensure it is
# added to the link line for anything that depends on hermesvm. If it's a static
# library, ensure it isn't since that would link in JSI twice.
if (HERMES_BUILD_SHARED_JSI)
  target_link_libraries(hermesvm PUBLIC jsi)
endif()

# Apply additional linker flags.
target_link_options(hermesvm PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})
target_link_options(hermesvmlean PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

if(HERMES_ENABLE_INTL)
  set(INTL_OBJ_LIBS hermesPlatformIntl_obj hermesBCP47Parser_obj)
  target_link_libraries(hermesvm_a PUBLIC ${INTL_OBJ_LIBS})
  target_link_libraries(hermesvmlean_a PUBLIC ${INTL_OBJ_LIBS})
endif()

# Construct the HermesVM model string
set(HERMESVM_MODEL "")

if (HERMESVM_COMPRESSED_POINTERS)
  string(APPEND HERMESVM_MODEL "_cp")
endif()
if (HERMESVM_BOXED_DOUBLES)
  string(APPEND HERMESVM_MODEL "_bd")
endif()
if (HERMESVM_CONTIGUOUS_HEAP)
  string(APPEND HERMESVM_MODEL "_ch")
endif()
string(APPEND HERMESVM_MODEL "_s" ${HERMESVM_LOG_HEAP_SEGMENT_SIZE})
# The pointer size is not strictly necessary, but include it for completeness.
string(APPEND HERMESVM_MODEL "_p" ${CMAKE_SIZEOF_VOID_P})

configure_file(config/libhermesvm-config.h.in config/libhermesvm-config.h)
