cmake_minimum_required(VERSION 3.21)
project(NadirStation VERSION 2.0.0 LANGUAGES C CXX)

if(UNIX AND NOT APPLE)
    # GNU/Linux-specific temporary fixes
    include(GNUInstallDirs)

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# MinGW Makefiles are single-configuration generators; `cmake --build --config
# Release` does not change their compile flags. Default an unset build type to
# Release so a normal Windows build does not run the per-sample DSP unoptimized.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NADIRSTATION_BUILD_PLUTO "Build with libiio/PlutoSDR support" ON)
include(CTest)

# The bundled OpenJPEG sets a legacy LIBRARY_OUTPUT_PATH cache entry. Keep archives
# in this build tree even if that entry points at /bin.
if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

# One warning set for every target. Applying it only to NadirStation made
# "builds without warnings" a vacuous claim for the command-line tools, which is
# exactly where the binary header encoding lives.
if(MSVC)
    set(NADIRSTATION_WARNING_FLAGS /W4 /permissive-)
else()
    set(NADIRSTATION_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

set(SGP4_DIR "${PROJECT_SOURCE_DIR}/third_party/sgp4-source/libsgp4")
add_library(nadirstation_sgp4 STATIC
    ${SGP4_DIR}/CoordGeodetic.cpp ${SGP4_DIR}/CoordTopocentric.cpp
    ${SGP4_DIR}/DateTime.cpp ${SGP4_DIR}/DecayedException.cpp ${SGP4_DIR}/Eci.cpp
    ${SGP4_DIR}/Globals.cpp ${SGP4_DIR}/Observer.cpp ${SGP4_DIR}/OrbitalElements.cpp
    ${SGP4_DIR}/SatelliteException.cpp ${SGP4_DIR}/SGP4.cpp ${SGP4_DIR}/SolarPosition.cpp
    ${SGP4_DIR}/TimeSpan.cpp ${SGP4_DIR}/Tle.cpp ${SGP4_DIR}/TleException.cpp
    ${SGP4_DIR}/Util.cpp ${SGP4_DIR}/Vector.cpp)
target_include_directories(nadirstation_sgp4 PUBLIC ${SGP4_DIR})
# Third-party code: left as-is, it does not build clean under -Wextra.

set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow_shell.cpp
    src/mainwindow_live.cpp
    src/mainwindow_images.cpp
    src/mainwindow_add.cpp
    src/mainwindow_settings.cpp
    src/spectrumwidget.cpp
    src/constellationwidget.cpp
    src/imageview.cpp
    src/systemmonitorpage.cpp
    src/decoderpage.cpp
    src/telemetrypage.cpp
    src/gvartelemetrywidget.cpp
    src/plutoreceiver.cpp
    src/rtlsdrreceiver.cpp
    src/wavreceiver.cpp
    src/nadirstationdecoder.cpp
    src/imageproducts.cpp
    src/mcircolour.cpp
    src/lritadd.cpp
    src/des.cpp
    src/des.h
    src/gk2aproducts.cpp
    src/gk2aproducts.h
    src/orbit.cpp
    src/dsp.cpp
    src/runtime_logger.cpp
    src/gk2adecoder.cpp
    src/gk2adatalink.cpp
    src/ggak.cpp
    src/ggakpage.cpp
    src/globecore.cpp
    src/globeview.cpp
    src/passpredict.cpp
    src/passpage.cpp
    src/swcore.cpp
    src/spaceweatherpage.cpp
    src/nowcast.cpp
)

set(HEADERS
    src/mainwindow.h
    src/mainwindow_common.h
    src/uikit.h
    src/uitheme.h
    src/snrtrend.h
    src/loadingsplash.h
    src/busyoverlay.h
    src/spectrumwidget.h
    src/constellationwidget.h
    src/imageview.h
    src/systemmonitorpage.h
    src/decoderpage.h
    src/telemetrypage.h
    src/gvartelemetrywidget.h
    src/gvardoc.h
    src/plutoreceiver.h
    src/rtlsdrreceiver.h
    src/wavreceiver.h
    src/nadirstationdecoder.h
    src/imageproducts.h
    src/mcircolour.h
    src/lritadd.h
    src/orbit.h
    src/dsp.h
    src/gk2adecoder.h
    src/gk2adatalink.h
    src/ggak.h
    src/ggakpage.h
    src/spectrumtap.h
    src/iqrecorder.h
    src/globecore.h
    src/globeview.h
    src/passpredict.h
    src/passpage.h
    src/swcore.h
    src/spaceweatherpage.h
    src/nowcast.h
)

