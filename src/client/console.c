/*
 * console.c - In-game drop-down console
 *
 * Tilde (~) toggles the console. Text from Com_Printf is routed here.
 * Commands are typed and executed through the Cmd system.
 * Scrollback with PgUp/PgDn, command history with Up/Down arrows.
 *
 * Renders using simple GL character drawing (8x8 pixel grid font).
 */

#include "console.h"
#include "keys.h"
#include "../common/qcommon.h"
#include "../renderer/r_local.h"

/* ==========================================================================
   Global Console State
   ========================================================================== */

console_t con;

/* ==========================================================================
   Console Init / Shutdown
   ========================================================================== */

void Con_Init(void)
{
    memset(&con, 0, sizeof(con));
    con.initialized = qtrue;
    con.dest_frac = 0;
    con.current_frac = 0;
    con.history_pos = -1;
    con.line_head = 0;
    con.line_count = 0;
    con.scroll_offset = 0;

    Com_Printf("Console initialized\n");
}

void Con_Shutdown(void)
{
    con.initialized = qfalse;
}

/* ==========================================================================
   Text Output
   ========================================================================== */

/*
 * Add a line to the scrollback buffer
 */
static void Con_AddLine(const char *line)
{
    Q_strncpyz(con.lines[con.line_head], line, CON_LINEWIDTH + 1);
    con.line_head = (con.line_head + 1) % CON_MAXLINES;
    if (con.line_count < CON_MAXLINES)
        con.line_count++;
}

/*
 * Add a notify line (brief text at top of screen during gameplay)
 */
static void Con_AddNotify(const char *line)
{
    Q_strncpyz(con.notify_text[con.notify_head], line, CON_LINEWIDTH + 1);
    con.notify_times[con.notify_head] = (float)Sys_Milliseconds() / 1000.0f;
    con.notify_head = (con.notify_head + 1) % CON_NOTIFY_TIMES;
}

/*
 * Con_Print - Route text to console scrollback and notify
 *
 * Called from Com_Printf. Handles multi-line text by splitting at newlines.
 */
void Con_Print(const char *txt)
{
    static char linebuf[CON_LINEWIDTH + 1];
    static int  linepos = 0;

    if (!con.initialized)
        return;

    while (*txt) {
        if (*txt == '\n' || linepos >= CON_LINEWIDTH) {
            linebuf[linepos] = '\0';
            Con_AddLine(linebuf);
            Con_AddNotify(linebuf);
            linepos = 0;

            if (*txt == '\n')
                txt++;
            continue;
        }

        /* Skip color codes (^0-^9) if present */
        if (*txt == '^' && txt[1] >= '0' && txt[1] <= '9') {
            txt += 2;
            continue;
        }

        linebuf[linepos++] = *txt++;
    }
}

/* ==========================================================================
   Console Toggle
   ========================================================================== */

void Con_ToggleConsole(void)
{
    if (con.dest_frac > 0) {
        con.dest_frac = 0;
    } else {
        con.dest_frac = 0.5f;   /* half-screen */
    }

    /* Clear input on toggle */
    con.input[0] = '\0';
    con.input_pos = 0;
    con.input_len = 0;
    con.scroll_offset = 0;
}

qboolean Con_IsVisible(void)
{
    return con.current_frac > 0.01f;
}

void Con_ClearNotify(void)
{
    int i;
    for (i = 0; i < CON_NOTIFY_TIMES; i++)
        con.notify_times[i] = 0;
}

void Con_Clear(void)
{
    con.line_count = 0;
    con.line_head = 0;
    con.scroll_offset = 0;
}

/* ==========================================================================
   Input Handling
   ========================================================================== */

/*
 * Add current input to command history
 */
static void Con_AddHistory(const char *cmd)
{
    int idx;

    if (cmd[0] == '\0')
        return;

    /* Don't add duplicates of the most recent */
    if (con.history_count > 0) {
        idx = (con.history_count - 1) % 32;
        if (strcmp(con.history[idx], cmd) == 0)
            return;
    }

    idx = con.history_count % 32;
    Q_strncpyz(con.history[idx], cmd, CON_INPUTSIZE);
    con.history_count++;
    con.history_pos = -1;
}

/*
 * Con_KeyEvent - Handle special keys when console is active
 */
