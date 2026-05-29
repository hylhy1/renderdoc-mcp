#include "core/android.h"
#include "core/errors.h"
#include "core/session.h"

#include <renderdoc_replay.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace renderdoc::core {

namespace fs = std::filesystem;

// ── Private helpers ────────────────────────────────────────────────────

std::string AndroidManager::trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                                s[start] == '\r' || s[start] == '\n'))
        ++start;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r' || s[end - 1] == '\n'))
        --end;
    return s.substr(start, end - start);
}

std::pair<int, std::string> AndroidManager::runCommand(const std::string& cmd) {
    std::string fullCmd = cmd + " 2>&1";
#ifdef _WIN32
    FILE* pipe = _popen(fullCmd.c_str(), "r");
#else
    FILE* pipe = popen(fullCmd.c_str(), "r");
#endif
    if (!pipe)
        throw CoreError(CoreError::Code::AdbOperationFailed,
                        "Failed to execute: " + cmd);

    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        output += buffer;

#ifdef _WIN32
    int exitCode = _pclose(pipe);
#else
    int rawStatus = pclose(pipe);
    int exitCode = WIFEXITED(rawStatus) ? WEXITSTATUS(rawStatus) : -1;
#endif
    return {exitCode, output};
}

std::string AndroidManager::findAdb() {
    // 1. Try RenderDoc's bundled ADB
#ifdef _WIN32
    fs::path rdDir = "C:/Program Files/RenderDoc";
    if (fs::exists(rdDir / "renderdoccmd.exe")) {
        auto candidate = rdDir / "plugins" / "android" / "adb.exe";
        if (fs::exists(candidate)) return candidate.string();
    }
#endif
    // 2. Try next to this executable
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(),
                                    static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        buf.resize(len);
        auto exeDir = fs::path(buf).parent_path();
        auto candidate = exeDir / "adb.exe";
        if (fs::exists(candidate)) return candidate.string();
    }
#endif
    return "adb";
}

std::string AndroidManager::adbShell(const std::string& serial, const std::string& cmd) {
    // Use short path on Windows to avoid quoting issues
    std::string adb = findAdb();
#ifdef _WIN32
    // If adb contains spaces, use double-quote wrapping
    if (adb.find(' ') != std::string::npos)
        adb = "\"" + adb + "\"";
#endif
    return adb + " -s " + serial + " " + cmd;
}

void* AndroidManager::ensureDeviceProtocol() {
    static IDeviceProtocolController* controller = nullptr;
    static bool tried = false;

    if (tried) {
        if (!controller)
            throw CoreError(CoreError::Code::AdbNotFound,
                            "Android device protocol not available");
        return controller;
    }

    tried = true;

    // Initialize replay subsystem (must be done exactly once)
    static bool replayInit = false;
    if (!replayInit) {
        GlobalEnvironment env;
        memset(&env, 0, sizeof(env));
        rdcarray<rdcstr> args;
        RENDERDOC_InitialiseReplay(env, args);
        replayInit = true;
    }

    // Check available protocols
    rdcarray<rdcstr> protocols;
    RENDERDOC_GetSupportedDeviceProtocols(&protocols);

    bool hasAdb = false;
    for (const auto& p : protocols) {
        if (std::string(p.c_str()) == "adb") { hasAdb = true; break; }
    }

    if (!hasAdb) {
        throw CoreError(CoreError::Code::AdbNotFound,
                        "RenderDoc does not support the ADB device protocol.");
    }

    controller = RENDERDOC_GetDeviceProtocolController(rdcstr("adb"));
    if (!controller) {
        throw CoreError(CoreError::Code::AdbNotFound,
                        "Failed to get ADB device protocol controller");
    }

    std::fprintf(stderr, "[renderdoc-mcp] ADB device protocol initialized\n");
    return controller;
}

// ── ADB availability ───────────────────────────────────────────────────

void AndroidManager::ensureAdbAvailable() {
    static bool available = false;
    if (available) return;

    ensureDeviceProtocol(); // This will throw if ADB protocol is not available
    available = true;
}

