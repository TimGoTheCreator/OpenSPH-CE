set(SPH2EXTRAPRESETS_SOURCES SphToExtraPresets.cpp)
add_executable(sph2extrapresets ${SPH2EXTRAPRESETS_SOURCES})

target_include_directories(sph2extrapresets PRIVATE ../../core)
target_link_libraries(sph2extrapresets PRIVATE core)
