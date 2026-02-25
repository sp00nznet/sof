/*
 * cmd.c - Console command buffer and command registration
 *
 * Based on Quake II cmd.c (id Software GPL).
 * Handles command buffer management, tokenization, and command dispatch.
 *
 * SoF exports: Cmd_Argc, Cmd_Argv, Cmd_AddCommand, Cmd_RemoveCommand
 * Original addresses: Cmd_Argc=0x1A2F0, Cmd_Argv=0x1A300,
 *                     Cmd_AddCommand=0x1A780, Cmd_RemoveCommand=0x1A830
 */

#include "../common/qcommon.h"

/* ==========================================================================
   Command Buffer
   ========================================================================== */

#define CBUF_SIZE   8192

typedef struct {
    char    text[CBUF_SIZE];
    int     cursize;
} cmdbuf_t;

static cmdbuf_t cmd_text;
static char     cmd_text_defer[CBUF_SIZE];
static int      cmd_text_defer_size;

void Cbuf_Init(void)
{
    memset(&cmd_text, 0, sizeof(cmd_text));
    cmd_text_defer_size = 0;
}

void Cbuf_AddText(const char *text)
{
    int l = (int)strlen(text);

    if (cmd_text.cursize + l >= CBUF_SIZE) {
        Com_Printf("Cbuf_AddText: overflow\n");
        return;
    }

    memcpy(&cmd_text.text[cmd_text.cursize], text, l);
    cmd_text.cursize += l;
}

void Cbuf_InsertText(const char *text)
{
    int l = (int)strlen(text);
    char temp[CBUF_SIZE];
    int templen;

    /* Copy off any commands still remaining in the exec buffer */
    templen = cmd_text.cursize;
    if (templen) {
        memcpy(temp, cmd_text.text, templen);
        cmd_text.cursize = 0;
    }

    /* Add the entire text of the file */
    Cbuf_AddText(text);

    /* Add the copied off data */
    if (templen) {
        if (cmd_text.cursize + templen >= CBUF_SIZE) {
            Com_Printf("Cbuf_InsertText: overflow\n");
            return;
        }
        memcpy(&cmd_text.text[cmd_text.cursize], temp, templen);
        cmd_text.cursize += templen;
    }
}

void Cbuf_CopyToDefer(void)
{
    memcpy(cmd_text_defer, cmd_text.text, cmd_text.cursize);
    cmd_text_defer_size = cmd_text.cursize;
    cmd_text.cursize = 0;
}

void Cbuf_ExecuteDefer(void)
{
    if (cmd_text_defer_size) {
        if (cmd_text.cursize + cmd_text_defer_size >= CBUF_SIZE) {
            Com_Printf("Cbuf_ExecuteDefer: overflow\n");
        } else {
            memcpy(&cmd_text.text[cmd_text.cursize], cmd_text_defer, cmd_text_defer_size);
            cmd_text.cursize += cmd_text_defer_size;
        }
        cmd_text_defer_size = 0;
    }
}

void Cbuf_Execute(void)
{
    int     i;
    char    *text;
    char    line[1024];
    int     quotes;

    while (cmd_text.cursize) {
        /* Find a \n or ; line break */
        text = cmd_text.text;
        quotes = 0;

        for (i = 0; i < cmd_text.cursize; i++) {
            if (text[i] == '"')
                quotes++;
            if (!(quotes & 1) && text[i] == ';')
                break;  /* don't break if inside a quoted string */
            if (text[i] == '\n')
                break;
        }

        if (i > (int)sizeof(line) - 1)
            i = sizeof(line) - 1;

        memcpy(line, text, i);
        line[i] = 0;

        /* Delete the text from the command buffer and move remaining
         * commands down (this is necessary because commands (exec, alias)
         * can insert data at the beginning of the text buffer) */
        if (i == cmd_text.cursize) {
            cmd_text.cursize = 0;
        } else {
            i++;    /* skip the \n or ; */
            cmd_text.cursize -= i;
            memmove(text, text + i, cmd_text.cursize);
        }

        /* Execute the command line */
        Cmd_ExecuteString(line);
    }
}

/* ==========================================================================
   Command Tokenization
   ========================================================================== */

static int      cmd_argc;
static char     *cmd_argv[MAX_STRING_TOKENS];
static char     cmd_args[MAX_STRING_CHARS];
static char     cmd_tokenized[MAX_STRING_CHARS + MAX_STRING_TOKENS]; /* space for terminators */

int Cmd_Argc(void)
{
    return cmd_argc;
}

char *Cmd_Argv(int arg)
{
    if (arg < 0 || arg >= cmd_argc)
        return "";
    return cmd_argv[arg];
}

char *Cmd_Args(void)
{
    return cmd_args;
}

