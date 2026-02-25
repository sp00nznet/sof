/*
 * files.c - Quake II-style virtual filesystem with PAK support
 *
 * Based on Quake II files.c (id Software GPL).
 * Handles the PAK file format and search path management.
 *
 * SoF exports: FS_FOpenFile, FS_FCloseFile, FS_LoadFile, FS_FreeFile
 * Original: FS_FOpenFile=0x278D0, FS_FCloseFile=0x278A0,
 *           FS_LoadFile=0x27E20, FS_FreeFile=0x27EE0
 *
 * SoF's FS_InitFilesystem (0x28AA1) refs: /BASE/, /GHOUL/, %s/pak%i.pak
 *
 * PAK format:
 *   Header: "PACK" magic, directory offset (uint32), directory size (uint32)
 *   Directory: 64-byte entries (56-byte name + uint32 offset + uint32 size)
 *   pak0.pak = 9728 files (673 MB), pak1.pak = 533 files (19.6 MB)
 */

#include "../common/qcommon.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ==========================================================================
   Types
   ========================================================================== */

#define PAK_MAGIC       (('K'<<24)+('C'<<16)+('A'<<8)+'P')  /* "PACK" */
#define MAX_FILES_IN_PAK    16384
#define MAX_HANDLES         64

typedef struct {
    char    name[56];
    int     filepos;
    int     filelen;
} dpackfile_t;

typedef struct pack_s {
    char            filename[MAX_OSPATH];
    FILE            *handle;
    int             numfiles;
    dpackfile_t     *files;
    struct pack_s   *next;      /* for search path chaining */
} pack_t;

typedef struct searchpath_s {
    char                    filename[MAX_OSPATH];   /* loose files directory */
    pack_t                  *pack;                  /* only one of these is set */
    struct searchpath_s     *next;
} searchpath_t;

typedef struct {
    FILE            *file;          /* for loose files */
    pack_t          *pack;          /* for PAK files */
    int             pak_offset;     /* offset within PAK */
    int             pak_remaining;  /* bytes left to read */
    qboolean        used;
} fshandle_t;

/* ==========================================================================
   Globals
   ========================================================================== */

static searchpath_t *fs_searchpaths;
static char         fs_gamedir[MAX_OSPATH];
static char         fs_basedir[MAX_OSPATH];

static fshandle_t   fs_handles[MAX_HANDLES];

/* ==========================================================================
   Handle Management
   ========================================================================== */

static fileHandle_t FS_AllocHandle(void)
{
    int i;
    for (i = 0; i < MAX_HANDLES; i++) {
        if (!fs_handles[i].used) {
            memset(&fs_handles[i], 0, sizeof(fshandle_t));
            fs_handles[i].used = qtrue;
            return i + 1;  /* 1-based handles (0 = invalid) */
        }
    }
    Com_Error(ERR_FATAL, "FS_AllocHandle: no free handles");
    return 0;
}

static fshandle_t *FS_GetHandle(fileHandle_t f)
{
    if (f <= 0 || f > MAX_HANDLES)
        return NULL;
    if (!fs_handles[f - 1].used)
        return NULL;
    return &fs_handles[f - 1];
}

/* ==========================================================================
   PAK File Loading
   ========================================================================== */

static pack_t *FS_LoadPackFile(const char *packfile)
{
    FILE        *packhandle;
    int         header[3];  /* magic, dir_offset, dir_size */
    int         numpackfiles;
    pack_t      *pack;
    dpackfile_t *info;
    int         i;

    packhandle = fopen(packfile, "rb");
    if (!packhandle)
        return NULL;

    if (fread(header, 1, 12, packhandle) != 12) {
        fclose(packhandle);
        return NULL;
    }

    if (LittleLong(header[0]) != PAK_MAGIC) {
        fclose(packhandle);
        Com_Printf("%s is not a packfile\n", packfile);
        return NULL;
    }

    int dir_offset = LittleLong(header[1]);
    int dir_size = LittleLong(header[2]);
    numpackfiles = dir_size / 64;

    if (numpackfiles > MAX_FILES_IN_PAK) {
        fclose(packhandle);
        Com_Error(ERR_FATAL, "%s has %d files (max %d)", packfile, numpackfiles, MAX_FILES_IN_PAK);
        return NULL;
    }

    info = (dpackfile_t *)Z_Malloc(numpackfiles * sizeof(dpackfile_t));

    fseek(packhandle, dir_offset, SEEK_SET);

    /* Read directory entries (64 bytes each: 56 name + 4 offset + 4 size) */
    for (i = 0; i < numpackfiles; i++) {
        char raw[64];
        if (fread(raw, 1, 64, packhandle) != 64)
            break;
        memcpy(info[i].name, raw, 56);
        info[i].name[55] = 0;  /* ensure null termination */
        memcpy(&info[i].filepos, raw + 56, 4);
        memcpy(&info[i].filelen, raw + 60, 4);
        info[i].filepos = LittleLong(info[i].filepos);
        info[i].filelen = LittleLong(info[i].filelen);
    }

    pack = (pack_t *)Z_Malloc(sizeof(pack_t));
    Q_strncpyz(pack->filename, packfile, sizeof(pack->filename));
    pack->handle = packhandle;
    pack->numfiles = i;
    pack->files = info;

    Com_Printf("Added packfile %s (%d files)\n", packfile, pack->numfiles);

    return pack;
}