// ── Device discovery ───────────────────────────────────────────────────

std::vector<AndroidDeviceInfo> AndroidManager::listDevices() {
    std::vector<AndroidDeviceInfo> devices;
    auto* ctrl = static_cast<IDeviceProtocolController*>(ensureDeviceProtocol());

    auto deviceList = ctrl->GetDevices();
    for (const auto& host : deviceList) {
        std::string hostStr = host.c_str();
        std::string url = "adb://" + hostStr;

        AndroidDeviceInfo info;
        info.serial = hostStr;
        info.isAuthorized = true;
        info.status = "device";

        // Get friendly name
        auto friendly = ctrl->GetFriendlyName(rdcstr(url.c_str()));
        info.model = friendly.c_str();

        // Get additional info via adb getprop
        std::string adbExe = findAdb();
        auto runProp = [&](const std::string& prop) -> std::string {
            auto [rc, out] = runCommand(adbShell(hostStr, "shell getprop " + prop));
            return (rc == 0) ? trim(out) : "";
        };

        info.product = runProp("ro.product.name");
        info.device = runProp("ro.product.device");
        info.androidVersion = runProp("ro.build.version.release");

        devices.push_back(info);
    }

    return devices;
}

AndroidDeviceInfo AndroidManager::getDeviceInfo(const std::string& serial) {
    auto devices = listDevices();
    for (const auto& d : devices) {
        if (d.serial == serial) return d;
    }
    throw CoreError(CoreError::Code::AdbDeviceNotConnected,
                    "Device not found: " + serial);
}

// ── Provisioning ───────────────────────────────────────────────────────

bool AndroidManager::isProvisioned(const std::string& serial) {
    // Check if the RenderDoc APK is installed
    auto [rc1, out1] = runCommand(
        adbShell(serial, "shell pm list packages org.renderdoc.renderdoccmd.arm64 2>/dev/null"));
    auto [rc2, out2] = runCommand(
        adbShell(serial, "shell pm list packages org.renderdoc.renderdoccmd.arm32 2>/dev/null"));
    return !trim(out1).empty() || !trim(out2).empty();
}

void AndroidManager::provisionDevice(const std::string& serial) {
    // RenderDoc's ADB protocol handles provisioning automatically.
    // We just install the APKs if needed.
    std::string adbExe = findAdb();

    // Find RenderDoc install dir for APK files
#ifdef _WIN32
    fs::path rdDir = "C:/Program Files/RenderDoc";
    auto apk64 = rdDir / "plugins" / "android" / "org.renderdoc.renderdoccmd.arm64.apk";
    auto apk32 = rdDir / "plugins" / "android" / "org.renderdoc.renderdoccmd.arm32.apk";

    if (fs::exists(apk64)) {
        std::fprintf(stderr, "[renderdoc-mcp] Installing RenderDoc ARM64 APK...\n");
        auto [rc, out] = runCommand(
            adbShell(serial, "install -r \"" + apk64.string() + "\""));
        if (rc != 0) {
            throw CoreError(CoreError::Code::AndroidProvisionFailed,
                            "Failed to install RenderDoc APK: " + trim(out));
        }
    } else {
        throw CoreError(CoreError::Code::AndroidProvisionFailed,
                        "RenderDoc APK not found. Reinstall RenderDoc for Android support.");
    }
#else
    throw CoreError(CoreError::Code::AndroidProvisionFailed,
                    "Provisioning is only supported on Windows currently");
#endif

    std::fprintf(stderr, "[renderdoc-mcp] Device provisioned: %s\n", serial.c_str());
}

// ── App lifecycle ──────────────────────────────────────────────────────

