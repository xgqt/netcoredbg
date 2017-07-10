#include <windows.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"
#include "clrinternal.h"

#include <unistd.h>

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <fstream>

#include <arrayholder.h>
#include "torelease.h"

EXTERN_C HRESULT CreateDebuggingInterfaceFromVersionEx(
    int iDebuggerVersion,
    LPCWSTR szDebuggeeVersion,
    IUnknown ** ppCordb);

EXTERN_C HRESULT
CreateVersionStringFromModule(
    DWORD pidDebuggee,
    LPCWSTR szModuleName,
    LPWSTR pBuffer,
    DWORD cchBuffer,
    DWORD* pdwLength);

std::mutex g_processMutex;
ICorDebugProcess *g_process = NULL;

ULONG OSPageSize ()
{
    static ULONG pageSize = 0;
    if (pageSize == 0)
        pageSize = sysconf(_SC_PAGESIZE);

    return pageSize;
}

size_t NextOSPageAddress (size_t addr)
{
    size_t pageSize = OSPageSize();
    return (addr+pageSize)&(~(pageSize-1));
}

/**********************************************************************\
* Routine Description:                                                 *
*                                                                      *
*    This function is called to read memory from the debugee's         *
*    address space.  If the initial read fails, it attempts to read    *
*    only up to the edge of the page containing "offset".              *
*                                                                      *
\**********************************************************************/
BOOL SafeReadMemory (TADDR offset, PVOID lpBuffer, ULONG cb,
                     PULONG lpcbBytesRead)
{
    std::lock_guard<std::mutex> lock(g_processMutex);

    if (!g_process)
        return FALSE;

    BOOL bRet = FALSE;

    SIZE_T bytesRead = 0;

    bRet = SUCCEEDED(g_process->ReadMemory(TO_CDADDR(offset), cb, (BYTE*)lpBuffer,
                                           &bytesRead));

    if (!bRet)
    {
        cb   = (ULONG)(NextOSPageAddress(offset) - offset);
        bRet = SUCCEEDED(g_process->ReadMemory(TO_CDADDR(offset), cb, (BYTE*)lpBuffer,
                                            &bytesRead));
    }

    *lpcbBytesRead = bytesRead;
    return bRet;
}

bool g_processExited = false;

std::mutex g_outMutex;

void _out_printf(const char *fmt, ...)
    __attribute__((format (printf, 1, 2)));

#define out_printf(fmt, ...) _out_printf(fmt, ##__VA_ARGS__)

void _out_printf(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_outMutex);
    va_list arg;

    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);

    fflush(stdout);
}

// Breakpoints
void DeleteAllBreakpoints();
HRESULT FindCurrentBreakpointId(ICorDebugThread *pThread, ULONG32 &id);
HRESULT DeleteBreakpoint(ULONG32 id);
HRESULT CreateBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id);
HRESULT TryResolveBreakpointsForModule(ICorDebugModule *pModule);
HRESULT PrintBreakpoint(ULONG32 id, std::string &output);

// Modules
void SetCoreCLRPath(const std::string &coreclrPath);
std::string GetModuleName(ICorDebugModule *pModule);
HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range);
HRESULT TryLoadModuleSymbols(ICorDebugModule *pModule);
HRESULT GetFrameLocation(ICorDebugFrame *pFrame,
                         ULONG32 &ilOffset,
                         mdMethodDef &methodToken,
                         std::string &fullname,
                         ULONG &linenum);


// Varobj
HRESULT ListVariables(ICorDebugFrame *pFrame, std::string &output);

// TypePrinter
#include "typeprinter.h"

