# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

hermes_enable_werror_undef()

if(HERMES_ENABLE_DEBUGGER)
  # Some inspector sources need to be built without RTTI because they consume
  # internal data structures compiled without RTTI.
  set(CDP_NO_EH_RTTI_SOURCES
    cdp/JSONValueInterfaces.cpp
    cdp/MessageConverters.cpp
    cdp/MessageInterfaces.cpp
    cdp/MessageTypes.cpp
  )

  if (NOT HERMES_ENABLE_EH AND NOT HERMES_ENABLE_RTTI)
    if (GCC_COMPATIBLE)
      set_property(SOURCE ${CDP_NO_EH_RTTI_SOURCES} APPEND_STRING
        PROPERTY COMPILE_FLAGS "-fno-exceptions -fno-rtti")
    elseif (MSVC)
      set_property(SOURCE ${CDP_NO_EH_RTTI_SOURCES} APPEND_STRING
        PROPERTY COMPILE_FLAGS "/EHs-c- /GR-")
    endif ()
  elseif (NOT HERMES_ENABLE_EH OR NOT HERMES_ENABLE_RTTI)
    message(
      FATAL_ERROR
      "Currently only support having exceptions and RTTI having the same enable status."
    )
  endif ()

  set(CDP_API_SOURCES
    cdp/CDPAgent.cpp
    cdp/CallbackOStream.cpp
    cdp/ConsoleMessage.cpp
    cdp/DebuggerDomainAgent.cpp
    cdp/DebuggerDomainCoordinator.cpp
    cdp/EvaluatedScriptSources.cpp
    cdp/HeapProfilerDomainAgent.cpp
    cdp/ProfilerDomainAgent.cpp
    cdp/RemoteObjectConverters.cpp
    cdp/RemoteObjectsTable.cpp
    cdp/RuntimeDomainAgent.cpp
    cdp/ConsoleMessage.cpp
    cdp/CDPDebugAPI.cpp
    cdp/DomainState.cpp
    ${CDP_NO_EH_RTTI_SOURCES}
  )

  # Even in lean builds, the inspector needs the parser to parse JSON.
  set(CDP_DEPS hermesParser)
endif()

set(HERMES_ENABLE_EH ON)
set(HERMES_ENABLE_RTTI ON)

add_subdirectory(cdp)
if(HERMES_ENABLE_CORE_EXTENSIONS)
  add_subdirectory(extensions)
endif()

set(api_sources
  hermes.cpp
  DebuggerAPI.cpp
  AsyncDebuggerAPI.cpp
  RuntimeTaskRunner.cpp
  ${CDP_API_SOURCES}
  )

if(HERMES_ENABLE_CORE_EXTENSIONS)
  list(APPEND api_sources ${EXTENSIONS_CPP_SOURCES})
endif()

file(GLOB api_headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB api_public_headers ${PROJECT_SOURCE_DIR}/public/hermes/Public/*.h)

if(HERMES_THREAD_SAFETY_ANALYSIS)
  set(TSA_SOURCES
    AsyncDebuggerAPI.cpp
    ${CDP_API_SOURCES}
  )
  if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set_property(SOURCE ${TSA_SOURCES} APPEND_STRING PROPERTY
      COMPILE_FLAGS "-Wthread-safety -Werror=thread-safety-analysis -Werror=thread-safety-precise -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS")
  endif()
endif()

if(HERMES_IS_ANDROID)
  # hermes.cpp may wrap the finalizer worker thread in an fbjni ThreadScope.
  set(API_JNI_DEPS fbjni::fbjni)
endif()

if(WIN32 AND HERMES_ENABLE_CORE_EXTENSIONS)
  # The Crypto extension sources entropy from BCryptGenRandom.
  set(API_BCRYPT_DEPS bcrypt)
endif()

add_hermes_library(hermesapi
  ${api_sources}
  LINK_OBJLIBS hermesPublic LINK_LIBS jsi ${CDP_DEPS} ${API_JNI_DEPS} ${API_BCRYPT_DEPS}
  )
target_include_directories(hermesapi_obj PUBLIC ..)

# Pass core extensions flag to C++ code.
if(HERMES_ENABLE_CORE_EXTENSIONS)
  target_compile_definitions(hermesapi_obj PRIVATE HERMES_ENABLE_CORE_EXTENSIONS=1)

  # Pass contrib extensions flag to C++ code.
  # Contrib extensions are only effective when core extensions are enabled.
  if(HERMES_ENABLE_CONTRIB_EXTENSIONS)
    target_compile_definitions(hermesapi_obj PRIVATE HERMES_ENABLE_CONTRIB_EXTENSIONS=1)
  else()
    target_compile_definitions(hermesapi_obj PRIVATE HERMES_ENABLE_CONTRIB_EXTENSIONS=0)
  endif()
else()
  target_compile_definitions(hermesapi_obj PRIVATE HERMES_ENABLE_CORE_EXTENSIONS=0)
  target_compile_definitions(hermesapi_obj PRIVATE HERMES_ENABLE_CONTRIB_EXTENSIONS=0)
endif()

if(HERMES_ENABLE_CORE_EXTENSIONS)
  # ExtensionsBytecode.cpp needs to find the generated .inc file
  target_include_directories(hermesapi_obj PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/extensions)

  # Ensure hermesapi_obj waits for ExtensionsBytecodeInclude to complete.
  # This prevents race conditions with Visual Studio where the .inc file might be
  # accessed while still being generated.
  add_dependencies(hermesapi_obj ExtensionsBytecodeInclude)
endif()

add_hermes_library(synthTrace hermes_tracing.cpp SynthTrace.cpp TracingRuntime.cpp
  LINK_OBJLIBS hermesapi)

add_hermes_library(timerStats TimerStats.cpp LINK_LIBS jsi LINK_OBJLIBS hermesSupport)

add_hermes_library(traceInterpreter TraceInterpreter.cpp
  LINK_OBJLIBS hermesapi hermesInstrumentation synthTrace synthTraceParser)

set(HERMES_ENABLE_RTTI OFF)
set(HERMES_ENABLE_EH OFF)

add_hermes_library(synthTraceParser SynthTraceParser.cpp LINK_OBJLIBS hermesSupport hermesParser synthTrace)

# compileJS uses neither exceptions nor RTTI
add_hermes_library(compileJS CompileJS.cpp LINK_OBJLIBS hermesPublic hermesHBCBackend)
