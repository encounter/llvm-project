include(ExternalProject)

# DXCompiler library
set(lib_destination "${LLVM_LIBRARY_OUTPUT_INTDIR}/clang-hsh/${CLANG_VERSION}")
set(lib_install_destination "lib${LLVM_LIBDIR_SUFFIX}/clang-hsh/${CLANG_VERSION}")

if(WIN32)
  set(DXCOMPILER_SHLIB dxcompiler${CMAKE_SHARED_LIBRARY_SUFFIX})
elseif(APPLE)
  set(DXCOMPILER_SHLIB libdxcompiler.3.7${CMAKE_SHARED_LIBRARY_SUFFIX})
else()
  set(DXCOMPILER_SHLIB libdxcompiler${CMAKE_SHARED_LIBRARY_SUFFIX}.3.7)
endif()

if(CMAKE_HOST_UNIX)
  set(INTERN_COPY COMMAND "${CMAKE_COMMAND}" -E copy
          lib/${DXCOMPILER_SHLIB} "${lib_destination}")
  set(LINK_OR_COPY COMMAND "${CMAKE_COMMAND}" -E create_symlink
          ${DXCOMPILER_SHLIB}
          "${lib_destination}/libdxcompiler${CMAKE_SHARED_LIBRARY_SUFFIX}")
else()
  set(INTERN_COPY COMMAND "${CMAKE_COMMAND}" -E copy
          bin/dxcompiler${CMAKE_SHARED_LIBRARY_SUFFIX} "${lib_destination}")
  unset(LINK_OR_COPY)
endif()

ExternalProject_Add(
        DirectXShaderCompiler
        GIT_REPOSITORY https://github.com/microsoft/DirectXShaderCompiler
        GIT_TAG v1.5.2010
        PATCH_COMMAND "${GIT_EXECUTABLE}" reset --hard HEAD
        COMMAND "${GIT_EXECUTABLE}" apply "${CMAKE_CURRENT_SOURCE_DIR}/dxc.patch"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build . --target dxcompiler
        INSTALL_COMMAND
        "${CMAKE_COMMAND}" -E make_directory "${CLANG_BINARY_DIR}/include/dxc/"
        COMMAND
        "${CMAKE_COMMAND}" -E make_directory "${CLANG_BINARY_DIR}/include/dxc/Support"
        COMMAND
        "${CMAKE_COMMAND}" -E copy ../DirectXShaderCompiler/include/dxc/dxcapi.h
        "${CLANG_BINARY_DIR}/include/dxc/dxcapi.h"
        COMMAND
        "${CMAKE_COMMAND}" -E copy ../DirectXShaderCompiler/include/dxc/Support/WinAdapter.h
        "${CLANG_BINARY_DIR}/include/dxc/Support/WinAdapter.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${lib_destination}"
        ${INTERN_COPY}
        ${LINK_OR_COPY}
        CMAKE_CACHE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
        -DCMAKE_SYSTEM_NAME:STRING=${CMAKE_SYSTEM_NAME}
        -DCMAKE_SYSTEM_PROCESSOR:STRING=${CMAKE_SYSTEM_PROCESSOR}
        -DCMAKE_CROSSCOMPILING:STRING=${CMAKE_CROSSCOMPILING}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_C_COMPILER_LAUNCHER:STRING=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_COMPILER_LAUNCHER:STRING=${CMAKE_CXX_COMPILER_LAUNCHER}
        -DCMAKE_AR:STRING=${CMAKE_AR}
        -DCMAKE_STRIP:STRING=${CMAKE_STRIP}
        -DPKG_CONFIG_EXECUTABLE:STRING=${PKG_CONFIG_EXECUTABLE}
        -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
        -DLLVM_USE_LINKER:STRING=${LLVM_USE_LINKER}
        -DSPIRV_WERROR:STRING=Off
        -DENABLE_SPIRV_CODEGEN:STRING=On
        -DLLVM_TARGETS_TO_BUILD:STRING=None
        -DCLANG_ENABLE_ARCMT:STRING=Off
        -DCLANG_ENABLE_STATIC_ANALYZER:STRING=Off
        -DCLANG_INCLUDE_TESTS:STRING=Off
        -DLLVM_INCLUDE_TESTS:STRING=Off
        -DHLSL_INCLUDE_TESTS:STRING=Off
        -DHLSL_BUILD_DXILCONV:STRING=Off
        -DLLVM_INCLUDE_DOCS:STRING=Off
        -DLLVM_INCLUDE_EXAMPLES:STRING=Off
        -DLIBCLANG_BUILD_STATIC:STRING=On
        -DLLVM_OPTIMIZED_TABLEGEN:STRING=Off
        -DLLVM_REQUIRES_EH:STRING=On
        -DLLVM_APPEND_VC_REV:STRING=On
        -DLLVM_ENABLE_RTTI:STRING=On
        -DLLVM_ENABLE_EH:STRING=On
        -DLLVM_DEFAULT_TARGET_TRIPLE:STRING=dxil-ms-dx
        -DCLANG_BUILD_EXAMPLES:STRING=Off
        -DLLVM_REQUIRES_RTTI:STRING=On
        -DCLANG_CL:STRING=Off)

