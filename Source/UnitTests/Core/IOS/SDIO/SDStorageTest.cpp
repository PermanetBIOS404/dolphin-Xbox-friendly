// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/SDIO/SDStorage.h"
#include "Core/IOS/SDIO/SDStorageConfig.h"

namespace IOS::HLE
{
namespace
{
constexpr size_t IMAGE_SIZE = 1024;

class SDImageStorageTest : public testing::Test
{
protected:
  SDImageStorageTest()
      : m_parent_directory(File::CreateTempDir()), m_image_path(m_parent_directory + "/sd.raw")
  {
  }

  ~SDImageStorageTest() override
  {
    if (!m_parent_directory.empty())
      File::DeleteDirRecursively(m_parent_directory);
  }

  void SetUp() override
  {
    ASSERT_FALSE(m_parent_directory.empty());

    std::array<u8, IMAGE_SIZE> contents;
    for (size_t i = 0; i < contents.size(); ++i)
      contents[i] = static_cast<u8>(i);

    File::IOFile file(m_image_path, "wb");
    ASSERT_TRUE(file);
    ASSERT_TRUE(file.WriteArray(contents));
  }

  const std::string m_parent_directory;
  const std::string m_image_path;
};

TEST_F(SDImageStorageTest, OpensValidImageAndReportsProperties)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);

  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_TRUE(storage->IsPresent());
  EXPECT_FALSE(storage->IsReadOnly());
  EXPECT_EQ(storage->GetSize(), IMAGE_SIZE);
  EXPECT_EQ(storage->GetLogicalSectorSize(), 512u);
}

TEST_F(SDImageStorageTest, ReadsInRange)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 4> data;
  EXPECT_EQ(storage->Read(510, data), SDStorageResult::Success);
  EXPECT_EQ(data, (std::array<u8, 4>{254, 255, 0, 1}));
}

TEST_F(SDImageStorageTest, WritesWhenWritable)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  constexpr std::array<u8, 4> data = {10, 20, 30, 40};
  EXPECT_EQ(storage->Write(512, data), SDStorageResult::Success);
  EXPECT_EQ(storage->Flush(), SDStorageResult::Success);

  std::array<u8, 4> readback;
  EXPECT_EQ(storage->Read(512, readback), SDStorageResult::Success);
  EXPECT_EQ(readback, data);
}

TEST_F(SDImageStorageTest, RejectsWritesWhenReadOnly)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, true);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  constexpr std::array<u8, 1> data = {42};
  EXPECT_TRUE(storage->IsReadOnly());
  EXPECT_EQ(storage->Write(0, data), SDStorageResult::WriteProtected);
}

TEST_F(SDImageStorageTest, RejectsOutOfRangeReads)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  std::array<u8, 2> data;
  EXPECT_EQ(storage->Read(IMAGE_SIZE - 1, data), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Read(IMAGE_SIZE + 1, std::span<u8>{}), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Read(std::numeric_limits<u64>::max(), std::span<u8>{}),
            SDStorageResult::OutOfRange);
}

TEST_F(SDImageStorageTest, RejectsOutOfRangeWrites)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  ASSERT_EQ(storage->Open(), SDStorageResult::Success);

  constexpr std::array<u8, 2> data = {1, 2};
  EXPECT_EQ(storage->Write(IMAGE_SIZE - 1, data), SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Write(IMAGE_SIZE + 1, std::span<const u8>{}),
            SDStorageResult::OutOfRange);
  EXPECT_EQ(storage->Write(std::numeric_limits<u64>::max(), std::span<const u8>{}),
            SDStorageResult::OutOfRange);
}

TEST_F(SDImageStorageTest, ReportsMissingImageAsNoMedia)
{
  const std::unique_ptr<SDStorage> storage =
      CreateSDImageStorage(m_parent_directory + "/missing.raw", false);

  EXPECT_EQ(storage->Open(), SDStorageResult::NoMedia);
  EXPECT_FALSE(storage->IsPresent());
}

TEST_F(SDImageStorageTest, RejectsOperationsWhileClosed)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);
  std::array<u8, 1> data;

  EXPECT_EQ(storage->Read(0, data), SDStorageResult::NoMedia);
  EXPECT_EQ(storage->Write(0, data), SDStorageResult::NoMedia);
  EXPECT_EQ(storage->Flush(), SDStorageResult::NoMedia);
}

