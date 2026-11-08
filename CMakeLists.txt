# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.21)
project(aslgc VERSION 0.1.0 DESCRIPTION "A simple Linux Git client" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(ASLGC_APP_ID "io.github.kyleschouw.aslgc")  # desktop entry, AppStream id, icon name, Wayland app id

# ---- options ----
option(BUILD_TESTING "Build the test suite" ON)
set(ASLGC_GITHUB_CLIENT_ID "" CACHE STRING
    "Client ID of a GitHub OAuth App (device flow enabled) for browser sign-in; empty disables it")

# ---- dependencies ----
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network Svg)
find_package(Qt6Keychain REQUIRED)  # account tokens in Secret Service / KWallet
qt_standard_project_setup()          # also includes GNUInstallDirs
include(GNUInstallDirs)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# The credential helper is internal, so it lives in libexec; the app finds it
# relative to its own location, which keeps installs relocatable.
set(ASLGC_HELPER_DIR "${CMAKE_INSTALL_LIBEXECDIR}/aslgc")
file(RELATIVE_PATH ASLGC_ASKPASS_FROM_BIN "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_PREFIX}/${ASLGC_HELPER_DIR}/aslgc-askpass")

# ---- core library: git process wrapper, parsers, diff/patch builder, graph, hosting URLs ----
add_library(aslgccore STATIC
    src/git/Types.h
    src/git/Git.h src/git/Git.cpp
    src/git/Parsers.h src/git/Parsers.cpp
    src/git/Diff.h src/git/Diff.cpp
    src/git/GitFlow.h src/git/GitFlow.cpp
    src/git/Graph.h src/git/Graph.cpp
    src/git/Rebase.h src/git/Rebase.cpp
    src/git/Repository.h src/git/Repository.cpp
    src/hosting/Account.h src/hosting/Account.cpp
    src/hosting/RemoteUrl.h src/hosting/RemoteUrl.cpp
)
target_include_directories(aslgccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aslgccore PUBLIC Qt6::Core)
target_compile_definitions(aslgccore PUBLIC ASLGC_VERSION="${PROJECT_VERSION}")

# ---- application ----
qt_add_executable(aslgc
    src/main.cpp
    src/hosting/AccountStore.h src/hosting/AccountStore.cpp
    src/hosting/HostingClient.h src/hosting/HostingClient.cpp
    src/ui/AccountsDialog.h src/ui/AccountsDialog.cpp
    src/ui/Async.h
    src/ui/BlameView.h src/ui/BlameView.cpp
    src/ui/ChangesView.h src/ui/ChangesView.cpp
    src/ui/CommitModel.h src/ui/CommitModel.cpp
    src/ui/Dialogs.h src/ui/Dialogs.cpp
    src/ui/DiffView.h src/ui/DiffView.cpp
    src/ui/FileHistoryWindow.h src/ui/FileHistoryWindow.cpp
    src/ui/FileList.h src/ui/FileList.cpp
    src/ui/HistoryView.h src/ui/HistoryView.cpp
    src/ui/InteractiveRebaseDialog.h src/ui/InteractiveRebaseDialog.cpp
    src/ui/Log.h src/ui/Log.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
    src/ui/OperationDialog.h src/ui/OperationDialog.cpp
    src/ui/PullRequestDialog.h src/ui/PullRequestDialog.cpp
    src/ui/RepoPickerDialog.h src/ui/RepoPickerDialog.cpp
    src/ui/RepoView.h src/ui/RepoView.cpp
    src/ui/Sidebar.h src/ui/Sidebar.cpp
    src/ui/Theme.h src/ui/Theme.cpp
    src/ui/WelcomeWidget.h src/ui/WelcomeWidget.cpp
)
target_link_libraries(aslgc PRIVATE aslgccore Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Svg qt6keychain)
target_compile_definitions(aslgc PRIVATE
    ASLGC_APP_ID="${ASLGC_APP_ID}"
    ASLGC_GITHUB_CLIENT_ID="${ASLGC_GITHUB_CLIENT_ID}"
    ASLGC_ASKPASS_FROM_BIN="${ASLGC_ASKPASS_FROM_BIN}")

# App icon sizes are generated from resources/icons/aslgc-source.png by tools/make-icons.sh.
set(ASLGC_ICON_SIZES 16 22 24 32 48 64 128 256 512)
set(ASLGC_EMBEDDED_ICONS)
foreach(size IN LISTS ASLGC_ICON_SIZES)
    if(size LESS_EQUAL 256)
        list(APPEND ASLGC_EMBEDDED_ICONS resources/icons/hicolor/${size}x${size}/apps/aslgc.png)
    endif()
endforeach()
# Action icons: Tabler Icons (MIT), see resources/icons/actions/LICENSE.
file(GLOB ASLGC_ACTION_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS resources/icons/actions/*.svg)
qt_add_resources(aslgc "aslgc_resources" PREFIX "/" BASE resources FILES ${ASLGC_EMBEDDED_ICONS} ${ASLGC_ACTION_ICONS})

# Translations: i18n/*.ts are compiled with lrelease and embedded at :/i18n. aslgc_en.ts
# only carries English plural forms. Without Qt LinguistTools the build simply skips them.
find_package(Qt6 6.4 QUIET COMPONENTS LinguistTools)
file(GLOB ASLGC_TS_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.ts)
if(TARGET Qt6::lrelease AND ASLGC_TS_FILES)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/i18n")
    set(ASLGC_QM_FILES)
    foreach(ts IN LISTS ASLGC_TS_FILES)
        get_filename_component(name "${ts}" NAME_WE)
        set(qm "${CMAKE_CURRENT_BINARY_DIR}/i18n/${name}.qm")
        add_custom_command(OUTPUT "${qm}" COMMAND Qt6::lrelease -silent "${ts}" -qm "${qm}" DEPENDS "${ts}" VERBATIM)
        list(APPEND ASLGC_QM_FILES "${qm}")
    endforeach()
    qt_add_resources(aslgc "aslgc_translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}/i18n" FILES ${ASLGC_QM_FILES})
else()
    message(STATUS "Qt LinguistTools not found: building without translations")
endif()

# ---- credential helper: GIT_ASKPASS / SSH_ASKPASS prompt and git credential helper ----
qt_add_executable(aslgc-askpass src/askpass/main.cpp)
target_link_libraries(aslgc-askpass PRIVATE aslgccore Qt6::Widgets qt6keychain)

# ---- tests ----
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    enable_testing()
    qt_add_executable(aslgc-tests tests/tst_core.cpp)
    target_link_libraries(aslgc-tests PRIVATE aslgccore Qt6::Test)
    add_test(NAME aslgc-tests COMMAND aslgc-tests)
endif()

# ---- install ----
install(TARGETS aslgc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS aslgc-askpass RUNTIME DESTINATION ${ASLGC_HELPER_DIR})
install(FILES packaging/${ASLGC_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/${ASLGC_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/aslgc.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES LICENSE README.md CHANGELOG.md THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
foreach(size IN LISTS ASLGC_ICON_SIZES)
    install(FILES resources/icons/hicolor/${size}x${size}/apps/aslgc.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME ${ASLGC_APP_ID}.png)
endforeach()
