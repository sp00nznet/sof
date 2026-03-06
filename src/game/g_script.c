/*
 * g_script.c - Objective Script (.os) bytecode interpreter
 *
 * SoF uses compiled bytecode scripts (.os files in ds/ directory) for:
 *   - Level events and cinematics
 *   - NPC behavior sequences
 *   - Door/trigger scripting
 *   - Camera control and cutscenes
 *   - Precaching sounds, models, and ROFF animations
 *
 * Scripts are referenced by:
 *   - worldspawn "script" key (precache script, runs at map load)
 *   - script_runner entities (triggered by targetname activation)
 *
 * .os binary format:
 *   Header:   4 bytes (version = 3)
 *   Symbols:  Variable-length symbol table (typed variables)
 *   Bytecode: Stack-based instruction stream
 *
 * Original interpreter: ~0x500B0000 in gamex86.dll
 */

#include "g_local.h"
#include <math.h>
#include <string.h>

extern game_export_t globals;

/* ==========================================================================
   Constants
   ========================================================================== */

#define OS_VERSION          3
#define MAX_SCRIPTS         64      /* max concurrent running scripts */
#define MAX_STACK           64      /* VM stack depth */
#define MAX_SYMBOLS         64      /* max symbols per script */
#define MAX_SCRIPT_SIZE     16384   /* max bytecode size */

/* Symbol types (in .os symbol table) */
#define SYM_INT     0
#define SYM_FLOAT   1
#define SYM_VECTOR  2
#define SYM_ENTITY  3

/* Push argument sub-types (after opcode 0x11) */
#define PUSH_INT        0x00
#define PUSH_FLOAT      0x01
#define PUSH_VECTOR     0x02
#define PUSH_ENTITY_REF 0x03
#define PUSH_STRING     0x04
#define PUSH_VARREF     0x05
#define PUSH_VARREF2    0x06
#define PUSH_ENTFIND    0x07

/* ==========================================================================
   Opcodes
   ========================================================================== */

#define OP_NOP          0x00
#define OP_IF_FALSE     0x01
#define OP_EQUAL        0x02
#define OP_NOT_EQUAL    0x03
#define OP_LESS         0x04
#define OP_GREATER      0x05
#define OP_LESS_EQ      0x06
#define OP_GREATER_EQ   0x07
#define OP_AND          0x08
#define OP_OR           0x09
#define OP_ADD          0x0A
#define OP_SUB          0x0B
#define OP_MUL          0x0C
#define OP_DIV          0x0D
#define OP_NEG          0x0E
#define OP_NOT          0x0F
#define OP_JUMP         0x10
#define OP_PUSH         0x11
#define OP_CALL         0x12
#define OP_RETURN       0x13
#define OP_END          0x14
#define OP_NOP2         0x15
#define OP_SET          0x16
#define OP_GET          0x17
#define OP_LOOP         0x18
#define OP_IF           0x19
#define OP_ELSE         0x1A
#define OP_ENTREF       0x1B
#define OP_REMOVE       0x1C
#define OP_PLAYSOUND    0x1D
#define OP_FUNC_CALL    0x1E
#define OP_SPAWN        0x1F
#define OP_KILL         0x20
#define OP_WAIT         0x21
#define OP_PRINT        0x22
#define OP_PRECACHE_SND 0x23
#define OP_ACTIVATE     0x24
#define OP_DEACTIVATE   0x25
#define OP_USE          0x26
#define OP_SLEEP        0x27
#define OP_ANIM         0x28
#define OP_PLAY_ROFF    0x29
#define OP_CAMERA       0x2A
#define OP_SET_CVAR     0x2B
#define OP_MOVE_TO      0x2C
#define OP_FACE         0x2D
#define OP_STOP_ROFF    0x2E
#define OP_PRECACHE_ROF 0x2F
#define OP_PRECACHE_MDL 0x30
#define OP_TOUCH        0x31

/* ==========================================================================
   Data Types
   ========================================================================== */

typedef union {
    int     i;
    float   f;
    vec3_t  v;
    char    *s;
    edict_t *ent;
} script_val_t;

typedef struct {
    char        name[64];
    int         type;       /* SYM_INT, SYM_FLOAT, etc. */
    int         id;
    script_val_t val;
} script_symbol_t;

typedef struct {
    int             type;   /* 0=int, 1=float, 2=vec, 3=string, 4=entity */
    script_val_t    val;
    char            str[128]; /* string storage */
} stack_entry_t;