void AndroidManager::startApp(const std::string& serial,
                               const std::string& packageName,
                               const std::string& activityName) {
    ensureDeviceProtocol();
    std::string adbExe = findAdb();

    // Build activity target
    std::string target = packageName;
    if (!activityName.empty()) {
        std::string act = activityName;
        if (act.find(packageName + ".") == 0)
            act = "." + act.substr(packageName.size() + 1);
        else if (act[0] != '.' && act.find('.') == std::string::npos)
            act = "." + act;
        target = packageName + "/" + act;
    }

    // Start the RenderDoc Loader activity first (creates the communication socket)
    // Then use the Loader's "start" command to launch the target app
    // This mirrors how qrenderdoc launches Android apps
    std::fprintf(stderr, "[renderdoc-mcp] Starting RenderDoc Loader on device...\n");

    // Force-stop any existing Loader to get a clean state
    runCommand(adbShell(serial,
               "shell am force-stop org.renderdoc.renderdoccmd.arm64"));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start the Loader with the "start <package/activity>" command
    std::string loaderCmd = "start " + target;
    // Use single quotes inside the adb shell command to avoid nesting issues
    std::string startCmd =
        "shell am start -n org.renderdoc.renderdoccmd.arm64/"
        "org.renderdoc.renderdoccmd.arm64.Loader"
        " -e renderdoccmd '" + loaderCmd + "'";

    std::fprintf(stderr, "[renderdoc-mcp] Launching via Loader: %s\n", loaderCmd.c_str());

    auto [rc, out] = runCommand(adbShell(serial, startCmd));

    if (rc != 0) {
        throw CoreError(CoreError::Code::AndroidStartAppFailed,
                        "Failed to start via Loader: " + trim(out));
    }

    // Wait for the Loader to initialize and the app to start
    // The Loader creates a Unix abstract socket for communication
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    std::fprintf(stderr, "[renderdoc-mcp] App started via RenderDoc Loader\n");
}

void AndroidManager::stopApp(const std::string& serial, const std::string& packageName) {
    runCommand(adbShell(serial, "shell am force-stop " + packageName));
    // Clean up layer settings
    runCommand(adbShell(serial, "shell settings delete global enable_gpu_debug_layers"));
    runCommand(adbShell(serial, "shell settings delete global gpu_debug_app"));
    runCommand(adbShell(serial, "shell settings delete global gpu_debug_layers"));
    std::fprintf(stderr, "[renderdoc-mcp] App stopped: %s on %s\n",
                 packageName.c_str(), serial.c_str());
}

// ── Capture ────────────────────────────────────────────────────────────

std::string AndroidManager::triggerCapture(const std::string& serial) {
    std::string url = "adb://" + serial;

    std::fprintf(stderr, "[renderdoc-mcp] Enumerating targets on %s...\n", serial.c_str());

    // Enumerate remote targets — the function blocks for a timeout to scan
    uint32_t targetIdent = 0;
    uint32_t nextIdent = 0;

    // RENDERDOC_EnumerateRemoteTargets blocks per call.
    // Try up to 3 times with increasing wait between calls.
    for (int attempt = 0; attempt < 3 && targetIdent == 0; attempt++) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            nextIdent = 0; // restart enumeration
        }

        nextIdent = RENDERDOC_EnumerateRemoteTargets(
            rdcstr(url.c_str()), nextIdent);
        if (nextIdent != 0) {
            targetIdent = nextIdent;
            std::fprintf(stderr, "[renderdoc-mcp] Found target ident: %u\n", targetIdent);
            break;
        }
    }

    if (targetIdent == 0) {
        throw CoreError(CoreError::Code::AndroidCaptureFailed,
                        "No RenderDoc-instrumented process found on device " + serial + ". "
                        "Start the app with android_start_app first (which uses "
                        "RENDERDOC_ExecuteAndInject).");
    }

    // Connect to the target for control
    ITargetControl* ctrl = RENDERDOC_CreateTargetControl(
        rdcstr(url.c_str()), targetIdent, rdcstr("renderdoc-mcp"), true);

    if (!ctrl) {
        // Try nearby idents
        for (uint32_t offset = 1; offset <= 5; offset++) {
            ctrl = RENDERDOC_CreateTargetControl(
                rdcstr(url.c_str()), targetIdent + offset, rdcstr("renderdoc-mcp"), true);
            if (ctrl) break;
        }
    }

    if (!ctrl) {
        throw CoreError(CoreError::Code::AndroidCaptureFailed,
                        "Failed to connect to target on device " + serial);
    }

    struct CtrlGuard {
        ITargetControl* c;
        ~CtrlGuard() { if (c) c->Shutdown(); }
    };
    std::string capturePath;
    {
        CtrlGuard guard{ctrl};

        std::fprintf(stderr, "[renderdoc-mcp] Triggering capture...\n");
        ctrl->TriggerCapture(1);

        // Wait for NewCapture message
        bool captured = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!ctrl->Connected()) break;
            TargetControlMessage msg = ctrl->ReceiveMessage(nullptr);
            if (msg.type == TargetControlMessageType::NewCapture) {
                capturePath = std::string(msg.newCapture.path.c_str());
                captured = true;
                break;
            }
            if (msg.type == TargetControlMessageType::Disconnected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!captured || capturePath.empty()) {
            throw CoreError(CoreError::Code::AndroidCaptureFailed,
                            "Capture timed out or failed on device " + serial);
        }

        std::fprintf(stderr, "[renderdoc-mcp] Capture saved: %s\n", capturePath.c_str());
    }

    // Wait for file flush
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    return capturePath;
}

