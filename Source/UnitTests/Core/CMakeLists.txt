add_dolphin_test(MMIOTest MMIOTest.cpp)
add_dolphin_test(PageFaultTest PageFaultTest.cpp)
add_dolphin_test(CoreTimingTest CoreTimingTest.cpp)
add_dolphin_test(PatchAllowlistTest PatchAllowlistTest.cpp)
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

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
  add_dolphin_test(SDStorageLinuxTest IOS/SDIO/SDStorageLinuxTest.cpp)
  add_dolphin_test(SDStoragePreflightLinuxTest IOS/SDIO/SDStoragePreflightLinuxTest.cpp)
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
