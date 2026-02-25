/*
 * main.c - Application entry point
 *
 * Replaces the original WinMain (sys_win.c) with an SDL2-based
 * cross-platform entry point. Drives the Qcommon_Init/Qcommon_Frame loop.
 *
 * Original: WinMain → Qcommon_Init → main loop calling Qcommon_Frame
 * The original used timeGetTime() for frame timing and PeekMessage()
 * for the Windows message pump. We use SDL2 for both.
 */

#include "../common/qcommon.h"
#include "win32_compat.h"

#include <SDL2/SDL.h>

/* ==========================================================================
   System Layer (Sys_* functions expected by engine)
   ========================================================================== */

void Sys_Init(void)
{
    /* Platform-specific initialization beyond what SDL handles */
    Com_Printf("Sys_Init: SDL2 platform layer\n");
}

void Sys_Quit(void)
{
    Qcommon_Shutdown();
    Sys_PlatformShutdown();
    exit(0);
}

void Sys_Error(const char *error, ...)
{
    va_list argptr;
    char    string[1024];

    va_start(argptr, error);
    vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);

    /* Show error in message box */
    Sys_MessageBox("Soldier of Fortune - Fatal Error", string, 1);

    fprintf(stderr, "\n=== FATAL ERROR ===\n%s\n", string);

    Qcommon_Shutdown();
    Sys_PlatformShutdown();
    exit(1);
}

char *Sys_ConsoleInput(void)
{
    /* TODO: Non-blocking stdin read for dedicated server mode */
    return NULL;
}

void Sys_ConsoleOutput(const char *string)
{
    fputs(string, stdout);
}

void Sys_Mkdir(const char *path)
{
#ifdef SOF_PLATFORM_WINDOWS
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

/* File search — used by FS for scanning directories */
#ifdef SOF_PLATFORM_WINDOWS
  #include <io.h>
  static intptr_t findhandle = -1;
  static struct _finddata_t finddata;
#else
  #include <dirent.h>
  #include <fnmatch.h>
  #include <sys/stat.h>
  static DIR *finddir;
  static char findpattern[MAX_OSPATH];
  static char findbase[MAX_OSPATH];
#endif

char *Sys_FindFirst(const char *path, unsigned musthave, unsigned canthave)
{
    (void)musthave;
    (void)canthave;

#ifdef SOF_PLATFORM_WINDOWS
    if (findhandle != -1)
        Sys_FindClose();

    findhandle = _findfirst(path, &finddata);
    if (findhandle == -1)
        return NULL;
    return finddata.name;
#else
    /* TODO: POSIX implementation */
    (void)path;
    return NULL;
#endif
}

char *Sys_FindNext(unsigned musthave, unsigned canthave)
{
    (void)musthave;
    (void)canthave;

#ifdef SOF_PLATFORM_WINDOWS
    if (findhandle == -1)
        return NULL;
    if (_findnext(findhandle, &finddata) == -1)
        return NULL;
    return finddata.name;
#else
    return NULL;
#endif
}

void Sys_FindClose(void)
{
#ifdef SOF_PLATFORM_WINDOWS
    if (findhandle != -1) {
        _findclose(findhandle);
        findhandle = -1;
    }
#else
    if (finddir) {
        closedir(finddir);
        finddir = NULL;
    }
#endif
}

/* ==========================================================================
   Main Entry Point
   ========================================================================== */

int main(int argc, char **argv)
{
    int     oldtime, newtime, msec;

    /* Initialize SDL2 platform layer */
    if (!Sys_PlatformInit(argc, argv)) {
        fprintf(stderr, "Failed to initialize platform layer\n");
        return 1;
    }

    /* Initialize engine */
    Qcommon_Init(argc, argv);

    /* Main loop — replaces the original WinMain message pump */
    oldtime = Sys_Milliseconds();

    while (1) {
        /* Pump SDL events (replaces PeekMessage/DispatchMessage) */
        Sys_PumpEvents();

        /* Calculate frame time */
        do {
            newtime = Sys_Milliseconds();
            msec = newtime - oldtime;
        } while (msec < 1);    /* don't spin at 0ms frames */

        oldtime = newtime;

        /* Run one engine frame */
        Qcommon_Frame(msec);
    }

    /* Never reached */
    return 0;
}
