cmake_minimum_required(VERSION 3.24)
project(OutfitManager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(minhook)

# The plugin: OutfitManager.asi
add_library(OutfitManager SHARED src/outfit_manager.cpp src/addrs.cpp src/core.cpp src/items.cpp src/looks.cpp src/looks_view.cpp src/saves.cpp src/store.cpp src/ui.cpp src/uibin_patch.cpp src/lang.cpp src/resolver.cpp src/log.cpp src/game.cpp src/outfit_manager.rc)
target_include_directories(OutfitManager PRIVATE src)
target_compile_definitions(OutfitManager PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(OutfitManager PRIVATE /W4 /permissive- /utf-8)
set_target_properties(OutfitManager PROPERTIES SUFFIX ".asi")
target_link_libraries(OutfitManager PRIVATE minhook version hid setupapi)

# Resolver test build: ignores the cache and the known addresses, so every address must be found by search alone
# (log lines "resolver: ..."). Deploy it as OutfitManager.asi for one launch, then put the normal build back.
add_library(OutfitManagerResolveTest SHARED src/outfit_manager.cpp src/addrs.cpp src/core.cpp src/items.cpp src/looks.cpp src/looks_view.cpp src/saves.cpp src/store.cpp src/ui.cpp src/uibin_patch.cpp src/lang.cpp src/resolver.cpp src/log.cpp src/game.cpp)
target_include_directories(OutfitManagerResolveTest PRIVATE src)
target_compile_definitions(OutfitManagerResolveTest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE OM_RESOLVE_TEST)
target_compile_options(OutfitManagerResolveTest PRIVATE /W4 /permissive- /utf-8)
set_target_properties(OutfitManagerResolveTest PROPERTIES SUFFIX ".asi")
target_link_libraries(OutfitManagerResolveTest PRIVATE minhook version hid setupapi)

# Offline resolver test (maps game exes from disk): ResolverTest.exe <cache dir> <exe>...
add_executable(ResolverTest src/resolver_test.cpp src/resolver.cpp src/log.cpp src/game.cpp)
target_include_directories(ResolverTest PRIVATE src)
target_compile_definitions(ResolverTest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ResolverTest PRIVATE /W4 /permissive- /utf-8)
