#include "nogui_host_interface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/controller_interface.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/icon.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include <cinttypes>
#include <cmath>
Log_SetChannel(NoGUIHostInterface);

#ifdef WIN32
#include "frontend-common/d3d11_host_display.h"
#endif

NoGUIHostInterface::NoGUIHostInterface() = default;

NoGUIHostInterface::~NoGUIHostInterface() = default;

const char* NoGUIHostInterface::GetFrontendName() const
{
  return "DuckStation NoGUI Frontend";
}

bool NoGUIHostInterface::Initialize()
{
  SetUserDirectory();
  m_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());

  // TODO: Make command line.
  m_flags.force_fullscreen_ui = true;

  if (!CommonHostInterface::Initialize())
    return false;

  const bool start_fullscreen = m_flags.start_fullscreen || g_settings.start_fullscreen;
  if (!CreatePlatformWindow(start_fullscreen))
  {
    Log_ErrorPrintf("Failed to create platform window");
    return false;
  }

  if (!CreateDisplay())
  {
    Log_ErrorPrintf("Failed to create host display");
    DestroyPlatformWindow();
    return false;
  }

  if (m_fullscreen_ui_enabled)
  {
    FullscreenUI::SetDebugMenuAllowed(true);
    FullscreenUI::QueueGameListRefresh();
  }

  // process events to pick up controllers before updating input map
  PollAndUpdate();
  UpdateInputMap();
  return true;
}

void NoGUIHostInterface::Shutdown()
{
  DestroyDisplay();
  DestroyPlatformWindow();

  CommonHostInterface::Shutdown();
}

bool NoGUIHostInterface::CreateDisplay()
{
  std::optional<WindowInfo> wi = GetPlatformWindowInfo();
  if (!wi)
  {
    ReportError("Failed to get platform window info");
    return false;
  }

  Assert(!m_display);
  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      m_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef WIN32
    default:
#endif
      m_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
    default:
      m_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }

  if (!m_display->CreateRenderDevice(wi.value(), g_settings.gpu_adapter, g_settings.gpu_use_debug_device,
                                     g_settings.gpu_threaded_presentation) ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device,
                                         g_settings.gpu_threaded_presentation))
  {
    m_display->DestroyRenderDevice();
    m_display.reset();
    ReportError("Failed to create/initialize display render device");
    return false;
  }

  if (!CreateHostDisplayResources())
    Log_WarningPrint("Failed to create host display resources");

  return true;
}

void NoGUIHostInterface::DestroyDisplay()
{
  ReleaseHostDisplayResources();

  if (m_display)
    m_display->DestroyRenderDevice();

  m_display.reset();
}

bool NoGUIHostInterface::AcquireHostDisplay()
{
  // Handle renderer switch if required.
  const HostDisplay::RenderAPI render_api = m_display->GetRenderAPI();
  bool needs_switch = false;
  switch (g_settings.gpu_renderer)
  {
#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      needs_switch = (render_api != HostDisplay::RenderAPI::D3D11);
      break;
#endif

    case GPURenderer::HardwareVulkan:
      needs_switch = (render_api != HostDisplay::RenderAPI::Vulkan);
      break;

    case GPURenderer::HardwareOpenGL:
      needs_switch = (render_api != HostDisplay::RenderAPI::OpenGL && render_api != HostDisplay::RenderAPI::OpenGLES);
      break;

    case GPURenderer::Software:
    default:
      needs_switch = false;
      break;
  }

  if (needs_switch)
  {
    const bool was_fullscreen = IsFullscreen();

    DestroyDisplay();

    // We need to recreate the window, otherwise bad things happen...
    DestroyPlatformWindow();
    if (!CreatePlatformWindow(was_fullscreen))
      Panic("Failed to recreate platform window on GPU renderer switch");

    if (!CreateDisplay())
      Panic("Failed to recreate display on GPU renderer switch");
  }

  return true;
}

void NoGUIHostInterface::ReleaseHostDisplay()
{
  // restore vsync, since we don't want to burn cycles at the menu
  m_display->SetVSync(true);
}

void NoGUIHostInterface::RequestExit()
{
  m_quit_request = true;
}

void NoGUIHostInterface::PollAndUpdate()
{
  CommonHostInterface::PollAndUpdate();

  if (m_controller_interface)
    m_controller_interface->PollEvents();
}

void NoGUIHostInterface::Run()
{
  while (!m_quit_request)
  {
    RunCallbacks();
    PollAndUpdate();
    if (m_fullscreen_ui_enabled)
      FullscreenUI::SetImGuiNavInputs();

    ImGui::NewFrame();

    if (System::IsRunning())
    {
      if (m_display_all_frames)
        System::RunFrame();
      else
        System::RunFrames();

      UpdateControllerRumble();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }
    }

    // rendering
    {
      DrawImGuiWindows();
      ImGui::Render();
      ImGui::EndFrame();

      m_display->Render();

      if (System::IsRunning())
      {
        System::UpdatePerformanceCounters();

        if (m_throttler_enabled)
          System::Throttle();
      }
    }
  }

  // Save state on exit so it can be resumed
  if (!System::IsShutdown())
  {
    if (g_settings.save_state_on_exit)
      SaveResumeSaveState();
    DestroySystem();
  }
}

void NoGUIHostInterface::ReportMessage(const char* message)
{
  Log_InfoPrint(message);
  AddOSDMessage(message, 10.0f);
}

void NoGUIHostInterface::ReportError(const char* message)
{
  Log_ErrorPrint(message);

  if (!m_display)
    return;

  const bool was_in_frame = GImGui->FrameCount != GImGui->FrameCountEnded;
  if (was_in_frame)
    ImGui::EndFrame();

  bool done = false;
  while (!done)
  {
    RunCallbacks();
    PollAndUpdate();
    if (m_fullscreen_ui_enabled)
      FullscreenUI::SetImGuiNavInputs();

    ImGui::NewFrame();
    done = FullscreenUI::DrawErrorWindow(message);
    ImGui::EndFrame();
    m_display->Render();
  }

  if (was_in_frame)
    ImGui::NewFrame();
}

bool NoGUIHostInterface::ConfirmMessage(const char* message)
{
  Log_InfoPrintf("Confirm: %s", message);

  if (!m_display)
    return true;

  const bool was_in_frame = GImGui->FrameCount != GImGui->FrameCountEnded;
  if (was_in_frame)
    ImGui::EndFrame();

  bool done = false;
  bool result = true;
  while (!done)
  {
    RunCallbacks();
    PollAndUpdate();
    if (m_fullscreen_ui_enabled)
      FullscreenUI::SetImGuiNavInputs();

    ImGui::NewFrame();
    done = FullscreenUI::DrawConfirmWindow(message, &result);
    ImGui::EndFrame();
    m_display->Render();
  }

  if (was_in_frame)
    ImGui::NewFrame();

  return result;
}

void NoGUIHostInterface::RunLater(std::function<void()> callback)
{
  std::unique_lock<std::mutex> lock(m_queued_callbacks_lock);
  m_queued_callbacks.push_back(std::move(callback));
}

void NoGUIHostInterface::RunCallbacks()
{
  std::unique_lock<std::mutex> lock(m_queued_callbacks_lock);

  while (!m_queued_callbacks.empty())
  {
    auto callback = std::move(m_queued_callbacks.front());
    m_queued_callbacks.pop_front();
    lock.unlock();
    callback();
    lock.lock();
  }
}