add_executable(NadirStation ${SOURCES} ${HEADERS})

# Pass prediction on the command line (no Qt): the same code as the Passes page, for checking
# against Orbitron.  NadirStation_PassPredict <tle file> <lat> <lon> [alt_m] [days] [min_peak_deg]
add_executable(NadirStation_PassPredict tools/pass_predict.cpp src/passpredict.cpp src/passpredict.h)
target_include_directories(NadirStation_PassPredict PRIVATE src)
target_link_libraries(NadirStation_PassPredict PRIVATE nadirstation_sgp4)
# Application icon (resources/icon/nadirstation.ico) as the executable's IDI_ICON1 resource.
if(WIN32)
    enable_language(RC)
    target_sources(NadirStation PRIVATE resources/icon/nadirstation.rc)
endif()


# GK-2A LRIT demodulation and framing, with a self test for the Viterbi decoder so a failure can
# be attributed to the decoder or to the recording rather than guessed at.
add_executable(NadirStation_GK2ADemod tools/gk2a_demod.cpp src/gk2adecoder.cpp src/gk2adecoder.h
    src/dsp.cpp src/dsp.h)
target_include_directories(NadirStation_GK2ADemod PRIVATE src)

add_executable(NadirStation_GK2ATest tools/gk2a_test.cpp src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2ATest PRIVATE src)

add_executable(NadirStation_GK2ADiag tools/gk2a_diag.cpp src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2ADiag PRIVATE src)

add_executable(NadirStation_GK2ATrace tools/gk2a_trace.cpp src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2ATrace PRIVATE src)

add_executable(NadirStation_GK2AE2E tools/gk2a_e2e.cpp src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2AE2E PRIVATE src)

add_executable(NadirStation_GK2ADatalink tools/gk2a_datalink_test.cpp src/gk2adatalink.cpp src/gk2adatalink.h src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2ADatalink PRIVATE src)

add_executable(NadirStation_GK2AStream tools/gk2a_stream.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.cpp src/gk2adecoder.h src/gk2adatalink.cpp src/gk2adatalink.h src/des.cpp src/des.h src/lritadd.cpp src/lritadd.h)
# Qt6::Gui is needed for QImage, which decodes the JPEG the payload carries.
target_link_libraries(NadirStation_GK2AStream PRIVATE Qt6::Core Qt6::Gui)
target_compile_definitions(NadirStation_GK2AStream PRIVATE NADIRSTATION_QT_PLUGIN_DIR="/../../../plugins")
target_include_directories(NadirStation_GK2AStream PRIVATE src)

add_executable(NadirStation_GK2AMcir tools/gk2a_products_export.cpp
    src/gk2aproducts.cpp src/gk2aproducts.h src/gk2adatalink.h
    src/runtime_logger.cpp src/runtime_logger.h)
target_include_directories(NadirStation_GK2AMcir PRIVATE src)
target_link_libraries(NadirStation_GK2AMcir PRIVATE Qt6::Core Qt6::Gui)
target_compile_definitions(NadirStation_GK2AMcir PRIVATE NADIRSTATION_QT_PLUGIN_DIR="/../../../plugins")
add_custom_command(TARGET NadirStation_GK2AMcir POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:NadirStation_GK2AMcir>/resources"
    COMMENT "Copying resources for GK-2A products")

add_executable(NadirStation_DecodeProbe
    tools/decode_probe.cpp
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/des.cpp src/des.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_DecodeProbe PRIVATE src)
target_link_libraries(NadirStation_DecodeProbe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets nadirstation_sgp4)

# Decoding threshold of any link: adds calibrated noise to a recording and counts what each stage passes.
add_executable(NadirStation_ThresholdProbe
    tools/threshold_probe.cpp
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/des.cpp src/des.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_ThresholdProbe PRIVATE src)
target_link_libraries(NadirStation_ThresholdProbe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets nadirstation_sgp4)

