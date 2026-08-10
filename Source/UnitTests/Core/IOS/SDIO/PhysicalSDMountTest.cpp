// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/IOS/SDIO/PhysicalSDMount.h"
#include "Core/IOS/SDIO/PhysicalSDUnmount.h"

namespace IOS::HLE
{
namespace
{
constexpr PhysicalSDDeviceIdentity SELECTED_IDENTITY{8, 17};
constexpr PhysicalSDDeviceIdentity OTHER_IDENTITY{8, 18};

PhysicalSDPreflightOutcome ReadyDevice()
{
  return {
      .result = PhysicalSDPreflightResult::Ready,
      .resolved_path = "/simulated/card-partition",
      .device_identity = SELECTED_IDENTITY,
  };
}

PhysicalSDPreflightOutcome MountedDevice(std::string mount_point = "/mnt/simulated-card")
{
  return {
      .result = PhysicalSDPreflightResult::Mounted,
      .resolved_path = "/simulated/card-partition",
      .mount_point = std::move(mount_point),
      .device_identity = SELECTED_IDENTITY,
  };
}

class SequencePreflight final : public PhysicalSDPreflight
{
public:
  explicit SequencePreflight(std::vector<PhysicalSDPreflightOutcome> outcomes)
      : m_outcomes(std::move(outcomes))
  {
  }

  PhysicalSDPreflightOutcome Check(const std::string& path) override
  {
    checked_paths.emplace_back(path);
    if (m_next >= m_outcomes.size())
      return {};
    return m_outcomes[m_next++];
  }

  std::vector<std::string> checked_paths;

private:
  std::vector<PhysicalSDPreflightOutcome> m_outcomes;
  size_t m_next = 0;
};

class FakeMountBackend final : public PhysicalSDMountBackend
{
public:
  PhysicalSDMountOutcome Mount(const PhysicalSDMountRequest& request) override
  {
    requests.emplace_back(request);
    return outcome;
  }

  PhysicalSDMountOutcome outcome{
      .result = PhysicalSDMountResult::Success,
      .mount_point = "/udisks/returned-mount-point",
      .diagnostic = "simulated success",
  };
  std::vector<PhysicalSDMountRequest> requests;
};

TEST(PhysicalSDMountTest, MountedDeviceOffersOnlyUnmountAction)
{
  EXPECT_FALSE(IsPhysicalSDMountAvailable(MountedDevice(), false));
  EXPECT_TRUE(IsPhysicalSDUnmountAvailable(MountedDevice()));
}

TEST(PhysicalSDMountTest, SafelyUnmountedDeviceOffersOnlyMountAction)
{
  EXPECT_TRUE(IsPhysicalSDMountAvailable(ReadyDevice(), false));
  EXPECT_FALSE(IsPhysicalSDUnmountAvailable(ReadyDevice()));
}

TEST(PhysicalSDMountTest, DeviceWithoutValidatedIdentityDoesNotOfferMountAction)
{
  PhysicalSDPreflightOutcome ready = ReadyDevice();
  ready.device_identity.reset();
  EXPECT_FALSE(IsPhysicalSDMountAvailable(ready, false));
}

TEST(PhysicalSDMountTest, SuccessfulMountUsesExactValidatedDeviceAndConfirmedMountPoint)
{
  SequencePreflight preflight({ReadyDevice(), MountedDevice("/media/user/WII_SD")});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::Success);
  EXPECT_EQ(outcome.mount_point, "/media/user/WII_SD");
  ASSERT_EQ(backend.requests.size(), 1u);
  EXPECT_EQ(backend.requests[0].resolved_path, "/simulated/card-partition");
  EXPECT_EQ(backend.requests[0].device_identity, SELECTED_IDENTITY);
  EXPECT_EQ(preflight.checked_paths,
            (std::vector<std::string>{"/simulated/by-uuid/card", "/simulated/by-uuid/card"}));
}

TEST(PhysicalSDMountTest, ReturnedMountPointIsRetainedWhenPostflightHasNoPath)
{
  SequencePreflight preflight({ReadyDevice(), MountedDevice("")});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::Success);
  EXPECT_EQ(outcome.mount_point, "/udisks/returned-mount-point");
}

