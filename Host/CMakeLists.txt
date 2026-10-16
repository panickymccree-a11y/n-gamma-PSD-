cmake_minimum_required(VERSION 3.21)
project(MCAStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network SerialPort Charts Concurrent Test)

add_library(mca_core
    src/core/psd.h src/core/psd.cpp
    src/core/types.h
    src/core/protocol.h src/core/protocol.cpp
    src/core/serialprotocol.h src/core/serialprotocol.cpp
    src/core/spectrum.h src/core/spectrum.cpp
    src/core/spectrumfile.h src/core/spectrumfile.cpp
    src/core/listfile.h src/core/listfile.cpp
    src/core/eventtextwriter.h src/core/eventtextwriter.cpp
    src/core/waveform.h src/core/waveform.cpp
    src/analysis/analysis.h src/analysis/analysis.cpp
    src/app/acquisition.h src/app/acquisition.cpp
)
target_include_directories(mca_core PUBLIC src)
target_link_libraries(mca_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::SerialPort Qt6::Concurrent)

qt_add_executable(mca_studio
    src/main.cpp
    src/ui/spectrumview.h src/ui/spectrumview.cpp
    src/ui/waveformview.h src/ui/waveformview.cpp
    src/ui/waveformpage.h src/ui/waveformpage.cpp
    src/ui/mainwindow.h src/ui/mainwindow.cpp
    src/ui/psdpage.h src/ui/psdpage.cpp
    resources/resources.qrc
)
target_link_libraries(mca_studio PRIVATE mca_core Qt6::Widgets Qt6::Charts)

enable_testing()
qt_add_executable(mca_udp_sender tests/udp_sender.cpp)
target_link_libraries(mca_udp_sender PRIVATE mca_core)
qt_add_executable(mca_throughput_bench tests/throughput_bench.cpp
    src/ui/mainwindow.h src/ui/mainwindow.cpp src/ui/waveformview.h src/ui/waveformview.cpp
    src/ui/waveformpage.h src/ui/waveformpage.cpp src/ui/spectrumview.h src/ui/spectrumview.cpp
    src/ui/psdpage.h src/ui/psdpage.cpp resources/resources.qrc)
target_link_libraries(mca_throughput_bench PRIVATE mca_core Qt6::Widgets Qt6::Charts)
qt_add_executable(mca_psd_tests tests/test_psd.cpp src/ui/psdpage.h src/ui/psdpage.cpp)
target_link_libraries(mca_psd_tests PRIVATE mca_core Qt6::Widgets Qt6::Test)
add_test(NAME mca_psd_tests COMMAND mca_psd_tests -o psd_tests_result.txt,txt)
set_tests_properties(mca_psd_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
qt_add_executable(mca_tests tests/test_core.cpp)
target_link_libraries(mca_tests PRIVATE mca_core Qt6::Test)
add_test(NAME mca_tests COMMAND mca_tests -o core_tests_result.txt,txt)

qt_add_executable(mca_ui_tests
    tests/test_ui.cpp
    src/ui/spectrumview.h src/ui/spectrumview.cpp
    src/ui/waveformview.h src/ui/waveformview.cpp
    src/ui/waveformpage.h src/ui/waveformpage.cpp
    src/ui/mainwindow.h src/ui/mainwindow.cpp
    src/ui/psdpage.h src/ui/psdpage.cpp
    resources/resources.qrc
)
target_link_libraries(mca_ui_tests PRIVATE
    mca_core Qt6::Widgets Qt6::Charts Qt6::Test)
add_test(NAME mca_ui_tests COMMAND mca_ui_tests -o ui_tests_result.txt,txt)
set_tests_properties(mca_ui_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The UI tests build the full MainWindow/SpectrumView/WaveformView widget tree
# on the main thread.  With /O2 the release Qt widget and Qt Charts code paths
# need considerably more than the 1 MB default reserve, and exhausting it
# raises STATUS_STACK_BUFFER_OVERRUN (0xC0000409) during teardown.  Only the
# test executables get the larger reserve; the shipped mca_studio stack is
# untouched.
if (MSVC)
    foreach (test_target mca_tests mca_ui_tests mca_psd_tests)
        target_link_options(${test_target} PRIVATE
            "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/STACK:8388608>")
    endforeach()
endif ()

install(TARGETS mca_studio RUNTIME DESTINATION bin)
