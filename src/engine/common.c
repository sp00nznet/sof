/*
 * common.c - Engine common functions
 *
 * Based on Quake II common.c (id Software GPL).
 * Provides console output, error handling, and the main engine loop.
 *
 * SoF exports: Com_Printf, Com_DPrintf, Com_Error
 * Original addresses: Com_Printf=0x1DE70, Com_DPrintf=0x1E070, Com_Error=0x1E130
 *
 * Qcommon_Init (0x20BB0): Refs z_stats, error, disconnect, subliminal
 * Com_Init (0x23760): Refs cpu_mmx, cpu_amd3d, timescale, game
 */

#include "../common/qcommon.h"
#include "win32_compat.h"
#include "../renderer/r_local.h"
#include "../ghoul/ghoul.h"

#include <time.h>

/* Forward declarations — input system (client/in_sdl.c) */
extern void IN_Init(void);
extern void IN_Shutdown(void);

/* ==========================================================================
   Global Cvars
   ========================================================================== */

cvar_t  *developer;
cvar_t  *timescale;
cvar_t  *fixedtime;
cvar_t  *dedicated;
cvar_t  *com_speeds;
cvar_t  *logfile_cvar;
cvar_t  *showtrace;

/* SoF-specific cvars discovered through binary analysis */
cvar_t  *sof_version;
cvar_t  *gore_detail;

static FILE *logfile;
static int  server_state;

/* ==========================================================================
   Console Output
   ========================================================================== */

static char     com_printbuf[8192];

void Com_Printf(const char *fmt, ...)
{
    va_list argptr;
    char    msg[4096];

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    /* Print to stdout */
    fputs(msg, stdout);
    fflush(stdout);

    /* Log to file if enabled */
    if (logfile) {
        fputs(msg, logfile);
        fflush(logfile);
    }

    /* TODO: Print to in-game console when implemented */
    /* Con_Print(msg); */
}

void Com_DPrintf(const char *fmt, ...)
{
    va_list argptr;
    char    msg[4096];

    if (!developer || !developer->value)
        return;

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    Com_Printf("%s", msg);
}

/* ==========================================================================
   Error Handling
   ========================================================================== */

void Com_Error(int code, const char *fmt, ...)
{
    va_list     argptr;
    static char msg[4096];
    static int  recursive = 0;

    if (recursive)
        Sys_Error("recursive error after: %s", msg);

    recursive = 1;

    va_start(argptr, fmt);
    vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    switch (code) {
    case ERR_DISCONNECT:
        CL_Drop();
        recursive = 0;
        return;

    case ERR_DROP:
        Com_Printf("********************\nERROR: %s\n********************\n", msg);
        SV_Shutdown(va("server crashed: %s\n", msg), qfalse);
        CL_Drop();
        recursive = 0;
        return;

    case ERR_FATAL:
    default:
        SV_Shutdown(va("server fatal crashed: %s\n", msg), qfalse);
        CL_Shutdown();
        break;
    }

    Sys_Error("%s", msg);
}

/* ==========================================================================
   Engine Initialization
   ========================================================================== */