TEST(PhysicalSDMountTest, DeviceThatBecameMountedReturnsAlreadyMountedWithoutBackend)
{
  SequencePreflight preflight({MountedDevice("/media/race")});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::AlreadyMounted);
  EXPECT_EQ(outcome.mount_point, "/media/race");
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDMountTest, BackendAlreadyMountedRaceIsConfirmedByPostflight)
{
  SequencePreflight preflight({ReadyDevice(), MountedDevice("/media/race")});
  FakeMountBackend backend;
  backend.outcome = {
      .result = PhysicalSDMountResult::AlreadyMounted,
      .diagnostic = "simulated race",
  };

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::AlreadyMounted);
  EXPECT_EQ(outcome.mount_point, "/media/race");
  EXPECT_EQ(backend.requests.size(), 1u);
}

TEST(PhysicalSDMountTest, GenericMountFailureIsPreservedAndSkipsPostflight)
{
  SequencePreflight preflight({ReadyDevice()});
  FakeMountBackend backend;
  backend.outcome = {
      .result = PhysicalSDMountResult::Failed,
      .diagnostic = "simulated failure",
  };

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::Failed);
  EXPECT_EQ(outcome.diagnostic, "simulated failure");
  EXPECT_EQ(preflight.checked_paths.size(), 1u);
}

TEST(PhysicalSDMountTest, UnavailableBackendIsReported)
{
  SequencePreflight preflight({ReadyDevice()});
  FakeMountBackend backend;
  backend.outcome = {
      .result = PhysicalSDMountResult::BackendUnavailable,
      .diagnostic = "simulated unavailable service",
  };

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::BackendUnavailable);
  EXPECT_EQ(backend.requests.size(), 1u);
}

TEST(PhysicalSDMountTest, PermissionFailureIsReported)
{
  SequencePreflight preflight({ReadyDevice()});
  FakeMountBackend backend;
  backend.outcome = {
      .result = PhysicalSDMountResult::PermissionDenied,
      .diagnostic = "simulated policy failure",
  };

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::PermissionDenied);
  EXPECT_EQ(backend.requests.size(), 1u);
}

TEST(PhysicalSDMountTest, DisappearingDeviceNeverReachesBackend)
{
  SequencePreflight preflight({{.result = PhysicalSDPreflightResult::Missing}});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDMountTest, ChangedDeviceIdentityNeverReachesBackend)
{
  PhysicalSDPreflightOutcome changed = ReadyDevice();
  changed.device_identity = OTHER_IDENTITY;
  SequencePreflight preflight({changed});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDMountTest, DeviceChangedAfterBackendSuccessIsReported)
{
  PhysicalSDPreflightOutcome replacement = MountedDevice();
  replacement.device_identity = OTHER_IDENTITY;
  SequencePreflight preflight({ReadyDevice(), replacement});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::DeviceChanged);
  EXPECT_EQ(backend.requests.size(), 1u);
}

TEST(PhysicalSDMountTest, SuccessfulBackendMustActuallyProduceMountedState)
{
  SequencePreflight preflight({ReadyDevice(), ReadyDevice()});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::Failed);
  EXPECT_NE(outcome.diagnostic.find("still reports"), std::string::npos);
}

TEST(PhysicalSDMountTest, ActiveRawDevicePreventsChecksAndMountRequest)
{
  SequencePreflight preflight({ReadyDevice()});
  FakeMountBackend backend;

  EXPECT_FALSE(IsPhysicalSDMountAvailable(ReadyDevice(), true));
  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, true, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::RawDeviceInUse);
  EXPECT_TRUE(preflight.checked_paths.empty());
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDMountTest, UnexpectedPathTextCannotReachPlatformBackend)
{
  constexpr char unexpected_path[] = "/dev/fake; touch /tmp/should-never-run";
  SequencePreflight preflight({{
      .result = PhysicalSDPreflightResult::NotBlockDevice,
      .resolved_path = unexpected_path,
  }});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Mount, false, unexpected_path, ReadyDevice(), preflight, backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
  ASSERT_EQ(preflight.checked_paths.size(), 1u);
  EXPECT_EQ(preflight.checked_paths[0], unexpected_path);
}

TEST(PhysicalSDMountTest, CancelPerformsNoCheckAndNoMount)
{
  SequencePreflight preflight({ReadyDevice()});
  FakeMountBackend backend;

  const PhysicalSDMountOutcome outcome = HandlePhysicalSDMountAction(
      PhysicalSDMountAction::Cancel, false, "/simulated/by-uuid/card", ReadyDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDMountResult::Cancelled);
  EXPECT_TRUE(preflight.checked_paths.empty());
  EXPECT_TRUE(backend.requests.empty());
}
}  // namespace
}  // namespace IOS::HLE