TEST_F(SDImageStorageTest, RepeatedOpenAndCloseIsSafe)
{
  const std::unique_ptr<SDStorage> storage = CreateSDImageStorage(m_image_path, false);

  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_EQ(storage->Open(), SDStorageResult::Success);
  EXPECT_EQ(storage->Close(), SDStorageResult::Success);
  EXPECT_FALSE(storage->IsPresent());
}

class FakeConfiguredStorage final : public SDStorage
{
public:
  explicit FakeConfiguredStorage(SDStorageResult open_result, bool read_only = false)
      : m_open_result(open_result), m_read_only(read_only)
  {
  }

  SDStorageResult Open() override
  {
    ++open_calls;
    m_present = m_open_result == SDStorageResult::Success;
    return m_open_result;
  }
  SDStorageResult Close() override
  {
    m_present = false;
    return SDStorageResult::Success;
  }
  SDStorageResult Read(u64, std::span<u8>) override { return SDStorageResult::Success; }
  SDStorageResult Write(u64, std::span<const u8>) override
  {
    ++write_calls;
    return m_read_only ? SDStorageResult::WriteProtected : SDStorageResult::Success;
  }
  SDStorageResult Flush() override { return SDStorageResult::Success; }
  u64 GetSize() const override { return IMAGE_SIZE; }
  u32 GetLogicalSectorSize() const override { return 512; }
  bool IsReadOnly() const override { return m_read_only; }
  bool IsPresent() const override { return m_present; }
  void Update() override {}

  int open_calls = 0;
  int write_calls = 0;

private:
  const SDStorageResult m_open_result;
  const bool m_read_only;
  bool m_present = false;
};

class FakeSDStorageBackendFactory final : public SDStorageBackendFactory
{
public:
  std::unique_ptr<SDStorage> CreateImageFile(std::string path) override
  {
    ++image_create_calls;
    image_path = std::move(path);
    auto storage = std::make_unique<FakeConfiguredStorage>(image_open_result);
    last_storage = storage.get();
    return storage;
  }

  std::unique_ptr<SDStorage> CreatePhysicalDeviceReadOnly(std::string path) override
  {
    ++physical_create_calls;
    physical_path = std::move(path);
    auto storage = std::make_unique<FakeConfiguredStorage>(physical_open_result, true);
    last_storage = storage.get();
    return storage;
  }

  SDStorageResult image_open_result = SDStorageResult::Success;
  SDStorageResult physical_open_result = SDStorageResult::Success;
  int image_create_calls = 0;
  int physical_create_calls = 0;
  std::string image_path;
  std::string physical_path;
  FakeConfiguredStorage* last_storage = nullptr;
};

class FakePhysicalSDPreflight final : public PhysicalSDPreflight
{
public:
  PhysicalSDPreflightOutcome Check(const std::string& path) override
  {
    ++check_calls;
    checked_path = path;
    return outcome;
  }

  PhysicalSDPreflightOutcome outcome{
      .result = PhysicalSDPreflightResult::Ready,
      .resolved_path = "/simulated/resolved-device",
  };
  int check_calls = 0;
  std::string checked_path;
};

TEST(SDStorageConfigurationTest, VirtualImageIsDefaultAndModesHaveStableSerializedValues)
{
  EXPECT_EQ(Config::MAIN_WII_SD_STORAGE_MODE.GetDefaultValue(),
            Config::WiiSDStorageMode::VirtualSDImage);

  Config::Layer layer(Config::LayerType::Base);
  ASSERT_TRUE(
      layer.Set(Config::MAIN_WII_SD_STORAGE_MODE, Config::WiiSDStorageMode::VirtualSDImage));
  EXPECT_EQ(layer.GetLayerMap().at(Config::MAIN_WII_SD_STORAGE_MODE.GetLocation()), "0");
  ASSERT_TRUE(
      layer.Set(Config::MAIN_WII_SD_STORAGE_MODE, Config::WiiSDStorageMode::PhysicalDeviceReadOnly));
  EXPECT_EQ(layer.GetLayerMap().at(Config::MAIN_WII_SD_STORAGE_MODE.GetLocation()), "1");
  EXPECT_EQ(layer.Get(Config::MAIN_WII_SD_STORAGE_MODE),
            Config::WiiSDStorageMode::PhysicalDeviceReadOnly);
}

