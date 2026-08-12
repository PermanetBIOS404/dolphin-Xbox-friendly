add_dolphin_test(MMIOTest MMIOTest.cpp)
add_dolphin_test(PageFaultTest PageFaultTest.cpp)
add_dolphin_test(CoreTimingTest CoreTimingTest.cpp)
add_dolphin_test(PatchAllowlistTest PatchAllowlistTest.cpp)
add_dolphin_test(WiiExportExecutionTest UICommon/WiiExportExecutionTest.cpp)
add_dolphin_test(WiiExportNativeBackendTest UICommon/WiiExportNativeBackendTest.cpp)
add_dolphin_test(WiiExportPlanTest UICommon/WiiExportPlanTest.cpp)
add_dolphin_test(WiiExportPreviewTest UICommon/WiiExportPreviewTest.cpp)
add_dolphin_test(WbfsWriterTest WbfsWriterTest.cpp)
add_dolphin_test(WiimotePointerTest HW/WiimotePointerTest.cpp)

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Test Widgets)
  add_dolphin_test(QuickMenuTest
    ../DolphinQt/QuickMenuTest.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QuickMenu.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QuickMenu.h
  )
  set_target_properties(QuickMenuTest PROPERTIES AUTOMOC ON)
  target_link_libraries(QuickMenuTest PUBLIC Qt6::Widgets)

  add_dolphin_test(WiiExportPreviewQtTest
    ../DolphinQt/WiiExportPreviewQtTest.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QtUtils/DolphinFileDialog.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QtUtils/DolphinFileDialog.h
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/WiiExportDestinationInspector.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/WiiExportDestinationInspector.h
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/WiiExportPreviewDialog.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/WiiExportPreviewDialog.h
  )
  set_target_properties(WiiExportPreviewQtTest PROPERTIES AUTOMOC ON)
  target_link_libraries(WiiExportPreviewQtTest PUBLIC Qt6::Widgets)

  add_dolphin_test(WiiExportGameListPreviewQtTest
    ../DolphinQt/WiiExportGameListPreviewQtTest.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/GameList/WiiExportGameListPreview.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/GameList/WiiExportGameListPreview.h
  )
  set_target_properties(WiiExportGameListPreviewQtTest PROPERTIES AUTOMOC ON)
  target_link_libraries(WiiExportGameListPreviewQtTest PUBLIC Qt6::Widgets)

  add_dolphin_test(WiiExportGameListExecutionQtTest
    ../DolphinQt/WiiExportGameListExecutionQtTest.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/GameList/WiiExportGameListExecution.cpp
    ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/GameList/WiiExportGameListExecution.h
  )
  set_target_properties(WiiExportGameListExecutionQtTest PROPERTIES AUTOMOC ON)
  target_link_libraries(WiiExportGameListExecutionQtTest PUBLIC Qt6::Widgets)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11_INPUT_TEST REQUIRED IMPORTED_TARGET xi)
    pkg_check_modules(X11_XTEST REQUIRED IMPORTED_TARGET xtst)
    add_executable(QuickMenuNativeTest EXCLUDE_FROM_ALL
      ../DolphinQt/QuickMenuNativeTest.cpp
      ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QuickMenu.cpp
      ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QuickMenu.h
    )
    set_target_properties(QuickMenuNativeTest PROPERTIES
      AUTOMOC ON
      FOLDER Tests
    )
    target_include_directories(QuickMenuNativeTest PRIVATE ${CMAKE_SOURCE_DIR}/Source/Core)
    target_link_libraries(QuickMenuNativeTest PRIVATE common Qt6::Test Qt6::Widgets X11::X11)

    add_custom_target(quick-menu-native-test
      COMMAND
        ${CMAKE_SOURCE_DIR}/Tools/test-quick-menu-xvfb.sh
        $<TARGET_FILE:QuickMenuNativeTest>
        ${CMAKE_BINARY_DIR}
      DEPENDS QuickMenuNativeTest
      USES_TERMINAL
    )

    add_executable(PointerRecoveryNativeTest EXCLUDE_FROM_ALL
      ../DolphinQt/PointerRecoveryNativeTest.cpp
      ../StubHost.cpp
      ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QuickMenu.cpp
      ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/QuickMenu.h
      ${CMAKE_SOURCE_DIR}/Source/Core/DolphinQt/PointerRecoveryWindow.h
    )
    set_target_properties(PointerRecoveryNativeTest PROPERTIES
      AUTOMOC ON
      FOLDER Tests
    )
    target_include_directories(PointerRecoveryNativeTest PRIVATE ${CMAKE_SOURCE_DIR}/Source/Core)
    target_link_libraries(PointerRecoveryNativeTest PRIVATE
      core
      uicommon
      Qt6::Test
      Qt6::Widgets
      X11::X11
      PkgConfig::X11_INPUT_TEST
      PkgConfig::X11_XTEST
    )

    add_custom_target(pointer-recovery-native-test
      COMMAND
        ${CMAKE_SOURCE_DIR}/Tools/test-pointer-recovery-xvfb.sh
        $<TARGET_FILE:PointerRecoveryNativeTest>
        ${CMAKE_BINARY_DIR}
      DEPENDS PointerRecoveryNativeTest
      USES_TERMINAL
    )
  endif()
endif()

add_dolphin_test(DSPAcceleratorTest DSP/DSPAcceleratorTest.cpp)
add_dolphin_test(DSPAssemblyTest
  DSP/DSPAssemblyTest.cpp
  DSP/DSPTestBinary.cpp
  DSP/DSPTestText.cpp
  DSP/HermesBinary.cpp
  DSP/HermesText.cpp
)

add_dolphin_test(ESFormatsTest IOS/ES/FormatsTest.cpp)

add_dolphin_test(FileSystemTest IOS/FS/FileSystemTest.cpp)

add_dolphin_test(SDStorageTest IOS/SDIO/SDStorageTest.cpp)
add_dolphin_test(PhysicalSDMountTest IOS/SDIO/PhysicalSDMountTest.cpp)
add_dolphin_test(PhysicalSDUnmountTest IOS/SDIO/PhysicalSDUnmountTest.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  add_dolphin_test(SDStorageLinuxTest IOS/SDIO/SDStorageLinuxTest.cpp)
  add_dolphin_test(SDStoragePreflightLinuxTest IOS/SDIO/SDStoragePreflightLinuxTest.cpp)
  if(ENABLE_QT)
    add_dolphin_test(PhysicalSDUnmountUDisksTest
      UICommon/PhysicalSDUnmountUDisksTest.cpp
    )
  endif()
endif()

add_dolphin_test(SkylandersTest IOS/USB/SkylandersTest.cpp)

if(_M_X86_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/Jit64Common/ConvertDoubleToSingle.cpp
    PowerPC/Jit64Common/Fres.cpp
    PowerPC/Jit64Common/Frsqrte.cpp
  )
elseif(_M_ARM_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/JitArm64/ConvertSingleDouble.cpp
    PowerPC/JitArm64/FPRF.cpp
    PowerPC/JitArm64/Fres.cpp
    PowerPC/JitArm64/Frsqrte.cpp
    PowerPC/JitArm64/MovI2R.cpp
  )
else()
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
  )
endif()

target_sources(PowerPCTest PRIVATE
  PowerPC/TestValues.h
  StubJit.h
)
