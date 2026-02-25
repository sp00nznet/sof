/*
 * cvar.c - Console variable system
 *
 * Based on Quake II cvar.c (id Software GPL).
 * Manages engine configuration through typed key-value variables.
 *
 * SoF exports: Cvar_Get, Cvar_Set, Cvar_SetValue
 * Original addresses: Cvar_Get=0x240B0, Cvar_Set=0x24830, Cvar_SetValue=0x24A40
 *
 * SoF registers 85+ cvars including SoF-specific ones:
 *   ghl_specular, ghl_mip, gore_*, team names, weapon cvars, etc.
 */

#include "../common/qcommon.h"

static cvar_t   *cvar_vars;
static qboolean cvar_allowCheats = qtrue;

/* ==========================================================================
   Cvar_FindVar
   ========================================================================== */

cvar_t *Cvar_FindVar(const char *var_name)
{
    cvar_t *var;

    for (var = cvar_vars; var; var = var->next) {
        if (!Q_stricmp(var_name, var->name))
            return var;
    }
    return NULL;
}

/* ==========================================================================
   Cvar_VariableValue / Cvar_VariableString
   ========================================================================== */

float Cvar_VariableValue(const char *var_name)
{
    cvar_t *var = Cvar_FindVar(var_name);
    if (!var)
        return 0;
    return (float)atof(var->string);
}

char *Cvar_VariableString(const char *var_name)
{
    cvar_t *var = Cvar_FindVar(var_name);
    if (!var)
        return "";
    return var->string;
}

/* ==========================================================================
   Cvar_Get — SoF export
   ========================================================================== */

cvar_t *Cvar_Get(const char *var_name, const char *var_value, int flags)
{
    cvar_t *var;

    if (flags & (CVAR_USERINFO | CVAR_SERVERINFO)) {
        /* Validate the info string */
        if (var_value && (strchr(var_value, '\\') || strchr(var_value, ';') || strchr(var_value, '"'))) {
            Com_Printf("invalid info cvar value\n");
            return NULL;
        }
    }

    var = Cvar_FindVar(var_name);
    if (var) {
        var->flags |= flags;
        return var;
    }

    if (!var_value)
        return NULL;

    var = (cvar_t *)Z_Malloc(sizeof(cvar_t));
    var->name = (char *)Z_Malloc((int)strlen(var_name) + 1);
    strcpy(var->name, var_name);

    var->string = (char *)Z_Malloc((int)strlen(var_value) + 1);
    strcpy(var->string, var_value);

    var->modified = 1;
    var->value = (float)atof(var->string);
    var->flags = flags;
    var->latched_string = NULL;

    /* Link it in */
    var->next = cvar_vars;
    cvar_vars = var;

    return var;
}

/* ==========================================================================
   Cvar_Set — SoF export
   ========================================================================== */

static cvar_t *Cvar_Set2(const char *var_name, const char *value, qboolean force)
{
    cvar_t *var;

    var = Cvar_FindVar(var_name);
    if (!var) {
        /* Create it */
        return Cvar_Get(var_name, value, 0);
    }

    if (var->flags & (CVAR_USERINFO | CVAR_SERVERINFO)) {
        if (value && (strchr(value, '\\') || strchr(value, ';') || strchr(value, '"'))) {
            Com_Printf("invalid info cvar value\n");
            return var;
        }
    }

    if (!force) {
        if (var->flags & CVAR_NOSET) {
            Com_Printf("%s is write protected.\n", var_name);
            return var;
        }

        if (var->flags & CVAR_LATCH) {
            if (var->latched_string) {
                if (!strcmp(value, var->latched_string))
                    return var;
                Z_Free(var->latched_string);
            } else {
                if (!strcmp(value, var->string))
                    return var;
            }

            var->latched_string = (char *)Z_Malloc((int)strlen(value) + 1);
            strcpy(var->latched_string, value);
            Com_Printf("%s will be changed for next game.\n", var_name);
            return var;
        }
    } else {
        if (var->latched_string) {
            Z_Free(var->latched_string);
            var->latched_string = NULL;
        }
    }

    if (!strcmp(value, var->string))
        return var;     /* not changed */

    var->modified = 1;

    Z_Free(var->string);
    var->string = (char *)Z_Malloc((int)strlen(value) + 1);
    strcpy(var->string, value);
    var->value = (float)atof(var->string);

    return var;
}

cvar_t *Cvar_Set(const char *var_name, const char *value)
{
    return Cvar_Set2(var_name, value, qfalse);
}

cvar_t *Cvar_ForceSet(const char *var_name, const char *value)
{
    return Cvar_Set2(var_name, value, qtrue);
}

/* ==========================================================================
   Cvar_SetValue — SoF export
   ========================================================================== */

void Cvar_SetValue(const char *var_name, float value)
{
    char val[32];

    if (value == (int)value)
        Com_sprintf(val, sizeof(val), "%d", (int)value);
    else
        Com_sprintf(val, sizeof(val), "%f", value);

    Cvar_Set(var_name, val);
}

/* ==========================================================================
   Info String Building
   ========================================================================== */

char *Cvar_Userinfo(void)
{
    static char info[MAX_INFO_STRING];
    cvar_t *var;

    info[0] = 0;
    for (var = cvar_vars; var; var = var->next) {
        if (var->flags & CVAR_USERINFO)
            Info_SetValueForKey(info, var->name, var->string);
    }
    return info;
}

char *Cvar_Serverinfo(void)
{
    static char info[MAX_INFO_STRING];
    cvar_t *var;

    info[0] = 0;
    for (var = cvar_vars; var; var = var->next) {
        if (var->flags & CVAR_SERVERINFO)
            Info_SetValueForKey(info, var->name, var->string);
    }
    return info;
}

/* ==========================================================================
   Cvar_WriteVariables — save archive cvars to config file
   ========================================================================== */

void Cvar_WriteVariables(const char *path)
{
    cvar_t  *var;
    FILE    *f;

    f = fopen(path, "a");
    if (!f)
        return;

    for (var = cvar_vars; var; var = var->next) {
        if (var->flags & CVAR_ARCHIVE)
            fprintf(f, "set %s \"%s\"\n", var->name, var->string);
    }

    fclose(f);
}

/* ==========================================================================
   Console Commands
   ========================================================================== */

static void Cvar_List_f(void)
{
    cvar_t  *var;
    int     count = 0;

    for (var = cvar_vars; var; var = var->next) {
        Com_Printf("%c%c%c %s \"%s\"\n",
            var->flags & CVAR_ARCHIVE ? 'A' : ' ',
            var->flags & CVAR_USERINFO ? 'U' : ' ',
            var->flags & CVAR_SERVERINFO ? 'S' : ' ',
            var->name, var->string);
        count++;
    }
    Com_Printf("%d cvars\n", count);
}

static void Cvar_Set_f(void)
{
    if (Cmd_Argc() != 3) {
        Com_Printf("usage: set <variable> <value>\n");
        return;
    }
    Cvar_Set(Cmd_Argv(1), Cmd_Argv(2));
}

void Cvar_Init(void)
{
    Cmd_AddCommand("set", Cvar_Set_f);
    Cmd_AddCommand("cvarlist", Cvar_List_f);
}