/* A running script instance */
typedef struct {
    qboolean        active;
    char            name[64];       /* script path (e.g., "nyc1/intro") */
    edict_t         *owner;         /* entity that started this script */

    /* Loaded bytecode */
    byte            *bytecode;
    int             bytecode_len;
    int             pc;             /* program counter */

    /* Symbol table */
    script_symbol_t symbols[MAX_SYMBOLS];
    int             num_symbols;

    /* Execution stack */
    stack_entry_t   stack[MAX_STACK];
    int             sp;             /* stack pointer */

    /* Wait state */
    float           wait_until;     /* level.time when script resumes */
    edict_t         *current_ent;   /* entity being operated on */
} script_instance_t;

static script_instance_t scripts[MAX_SCRIPTS];
static int num_active_scripts = 0;

/* ==========================================================================
   Symbol Table Parser
   ========================================================================== */

static int OS_ParseSymbolTable(byte *data, int len, script_symbol_t *syms, int max_syms)
{
    int pos = 4;    /* skip version */
    int count = 0;

    while (pos < len && count < max_syms) {
        byte flag = data[pos];
        if (flag != 0x06 && flag != 0x02)
            break;  /* end of symbol table */

        pos++;

        /* Read null-terminated name */
        {
            int start = pos;
            while (pos < len && data[pos] != 0)
                pos++;
            if (pos >= len) break;

            {
                int namelen = pos - start;
                if (namelen >= (int)sizeof(syms[count].name))
                    namelen = (int)sizeof(syms[count].name) - 1;
                memcpy(syms[count].name, &data[start], namelen);
                syms[count].name[namelen] = 0;
            }
        }
        pos++;  /* skip null */

        /* Type byte */
        if (pos >= len) break;
        syms[count].type = data[pos];
        pos++;

        /* ID (4 bytes LE) */
        if (pos + 4 > len) break;
        syms[count].id = *(int *)(data + pos);
        pos += 4;

        /* Initialize value */
        memset(&syms[count].val, 0, sizeof(script_val_t));
        count++;
    }

    return count;
}

/* ==========================================================================
   Stack Operations
   ========================================================================== */

static void OS_Push(script_instance_t *sc, stack_entry_t *entry)
{
    if (sc->sp >= MAX_STACK) return;
    sc->stack[sc->sp++] = *entry;
}

static stack_entry_t *OS_Pop(script_instance_t *sc)
{
    if (sc->sp <= 0) {
        static stack_entry_t empty = {0};
        return &empty;
    }
    return &sc->stack[--sc->sp];
}

static void OS_PushInt(script_instance_t *sc, int val)
{
    stack_entry_t e = {0};
    e.type = 0;
    e.val.i = val;
    OS_Push(sc, &e);
}

static void OS_PushFloat(script_instance_t *sc, float val)
{
    stack_entry_t e = {0};
    e.type = 1;
    e.val.f = val;
    OS_Push(sc, &e);
}

static void OS_PushString(script_instance_t *sc, const char *s)
{
    stack_entry_t e = {0};
    e.type = 3;
    Q_strncpyz(e.str, s, sizeof(e.str));
    e.val.s = e.str;
    OS_Push(sc, &e);
}

static void OS_PushEntity(script_instance_t *sc, edict_t *ent)
{
    stack_entry_t e = {0};
    e.type = 4;
    e.val.ent = ent;
    OS_Push(sc, &e);
}

static float OS_StackFloat(stack_entry_t *e)
{
    if (e->type == 1) return e->val.f;
    if (e->type == 0) return (float)e->val.i;
    return 0.0f;
}

static int OS_StackInt(stack_entry_t *e)
{
    if (e->type == 0) return e->val.i;
    if (e->type == 1) return (int)e->val.f;
    return 0;
}

/* ==========================================================================
   Entity Lookup
   ========================================================================== */

static edict_t *OS_FindEntity(const char *targetname)
{
    int i;
    if (!targetname || !targetname[0])
        return NULL;

    for (i = 0; i < globals.num_edicts; i++) {
        edict_t *e = &globals.edicts[i];
        if (!e->inuse) continue;
        if (e->targetname && Q_stricmp(e->targetname, targetname) == 0)
            return e;
    }
    return NULL;
}

/* ==========================================================================
   Bytecode Execution
   ========================================================================== */

/*
 * Execute bytecode for one time-slice.
 * Returns: 0 = finished, 1 = waiting, -1 = error
 */