TEST(SDStorageConfigurationTest, PhysicalPathIsIndependentFromImagePath)
{
  Config::Layer layer(Config::LayerType::Base);
  ASSERT_TRUE(layer.Set(Config::MAIN_WII_SD_CARD_IMAGE_PATH, "/temporary/image.raw"));
  ASSERT_TRUE(layer.Set(Config::MAIN_WII_SD_PHYSICAL_DEVICE_PATH, "/simulated/device"));

  EXPECT_EQ(layer.Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH), "/temporary/image.raw");
  EXPECT_EQ(layer.Get(Config::MAIN_WII_SD_PHYSICAL_DEVICE_PATH), "/simulated/device");
}

TEST(SDStorageConfigurationTest, InvalidModeFallsBackToVirtualImage)
{
  Config::Layer layer(Config::LayerType::Base);
  ASSERT_TRUE(layer.Set(Config::MAIN_WII_SD_STORAGE_MODE.GetLocation(), std::string{"99"}));
  EXPECT_EQ(Config::ValidateWiiSDStorageMode(layer.Get(Config::MAIN_WII_SD_STORAGE_MODE)),
            Config::WiiSDStorageMode::VirtualSDImage);
}

TEST(SDStorageSelectionTest, ImageModeSelectsOnlyImageBackend)
{
  FakeSDStorageBackendFactory factory;
  FakePhysicalSDPreflight preflight;
  int image_creation_calls = 0;
  SDStorageOpenResult result = OpenConfiguredSDStorage(
      Config::WiiSDStorageMode::VirtualSDImage, "/temporary/image.raw", "/simulated/device",
      factory, preflight, [&image_creation_calls] {
        ++image_creation_calls;
        return false;
      });

  EXPECT_EQ(result.result, SDStorageResult::Success);
  EXPECT_EQ(result.backend_kind, SDStorageBackendKind::ImageFile);
  EXPECT_EQ(factory.image_create_calls, 1);
  EXPECT_EQ(factory.physical_create_calls, 0);
  EXPECT_EQ(factory.image_path, "/temporary/image.raw");
  EXPECT_EQ(image_creation_calls, 0);
  EXPECT_EQ(preflight.check_calls, 0);
}

TEST(SDStorageSelectionTest, PhysicalModeSelectsOnlyReadOnlyBackend)
{
  FakeSDStorageBackendFactory factory;
  FakePhysicalSDPreflight preflight;
  int image_creation_calls = 0;
  SDStorageOpenResult result = OpenConfiguredSDStorage(
      Config::WiiSDStorageMode::PhysicalDeviceReadOnly, "/temporary/image.raw",
      "/simulated/device", factory, preflight, [&image_creation_calls] {
        ++image_creation_calls;
        return false;
      });

  ASSERT_EQ(result.result, SDStorageResult::Success);
  EXPECT_EQ(result.backend_kind, SDStorageBackendKind::PhysicalDeviceReadOnly);
  EXPECT_EQ(factory.image_create_calls, 0);
  EXPECT_EQ(factory.physical_create_calls, 1);
  EXPECT_EQ(factory.physical_path, "/simulated/device");
  EXPECT_EQ(image_creation_calls, 0);
  EXPECT_TRUE(result.storage->IsReadOnly());
  EXPECT_EQ(preflight.check_calls, 1);
  EXPECT_EQ(preflight.checked_path, "/simulated/device");
}

TEST(SDStorageSelectionTest, RawOpenFailureNeverFallsBackOrCreatesImage)
{
  FakeSDStorageBackendFactory factory;
  FakePhysicalSDPreflight preflight;
  factory.physical_open_result = SDStorageResult::PermissionDenied;
  int image_creation_calls = 0;
  SDStorageOpenResult result = OpenConfiguredSDStorage(
      Config::WiiSDStorageMode::PhysicalDeviceReadOnly, "/temporary/image.raw",
      "/simulated/device", factory, preflight, [&image_creation_calls] {
        ++image_creation_calls;
        return true;
      });

  EXPECT_EQ(result.result, SDStorageResult::PermissionDenied);
  EXPECT_EQ(factory.image_create_calls, 0);
  EXPECT_EQ(factory.physical_create_calls, 1);
  EXPECT_EQ(image_creation_calls, 0);
}

