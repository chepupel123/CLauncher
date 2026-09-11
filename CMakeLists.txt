cmake_minimum_required(VERSION 3.20)
project(CLauncher CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Не задаём -O2/-march=native глобально: оптимизацию выбирает
# CMAKE_BUILD_TYPE=Release, а -march=native привязывает бинарник к конкретному CPU
# и ломает переносимость собранного лаунчера.

# ========== FIND PACKAGES ==========
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.2.0 QUIET)
find_package(PkgConfig REQUIRED)
pkg_check_modules(IMGUI imgui)

# ========== NLOHMANN JSON SETUP ==========
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via CMake, expecting system headers (apt: libnlohmann-json3-dev)")
endif()

# ========== IMGUI SETUP ==========
if(NOT IMGUI_FOUND)
    message(STATUS "ImGui not found via pkg-config, using vendored headers")
    set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/extern/imgui")
    set(IMGUI_INCLUDE_DIRS "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
else()
    set(IMGUI_DIR "${IMGUI_INCLUDEDIR}")
    set(IMGUI_INCLUDE_DIRS ${IMGUI_INCLUDE_DIRS} "${IMGUI_DIR}/backends")
endif()

include_directories(${IMGUI_INCLUDE_DIRS})

# ========== SOURCE FILES ==========
set(SOURCES
    src/main.cpp
    src/launcher.cpp
    src/version_scanner.cpp
    src/version_manager.cpp
    src/java_launcher.cpp
    src/MinecraftInstaller.cpp
    src/JavaManager.cpp
)

# ImGui sources (if vendored)
if(NOT IMGUI_FOUND)
    list(APPEND SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
endif()

# ========== BUILD EXECUTABLE ==========
add_executable(CLauncher ${SOURCES})

# ========== LINK LIBRARIES ==========
if(WIN32)
    target_link_libraries(CLauncher PRIVATE
        glfw
        OpenGL::GL
        CURL::libcurl
        Threads::Threads
        shell32 user32 gdi32 advapi32
    )
    target_link_options(CLauncher PRIVATE -static-libgcc -static-libstdc++ -static)
else()
    target_link_libraries(CLauncher PRIVATE
        glfw
        OpenGL::OpenGL
        CURL::libcurl
        Threads::Threads
    )
endif()

if(nlohmann_json_FOUND)
    target_link_libraries(CLauncher PRIVATE nlohmann_json::nlohmann_json)
endif()

if(IMGUI_FOUND)
    target_link_libraries(CLauncher PRIVATE ${IMGUI_LIBRARIES})
endif()

# ========== INCLUDE DIRECTORIES ==========
target_include_directories(CLauncher PRIVATE 
    src/
    ${IMGUI_INCLUDE_DIRS}
)

# ========== POST BUILD ==========
if(NOT WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET CLauncher POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:CLauncher>
    )
endif()