static int OS_Execute(script_instance_t *sc, float current_time)
{
    byte *bc = sc->bytecode;
    int len = sc->bytecode_len;
    int ops_executed = 0;
    int max_ops = 10000;    /* prevent infinite loops per frame */

    /* Check wait state */
    if (sc->wait_until > 0 && current_time < sc->wait_until)
        return 1;   /* still waiting */
    sc->wait_until = 0;

    while (sc->pc < len && ops_executed < max_ops) {
        byte op = bc[sc->pc++];
        ops_executed++;

        switch (op) {
        case OP_NOP:
        case OP_NOP2:
            break;

        case OP_END:
        case OP_RETURN:
            return 0;   /* script finished */

        case OP_PUSH: {
            if (sc->pc >= len) return -1;
            byte ptype = bc[sc->pc++];

            switch (ptype) {
            case PUSH_INT:
                if (sc->pc + 4 > len) return -1;
                OS_PushInt(sc, *(int *)(bc + sc->pc));
                sc->pc += 4;
                break;

            case PUSH_FLOAT:
                if (sc->pc + 4 > len) return -1;
                OS_PushFloat(sc, *(float *)(bc + sc->pc));
                sc->pc += 4;
                break;

            case PUSH_STRING: {
                int start = sc->pc;
                while (sc->pc < len && bc[sc->pc] != 0) sc->pc++;
                {
                    char buf[128];
                    int slen = sc->pc - start;
                    if (slen >= (int)sizeof(buf)) slen = (int)sizeof(buf) - 1;
                    memcpy(buf, &bc[start], slen);
                    buf[slen] = 0;
                    OS_PushString(sc, buf);
                }
                if (sc->pc < len) sc->pc++;  /* skip null */
                break;
            }

            case PUSH_VARREF:
            case PUSH_VARREF2: {
                if (sc->pc + 4 > len) return -1;
                {
                    int var_id = *(int *)(bc + sc->pc);
                    sc->pc += 4;
                    /* Look up symbol by ID */
                    {
                        int si;
                        qboolean found = qfalse;
                        for (si = 0; si < sc->num_symbols; si++) {
                            if (sc->symbols[si].id == var_id) {
                                switch (sc->symbols[si].type) {
                                case SYM_INT:
                                    OS_PushInt(sc, sc->symbols[si].val.i);
                                    break;
                                case SYM_FLOAT:
                                    OS_PushFloat(sc, sc->symbols[si].val.f);
                                    break;
                                case SYM_ENTITY:
                                    OS_PushEntity(sc, sc->symbols[si].val.ent);
                                    break;
                                default:
                                    OS_PushInt(sc, 0);
                                    break;
                                }
                                found = qtrue;
                                break;
                            }
                        }
                        if (!found)
                            OS_PushInt(sc, var_id);
                    }
                }
                break;
            }

            case PUSH_ENTFIND: {
                /* The previous string on stack is the entity targetname */
                /* Nothing to push here — the string is already on stack */
                break;
            }

            case PUSH_VECTOR:
                if (sc->pc + 12 > len) return -1;
                {
                    stack_entry_t e = {0};
                    e.type = 2;
                    e.val.v[0] = *(float *)(bc + sc->pc);
                    e.val.v[1] = *(float *)(bc + sc->pc + 4);
                    e.val.v[2] = *(float *)(bc + sc->pc + 8);
                    OS_Push(sc, &e);
                }
                sc->pc += 12;
                break;

            default:
                /* Unknown push type — skip */
                break;
            }
            break;
        }

        case 0x07: {
            /* Literal string (null-terminated) */
            int start = sc->pc;
            while (sc->pc < len && bc[sc->pc] != 0) sc->pc++;
            {
                char buf[128];
                int slen = sc->pc - start;
                if (slen >= (int)sizeof(buf)) slen = (int)sizeof(buf) - 1;
                memcpy(buf, &bc[start], slen);
                buf[slen] = 0;
                OS_PushString(sc, buf);
            }
            if (sc->pc < len) sc->pc++;
            break;
        }

        case OP_ENTREF: {
            /* Pop string from stack, find entity by targetname */
            stack_entry_t *name_entry = OS_Pop(sc);
            const char *tname = (name_entry->type == 3) ? name_entry->str : "";
            edict_t *ent = OS_FindEntity(tname);
            OS_PushEntity(sc, ent);
            sc->current_ent = ent;
            break;
        }

        case OP_SET: {
            /* Set entity property: pop value, pop property name / var ref */
            stack_entry_t *val = OS_Pop(sc);
            stack_entry_t *prop = OS_Pop(sc);

            if (sc->current_ent && prop->type == 0) {
                /* prop is a variable reference (symbol ID) */
                int var_id = prop->val.i;
                int si;
                for (si = 0; si < sc->num_symbols; si++) {
                    if (sc->symbols[si].id == var_id) {
                        edict_t *ent = sc->current_ent;
                        const char *pname = sc->symbols[si].name;

                        /* Apply known properties */
                        if (Q_stricmp(pname, "movetype") == 0) {
                            ent->movetype = OS_StackInt(val);
                        } else if (Q_stricmp(pname, "solid") == 0) {
                            ent->solid = OS_StackInt(val);
                        } else if (Q_stricmp(pname, "health") == 0) {
                            ent->health = OS_StackInt(val);
                        } else if (Q_stricmp(pname, "takedamage") == 0) {
                            ent->takedamage = OS_StackInt(val);
                        } else if (Q_stricmp(pname, "state") == 0) {
                            ent->s.frame = OS_StackInt(val);
                        } else if (Q_stricmp(pname, "count") == 0) {
                            ent->count = OS_StackInt(val);
                        } else if (Q_stricmp(pname, "wait") == 0) {
                            ent->wait = OS_StackFloat(val);
                        } else if (Q_stricmp(pname, "speed") == 0) {
                            ent->speed = OS_StackFloat(val);
                        } else if (Q_stricmp(pname, "yaw_speed") == 0) {
                            ent->yaw_speed = OS_StackFloat(val);
                        }
                        /* Store in symbol table too */
                        sc->symbols[si].val = val->val;
                        break;
                    }
                }
            }
            break;
        }

        case OP_WAIT: {
            /* Wait N milliseconds / frames */
            if (sc->pc + 4 > len) return -1;
            {
                int duration = *(int *)(bc + sc->pc);
                sc->pc += 4;
                sc->wait_until = current_time + (float)duration * 0.1f;
                return 1;   /* yield execution */
            }
        }

        case OP_SLEEP: {
            /* Sleep — pop duration from stack */
            stack_entry_t *dur = OS_Pop(sc);
            float secs = OS_StackFloat(dur);
            if (secs > 0)
                sc->wait_until = current_time + secs;
            return 1;
        }

        case OP_PRECACHE_SND: {
            /* Pop sound path, precache it */
            stack_entry_t *path = OS_Pop(sc);
            if (path->type == 3 && path->str[0]) {
                gi.soundindex(path->str);
            }
            break;
        }

        case OP_PRECACHE_ROF:
        case OP_PRECACHE_MDL:
        case OP_TOUCH: {
            /* Pop path, precache (currently stub) */
            OS_Pop(sc);
            break;
        }

        case OP_PLAYSOUND: {
            /* Play sound on current entity */
            stack_entry_t *snd = OS_Pop(sc);
            if (sc->current_ent && snd->type == 3 && snd->str[0]) {
                int idx = gi.soundindex(snd->str);
                if (idx > 0) {
                    gi.sound(sc->current_ent, CHAN_AUTO, idx, 1.0f, ATTN_NORM, 0);
                }
            }
            break;
        }

        case OP_PLAY_ROFF: {
            /* Play ROFF animation on entity (stub) */
            OS_Pop(sc);  /* path */
            break;
        }

        case OP_STOP_ROFF:
            break;

        case OP_ACTIVATE: {
            /* Activate entity (set SVF_NOCLIENT off, enable thinking) */
            if (sc->current_ent) {
                sc->current_ent->svflags &= ~SVF_NOCLIENT;
                sc->current_ent->solid = SOLID_BSP;
                gi.linkentity(sc->current_ent);
            }
            break;
        }

        case OP_DEACTIVATE: {
            if (sc->current_ent) {
                sc->current_ent->svflags |= SVF_NOCLIENT;
                sc->current_ent->solid = SOLID_NOT;
                gi.linkentity(sc->current_ent);
            }
            break;
        }

        case OP_USE: {
            /* Trigger entity's use callback */
            if (sc->current_ent && sc->current_ent->use) {
                sc->current_ent->use(sc->current_ent, sc->owner, sc->owner);
            }
            break;
        }

        case OP_KILL: {
            /* Remove/kill entity */
            if (sc->current_ent) {
                sc->current_ent->inuse = qfalse;
                gi.unlinkentity(sc->current_ent);
                sc->current_ent = NULL;
            }
            break;
        }

        case OP_SET_CVAR: {
            /* Set a cvar: pop value, pop name */
            stack_entry_t *val = OS_Pop(sc);
            stack_entry_t *name_e = OS_Pop(sc);
            if (name_e->type == 3 && name_e->str[0]) {
                char vbuf[64];
                if (val->type == 1)
                    Com_sprintf(vbuf, sizeof(vbuf), "%f", val->val.f);
                else if (val->type == 0)
                    Com_sprintf(vbuf, sizeof(vbuf), "%d", val->val.i);
                else
                    Q_strncpyz(vbuf, val->str, sizeof(vbuf));
                Cvar_Set(name_e->str, vbuf);
            }
            break;
        }

        case OP_PRINT: {
            stack_entry_t *msg = OS_Pop(sc);
            if (msg->type == 3)
                gi.dprintf("Script: %s\n", msg->str);
            break;
        }

        case OP_CAMERA:
            /* Camera control — stub */
            break;

        default:
            /* Unknown opcode — skip and continue */
            break;
        }
    }

    return (sc->pc >= len) ? 0 : 1;
}