void Qcommon_Init(int argc, char **argv)
{
    Com_Printf("\n");
    Com_Printf("=== Soldier of Fortune ===\n");
    Com_Printf("Static Recompilation v%s\n", "0.1.0");
    Com_Printf("Based on id Tech 2 (Quake II Engine)\n");
    Com_Printf("Original by Raven Software, 2000\n");
    Com_Printf("\n");

    /* Initialize subsystems in dependency order */
    Z_Init();
    Cbuf_Init();
    Cmd_Init();
    Cvar_Init();

    /* Register core commands */
    Cmd_AddCommand("quit", Sys_Quit);
    Cmd_AddCommand("error", NULL);  /* placeholder */

    /* Register core cvars */
    developer = Cvar_Get("developer", "0", 0);
    timescale = Cvar_Get("timescale", "1", 0);
    fixedtime = Cvar_Get("fixedtime", "0", 0);
    dedicated = Cvar_Get("dedicated", "0", CVAR_NOSET);
    com_speeds = Cvar_Get("com_speeds", "0", 0);
    logfile_cvar = Cvar_Get("logfile", "0", 0);
    showtrace = Cvar_Get("showtrace", "0", 0);

    /* SoF-specific cvars */
    sof_version = Cvar_Get("version", "SoF Recomp v0.1.0", CVAR_SERVERINFO | CVAR_NOSET);
    gore_detail = Cvar_Get("gore_detail", "2", CVAR_ARCHIVE);  /* 0=off, 1=low, 2=full */

    /* SoF gore zone cvars (from binary analysis: 85 cvars registered) */
    Cvar_Get("ghl_specular", "1", CVAR_ARCHIVE);
    Cvar_Get("ghl_mip", "1", CVAR_ARCHIVE);

    /* Initialize filesystem */
    FS_InitFilesystem();

    /* Execute default config */
    Cbuf_AddText("exec default.cfg\n");
    Cbuf_AddText("exec config.cfg\n");
    Cbuf_Execute();

    /* Parse command line arguments */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (argv[i][0] == '+') {
                Cbuf_AddText(va("%s ", argv[i] + 1));
                /* Add remaining args until next + command */
                while (i + 1 < argc && argv[i + 1][0] != '+') {
                    i++;
                    Cbuf_AddText(va("%s ", argv[i]));
                }
                Cbuf_AddText("\n");
            }
        }
        Cbuf_Execute();
    }

    /* Initialize subsystems that depend on filesystem */
    /* TODO: NET_Init(), Netchan_Init() */

    Com_Printf("====== Soldier of Fortune Initialized ======\n\n");

    /* Initialize GHOUL model system */
    GHOUL_Init();

    /* Initialize input */
    IN_Init();

    /* Initialize renderer */
    if (!dedicated->value) {
        R_Init(NULL, NULL);
    }
}

/* ==========================================================================
   Main Frame
   ========================================================================== */

void Qcommon_Frame(int msec)
{
    int     time_before, time_between, time_after;

    if (fixedtime && fixedtime->value)
        msec = (int)fixedtime->value;
    else if (timescale && timescale->value)
        msec = (int)(msec * timescale->value);

    if (msec < 1)
        msec = 1;

    if (com_speeds && com_speeds->value)
        time_before = Sys_Milliseconds();

    /* Execute any pending console commands */
    Cbuf_Execute();

    if (com_speeds && com_speeds->value)
        time_between = Sys_Milliseconds();

    /* Run server frame */
    SV_Frame(msec);

    /* Run client frame (includes rendering) */
    if (!dedicated || !dedicated->value) {
        CL_Frame(msec);

        /* Render frame */
        R_BeginFrame(0.0f);
        /* TODO: R_RenderFrame(&cl.refdef) when client is implemented */
        R_EndFrame();
    }

    if (com_speeds && com_speeds->value) {
        time_after = Sys_Milliseconds();
        Com_Printf("all:%3d sv:%3d cl:%3d\n",
            time_after - time_before,
            time_between - time_before,
            time_after - time_between);
    }
}

/* ==========================================================================
   Shutdown
   ========================================================================== */

void Qcommon_Shutdown(void)
{
    if (logfile) {
        fclose(logfile);
        logfile = NULL;
    }
}

/* ==========================================================================
   Stub Client/Server (until properly implemented)
   ========================================================================== */

void CL_Init(void) {}
void CL_Drop(void) {}
void CL_Shutdown(void)
{
    IN_Shutdown();
    R_Shutdown();
}
void CL_Frame(int msec) { (void)msec; }

void SV_Init(void) {}
void SV_Shutdown(const char *finalmsg, qboolean reconnect)
{
    (void)finalmsg;
    (void)reconnect;
}
void SV_Frame(int msec) { (void)msec; }