std::string AndroidManager::pullCapture(const std::string& serial,
                                        const std::string& remotePath,
                                        const std::string& localPath) {
    std::string destination = localPath;
    if (destination.empty()) {
        auto tempDir = fs::temp_directory_path() / "renderdoc-mcp";
        fs::create_directories(tempDir);

        auto fileName = fs::path(remotePath).filename();
        if (fileName.empty()) fileName = "android_capture.rdc";

        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &timeT);
#else
        localtime_r(&timeT, &tm);
#endif
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tm);
        destination = (tempDir / (std::string(timeBuf) + "_" + fileName.string())).string();
    }

    std::fprintf(stderr, "[renderdoc-mcp] Pulling capture: %s -> %s\n",
                 remotePath.c_str(), destination.c_str());

    auto [rc, out] = runCommand(
        adbShell(serial, "pull \"" + remotePath + "\" \"" + destination + "\""));
    if (rc != 0 || !fs::exists(destination)) {
        throw CoreError(CoreError::Code::AndroidPullFailed,
                        "Failed to pull capture: " + trim(out));
    }

    std::fprintf(stderr, "[renderdoc-mcp] Capture pulled: %s\n", destination.c_str());
    return destination;
}

// ── Remote server ──────────────────────────────────────────────────────

std::string AndroidManager::pushCapture(const std::string& serial,
                                         const std::string& localPath,
                                         const std::string& remotePath) {
    std::string destination = remotePath;
    if (destination.empty()) {
        // Auto-generate: /sdcard/<filename>
        auto fileName = fs::path(localPath).filename();
        if (fileName.empty()) fileName = "capture.rdc";
        destination = "/sdcard/" + fileName.string();
    }

    // Convert forward slashes to backslashes for ADB on Windows
    std::string local = localPath;
#ifdef _WIN32
    for (auto& c : local) if (c == '/') c = '\\';
#endif

    std::fprintf(stderr, "[renderdoc-mcp] Pushing capture to device: %s -> %s\n",
                 local.c_str(), destination.c_str());

    // Wrap the whole command in quotes for cmd /c on Windows
    std::string pushCmd = adbShell(serial, "push \"" + local + "\" " + destination);
#ifdef _WIN32
    pushCmd = "\"" + pushCmd + "\"";
#endif
    auto [rc, out] = runCommand(pushCmd);
    if (rc != 0) {
        throw CoreError(CoreError::Code::AndroidPushFailed,
                        "Failed to push capture to device: " + trim(out));
    }

    std::fprintf(stderr, "[renderdoc-mcp] Capture pushed: %s\n", destination.c_str());
    return destination;
}