TEST(SDStorageSelectionTest, SuccessfulPreflightPreservesAuthoritativeBackendFailures)
{
  for (const SDStorageResult backend_result :
       {SDStorageResult::Busy, SDStorageResult::PermissionDenied, SDStorageResult::NoMedia})
  {
    FakeSDStorageBackendFactory factory;
    FakePhysicalSDPreflight preflight;
    factory.physical_open_result = backend_result;
    int image_creation_calls = 0;

    const SDStorageOpenResult result = OpenConfiguredSDStorage(
        Config::WiiSDStorageMode::PhysicalDeviceReadOnly, "/temporary/image.raw",
        "/simulated/device", factory, preflight, [&image_creation_calls] {
          ++image_creation_calls;
          return true;
        });

    EXPECT_EQ(result.preflight.result, PhysicalSDPreflightResult::Ready);
    EXPECT_EQ(result.result, backend_result);
    EXPECT_EQ(factory.physical_create_calls, 1);
    EXPECT_EQ(factory.image_create_calls, 0);
    EXPECT_EQ(image_creation_calls, 0);
  }
}

TEST(SDStorageSelectionTest, PreflightFailureNeverCreatesAnyBackendOrImage)
{
  FakeSDStorageBackendFactory factory;
  FakePhysicalSDPreflight preflight;
  preflight.outcome = {
      .result = PhysicalSDPreflightResult::Mounted,
      .resolved_path = "/simulated/resolved-device",
      .mount_point = "/mnt/simulated",
  };
  int image_creation_calls = 0;

  const SDStorageOpenResult result = OpenConfiguredSDStorage(
      Config::WiiSDStorageMode::PhysicalDeviceReadOnly, "/temporary/image.raw",
      "/simulated/device", factory, preflight, [&image_creation_calls] {
        ++image_creation_calls;
        return true;
      });

  EXPECT_EQ(result.result, SDStorageResult::Busy);
  EXPECT_EQ(factory.physical_create_calls, 0);
  EXPECT_EQ(factory.image_create_calls, 0);
  EXPECT_EQ(image_creation_calls, 0);
}

TEST(SDStorageSelectionTest, EmptyPhysicalPathDoesNotInvokeAnyFactory)
{
  FakeSDStorageBackendFactory factory;
  FakePhysicalSDPreflight preflight;
  preflight.outcome = {.result = PhysicalSDPreflightResult::EmptyPath};
  SDStorageOpenResult result = OpenConfiguredSDStorage(
      Config::WiiSDStorageMode::PhysicalDeviceReadOnly, "/temporary/image.raw", "", factory,
      preflight, [] { return true; });

  EXPECT_EQ(result.result, SDStorageResult::NoMedia);
  EXPECT_EQ(factory.image_create_calls, 0);
  EXPECT_EQ(factory.physical_create_calls, 0);
  EXPECT_EQ(preflight.check_calls, 1);
}

TEST(SDStorageSelectionTest, UnsupportedPlatformFailureIsDeterministic)
{
  FakeSDStorageBackendFactory factory;
  FakePhysicalSDPreflight preflight;
  preflight.outcome = {.result = PhysicalSDPreflightResult::UnsupportedPlatform};
  SDStorageOpenResult result = OpenConfiguredSDStorage(
      Config::WiiSDStorageMode::PhysicalDeviceReadOnly, "/temporary/image.raw",
      "/simulated/device", factory, preflight, [] { return false; });

  EXPECT_EQ(result.result, SDStorageResult::UnsupportedPlatform);
  EXPECT_EQ(result.backend_kind, SDStorageBackendKind::PhysicalDeviceReadOnly);
  EXPECT_EQ(factory.physical_create_calls, 0);
}

TEST(SDStorageWriteProtectionTest, ReadOnlyBackendCannotBeOverriddenOrReached)
{
  FakeConfiguredStorage storage(SDStorageResult::Success, true);
  constexpr std::array<u8, 1> data = {1};

  EXPECT_EQ(WriteToSDStorage(storage, true, 0, data), SDStorageResult::WriteProtected);
  EXPECT_EQ(GetSDIOWriteCommandResult(SDStorageResult::WriteProtected), -10);
  EXPECT_EQ(storage.write_calls, 0);
}

TEST(SDStorageWriteProtectionTest, ImageBackendFollowsAllowWritesSetting)
{
  FakeConfiguredStorage storage(SDStorageResult::Success, false);
  constexpr std::array<u8, 1> data = {1};

  EXPECT_EQ(WriteToSDStorage(storage, false, 0, data), SDStorageResult::WriteProtected);
  EXPECT_EQ(storage.write_calls, 0);
  EXPECT_EQ(WriteToSDStorage(storage, true, 0, data), SDStorageResult::Success);
  EXPECT_EQ(GetSDIOWriteCommandResult(SDStorageResult::Success), 0);
  EXPECT_EQ(storage.write_calls, 1);
}