/* ==========================================================================
   Public API
   ========================================================================== */

/*
 * G_ScriptLoad — Load and start executing a script
 */
void G_ScriptLoad(const char *scriptname, edict_t *owner)
{
    byte    *raw;
    int     len;
    char    path[MAX_QPATH];
    int     slot;
    script_instance_t *sc;
    int     sym_end;

    /* Find free slot */
    slot = -1;
    for (slot = 0; slot < MAX_SCRIPTS; slot++) {
        if (!scripts[slot].active)
            break;
    }
    if (slot >= MAX_SCRIPTS) {
        gi.dprintf("G_ScriptLoad: no free script slots for %s\n", scriptname);
        return;
    }

    /* Load .os file from PAK */
    Com_sprintf(path, sizeof(path), "ds/%s.os", scriptname);
    len = FS_LoadFile(path, (void **)&raw);
    if (!raw || len < 8) {
        Com_DPrintf("G_ScriptLoad: %s not found\n", path);
        return;
    }

    /* Verify version */
    if (*(int *)raw != OS_VERSION) {
        gi.dprintf("G_ScriptLoad: %s bad version %d\n", path, *(int *)raw);
        FS_FreeFile(raw);
        return;
    }

    sc = &scripts[slot];
    memset(sc, 0, sizeof(*sc));
    sc->active = qtrue;
    sc->owner = owner;
    Q_strncpyz(sc->name, scriptname, sizeof(sc->name));

    /* Parse symbol table */
    sc->num_symbols = OS_ParseSymbolTable(raw, len, sc->symbols, MAX_SYMBOLS);

    /* Find bytecode start (after symbol table) */
    {
        int pos = 4;
        while (pos < len && (raw[pos] == 0x06 || raw[pos] == 0x02)) {
            pos++;
            while (pos < len && raw[pos] != 0) pos++;
            pos++;  /* null */
            pos++;  /* type */
            pos += 4; /* id */
        }
        sym_end = pos;
    }

    /* Copy bytecode */
    sc->bytecode_len = len - sym_end;
    if (sc->bytecode_len > MAX_SCRIPT_SIZE)
        sc->bytecode_len = MAX_SCRIPT_SIZE;
    sc->bytecode = (byte *)gi.TagMalloc(sc->bytecode_len, Z_TAG_GAME);
    memcpy(sc->bytecode, raw + sym_end, sc->bytecode_len);
    sc->pc = 0;
    sc->sp = 0;

    FS_FreeFile(raw);

    num_active_scripts++;
    Com_DPrintf("Script loaded: %s (%d symbols, %d bytes bytecode)\n",
               scriptname, sc->num_symbols, sc->bytecode_len);
}