AndroidRemoteServerResult AndroidManager::startRemoteServer(
    const std::string& serial, int remotePort, int localPort) {

    // Use the RenderDoc Loader activity to start remoteserver on the device.
    // This is the same mechanism qrenderdoc uses — start the Loader with
    // "remoteserver --port N" as the renderdoccmd extra.
    std::string cmd = "remoteserver --port " + std::to_string(remotePort);

    std::fprintf(stderr, "[renderdoc-mcp] Starting remote server via Loader: %s\n",
                 cmd.c_str());

    // Force-stop any existing Loader
    runCommand(adbShell(serial,
               "shell am force-stop org.renderdoc.renderdoccmd.arm64"));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start Loader with remoteserver command
    auto [rc, out] = runCommand(adbShell(serial,
        "shell am start -n org.renderdoc.renderdoccmd.arm64/"
        "org.renderdoc.renderdoccmd.arm64.Loader"
        " --es renderdoccmd '" + cmd + "'"));

    if (rc != 0) {
        throw CoreError(CoreError::Code::AndroidRemoteServerFailed,
                        "Failed to start Loader: " + trim(out));
    }

    // Wait for the Loader to start and create the socket
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Set up port forwarding via ADB to the renderdoc Unix abstract socket
    std::string remotePortStr = std::to_string(remotePort);
    std::string localPortStr = std::to_string(localPort);

    // ADB forward from local TCP to the device's abstract socket
    auto [fwdRc, fwdOut] = runCommand(
        adbShell(serial, "forward tcp:" + localPortStr +
                 " localabstract:renderdoc_" + remotePortStr));
    if (fwdRc != 0) {
        throw CoreError(CoreError::Code::AndroidRemoteServerFailed,
                        "Port forwarding failed: " + trim(fwdOut));
    }

    // Read back assigned local port.
    // Note: adb forward --list does NOT take -s serial; it lists all forwards.
    int actualLocalPort = localPort;
    std::string adbExe = findAdb();
    auto [portRc, portOut] = runCommand(
        "\"" + adbExe + "\" forward --list 2>&1");
    if (portRc == 0) {
        std::string search = serial + " tcp:";
        auto pos = portOut.find(search);
        if (pos != std::string::npos) {
            auto tcpStart = pos + search.size();
            auto tcpEnd = portOut.find(' ', tcpStart);
            std::string portStr = portOut.substr(tcpStart, tcpEnd - tcpStart);
            try { actualLocalPort = std::stoi(portStr); }
            catch (...) {}
        }
    }

    std::string connectionUrl = "localhost:" + std::to_string(actualLocalPort);

    std::fprintf(stderr, "[renderdoc-mcp] Remote server ready: %s\n", connectionUrl.c_str());

    return AndroidRemoteServerResult{serial, actualLocalPort, remotePort, connectionUrl};
}

void AndroidManager::stopRemoteServer(const std::string& serial,
                                       const AndroidRemoteServerResult& state) {
    runCommand(adbShell(serial, "forward --remove tcp:" + std::to_string(state.localPort)));
    runCommand(adbShell(serial, "shell pkill renderdoccmd"));
    std::fprintf(stderr, "[renderdoc-mcp] Remote server stopped on %s\n", serial.c_str());
}

// ── Combined capture workflow ──────────────────────────────────────────

AndroidCaptureResult AndroidManager::captureFrame(Session& session,
                                                   const AndroidCaptureRequest& req) {
    // 1. Validate device
    auto devices = listDevices();
    auto it = std::find_if(devices.begin(), devices.end(),
        [&](const auto& d) { return d.serial == req.deviceSerial; });
    if (it == devices.end())
        throw CoreError(CoreError::Code::AdbDeviceNotConnected,
                        "Device not found: " + req.deviceSerial);
    if (!it->isAuthorized)
        throw CoreError(CoreError::Code::AdbDeviceNotConnected,
                        "Device not authorized: " + req.deviceSerial);

    // 2. Start the app via ADB
    startApp(req.deviceSerial, req.packageName, req.activityName);

    // 3. Trigger capture (enumerates targets, connects, triggers)
    std::string remotePath = triggerCapture(req.deviceSerial);

    // 4. Pull
    std::string localPath = pullCapture(req.deviceSerial, remotePath, req.outputPath);

    // 5. Auto-open
    session.open(localPath);

    return AndroidCaptureResult{localPath, req.deviceSerial, req.packageName, true};
}

} // namespace renderdoc::core