add_executable(NadirStation_Fy2SnrSweep
    tools/fy2_snr_sweep.cpp
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/des.cpp src/des.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_Fy2SnrSweep PRIVATE src)
target_link_libraries(NadirStation_Fy2SnrSweep PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets nadirstation_sgp4)

add_executable(NadirStation_SnrProbe tools/snr_probe.cpp src/dsp.cpp src/dsp.h)
target_include_directories(NadirStation_SnrProbe PRIVATE src)
target_link_libraries(NadirStation_SnrProbe PRIVATE Qt6::Core)

add_executable(NadirStation_Decrypt tools/decrypt_lrit.cpp src/des.cpp src/des.h src/gk2adatalink.cpp src/gk2adatalink.h src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_Decrypt PRIVATE src)
target_link_libraries(NadirStation_Decrypt PRIVATE Qt6::Core)

add_executable(NadirStation_GK2ASplit tools/gk2a_split_test.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2ASplit PRIVATE src)

add_executable(NadirStation_GK2APolarity tools/gk2a_polarity.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.cpp src/gk2adecoder.h)
target_include_directories(NadirStation_GK2APolarity PRIVATE src)

# Low-SNR test benches (2026-11-01), no Qt: synthetic GK-2A LRIT link with known frames, a
# recording runner that can add noise to a real capture, and the same for HRIT (QPSK 3 Mbaud).
add_executable(NadirStation_GK2ASim tools/gk2a_sim.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.h)
target_include_directories(NadirStation_GK2ASim PRIVATE src)
add_executable(NadirStation_GK2AFileAB tools/gk2a_file_ab.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.cpp src/gk2adecoder.h src/gk2adatalink.cpp src/gk2adatalink.h)
target_include_directories(NadirStation_GK2AFileAB PRIVATE src)
add_executable(NadirStation_HritSim tools/gk2a_hrit_sim.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.h)
target_include_directories(NadirStation_HritSim PRIVATE src)
add_executable(NadirStation_HritFile tools/gk2a_hrit_file.cpp src/dsp.cpp src/dsp.h src/gk2adecoder.cpp src/gk2adecoder.h src/gk2adatalink.cpp src/gk2adatalink.h)
target_include_directories(NadirStation_HritFile PRIVATE src)

# Offline RRC/PLL comparison helper. It only depends on the shared DSP code,
# so it can process an IQ WAV without opening the GUI or Pluto device.
add_executable(NadirStation_DemodSweep tools/demod_sweep.cpp src/dsp.cpp src/dsp.h)
target_include_directories(NadirStation_DemodSweep PRIVATE src)

add_executable(NadirStation_ProductPreview tools/product_preview.cpp src/imageproducts.cpp src/imageproducts.h src/orbit.cpp src/orbit.h)
target_link_libraries(NadirStation_ProductPreview PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)

add_executable(NadirStation_MCIRColour tools/mcir_colour_preview.cpp src/imageproducts.cpp src/imageproducts.h src/mcircolour.cpp src/mcircolour.h src/orbit.cpp src/orbit.h)

add_executable(NadirStation_AddProbe tools/add_probe.cpp src/lritadd.cpp src/lritadd.h)
target_include_directories(NadirStation_AddProbe PRIVATE src)
target_link_libraries(NadirStation_AddProbe PRIVATE Qt6::Core)
target_include_directories(NadirStation_MCIRColour PRIVATE src)
target_link_libraries(NadirStation_MCIRColour PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)
target_include_directories(NadirStation_ProductPreview PRIVATE src)
target_link_libraries(NadirStation_ProductPreview PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)
add_custom_command(TARGET NadirStation_ProductPreview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:NadirStation_ProductPreview>/resources"
            COMMENT "Copying FY-2 lookup tables for product preview")

add_executable(NadirStation_DerivedProduct tools/derived_product_preview.cpp src/imageproducts.cpp src/imageproducts.h src/orbit.cpp src/orbit.h)
target_include_directories(NadirStation_DerivedProduct PRIVATE src)
target_link_libraries(NadirStation_DerivedProduct PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)
target_compile_options(NadirStation_DerivedProduct PRIVATE ${NADIRSTATION_WARNING_FLAGS})

