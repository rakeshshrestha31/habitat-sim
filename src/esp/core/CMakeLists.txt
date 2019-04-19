file(GLOB_RECURSE core_SOURCES "*.cpp")

find_package(Corrade REQUIRED Utility)
add_compile_options(-std=c++14)

add_library(core STATIC ${core_SOURCES})

target_link_libraries(core
  PUBLIC
    Corrade::Utility
    glog
)
