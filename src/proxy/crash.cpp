// Logs fatal-looking exceptions (with a stack) before the game's own crash handler can
// swallow them; many games terminate silently.
#include <cstdio>

#include "proxy.h"

namespace {

std::string Where(DWORD64 address) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module))
        GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    char buf[MAX_PATH + 32];
    snprintf(buf, sizeof(buf), "%s+0x%llx", name ? name + 1 : path,
             (unsigned long long)(module ? address - DWORD64(module) : address));
    return buf;
}

bool Fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case 0xC0000409:  // stack buffer overrun / fail fast
        case 0xC0000374:  // heap corruption
            return true;
        default: return false;
    }
}

// The thread's stack range from its TEB (GetCurrentThreadStackLimits needs Windows 8, and
// importing it would stop the DLL from loading on Windows 7).
void StackLimits(ULONG_PTR& low, ULONG_PTR& high) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    low = reinterpret_cast<ULONG_PTR>(tib->StackLimit);
    high = reinterpret_cast<ULONG_PTR>(tib->StackBase);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    static LONG reported = 0;
    if (!Fatal(code) || InterlockedIncrement(&reported) > 8) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (code == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        Log("EXCEPTION 0x%08lx at %s: %s address 0x%llx", code, Where(DWORD64(r->ExceptionAddress)).c_str(),
            r->ExceptionInformation[0] == 1 ? "write to" : r->ExceptionInformation[0] == 8 ? "execute at" : "read from",
            (unsigned long long)r->ExceptionInformation[1]);
    else
        Log("EXCEPTION 0x%08lx at %s", code, Where(DWORD64(r->ExceptionAddress)).c_str());
#ifdef _WIN64
    CONTEXT context = *info->ContextRecord;
    ULONG_PTR low, high;
    StackLimits(low, high);
    for (int depth = 0; depth < 24 && context.Rip; depth++) {
        DWORD64 imageBase;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function) {
            void* handlerData;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &frame,
                             nullptr);
        } else {
            if (context.Rsp < low || context.Rsp + 8 > high) break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
        if (context.Rip) Log("    called from %s", Where(context.Rip).c_str());
    }
#else
    const CONTEXT* context = info->ContextRecord;
    ULONG_PTR low, high;
    StackLimits(low, high);
    int found = 0;
    for (DWORD esp = context->Esp; esp + 4 <= high && found < 16; esp += 4) {
        DWORD value = *reinterpret_cast<DWORD*>(esp);
        HMODULE module;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)value, &module)) {
            Log("    stack: %s", Where(value).c_str());
            found++;
        }
    }
#endif
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLogger() { AddVectoredExceptionHandler(1, OnException); }