add_executable(NadirStation_RegionalProducts tools/regional_product_preview.cpp src/imageproducts.cpp src/imageproducts.h src/orbit.cpp src/orbit.h)
target_include_directories(NadirStation_RegionalProducts PRIVATE src)
target_link_libraries(NadirStation_RegionalProducts PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)
target_compile_options(NadirStation_RegionalProducts PRIVATE ${NADIRSTATION_WARNING_FLAGS})
add_custom_command(TARGET NadirStation_RegionalProducts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:NadirStation_RegionalProducts>/resources"
    COMMENT "Copying FY-2 resources for regional products")

# The L1B export shares the receiver's calibration-block verdict, so it links the
# decoder rather than keeping a second copy of the rules.
add_executable(NadirStation_L1BExport
    tools/l1b_export.cpp
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    # nadirstationdecoder.cpp now carries the GK-2A LRIT chain, so these come with it. A target that
    # compiles the decoder needs them the same way it needs dsp.cpp.
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/des.cpp src/des.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_L1BExport PRIVATE src)
target_link_libraries(NadirStation_L1BExport PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)
if(WIN32)
    target_link_libraries(NadirStation_L1BExport PRIVATE dbghelp)
endif()
target_compile_options(NadirStation_L1BExport PRIVATE ${NADIRSTATION_WARNING_FLAGS})

# Judges a Calibration-2 block with the same rules the station uses, so the
# accept/reject decision can be audited outside the receive path. Needed because
# a damaged block was found to pass the original range-count test.
add_executable(NadirStation_CalibrationCheck
    tools/calibration_check.cpp
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    # nadirstationdecoder.cpp now carries the GK-2A LRIT chain, so these come with it. A target that
    # compiles the decoder needs them the same way it needs dsp.cpp.
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/des.cpp src/des.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_CalibrationCheck PRIVATE src)
target_link_libraries(NadirStation_CalibrationCheck PRIVATE Qt6::Core Qt6::Gui nadirstation_sgp4)
if(WIN32)
    # runtime_logger.cpp installs a crash handler that calls MiniDumpWriteDump.
    target_link_libraries(NadirStation_CalibrationCheck PRIVATE dbghelp)
endif()
target_compile_options(NadirStation_CalibrationCheck PRIVATE ${NADIRSTATION_WARNING_FLAGS})
add_custom_command(TARGET NadirStation_CalibrationCheck POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:NadirStation_CalibrationCheck>/resources"
    COMMENT "Copying FY-2 lookup tables for the calibration checker")
add_custom_command(TARGET NadirStation_L1BExport POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:NadirStation_L1BExport>/resources"
    COMMENT "Copying FY-2 resources for the L1B export tool")

add_custom_command(TARGET NadirStation POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:NadirStation>/resources"
    COMMENT "Copying bundled FY-2 S-VISSR lookup tables")

target_include_directories(NadirStation PRIVATE src)

target_link_libraries(NadirStation PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network nadirstation_sgp4)
if(WIN32)
    target_link_libraries(NadirStation PRIVATE dbghelp psapi)
endif()

target_compile_definitions(NadirStation PRIVATE
    NADIRSTATION_DEFAULT_PLUTO_IP=\"192.168.3.10\"
)

if(NADIRSTATION_BUILD_PLUTO)
    # Linux/macOS: pkg-config is the most reliable discovery route.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PLUTO_IIO QUIET IMPORTED_TARGET libiio)
    endif()

    if(TARGET PkgConfig::PLUTO_IIO)
        target_link_libraries(NadirStation PRIVATE PkgConfig::PLUTO_IIO)
        target_compile_definitions(NadirStation PRIVATE NADIRSTATION_HAS_LIBIIO=1)
    else()
        # Windows / manual installation route:
        #   cmake -DLIBIIO_ROOT=C:/path/to/libiio ...
        set(LIBIIO_ROOT "" CACHE PATH "Root of libiio installation")
        set(_LIBIIO_HINTS
            ${LIBIIO_ROOT}
            ${LIBIIO_ROOT}/include
            "$ENV{ProgramFiles}/libiio"
            "$ENV{ProgramFiles}/libiio-0.26.ga0eca0d2"
            "$ENV{ProgramFiles}/Analog Devices/libiio"
            C:/libiio
            C:/libiio/Windows-MinGW-W64)
        find_path(LIBIIO_INCLUDE_DIR iio.h HINTS ${_LIBIIO_HINTS} PATH_SUFFIXES include)
        find_library(LIBIIO_LIBRARY NAMES iio libiio HINTS ${_LIBIIO_HINTS} PATH_SUFFIXES lib lib64 Windows-MinGW-W64)
        if(LIBIIO_INCLUDE_DIR AND LIBIIO_LIBRARY)
            message(STATUS "libiio found: include=${LIBIIO_INCLUDE_DIR}; library=${LIBIIO_LIBRARY}")
            target_include_directories(NadirStation PRIVATE ${LIBIIO_INCLUDE_DIR})
            target_link_libraries(NadirStation PRIVATE ${LIBIIO_LIBRARY})
            target_compile_definitions(NadirStation PRIVATE NADIRSTATION_HAS_LIBIIO=1)
        else()
            message(WARNING "libiio not found; building simulation-only mode. Install libiio and reconfigure to enable PlutoSDR.")
        endif()
    endif()
