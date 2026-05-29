#pragma once

#include "core/types.h"
#include <string>
#include <utility>
#include <vector>

namespace renderdoc::core {

class Session;

/// Manages Android device interaction using the RenderDoc C API
/// (IDeviceProtocolController, target control, etc.) with ADB as a fallback
/// for device-side operations (layer setup, file pull).
class AndroidManager {
public:
    /// Check that the "adb" device protocol is available via RenderDoc.
    /// Throws CoreError(AdbNotFound) if not supported.
    static void ensureAdbAvailable();

    // ── Device discovery ──────────────────────────────────────────────

    /// List all ADB-connected Android devices via the RenderDoc device protocol.
    static std::vector<AndroidDeviceInfo> listDevices();

    /// Get detailed info for a single device by serial.
    static AndroidDeviceInfo getDeviceInfo(const std::string& serial);

    // ── RenderDoc provisioning ────────────────────────────────────────

    /// Check whether a device already has RenderDoc provisioned.
    static bool isProvisioned(const std::string& serial);

    /// Provision a device with RenderDoc interceptor libraries via ADB.
    static void provisionDevice(const std::string& serial);

    // ── App lifecycle ─────────────────────────────────────────────────

    /// Start an Android app with the RenderDoc Vulkan layer enabled.
    static void startApp(const std::string& serial, const std::string& packageName,
                         const std::string& activityName = "");

    /// Stop (force-quit) the target app on the device.
    static void stopApp(const std::string& serial, const std::string& packageName);

    // ── Capture ───────────────────────────────────────────────────────

    /// Trigger a RenderDoc capture on the running app using RenderDoc target control.
    /// Returns the remote path of the captured .rdc file on the device.
    static std::string triggerCapture(const std::string& serial);

    /// Pull a .rdc file from the device to a local path.
    static std::string pullCapture(const std::string& serial,
                                   const std::string& remotePath,
                                   const std::string& localPath = "");

    /// Push a local .rdc file to the device for remote replay.
    /// Returns the remote path on the device.
    static std::string pushCapture(const std::string& serial,
                                   const std::string& localPath,
                                   const std::string& remotePath = "");

    // ── Remote server ─────────────────────────────────────────────────

    /// Start renderdoc remoteserver on the device and set up port forwarding.
    /// Returns a connection URL usable with RemoteConnection.
    static AndroidRemoteServerResult startRemoteServer(
        const std::string& serial, int remotePort = 39920, int localPort = 0);

    /// Stop the remote server process on the device and remove port forwarding.
    static void stopRemoteServer(const std::string& serial,
                                 const AndroidRemoteServerResult& serverState);

    // ── Combined capture workflow ─────────────────────────────────────

    /// Full capture workflow: start app, wait, trigger capture, pull .rdc, auto-open.
    static AndroidCaptureResult captureFrame(Session& session,
                                              const AndroidCaptureRequest& req);

private:
    /// Run a command and capture stdout. Returns {exitCode, stdout}.
    static std::pair<int, std::string> runCommand(const std::string& cmd);

    /// Strip trailing whitespace/newlines from a string.
    static std::string trim(const std::string& s);

    /// Return the resolved ADB executable path.
    static std::string findAdb();

    /// Build an ADB command string with -s <serial> prefix.
    static std::string adbShell(const std::string& serial, const std::string& cmd);

    /// Ensure RenderDoc replay is initialized and get the device protocol controller.
    /// Cached after first call.
    static void* ensureDeviceProtocol();
};

} // namespace renderdoc::core
