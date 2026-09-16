add_subdirectory(src)
add_subdirectory(../common common)

target_include_directories(board PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/include)