TEST(SDStorageSettingsStateTest, RawModeDisablesImageControlsWithoutOpeningStorage)
{
  FakePhysicalSDPreflight preflight;
  const SDStorageModeControlState state =
      GetSDStorageModeControlState(Config::WiiSDStorageMode::PhysicalDeviceReadOnly);

  EXPECT_FALSE(state.image_controls_enabled);
  EXPECT_TRUE(state.physical_path_enabled);
  EXPECT_TRUE(state.read_only_warning_visible);
  EXPECT_EQ(preflight.check_calls, 0);
}

TEST(SDStorageSettingsStateTest, ReturningToImageModeRestoresImageControls)
{
  const SDStorageModeControlState state =
      GetSDStorageModeControlState(Config::WiiSDStorageMode::VirtualSDImage);

  EXPECT_TRUE(state.image_controls_enabled);
  EXPECT_FALSE(state.physical_path_enabled);
  EXPECT_FALSE(state.read_only_warning_visible);
}

TEST(SDStorageErrorTest, PhysicalFailuresHaveSpecificSafeReasons)
{
  SDStorageOpenResult result;
  result.backend_kind = SDStorageBackendKind::PhysicalDeviceReadOnly;

  result.preflight.result = PhysicalSDPreflightResult::PermissionDenied;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("permission"), std::string::npos);
  result.preflight = {.result = PhysicalSDPreflightResult::Mounted, .mount_point = "/mnt/card"};
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("mounted"), std::string::npos);
  result.preflight.result = PhysicalSDPreflightResult::Missing;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("missing"), std::string::npos);
  result.preflight.result = PhysicalSDPreflightResult::NotBlockDevice;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("block-device"), std::string::npos);

  result.preflight.result = PhysicalSDPreflightResult::Ready;
  result.result = SDStorageResult::UnsupportedSectorSize;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("512-byte"), std::string::npos);
  result.result = SDStorageResult::IoError;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("I/O"), std::string::npos);
}

TEST(SDStorageErrorTest, StartupMessageIncludesPathGuidanceAndNoWriteAssurance)
{
  SDStorageOpenResult result;
  result.result = SDStorageResult::Busy;
  result.backend_kind = SDStorageBackendKind::PhysicalDeviceReadOnly;
  result.preflight = {
      .result = PhysicalSDPreflightResult::Mounted,
      .mount_point = "/mnt/simulated card",
  };
  const std::string message = GetPhysicalSDStorageErrorMessage("/simulated/device", result);

  EXPECT_NE(message.find("Physical SD Device mode failed"), std::string::npos);
  EXPECT_NE(message.find("/simulated/device"), std::string::npos);
  EXPECT_NE(message.find("mounted"), std::string::npos);
  EXPECT_NE(message.find("Unmount"), std::string::npos);
  EXPECT_NE(message.find("do not eject"), std::string::npos);
  EXPECT_NE(message.find("Dolphin did not write to the card"), std::string::npos);
}

TEST(SDStorageErrorTest, PermissionMessageRejectsRootAndRequiresNarrowReadOnlyAccess)
{
  SDStorageOpenResult result;
  result.result = SDStorageResult::PermissionDenied;
  result.backend_kind = SDStorageBackendKind::PhysicalDeviceReadOnly;
  result.preflight = {.result = PhysicalSDPreflightResult::PermissionDenied};

  const std::string message = GetPhysicalSDStorageErrorMessage("/simulated/device", result);
  EXPECT_NE(message.find("Do not run Dolphin as root"), std::string::npos);
  EXPECT_NE(message.find("narrowly scoped read-only permissions"), std::string::npos);
  EXPECT_NE(message.find("Dolphin did not write to the card"), std::string::npos);
}

TEST(SDStorageErrorTest, BackendRaceFailuresRemainSpecific)
{
  SDStorageOpenResult result;
  result.backend_kind = SDStorageBackendKind::PhysicalDeviceReadOnly;
  result.preflight = {.result = PhysicalSDPreflightResult::Ready};

  result.result = SDStorageResult::Busy;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("authoritative exclusive open"),
            std::string::npos);
  result.result = SDStorageResult::PermissionDenied;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("authoritative read-only"),
            std::string::npos);
  result.result = SDStorageResult::NoMedia;
  EXPECT_NE(GetPhysicalSDStorageErrorReason(result).find("disappeared"), std::string::npos);
}
}  // namespace
}  // namespace IOS::HLE
