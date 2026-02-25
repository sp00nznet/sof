/*
 * console.h - In-game console definitions
 *
 * Drop-down console activated by tilde (~), displays scrollback text
 * and accepts typed commands. Standard Q2/SoF console behavior.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "../common/q_shared.h"

/* ==========================================================================
   Console Limits
   ========================================================================== */

#define CON_TEXTSIZE        32768   /* scrollback buffer size */
#define CON_LINEWIDTH       78      /* characters per line */
#define CON_MAXLINES        512     /* max lines in scrollback */
#define CON_INPUTSIZE       256     /* max input line length */
#define CON_NOTIFY_TIMES    4       /* number of notify lines */
#define CON_NOTIFY_TIME     3.0f    /* seconds to show notify text */

/* ==========================================================================
   Console State
   ========================================================================== */

typedef struct {
    qboolean    initialized;

    /* Scrollback buffer */
    char        text[CON_TEXTSIZE];
    int         text_pos;           /* write position in circular buffer */
    int         total_lines;        /* total lines written */

    /* Line tracking */
    int         current_line;       /* line at bottom of display */
    int         display_line;       /* line being displayed at top (for scroll) */

    /* Input line */
    char        input[CON_INPUTSIZE];
    int         input_pos;          /* cursor position */
    int         input_len;          /* current input length */

    /* Visual state */
    float       dest_frac;          /* target fraction (0=hidden, 0.5=half, 1=full) */
    float       current_frac;       /* current interpolated fraction */
    int         visible_lines;      /* computed pixel height of visible console */

    /* Notify lines (brief messages at top of screen) */
    float       notify_times[CON_NOTIFY_TIMES];
    char        notify_text[CON_NOTIFY_TIMES][CON_LINEWIDTH + 1];
    int         notify_head;

    /* Command history */
    char        history[32][CON_INPUTSIZE];
    int         history_count;
    int         history_pos;        /* browse position (-1 = current input) */

    /* Simple line buffer for display */
    char        lines[CON_MAXLINES][CON_LINEWIDTH + 1];
    int         line_head;          /* circular write index */
    int         line_count;         /* total lines stored */
    int         scroll_offset;      /* how many lines scrolled back */
} console_t;

extern console_t con;

/* ==========================================================================
   Console API
   ========================================================================== */

void    Con_Init(void);
void    Con_Shutdown(void);

/* Print text to console */
void    Con_Print(const char *txt);

/* Draw console (called from renderer) */
void    Con_DrawConsole(float frac);
void    Con_DrawNotify(void);

/* Input handling */
void    Con_KeyEvent(int key);
void    Con_CharEvent(int ch);

/* Console toggle */
void    Con_ToggleConsole(void);
qboolean Con_IsVisible(void);

/* Clear */
void    Con_ClearNotify(void);
void    Con_Clear(void);

#endif /* CONSOLE_H */