# Use DXC's main header as a dependency barrier to defer
# building clangHsh until after DXC is built.
set(dxc_header "${CLANG_BINARY_DIR}/include/dxc/dxcapi.h")
add_custom_command(OUTPUT "${dxc_header}"
        COMMAND "${CMAKE_COMMAND}" ARGS
        --build . --target DirectXShaderCompiler
        DEPENDS "${CMAKE_CURRENT_LIST_FILE}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Fetching and building DirectXShaderCompiler")

# UAM library
find_program(MESON meson)
if(NOT MESON)
  message(FATAL_ERROR "Failed to find meson, which is required to build uam")
endif()

find_program(NINJA ninja)
if(NOT NINJA)
  message(FATAL_ERROR "Failed to find ninja, which is required to build uam")
endif()

set(MESON_OPTIONS "--prefix=${CLANG_BINARY_DIR}" -Dbuildtype=$<IF:$<CONFIG:Debug>,debug,release>)
if(APPLE)
  set(archs ${CMAKE_OSX_ARCHITECTURES})
  list(TRANSFORM archs PREPEND "-arch ")
  list(JOIN archs " " archs_join)
  list(APPEND MESON_OPTIONS "-Dc_args=${archs_join}" "-Dcpp_args=${archs_join}")
endif()

if(CMAKE_CROSSCOMPILING)
  string(REPLACE "-" ";" target_list ${LLVM_DEFAULT_TARGET_TRIPLE})
  list(GET target_list 0 arch)
  list(GET target_list 1 system)
  file(WRITE "${CMAKE_BINARY_DIR}/uam-cross" "[binaries]
c = '${CMAKE_C_COMPILER}'
cpp = '${CMAKE_CXX_COMPILER}'
ar = '${CMAKE_AR}'
strip = '${CMAKE_STRIP}'
pkgconfig = '${PKG_CONFIG_EXECUTABLE}'
c_ld = '${LLVM_USE_LINKER}'
cpp_ld = '${LLVM_USE_LINKER}'

[host_machine]
system = '${system}'
cpu = '${arch}'
cpu_family = '${arch}'
endian = 'little' # fixme?
")
  list(APPEND MESON_OPTIONS "--cross-file=${CMAKE_BINARY_DIR}/uam-cross")
else()
  set(link "${LLVM_USE_LINKER}")
  if(WIN32 OR APPLE)
    # meson misidentifies cl.exe and XCode cc when given a full path (sigh)
    # we'll just assume they're on $PATH
    get_filename_component(c_compiler "${CMAKE_C_COMPILER}" NAME)
    get_filename_component(cpp_compiler "${CMAKE_CXX_COMPILER}" NAME)
    set(COMPILERS "c = '${c_compiler}'
cpp = '${cpp_compiler}'")
  endif()
  if(WIN32 AND "${link}" STREQUAL "lld")
    set(link "lld-link")
  endif()
  if(NOT WIN32)
    set(COMPILERS "${COMPILERS}
ar = '${CMAKE_AR}'
strip = '${CMAKE_STRIP}'
pkgconfig = '${PKG_CONFIG_EXECUTABLE}'")
  endif()
  file(WRITE "${CMAKE_BINARY_DIR}/uam-native" "[binaries]
${COMPILERS}
c_ld = '${link}'
cpp_ld = '${link}'
")
  list(APPEND MESON_OPTIONS "--native-file=${CMAKE_BINARY_DIR}/uam-native")
endif()

ExternalProject_Add(
        UAM
        GIT_REPOSITORY https://github.com/devkitPro/uam.git
        GIT_TAG v1.1.0
        PATCH_COMMAND "${GIT_EXECUTABLE}" reset --hard HEAD
        COMMAND "${GIT_EXECUTABLE}" apply "${CMAKE_CURRENT_SOURCE_DIR}/uam.patch"
        CONFIGURE_COMMAND "${MESON}" "<SOURCE_DIR>" ${MESON_OPTIONS}
        BUILD_COMMAND "${NINJA}"
        INSTALL_COMMAND "")

ExternalProject_Get_Property(UAM SOURCE_DIR BINARY_DIR)
set(uam_source_dir "${SOURCE_DIR}")
set(uam_binary_dir "${BINARY_DIR}")

# Use UAM's main library as a dependency barrier to defer
# building clangHsh until after UAM is built.
set(uam_library "${uam_binary_dir}/libuam.a")
add_custom_command(OUTPUT "${uam_library}"
        COMMAND "${CMAKE_COMMAND}" ARGS
        --build . --target UAM
        DEPENDS "${CMAKE_CURRENT_LIST_FILE}" uam.patch
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Fetching and building UAM")

include_directories(
        "${uam_source_dir}/source"
        "${uam_source_dir}/mesa-imported")

add_clang_library(clangHsh
        "${dxc_header}"
        "${uam_library}"
        Builtins/Builtins.cpp
        Builtins/NonConstExpr.cpp
        Compilers/DxcStageCompiler.cpp
        Compilers/MetalStageCompiler.cpp
        Compilers/UamStageCompiler.cpp
        Compilers/StageCompiler.cpp
        PrintingPolicies/GLSLPrintingPolicy.cpp
        PrintingPolicies/HLSLPrintingPolicy.cpp
        PrintingPolicies/MetalPrintingPolicy.cpp
        PrintingPolicies/ShaderPrintingPolicy.cpp
        HshAction.cpp
        Partitioner.cpp
        Builder.cpp
        LINK_LIBS
        clangTooling)
target_link_libraries(clangHsh PRIVATE "${uam_library}")

# Enable -fms-extensions for clang to use MS uuid extensions for COM.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set_source_files_properties(Compilers/DxcStageCompiler.cpp PROPERTIES
          COMPILE_FLAGS "-fms-extensions -Wno-language-extension-token")
endif()

add_custom_target(DirectXShaderCompiler-libs ALL DEPENDS "${dxc_header}")
set_target_properties(DirectXShaderCompiler-libs PROPERTIES
        FOLDER "Misc"
        RUNTIME_OUTPUT_DIRECTORY "${lib_install_destination}")

install(FILES
        "${lib_destination}/${CMAKE_SHARED_LIBRARY_PREFIX}dxcompiler${CMAKE_SHARED_LIBRARY_SUFFIX}"
        "${lib_destination}/${DXCOMPILER_SHLIB}"
        COMPONENT DirectXShaderCompiler-libs
        DESTINATION "${lib_install_destination}")

if(NOT LLVM_ENABLE_IDE)
  add_llvm_install_targets(install-DirectXShaderCompiler-libs
          DEPENDS "${dxc_header}"
          COMPONENT DirectXShaderCompiler-libs)
endif()
