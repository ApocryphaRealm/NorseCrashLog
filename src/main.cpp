// NorseCrashLog - crash reports that name the Norse frameworks' parts (UE4SS v4.0.0 C++ mod for NORSE: Oath of Blood),
// 2026-11-17. Modelled on Skyrim's Crash Logger. Unreal's own crash report names the game's code only; this one says
// which of our mods (and which function in it) the fault is in, names game functions through the Norse Address
// Library, and adds what was happening (the last NorseEvents events) and what was installed (NorseManifest's packs).
//
// How: a vectored exception handler sees every fault first. Only the serious kinds are written (access violation,
// illegal / privileged instruction, stack overflow, divide by zero, in-page error) - each entry says "may have been
// caught", because code that guards itself on purpose (our own Unreal text import, for one) raises the same faults and
// carries on; the last entry before the game dies is the crash. At most 20 entries per session, one per address.
// Report: NorseCrashLog.log (sLogDir in NorseCrashLog.ini). Console: ncl.test (a deliberate, caught fault - to check
// the report), ncl.where <rva hex> (what the Address Library calls a game address).
#include <Windows.h>
#include <DbgHelp.h>

#include "NorseAddressLibrary.hpp"
#include "NorseEvents.h"
#include "NorseManifest.hpp"
#include "Log.hpp"
#include "Ue.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace nrn;

namespace
{
    fs::path g_mod;
    std::mutex g_mx;
    std::deque<std::string> g_recent; // last NorseEvents events
    std::set<uintptr_t> g_seen;
    std::atomic<int> g_written{0};
    bool g_symbols = false;
    bool g_nal = false;