void Con_KeyEvent(int key)
{
    if (!con.initialized)
        return;

    switch (key) {
    case K_ENTER:
    case K_KP_ENTER:
        /* Execute the command */
        if (con.input_len > 0) {
            Con_AddHistory(con.input);
            Com_Printf("] %s\n", con.input);
            Cbuf_AddText(con.input);
            Cbuf_AddText("\n");
        }
        con.input[0] = '\0';
        con.input_pos = 0;
        con.input_len = 0;
        con.scroll_offset = 0;
        break;

    case K_BACKSPACE:
        if (con.input_pos > 0) {
            /* Remove character before cursor */
            memmove(&con.input[con.input_pos - 1],
                    &con.input[con.input_pos],
                    con.input_len - con.input_pos + 1);
            con.input_pos--;
            con.input_len--;
        }
        break;

    case K_DEL:
        if (con.input_pos < con.input_len) {
            memmove(&con.input[con.input_pos],
                    &con.input[con.input_pos + 1],
                    con.input_len - con.input_pos);
            con.input_len--;
        }
        break;

    case K_LEFTARROW:
        if (con.input_pos > 0)
            con.input_pos--;
        break;

    case K_RIGHTARROW:
        if (con.input_pos < con.input_len)
            con.input_pos++;
        break;

    case K_HOME:
        con.input_pos = 0;
        break;

    case K_END:
        con.input_pos = con.input_len;
        break;

    case K_UPARROW:
        /* Browse command history (older) */
        if (con.history_count > 0) {
            if (con.history_pos < 0)
                con.history_pos = con.history_count - 1;
            else if (con.history_pos > 0)
                con.history_pos--;

            {
                int idx = con.history_pos % 32;
                Q_strncpyz(con.input, con.history[idx], CON_INPUTSIZE);
                con.input_len = (int)strlen(con.input);
                con.input_pos = con.input_len;
            }
        }
        break;

    case K_DOWNARROW:
        /* Browse command history (newer) */
        if (con.history_pos >= 0) {
            con.history_pos++;
            if (con.history_pos >= con.history_count) {
                /* Back to empty input */
                con.history_pos = -1;
                con.input[0] = '\0';
                con.input_pos = 0;
                con.input_len = 0;
            } else {
                int idx = con.history_pos % 32;
                Q_strncpyz(con.input, con.history[idx], CON_INPUTSIZE);
                con.input_len = (int)strlen(con.input);
                con.input_pos = con.input_len;
            }
        }
        break;

    case K_PGUP:
        con.scroll_offset += 5;
        if (con.scroll_offset > con.line_count - 1)
            con.scroll_offset = con.line_count - 1;
        break;

    case K_PGDN:
        con.scroll_offset -= 5;
        if (con.scroll_offset < 0)
            con.scroll_offset = 0;
        break;
    }
}

/*
 * Con_CharEvent - Handle typed character input
 */
void Con_CharEvent(int ch)
{
    if (!con.initialized)
        return;

    /* Ignore non-printable and tilde (toggle key) */
    if (ch < 32 || ch > 126 || ch == '~' || ch == '`')
        return;

    if (con.input_len >= CON_INPUTSIZE - 1)
        return;

    /* Insert character at cursor */
    memmove(&con.input[con.input_pos + 1],
            &con.input[con.input_pos],
            con.input_len - con.input_pos + 1);
    con.input[con.input_pos] = (char)ch;
    con.input_pos++;
    con.input_len++;
}

/* ==========================================================================
   Drawing
   ========================================================================== */

/*
 * Draw a single character at pixel position using GL quads
 * Uses the classic 8x8 Q2 character set (16x16 grid = 256 chars)
 */
static void Con_DrawChar(int x, int y, int ch)
{
    /* For now, use R_DrawChar if available, otherwise a simple colored quad */
    R_DrawChar(x, y, ch);
}

/*
 * Draw a string at pixel position
 */
static void Con_DrawString(int x, int y, const char *s)
{
    while (*s) {
        Con_DrawChar(x, y, (unsigned char)*s);
        x += 8;
        s++;
    }
}

/*
 * Con_DrawConsole - Render the console overlay
 *
 * frac: 0.0 = hidden, 0.5 = half screen, 1.0 = full screen
 */
void Con_DrawConsole(float frac)
{
    int     i, y;
    int     rows;
    int     line_idx;
    int     screen_w, screen_h;
    int     con_height;
    char    prompt[CON_INPUTSIZE + 4];

    if (frac <= 0)
        return;

    /* Get screen dimensions from display */
    {
        extern sof_display_t g_display;
        screen_w = g_display.width;
        screen_h = g_display.height;
    }

    con_height = (int)(screen_h * frac);
    rows = (con_height / 8) - 2;   /* rows available for text, minus input line */

    if (rows < 1)
        return;

    /* Draw semi-transparent background */
    R_DrawFill(0, 0, screen_w, con_height, 0x80000010);

    /* Draw separator line */
    R_DrawFill(0, con_height - 2, screen_w, 2, 0xFF404040);

    /* Draw scrollback text */
    y = con_height - 24;    /* start above input line */

    for (i = 0; i < rows && i < con.line_count; i++) {
        /* Calculate which line to show (from bottom, with scroll offset) */
        line_idx = con.line_head - 1 - i - con.scroll_offset;
        while (line_idx < 0)
            line_idx += CON_MAXLINES;
        line_idx = line_idx % CON_MAXLINES;

        if (i + con.scroll_offset >= con.line_count)
            break;

        Con_DrawString(8, y, con.lines[line_idx]);
        y -= 8;
    }

    /* Draw scroll indicator */
    if (con.scroll_offset > 0) {
        Con_DrawString(screen_w / 2 - 16, con_height - 24, "^^^");
    }

    /* Draw input line */
    Com_sprintf(prompt, sizeof(prompt), "] %s", con.input);
    Con_DrawString(8, con_height - 12, prompt);

    /* Draw cursor (blinking) */
    if ((Sys_Milliseconds() / 500) & 1) {
        int cursor_x = 8 + (con.input_pos + 2) * 8;
        Con_DrawString(cursor_x, con_height - 12, "_");
    }
}

/*
 * Con_DrawNotify - Draw notify lines at top of screen during gameplay
 */
void Con_DrawNotify(void)
{
    int     i;
    int     y = 0;
    float   now;

    if (!con.initialized)
        return;

    now = (float)Sys_Milliseconds() / 1000.0f;

    for (i = 0; i < CON_NOTIFY_TIMES; i++) {
        int idx = (con.notify_head - CON_NOTIFY_TIMES + i + CON_NOTIFY_TIMES) % CON_NOTIFY_TIMES;

        if (con.notify_times[idx] == 0)
            continue;

        if (now - con.notify_times[idx] > CON_NOTIFY_TIME) {
            con.notify_times[idx] = 0;
            continue;
        }

        Con_DrawString(8, y, con.notify_text[idx]);
        y += 8;
    }
}
