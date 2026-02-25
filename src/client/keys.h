/*
 * keys.h - Quake II key definitions and input system
 *
 * Maps SDL2 scancodes to the original Quake II key numbering scheme.
 * SoF uses the exact same key system as Q2.
 */

#ifndef KEYS_H
#define KEYS_H

#include "../common/q_shared.h"

/* ==========================================================================
   Quake II Key Numbers
   These must match the original values since they're referenced in config
   files, bindings, and the game DLL.
   ========================================================================== */

#define K_TAB           9
#define K_ENTER         13
#define K_ESCAPE        27
#define K_SPACE         32

#define K_BACKSPACE     127
#define K_UPARROW       128
#define K_DOWNARROW     129
#define K_LEFTARROW     130
#define K_RIGHTARROW    131

/* Q2 function keys (note: not standard ASCII) */
#define K_ALT           132
#define K_CTRL          133
#define K_SHIFT         134
#define K_F1            135
#define K_F2            136
#define K_F3            137
#define K_F4            138
#define K_F5            139
#define K_F6            140
#define K_F7            141
#define K_F8            142
#define K_F9            143
#define K_F10           144
#define K_F11           145
#define K_F12           146
#define K_INS           147
#define K_DEL           148
#define K_PGDN          149
#define K_PGUP          150
#define K_HOME          151
#define K_END           152

#define K_KP_HOME       160
#define K_KP_UPARROW    161
#define K_KP_PGUP       162
#define K_KP_LEFTARROW  163
#define K_KP_5          164
#define K_KP_RIGHTARROW 165
#define K_KP_END        166
#define K_KP_DOWNARROW  167
#define K_KP_PGDN       168
#define K_KP_ENTER      169
#define K_KP_INS        170
#define K_KP_DEL        171
#define K_KP_SLASH      172
#define K_KP_MINUS      173
#define K_KP_PLUS       174

#define K_PAUSE         255

/* Mouse buttons */
#define K_MOUSE1        200
#define K_MOUSE2        201
#define K_MOUSE3        202
#define K_MOUSE4        203    /* wheel up in some configs */
#define K_MOUSE5        204    /* wheel down in some configs */

#define K_MWHEELDOWN    205
#define K_MWHEELUP      206

/* Joystick buttons */
#define K_JOY1          207
#define K_JOY2          208
#define K_JOY3          209
#define K_JOY4          210
#define K_JOY5          211
#define K_JOY6          212
#define K_JOY7          213
#define K_JOY8          214

/* Key destination (where keys go) */
typedef enum {
    key_game,
    key_console,
    key_message,
    key_menu
} keydest_t;

#define MAX_KEYS    256

/* ==========================================================================
   Key System API
   ========================================================================== */

void    Key_Init(void);
void    Key_Event(int key, qboolean down, unsigned time);
void    Key_ClearStates(void);
char    *Key_KeynumToString(int keynum);
int     Key_StringToKeynum(const char *str);
void    Key_SetBinding(int keynum, const char *binding);
char    *Key_GetBinding(int keynum);
void    Key_WriteBindings(FILE *f);

/* Global key state */
extern keydest_t    key_dest;
extern qboolean     key_down[MAX_KEYS];
extern int          key_repeats[MAX_KEYS];

#endif /* KEYS_H */
