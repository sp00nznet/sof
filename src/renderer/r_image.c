/*
 * r_image.c - Texture loading and management
 *
 * Loads textures from SoF PAK archives for BSP surface rendering.
 * SoF uses two texture formats:
 *
 * 1. WAL (Quake II standard) - 8-bit paletted, 4 mip levels
 *    Header: 32-byte name, width, height, 4 offsets, next texture, flags, value
 *
 * 2. M32 (MIP32 / SoF enhanced) - 32-bit RGBA, 4 mip levels
 *    Header: 0x28 bytes minimum, width/height at +4/+8, pixel data follows
 *    Some M32 files have extended headers with data at offset 0xA0
 *
 * Texture lookup: BSP texinfo contains a texture name (e.g., "base/floor01")
 * that maps to "textures/base/floor01.m32" or "textures/base/floor01.wal"
 * in the PAK filesystem.
 *
 * Original ref_gl.dll: GL_FindImage at 0x30010030
 */

#include "r_local.h"

/* ==========================================================================
   Image Cache
   ========================================================================== */

#define MAX_R_IMAGES    512

static image_t  r_images[MAX_R_IMAGES];
static int      r_numimages;
static int      r_registration_sequence;

/* Checkerboard fallback texture */
static GLuint   r_notexture;
static GLuint   r_whitetexture;

/* ==========================================================================
   Q2 Palette (for WAL texture decoding)
   Approximate — the real palette is in colormap.pcx
   ========================================================================== */

static byte q2_palette[768];
static qboolean q2_palette_loaded = qfalse;

static void R_LoadPalette(void)
{
    byte    *raw;
    int     len;
    int     i;

    len = FS_LoadFile("pics/colormap.pcx", (void **)&raw);
    if (raw && len > 768 + 1) {
        /* PCX palette is at end of file: last 769 bytes = 0x0C marker + 768 bytes RGB */
        byte *pal = raw + len - 768;
        if (raw[len - 769] == 0x0C) {
            memcpy(q2_palette, pal, 768);
            q2_palette_loaded = qtrue;
            Com_DPrintf("Loaded Q2 palette from colormap.pcx\n");
        }
        FS_FreeFile(raw);
    }

    if (!q2_palette_loaded) {
        /* Generate a default greyscale palette */
        for (i = 0; i < 256; i++) {
            q2_palette[i * 3 + 0] = (byte)i;
            q2_palette[i * 3 + 1] = (byte)i;
            q2_palette[i * 3 + 2] = (byte)i;
        }
    }
}

/* ==========================================================================
   GL Texture Upload
   ========================================================================== */

static GLuint R_UploadTexture(byte *data, int width, int height,
                               qboolean mipmap, qboolean has_alpha)
{
    GLuint texnum;

    qglGenTextures(1, &texnum);
    qglBindTexture(GL_TEXTURE_2D, texnum);

    qglTexImage2D(GL_TEXTURE_2D, 0, has_alpha ? GL_RGBA : GL_RGBA,
                  width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    if (mipmap) {
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        /* Generate mipmaps manually or use GL extension */
        {
            int level = 0;
            int w = width, h = height;
            byte *mip = data;
            byte *prev = NULL;

            while (w > 1 || h > 1) {
                int nw = w > 1 ? w / 2 : 1;
                int nh = h > 1 ? h / 2 : 1;
                int x, y;

                prev = mip;
                mip = (byte *)Z_Malloc(nw * nh * 4);

                for (y = 0; y < nh; y++) {
                    for (x = 0; x < nw; x++) {
                        int sx = x * 2, sy = y * 2;
                        int src = (sy * w + sx) * 4;
                        int dst = (y * nw + x) * 4;
                        int c;
                        for (c = 0; c < 4; c++) {
                            int sum = prev[src + c];
                            if (sx + 1 < w) sum += prev[src + 4 + c]; else sum += prev[src + c];
                            if (sy + 1 < h) sum += prev[(src + w * 4) + c]; else sum += prev[src + c];
                            if (sx + 1 < w && sy + 1 < h) sum += prev[(src + w * 4 + 4) + c]; else sum += prev[src + c];
                            mip[dst + c] = (byte)(sum / 4);
                        }
                    }
                }

                level++;
                w = nw;
                h = nh;
                qglTexImage2D(GL_TEXTURE_2D, level, has_alpha ? GL_RGBA : GL_RGBA,
                              w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, mip);

                if (prev != data)
                    Z_Free(prev);
            }
            if (mip != data)
                Z_Free(mip);
        }
    } else {
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    return texnum;
}

/* ==========================================================================
   Fallback Textures
   ========================================================================== */

static void R_InitDefaultTextures(void)
{
    byte    checker[64 * 64 * 4];
    byte    white[4] = { 255, 255, 255, 255 };
    int     x, y;

    /* Checkerboard pattern for missing textures */
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            int ofs = (y * 64 + x) * 4;
            if ((x ^ y) & 8) {
                checker[ofs + 0] = 255; checker[ofs + 1] = 0;
                checker[ofs + 2] = 255; checker[ofs + 3] = 255;
            } else {
                checker[ofs + 0] = 0; checker[ofs + 1] = 0;
                checker[ofs + 2] = 0; checker[ofs + 3] = 255;
            }
        }
    }
    r_notexture = R_UploadTexture(checker, 64, 64, qtrue, qfalse);

    /* Solid white for flat-shaded rendering */
    r_whitetexture = R_UploadTexture(white, 1, 1, qfalse, qfalse);
}

