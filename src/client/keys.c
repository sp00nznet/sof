/*
 * keys.c - Key binding and input processing
 *
 * Based on Quake II keys.c (id Software GPL).
 * Handles key bindings, key-to-command mapping, and input routing.
 */

#include "../common/qcommon.h"
#include "keys.h"

/* ==========================================================================
   State
   ========================================================================== */

keydest_t   key_dest = key_game;
qboolean    key_down[MAX_KEYS];
int         key_repeats[MAX_KEYS];

static char *keybindings[MAX_KEYS];
static int  key_waiting;
static int  anykeydown;

/* Key name table for config file I/O */
typedef struct {
    const char  *name;
    int         keynum;
} keyname_t;

static keyname_t keynames[] = {
    {"TAB", K_TAB},
    {"ENTER", K_ENTER},
    {"ESCAPE", K_ESCAPE},
    {"SPACE", K_SPACE},
    {"BACKSPACE", K_BACKSPACE},
    {"UPARROW", K_UPARROW},
    {"DOWNARROW", K_DOWNARROW},
    {"LEFTARROW", K_LEFTARROW},
    {"RIGHTARROW", K_RIGHTARROW},
    {"ALT", K_ALT},
    {"CTRL", K_CTRL},
    {"SHIFT", K_SHIFT},
    {"F1", K_F1}, {"F2", K_F2}, {"F3", K_F3}, {"F4", K_F4},
    {"F5", K_F5}, {"F6", K_F6}, {"F7", K_F7}, {"F8", K_F8},
    {"F9", K_F9}, {"F10", K_F10}, {"F11", K_F11}, {"F12", K_F12},
    {"INS", K_INS},
    {"DEL", K_DEL},
    {"PGDN", K_PGDN},
    {"PGUP", K_PGUP},
    {"HOME", K_HOME},
    {"END", K_END},
    {"MOUSE1", K_MOUSE1},
    {"MOUSE2", K_MOUSE2},
    {"MOUSE3", K_MOUSE3},
    {"MOUSE4", K_MOUSE4},
    {"MOUSE5", K_MOUSE5},
    {"MWHEELUP", K_MWHEELUP},
    {"MWHEELDOWN", K_MWHEELDOWN},
    {"KP_HOME", K_KP_HOME},
    {"KP_UPARROW", K_KP_UPARROW},
    {"KP_PGUP", K_KP_PGUP},
    {"KP_LEFTARROW", K_KP_LEFTARROW},
    {"KP_5", K_KP_5},
    {"KP_RIGHTARROW", K_KP_RIGHTARROW},
    {"KP_END", K_KP_END},
    {"KP_DOWNARROW", K_KP_DOWNARROW},
    {"KP_PGDN", K_KP_PGDN},
    {"KP_ENTER", K_KP_ENTER},
    {"KP_INS", K_KP_INS},
    {"KP_DEL", K_KP_DEL},
    {"KP_SLASH", K_KP_SLASH},
    {"KP_MINUS", K_KP_MINUS},
    {"KP_PLUS", K_KP_PLUS},
    {"PAUSE", K_PAUSE},
    {"JOY1", K_JOY1}, {"JOY2", K_JOY2}, {"JOY3", K_JOY3}, {"JOY4", K_JOY4},
    {"SEMICOLON", ';'},     /* because a raw ; is a command separator */
    {NULL, 0}
};

/* ==========================================================================
   Key Name Lookup
   ========================================================================== */

int Key_StringToKeynum(const char *str)
{
    keyname_t *kn;

    if (!str || !str[0])
        return -1;

    /* Single character? Use its ASCII value */
    if (!str[1])
        return (int)(unsigned char)str[0];

    /* Look up by name */
    for (kn = keynames; kn->name; kn++) {
        if (!Q_stricmp(str, kn->name))
            return kn->keynum;
    }

    return -1;
}

char *Key_KeynumToString(int keynum)
{
    keyname_t   *kn;
    static char tinystr[2];

    if (keynum < 0 || keynum >= MAX_KEYS)
        return "<UNKNOWN>";

    /* Check named keys */
    for (kn = keynames; kn->name; kn++) {
        if (keynum == kn->keynum)
            return (char *)kn->name;
    }

    /* Printable ASCII */
    if (keynum > 32 && keynum < 127) {
        tinystr[0] = (char)keynum;
        tinystr[1] = 0;
        return tinystr;
    }

    return "<UNKNOWN>";
}

/* ==========================================================================
   Bindings
   ========================================================================== */

void Key_SetBinding(int keynum, const char *binding)
{
    if (keynum < 0 || keynum >= MAX_KEYS)
        return;

    /* Free old binding */
    if (keybindings[keynum]) {
        Z_Free(keybindings[keynum]);
        keybindings[keynum] = NULL;
    }

    if (binding && binding[0]) {
        int len = (int)strlen(binding);
        keybindings[keynum] = (char *)Z_Malloc(len + 1);
        strcpy(keybindings[keynum], binding);
    }
}

char *Key_GetBinding(int keynum)
{
    if (keynum < 0 || keynum >= MAX_KEYS)
        return "";
    if (!keybindings[keynum])
        return "";
    return keybindings[keynum];
}

void Key_WriteBindings(FILE *f)
{
    int i;
    for (i = 0; i < MAX_KEYS; i++) {
        if (keybindings[i] && keybindings[i][0]) {
            fprintf(f, "bind %s \"%s\"\n",
                Key_KeynumToString(i), keybindings[i]);
        }
    }
}

