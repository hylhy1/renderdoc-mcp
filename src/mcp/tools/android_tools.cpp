#include "mcp/tools/tools.h"
#include "mcp/tool_registry.h"
#include "mcp/serialization.h"
#include "core/android.h"
#include "core/info.h"
#include "core/session.h"

namespace renderdoc::mcp::tools {

void registerAndroidTools(ToolRegistry& registry) {

    // ── 1. android_list_devices ──────────────────────────────────

    registry.registerTool({
        "android_list_devices",
        "List all ADB-connected Android devices with their model, Android version, "
        "and authorization status. Useful for discovering available capture targets.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](mcp::ToolContext& /*ctx*/, const nlohmann::json& /*args*/) -> nlohmann::json {
            auto devices = core::AndroidManager::listDevices();
            auto arr = nlohmann::json::array();
            for (const auto& d : devices) {
                arr.push_back({
                    {"serial", d.serial},
                    {"model", d.model},
                    {"product", d.product},
                    {"deviceName", d.device},
                    {"androidVersion", d.androidVersion},
                    {"status", d.status},
                    {"authorized", d.isAuthorized}
                });
            }
            return {{"devices", arr}, {"count", devices.size()}};
        }
    });

    // ── 2. android_provision ─────────────────────────────────────

    registry.registerTool({
        "android_provision",
        "Provision an Android device with the RenderDoc interceptor libraries. "
        "This must be done once per device before any captures can be taken. "
        "Requires renderdoccmd on the system PATH.",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial (from android_list_devices)"}}},
           {"force", {{"type", "boolean"},
                      {"description", "Re-provision even if already provisioned. "
                                      "Default: false"}}}}},
         {"required", {"serial"}}},
        [](mcp::ToolContext& /*ctx*/, const nlohmann::json& args) -> nlohmann::json {
            auto serial = args.at("serial").get<std::string>();
            bool force = args.value("force", false);

            if (!force && core::AndroidManager::isProvisioned(serial)) {
                return {{"status", "already_provisioned"}, {"serial", serial}};
            }

            core::AndroidManager::provisionDevice(serial);
            return {{"status", "provisioned"}, {"serial", serial}};
        }
    });

    // ── 3. android_start_app ─────────────────────────────────────

    registry.registerTool({
        "android_start_app",
        "Start an Android app with the RenderDoc interceptor layer enabled. "
        "The device must have been provisioned first (use android_provision).",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial (from android_list_devices)"}}},
           {"packageName", {{"type", "string"},
                            {"description", "Android app package name (e.g., "
                                            "com.example.vulkan_app)"}}},
           {"activityName", {{"type", "string"},
                             {"description", "Specific activity to launch. "
                                             "Uses default launcher activity if omitted."}}}}},
         {"required", {"serial", "packageName"}}},
        [](mcp::ToolContext& /*ctx*/, const nlohmann::json& args) -> nlohmann::json {
            auto serial = args.at("serial").get<std::string>();
            auto package = args.at("packageName").get<std::string>();
            auto activity = args.value("activityName", "");

            core::AndroidManager::startApp(serial, package, activity);
            return {
                {"status", "started"},
                {"serial", serial},
                {"package", package}
            };
        }
    });

    // ── 4. android_trigger_capture ───────────────────────────────

    registry.registerTool({
        "android_trigger_capture",
        "Trigger a RenderDoc capture on the currently running Android app. "
        "Returns the remote path of the captured .rdc file on the device. "
        "Use android_pull_capture to download it to the local filesystem.",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial"}}}}},
         {"required", {"serial"}}},
        [](mcp::ToolContext& /*ctx*/, const nlohmann::json& args) -> nlohmann::json {
            auto serial = args.at("serial").get<std::string>();
            auto remotePath = core::AndroidManager::triggerCapture(serial);
            return {
                {"status", "captured"},
                {"serial", serial},
                {"remotePath", remotePath}
            };
        }
    });

    // ── 5. android_pull_capture ──────────────────────────────────

    registry.registerTool({
        "android_pull_capture",
        "Pull a captured .rdc file from an Android device to the local filesystem. "
        "Optionally auto-opens it for replay analysis.",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial"}}},
           {"remotePath", {{"type", "string"},
                           {"description", "Remote path of the .rdc file on the device "
                                           "(from android_trigger_capture)"}}},
           {"outputPath", {{"type", "string"},
                           {"description", "Local path to save the .rdc file. "
                                           "Default: auto-generated in temp directory"}}},
           {"open", {{"type", "boolean"},
                     {"description", "Automatically open the pulled capture for replay. "
                                     "Default: true"}}}}},
         {"required", {"serial", "remotePath"}}},
        [](mcp::ToolContext& ctx, const nlohmann::json& args) -> nlohmann::json {
            auto serial = args.at("serial").get<std::string>();
            auto remotePath = args.at("remotePath").get<std::string>();
            auto outputPath = args.value("outputPath", "");
            bool open = args.value("open", true);

            auto localPath = core::AndroidManager::pullCapture(
                serial, remotePath, outputPath);

            nlohmann::json result = {
                {"status", "pulled"},
                {"serial", serial},
                {"localPath", localPath}
            };

            if (open) {
                ctx.session.open(localPath);
                auto info = core::getCaptureInfo(ctx.session);
                auto j = to_json(info);
                j["path"] = localPath;
                return j;
            }
            return result;
        }
    });

    // ── 6. android_capture_frame ─────────────────────────────────

    registry.registerTool({
        "android_capture_frame",
        "Complete Android capture workflow: start an app with RenderDoc interceptor, "
        "wait the specified number of frames, trigger a capture, pull the .rdc file "
        "from the device, and automatically open it for analysis. "
        "This combines android_start_app, android_trigger_capture, and "
        "android_pull_capture into a single operation.",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial (from android_list_devices)"}}},
           {"packageName", {{"type", "string"},
                            {"description", "Android app package name to launch "
                                            "(e.g., com.example.vulkan_app)"}}},
           {"activityName", {{"type", "string"},
                             {"description", "Specific activity to launch (optional)"}}},
           {"delayFrames", {{"type", "integer"},
                            {"description", "Number of frames to wait before capturing. "
                                            "Default: 100"}}},
           {"outputPath", {{"type", "string"},
                           {"description", "Local path for the pulled .rdc file. "
                                           "Default: auto-generated in temp directory"}}}}},
         {"required", {"serial", "packageName"}}},
        [](mcp::ToolContext& ctx, const nlohmann::json& args) -> nlohmann::json {
            auto& session = ctx.session;
            core::AndroidCaptureRequest req;
            req.deviceSerial = args.at("serial").get<std::string>();
            req.packageName = args.at("packageName").get<std::string>();
            req.activityName = args.value("activityName", "");
            req.delayFrames = args.value("delayFrames", 100);
            req.outputPath = args.value("outputPath", "");

            auto result = core::AndroidManager::captureFrame(session, req);

            // Session is now open — return same format as capture_frame
            auto info = core::getCaptureInfo(session);
            auto j = to_json(info);
            j["path"] = result.capturePath;
            j["deviceSerial"] = result.deviceSerial;
            j["packageName"] = result.packageName;
            return j;
        }
    });

    // ── 7. android_push_capture ──────────────────────────────────

    registry.registerTool({
        "android_push_capture",
        "Push a local .rdc capture file to an Android device for remote replay. "
        "After pushing, use android_start_remote_server + open_capture with the "
        "remote device path to replay on the device GPU.",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial"}}},
           {"localPath", {{"type", "string"},
                          {"description", "Local path to the .rdc file"}}},
           {"remotePath", {{"type", "string"},
                           {"description", "Remote path on device. "
                                           "Default: /sdcard/<filename>"}}}}},
         {"required", {"serial", "localPath"}}},
        [](mcp::ToolContext& /*ctx*/, const nlohmann::json& args) -> nlohmann::json {
            auto serial = args.at("serial").get<std::string>();
            auto localPath = args.at("localPath").get<std::string>();
            auto remotePath = args.value("remotePath", "");

            auto result = core::AndroidManager::pushCapture(serial, localPath, remotePath);
            return {
                {"status", "pushed"},
                {"serial", serial},
                {"remotePath", result}
            };
        }
    });

    // ── 8. android_start_remote_server ───────────────────────────

    registry.registerTool({
        "android_start_remote_server",
        "Start renderdoccmd remoteserver on an Android device and set up ADB port "
        "forwarding. Returns a connection URL that can be used with the --remote-url "
        "server flag to replay captures directly on the device GPU. "
        "After calling this tool, subsequent open_capture calls with remote URL "
        "configured will replay on the device.",
        {{"type", "object"},
         {"properties",
          {{"serial", {{"type", "string"},
                       {"description", "ADB device serial"}}},
           {"remotePort", {{"type", "integer"},
                           {"description", "Port for the remote server on the device. "
                                           "Default: 39920"}}},
           {"localPort", {{"type", "integer"},
                          {"description", "Local port for ADB forwarding. "
                                          "Default: auto-assign (recommended)"}}}}},
         {"required", {"serial"}}},
        [](mcp::ToolContext& ctx, const nlohmann::json& args) -> nlohmann::json {
            auto serial = args.at("serial").get<std::string>();
            int remotePort = args.value("remotePort", 39920);
            int localPort = args.value("localPort", 0);

            auto result = core::AndroidManager::startRemoteServer(
                serial, remotePort, localPort);

            // Configure the session for remote replay via the returned URL.
            // Also set remoteOpenDirect since the file is on the device
            // (no need for copyCapture — path is a device path like /sdcard/...).
            ctx.session.setRemoteUrl(result.connectionUrl);
            ctx.session.setRemoteOpenDirect(true);

            return {
                {"status", "started"},
                {"serial", result.deviceSerial},
                {"localPort", result.localPort},
                {"remotePort", result.remotePort},
                {"connectionUrl", result.connectionUrl}
            };
        }
    });
}

} // namespace renderdoc::mcp::tools
