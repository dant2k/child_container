#ifdef _MSC_VER

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>

// Bunch of stuff lifted from windows headers.

typedef struct _STARTUPINFOA {
  unsigned int  cb;
  char*  lpReserved;
  char*  lpDesktop;
  char*  lpTitle;
  unsigned int  dwX;
  unsigned int  dwY;
  unsigned int  dwXSize;
  unsigned int  dwYSize;
  unsigned int  dwXCountChars;
  unsigned int  dwYCountChars;
  unsigned int  dwFillAttribute;
  unsigned int  dwFlags;
  unsigned short   wShowWindow;
  unsigned short   cbReserved2;
  unsigned char* lpReserved2;
  void* hStdInput;
  void* hStdOutput;
  void* hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef struct _PROCESS_INFORMATION {
  void* hProcess;
  void* hThread;
  unsigned int  dwProcessId;
  unsigned int  dwThreadId;
} PROCESS_INFORMATION, *PPROCESS_INFORMATION, *LPPROCESS_INFORMATION;

#ifdef _WIN64
#define SIZE_T unsigned __int64
#else
#define SIZE_T unsigned int
#endif

extern "C" int CreateProcessA(
    const char*           lpApplicationName,
    char*                 lpCommandLine,
    void*                 lpProcessAttributes,
    void*                 lpThreadAttributes,
    int                   bInheritHandles,
    unsigned int          dwCreationFlags,
    void*                 lpEnvironment,
    const char*           lpCurrentDirectory,
    LPSTARTUPINFOA        lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
);

extern "C" void* OpenProcess(unsigned int dwDesiredAccess, int bInheritHandle, unsigned int dwProcessId);
extern "C" unsigned int WaitForMultipleObjects(unsigned int nCount, const void** lpHandles, int bWaitAll, unsigned int dwMilliseconds);
extern "C" int __stdcall GenerateConsoleCtrlEvent(unsigned int dwCtrlEvent, unsigned int dwProcessGroupId);
extern "C" void*    CreateRemoteThread(void* hProcess, void* lpThreadAttributes, SIZE_T dwStackSize, void* lpStartAddress, void* lpParameter, unsigned int dwCreationFlags, unsigned int lpThreadId);
extern "C" int      CloseHandle(void* handle);
extern "C" void     ExitProcess(unsigned int uExitCode);
extern "C" void     Sleep(unsigned int ms);
extern "C" int      TerminateProcess(void* hProcess, unsigned int uExitCode);
extern "C" int      GetLastError();


//
// This function is called in the child process' address space, so don't reference anything.
// The purpose is to try and fake a ctrl-c so that the app can shut down gracefully. At worse,
// we ExitProcess, which is still way better than calling TerminateProcess externally.
//
unsigned int __stdcall RemoteThreadProc(void*)
{    
    // Try ctrl-c
    GenerateConsoleCtrlEvent(0, 0);

    Sleep(5000);

    // OK, that didn't work. Use exit process.
    ExitProcess(0);
    return 0;
}

#else

#include <unistd.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <alloca.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#endif

int main(int argc, char** argv)
{
    //
    // We have the name and arguments of the process we are to run and monitor for shutdown,
    // including our parent id to monitor (which we dont bother with on posix)
    //
    // argv[0] = us
    // argv[1] = parent pid
    // argv[2] = child process path
    // argv[3...argc-1] = arguments to pass.
    //
    if (argc < 3)
    {
        printf("Usage: child_container <parent_pid> <path/to/child/exe> [child arguments]\n");
        printf("    On posix, parent_pid is ignored, but still required.\n");
        return 1;
    }

#ifdef _MSC_VER
    
    //
    // Try to get the parent process handle.
    //
    int parent_process_id = atoi(argv[1]);
    unsigned int access = 0x00100000; /* SYNCHRONIZE */
    access |= 0x400; /* PROCESS_QUERY_INFORMATION */
    void* parent_process_handle = OpenProcess(access, 0, parent_process_id);
    if (parent_process_handle == 0)
    {
        printf("Failed to get handle to process id %d.\n", parent_process_id);
        return 3;
    }

    //
    // assemble in to one command line.
    //
    int total_len = 0;
    for (int i = 2; i < argc; i++)
        total_len += strlen(argv[i]) + 3;
    total_len++;

    char* line = (char*)alloca(total_len);
    char* write = line;
    for (int i = 2; i < argc; i++)
    {
        write[0] = '"';
        strcpy(write + 1, argv[i]);
        write = write + strlen(write);
        write[0] = '"';
        write[1] = ' ';
        write[2] = 0;
        write+=2;
    }

    //
    // Try to start the child process.
    //
    STARTUPINFOA startup_info = {};
    PROCESS_INFORMATION process_info = {};

    startup_info.cb = sizeof(startup_info);

    int result = CreateProcessA(argv[2], line, 0, 0, 0, 0, 0, 0, &startup_info, &process_info);
    if (result == 0)
    {
        fprintf(stderr, "Failed to start child process. GetLastError = %d\n", GetLastError());
        return 2;
    }

    // Got the child handle and the parent handle.
    //
    // Now we wait on either the parent or the child handle to go away
    //
    void const* handles[2] = {process_info.hProcess, parent_process_handle};
    int wait_result = WaitForMultipleObjects(2, handles, 0, ~0U);
    
    if (wait_result == 0) // WAIT_OBJECT_0
        return 0; // child closed, we're done here, just die.

    //
    // We don't really care what the return value is otherwise. The only time we
    // don't try to kill the child is if the child was the one that ended,
    // which is handled above. So... just always try to end the child.
    //
    
    // There's no real way to send signals to other apps in windows. So we
    // need to create a thread in our child process, and then raise a ctrl
    // event that way.
    void* child_thread_handle = CreateRemoteThread(process_info.hProcess, 0, 0, RemoteThreadProc, 0, 0, 0);
    if (child_thread_handle == 0)
    {
        fprintf(stderr, "Failed to create remote thread. GetLastError() = %d\n", GetLastError());

        // Well, we can't do anything clever, so just nuke it. MOST LIKELY
        // we couldn't create the remote thread because the process is already dead...
        TerminateProcess(process_info.hProcess, 0);
        return 2;
    }

    // Close our handle so the other process can be cleaned up.
    CloseHandle(child_thread_handle);

    // MOST LIKELY the child is dead here pretty quick. Our remote thread raises ctrl-c, and then
    // calls ExitProcess (which is the default ctrl-c anyway), so do a quick check to see
    // if it's done.
    Sleep(250);
    if (WaitForMultipleObjects(1, handles, 0, 0) == 0) 
        return 0; // Child has exited, we're done.

    // OK, well they didn't die quickly, so wait a long time. If it's still not dead after 5 seconds,
    // we just nuke it from orbit.
    Sleep(5000);
    if (WaitForMultipleObjects(1, handles, 0, 0) != 0) 
    {
        // We got a result that was something OTHER than it has closed - after 5
        // seconds. So kill it.
        TerminateProcess(process_info.hProcess, 0);
    }

    return 0;

#else

    char** child_argv = (char**)alloca(sizeof(char*) * (argc));
    child_argv[0] = argv[2];
    for (int i = 3; i < argc; i++)
        child_argv[i-3+1] = argv[i];
    child_argv[argc - 2] = 0; // null terminate the list of args.

    // Spawn a child process.
    pid_t child_pid = fork();
    if (child_pid == 0)
    {
        // we are the child - exec.
        if (execv(child_argv[0], child_argv) == -1)
        {
            if (errno == ENOENT)
            {
                fprintf(stderr, "Program doesn't exist: %s", child_argv[0]);
                exit(2);
            }
            printf("execv failed, errno %d", errno);
            return 1;
        }

        // unreachable.
    }

    // Here we are the parent, and our job is to kill the child when the parent dies,
    // or just die if the child dies. Unfortunatley AFAICT posix doesn't have a way to wait
    // on a PID that isn't our child - and we need to know when our parent dies, so we just
    // have to poll.
    for (;;)
    {
        // Poll 1/second
        sleep(1);

        // we are the parent - check if the child has died
        int child_status = 0;
        int waitresult = waitpid(child_pid, &child_status, WNOHANG);
        if (child_status || waitresult < 0)
            break; // something is wrong with the child or it died, we no longer have a purpose.
        
        // check if the parent process has disappeared. (parentid goes to 1)
        if (getppid() == 1)
        {
            // Need to kill the child process.
            kill(child_pid, SIGTERM);

            // Wait to see if it listened.
            sleep(5);

            // try and reap.
            if (waitpid(child_pid, &child_status, WNOHANG) < 0 ||
                child_status)
            {
                // Child is dead - we are done.
                exit(EXIT_SUCCESS);
            }

            // Still alive, send kill
            kill(child_pid, SIGKILL);

            // reap
            wait(0);

            exit(EXIT_SUCCESS);
        }
    }

    exit(EXIT_SUCCESS);
#endif
}