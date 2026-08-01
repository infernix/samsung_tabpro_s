cmake_minimum_required(VERSION 3.20)
project(tabprobright)

set(KF6_MIN_VERSION "6.0.0")
set(QT_MIN_VERSION "6.5.0")

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(FeatureSummary)
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

# KWin 6.7 effect API uses std::span and designated initializers (C++20).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Gui Widgets)
find_package(KWin REQUIRED COMPONENTS kwin)

set(tabprobright_SOURCES
    main.cpp
    tabprobright.cpp
    tabprobright.qrc
)

kcoreaddons_add_plugin(tabprobright SOURCES ${tabprobright_SOURCES} INSTALL_NAMESPACE "kwin/effects/plugins")
target_link_libraries(tabprobright PRIVATE
    KWin::kwin
    Qt6::Gui
)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)