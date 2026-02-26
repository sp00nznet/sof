/*
 * cl_input.c - Client input command processing
 *
 * Handles +forward/-forward style button commands and builds usercmd_t
 * from the current input state each frame.
 *
 * In the original SoF, this was part of cl_input.c in the engine.
 * The button state is sampled each frame to produce a usercmd_t
 * that drives player movement through Pmove.
 *
 * Based on Q2 cl_input.c (id Software GPL)
 */

#include "../common/qcommon.h"

/* ==========================================================================
   Button State
   ========================================================================== */

typedef struct {
    int     down[2];        /* key nums holding it down */
    int     state;          /* 1 if down (accumulated) */
    qboolean active;        /* currently pressed */
} kbutton_t;

static kbutton_t   in_forward;
static kbutton_t   in_back;
static kbutton_t   in_moveleft;
static kbutton_t   in_moveright;
static kbutton_t   in_moveup;
static kbutton_t   in_movedown;
static kbutton_t   in_attack;
static kbutton_t   in_attack2;
static kbutton_t   in_use;
static kbutton_t   in_speed;        /* walk/run toggle */
static kbutton_t   in_lookup;
static kbutton_t   in_lookdown;
static kbutton_t   in_left;
static kbutton_t   in_right;

/* ==========================================================================
   Button Press/Release
   ========================================================================== */

static void KeyDown(kbutton_t *b)
{
    int k;
    const char *c = Cmd_Argv(1);

    if (c[0])
        k = atoi(c);
    else
        k = -1;    /* typed manually at console */

    if (k == b->down[0] || k == b->down[1])
        return;     /* repeating key */

    if (!b->down[0])
        b->down[0] = k;
    else if (!b->down[1])
        b->down[1] = k;

    b->active = qtrue;
    b->state |= 1;
}

static void KeyUp(kbutton_t *b)
{
    int k;
    const char *c = Cmd_Argv(1);

    if (c[0])
        k = atoi(c);
    else {
        /* typed manually, clear all */
        b->down[0] = b->down[1] = 0;
        b->active = qfalse;
        b->state = 0;
        return;
    }

    if (b->down[0] == k)
        b->down[0] = 0;
    else if (b->down[1] == k)
        b->down[1] = 0;

    if (b->down[0] || b->down[1])
        return;     /* some other key is still holding it down */

    b->active = qfalse;
    b->state &= ~1;
}

/*
 * Return the movement value for a button.
 * Returns 1.0 if fully pressed, 0.0 if not.
 */
static float CL_KeyState(kbutton_t *key)
{
    return key->active ? 1.0f : 0.0f;
}

/* ==========================================================================
   Command Handlers
   ========================================================================== */

static void IN_ForwardDown(void)    { KeyDown(&in_forward); }
static void IN_ForwardUp(void)      { KeyUp(&in_forward); }
static void IN_BackDown(void)       { KeyDown(&in_back); }
static void IN_BackUp(void)         { KeyUp(&in_back); }
static void IN_MoveLeftDown(void)   { KeyDown(&in_moveleft); }
static void IN_MoveLeftUp(void)     { KeyUp(&in_moveleft); }
static void IN_MoveRightDown(void)  { KeyDown(&in_moveright); }
static void IN_MoveRightUp(void)    { KeyUp(&in_moveright); }
static void IN_MoveUpDown(void)     { KeyDown(&in_moveup); }
static void IN_MoveUpUp(void)       { KeyUp(&in_moveup); }
static void IN_MoveDownDown(void)   { KeyDown(&in_movedown); }
static void IN_MoveDownUp(void)     { KeyUp(&in_movedown); }
static void IN_AttackDown(void)     { KeyDown(&in_attack); }
static void IN_AttackUp(void)       { KeyUp(&in_attack); }
static void IN_Attack2Down(void)    { KeyDown(&in_attack2); }
static void IN_Attack2Up(void)      { KeyUp(&in_attack2); }
static void IN_UseDown(void)        { KeyDown(&in_use); }
static void IN_UseUp(void)          { KeyUp(&in_use); }
static void IN_SpeedDown(void)      { KeyDown(&in_speed); }
static void IN_SpeedUp(void)        { KeyUp(&in_speed); }
static void IN_LookupDown(void)     { KeyDown(&in_lookup); }
static void IN_LookupUp(void)       { KeyUp(&in_lookup); }
static void IN_LookdownDown(void)   { KeyDown(&in_lookdown); }
static void IN_LookdownUp(void)     { KeyUp(&in_lookdown); }
static void IN_LeftDown(void)       { KeyDown(&in_left); }
static void IN_LeftUp(void)         { KeyUp(&in_left); }
static void IN_RightDown(void)      { KeyDown(&in_right); }
static void IN_RightUp(void)        { KeyUp(&in_right); }

