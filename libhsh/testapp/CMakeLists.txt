#hsh_sources(test-input.cpp)
add_library(hsh-test-lib test-input.cpp urde-test.cpp)
target_hsh(hsh-test-lib ALL_SOURCES)
#target_compile_options(hsh-test-lib PRIVATE -fno-rtti)

add_executable(hsh-test-pre main.cpp)
target_include_directories(hsh-test-pre PRIVATE "${HSH_INCLUDE_DIR}")
target_link_libraries(hsh-test-pre PRIVATE hsh-test-lib)
if(WIN32)
  target_compile_definitions(hsh-test-lib PUBLIC _HAS_EXCEPTIONS=0)
elseif(APPLE)
  find_library(APPKIT_LIB AppKit)
  target_link_libraries(hsh-test-pre PRIVATE ${APPKIT_LIB})
  target_include_directories(hsh-test-pre PRIVATE ${APPKIT_LIB})
  set_source_files_properties(main.cpp PROPERTIES LANGUAGE OBJCXX)
else()
  target_link_libraries(hsh-test-pre PRIVATE xcb ${CMAKE_DL_LIBS})
endif()

hsh_add_executable(hsh-test hsh-test-pre)
