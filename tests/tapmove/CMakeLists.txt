# Host tests for tap-to-walk (SOURCES/TAPMOVE.H): the route finder, and the maths that turns a touch
# into a spot on the floor and a heading into key presses. Neither half reads the game, so they run
# anywhere with no retail data and no display. The part that does read it (TAPMOVE.CPP: the touch,
# the destination, the stop conditions) is exercised by hand on a device.
add_executable(test_tapmove_path
    test_tapmove_path.cpp
    ${CMAKE_SOURCE_DIR}/SOURCES/TAPMOVE_PATH.CPP)

target_include_directories(test_tapmove_path PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H   # SYSTEM/ADELINE_TYPES.H
    ${CMAKE_SOURCE_DIR}/SOURCES    # TAPMOVE.H
    ${CMAKE_SOURCE_DIR}/tests      # test_harness.h
)
target_compile_definitions(test_tapmove_path PRIVATE
    "TEST_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}/\""
)
set_target_properties(test_tapmove_path PROPERTIES CXX_STANDARD 11)

add_test(NAME test_tapmove_path COMMAND test_tapmove_path)
register_host_test(test_tapmove_path)

# The maths half runs on the engine's own projection, so it links the 3D library: the point of
# inverting the live camera, rather than a model of it, is that it cannot drift from what is drawn.
add_executable(test_tapmove_math
    test_tapmove_math.cpp
    ${CMAKE_SOURCE_DIR}/SOURCES/TAPMOVE_MATH.CPP)

target_include_directories(test_tapmove_math PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H
    ${CMAKE_SOURCE_DIR}/SOURCES
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_definitions(test_tapmove_math PRIVATE
    "TEST_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}/\""
)
target_link_libraries(test_tapmove_math PRIVATE 3D)
set_target_properties(test_tapmove_math PROPERTIES CXX_STANDARD 11)

add_test(NAME test_tapmove_math COMMAND test_tapmove_math)
register_host_test(test_tapmove_math)