/* ==========================================================================
   Build Usercmd
   ========================================================================== */

/*
 * CL_CreateCmd - Build a usercmd_t from current input state
 *
 * Called each client frame to sample input and produce a movement command.
 * The resulting usercmd is sent to the server for processing via ClientThink.
 */
void CL_CreateCmd(usercmd_t *cmd, int msec)
{
    float forward, side, up;

    memset(cmd, 0, sizeof(*cmd));
    cmd->msec = (byte)(msec > 250 ? 250 : msec);

    /* Movement axes */
    forward = CL_KeyState(&in_forward) - CL_KeyState(&in_back);
    side = CL_KeyState(&in_moveright) - CL_KeyState(&in_moveleft);
    up = CL_KeyState(&in_moveup) - CL_KeyState(&in_movedown);

    /* Scale to Q2 movement range (max +-400) */
    cmd->forwardmove = (short)(forward * 400);
    cmd->sidemove = (short)(side * 400);
    cmd->upmove = (short)(up * 400);

    /* Speed modifier (walk) */
    if (in_speed.active) {
        cmd->forwardmove = (short)(cmd->forwardmove * 0.5f);
        cmd->sidemove = (short)(cmd->sidemove * 0.5f);
    }

    /* Buttons */
    if (in_attack.active)
        cmd->buttons |= BUTTON_ATTACK;
    if (in_use.active)
        cmd->buttons |= BUTTON_USE;
    if (in_movedown.active)
        cmd->buttons |= BUTTON_CROUCH;
    if (in_attack.active || in_attack2.active ||
        in_forward.active || in_back.active ||
        in_moveleft.active || in_moveright.active ||
        in_moveup.active || in_movedown.active)
        cmd->buttons |= BUTTON_ANY;
}

/* ==========================================================================
   Initialization
   ========================================================================== */

void CL_InitInput(void)
{
    Cmd_AddCommand("+forward", IN_ForwardDown);
    Cmd_AddCommand("-forward", IN_ForwardUp);
    Cmd_AddCommand("+back", IN_BackDown);
    Cmd_AddCommand("-back", IN_BackUp);
    Cmd_AddCommand("+moveleft", IN_MoveLeftDown);
    Cmd_AddCommand("-moveleft", IN_MoveLeftUp);
    Cmd_AddCommand("+moveright", IN_MoveRightDown);
    Cmd_AddCommand("-moveright", IN_MoveRightUp);
    Cmd_AddCommand("+moveup", IN_MoveUpDown);
    Cmd_AddCommand("-moveup", IN_MoveUpUp);
    Cmd_AddCommand("+movedown", IN_MoveDownDown);
    Cmd_AddCommand("-movedown", IN_MoveDownUp);
    Cmd_AddCommand("+attack", IN_AttackDown);
    Cmd_AddCommand("-attack", IN_AttackUp);
    Cmd_AddCommand("+attack2", IN_Attack2Down);
    Cmd_AddCommand("-attack2", IN_Attack2Up);
    Cmd_AddCommand("+use", IN_UseDown);
    Cmd_AddCommand("-use", IN_UseUp);
    Cmd_AddCommand("+speed", IN_SpeedDown);
    Cmd_AddCommand("-speed", IN_SpeedUp);
    Cmd_AddCommand("+lookup", IN_LookupDown);
    Cmd_AddCommand("-lookup", IN_LookupUp);
    Cmd_AddCommand("+lookdown", IN_LookdownDown);
    Cmd_AddCommand("-lookdown", IN_LookdownUp);
    Cmd_AddCommand("+left", IN_LeftDown);
    Cmd_AddCommand("-left", IN_LeftUp);
    Cmd_AddCommand("+right", IN_RightDown);
    Cmd_AddCommand("-right", IN_RightUp);

    /* Additional aliases */
    Cmd_AddCommand("+scores", NULL);  /* TODO: scoreboard toggle */
    Cmd_AddCommand("-scores", NULL);
}