/* ==========================================================================
   Search Path Management
   ========================================================================== */

static void FS_AddGameDirectory(const char *dir)
{
    searchpath_t    *search;
    pack_t          *pak;
    char            packfile[MAX_OSPATH];
    int             i;

    Q_strncpyz(fs_gamedir, dir, sizeof(fs_gamedir));

    /* Add the directory itself to the search path (for loose files) */
    search = (searchpath_t *)Z_Malloc(sizeof(searchpath_t));
    Q_strncpyz(search->filename, dir, sizeof(search->filename));
    search->next = fs_searchpaths;
    fs_searchpaths = search;

    /* Add any pak files in numbered order: pak0.pak, pak1.pak, etc. */
    for (i = 0; i < 10; i++) {
        Com_sprintf(packfile, sizeof(packfile), "%s/pak%d.pak", dir, i);

        pak = FS_LoadPackFile(packfile);
        if (!pak)
            break;

        search = (searchpath_t *)Z_Malloc(sizeof(searchpath_t));
        search->pack = pak;
        search->next = fs_searchpaths;
        fs_searchpaths = search;
    }
}

/* ==========================================================================
   FS_FOpenFile — SoF export
   ========================================================================== */

int FS_FOpenFile(const char *filename, fileHandle_t *f)
{
    searchpath_t    *search;
    char            netpath[MAX_OSPATH];
    int             i;

    *f = 0;

    /* Search through the path, one element at a time */
    for (search = fs_searchpaths; search; search = search->next) {
        /* Check PAK file */
        if (search->pack) {
            pack_t *pak = search->pack;
            for (i = 0; i < pak->numfiles; i++) {
                if (!Q_stricmp(pak->files[i].name, filename)) {
                    /* Found it */
                    fileHandle_t handle = FS_AllocHandle();
                    fshandle_t *fsh = FS_GetHandle(handle);

                    fsh->pack = pak;
                    fsh->pak_offset = pak->files[i].filepos;
                    fsh->pak_remaining = pak->files[i].filelen;

                    Com_DPrintf("FS_FOpenFile: %s (pak: %s)\n", filename, pak->filename);
                    *f = handle;
                    return pak->files[i].filelen;
                }
            }
        }

        /* Check loose file */
        if (search->filename[0]) {
            Com_sprintf(netpath, sizeof(netpath), "%s/%s", search->filename, filename);

            FILE *fp = fopen(netpath, "rb");
            if (fp) {
                fileHandle_t handle = FS_AllocHandle();
                fshandle_t *fsh = FS_GetHandle(handle);

                /* Get file size */
                fseek(fp, 0, SEEK_END);
                int len = (int)ftell(fp);
                fseek(fp, 0, SEEK_SET);

                fsh->file = fp;
                Com_DPrintf("FS_FOpenFile: %s (loose: %s)\n", filename, netpath);
                *f = handle;
                return len;
            }
        }
    }

    Com_DPrintf("FS_FOpenFile: can't find %s\n", filename);
    return -1;
}

/* ==========================================================================
   FS_FCloseFile — SoF export
   ========================================================================== */

void FS_FCloseFile(fileHandle_t f)
{
    fshandle_t *fsh = FS_GetHandle(f);
    if (!fsh)
        return;

    if (fsh->file) {
        fclose(fsh->file);
        fsh->file = NULL;
    }

    fsh->used = qfalse;
}