endif()

# Live measurement harness: connects to the receiver and feeds the decoder the way the
# application does, so the rates at which it locks can be measured rather than argued.
add_executable(NadirStation_LiveProbe
    tools/live_probe.cpp
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/plutoreceiver.cpp src/plutoreceiver.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/des.cpp src/des.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_LiveProbe PRIVATE src)
target_link_libraries(NadirStation_LiveProbe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets nadirstation_sgp4)
if(TARGET PkgConfig::PLUTO_IIO)
    target_link_libraries(NadirStation_LiveProbe PRIVATE PkgConfig::PLUTO_IIO)
    target_compile_definitions(NadirStation_LiveProbe PRIVATE NADIRSTATION_HAS_LIBIIO=1)
elseif(NADIRSTATION_BUILD_PLUTO AND LIBIIO_INCLUDE_DIR AND LIBIIO_LIBRARY)
    target_include_directories(NadirStation_LiveProbe PRIVATE ${LIBIIO_INCLUDE_DIR})
    target_link_libraries(NadirStation_LiveProbe PRIVATE ${LIBIIO_LIBRARY})
    target_compile_definitions(NadirStation_LiveProbe PRIVATE NADIRSTATION_HAS_LIBIIO=1)
endif()

# Runs the application's GGAK decoder on the PlutoSDR or a recorded .cs16 file, without the GUI.
add_executable(NadirStation_GgakProbe
    tools/ggak_probe.cpp
    src/wavreceiver.cpp src/wavreceiver.h
    src/nadirstationdecoder.cpp src/nadirstationdecoder.h src/ggak.cpp src/ggak.h
    src/plutoreceiver.cpp src/plutoreceiver.h
    src/imageproducts.cpp src/imageproducts.h
    src/orbit.cpp src/orbit.h
    src/dsp.cpp src/dsp.h
    src/lritadd.cpp src/lritadd.h
    src/gk2adecoder.cpp src/gk2adecoder.h
    src/gk2adatalink.cpp src/gk2adatalink.h
    src/gk2aproducts.cpp src/gk2aproducts.h
    src/des.cpp src/des.h
    src/runtime_logger.cpp)
target_include_directories(NadirStation_GgakProbe PRIVATE src)
target_link_libraries(NadirStation_GgakProbe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets nadirstation_sgp4)
if(TARGET PkgConfig::PLUTO_IIO)
    target_link_libraries(NadirStation_GgakProbe PRIVATE PkgConfig::PLUTO_IIO)
    target_compile_definitions(NadirStation_GgakProbe PRIVATE NADIRSTATION_HAS_LIBIIO=1)
elseif(NADIRSTATION_BUILD_PLUTO AND LIBIIO_INCLUDE_DIR AND LIBIIO_LIBRARY)
    target_include_directories(NadirStation_GgakProbe PRIVATE ${LIBIIO_INCLUDE_DIR})
    target_link_libraries(NadirStation_GgakProbe PRIVATE ${LIBIIO_LIBRARY})
    target_compile_definitions(NadirStation_GgakProbe PRIVATE NADIRSTATION_HAS_LIBIIO=1)
endif()