/* ==========================================================================
   Key Event Processing
   ========================================================================== */

void Key_Event(int key, qboolean down, unsigned time)
{
    char    *kb;
    char    cmd[1024];

    (void)time;

    if (key < 0 || key >= MAX_KEYS)
        return;

    /* Track key state */
    if (down) {
        key_repeats[key]++;
        if (key_repeats[key] > 1 && key_dest == key_game)
            return;     /* ignore repeats in game mode */
    } else {
        key_repeats[key] = 0;
    }

    key_down[key] = down;

    if (down)
        anykeydown++;
    else if (anykeydown > 0)
        anykeydown--;

    /* Escape is special — always handled */
    if (key == K_ESCAPE && down) {
        switch (key_dest) {
        case key_message:
            key_dest = key_game;
            break;
        case key_menu:
            /* TODO: M_Keydown(key) */
            break;
        case key_console:
            /* TODO: Toggle console */
            break;
        case key_game:
            /* TODO: M_Menu_Main_f() — open main menu */
            break;
        }
        return;
    }

    /* Tilde/backtick toggles console */
    if (key == '`' && down) {
        if (key_dest == key_console) {
            key_dest = key_game;
        } else {
            key_dest = key_console;
        }
        return;
    }

    /* Route to appropriate handler */
    switch (key_dest) {
    case key_console:
        /* TODO: Con_KeyEvent(key) for console input */
        break;

    case key_menu:
        /* TODO: M_Keydown(key) for menu navigation */
        break;

    case key_message:
        /* TODO: Message_Key(key) for chat input */
        break;

    case key_game:
        /* Execute the bound command */
        kb = keybindings[key];
        if (kb && kb[0]) {
            if (kb[0] == '+') {
                /* Button command (e.g., +forward, +attack) */
                Com_sprintf(cmd, sizeof(cmd), "%s %d %u\n",
                    down ? kb : va("-%s", kb + 1), key, 0);
                Cbuf_AddText(cmd);
            } else if (down) {
                /* Regular command — only on keydown */
                Cbuf_AddText(kb);
                Cbuf_AddText("\n");
            }
        }
        break;
    }
}

void Key_ClearStates(void)
{
    int i;
    anykeydown = 0;
    for (i = 0; i < MAX_KEYS; i++) {
        if (key_down[i])
            Key_Event(i, qfalse, 0);
        key_down[i] = qfalse;
        key_repeats[i] = 0;
    }
}

/* ==========================================================================
   Console Commands
   ========================================================================== */

static void Key_Bind_f(void)
{
    int     key;

    if (Cmd_Argc() < 2) {
        Com_Printf("bind <key> [command] : attach a command to a key\n");
        return;
    }

    key = Key_StringToKeynum(Cmd_Argv(1));
    if (key == -1) {
        Com_Printf("\"%s\" isn't a valid key\n", Cmd_Argv(1));
        return;
    }

    if (Cmd_Argc() == 2) {
        if (keybindings[key])
            Com_Printf("\"%s\" = \"%s\"\n", Cmd_Argv(1), keybindings[key]);
        else
            Com_Printf("\"%s\" is not bound\n", Cmd_Argv(1));
        return;
    }

    Key_SetBinding(key, Cmd_Args());
}

static void Key_Unbind_f(void)
{
    int key;

    if (Cmd_Argc() != 2) {
        Com_Printf("unbind <key> : remove commands from a key\n");
        return;
    }

    key = Key_StringToKeynum(Cmd_Argv(1));
    if (key == -1) {
        Com_Printf("\"%s\" isn't a valid key\n", Cmd_Argv(1));
        return;
    }

    Key_SetBinding(key, "");
}

static void Key_Bindlist_f(void)
{
    int i;
    for (i = 0; i < MAX_KEYS; i++) {
        if (keybindings[i] && keybindings[i][0])
            Com_Printf("%s \"%s\"\n", Key_KeynumToString(i), keybindings[i]);
    }
}

/* ==========================================================================
   Key_Init
   ========================================================================== */

void Key_Init(void)
{
    int i;

    for (i = 0; i < MAX_KEYS; i++) {
        key_down[i] = qfalse;
        key_repeats[i] = 0;
        keybindings[i] = NULL;
    }

    Cmd_AddCommand("bind", Key_Bind_f);
    Cmd_AddCommand("unbind", Key_Unbind_f);
    Cmd_AddCommand("bindlist", Key_Bindlist_f);

    /* Default FPS bindings (matching SoF defaults) */
    Key_SetBinding('w', "+forward");
    Key_SetBinding('s', "+back");
    Key_SetBinding('a', "+moveleft");
    Key_SetBinding('d', "+moveright");
    Key_SetBinding(K_SPACE, "+moveup");
    Key_SetBinding(K_MOUSE1, "+attack");
    Key_SetBinding(K_MOUSE2, "+attack2");
    Key_SetBinding(K_ESCAPE, "togglemenu");
    Key_SetBinding(K_F1, "help");
    Key_SetBinding(K_F5, "savegame quick");
    Key_SetBinding(K_F9, "loadgame quick");
    Key_SetBinding('`', "toggleconsole");
    Key_SetBinding(K_TAB, "+scores");
    Key_SetBinding(K_MWHEELUP, "cmd weapnext");
    Key_SetBinding(K_MWHEELDOWN, "cmd weapprev");
    Key_SetBinding('e', "+use");
    Key_SetBinding('r', "cmd reload");
}
