#include <unistd.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <alloca.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

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
        printf("    On posix, parent_pid is ignored.\n");
        return 1;
    }

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
}