# Records IQ from the PlutoSDR through the application's PlutoReceiver (for offline analysis).
add_executable(NadirStation_PlutoCapture tools/pluto_capture.cpp src/plutoreceiver.cpp src/plutoreceiver.h src/dsp.cpp src/dsp.h)
target_include_directories(NadirStation_PlutoCapture PRIVATE src)
target_link_libraries(NadirStation_PlutoCapture PRIVATE Qt6::Core)
if(TARGET PkgConfig::PLUTO_IIO)
    target_link_libraries(NadirStation_PlutoCapture PRIVATE PkgConfig::PLUTO_IIO)
    target_compile_definitions(NadirStation_PlutoCapture PRIVATE NADIRSTATION_HAS_LIBIIO=1)
elseif(NADIRSTATION_BUILD_PLUTO AND LIBIIO_INCLUDE_DIR AND LIBIIO_LIBRARY)
    target_include_directories(NadirStation_PlutoCapture PRIVATE ${LIBIIO_INCLUDE_DIR})
    target_link_libraries(NadirStation_PlutoCapture PRIVATE ${LIBIIO_LIBRARY})
    target_compile_definitions(NadirStation_PlutoCapture PRIVATE NADIRSTATION_HAS_LIBIIO=1)
endif()

foreach(pluto_probe IN ITEMS NadirStation_LiveProbe NadirStation_GgakProbe NadirStation_PlutoCapture)
    target_compile_definitions(${pluto_probe} PRIVATE NADIRSTATION_DEFAULT_PLUTO_IP="192.168.3.10")
endforeach()

if(WIN32)
    foreach(probe IN ITEMS NadirStation_GK2AMcir NadirStation_DecodeProbe NadirStation_ThresholdProbe NadirStation_Fy2SnrSweep NadirStation_LiveProbe NadirStation_GgakProbe)
        target_link_libraries(${probe} PRIVATE dbghelp)
    endforeach()
endif()

target_compile_options(NadirStation PRIVATE ${NADIRSTATION_WARNING_FLAGS})

set_target_properties(NadirStation PROPERTIES
    OUTPUT_NAME NADIR_Station   # product name since 1.1.0; the CMake target keeps its old name
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

install(TARGETS NadirStation RUNTIME DESTINATION bin BUNDLE DESTINATION .)
install(TARGETS NadirStation_RegionalProducts RUNTIME DESTINATION bin)

# HRIT image libraries are pinned, bundled source, with their upstream licenses retained.
# Static linking keeps the deployed receiver independent of the SatDump installation.
function(nadir_add_openjpeg)
    set(OPENJPEG_NAMESPACE NADIR_OPENJPEG)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_CODEC OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF)
    add_subdirectory(third_party/openjpeg-2.5.3 EXCLUDE_FROM_ALL)
