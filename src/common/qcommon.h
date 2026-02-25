/*
 * qcommon.h - Common definitions for client and server
 *
 * Based on Quake II qcommon.h (id Software GPL) with SoF modifications.
 * Declares the engine's core subsystems: memory, commands, cvars, filesystem.
 */

#ifndef QCOMMON_H
#define QCOMMON_H

#include "q_shared.h"

/* ==========================================================================
   Command Buffer
   ========================================================================== */

void    Cbuf_Init(void);
void    Cbuf_AddText(const char *text);
void    Cbuf_InsertText(const char *text);
void    Cbuf_Execute(void);
void    Cbuf_CopyToDefer(void);
void    Cbuf_ExecuteDefer(void);

/* ==========================================================================
   Console Commands
   ========================================================================== */

void    Cmd_Init(void);
int     Cmd_Argc(void);
char    *Cmd_Argv(int arg);
char    *Cmd_Args(void);
void    Cmd_TokenizeString(char *text, qboolean macroExpand);
void    Cmd_AddCommand(const char *cmd_name, xcommand_t function);
void    Cmd_RemoveCommand(const char *cmd_name);
qboolean Cmd_Exists(const char *cmd_name);
void    Cmd_ExecuteString(char *text);

/* ==========================================================================
   Console Variables
   ========================================================================== */

void    Cvar_Init(void);
cvar_t  *Cvar_Get(const char *var_name, const char *var_value, int flags);
cvar_t  *Cvar_Set(const char *var_name, const char *value);
cvar_t  *Cvar_ForceSet(const char *var_name, const char *value);
void    Cvar_SetValue(const char *var_name, float value);
float   Cvar_VariableValue(const char *var_name);
char    *Cvar_VariableString(const char *var_name);
cvar_t  *Cvar_FindVar(const char *var_name);
void    Cvar_WriteVariables(const char *path);
char    *Cvar_Userinfo(void);
char    *Cvar_Serverinfo(void);

/* ==========================================================================
   Zone Memory Allocator
   ========================================================================== */

#define Z_TAG_GENERAL   0
#define Z_TAG_LEVEL     1
#define Z_TAG_GAME      2

void    Z_Init(void);
void    *Z_Malloc(int size);
void    *Z_TagMalloc(int size, int tag);
void    Z_Free(void *ptr);
void    Z_Touch(void *ptr);
void    Z_FreeTags(int tag);

/* ==========================================================================
   Hunk Memory (Large Persistent Allocations)
   ========================================================================== */

void    *Hunk_Begin(int maxsize);
void    *Hunk_Alloc(int size);
int     Hunk_End(void);
void    Hunk_Free(void *base);

/* ==========================================================================
   Filesystem
   ========================================================================== */

#define MAX_READ    0x10000     /* read in blocks of 64k */

typedef int fileHandle_t;

typedef enum {
    FS_READ,
    FS_WRITE,
    FS_APPEND
} fsMode_t;

void    FS_InitFilesystem(void);
void    FS_Shutdown(void);

int     FS_FOpenFile(const char *filename, fileHandle_t *f);
void    FS_FCloseFile(fileHandle_t f);
int     FS_Read(void *buffer, int len, fileHandle_t f);
int     FS_Write(const void *buffer, int len, fileHandle_t f);

int     FS_LoadFile(const char *path, void **buffer);
void    FS_FreeFile(void *buffer);

char    **FS_ListFiles(const char *findname, int *numfiles);
void    FS_FreeFileList(char **list, int nfiles);

void    FS_SetGamedir(const char *dir);
char    *FS_Gamedir(void);
char    *FS_NextPath(char *prevpath);

/* ==========================================================================
   Console Printing
   ========================================================================== */

#define PRINT_ALL       0
#define PRINT_DEVELOPER 1
#define PRINT_ALERT     2

void    Com_Printf(const char *fmt, ...);
void    Com_DPrintf(const char *fmt, ...);
void    Com_Error(int code, const char *fmt, ...);

/* ==========================================================================
   Network
   ========================================================================== */

typedef enum {
    NA_LOOPBACK,
    NA_BROADCAST,
    NA_IP,
    NA_IPX,
    NA_BROADCAST_IPX
} netadrtype_t;

typedef struct {
    netadrtype_t    type;
    byte            ip[4];
    byte            ipx[10];
    unsigned short  port;
} netadr_t;

/* ==========================================================================
   Engine Core
   ========================================================================== */

void    Qcommon_Init(int argc, char **argv);
void    Qcommon_Frame(int msec);
void    Qcommon_Shutdown(void);

/* from sys layer */
void    Sys_Init(void);
void    Sys_Quit(void);
void    Sys_Error(const char *error, ...);
char    *Sys_ConsoleInput(void);
void    Sys_ConsoleOutput(const char *string);
int     Sys_Milliseconds(void);
void    Sys_Mkdir(const char *path);
char    *Sys_FindFirst(const char *path, unsigned musthave, unsigned canthave);
char    *Sys_FindNext(unsigned musthave, unsigned canthave);
void    Sys_FindClose(void);

/* Client and Server forward declarations */
void    CL_Init(void);
void    CL_Drop(void);
void    CL_Shutdown(void);
void    CL_Frame(int msec);

void    SV_Init(void);
void    SV_Shutdown(const char *finalmsg, qboolean reconnect);
void    SV_Frame(int msec);

#endif /* QCOMMON_H */