/* ==========================================================================
   WAL Texture Loader (Quake II standard)
   ========================================================================== */

typedef struct {
    char    name[32];
    int     width;
    int     height;
    int     offsets[4];     /* 4 mip level offsets */
    char    animname[32];   /* next frame in animation */
    int     flags;
    int     contents;
    int     value;
} wal_header_t;

static image_t *R_LoadWAL(const char *name, byte *raw, int rawlen)
{
    wal_header_t    *wal;
    image_t         *img;
    byte            *pixels;
    byte            *rgba;
    int             i, size;

    if (rawlen < (int)sizeof(wal_header_t))
        return NULL;

    wal = (wal_header_t *)raw;

    if (wal->width <= 0 || wal->height <= 0 ||
        wal->width > 2048 || wal->height > 2048)
        return NULL;

    size = wal->width * wal->height;
    if (wal->offsets[0] + size > rawlen)
        return NULL;

    /* Find or create image slot */
    img = &r_images[r_numimages++];
    memset(img, 0, sizeof(*img));
    Q_strncpyz(img->name, name, sizeof(img->name));
    img->width = wal->width;
    img->height = wal->height;
    img->type = it_wall;

    /* Convert 8-bit paletted to RGBA */
    pixels = raw + wal->offsets[0];
    rgba = (byte *)Z_Malloc(size * 4);

    for (i = 0; i < size; i++) {
        byte idx = pixels[i];
        rgba[i * 4 + 0] = q2_palette[idx * 3 + 0];
        rgba[i * 4 + 1] = q2_palette[idx * 3 + 1];
        rgba[i * 4 + 2] = q2_palette[idx * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }

    img->texnum = R_UploadTexture(rgba, wal->width, wal->height, qtrue, qfalse);
    Z_Free(rgba);

    return img;
}

/* ==========================================================================
   M32 Texture Loader (SoF MIP32 enhanced)
   ========================================================================== */

typedef struct {
    int     version;        /* +0x00 */
    int     width;          /* +0x04 */
    int     height;         /* +0x08 */
    int     mip_offsets[4]; /* +0x0C */
    char    name[32];       /* +0x1C */
    /* ... extended header fields follow */
} m32_header_t;

static image_t *R_LoadM32(const char *name, byte *raw, int rawlen)
{
    image_t     *img;
    int         width, height;
    int         data_offset;
    int         size;

    if (rawlen < 0x28)
        return NULL;

    width = *(int *)(raw + 4);
    height = *(int *)(raw + 8);

    if (width <= 0 || height <= 0 || width > 2048 || height > 2048)
        return NULL;

    size = width * height * 4;  /* RGBA */

    /* M32 files have pixel data at different offsets depending on version */
    data_offset = 0x28;     /* Standard header size */
    if (data_offset + size > rawlen) {
        data_offset = 0xA0;    /* Extended header */
        if (data_offset + size > rawlen) {
            Com_DPrintf("R_LoadM32: %s data doesn't fit (w=%d h=%d len=%d)\n",
                       name, width, height, rawlen);
            return NULL;
        }
    }

    /* Find or create image slot */
    img = &r_images[r_numimages++];
    memset(img, 0, sizeof(*img));
    Q_strncpyz(img->name, name, sizeof(img->name));
    img->width = width;
    img->height = height;
    img->type = it_wall;

    /* M32 stores RGBA directly */
    img->texnum = R_UploadTexture(raw + data_offset, width, height, qtrue, qtrue);
    img->has_alpha = qtrue;

    return img;
}

/* ==========================================================================
   Texture Lookup
   ========================================================================== */

/*
 * R_FindImage - Find or load a texture by name
 *
 * Searches cache first, then tries loading from PAK filesystem.
 * Texture names from BSP texinfo (e.g., "base/floor01") are looked up as:
 *   textures/base/floor01.m32  (SoF enhanced)
 *   textures/base/floor01.wal  (Q2 standard)
 */
image_t *R_FindImage(const char *name)
{
    image_t *img;
    byte    *raw;
    int     len;
    char    fullname[MAX_QPATH];
    int     i;

    if (!name || !name[0])
        return NULL;

    /* Search existing images */
    for (i = 0; i < r_numimages; i++) {
        if (Q_stricmp(r_images[i].name, name) == 0) {
            r_images[i].registration_sequence = r_registration_sequence;
            return &r_images[i];
        }
    }

    if (r_numimages >= MAX_R_IMAGES) {
        Com_Printf("R_FindImage: MAX_R_IMAGES exceeded\n");
        return NULL;
    }

    /* Try M32 first (SoF enhanced format) */
    Com_sprintf(fullname, sizeof(fullname), "textures/%s.m32", name);
    len = FS_LoadFile(fullname, (void **)&raw);
    if (raw) {
        img = R_LoadM32(name, raw, len);
        FS_FreeFile(raw);
        if (img) {
            img->registration_sequence = r_registration_sequence;
            return img;
        }
    }

    /* Try WAL (Q2 standard format) */
    Com_sprintf(fullname, sizeof(fullname), "textures/%s.wal", name);
    len = FS_LoadFile(fullname, (void **)&raw);
    if (raw) {
        img = R_LoadWAL(name, raw, len);
        FS_FreeFile(raw);
        if (img) {
            img->registration_sequence = r_registration_sequence;
            return img;
        }
    }

    Com_DPrintf("R_FindImage: couldn't load %s\n", name);
    return NULL;
}

/*
 * R_GetNoTexture - Returns the magenta checkerboard fallback
 */
GLuint R_GetNoTexture(void)
{
    return r_notexture;
}

/* ==========================================================================
   Image System Init / Shutdown
   ========================================================================== */

void R_InitImages(void)
{
    r_numimages = 0;
    r_registration_sequence = 0;

    R_LoadPalette();
    R_InitDefaultTextures();

    Com_Printf("Image system initialized\n");
}

void R_ShutdownImages(void)
{
    int i;

    for (i = 0; i < r_numimages; i++) {
        if (r_images[i].texnum)
            qglDeleteTextures(1, &r_images[i].texnum);
    }

    if (r_notexture) qglDeleteTextures(1, &r_notexture);
    if (r_whitetexture) qglDeleteTextures(1, &r_whitetexture);

    r_numimages = 0;
}

void R_ImageBeginRegistration(void)
{
    r_registration_sequence++;
}

void R_ImageEndRegistration(void)
{
    int i;

    /* Free images that weren't re-registered this level */
    for (i = 0; i < r_numimages; i++) {
        if (r_images[i].registration_sequence != r_registration_sequence) {
            if (r_images[i].texnum) {
                qglDeleteTextures(1, &r_images[i].texnum);
                r_images[i].texnum = 0;
            }
        }
    }
}
