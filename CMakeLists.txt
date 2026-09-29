cmake_minimum_required(VERSION 3.20)
project(CLauncher CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Don't force -O2/-march=native globally: optimization is chosen by
# CMAKE_BUILD_TYPE=Release, and -march=native pins the binary to a specific CPU
# and breaks portability of the built launcher.
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
    src/logger.cpp
    src/curl_tls.cpp
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

# .exe icon (Windows): embed clauncher.ico via resource file.
# windres processes .rc automatically when file is added to sources.
if(WIN32)
    target_sources(CLauncher PRIVATE clauncher.rc)
endif()

# ========== LINK LIBRARIES ==========
if(WIN32)
    target_link_libraries(CLauncher PRIVATE
        glfw
        OpenGL::GL
        CURL::libcurl
        Threads::Threads
        shell32 user32 gdi32 advapi32
    )

    # Statically link ONLY the GCC/MinGW runtime (libstdc++-6.dll, libgcc_s_seh-1.dll,
    # libwinpthread-1.dll end up inside the exe). glfw3.dll and libcurl-4.dll
    # remain DYNAMIC: CI (build.yml) copies them and transitive mingw-DLLs
    # next to the exe in CLauncher-Windows-x64.zip, otherwise the program won't start.
    target_link_options(CLauncher PRIVATE
        -static-libgcc
        -static-libstdc++
    )
else()
    target_link_libraries(CLauncher PRIVATE
        glfw
        OpenGL::OpenGL
        CURL::libcurl
        Threads::Threads
    )
endif()

# ========== TLS CAs ON WINDOWS ==========
# CI packaging script (build.yml) finds ca-certificates in MSYS2 and copies
# cacert.pem next to the exe. Here we duplicate that at CMake level: if
# cacert.pem is present next to the sources — put it next to the binary
# at build time (including local builds outside CI).
if(WIN32)
    if(EXISTS "${CMAKE_SOURCE_DIR}/cacert.pem")
        add_custom_command(TARGET CLauncher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/cacert.pem"
                "$<TARGET_FILE_DIR:CLauncher>/cacert.pem"
            COMMENT "Copying cacert.pem next to CLauncher.exe"
        )
    else()
        message(WARNING
            "cacert.pem not found next to CMakeLists.txt. "
            "On Windows, libcurl (MinGW/OpenSSL) cannot find system CAs: "
            "either place cacert.pem next to the .exe, or use a libcurl build "
            "with Schannel (CURLSSLOPT_NATIVE_CA). CI does this automatically."
        )
    endif()
endif()
