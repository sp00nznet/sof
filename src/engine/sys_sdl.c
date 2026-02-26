/*
 * sys_sdl.c - SDL2-based platform layer for SoF recompilation
 *
 * Replaces the original Win32 platform code (sys_win.c in Quake II terms).
 * Handles window creation, input, timers, and OpenGL context via SDL2.
 *
 * This file implements the functions declared in win32_compat.h.
 */

#include "win32_compat.h"
#include "../common/sof_types.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ==========================================================================
   Global State
   ========================================================================== */

sof_display_t   g_display;
sof_input_t     g_input;
sof_paths_t     g_paths;

static int      sys_initialized = 0;

/* ==========================================================================
   Platform Init / Shutdown
   ========================================================================== */

int Sys_PlatformInit(int argc, char **argv)
{
    if (sys_initialized)
        return 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    /* Set up paths */
    {
        char *base = SDL_GetBasePath();
        if (base) {
            snprintf(g_paths.game_path, sizeof(g_paths.game_path), "%s", base);
            SDL_free(base);
        }

        char *pref = SDL_GetPrefPath("Raven Software", "Soldier of Fortune");
        if (pref) {
            snprintf(g_paths.save_path, sizeof(g_paths.save_path), "%s", pref);
            snprintf(g_paths.ini_path, sizeof(g_paths.ini_path), "%ssof.ini", pref);
            SDL_free(pref);
        }
    }

    memset(&g_display, 0, sizeof(g_display));
    memset(&g_input, 0, sizeof(g_input));

    sys_initialized = 1;
    return 1;
}

void Sys_PlatformShutdown(void)
{
    if (!sys_initialized)
        return;

    Sys_DestroyWindow();
    SDL_Quit();
    sys_initialized = 0;
}

/* ==========================================================================
   Timer
   ========================================================================== */

int Sys_Milliseconds(void)
{
    return (int)SDL_GetTicks();
}

/* ==========================================================================
   Window Management
   ========================================================================== */

int Sys_CreateWindow(int width, int height, int fullscreen)
{
    uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;

    if (fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;

    /* Request OpenGL 2.1 compatibility (enough for SoF's GL 1.1 code) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    g_display.window = SDL_CreateWindow(
        "Soldier of Fortune",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, flags
    );

    if (!g_display.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }

    g_display.gl_context = SDL_GL_CreateContext(g_display.window);
    if (!g_display.gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_display.window);
        g_display.window = NULL;
        return 0;
    }

    /* Enable vsync by default (1 = vsync on) */
    SDL_GL_SetSwapInterval(1);

    g_display.width = width;
    g_display.height = height;
    g_display.fullscreen = fullscreen;

    return 1;
}

void Sys_DestroyWindow(void)
{
    if (g_display.gl_context) {
        SDL_GL_DeleteContext(g_display.gl_context);
        g_display.gl_context = NULL;
    }
    if (g_display.window) {
        SDL_DestroyWindow(g_display.window);
        g_display.window = NULL;
    }
}

void Sys_SetWindowTitle(const char *title)
{
    if (g_display.window)
        SDL_SetWindowTitle(g_display.window, title);
}

int Sys_SetDisplayMode(int width, int height, int fullscreen)
{
    if (!g_display.window)
        return 0;

    if (fullscreen) {
        SDL_DisplayMode mode;
        mode.w = width;
        mode.h = height;
        mode.format = 0;
        mode.refresh_rate = 0;
        mode.driverdata = NULL;
        SDL_SetWindowDisplayMode(g_display.window, &mode);
        SDL_SetWindowFullscreen(g_display.window, SDL_WINDOW_FULLSCREEN);
    } else {
        SDL_SetWindowFullscreen(g_display.window, 0);
        SDL_SetWindowSize(g_display.window, width, height);
        SDL_SetWindowPosition(g_display.window,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    g_display.width = width;
    g_display.height = height;
    g_display.fullscreen = fullscreen;

    return 1;
}

void Sys_GetDesktopSize(int *width, int *height)
{
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        *width = mode.w;
        *height = mode.h;
    } else {
        *width = 1920;
        *height = 1080;
    }
}

void Sys_SwapBuffers(void)
{
    if (g_display.window)
        SDL_GL_SwapWindow(g_display.window);
}

/* ==========================================================================
   Input
   ========================================================================== */

void Sys_GrabMouse(int grab)
{
    if (!g_display.window)
        return;

    SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
    SDL_SetWindowGrab(g_display.window, grab ? SDL_TRUE : SDL_FALSE);
    g_input.mouse_grabbed = grab;
}

void Sys_WarpMouse(int x, int y)
{
    if (g_display.window)
        SDL_WarpMouseInWindow(g_display.window, x, y);
}

void Sys_GetMousePos(int *x, int *y)
{
    SDL_GetMouseState(x, y);
}

void Sys_ShowCursor(int show)
{
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
    g_input.cursor_visible = show;
}

/* Forward declaration — implemented in client/in_sdl.c */
extern void IN_ProcessSDLEvent(SDL_Event *event);

void Sys_PumpEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        IN_ProcessSDLEvent(&event);
    }
}

/* ==========================================================================
   OpenGL
   ========================================================================== */

void *Sys_GL_GetProcAddress(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}

int Sys_GL_SetAttribute(int attr, int value)
{
    return SDL_GL_SetAttribute(attr, value);
}

int Sys_GL_MakeCurrent(void)
{
    if (!g_display.window || !g_display.gl_context)
        return 0;
    return SDL_GL_MakeCurrent(g_display.window, g_display.gl_context) == 0;
}