endfunction()
nadir_add_openjpeg()
include(ExternalProject)
set(HRIT_JPEG_BINARY "${CMAKE_BINARY_DIR}/hrit-jpeg")
ExternalProject_Add(nadir_jpeg_build
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libjpeg-turbo-3.1.2"
    BINARY_DIR "${HRIT_JPEG_BINARY}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
        -DWITH_SIMD=OFF -DWITH_TURBOJPEG=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static --parallel 4
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${HRIT_JPEG_BINARY}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
add_library(nadir_turbojpeg STATIC IMPORTED)
set_target_properties(nadir_turbojpeg PROPERTIES
    IMPORTED_LOCATION "${HRIT_JPEG_BINARY}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
add_dependencies(nadir_turbojpeg nadir_jpeg_build)
add_library(nadir_hrit STATIC src/gk2ahrit.cpp src/gk2ahritimages.cpp)
target_include_directories(nadir_hrit PUBLIC src PRIVATE
    third_party/openjpeg-2.5.3/src/lib/openjp2
    "${CMAKE_BINARY_DIR}/third_party/openjpeg-2.5.3/src/lib/openjp2"
    third_party/libjpeg-turbo-3.1.2/src "${HRIT_JPEG_BINARY}")
target_compile_definitions(nadir_hrit PRIVATE OPJ_STATIC)
target_compile_options(nadir_hrit PRIVATE ${NADIRSTATION_WARNING_FLAGS})
target_link_libraries(nadir_hrit PUBLIC Qt6::Core Qt6::Gui openjp2 nadir_turbojpeg)
# ELEKTRO-L LRIT / HRIT images: EUMETSAT PublicDecompWT (Apache-2.0) for the wavelet and JPEG data
# fields, built without its time classes (PUBLICDECOMPWT_NO_TIME, see third_party/PublicDecompWT).
file(GLOB NADIR_PDWT_SOURCES
    third_party/PublicDecompWT/COMP/Src/*.cpp
    third_party/PublicDecompWT/COMP/JPEG/Src/*.cpp
    third_party/PublicDecompWT/COMP/T4/Src/*.cpp
    third_party/PublicDecompWT/COMP/WT/Src/*.cpp)
add_library(nadir_pdwt STATIC ${NADIR_PDWT_SOURCES} third_party/PublicDecompWT/DISE/ErrorHandling.cpp)
target_include_directories(nadir_pdwt PUBLIC
    third_party/PublicDecompWT/DISE
    third_party/PublicDecompWT/COMP/Inc
    third_party/PublicDecompWT/COMP/JPEG/Inc
    third_party/PublicDecompWT/COMP/T4/Inc
    third_party/PublicDecompWT/COMP/WT/Inc)
target_compile_definitions(nadir_pdwt PUBLIC PUBLICDECOMPWT_NO_TIME)
target_compile_options(nadir_pdwt PRIVATE -w)
add_library(nadir_elektro STATIC src/elektroxrit.cpp src/elektroxrit.h src/elektrol2.cpp src/elektrol2.h)
target_include_directories(nadir_elektro PUBLIC src)
target_link_libraries(nadir_elektro PUBLIC nadir_pdwt Qt6::Core Qt6::Gui)
get_property(nadir_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(nadir_target IN LISTS nadir_targets)
    get_target_property(nadir_sources ${nadir_target} SOURCES)
    if("src/nadirstationdecoder.cpp" IN_LIST nadir_sources)
        target_sources(${nadir_target} PRIVATE src/nadirstationhrit.cpp
            src/gvarengine.cpp src/nadirstationgvar.cpp src/goessd.cpp src/goessd.h src/nadirstationgoessd.cpp src/nadirstationelektro.cpp src/nadirstationdiag.cpp src/pipelinediag.h
            src/polarhrpt.cpp src/polarhrpt.h src/polarinstruments.cpp src/polarinstruments.h src/ascatl2.cpp src/ascatl2.h src/geomap.cpp src/geomap.h src/metopmaps.cpp src/nadirstationpolar.cpp src/snrmeter.h)
        if(NOT "src/passpredict.cpp" IN_LIST nadir_sources)
            target_sources(${nadir_target} PRIVATE src/passpredict.cpp src/passpredict.h)
        endif()
        target_link_libraries(${nadir_target} PRIVATE nadir_hrit nadir_elektro nadirstation_sgp4)
    endif()
endforeach()
add_executable(NadirStation_HritTest tools/gk2a_hrit_test.cpp
    src/dsp.cpp src/gk2adecoder.cpp src/gk2adatalink.cpp src/des.cpp)
target_link_libraries(NadirStation_HritTest PRIVATE nadir_hrit)
target_include_directories(NadirStation_HritTest PRIVATE src
    third_party/openjpeg-2.5.3/src/lib/openjp2
    "${CMAKE_BINARY_DIR}/third_party/openjpeg-2.5.3/src/lib/openjp2"
    third_party/libjpeg-turbo-3.1.2/src)
target_compile_definitions(NadirStation_HritTest PRIVATE OPJ_STATIC)

if(BUILD_TESTING)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/work")
    add_executable(NadirStation_IqRecorderTest tools/iq_recorder_test.cpp)
    target_include_directories(NadirStation_IqRecorderTest PRIVATE src)
    target_link_libraries(NadirStation_IqRecorderTest PRIVATE Qt6::Core)
    add_executable(NadirStation_ElektroTest tools/elektro_products_test.cpp src/runtime_logger.cpp)
    target_link_libraries(NadirStation_ElektroTest PRIVATE nadir_elektro)
    if(WIN32)
        target_link_libraries(NadirStation_ElektroTest PRIVATE dbghelp)
    endif()
    foreach(test IN ITEMS GK2ATest GK2AE2E GK2ADatalink HritTest IqRecorderTest ElektroTest)
        add_test(NAME ${test} COMMAND NadirStation_${test})
    endforeach()
    set_tests_properties(IqRecorderTest PROPERTIES SKIP_RETURN_CODE 77)
endif()
