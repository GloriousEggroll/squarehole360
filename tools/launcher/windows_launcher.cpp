// Windows package launchers, built twice from this file: squarehole360.exe
// (asset preflight, setup wizard on first run, then the game with default
// paths inside the package, like tools/run-windows.bat) and, with
// SQUAREHOLE_SETTINGS_LAUNCHER, Settings.exe (the settings editor). Both sit at
// the package root, run without a console and work under Proton/Wine.
#include <windows.h>
#include <intrin.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {
std::wstring Directory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

std::wstring ModuleDirectory() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length < path.size()) {
      path.resize(length);
      return Directory(path);
    }
    path.resize(path.size() * 2);
  }
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// CommandLineToArgvW's inverse, so arguments with spaces and quotes survive.
std::wstring Quote(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
  std::wstring quoted = L"\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < argument.size() && argument[i] == L'\\') ++i, ++backslashes;
    if (i == argument.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (argument[i] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(argument[i]);
  }
  return quoted + L"\"";
}

// Runs a program and waits for it; returns its exit code, or -1 when it could
// not start.
int Run(const std::wstring& program, const std::vector<std::wstring>& arguments, const std::wstring& directory,
        bool hidden) {
  std::wstring command_line = Quote(program);
  for (const auto& argument : arguments) command_line += L" " + Quote(argument);
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                      hidden ? CREATE_NO_WINDOW : 0, nullptr, directory.c_str(), &startup, &process)) {
    return -1;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

int Fail(const std::wstring& message) {
  MessageBoxW(nullptr, message.c_str(), L"squarehole360", MB_OK | MB_ICONERROR);
  return 1;
}

bool StartsWith(const std::wstring& text, const std::wstring& prefix) { return text.rfind(prefix, 0) == 0; }

// gow3.exe is built with FMA3 instructions; without them it would stop with
// an illegal instruction instead of a message.
bool CpuHasFma() {
  int info[4];
  __cpuid(info, 1);
  const bool fma = info[2] & (1 << 12), osxsave = info[2] & (1 << 27), avx = info[2] & (1 << 28);
  return fma && osxsave && avx && (_xgetbv(0) & 0x6) == 0x6;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  const std::wstring root = ModuleDirectory();
  // pythonw.exe keeps the wizard and settings editor free of a console window;
  // python.exe runs the hidden asset check.
  const std::wstring pythonw = root + L"\\python\\pythonw.exe";
  const std::wstring python = root + L"\\python\\python.exe";

#ifdef SQUAREHOLE_SETTINGS_LAUNCHER
  if (!Exists(pythonw)) return Fail(L"Missing " + pythonw + L".\nUse the complete Windows package.");
  return Run(pythonw, {root + L"\\tools\\settings\\app.py"}, root, false) == -1
             ? Fail(L"Could not start the settings editor.")
             : 0;
#else
  const std::wstring exe = root + L"\\build-windows\\bin\\gow3.exe";
  if (!Exists(exe)) return Fail(L"Missing " + exe + L".\nUse the complete Windows package.");
  if (!CpuHasFma()) {
    return Fail(L"This processor lacks FMA3 instructions, which squarehole360 needs.\n"
                L"Intel Core 4th generation (Haswell) or AMD FX/Ryzen and newer have them.");
  }

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> arguments(argv + (argc > 0 ? 1 : 0), argv + argc);
  LocalFree(argv);

  // CLI11 rejects repeated options, so a default is omitted when the caller
  // supplies it. A custom asset directory is honored by the preflight too.
  std::wstring game_data = root + L"\\game";
  bool has_game_data = false, has_user_data = false, has_log = false, has_config = false, check_assets = true;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];
    if (argument == L"--game_data_root" && i + 1 < arguments.size()) {
      has_game_data = true;
      game_data = arguments[i + 1];
    } else if (StartsWith(argument, L"--game_data_root=")) {
      has_game_data = true;
      game_data = argument.substr(17);
    }
    has_user_data |= StartsWith(argument, L"--user_data_root");
    has_log |= StartsWith(argument, L"--log_file");
    has_config |= StartsWith(argument, L"--gow3_config");
    if (argument == L"--help" || argument == L"-h" || argument == L"--version") check_assets = false;
  }

  if (check_assets && Exists(python)) {
    const std::vector<std::wstring> check{root + L"\\tools\\import-assets.py", L"check", L"--output", game_data};
    if (Run(python, check, root, true) != 0) {
      // First run or damaged files: the setup wizard imports your own copy of
      // the game, then optionally its DLC.
      Run(Exists(pythonw) ? pythonw : python, {root + L"\\tools\\setup-wizard.py", L"--output", game_data}, root,
          false);
      if (Run(python, check, root, true) != 0) {
        return Fail(L"The game files are not set up.\n\nStart squarehole360 again to run the setup wizard, or "
                    L"import your own ISO:\n  python\\python.exe tools\\import-assets.py import \"C:\\path\\to\\gow3.iso\"");
      }
    }
  }
  CreateDirectoryW((root + L"\\user").c_str(), nullptr);
  CreateDirectoryW((root + L"\\logs").c_str(), nullptr);

  std::vector<std::wstring> game_arguments;
  if (!has_game_data) game_arguments.push_back(L"--game_data_root=" + game_data);
  if (!has_user_data) game_arguments.push_back(L"--user_data_root=" + root + L"\\user");
  if (!has_log) game_arguments.push_back(L"--log_file=" + root + L"\\logs\\gow3.log");
  if (!has_config) game_arguments.push_back(L"--gow3_config=" + root + L"\\config\\gow3.toml");
  game_arguments.insert(game_arguments.end(), arguments.begin(), arguments.end());
  const int code = Run(exe, game_arguments, root, false);
  return code == -1 ? Fail(L"Could not start " + exe + L".") : code;
#endif
}