HRESULT PrintThread(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status = S_OK;

    std::stringstream ss;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));
    CorDebugThreadState state = THREAD_SUSPEND;
    IfFailRet(pThread->GetDebugState(&state));

    CorDebugUserState ustate;
    IfFailRet(pThread->GetUserState(&ustate));

    static const struct { int val; const char *name; } states[] = {
        { USER_STOP_REQUESTED, "USER_STOP_REQUESTED" },
        { USER_SUSPEND_REQUESTED, "USER_SUSPEND_REQUESTED" },
        { USER_BACKGROUND, "USER_BACKGROUND" },
        { USER_UNSTARTED, "USER_UNSTARTED" },
        { USER_STOPPED, "USER_STOPPED" },
        { USER_WAIT_SLEEP_JOIN, "USER_WAIT_SLEEP_JOIN" },
        { USER_SUSPENDED, "USER_SUSPENDED" },
        { USER_UNSAFE_POINT, "USER_UNSAFE_POINT" },
        { USER_THREADPOOL, "USER_THREADPOOL" }
    };

    std::string user_state;
    for (int i = 0; i < sizeof(states)/sizeof(states[0]); i++)
    {
        if (ustate & states[i].val)
        {
            if (!user_state.empty()) user_state += '|';
            user_state += states[i].name;
        }
    }

    ss << "{id=\"" << threadId
       << "\",name=\"<No name>\",state=\"" << (state == THREAD_RUN ? "running" : "stopped")
       << "-" << user_state << "\"}";

    output = ss.str();

    return S_OK;
}

HRESULT PrintThreadsState(ICorDebugController *controller, std::string &output)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugThreadEnum> pThreads;
    IfFailRet(controller->EnumerateThreads(&pThreads));

    std::stringstream ss;

    ss << "threads=[";

    ICorDebugThread *handle;
    ULONG fetched;
    const char *sep = "";
    while (SUCCEEDED(Status = pThreads->Next(1, &handle, &fetched)) && fetched == 1)
    {
        ToRelease<ICorDebugThread> pThread(handle);

        std::string threadOutput;
        PrintThread(pThread, threadOutput);

        ss << sep << threadOutput;
        sep = ",";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

HRESULT PrintFrameLocation(ICorDebugFrame *pFrame, std::string &output)
{
    HRESULT Status;
    ULONG32 ilOffset;
    mdMethodDef methodToken;
    std::string fullname;
    ULONG linenum;

    IfFailRet(GetFrameLocation(pFrame, ilOffset, methodToken, fullname, linenum));

    std::stringstream ss;
    ss << "line=\"" << linenum << "\",fullname=\"" << fullname << "\"";
    output = ss.str();

    return S_OK;
}

HRESULT PrintLocation(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));

    ULONG32 ilOffset;
    mdMethodDef methodToken;
    std::string fullname;
    ULONG linenum;

    IfFailRet(GetFrameLocation(pFrame, ilOffset, methodToken, fullname, linenum));

    std::stringstream ss;
    ss << "line=\"" << linenum << "\",fullname=\"" << fullname << "\"";
    output = ss.str();

    return S_OK;
}

HRESULT DisableAllBreakpointsAndSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;

    ToRelease<ICorDebugBreakpointEnum> breakpoints;
    if (SUCCEEDED(pAppDomain->EnumerateBreakpoints(&breakpoints)))
    {
        ICorDebugBreakpoint *curBreakpoint;
        ULONG breakpointsFetched;
        while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
        {
            ToRelease<ICorDebugBreakpoint> pBreakpoint(curBreakpoint);
            pBreakpoint->Activate(FALSE);
        }
    }

    DeleteAllBreakpoints();

    ToRelease<ICorDebugStepperEnum> steppers;
    if (SUCCEEDED(pAppDomain->EnumerateSteppers(&steppers)))
    {
        ICorDebugStepper *curStepper;
        ULONG steppersFetched;
        while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
        {
            ToRelease<ICorDebugStepper> pStepper(curStepper);
            pStepper->Deactivate();
        }
    }

    return S_OK;
}

HRESULT DisableAllBreakpointsAndSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        DisableAllBreakpointsAndSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

enum StepType {
    STEP_IN = 0,
    STEP_OVER,
    STEP_OUT
};

HRESULT RunStep(ICorDebugThread *pThread, StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());
        return S_OK;
    }

    BOOL bStepIn = stepType == STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    return S_OK;
}

