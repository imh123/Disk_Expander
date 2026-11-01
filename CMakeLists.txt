cmake_minimum_required(VERSION 3.16)

project(Disk_Expander VERSION 0.3.0 LANGUAGES CXX RC)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    diskmodel.h
    diskscanner.cpp
    diskscanner.h
    diskmapwidget.cpp
    diskmapwidget.h
    spaceaudit.cpp
    spaceaudit.h
    auditdialog.cpp
    auditdialog.h
    maintenance.cpp
    maintenance.h
    maintenancedialog.cpp
    maintenancedialog.h
    rollback.cpp
    rollback.h
    rollbackdialog.cpp
    rollbackdialog.h
    app.rc
    resources.qrc
)

qt_add_executable(Disk_Expander
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)

target_link_libraries(Disk_Expander PRIVATE
    Qt6::Widgets
    ole32
    shell32
    advapi32
)

# 读写 PhysicalDrive 必须提权；启动即请求管理员权限。
if(MSVC)
    # CMake 自己会生成并嵌入 manifest，这里只补充 UAC 提权要求。
    target_link_options(Disk_Expander PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

set_target_properties(Disk_Expander PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

include(GNUInstallDirs)
install(TARGETS Disk_Expander
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Disk_Expander)
endif()
