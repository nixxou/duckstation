// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

#undef min
#undef max
#include <windows.h>
#undef min
#undef max
class GunCon final : public Controller
{
public:
  enum class Binding : u8
  {
    Trigger = 0,
    A = 1,
    B = 2,
    ShootOffscreen = 3,
    ButtonCount = 4,

    RelativeLeft = 4,
    RelativeRight = 5,
    RelativeUp = 6,
    RelativeDown = 7,
    BindingCount = 8,
  };

  static const Controller::ControllerInfo INFO;

  GunCon(u32 index);
  ~GunCon() override;

  void SendComMessage(const std::string& message);
  static std::unique_ptr<GunCon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void LoadSettings(SettingsInterface& si, const char* section) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  void SetBindState(u32 index, float value, bool skipRegisterTrigger);
  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    XLSB,
    XMSB,
    YLSB,
    YMSB
  };

  void UpdatePosition();

  // 0..1, not -1..1.
  std::pair<float, float> GetAbsolutePositionFromRelativeAxes() const;
  bool CanUseSoftwareCursor() const;
  u32 GetSoftwarePointerIndex() const;
  void UpdateSoftwarePointerPosition();

  std::string m_cursor_path;
  float m_cursor_scale = 1.0f;
  u32 m_cursor_color = 0xFFFFFFFFu;
  float m_x_scale = 1.0f;

  float m_relative_pos[4] = {};

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  u16 m_position_x = 0;
  u16 m_position_y = 0;
  bool m_shoot_offscreen = false;
  bool m_has_relative_binds = false;


  bool useRecoil = false;
  int recoilMaxDelay = 100000;
  int recoilPoolSpeed = 10;
  void threadOutputs();
  std::thread* myThread = nullptr;
  bool quitThread = false;
  std::string active_game = "";
  int output_current = -1;
  int output_previous = -1;
  int port = -1;
  /*
  HANDLE hPipe = nullptr;
  bool pipeConnected = false;
  */
  bool isOutScreen = false;


  bool triggerIsActive = false;
  std::chrono::microseconds::rep triggerLastPress = 0;
  std::chrono::microseconds::rep triggerLastRelease = 0;
  std::chrono::microseconds::rep lastGunShot = 0;
  std::chrono::microseconds::rep nextGunShot = 0;
  int queueSizeGunshot = 0;
  long fullAutoDelay = 0;
  long multishotDelay = 0;

  int lastAmmo = INT32_MAX;
  int lastWeapon = 0;
  int lastCharged = 0;
  int lastOther1 = 0;
  int lastOther2 = 0;
  bool fullAutoActive = false;
  float m_gun4irComPort = 0;
  int gun4irComPort = 0;
  HANDLE serialPort;


  TransferState m_transfer_state = TransferState::Idle;
};
