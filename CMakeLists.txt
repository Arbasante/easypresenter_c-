cmake_minimum_required(VERSION 3.16)
project(EasyPresenter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- CONFIGURACIÓN DE SLINT ---
include(FetchContent)

# Forzamos a Slint a que construya la API de C++
set(SLINT_BUILD_CPP ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG v1.8.0 # Usamos una versión más reciente y estable
)
FetchContent_MakeAvailable(Slint)

# --- SQLITE ---
find_package(SQLite3 REQUIRED)

# --- EJECUTABLE ---
add_executable(EasyPresenter src/main.cpp)

# Generar y vincular la interfaz
slint_target_sources(EasyPresenter
    ui/main_ui.slint
    
)

# Enlazar todo
target_link_libraries(EasyPresenter PRIVATE
    Slint::Slint
    SQLite::SQLite3
)

# --- CPACK (Para el .deb) ---
set(CPACK_PACKAGE_NAME "easypresenter")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Tu Nombre")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libfontconfig1, libsqlite3-0")
install(TARGETS EasyPresenter DESTINATION bin)
include(CPack)
