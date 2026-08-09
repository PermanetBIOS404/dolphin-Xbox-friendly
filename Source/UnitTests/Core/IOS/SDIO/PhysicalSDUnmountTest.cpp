// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/IOS/SDIO/PhysicalSDUnmount.h"

namespace IOS::HLE
{
namespace
{
constexpr PhysicalSDDeviceIdentity SELECTED_IDENTITY{8, 17};
constexpr PhysicalSDDeviceIdentity OTHER_IDENTITY{8, 18};

PhysicalSDPreflightOutcome MountedDevice()
{
  return {
      .result = PhysicalSDPreflightResult::Mounted,
      .resolved_path = "/simulated/card-partition",
      .mount_point = "/mnt/simulated-card",
      .device_identity = SELECTED_IDENTITY,
  };
}

PhysicalSDPreflightOutcome ReadyDevice()
{
  return {
      .result = PhysicalSDPreflightResult::Ready,
      .resolved_path = "/simulated/card-partition",
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

class FakeUnmountBackend final : public PhysicalSDUnmountBackend
{
public:
  PhysicalSDUnmountOutcome Unmount(const PhysicalSDUnmountRequest& request) override
  {
    requests.emplace_back(request);
    return outcome;
  }

  PhysicalSDUnmountOutcome outcome{
      .result = PhysicalSDUnmountResult::Success,
      .diagnostic = "simulated success",
  };
  std::vector<PhysicalSDUnmountRequest> requests;
};

TEST(PhysicalSDUnmountTest, MountedSelectedDeviceMakesActionAvailable)
{
  EXPECT_TRUE(IsPhysicalSDUnmountAvailable(MountedDevice()));
}

TEST(PhysicalSDUnmountTest, UnmountedSelectedDeviceDoesNotOfferAction)
{
  EXPECT_FALSE(IsPhysicalSDUnmountAvailable(ReadyDevice()));
}

TEST(PhysicalSDUnmountTest, MountedDeviceWithoutValidatedIdentityDoesNotOfferAction)
{
  PhysicalSDPreflightOutcome mounted = MountedDevice();
  mounted.device_identity.reset();
  EXPECT_FALSE(IsPhysicalSDUnmountAvailable(mounted));
}

TEST(PhysicalSDUnmountTest, SuccessfulUnmountUsesExactValidatedDeviceAndRechecks)
{
  SequencePreflight preflight({MountedDevice(), ReadyDevice()});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::Success);
  ASSERT_EQ(backend.requests.size(), 1u);
  EXPECT_EQ(backend.requests[0].resolved_path, "/simulated/card-partition");
  EXPECT_EQ(backend.requests[0].device_identity, SELECTED_IDENTITY);
  EXPECT_EQ(preflight.checked_paths,
            (std::vector<std::string>{"/simulated/by-uuid/card", "/simulated/by-uuid/card"}));
}

TEST(PhysicalSDUnmountTest, DeviceThatBecameUnmountedReturnsAlreadyUnmountedWithoutBackend)
{
  SequencePreflight preflight({ReadyDevice()});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::AlreadyUnmounted);
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDUnmountTest, BusyUnmountFailureIsPreservedAndDoesNotPostflight)
{
  SequencePreflight preflight({MountedDevice()});
  FakeUnmountBackend backend;
  backend.outcome = {
      .result = PhysicalSDUnmountResult::BusyOrRefused,
      .diagnostic = "simulated busy device",
  };

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::BusyOrRefused);
  EXPECT_EQ(outcome.diagnostic, "simulated busy device");
  EXPECT_EQ(preflight.checked_paths.size(), 1u);
}

TEST(PhysicalSDUnmountTest, GenericBackendFailureIsPreserved)
{
  SequencePreflight preflight({MountedDevice()});
  FakeUnmountBackend backend;
  backend.outcome = {
      .result = PhysicalSDUnmountResult::Failed,
      .diagnostic = "simulated failure",
  };

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::Failed);
  EXPECT_EQ(outcome.diagnostic, "simulated failure");
}

TEST(PhysicalSDUnmountTest, UnavailableBackendIsReported)
{
  SequencePreflight preflight({MountedDevice()});
  FakeUnmountBackend backend;
  backend.outcome = {
      .result = PhysicalSDUnmountResult::BackendUnavailable,
      .diagnostic = "simulated unavailable service",
  };

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::BackendUnavailable);
  EXPECT_EQ(backend.requests.size(), 1u);
}

TEST(PhysicalSDUnmountTest, DisappearingDeviceNeverReachesBackend)
{
  SequencePreflight preflight({{.result = PhysicalSDPreflightResult::Missing}});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDUnmountTest, ChangedDeviceIdentityNeverReachesBackend)
{
  PhysicalSDPreflightOutcome changed = MountedDevice();
  changed.device_identity = OTHER_IDENTITY;
  SequencePreflight preflight({changed});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDUnmountTest, ChangedResolvedPathNeverReachesBackend)
{
  PhysicalSDPreflightOutcome changed = MountedDevice();
  changed.resolved_path = "/simulated/replacement-partition";
  SequencePreflight preflight({changed});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
}

TEST(PhysicalSDUnmountTest, DeviceChangedAfterBackendSuccessIsReported)
{
  PhysicalSDPreflightOutcome replacement = ReadyDevice();
  replacement.device_identity = OTHER_IDENTITY;
  SequencePreflight preflight({MountedDevice(), replacement});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::DeviceChanged);
  EXPECT_EQ(backend.requests.size(), 1u);
}

TEST(PhysicalSDUnmountTest, SuccessfulBackendMustActuallyClearMountedState)
{
  SequencePreflight preflight({MountedDevice(), MountedDevice()});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::Failed);
  EXPECT_NE(outcome.diagnostic.find("still reports"), std::string::npos);
}

TEST(PhysicalSDUnmountTest, UnexpectedPathTextCannotReachPlatformBackend)
{
  constexpr char unexpected_path[] = "/dev/fake; touch /tmp/should-never-run";
  SequencePreflight preflight({{
      .result = PhysicalSDPreflightResult::NotBlockDevice,
      .resolved_path = unexpected_path,
  }});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Unmount, unexpected_path, MountedDevice(), preflight, backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::DeviceChanged);
  EXPECT_TRUE(backend.requests.empty());
  ASSERT_EQ(preflight.checked_paths.size(), 1u);
  EXPECT_EQ(preflight.checked_paths[0], unexpected_path);
}

TEST(PhysicalSDUnmountTest, CancelPerformsNoCheckAndNoUnmount)
{
  SequencePreflight preflight({MountedDevice()});
  FakeUnmountBackend backend;

  const PhysicalSDUnmountOutcome outcome = HandlePhysicalSDUnmountAction(
      PhysicalSDUnmountAction::Cancel, "/simulated/by-uuid/card", MountedDevice(), preflight,
      backend);

  EXPECT_EQ(outcome.result, PhysicalSDUnmountResult::Cancelled);
  EXPECT_TRUE(preflight.checked_paths.empty());
  EXPECT_TRUE(backend.requests.empty());
}
}  // namespace
}  // namespace IOS::HLE
