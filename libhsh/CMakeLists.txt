cmake_minimum_required(VERSION 3.4.3)

set(LIBHSH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LIBHSH_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Install headers
install(DIRECTORY include/hsh
        DESTINATION include
        COMPONENT libhsh-headers
        FILES_MATCHING
        PATTERN "*.def"
        PATTERN "*.h"
        PATTERN "config.h" EXCLUDE
        PATTERN ".svn" EXCLUDE
        )

add_custom_target(libhsh-headers)
set_target_properties(libhsh-headers PROPERTIES FOLDER "Misc")
if(NOT LLVM_ENABLE_IDE)
  add_llvm_install_targets(install-libhsh-headers
                           DEPENDS libhsh-headers
                           COMPONENT libhsh-headers)
endif()

include(CMakePackageConfigHelpers)

set(version_config_file "${CMAKE_CURRENT_BINARY_DIR}/hshConfigVersion.cmake")
set(config_file "${CMAKE_CURRENT_BINARY_DIR}/hshConfig.cmake")
set(config_install_dir "lib/cmake/hsh")

# Export binary targets for client usage
install(TARGETS hshgen llvm-objcopy EXPORT hshTargets)
if(MSVC)
  install(FILES $<TARGET_PDB_FILE:hshgen>
      CONFIGURATIONS Debug RelWithDebInfo
      DESTINATION bin
      COMPONENT hshgen)
  install(FILES $<TARGET_PDB_FILE:llvm-objcopy>
      CONFIGURATIONS Debug RelWithDebInfo
      DESTINATION bin
      COMPONENT llvm-objcopy)
endif()

# Install the target config files
install(EXPORT hshTargets DESTINATION ${config_install_dir}
        COMPONENT libhsh-cmake-exports
        )

# Generate version config file
write_basic_package_version_file(
        "${version_config_file}"
        COMPATIBILITY SameMajorVersion
        )

# Generate config file
configure_package_config_file(
        "hshConfig.cmake.in"
        "${config_file}"
        INSTALL_DESTINATION "lib/cmake/hsh"
        )

# Install the config files
install(FILES "${config_file}" "${version_config_file}" hshHelpers.cmake
        DESTINATION ${config_install_dir}
        COMPONENT libhsh-cmake-exports
        )

if(NOT LLVM_ENABLE_IDE)
  # Add a dummy target so this can be used with LLVM_DISTRIBUTION_COMPONENTS
  add_custom_target(libhsh-cmake-exports)
  add_llvm_install_targets(install-libhsh-cmake-exports
                           COMPONENT libhsh-cmake-exports)
endif()

# Include helper macros for build tree
set(HSH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE STRING "")
include(hshHelpers.cmake)
add_subdirectory(testapp)

# Add regression tests
add_subdirectory(test)
