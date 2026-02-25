/*
 * in_sdl.c - SDL2 input processing
 *
 * Maps SDL2 keyboard/mouse/gamepad events to Quake II key events.
 * Replaces the original Win32 input code (in_win.c) that used
 * GetCursorPos, SetCursorPos, ClipCursor, and raw WM_* messages.
 */

#include "../common/qcommon.h"
#include "../engine/win32_compat.h"
#include "keys.h"
#include "console.h"

#include <SDL2/SDL.h>

/* ==========================================================================
   SDL Scancode → Q2 Key Mapping
   ========================================================================== */

static int SDL_ScancodeToQ2(SDL_Scancode sc)
{
    switch (sc) {
    /* Letters (SDL scancodes map to lowercase ASCII) */
    case SDL_SCANCODE_A: return 'a';
    case SDL_SCANCODE_B: return 'b';
    case SDL_SCANCODE_C: return 'c';
    case SDL_SCANCODE_D: return 'd';
    case SDL_SCANCODE_E: return 'e';
    case SDL_SCANCODE_F: return 'f';
    case SDL_SCANCODE_G: return 'g';
    case SDL_SCANCODE_H: return 'h';
    case SDL_SCANCODE_I: return 'i';
    case SDL_SCANCODE_J: return 'j';
    case SDL_SCANCODE_K: return 'k';
    case SDL_SCANCODE_L: return 'l';
    case SDL_SCANCODE_M: return 'm';
    case SDL_SCANCODE_N: return 'n';
    case SDL_SCANCODE_O: return 'o';
    case SDL_SCANCODE_P: return 'p';
    case SDL_SCANCODE_Q: return 'q';
    case SDL_SCANCODE_R: return 'r';
    case SDL_SCANCODE_S: return 's';
    case SDL_SCANCODE_T: return 't';
    case SDL_SCANCODE_U: return 'u';
    case SDL_SCANCODE_V: return 'v';
    case SDL_SCANCODE_W: return 'w';
    case SDL_SCANCODE_X: return 'x';
    case SDL_SCANCODE_Y: return 'y';
    case SDL_SCANCODE_Z: return 'z';

    /* Numbers */
    case SDL_SCANCODE_1: return '1';
    case SDL_SCANCODE_2: return '2';
    case SDL_SCANCODE_3: return '3';
    case SDL_SCANCODE_4: return '4';
    case SDL_SCANCODE_5: return '5';
    case SDL_SCANCODE_6: return '6';
    case SDL_SCANCODE_7: return '7';
    case SDL_SCANCODE_8: return '8';
    case SDL_SCANCODE_9: return '9';
    case SDL_SCANCODE_0: return '0';

    /* Special keys */
    case SDL_SCANCODE_RETURN:       return K_ENTER;
    case SDL_SCANCODE_ESCAPE:       return K_ESCAPE;
    case SDL_SCANCODE_BACKSPACE:    return K_BACKSPACE;
    case SDL_SCANCODE_TAB:          return K_TAB;
    case SDL_SCANCODE_SPACE:        return K_SPACE;

    /* Punctuation */
    case SDL_SCANCODE_MINUS:        return '-';
    case SDL_SCANCODE_EQUALS:       return '=';
    case SDL_SCANCODE_LEFTBRACKET:  return '[';
    case SDL_SCANCODE_RIGHTBRACKET: return ']';
    case SDL_SCANCODE_BACKSLASH:    return '\\';
    case SDL_SCANCODE_SEMICOLON:    return ';';
    case SDL_SCANCODE_APOSTROPHE:   return '\'';
    case SDL_SCANCODE_GRAVE:        return '`';
    case SDL_SCANCODE_COMMA:        return ',';
    case SDL_SCANCODE_PERIOD:       return '.';
    case SDL_SCANCODE_SLASH:        return '/';

    /* Function keys */
    case SDL_SCANCODE_F1:   return K_F1;
    case SDL_SCANCODE_F2:   return K_F2;
    case SDL_SCANCODE_F3:   return K_F3;
    case SDL_SCANCODE_F4:   return K_F4;
    case SDL_SCANCODE_F5:   return K_F5;
    case SDL_SCANCODE_F6:   return K_F6;
    case SDL_SCANCODE_F7:   return K_F7;
    case SDL_SCANCODE_F8:   return K_F8;
    case SDL_SCANCODE_F9:   return K_F9;
    case SDL_SCANCODE_F10:  return K_F10;
    case SDL_SCANCODE_F11:  return K_F11;
    case SDL_SCANCODE_F12:  return K_F12;

    /* Navigation */
    case SDL_SCANCODE_INSERT:   return K_INS;
    case SDL_SCANCODE_DELETE:   return K_DEL;
    case SDL_SCANCODE_HOME:     return K_HOME;
    case SDL_SCANCODE_END:      return K_END;
    case SDL_SCANCODE_PAGEUP:   return K_PGUP;
    case SDL_SCANCODE_PAGEDOWN: return K_PGDN;

    /* Arrow keys */
    case SDL_SCANCODE_UP:       return K_UPARROW;
    case SDL_SCANCODE_DOWN:     return K_DOWNARROW;
    case SDL_SCANCODE_LEFT:     return K_LEFTARROW;
    case SDL_SCANCODE_RIGHT:    return K_RIGHTARROW;

    /* Modifiers */
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:   return K_SHIFT;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:    return K_CTRL;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:     return K_ALT;

    /* Numpad */
    case SDL_SCANCODE_KP_0:         return K_KP_INS;
    case SDL_SCANCODE_KP_1:         return K_KP_END;
    case SDL_SCANCODE_KP_2:         return K_KP_DOWNARROW;
    case SDL_SCANCODE_KP_3:         return K_KP_PGDN;
    case SDL_SCANCODE_KP_4:         return K_KP_LEFTARROW;
    case SDL_SCANCODE_KP_5:         return K_KP_5;
    case SDL_SCANCODE_KP_6:         return K_KP_RIGHTARROW;
    case SDL_SCANCODE_KP_7:         return K_KP_HOME;
    case SDL_SCANCODE_KP_8:         return K_KP_UPARROW;
    case SDL_SCANCODE_KP_9:         return K_KP_PGUP;
    case SDL_SCANCODE_KP_PERIOD:    return K_KP_DEL;
    case SDL_SCANCODE_KP_DIVIDE:    return K_KP_SLASH;
    case SDL_SCANCODE_KP_MINUS:     return K_KP_MINUS;
    case SDL_SCANCODE_KP_PLUS:      return K_KP_PLUS;
    case SDL_SCANCODE_KP_ENTER:     return K_KP_ENTER;

    case SDL_SCANCODE_PAUSE:        return K_PAUSE;

    default: return 0;
    }
}

