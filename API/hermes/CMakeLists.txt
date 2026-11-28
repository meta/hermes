# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# compileJS uses neither exceptions nor RTTI
add_hermes_library(compileJS CompileJS.cpp LINK_LIBS hermesPublic hermesHBCBackend)

if(HERMES_ENABLE_DEBUGGER)
  # Some inspector sources need to be built without RTTI because they consume
  # internal data structures compiled without RTTI.
  set(INSPECTOR_NO_EH_RTTI_SOURCES
    cdp/JSONValueInterfaces.cpp
    cdp/MessageConverters.cpp
    cdp/MessageInterfaces.cpp
    cdp/MessageTypes.cpp
    inspector/chrome/JSONValueInterfaces.cpp
    inspector/chrome/MessageConverters.cpp
    inspector/chrome/MessageInterfaces.cpp
    inspector/chrome/MessageTypes.cpp
  )

  if (NOT HERMES_ENABLE_EH_RTTI)
    if (GCC_COMPATIBLE)
      set_property(SOURCE ${INSPECTOR_NO_EH_RTTI_SOURCES} APPEND_STRING
        PROPERTY COMPILE_FLAGS "-fno-exceptions -fno-rtti")
    elseif (MSVC)
      set_property(SOURCE ${INSPECTOR_NO_EH_RTTI_SOURCES} APPEND_STRING
        PROPERTY COMPILE_FLAGS "/EHs-c- /GR-")
    endif ()
  endif ()

  set(CDP_API_SOURCES
    cdp/CDPAgent.cpp
    cdp/CallbackOStream.cpp
    cdp/ConsoleMessage.cpp
    cdp/DebuggerDomainAgent.cpp
    cdp/DebuggerDomainCoordinator.cpp
    cdp/HeapProfilerDomainAgent.cpp
    cdp/ProfilerDomainAgent.cpp
    cdp/RemoteObjectConverters.cpp
    cdp/RemoteObjectsTable.cpp
    cdp/RuntimeDomainAgent.cpp
    cdp/ConsoleMessage.cpp
    cdp/CDPDebugAPI.cpp
    cdp/DomainState.cpp
  )

  set(INSPECTOR_API_SOURCES
    inspector/chrome/CallbackOStream.cpp
    inspector/chrome/CDPHandler.cpp
    inspector/chrome/RemoteObjectConverters.cpp
    inspector/chrome/RemoteObjectsTable.cpp
    inspector/RuntimeAdapter.cpp
    ${CDP_API_SOURCES}
    ${INSPECTOR_NO_EH_RTTI_SOURCES}
  )

  # Even in lean builds, the inspector needs the parser to parse JSON.
  set(INSPECTOR_DEPS hermesParser)
endif()

add_hermes_library(synthTraceParser SynthTraceParser.cpp LINK_LIBS hermesSupport hermesParser synthTrace)

# All remaining targets in this file use both exceptions and RTTI.
set(HERMES_ENABLE_EH_RTTI ON)

add_subdirectory(cdp)

# List the files that define exported functions explicitly, and they can link
# against the internal functionality they need.
set(api_sources
  hermes.cpp
  DebuggerAPI.cpp
  AsyncDebuggerAPI.cpp
  RuntimeTaskRunner.cpp
  ${INSPECTOR_API_SOURCES}
  )

file(GLOB api_headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB api_public_headers ${PROJECT_SOURCE_DIR}/public/hermes/Public/*.h)

if(HERMES_THREAD_SAFETY_ANALYSIS)
  set(TSA_SOURCES
    AsyncDebuggerAPI.cpp
    ${CDP_API_SOURCES}
  )
  if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang") # warning instead for struggle with std:mutex capabilities annotation
    set_property(SOURCE ${TSA_SOURCES} APPEND_STRING PROPERTY
      COMPILE_FLAGS "-Wthread-safety -Wthread-safety-analysis -Werror=thread-safety-precise -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS")
  endif()
endif()

add_hermes_library(synthTrace hermes_tracing.cpp SynthTrace.cpp TracingRuntime.cpp
  LINK_LIBS hermesvm hermesPlatform)

add_hermes_library(timerStats TimerStats.cpp LINK_LIBS jsi hermesSupport)

add_hermes_library(traceInterpreter TraceInterpreter.cpp
  LINK_LIBS hermesvm hermesInstrumentation synthTrace synthTraceParser)

add_library(hermesvm ${api_sources})
target_link_libraries(hermesvm PUBLIC jsi PRIVATE hermesVMRuntime ${INSPECTOR_DEPS})
target_link_options(hermesvm PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

# Export the required header directory
target_include_directories(hermesvm PUBLIC .. ../../public ${HERMES_COMMON_DIR}/public ${HERMES_JSI_DIR})

# Create a lean version of hermesvm in the same way.
add_library(hermesvmlean ${api_sources})
target_link_libraries(hermesvmlean PUBLIC jsi PRIVATE hermesVMRuntimeLean ${INSPECTOR_DEPS})
target_link_options(hermesvmlean PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})
target_include_directories(hermesvmlean PUBLIC .. ../../public ${HERMES_COMMON_DIR}/public ${HERMES_JSI_DIR})
set_target_properties(hermesvmlean PROPERTIES OUTPUT_NAME hermes_lean)

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
  # Install headers into `Headers` while keeping required directory structure
  set_source_files_properties(${api_headers} PROPERTIES
    MACOSX_PACKAGE_LOCATION Headers
  )
  set_source_files_properties(${api_public_headers} PROPERTIES
    MACOSX_PACKAGE_LOCATION Headers/Public
  )
  if(HERMES_ENABLE_BITCODE)
    target_compile_options(hermesvm PUBLIC "-fembed-bitcode")
    target_link_options(hermesvm PUBLIC "-fembed-bitcode")
  endif()
  # Define the deployment target in the frameworks metadata
  if(HERMES_APPLE_TARGET_PLATFORM MATCHES "iphone")
    add_custom_command(TARGET hermesvm POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :MinimumOSVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:hermesvm>/Info.plist
    )
  elseif(HERMES_APPLE_TARGET_PLATFORM MATCHES "appletv")
    add_custom_command(TARGET hermesvm POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :MinimumOSVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:hermesvm>/Info.plist
    )
  elseif(HERMES_APPLE_TARGET_PLATFORM MATCHES "catalyst")
    add_custom_command(TARGET hermesvm POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:hermesvm>/Resources/Info.plist
    )
  elseif(HERMES_APPLE_TARGET_PLATFORM MATCHES "macos")
    add_custom_command(TARGET hermesvm POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:hermesvm>/Resources/Info.plist
    )
  elseif(HERMES_APPLE_TARGET_PLATFORM MATCHES "xr")
    if(CMAKE_VERSION VERSION_LESS 3.28.4)
      message("VisionOS Simulator requires CMake version >= 3.28.4")
    endif()

    add_custom_command(TARGET hermesvm POST_BUILD
      COMMAND /usr/libexec/PlistBuddy -c "Add :MinimumOSVersion string ${CMAKE_OSX_DEPLOYMENT_TARGET}" $<TARGET_FILE_DIR:hermesvm>/Info.plist
    )
  endif()
endif()

set_target_properties(hermesvm PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

install(TARGETS hermesvm
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  FRAMEWORK DESTINATION Library/Frameworks/${HERMES_APPLE_TARGET_PLATFORM}
)
# Install headers into `include` while keeping required directory structure
install(DIRECTORY "${PROJECT_SOURCE_DIR}/API/hermes" DESTINATION include
  FILES_MATCHING PATTERN "*.h"
  PATTERN "synthtest" EXCLUDE)