    fs::path ModDir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModDir), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
        return fs::path(buf).parent_path().parent_path();
    }

    const char* CodeName(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION (read/write of memory it may not touch)";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIVILEGED_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INTEGER_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        default: return nullptr;
        }
    }

    // "NorseHUD (mod) main.dll+0x1234 Draw+0x56" / "NorseSteam-Win64-Shipping.exe+0x5e32700 = ACombat_GM::OnUnitDeath (ID 2) +0x0"
    std::string Where(uintptr_t addr)
    {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &m) || !m)
        {
            char b[40];
            snprintf(b, sizeof(b), "0x%llx (no module)", static_cast<unsigned long long>(addr));
            return b;
        }
        wchar_t path[MAX_PATH * 2]{};
        GetModuleFileNameW(m, path, static_cast<DWORD>(std::size(path)));
        fs::path p(path);
        std::string who = p.filename().string();
        // a UE4SS C++ mod: ...\ue4ss\Mods\<Name>\dlls\main.dll
        if (p.parent_path().filename() == "dlls") who = p.parent_path().parent_path().filename().string() + " (mod) " + who;
        uintptr_t off = addr - reinterpret_cast<uintptr_t>(m);
        char b[64];
        snprintf(b, sizeof(b), "+0x%llx", static_cast<unsigned long long>(off));
        std::string out = who + b;
        if (m == GetModuleHandleW(nullptr) && g_nal)
        {
            // the nearest known function below, only when close: a fault deep in some other function must not borrow a name
            std::string n = nal::Library::Get().NameOf(addr);
            size_t plus = n.rfind(" +0x");
            if (!n.empty() && (plus == std::string::npos || std::strtoull(n.c_str() + plus + 4, nullptr, 16) < 0x1000)) out += " = " + n;
        }
        else if (g_symbols)
        {
            alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256]{};
            auto* si = reinterpret_cast<SYMBOL_INFO*>(sbuf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), addr, &disp, si))
            {
                snprintf(b, sizeof(b), "+0x%llx", static_cast<unsigned long long>(disp));
                out += std::string(" ") + si->Name + b;
            }
        }
        return out;
    }

    void InitSymbols()
    {
        if (g_symbols) return;
        // our mods ship their .pdb beside main.dll; the game exe has none (its functions are named by the Address Library)
        std::string path;
        std::error_code ec;
        for (const auto& d : fs::directory_iterator(g_mod.parent_path(), ec))
        {
            fs::path dlls = d.path() / "dlls";
            if (fs::exists(dlls / "main.pdb", ec)) path += (path.empty() ? "" : ";") + dlls.string();
        }
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        g_symbols = SymInitialize(GetCurrentProcess(), path.c_str(), TRUE) != FALSE;
    }

    void Report(EXCEPTION_POINTERS* ep, const char* code)
    {
        CONTEXT ctx = *ep->ContextRecord;
        uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        InitSymbols();
        std::string out = "\n================ NorseCrashLog: ";
        out += code;
        out += " - may have been caught; the last entry before the game closes is the crash\n";
        out += "at   " + Where(at) + "\n";
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        {
            char b[96];
            snprintf(b, sizeof(b), "     %s address 0x%llx\n", ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : "writing",
                     static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
            out += b;
        }
        char regs[256];
        snprintf(regs, sizeof(regs), "regs rcx 0x%llx rdx 0x%llx r8 0x%llx r9 0x%llx rsp 0x%llx\nstack:\n", static_cast<unsigned long long>(ctx.Rcx),
                 static_cast<unsigned long long>(ctx.Rdx), static_cast<unsigned long long>(ctx.R8), static_cast<unsigned long long>(ctx.R9),
                 static_cast<unsigned long long>(ctx.Rsp));
        out += regs;
        STACKFRAME64 f{};
        f.AddrPC.Offset = ctx.Rip;
        f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp;
        f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp;
        f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 32; ++i)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &f, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) ||
                !f.AddrPC.Offset)
                break;
            char n[8];
            snprintf(n, sizeof(n), "  %2d ", i);
            out += n + Where(static_cast<uintptr_t>(f.AddrPC.Offset)) + "\n";
        }
        {
            std::lock_guard lk(g_mx);
            out += "last NorseEvents events (oldest first):\n";
            if (g_recent.empty()) out += "  (none)\n";
            for (auto& e : g_recent) out += "  " + e + "\n";
        }
        out += "Norse packs (NorseManifest):\n";
        nm::Manifest man;
        man.Resolve(g_mod.parent_path().parent_path(), {}, [&](const std::string& s) { out += "  " + s + "\n"; });
        // one log line at a time: the log writes at most 2048 characters per call (first test: the stack was cut short)
        size_t a = 0;
        while (a < out.size())
        {
            size_t b = out.find(static_cast<char>(10), a); // the next line break
            if (b == std::string::npos) b = out.size();
            Log::Get().Write(Level::Error, "%s", out.substr(a, b - a).c_str());
            a = b + 1;
        }
    }

    // no C++ objects with destructors in this frame, so __try is allowed here
    __declspec(noinline) void SafeReport(EXCEPTION_POINTERS* ep, const char* code)
    {
        __try
        {
            Report(ep, code);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    LONG CALLBACK OnException(EXCEPTION_POINTERS* ep)
    {
        const char* code = CodeName(ep->ExceptionRecord->ExceptionCode);
        if (!code || g_written >= 20) return EXCEPTION_CONTINUE_SEARCH;
        uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        {
            std::lock_guard lk(g_mx);
            if (!g_seen.insert(at).second) return EXCEPTION_CONTINUE_SEARCH;
        }
        static thread_local bool inside = false; // a fault while writing a report is not reported again
        if (inside) return EXCEPTION_CONTINUE_SEARCH;
        inside = true;
        ++g_written;
        SafeReport(ep, code);
        inside = false;
        return EXCEPTION_CONTINUE_SEARCH; // never handle it - the game's own handling goes on unchanged
    }

    void OnEvent(const NE_Event* e, void*)
    {
        if (!e || !e->name) return;
        char b[200];
        SYSTEMTIME st;
        GetLocalTime(&st);
        const char* t = NE_Text(e);
        snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d %s %d -> %d%s%s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, e->name, e->oldValue, e->newValue,
                 t ? " " : "", t ? t : "");
        std::lock_guard lk(g_mx);
        g_recent.push_back(b);
        if (g_recent.size() > 20) g_recent.pop_front();
    }

    __declspec(noinline) void DeliberateFault()
    {
        __try
        {
            volatile int* p = reinterpret_cast<int*>(0x10);
            *p = 1; // an access violation, caught right here
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
} // namespace

class NorseCrashLog : public RC::CppUserModBase
{
  public:
    NorseCrashLog()
    {
        ModName = L"NorseCrashLog";
        ModVersion = L"" NCL_VERSION;
        ModDescription = L"Crash reports that name the Norse frameworks' parts, game functions, recent events and installed packs.";
        ModAuthors = L"ApocryphaRealm";
        g_mod = ModDir();
        std::wstring dir;
        {
            std::ifstream in(g_mod / "NorseCrashLog.ini");
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("sLogDir=", 0) == 0) dir = nm::Widen(line.substr(8));
            }
        }
        Log::Get().Open((dir.empty() ? g_mod : fs::path(dir)) / "NorseCrashLog.log", 2);
        Log::Get().Write(Level::Error, "NorseCrashLog v%s - watching for faults", NCL_VERSION);
        AddVectoredExceptionHandler(1, &OnException);
    }

    auto on_unreal_init() -> void override
    {
        g_nal = nal::Library::Get().Load(g_mod.parent_path().parent_path() / "NorseAddressLibrary", [](const std::string& m) { NRN_INFO("%s", m.c_str()); });
        NRN_INFO("Norse Address Library %s - game addresses %s", g_nal ? "loaded" : "NOT found", g_nal ? "are named by their IDs" : "stay offsets");
        if (const ne::ApiTable* api = ne::Api()) api->Subscribe("*", &OnEvent, nullptr);
        try
        {
            RC::Unreal::Hook::RegisterInitGameStatePostCallback([](RC::Unreal::AGameModeBase*) {
                static bool subscribed = false;
                if (!subscribed)
                    if (const ne::ApiTable* api = ne::Api())
                    {
                        api->Subscribe("*", &OnEvent, nullptr);
                        subscribed = true;
                    }
            });
            RC::Unreal::Hook::RegisterProcessConsoleExecCallback(
                    [](RC::Unreal::UObject*, const wchar_t* cmd, RC::Unreal::FOutputDevice&, RC::Unreal::UObject*) -> bool {
                        if (!cmd || _wcsnicmp(cmd, L"ncl.", 4) != 0) return false;
                        static ULONGLONG last = 0;
                        ULONGLONG now = GetTickCount64();
                        if (now - last < 250) return true;
                        last = now;
                        std::wstring c(cmd);
                        if (_wcsicmp(c.c_str(), L"ncl.test") == 0)
                        {
                            NRN_INFO("ncl.test: a deliberate access violation, caught - a report should follow");
                            DeliberateFault();
                        }
                        else if (_wcsnicmp(c.c_str(), L"ncl.where ", 10) == 0)
                        {
                            uintptr_t rva = static_cast<uintptr_t>(std::wcstoull(c.c_str() + 10, nullptr, 16));
                            uintptr_t abs = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + rva;
                            NRN_INFO("ncl.where 0x%llx: %s  (Address Library: NameOf '%s', ID 2 at base+0x%llx)", static_cast<unsigned long long>(rva),
                                     Where(abs).c_str(), nal::Library::Get().NameOf(abs).c_str(),
                                     static_cast<unsigned long long>(nal::Library::Get().Address(2) ? nal::Library::Get().Address(2) -
                                                                                                          reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))
                                                                                                    : 0));
                        }
                        else NRN_WARN("unknown command - ncl.test | ncl.where <rva hex>");
                        return true;
                    });
        }
        catch (...)
        {
        }
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new NorseCrashLog();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