HRESULT PrintFrames(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status;
    std::stringstream ss;

    ToRelease<ICorDebugThread3> pThread3;
    ToRelease<ICorDebugStackWalk> pStackWalk;

    IfFailRet(pThread->QueryInterface(IID_ICorDebugThread3, (LPVOID *) &pThread3));
    IfFailRet(pThread3->CreateStackWalk(&pStackWalk));

    int currentFrame = -1;

    ss << "stack=[";

    for (Status = S_OK; ; Status = pStackWalk->Next())
    {
        currentFrame++;

        if (Status == CORDBG_S_AT_END_OF_STACK)
            break;

        IfFailRet(Status);

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pStackWalk->GetFrame(&pFrame));
        if (Status == S_FALSE)
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"[NativeStackFrame]\"}";
            continue;
        }

        ToRelease<ICorDebugRuntimeUnwindableFrame> pRuntimeUnwindableFrame;
        Status = pFrame->QueryInterface(IID_ICorDebugRuntimeUnwindableFrame, (LPVOID *) &pRuntimeUnwindableFrame);
        if (SUCCEEDED(Status))
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"[RuntimeUnwindableFrame]\"}";
            continue;
        }

        ToRelease<ICorDebugILFrame> pILFrame;
        HRESULT hrILFrame = pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame);

        if (FAILED(hrILFrame))
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"?\"}";
            continue;
        }

        ToRelease<ICorDebugFunction> pFunction;
        Status = pFrame->GetFunction(&pFunction);
        if (FAILED(Status))
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"[IL Stub or LCG]\"}";
            continue;
        }

        std::string frameLocation;
        PrintFrameLocation(pFrame, frameLocation);

        ss << (currentFrame != 0 ? "," : "");
        ss << "frame={level=\"" << currentFrame << "\",";
        if (!frameLocation.empty())
            ss << frameLocation << ",";

        std::string methodName;
        TypePrinter::GetMethodName(pFrame, methodName);

        ss << "func=\"" << methodName << "\"}";
    }

    ss << "]";

    output = ss.str();

    return S_OK;
}

std::mutex g_currentThreadMutex;
ICorDebugThread *g_currentThread = nullptr;

class ManagedCallback : public ICorDebugManagedCallback, ICorDebugManagedCallback2
{
    ULONG m_refCount;
public:

        void HandleEvent(ICorDebugController *controller, const char *eventName)
        {
            out_printf("=message,text=\"event received %s\"\n", eventName);
            controller->Continue(0);
        }

        ManagedCallback() : m_refCount(1) {}
        virtual ~ManagedCallback() {}

