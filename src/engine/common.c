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
#include "../sound/snd_local.h"
#include "../client/console.h"

#include <time.h>

/* Forward declarations — input system (client/in_sdl.c) */
extern void IN_Init(void);
extern void IN_Shutdown(void);
extern void IN_GetMouseDelta(int *dx, int *dy);

/* Forward declarations — client input (client/cl_input.c) */
extern void CL_InitInput(void);
extern void CL_CreateCmd(usercmd_t *cmd, int msec);

/* Forward declarations — server frame (server/sv_game.c) */
extern void SV_RunGameFrame(void);
extern void SV_ClientThink(usercmd_t *cmd);
extern qboolean SV_GetPlayerState(vec3_t origin, vec3_t angles, float *viewheight);
extern qboolean SV_GetPlayerHealth(int *health, int *max_health);
extern const char *SV_GetPlayerWeapon(void);
extern void SV_GetPlayerBlend(float *blend);

/* Forward declaration — freecam toggle (defined below in client section) */
static void Cmd_Freecam_f(void);

/* Forward declaration — client command forwarding */
extern void SV_ExecuteClientCommand(void);

static void Cmd_ForwardToServer(void)
{
    /* Re-tokenize with "cmd" stripped so gi.argv(0) returns the actual command */
    const char *args = Cmd_Args();
    if (args && args[0]) {
        Cmd_TokenizeString((char *)args, qfalse);
        SV_ExecuteClientCommand();
    }
}

/* Forward declaration — HUD drawing (defined below in HUD section) */
static void SCR_DrawHUD(float frametime);