/*
 * G_ScriptRunFrame — Execute all active scripts for one frame
 */
void G_ScriptRunFrame(float level_time)
{
    int i;

    for (i = 0; i < MAX_SCRIPTS; i++) {
        script_instance_t *sc = &scripts[i];
        if (!sc->active) continue;

        {
            int result = OS_Execute(sc, level_time);
            if (result == 0 || result == -1) {
                /* Script finished or errored */
                if (sc->bytecode) {
                    gi.TagFree(sc->bytecode);
                    sc->bytecode = NULL;
                }
                sc->active = qfalse;
                num_active_scripts--;
                Com_DPrintf("Script %s: %s\n", sc->name,
                           result == 0 ? "completed" : "error");
            }
        }
    }
}

/*
 * G_ScriptInit — Initialize script system
 */
void G_ScriptInit(void)
{
    memset(scripts, 0, sizeof(scripts));
    num_active_scripts = 0;
}

/*
 * G_ScriptShutdown — Clean up all scripts
 */
void G_ScriptShutdown(void)
{
    int i;
    for (i = 0; i < MAX_SCRIPTS; i++) {
        if (scripts[i].active && scripts[i].bytecode) {
            gi.TagFree(scripts[i].bytecode);
        }
    }
    memset(scripts, 0, sizeof(scripts));
    num_active_scripts = 0;
}