/* ==========================================================================
   FS_Read
   ========================================================================== */

int FS_Read(void *buffer, int len, fileHandle_t f)
{
    fshandle_t *fsh = FS_GetHandle(f);
    if (!fsh)
        return 0;

    /* Reading from a PAK file */
    if (fsh->pack) {
        int toread = len;
        if (toread > fsh->pak_remaining)
            toread = fsh->pak_remaining;

        fseek(fsh->pack->handle, fsh->pak_offset, SEEK_SET);
        int got = (int)fread(buffer, 1, toread, fsh->pack->handle);

        fsh->pak_offset += got;
        fsh->pak_remaining -= got;

        return got;
    }

    /* Reading from a loose file */
    if (fsh->file) {
        return (int)fread(buffer, 1, len, fsh->file);
    }

    return 0;
}

/* ==========================================================================
   FS_Write
   ========================================================================== */

int FS_Write(const void *buffer, int len, fileHandle_t f)
{
    fshandle_t *fsh = FS_GetHandle(f);
    if (!fsh || !fsh->file)
        return 0;

    return (int)fwrite(buffer, 1, len, fsh->file);
}

/* ==========================================================================
   FS_LoadFile — SoF export (load entire file to memory)
   ========================================================================== */

int FS_LoadFile(const char *path, void **buffer)
{
    fileHandle_t    f;
    int             len;
    byte            *buf;

    *buffer = NULL;

    len = FS_FOpenFile(path, &f);
    if (!f || len < 0) {
        return -1;
    }

    buf = (byte *)Z_Malloc(len + 1);
    buf[len] = 0;  /* null terminate for text files */

    FS_Read(buf, len, f);
    FS_FCloseFile(f);

    *buffer = buf;
    return len;
}

/* ==========================================================================
   FS_FreeFile — SoF export
   ========================================================================== */

void FS_FreeFile(void *buffer)
{
    if (buffer)
        Z_Free(buffer);
}

/* ==========================================================================
   Directory Listing
   ========================================================================== */

char **FS_ListFiles(const char *findname, int *numfiles)
{
    /* TODO: Implement directory listing for loose files */
    *numfiles = 0;
    return NULL;
}

void FS_FreeFileList(char **list, int nfiles)
{
    int i;
    if (!list)
        return;
    for (i = 0; i < nfiles; i++)
        Z_Free(list[i]);
    Z_Free(list);
}

/* ==========================================================================
   Gamedir Management
   ========================================================================== */

char *FS_Gamedir(void)
{
    return fs_gamedir;
}

void FS_SetGamedir(const char *dir)
{
    /* TODO: Clear existing search paths and rebuild */
    Com_Printf("FS_SetGamedir: %s\n", dir);
}

char *FS_NextPath(char *prevpath)
{
    searchpath_t *s;

    if (!prevpath)
        return fs_gamedir;

    for (s = fs_searchpaths; s; s = s->next) {
        if (s->filename[0] && !strcmp(s->filename, prevpath)) {
            s = s->next;
            if (s)
                return s->filename;
            return NULL;
        }
    }
    return NULL;
}

/* ==========================================================================
   FS_InitFilesystem
   ========================================================================== */

static cvar_t *fs_basepath;
static cvar_t *fs_game;

void FS_InitFilesystem(void)
{
    /* Determine base directory */
    fs_basepath = Cvar_Get("basedir", ".", CVAR_NOSET);
    fs_game = Cvar_Get("game", "", CVAR_LATCH | CVAR_SERVERINFO);

    Q_strncpyz(fs_basedir, fs_basepath->string, sizeof(fs_basedir));

    /* Add base directory ("base" is the default game directory for SoF) */
    FS_AddGameDirectory(va("%s/base", fs_basedir));

    /* Add game directory if specified */
    if (fs_game->string[0]) {
        FS_AddGameDirectory(va("%s/%s", fs_basedir, fs_game->string));
    }
}

void FS_Shutdown(void)
{
    searchpath_t *next;

    while (fs_searchpaths) {
        next = fs_searchpaths->next;

        if (fs_searchpaths->pack) {
            if (fs_searchpaths->pack->handle)
                fclose(fs_searchpaths->pack->handle);
            Z_Free(fs_searchpaths->pack->files);
            Z_Free(fs_searchpaths->pack);
        }

        Z_Free(fs_searchpaths);
        fs_searchpaths = next;
    }
}