/* ==========================================================================
   SDL Mouse Button → Q2 Key
   ========================================================================== */

static int SDL_MouseButtonToQ2(int button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:   return K_MOUSE1;
    case SDL_BUTTON_RIGHT:  return K_MOUSE2;
    case SDL_BUTTON_MIDDLE: return K_MOUSE3;
    case SDL_BUTTON_X1:     return K_MOUSE4;
    case SDL_BUTTON_X2:     return K_MOUSE5;
    default: return 0;
    }
}

/* ==========================================================================
   Mouse State
   ========================================================================== */

static int  mouse_dx, mouse_dy;
static int  mouse_active;
static cvar_t *sensitivity;
static cvar_t *m_filter;

/* ==========================================================================
   Input Init / Shutdown
   ========================================================================== */

void IN_Init(void)
{
    sensitivity = Cvar_Get("sensitivity", "3", CVAR_ARCHIVE);
    m_filter = Cvar_Get("m_filter", "0", CVAR_ARCHIVE);

    Key_Init();

    mouse_active = 0;
    mouse_dx = 0;
    mouse_dy = 0;

    Com_Printf("Input: SDL2 keyboard + mouse\n");
}

void IN_Shutdown(void)
{
    Sys_GrabMouse(0);
    mouse_active = 0;
}

void IN_Activate(qboolean active)
{
    if (active && !mouse_active) {
        Sys_GrabMouse(1);
        Sys_ShowCursor(0);
        mouse_active = 1;
    } else if (!active && mouse_active) {
        Sys_GrabMouse(0);
        Sys_ShowCursor(1);
        mouse_active = 0;
    }
}

/* ==========================================================================
   Mouse Movement — called by frame to get accumulated delta
   ========================================================================== */

void IN_GetMouseDelta(int *dx, int *dy)
{
    *dx = mouse_dx;
    *dy = mouse_dy;
    mouse_dx = 0;
    mouse_dy = 0;
}

void IN_AccumulateMouseDelta(int dx, int dy)
{
    mouse_dx += dx;
    mouse_dy += dy;
}

/* ==========================================================================
   SDL Event Processing
   Called from Sys_PumpEvents in sys_sdl.c — processes SDL events into
   Q2 key events and mouse delta accumulation.
   ========================================================================== */

void IN_ProcessSDLEvent(SDL_Event *event)
{
    int key;
    unsigned time = (unsigned)SDL_GetTicks();

    switch (event->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        key = SDL_ScancodeToQ2(event->key.keysym.scancode);
        if (key) {
            /* Tilde toggles console */
            if (key == '`' && event->type == SDL_KEYDOWN) {
                Con_ToggleConsole();
                break;
            }

            /* Route keys to console when visible */
            if (Con_IsVisible() && event->type == SDL_KEYDOWN) {
                Con_KeyEvent(key);
                break;
            }

            Key_Event(key, event->type == SDL_KEYDOWN, time);
        }
        break;

    case SDL_TEXTINPUT:
        /* Character input for console typing */
        if (Con_IsVisible()) {
            const char *text = event->text.text;
            while (*text) {
                Con_CharEvent((unsigned char)*text);
                text++;
            }
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        key = SDL_MouseButtonToQ2(event->button.button);
        if (key)
            Key_Event(key, event->type == SDL_MOUSEBUTTONDOWN, time);
        break;

    case SDL_MOUSEWHEEL:
        if (event->wheel.y > 0) {
            Key_Event(K_MWHEELUP, qtrue, time);
            Key_Event(K_MWHEELUP, qfalse, time);
        } else if (event->wheel.y < 0) {
            Key_Event(K_MWHEELDOWN, qtrue, time);
            Key_Event(K_MWHEELDOWN, qfalse, time);
        }
        break;

    case SDL_MOUSEMOTION:
        if (mouse_active) {
            IN_AccumulateMouseDelta(event->motion.xrel, event->motion.yrel);
        }
        break;

    case SDL_WINDOWEVENT:
        switch (event->window.event) {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            IN_Activate(qtrue);
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            IN_Activate(qfalse);
            Key_ClearStates();
            break;
        }
        break;

    case SDL_QUIT:
        Cbuf_AddText("quit\n");
        break;
    }
}
