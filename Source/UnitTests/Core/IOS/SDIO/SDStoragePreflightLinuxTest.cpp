// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "Core/IOS/SDIO/SDStoragePreflight.h"
#include "Core/IOS/SDIO/SDStoragePreflightLinuxPrivate.h"

namespace IOS::HLE
{
namespace
{
constexpr int FAKE_FD = 73;

struct FakeNode
{
  mode_t mode = S_IFBLK | 0440;
  dev_t device = makedev(8, 17);
  int error = 0;
};

class FakeLinuxPhysicalSDPreflightOperations final
    : public detail::LinuxPhysicalSDPreflightOperations
{
public:
  int Lstat(const char* path, struct stat* status) override
  {
    ++lstat_calls;
    lstat_paths.emplace_back(path);
    const auto node = nodes.find(path);
    if (node == nodes.end())
    {
      errno = ENOENT;
      return -1;
    }
    if (node->second.error != 0)
    {
      errno = node->second.error;
      return -1;
    }

    *status = {};
    status->st_mode = node->second.mode;
    status->st_rdev = node->second.device;
    return 0;
  }

  std::optional<std::string> ReadLink(const char* path, int* error) override
  {
    ++readlink_calls;
    const auto target = symlinks.find(path);
    if (target == symlinks.end())
    {
      *error = readlink_error;
      return std::nullopt;
    }
    return target->second;
  }

  bool ReadMountInfo(std::string* contents, int* error) override
  {
    ++mountinfo_calls;
    if (!mountinfo_success)
    {
      *error = mountinfo_error;
      return false;
    }
    *contents = mountinfo;
    return true;
  }

  int OpenReadOnly(const char* path) override
  {
    ++open_calls;
    opened_path = path;
    if (open_result < 0)
      errno = open_error;
    return open_result;
  }

  int Close(int fd) override
  {
    ++close_calls;
    closed_fd = fd;
    if (close_result != 0)
      errno = close_error;
    return close_result;
  }

  std::unordered_map<std::string, FakeNode> nodes;
  std::unordered_map<std::string, std::string> symlinks;
  int readlink_error = EIO;
  bool mountinfo_success = true;
  int mountinfo_error = 0;
  std::string mountinfo = "20 1 0:1 / / rw - rootfs rootfs rw\n";
  int open_result = FAKE_FD;
  int open_error = 0;
  int close_result = 0;
  int close_error = 0;

  int lstat_calls = 0;
  int readlink_calls = 0;
  int mountinfo_calls = 0;
  int open_calls = 0;
  int close_calls = 0;
  int closed_fd = -1;
  std::string opened_path;
  std::vector<std::string> lstat_paths;
};

struct PreflightAndOperations
{
  std::unique_ptr<PhysicalSDPreflight> preflight;
  std::shared_ptr<FakeLinuxPhysicalSDPreflightOperations> operations;
};

PreflightAndOperations MakePreflight()
{
  auto operations = std::make_shared<FakeLinuxPhysicalSDPreflightOperations>();
  auto preflight = detail::CreateLinuxPhysicalSDPreflightWithOperations(operations);
  return {std::move(preflight), std::move(operations)};
}

void AddBlockDevice(FakeLinuxPhysicalSDPreflightOperations& operations, std::string path,
                    unsigned int device_major = 8, unsigned int device_minor = 17)
{
  operations.nodes.emplace(std::move(path),
                           FakeNode{.device = makedev(device_major, device_minor)});
}

TEST(SDStoragePreflightLinuxTest, EmptyPathDoesNotPerformAnyOperation)
{
  auto [preflight, operations] = MakePreflight();

  EXPECT_EQ(preflight->Check("").result, PhysicalSDPreflightResult::EmptyPath);
  EXPECT_EQ(operations->lstat_calls, 0);
  EXPECT_EQ(operations->mountinfo_calls, 0);
  EXPECT_EQ(operations->open_calls, 0);
}

TEST(SDStoragePreflightLinuxTest, MissingPathIsReported)
{
  auto [preflight, operations] = MakePreflight();

  EXPECT_EQ(preflight->Check("/simulated/missing").result, PhysicalSDPreflightResult::Missing);
  EXPECT_EQ(operations->mountinfo_calls, 0);
}

TEST(SDStoragePreflightLinuxTest, InspectionPermissionDeniedIsReported)
{
  auto [preflight, operations] = MakePreflight();
  operations->nodes["/simulated/denied"].error = EACCES;

  EXPECT_EQ(preflight->Check("/simulated/denied").result,
            PhysicalSDPreflightResult::PermissionDenied);
  EXPECT_EQ(operations->open_calls, 0);
}

TEST(SDStoragePreflightLinuxTest, StableSymlinkResolvesToBlockDevice)
{
  auto [preflight, operations] = MakePreflight();
  operations->nodes["/simulated/by-uuid/card-id"] = FakeNode{.mode = S_IFLNK | 0777};
  operations->symlinks["/simulated/by-uuid/card-id"] = "../../devices/card-partition";
  AddBlockDevice(*operations, "/devices/card-partition");

  const PhysicalSDPreflightOutcome outcome = preflight->Check("/simulated/by-uuid/card-id");
  EXPECT_EQ(outcome.result, PhysicalSDPreflightResult::Ready);
  EXPECT_EQ(outcome.resolved_path, "/devices/card-partition");
  EXPECT_EQ(outcome.device_identity, (PhysicalSDDeviceIdentity{8, 17}));
  EXPECT_EQ(operations->opened_path, "/devices/card-partition");
}

TEST(SDStoragePreflightLinuxTest, SymlinkLoopFailsSafely)
{
  auto [preflight, operations] = MakePreflight();
  operations->nodes["/simulated/first"] = FakeNode{.mode = S_IFLNK | 0777};
  operations->nodes["/simulated/second"] = FakeNode{.mode = S_IFLNK | 0777};
  operations->symlinks["/simulated/first"] = "second";
  operations->symlinks["/simulated/second"] = "first";

  EXPECT_EQ(preflight->Check("/simulated/first").result, PhysicalSDPreflightResult::IoError);
  EXPECT_EQ(operations->mountinfo_calls, 0);
  EXPECT_EQ(operations->open_calls, 0);
}

TEST(SDStoragePreflightLinuxTest, FailedSymlinkResolutionIsReported)
{
  auto [preflight, operations] = MakePreflight();
  operations->nodes["/simulated/link"] = FakeNode{.mode = S_IFLNK | 0777};
  operations->readlink_error = EACCES;

  EXPECT_EQ(preflight->Check("/simulated/link").result,
            PhysicalSDPreflightResult::PermissionDenied);
}

TEST(SDStoragePreflightLinuxTest, RejectsRegularFileDirectoryAndCharacterDevice)
{
  for (const mode_t mode :
       {S_IFREG | 0600, S_IFDIR | 0700, S_IFCHR | 0600, S_IFSOCK | 0600})
  {
    auto [preflight, operations] = MakePreflight();
    operations->nodes["/simulated/not-block"] = FakeNode{.mode = mode};

    EXPECT_EQ(preflight->Check("/simulated/not-block").result,
              PhysicalSDPreflightResult::NotBlockDevice);
    EXPECT_EQ(operations->mountinfo_calls, 0);
    EXPECT_EQ(operations->open_calls, 0);
  }
}

TEST(SDStoragePreflightLinuxTest, AcceptsUnmountedBlockDevice)
{
  auto [preflight, operations] = MakePreflight();
  AddBlockDevice(*operations, "/simulated/block");

  const PhysicalSDPreflightOutcome outcome = preflight->Check("/simulated/block");
  EXPECT_EQ(outcome.result, PhysicalSDPreflightResult::Ready);
  EXPECT_EQ(outcome.resolved_path, "/simulated/block");
  EXPECT_EQ(outcome.device_identity, (PhysicalSDDeviceIdentity{8, 17}));
  EXPECT_EQ(operations->mountinfo_calls, 1);
  EXPECT_EQ(operations->open_calls, 1);
  EXPECT_EQ(operations->close_calls, 1);
  EXPECT_EQ(operations->closed_fd, FAKE_FD);
}

TEST(SDStoragePreflightLinuxTest, DetectsMountedDeviceByMajorMinorUnderDifferentPath)
{
  auto [preflight, operations] = MakePreflight();
  AddBlockDevice(*operations, "/simulated/configured", 8, 33);
  operations->mountinfo = "41 30 8:33 / /mnt/card rw,nosuid - vfat /different/source rw\n";

  const PhysicalSDPreflightOutcome outcome = preflight->Check("/simulated/configured");
  EXPECT_EQ(outcome.result, PhysicalSDPreflightResult::Mounted);
  EXPECT_EQ(outcome.mount_point, "/mnt/card");
  EXPECT_EQ(outcome.device_identity, (PhysicalSDDeviceIdentity{8, 33}));
  EXPECT_EQ(operations->open_calls, 0);
}

TEST(SDStoragePreflightLinuxTest, DetectsMountedDeviceThroughStableSymlink)
{
  auto [preflight, operations] = MakePreflight();
  operations->nodes["/simulated/by-uuid/card-id"] = FakeNode{.mode = S_IFLNK | 0777};
  operations->symlinks["/simulated/by-uuid/card-id"] = "/devices/card-partition";
  AddBlockDevice(*operations, "/devices/card-partition", 8, 41);
  operations->mountinfo =
      "45 30 8:41 / /mnt/card rw,nosuid - vfat /another/device-name rw\n";

  const PhysicalSDPreflightOutcome outcome = preflight->Check("/simulated/by-uuid/card-id");
  EXPECT_EQ(outcome.result, PhysicalSDPreflightResult::Mounted);
  EXPECT_EQ(outcome.resolved_path, "/devices/card-partition");
  EXPECT_EQ(outcome.mount_point, "/mnt/card");
  EXPECT_EQ(outcome.device_identity, (PhysicalSDDeviceIdentity{8, 41}));
  EXPECT_EQ(operations->open_calls, 0);
}

TEST(SDStoragePreflightLinuxTest, DecodesEscapedMountedPath)
{
  auto [preflight, operations] = MakePreflight();
  AddBlockDevice(*operations, "/simulated/block", 8, 49);
  operations->mountinfo =
      "51 30 8:49 / /mnt/My\\040SD\\040Card rw,nosuid - vfat /source rw\n";

  const PhysicalSDPreflightOutcome outcome = preflight->Check("/simulated/block");
  EXPECT_EQ(outcome.result, PhysicalSDPreflightResult::Mounted);
  EXPECT_EQ(outcome.mount_point, "/mnt/My SD Card");
}

TEST(SDStoragePreflightLinuxTest, IgnoresUnrelatedMountedDevice)
{
  auto [preflight, operations] = MakePreflight();
  AddBlockDevice(*operations, "/simulated/block", 8, 17);
  operations->mountinfo = "61 30 8:18 / /mnt/other rw - vfat /source rw\n";

  EXPECT_EQ(preflight->Check("/simulated/block").result, PhysicalSDPreflightResult::Ready);
  EXPECT_EQ(operations->open_calls, 1);
}

TEST(SDStoragePreflightLinuxTest, MalformedOrUnavailableMountInfoFailsSafely)
{
  {
    auto [preflight, operations] = MakePreflight();
    AddBlockDevice(*operations, "/simulated/block");
    operations->mountinfo = "not valid mountinfo\n";
    EXPECT_EQ(preflight->Check("/simulated/block").result,
              PhysicalSDPreflightResult::IoError);
    EXPECT_EQ(operations->open_calls, 0);
  }

  {
    auto [preflight, operations] = MakePreflight();
    AddBlockDevice(*operations, "/simulated/block");
    operations->mountinfo_success = false;
    operations->mountinfo_error = ENOENT;
    EXPECT_EQ(preflight->Check("/simulated/block").result,
              PhysicalSDPreflightResult::IoError);
    EXPECT_EQ(operations->open_calls, 0);
  }
}

TEST(SDStoragePreflightLinuxTest, ReadOnlyProbeMapsPermissionBusyAndDisappearance)
{
  for (const auto& [error, expected] :
       {std::pair{EACCES, PhysicalSDPreflightResult::PermissionDenied},
        std::pair{EBUSY, PhysicalSDPreflightResult::BusyOrInUse},
        std::pair{ENODEV, PhysicalSDPreflightResult::Missing}})
  {
    auto [preflight, operations] = MakePreflight();
    AddBlockDevice(*operations, "/simulated/block");
    operations->open_result = -1;
    operations->open_error = error;

    EXPECT_EQ(preflight->Check("/simulated/block").result, expected);
    EXPECT_EQ(operations->close_calls, 0);
  }
}
}  // namespace
}  // namespace IOS::HLE
