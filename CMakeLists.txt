cmake_minimum_required(VERSION 3.27)
project(NewsBoard VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.3)
    message(FATAL_ERROR "需要 GCC 12.3 或更新（当前 ${CMAKE_CXX_COMPILER_VERSION}）")
endif()

# Qt 版本：默认只要求 6.8 及以上（本机 nixpkgs 提供 6.11）。
# 没有强制精确版本，方便在不同发行版上直接构建。
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)
find_package(Qt6 6.8 QUIET COMPONENTS Svg)

qt_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/core/Config.cpp
    src/core/FeedCatalog.cpp
    src/core/NewsAggregator.cpp
    src/core/PyEnv.cpp
    src/core/QuoteService.cpp
    src/ui/FilterPanel.cpp
    src/ui/NewsListPanel.cpp
    src/ui/Overlays.cpp
    src/ui/QuoteBoardPanel.cpp
    src/ui/TickerBar.cpp
    src/ui/Theme.cpp
)

set(HEADERS
    src/MainWindow.h
    src/core/Config.h
    src/core/FeedCatalog.h
    src/core/Models.h
    src/core/NewsAggregator.h
    src/core/PyEnv.h
    src/core/QuoteService.h
    src/ui/FilterPanel.h
    src/ui/NewsListPanel.h
    src/ui/Overlays.h
    src/ui/QuoteBoardPanel.h
    src/ui/TickerBar.h
    src/ui/Theme.h
)

qt_add_executable(NewsBoard ${SOURCES} ${HEADERS})

target_include_directories(NewsBoard PRIVATE src)

target_link_libraries(NewsBoard PRIVATE Qt6::Widgets Qt6::Network)

# 把 Python 桥脚本放到可执行文件旁边（运行时按 <exe_dir>/scripts/yf_fetch.py 查找）
add_custom_command(TARGET NewsBoard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts
            $<TARGET_FILE_DIR:NewsBoard>/scripts
    COMMENT "复制 yf_fetch.py 到输出目录")

install(TARGETS NewsBoard RUNTIME DESTINATION bin)
install(DIRECTORY scripts/ DESTINATION bin/scripts)

message(STATUS "NewsBoard ${PROJECT_VERSION} — Qt ${Qt6Widgets_VERSION} — ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
