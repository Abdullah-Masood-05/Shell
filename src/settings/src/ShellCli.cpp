#include "ShellCli.h"

#include "Registry.h"
#include "Text.h"

#include <shellapi.h>

#include <chrono>

namespace fs = std::filesystem;

namespace {

fs::path moduleDir()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

bool isFile(const fs::path &p)
{
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

struct Handle
{
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = nullptr;
    }
};

} // namespace

fs::path ShellCli::locate()
{
    auto local = moduleDir() / L"shell.exe";
    if (isFile(local))
        return local;
    for (const auto &dll : registry::registeredDllPaths()) {
        auto exe = fs::path(dll).parent_path() / L"shell.exe";
        if (isFile(exe))
            return exe;
    }
    return {};
}

bool ShellCli::available() const { return isFile(exe_); }

std::wstring ShellCli::quote(const std::wstring &arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    // CommandLineToArgvW rules: backslashes are literal unless they precede a quote.
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t slashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++slashes;
        }
        if (it == arg.end()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring ShellCli::commandLine(const std::vector<std::wstring> &args, bool withExe) const
{
    std::wstring cmd;
    if (withExe)
        cmd = quote(exe_.wstring());
    for (const auto &a : args) {
        if (!cmd.empty())
            cmd += L' ';
        cmd += quote(a);
    }
    if (!cmd.empty())
        cmd += L' ';
    cmd += L"-silent";
    return cmd;
}

CliResult ShellCli::run(const std::vector<std::wstring> &args, DWORD timeoutMs) const
{
    CliResult r;
    if (!available()) {
        r.error = L"shell.exe was not found next to NilesoftShellPro.exe or the registered shell.dll.";
        return r;
    }

    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE readRaw = nullptr, writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) {
        r.error = L"CreatePipe failed.";
        return r;
    }
    Handle readEnd(readRaw), writeEnd(writeRaw);
    SetHandleInformation(readEnd.h, HANDLE_FLAG_INHERIT, 0);

    Handle nul(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, nullptr));

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul.h;
    si.StartupInfo.hStdOutput = writeEnd.h;
    si.StartupInfo.hStdError = nul.h;

    // Only the two pipe/NUL handles are inherited.
    HANDLE inherit[2] = { writeEnd.h, nul.h };
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<std::byte> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                               sizeof inherit, nullptr, nullptr);
    if (haveAttrs)
        si.lpAttributeList = attrs;

    auto cmd = commandLine(args, true);
    auto dir = exe_.parent_path().wstring();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe_.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             nullptr, dir.c_str(), &si.StartupInfo, &pi);
    if (haveAttrs)
        DeleteProcThreadAttributeList(attrs);
    writeEnd.reset(); // the child holds the only write end now
    if (!ok) {
        r.error = L"Could not start shell.exe (error " + std::to_wstring(GetLastError()) + L").";
        return r;
    }
    r.started = true;
    Handle process(pi.hProcess), thread(pi.hThread);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[4096];
    bool exited = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(readEnd.h, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD got = 0;
            if (ReadFile(readEnd.h, buf, sizeof buf, &got, nullptr) && got)
                r.output.append(buf, got);
            continue;
        }
        if (exited)
            break; // drained
        if (WaitForSingleObject(process.h, 15) == WAIT_OBJECT_0) {
            exited = true; // one more pass to drain what is left
            continue;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(process.h, 1);
            WaitForSingleObject(process.h, 2000);
            r.timedOut = true;
            break;
        }
    }
    GetExitCodeProcess(process.h, &r.exitCode);
    return r;
}

CliResult ShellCli::runElevated(const std::vector<std::wstring> &args, HWND owner,
                                DWORD timeoutMs) const
{
    CliResult r;
    if (!available()) {
        r.error = L"shell.exe was not found.";
        return r;
    }
    auto params = commandLine(args, false);
    auto dir = exe_.parent_path().wstring();
    SHELLEXECUTEINFOW sei{ sizeof sei };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_.c_str();
    sei.lpParameters = params.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        r.cancelled = err == ERROR_CANCELLED;
        r.error = r.cancelled ? L"The administrator prompt was cancelled."
                              : L"Could not start shell.exe as administrator (error " +
                                    std::to_wstring(err) + L").";
        return r;
    }
    r.started = true;
    if (!sei.hProcess)
        return r;
    Handle process(sei.hProcess);
    if (WaitForSingleObject(process.h, timeoutMs) != WAIT_OBJECT_0) {
        r.timedOut = true;
        return r;
    }
    GetExitCodeProcess(process.h, &r.exitCode);
    return r;
}