void Cmd_TokenizeString(char *text, qboolean macroExpand)
{
    int     i;
    const char *com_token;

    /* Clear the args from the last string */
    for (i = 0; i < cmd_argc; i++)
        cmd_argv[i] = NULL;

    cmd_argc = 0;
    cmd_args[0] = 0;

    if (!text)
        return;

    char *out = cmd_tokenized;
    int out_remaining = sizeof(cmd_tokenized);

    while (1) {
        /* Skip whitespace up to a \n */
        while (*text && *text <= ' ' && *text != '\n')
            text++;

        if (!*text || *text == '\n')
            break;

        /* Set cmd_args to everything after the first arg */
        if (cmd_argc == 1) {
            int l;
            /* Strip leading whitespace */
            while (*text && *text <= ' ' && *text != '\n')
                text++;

            l = (int)strlen(text);
            if (l >= MAX_STRING_CHARS)
                l = MAX_STRING_CHARS - 1;
            memcpy(cmd_args, text, l);
            cmd_args[l] = 0;
        }

        /* Handle quoted strings */
        if (*text == '"') {
            text++;
            cmd_argv[cmd_argc] = out;
            cmd_argc++;
            while (*text && *text != '"') {
                if (out_remaining > 1) {
                    *out++ = *text;
                    out_remaining--;
                }
                text++;
            }
            *out++ = 0;
            out_remaining--;
            if (*text == '"')
                text++;
        } else {
            /* Regular token */
            cmd_argv[cmd_argc] = out;
            cmd_argc++;
            while (*text > ' ') {
                if (out_remaining > 1) {
                    *out++ = *text;
                    out_remaining--;
                }
                text++;
            }
            *out++ = 0;
            out_remaining--;
        }

        if (cmd_argc >= MAX_STRING_TOKENS)
            return;
    }
}

/* ==========================================================================
   Command Registration
   ========================================================================== */

typedef struct cmd_function_s {
    struct cmd_function_s   *next;
    char                    *name;
    xcommand_t              function;
} cmd_function_t;

static cmd_function_t *cmd_functions;

void Cmd_AddCommand(const char *cmd_name, xcommand_t function)
{
    cmd_function_t *cmd;

    /* Fail if the command already exists */
    for (cmd = cmd_functions; cmd; cmd = cmd->next) {
        if (!strcmp(cmd_name, cmd->name)) {
            Com_Printf("Cmd_AddCommand: %s already defined\n", cmd_name);
            return;
        }
    }

    cmd = (cmd_function_t *)Z_Malloc(sizeof(cmd_function_t));
    cmd->name = (char *)Z_Malloc((int)strlen(cmd_name) + 1);
    strcpy(cmd->name, cmd_name);
    cmd->function = function;
    cmd->next = cmd_functions;
    cmd_functions = cmd;
}

void Cmd_RemoveCommand(const char *cmd_name)
{
    cmd_function_t *cmd, **back;

    back = &cmd_functions;
    while (1) {
        cmd = *back;
        if (!cmd) {
            Com_Printf("Cmd_RemoveCommand: %s not added\n", cmd_name);
            return;
        }
        if (!strcmp(cmd_name, cmd->name)) {
            *back = cmd->next;
            Z_Free(cmd->name);
            Z_Free(cmd);
            return;
        }
        back = &cmd->next;
    }
}

qboolean Cmd_Exists(const char *cmd_name)
{
    cmd_function_t *cmd;

    for (cmd = cmd_functions; cmd; cmd = cmd->next) {
        if (!strcmp(cmd_name, cmd->name))
            return qtrue;
    }
    return qfalse;
}

/* ==========================================================================
   Command Execution
   ========================================================================== */

void Cmd_ExecuteString(char *text)
{
    cmd_function_t  *cmd;
    cvar_t          *v;

    Cmd_TokenizeString(text, qtrue);

    if (!Cmd_Argc())
        return;     /* no tokens */

    /* Check registered commands */
    for (cmd = cmd_functions; cmd; cmd = cmd->next) {
        if (!Q_stricmp(cmd_argv[0], cmd->name)) {
            if (cmd->function) {
                cmd->function();
            } else {
                /* Forward to server command */
                Cmd_ExecuteString(va("cmd %s", text));
            }
            return;
        }
    }

    /* Check cvars */
    if (Cvar_FindVar(cmd_argv[0])) {
        if (Cmd_Argc() == 1) {
            v = Cvar_FindVar(cmd_argv[0]);
            Com_Printf("\"%s\" is \"%s\"\n", v->name, v->string);
        } else {
            Cvar_Set(cmd_argv[0], cmd_argv[1]);
        }
        return;
    }

    Com_Printf("Unknown command \"%s\"\n", cmd_argv[0]);
}

/* ==========================================================================
   Cmd_Init
   ========================================================================== */

static void Cmd_List_f(void)
{
    cmd_function_t *cmd;
    int count = 0;

    for (cmd = cmd_functions; cmd; cmd = cmd->next) {
        Com_Printf("%s\n", cmd->name);
        count++;
    }
    Com_Printf("%d commands\n", count);
}

static void Cmd_Exec_f(void)
{
    char    *f;
    int     len;

    if (Cmd_Argc() != 2) {
        Com_Printf("exec <filename> : execute a script file\n");
        return;
    }

    len = FS_LoadFile(Cmd_Argv(1), (void **)&f);
    if (!f) {
        Com_Printf("couldn't exec %s\n", Cmd_Argv(1));
        return;
    }
    Com_Printf("execing %s\n", Cmd_Argv(1));

    Cbuf_InsertText(f);
    FS_FreeFile(f);
}

static void Cmd_Echo_f(void)
{
    int i;
    for (i = 1; i < Cmd_Argc(); i++)
        Com_Printf("%s ", Cmd_Argv(i));
    Com_Printf("\n");
}

void Cmd_Init(void)
{
    Cmd_AddCommand("cmdlist", Cmd_List_f);
    Cmd_AddCommand("exec", Cmd_Exec_f);
    Cmd_AddCommand("echo", Cmd_Echo_f);
}