        // IUnknown

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface)
        {
            if(riid == __uuidof(ICorDebugManagedCallback))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback*>(this);
                AddRef();
                return S_OK;
            }
            else if(riid == __uuidof(ICorDebugManagedCallback2))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback2*>(this);
                AddRef();
                return S_OK;
            }
            else if(riid == __uuidof(IUnknown))
            {
                *ppInterface = static_cast<IUnknown*>(static_cast<ICorDebugManagedCallback*>(this));
                AddRef();
                return S_OK;
            }
            else
            {
                return E_NOINTERFACE;
            }
        }

        virtual ULONG STDMETHODCALLTYPE AddRef()
        {
            return InterlockedIncrement((volatile LONG *) &m_refCount);
        }

        virtual ULONG STDMETHODCALLTYPE Release()
        {
            ULONG count = InterlockedDecrement((volatile LONG *) &m_refCount);
            if(count == 0)
            {
                delete this;
            }
            return count;
        }

        // ICorDebugManagedCallback

        virtual HRESULT STDMETHODCALLTYPE Breakpoint(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint)
        {
            ULONG32 id = 0;
            FindCurrentBreakpointId(pThread, id);

            std::string output;
            PrintLocation(pThread, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            out_printf("*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"%u\",%s\n",
                (int)threadId, (unsigned int)id, output.c_str());
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                if (g_currentThread)
                    g_currentThread->Release();
                pThread->AddRef();
                g_currentThread = pThread;
            }
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE StepComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugStepper *pStepper,
            /* [in] */ CorDebugStepReason reason)
        {
            std::string output;
            PrintLocation(pThread, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            out_printf("*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",%s\n",
                (int)threadId, output.c_str());

            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                if (g_currentThread)
                    g_currentThread->Release();
                pThread->AddRef();
                g_currentThread = pThread;
            }

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE Break(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread) { HandleEvent(pAppDomain, "Break"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ BOOL unhandled)
        {
            std::string output;
            PrintLocation(pThread, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            if (unhandled)
            {
                ToRelease<ICorDebugFrame> pFrame;
                std::string output;

                if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)))
                    PrintFrameLocation(pFrame, output);

                out_printf("*stopped,reason=\"exception-received\",exception-stage=\"%s\",thread-id=\"%i\",stopped-threads=\"all\",%s\n",
                    unhandled ? "unhandled" : "handled", (int)threadId, output.c_str());

                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                if (g_currentThread)
                    g_currentThread->Release();
                pThread->AddRef();
                g_currentThread = pThread;
            } else {
                out_printf("=message,text=\"Exception thrown: '%s' in %s\\n\",send-to=\"output-window\",source=\"target-exception\"\n",
                    "<exceptions.name>", "<short.module.name>");
                pAppDomain->Continue(0);
            }

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE EvalException(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            //HandleEvent(pProcess, "CreateProcess");
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            out_printf("*stopped,reason=\"exited\",exit-code=\"%i\"\n", 0);
            g_processExited = true;
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            DWORD threadId = 0;
            thread->GetID(&threadId);
            out_printf("=thread-created,id=\"%i\"\n", (int)threadId);
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            HandleEvent(pAppDomain, "ExitThread");
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE LoadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule)
        {
            std::string name = GetModuleName(pModule);
            if (!name.empty())
            {
                out_printf("=library-loaded,target-name=\"%s\"\n", name.c_str());
            }
            TryLoadModuleSymbols(pModule);
            TryResolveBreakpointsForModule(pModule);
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule) { HandleEvent(pAppDomain, "UnloadModule"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LoadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c) { HandleEvent(pAppDomain, "LoadClass"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE UnloadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c) { HandleEvent(pAppDomain, "UnloadClass"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE DebuggerError(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ HRESULT errorHR,
            /* [in] */ DWORD errorCode) { printf("DebuggerError\n"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LogMessage(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pMessage) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LogSwitch(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ ULONG ulReason,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pParentName) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain)
        {
            //HandleEvent(pProcess, "CreateAppDomain");
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain) { HandleEvent(pAppDomain, "ExitAppDomain"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LoadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly)
        {
            //HandleEvent(pAppDomain, "LoadAssembly");
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ControlCTrap(
            /* [in] */ ICorDebugProcess *pProcess) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE NameChange(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule,
            /* [in] */ IStream *pSymbolStream) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE EditAndContinueRemap(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction,
            /* [in] */ BOOL fAccurate) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE BreakpointSetError(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint,
            /* [in] */ DWORD dwError) {return S_OK; }


        // ICorDebugManagedCallback2

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pOldFunction,
            /* [in] */ ICorDebugFunction *pNewFunction,
            /* [in] */ ULONG32 oldILOffset) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId,
            /* [in] */ WCHAR *pConnName) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ChangeConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE DestroyConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFrame *pFrame,
            /* [in] */ ULONG32 nOffset,
            /* [in] */ CorDebugExceptionCallbackType dwEventType,
            /* [in] */ DWORD dwFlags)
        {
            // const char *cbTypeName;
            // switch(dwEventType)
            // {
            //     case DEBUG_EXCEPTION_FIRST_CHANCE: cbTypeName = "FIRST_CHANCE"; break;
            //     case DEBUG_EXCEPTION_USER_FIRST_CHANCE: cbTypeName = "USER_FIRST_CHANCE"; break;
            //     case DEBUG_EXCEPTION_CATCH_HANDLER_FOUND: cbTypeName = "CATCH_HANDLER_FOUND"; break;
            //     case DEBUG_EXCEPTION_UNHANDLED: cbTypeName = "UNHANDLED"; break;
            //     default: cbTypeName = "?"; break;
            // }
            // out_printf("*stopped,reason=\"exception-received2\",exception-stage=\"%s\"\n",
            //     cbTypeName);
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExceptionUnwind(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
            /* [in] */ DWORD dwFlags) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE MDANotification(
            /* [in] */ ICorDebugController *pController,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugMDA *pMDA) {return S_OK; }
};

std::string GetCoreCLRLPath(int pid)
{
    static const char *coreclr_so = "/libcoreclr.so";
    static const std::size_t coreclr_so_len = strlen(coreclr_so);

    char maps_name[100];
    snprintf(maps_name, sizeof(maps_name), "/proc/%i/maps", pid);
    std::ifstream input(maps_name);

    for(std::string line; std::getline(input, line); )
    {
        std::size_t i = line.rfind(coreclr_so);
        if (i == std::string::npos)
            continue;
        if (i + coreclr_so_len != line.size())
            continue;
        std::size_t si = line.rfind(' ', i);
        if (i == std::string::npos)
            continue;
        return line.substr(si + 1);//, i - si - 1);
    }
    return std::string();
}

void print_help()
{
    fprintf(stderr,
        "CoreCLR debugger for Linux.\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n");
}

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        print_help();
        return EXIT_FAILURE;
    }

    DWORD pidDebuggee = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--attach") == 0)
        {
            i++;
            if (i >= argc)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
            char *err;
            pidDebuggee = strtoul(argv[i], &err, 10);
            if (*err != 0)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "--interpreter=mi") == 0)
        {
            continue;
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            print_help();
            return EXIT_SUCCESS;
        }
        else
        {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (pidDebuggee == 0)
    {
        fprintf(stderr, "Error: Missing process id\n");
        return EXIT_FAILURE;
    }

    std::string coreclrPath = GetCoreCLRLPath(pidDebuggee);
    if (coreclrPath.empty())
    {
        fprintf(stderr, "Error: Unable to find libcoreclr.so\n");
        return EXIT_FAILURE;
    }

    SetCoreCLRPath(coreclrPath);

    WCHAR szModuleName[MAX_LONGPATH];
    MultiByteToWideChar(CP_UTF8, 0, coreclrPath.c_str(), coreclrPath.size() + 1, szModuleName, MAX_LONGPATH);

    WCHAR pBuffer[100];
    DWORD dwLength;
    HRESULT hr = CreateVersionStringFromModule(
        pidDebuggee,
        szModuleName,
        pBuffer,
        _countof(pBuffer),
        &dwLength);

    if (FAILED(hr))
    {
        fprintf(stderr, "CreateVersionStringFromModule failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    ToRelease<IUnknown> pCordb;
    //WCHAR szDebuggeeVersion[] = W("4.0");
    hr = CreateDebuggingInterfaceFromVersionEx(4, pBuffer, &pCordb);

    if (FAILED(hr))
    {
        fprintf(stderr, "CreateDebuggingInterfaceFromVersionEx failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    ToRelease<ICorDebug> pCorDebug;

    hr = pCordb->QueryInterface(IID_ICorDebug, (LPVOID *)&pCorDebug);
    if (FAILED(hr))
    {
        fprintf(stderr, "QueryInterface(IID_ICorDebug) failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    hr = pCorDebug->Initialize();
    if (FAILED(hr))
    {
        fprintf(stderr, "Initialize failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    hr = pCorDebug->SetManagedHandler(new ManagedCallback());
    if (FAILED(hr))
    {
        fprintf(stderr, "SetManagedHandler failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    hr = pCorDebug->CanLaunchOrAttach(pidDebuggee, FALSE);
    //fprintf(stderr, "CanLaunchOrAttach : hr=%x\n", hr);

    ToRelease<ICorDebugProcess> pProcess;
    hr = pCorDebug->DebugActiveProcess(
            pidDebuggee,
            FALSE,
            &pProcess);
    if (FAILED(hr))
    {
        fprintf(stderr, "DebugActiveProcess failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    {
        std::lock_guard<std::mutex> lock(g_processMutex);
        g_process = pProcess;
    }

    static char inputBuffer[1024];
    const char *token = "";

    for (;;) {
        token = "";

        out_printf("(gdb)\n");
        if (!fgets(inputBuffer, _countof(inputBuffer), stdin))
            break;

        size_t count = strlen(inputBuffer);

        if (count < 1 || inputBuffer[count - 1] != '\n')
            break;

        if (count >= 2 && inputBuffer[count - 2] == '\r')
            --count;

        int miCmdStart = 0;
        char *tokenEnd = strchr(inputBuffer, '-');
        if (tokenEnd)
            miCmdStart = tokenEnd - inputBuffer + 1;
        else
            tokenEnd = inputBuffer;
        std::string line(inputBuffer + miCmdStart, count - miCmdStart - 1);
        *tokenEnd = '\0';
        token = inputBuffer;

        if (line == "thread-info")
        {
            std::string output;
            HRESULT hr = PrintThreadsState(pProcess, output);
            if (SUCCEEDED(hr))
            {
                out_printf("%s^done,%s\n", token, output.c_str());
            }
            else
            {
                out_printf("%s^error,msg=\"HRESULT=%x\"\n", token, hr);
            }
        }
        else if (line == "exec-continue")
        {
            HRESULT hr = pProcess->Continue(0);
            if (SUCCEEDED(hr))
            {
                out_printf("%s^done\n", token);
            }
            else
            {
                out_printf("%s^error,msg=\"HRESULT=%x\"\n", token, hr);
            }
        }
        else if (line == "exec-interrupt")
        {
            HRESULT hr = pProcess->Stop(0);
            if (SUCCEEDED(hr))
            {
                out_printf("%s^done\n", token);
            }
            else
            {
                out_printf("%s^error,msg=\"HRESULT=%x\"\n", token, hr);
            }
        }
        else if (line.find("break-insert ") == 0)
        {
            // TODO: imlement proper argument parsing
            std::size_t i1 = line.find(' ');
            std::size_t i2 = line.rfind(':');

            if (i1 != std::string::npos && i2 != std::string::npos)
            {
                std::string filename = line.substr(i1 + 1, i2 - i1 - 1);
                std::string slinenum = line.substr(i2 + 1);

                int linenum = std::stoi(slinenum);
                ULONG32 id;
                if (SUCCEEDED(CreateBreakpointInProcess(pProcess, filename, linenum, id)))
                {
                    std::string output;
                    PrintBreakpoint(id, output);
                    out_printf("%s^done,%s\n", token, output.c_str());
                }
            }
            else
            {
                out_printf("%s^error,msg=\"Unknown breakpoint location format\"\n", token);
            }
        }
        else if (line.find("break-delete ") == 0)
        {
            std::size_t i = line.find(' ');
            ULONG32 id = std::stoul(line.substr(i));
            DeleteBreakpoint(id);
            out_printf("%s^done\n", token);
        }
        else if (line == "exec-next" || line == "exec-step" || line == "exec-finish")
        {
            StepType stepType;
            if (line == "exec-next")
                stepType = STEP_OVER;
            else if (line == "exec-step")
                stepType = STEP_IN;
            else if (line == "exec-finish")
                stepType = STEP_OUT;

            HRESULT hr;
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                hr = g_currentThread ? RunStep(g_currentThread, stepType) : E_FAIL;
            }

            if (FAILED(hr))
            {
                out_printf("%s^error,msg=\"Cannot create stepper: %x\"\n", token, hr);
            }
            else
            {
                hr = pProcess->Continue(0);
                if (SUCCEEDED(hr))
                {
                    out_printf("%s^done\n", token);
                }
                else
                {
                    out_printf("%s^error,msg=\"HRESULT=%x\"\n", token, hr);
                }
            }
        }
        else if (line == "stack-list-frames")
        {
            // TODO: Add parsing frame indeces and --thread
            std::string output;
            HRESULT hr;
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                hr = g_currentThread ? PrintFrames(g_currentThread, output) : E_FAIL;
            }
            if (SUCCEEDED(hr))
            {
                out_printf("%s^done,%s\n", token, output.c_str());
            }
            else
            {
                out_printf("%s^error,msg=\"HRESULT=%x\"\n", token, hr);
            }
        }
        else if (line.find("stack-list-variables ") == 0)
        {
            // TODO: Add parsing arguments --thread, --frame
            std::string output;
            HRESULT hr;
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);

                ToRelease<ICorDebugFrame> pFrame;
                hr = g_currentThread ? g_currentThread->GetActiveFrame(&pFrame) : E_FAIL;
                if (SUCCEEDED(hr))
                    hr = ListVariables(pFrame, output);
            }
            if (SUCCEEDED(hr))
            {
                out_printf("%s^done,%s\n", token, output.c_str());
            }
            else
            {
                out_printf("%s^error,msg=\"HRESULT=%x\"\n", token, hr);
            }
        }
        else if (line == "gdb-exit")
        {
            hr = pProcess->Stop(0);
            if (SUCCEEDED(hr))
            {
                DisableAllBreakpointsAndSteppers(pProcess);

                hr = pProcess->Terminate(0);

                while (!g_processExited)
                    Sleep(100);

                pProcess.Release();
            }
            break;
        } else {
            out_printf("%s^error,msg=\"Unknown command: %s\"\n", token, line.c_str());
        }
    }

    if (pProcess)
    {
        if (SUCCEEDED(pProcess->Stop(0)))
        {
            DisableAllBreakpointsAndSteppers(pProcess);
            hr = pProcess->Detach();
        }
    }

    pCorDebug->Terminate();

    out_printf("%s^exit\n", token);

    // TODO: Cleanup libcoreclr.so instance

    return EXIT_SUCCESS;
}