/* ==========================================================================
   Message Box
   ========================================================================== */

void Sys_MessageBox(const char *title, const char *message, int error)
{
    uint32_t flags = error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(flags, title, message, g_display.window);
}

/* ==========================================================================
   Version Shim
   ========================================================================== */

void Sys_GetOSVersion(sof_osversion_t *ver)
{
    /* Return Windows 2000 (5.0 build 2195) — what SoF was designed for */
    ver->dwMajorVersion = 5;
    ver->dwMinorVersion = 0;
    ver->dwBuildNumber = 2195;
    ver->dwPlatformId = 2;  /* VER_PLATFORM_WIN32_NT */
}

/* ==========================================================================
   Registry → INI File Redirection
   ========================================================================== */

/*
 * Simple INI file format:
 *   key=value\n
 * All keys are flat (no sections) since the original only used one
 * registry path: HKLM\Software\Raven Software\SoF
 */

static int ini_read(const char *key, char *buf, int bufsize)
{
    FILE *f = fopen(g_paths.ini_path, "r");
    if (!f)
        return 0;

    char line[512];
    int keylen = (int)strlen(key);

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            char *val = line + keylen + 1;
            /* Strip trailing newline */
            char *nl = strchr(val, '\n');
            if (nl) *nl = '\0';
            nl = strchr(val, '\r');
            if (nl) *nl = '\0';
            snprintf(buf, bufsize, "%s", val);
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

static int ini_write(const char *key, const char *value)
{
    /* Read existing file */
    char existing[16384] = {0};
    FILE *f = fopen(g_paths.ini_path, "r");
    if (f) {
        size_t n = fread(existing, 1, sizeof(existing) - 1, f);
        existing[n] = '\0';
        fclose(f);
    }

    /* Write back with updated/new key */
    f = fopen(g_paths.ini_path, "w");
    if (!f)
        return 0;

    int keylen = (int)strlen(key);
    int replaced = 0;
    char *line = existing;

    while (*line) {
        char *nl = strchr(line, '\n');
        int linelen = nl ? (int)(nl - line + 1) : (int)strlen(line);

        if (!replaced && strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            fprintf(f, "%s=%s\n", key, value);
            replaced = 1;
        } else {
            fwrite(line, 1, linelen, f);
        }
        line += linelen;
    }

    if (!replaced)
        fprintf(f, "%s=%s\n", key, value);

    fclose(f);
    return 1;
}

int Sys_RegReadString(const char *key, char *buf, int bufsize)
{
    return ini_read(key, buf, bufsize);
}

int Sys_RegWriteString(const char *key, const char *value)
{
    return ini_write(key, value);
}

int Sys_RegReadInt(const char *key, int *value)
{
    char buf[64];
    if (ini_read(key, buf, sizeof(buf))) {
        *value = atoi(buf);
        return 1;
    }
    return 0;
}

int Sys_RegWriteInt(const char *key, int value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", value);
    return ini_write(key, buf);
}

int Sys_RegDeleteKey(const char *key)
{
    /* Read existing, write back without the key */
    char existing[16384] = {0};
    FILE *f = fopen(g_paths.ini_path, "r");
    if (!f)
        return 0;

    size_t n = fread(existing, 1, sizeof(existing) - 1, f);
    existing[n] = '\0';
    fclose(f);

    f = fopen(g_paths.ini_path, "w");
    if (!f)
        return 0;

    int keylen = (int)strlen(key);
    char *line = existing;

    while (*line) {
        char *nl = strchr(line, '\n');
        int linelen = nl ? (int)(nl - line + 1) : (int)strlen(line);

        if (!(strncmp(line, key, keylen) == 0 && line[keylen] == '=')) {
            fwrite(line, 1, linelen, f);
        }
        line += linelen;
    }

    fclose(f);
    return 1;
}

/* ==========================================================================
   CD Audio Stubs
   ========================================================================== */

/*
 * CD Audio — plays music tracks via the sound system
 *
 * SoF originally used CD audio for background music.
 * We load WAV files from "music/trackNN.wav" and play them through
 * the sound system's S_StartSound/S_StartLocalSound interface.
 *
 * Track numbers map to files: track02.wav, track03.wav, etc.
 * (Track 1 is typically the data track on a game CD.)
 */

extern void S_StartLocalSound(const char *name);
extern void S_StopAllSounds(void);

static int  cdaudio_track;
static int  cdaudio_looping;
static int  cdaudio_playing;

int CDAudio_Init(void)
{
    cdaudio_track = 0;
    cdaudio_playing = 0;
    Com_Printf("CD Audio: initialized (WAV track playback)\n");
    return 0;
}

void CDAudio_Shutdown(void)
{
    CDAudio_Stop();
}

int CDAudio_Play(int track, int looping)
{
    char trackname[64];

    cdaudio_track = track;
    cdaudio_looping = looping;
    cdaudio_playing = 1;

    /* Try to load the music track */
    snprintf(trackname, sizeof(trackname), "music/track%02d.wav", track);
    Com_Printf("CD Audio: play track %d (%s) %s\n", track, trackname,
               looping ? "[looping]" : "");

    S_StartLocalSound(trackname);
    return 1;
}

void CDAudio_Stop(void)
{
    if (cdaudio_playing) {
        cdaudio_playing = 0;
        Com_Printf("CD Audio: stopped\n");
    }
}

void CDAudio_Update(void)
{
    /* In a full implementation, we'd check if the track finished
       and restart it if looping is enabled. For now, the sound
       system handles playback duration naturally. */
}