/* ANGLE2SHORT / SHORT2ANGLE for usercmd angle encoding */
#define ANGLE2SHORT(x)  ((int)((x)*65536.0f/360.0f) & 65535)
#define SHORT2ANGLE(x)  ((x)*(360.0f/65536.0f))

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

    /* Route to in-game console */
    Con_Print(msg);
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
    Cmd_AddCommand("freecam", Cmd_Freecam_f);
    Cmd_AddCommand("cmd", Cmd_ForwardToServer);

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

    /* Initialize game module (was gamex86.dll in original) */
    SV_InitGameProgs();

    /* Initialize console */
    Con_Init();

    /* Initialize input */
    IN_Init();
    CL_InitInput();

    /* Initialize renderer */
    if (!dedicated->value) {
        R_Init(NULL, NULL);
    }

    /* Initialize sound (replaces Defsnd.dll/EAXSnd.dll/A3Dsnd.dll) */
    S_Init();
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

    /* Run client frame (input, view setup) */
    if (!dedicated || !dedicated->value) {
        CL_Frame(msec);

        /* Animate console slide */
        {
            float speed = msec * 0.004f;  /* ~4 units per ms */
            if (con.current_frac < con.dest_frac) {
                con.current_frac += speed;
                if (con.current_frac > con.dest_frac)
                    con.current_frac = con.dest_frac;
            } else if (con.current_frac > con.dest_frac) {
                con.current_frac -= speed;
                if (con.current_frac < con.dest_frac)
                    con.current_frac = con.dest_frac;
            }
        }

        /* Render frame */
        R_BeginFrame(0.0f);
        SCR_DrawHUD(msec / 1000.0f);
        Con_DrawNotify();
        Con_DrawConsole(con.current_frac);
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
   Client State
   ========================================================================== */

typedef enum {
    CA_DISCONNECTED,    /* not connected to a server */
    CA_CONNECTING,      /* awaiting connection response */
    CA_CONNECTED,       /* connection established, loading map */
    CA_ACTIVE           /* fully in-game, rendering world */
} connstate_t;

static connstate_t  cl_state = CA_DISCONNECTED;
static refdef_t     cl_refdef;          /* current frame's render view */
static float        cl_time;            /* accumulated time */
static vec3_t       cl_viewangles;      /* accumulated mouse look angles */
static qboolean     cl_use_freecam;     /* toggle: freecam vs player movement */

void CL_Init(void)
{
    memset(&cl_refdef, 0, sizeof(cl_refdef));
    cl_state = CA_DISCONNECTED;
    cl_time = 0;
    VectorClear(cl_viewangles);
    cl_use_freecam = qfalse;
}

void CL_Drop(void)
{
    cl_state = CA_DISCONNECTED;
}

void CL_Shutdown(void)
{
    S_Shutdown();
    IN_Shutdown();
    R_Shutdown();
}

/* Toggle freecam console command */
static void Cmd_Freecam_f(void)
{
    cl_use_freecam = !cl_use_freecam;
    Com_Printf("Freecam: %s\n", cl_use_freecam ? "ON" : "OFF");
}

/*
 * CL_Frame — Client frame processing
 *
 * Runs every frame. Handles input, updates the view, builds refdef_t.
 * In the original Q2, this also handled network parsing and prediction.
 * For our unified binary, the server runs in-process.
 */
void CL_Frame(int msec)
{
    float frametime = msec / 1000.0f;
    cl_time += frametime;

    /* If world is loaded and we're disconnected, go active */
    if (R_WorldLoaded() && cl_state < CA_ACTIVE)
        cl_state = CA_ACTIVE;

    if (cl_state != CA_ACTIVE)
        return;

    /* Gather mouse input (always, for both modes) */
    if (!Con_IsVisible()) {
        int mx, my;
        IN_GetMouseDelta(&mx, &my);
        cl_viewangles[1] -= mx * 0.15f;    /* yaw */
        cl_viewangles[0] += my * 0.15f;    /* pitch */
        if (cl_viewangles[0] > 89) cl_viewangles[0] = 89;
        if (cl_viewangles[0] < -89) cl_viewangles[0] = -89;
    }

    if (cl_use_freecam) {
        /* Freecam mode — direct camera control */
        if (!Con_IsVisible()) {
            extern qboolean key_down[];
            float fwd = 0, side = 0, up = 0;
            if (key_down['w']) fwd += 1;
            if (key_down['s']) fwd -= 1;
            if (key_down['d']) side += 1;
            if (key_down['a']) side -= 1;
            if (key_down[' ']) up += 200.0f * frametime;
            if (key_down[133]) up -= 200.0f * frametime;

            R_SetCameraAngles(cl_viewangles);
            R_UpdateCamera(fwd, side, up, 0, 0, frametime);
        }

        /* Build refdef from freecam */
        {
            vec3_t org, ang;
            R_GetCameraOrigin(org);
            R_GetCameraAngles(ang);

            memset(&cl_refdef, 0, sizeof(cl_refdef));
            cl_refdef.x = 0;
            cl_refdef.y = 0;
            cl_refdef.width = g_display.width;
            cl_refdef.height = g_display.height;
            cl_refdef.fov_x = 90.0f;
            cl_refdef.fov_y = 73.74f;
            cl_refdef.time = cl_time;
            VectorCopy(org, cl_refdef.vieworg);
            VectorCopy(ang, cl_refdef.viewangles);
        }
    } else {
        /* Player movement mode — build usercmd, send to game */
        usercmd_t cmd;
        CL_CreateCmd(&cmd, msec);

        /* Pack view angles into usercmd */
        cmd.angles[0] = (short)ANGLE2SHORT(cl_viewangles[0]);
        cmd.angles[1] = (short)ANGLE2SHORT(cl_viewangles[1]);
        cmd.angles[2] = 0;

        /* Send to game module */
        if (!Con_IsVisible())
            SV_ClientThink(&cmd);

        /* Build refdef from player entity state */
        {
            vec3_t org, ang;
            float vh = 0;

            if (SV_GetPlayerState(org, ang, &vh)) {
                org[2] += vh;  /* add viewheight for eye position */

                memset(&cl_refdef, 0, sizeof(cl_refdef));
                cl_refdef.x = 0;
                cl_refdef.y = 0;
                cl_refdef.width = g_display.width;
                cl_refdef.height = g_display.height;
                cl_refdef.fov_x = 90.0f;
                cl_refdef.fov_y = 73.74f;
                cl_refdef.time = cl_time;
                VectorCopy(org, cl_refdef.vieworg);
                VectorCopy(ang, cl_refdef.viewangles);

                /* Sync camera for PVS culling */
                R_SetCameraOrigin(org);
                R_SetCameraAngles(ang);
            }
        }
    }
}

/* ==========================================================================
   HUD Drawing
   ========================================================================== */

/* Pickup message display */
static char     hud_pickup_msg[64];
static float    hud_pickup_time;

void HUD_SetPickupMessage(const char *msg)
{
    Q_strncpyz(hud_pickup_msg, msg, sizeof(hud_pickup_msg));
    hud_pickup_time = 3.0f;  /* display for 3 seconds */
}

static void SCR_DrawCrosshair(void)
{
    int cx = g_display.width / 2;
    int cy = g_display.height / 2;
    int size = 2;
    int gap = 3;
    int len = 6;
    /* Pack white color as ARGB for R_DrawFill */
    int white = (int)(0xFF000000 | 0x00FFFFFF);

    /* Crosshair: four lines forming a + with a gap in the center */
    R_DrawFill(cx - gap - len, cy - size/2, len, size, white);  /* left */
    R_DrawFill(cx + gap + 1,  cy - size/2, len, size, white);  /* right */
    R_DrawFill(cx - size/2, cy - gap - len, size, len, white);  /* top */
    R_DrawFill(cx - size/2, cy + gap + 1,  size, len, white);  /* bottom */
}

static void SCR_DrawHUD(float frametime)
{
    int health = 0, max_health = 100;
    char buf[32];

    if (cl_state != CA_ACTIVE)
        return;

    /* Crosshair */
    SCR_DrawCrosshair();

    /* Health display - bottom left */
    if (SV_GetPlayerHealth(&health, &max_health)) {
        /* Health label */
        R_SetDrawColor(0.8f, 0.8f, 0.8f, 1.0f);
        R_DrawString(16, g_display.height - 40, "HEALTH");

        /* Health value - color based on amount */
        if (health > 60)
            R_SetDrawColor(1.0f, 1.0f, 1.0f, 1.0f);
        else if (health > 25)
            R_SetDrawColor(1.0f, 1.0f, 0.0f, 1.0f);
        else
            R_SetDrawColor(1.0f, 0.2f, 0.2f, 1.0f);

        snprintf(buf, sizeof(buf), "%d", health);
        R_DrawString(16, g_display.height - 28, buf);
    }

    /* Weapon display - bottom right */
    {
        const char *wname = SV_GetPlayerWeapon();
        if (wname && wname[0]) {
            int len = (int)strlen(wname);
            int x = g_display.width - 16 - len * 8;

            R_SetDrawColor(0.8f, 0.8f, 0.8f, 1.0f);
            R_DrawString(x, g_display.height - 28, wname);
        }

        /* Reset to default green for console */
        R_SetDrawColor(0.0f, 1.0f, 0.0f, 1.0f);
    }

    /* Pickup message - center screen, fades out */
    if (hud_pickup_time > 0) {
        float alpha = hud_pickup_time > 1.0f ? 1.0f : hud_pickup_time;
        int len = (int)strlen(hud_pickup_msg);
        int x = (g_display.width - len * 8) / 2;
        int y = g_display.height / 2 + 40;

        R_SetDrawColor(1.0f, 1.0f, 1.0f, alpha);
        R_DrawString(x, y, hud_pickup_msg);
        R_SetDrawColor(0.0f, 1.0f, 0.0f, 1.0f);

        hud_pickup_time -= frametime;
    }

    /* Damage flash — full-screen red overlay */
    {
        float blend[4];
        SV_GetPlayerBlend(blend);
        if (blend[3] > 0.01f) {
            R_DrawFadeScreenColor(blend[0], blend[1], blend[2], blend[3]);
        }
    }

    /* Death message */
    if (health <= 0) {
        R_SetDrawColor(1.0f, 0.2f, 0.2f, 1.0f);
        R_DrawString((g_display.width - 14 * 8) / 2, g_display.height / 2 - 16, "YOU ARE DEAD");
        R_SetDrawColor(0.8f, 0.8f, 0.8f, 1.0f);
        R_DrawString((g_display.width - 23 * 8) / 2, g_display.height / 2, "Press FIRE to respawn");
        R_SetDrawColor(0.0f, 1.0f, 0.0f, 1.0f);
    }
}

void SV_Init(void) {}
void SV_Shutdown(const char *finalmsg, qboolean reconnect)
{
    (void)finalmsg;
    (void)reconnect;
    SV_ShutdownGameProgs();
}
/* Server frame — runs game at 10Hz tick rate */
static int sv_frame_residual = 0;

void SV_Frame(int msec)
{
    sv_frame_residual += msec;

    /* Run game frames at 100ms intervals (10 Hz) */
    while (sv_frame_residual >= 100) {
        sv_frame_residual -= 100;
        SV_RunGameFrame();
    }
}
