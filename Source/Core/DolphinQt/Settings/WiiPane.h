// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

#include "Common/CommonTypes.h"

#if defined(__linux__) && !defined(ANDROID)
#include "Core/IOS/SDIO/SDStoragePreflight.h"
#endif

namespace Config
{
enum class WiiSDStorageMode : int;
}

class ConfigBool;
template <typename T>
class ConfigChoiceMap;
class ConfigChoiceU32;
class ConfigSliderU32;
class ConfigText;
class ConfigUserPath;
class QLabel;
class QListWidget;
class QPushButton;
class QString;
class QVBoxLayout;

class WiiPane : public QWidget
{
  Q_OBJECT
public:
  explicit WiiPane(QWidget* parent = nullptr);

private:
  void PopulateUSBPassthroughListWidget();
  void CreateLayout();
  void ConnectLayout();
  void CreateMisc();
  void CreateSDCard();
  void CreateWhitelistedUSBPassthroughDevices();
  void CreateWiiRemoteSettings();
  void UpdateSDCardControls();
#if defined(__linux__) && !defined(ANDROID)
  void RefreshPhysicalSDStatus();
  void OnUnmountPhysicalSD();
#endif

  void OnEmulationStateChanged(bool running);

  void ValidateSelectionState();

  void OnUSBWhitelistAddButton();
  void OnUSBWhitelistRemoveButton();

  void BrowseSDRaw();
  void BrowseSDSyncFolder();

  // Widgets
  QVBoxLayout* m_main_layout;

  // Misc Settings
  ConfigBool* m_screensaver_checkbox;
  ConfigBool* m_pal60_mode_checkbox;
  ConfigBool* m_connect_keyboard_checkbox;
  ConfigBool* m_wiilink_checkbox;
  ConfigChoiceU32* m_system_language_choice;
  QLabel* m_system_language_choice_label;
  ConfigChoiceMap<bool>* m_aspect_ratio_choice;
  QLabel* m_aspect_ratio_choice_label;
  ConfigChoiceU32* m_sound_mode_choice;
  QLabel* m_sound_mode_choice_label;

  // SD Card Settings
  ConfigBool* m_sd_card_checkbox;
  ConfigBool* m_allow_sd_writes_checkbox;
  ConfigBool* m_sync_sd_folder_checkbox;
  ConfigChoiceMap<u64>* m_sd_card_size_combo;
  ConfigUserPath* m_sd_raw_edit;
  ConfigUserPath* m_sd_sync_folder_edit;
  QLabel* m_sd_raw_label;
  QLabel* m_sd_sync_folder_label;
  QLabel* m_sd_card_size_label;
  QPushButton* m_sd_raw_open_button;
  QPushButton* m_sd_sync_folder_open_button;
  QPushButton* m_sd_pack_button;
  QPushButton* m_sd_unpack_button;
#if defined(__linux__) && !defined(ANDROID)
  ConfigChoiceMap<Config::WiiSDStorageMode>* m_sd_storage_mode_combo;
  ConfigText* m_sd_physical_device_edit;
  QLabel* m_sd_physical_device_label;
  QLabel* m_sd_physical_read_only_warning;
  QLabel* m_sd_physical_mount_status;
  QPushButton* m_sd_physical_unmount_button;
  IOS::HLE::PhysicalSDPreflightOutcome m_detected_physical_sd;
  bool m_sd_unmount_in_progress = false;
#endif
  bool m_is_running = false;

  // Whitelisted USB Passthrough Devices
  QListWidget* m_whitelist_usb_list;
  QPushButton* m_whitelist_usb_add_button;
  QPushButton* m_whitelist_usb_remove_button;

  // Wii Remote Settings
  QLabel* m_wiimote_sensor_position_label;
  ConfigChoiceMap<u32>* m_wiimote_ir_sensor_position;
  ConfigSliderU32* m_wiimote_ir_sensitivity;
  QLabel* m_wiimote_ir_sensitivity_label;
  ConfigSliderU32* m_wiimote_speaker_volume;
  QLabel* m_wiimote_speaker_volume_label;
  ConfigBool* m_wiimote_motor;
};
