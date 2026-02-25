/*
 * net_msg.c - Network message buffer operations
 *
 * Quake II network serialization for sizebuf_t message buffers.
 * Used by game module (via game_import_t Write* functions) and
 * client/server for packet construction and parsing.
 *
 * Original SoF: MSG_WriteChar=0x1D790, MSG_WriteByte=0x1D7B0
 * Q2 standard encoding: little-endian, coords scaled by 8
 */

#include "../common/qcommon.h"

#include <string.h>
#include <math.h>

/* ==========================================================================
   Size Buffer Operations
   ========================================================================== */

void SZ_Init(sizebuf_t *buf, byte *data, int length)
{
    memset(buf, 0, sizeof(*buf));
    buf->data = data;
    buf->maxsize = length;
}

void SZ_Clear(sizebuf_t *buf)
{
    buf->cursize = 0;
    buf->overflowed = qfalse;
}

void *SZ_GetSpace(sizebuf_t *buf, int length)
{
    void *data;

    if (buf->cursize + length > buf->maxsize) {
        if (!buf->allowoverflow)
            Com_Error(ERR_FATAL, "SZ_GetSpace: overflow without allowoverflow set");

        if (length > buf->maxsize)
            Com_Error(ERR_FATAL, "SZ_GetSpace: %d is > full buffer size", length);

        Com_Printf("SZ_GetSpace: overflow\n");
        SZ_Clear(buf);
        buf->overflowed = qtrue;
    }

    data = buf->data + buf->cursize;
    buf->cursize += length;
    return data;
}

void SZ_Write(sizebuf_t *buf, const void *data, int length)
{
    memcpy(SZ_GetSpace(buf, length), data, length);
}

void SZ_Print(sizebuf_t *buf, const char *data)
{
    int len = (int)strlen(data) + 1;

    if (buf->cursize) {
        /* If buffer already has a trailing NUL, overwrite it */
        if (buf->data[buf->cursize - 1])
            memcpy((byte *)SZ_GetSpace(buf, len), data, len);
        else
            memcpy((byte *)SZ_GetSpace(buf, len - 1) - 1, data, len);
    } else {
        memcpy((byte *)SZ_GetSpace(buf, len), data, len);
    }
}

/* ==========================================================================
   Message Writing
   ========================================================================== */

void MSG_WriteChar(sizebuf_t *sb, int c)
{
    byte *buf = (byte *)SZ_GetSpace(sb, 1);
    buf[0] = (byte)(c & 0xFF);
}

void MSG_WriteByte(sizebuf_t *sb, int c)
{
    byte *buf = (byte *)SZ_GetSpace(sb, 1);
    buf[0] = (byte)(c & 0xFF);
}

void MSG_WriteShort(sizebuf_t *sb, int c)
{
    byte *buf = (byte *)SZ_GetSpace(sb, 2);
    buf[0] = (byte)(c & 0xFF);
    buf[1] = (byte)((c >> 8) & 0xFF);
}

void MSG_WriteLong(sizebuf_t *sb, int c)
{
    byte *buf = (byte *)SZ_GetSpace(sb, 4);
    buf[0] = (byte)(c & 0xFF);
    buf[1] = (byte)((c >> 8) & 0xFF);
    buf[2] = (byte)((c >> 16) & 0xFF);
    buf[3] = (byte)((c >> 24) & 0xFF);
}

void MSG_WriteFloat(sizebuf_t *sb, float f)
{
    union {
        float f;
        int l;
    } dat;
    dat.f = f;
    MSG_WriteLong(sb, dat.l);
}

void MSG_WriteString(sizebuf_t *sb, const char *s)
{
    if (!s)
        SZ_Write(sb, "", 1);
    else
        SZ_Write(sb, s, (int)strlen(s) + 1);
}

void MSG_WriteCoord(sizebuf_t *sb, float f)
{
    /* Q2: coordinates encoded as short * 8 */
    MSG_WriteShort(sb, (int)(f * 8));
}

void MSG_WritePos(sizebuf_t *sb, vec3_t pos)
{
    MSG_WriteCoord(sb, pos[0]);
    MSG_WriteCoord(sb, pos[1]);
    MSG_WriteCoord(sb, pos[2]);
}

void MSG_WriteAngle(sizebuf_t *sb, float f)
{
    MSG_WriteByte(sb, (int)(f * 256 / 360) & 0xFF);
}

void MSG_WriteAngle16(sizebuf_t *sb, float f)
{
    MSG_WriteShort(sb, (int)(f * 65536 / 360) & 0xFFFF);
}

/*
 * Q2 direction encoding: 162 pre-computed unit vectors
 * For simplicity, we encode as 3 bytes (compressed angles)
 */
void MSG_WriteDir(sizebuf_t *sb, vec3_t dir)
{
    int best = 0;
    /* Simplified: write as angles */
    float yaw = (float)atan2(dir[1], dir[0]) * (180.0f / 3.14159265f);
    float pitch = (float)asin(dir[2]) * (180.0f / 3.14159265f);
    (void)best;
    MSG_WriteAngle(sb, pitch);
    MSG_WriteAngle(sb, yaw);
}

void MSG_WriteDeltaUsercmd(sizebuf_t *sb, void *from, void *cmd)
{
    /* Stub — full delta encoding needed for multiplayer */
    (void)sb; (void)from; (void)cmd;
}

/* ==========================================================================
   Message Reading
   ========================================================================== */

void MSG_BeginReading(sizebuf_t *sb)
{
    sb->readcount = 0;
}

int MSG_ReadChar(sizebuf_t *sb)
{
    int c;

    if (sb->readcount + 1 > sb->cursize)
        return -1;

    c = (signed char)sb->data[sb->readcount];
    sb->readcount++;
    return c;
}

int MSG_ReadByte(sizebuf_t *sb)
{
    int c;

    if (sb->readcount + 1 > sb->cursize)
        return -1;

    c = (unsigned char)sb->data[sb->readcount];
    sb->readcount++;
    return c;
}

int MSG_ReadShort(sizebuf_t *sb)
{
    int c;

    if (sb->readcount + 2 > sb->cursize)
        return -1;

    c = (short)(sb->data[sb->readcount] | (sb->data[sb->readcount + 1] << 8));
    sb->readcount += 2;
    return c;
}

int MSG_ReadLong(sizebuf_t *sb)
{
    int c;

    if (sb->readcount + 4 > sb->cursize)
        return -1;

    c = sb->data[sb->readcount]
        | (sb->data[sb->readcount + 1] << 8)
        | (sb->data[sb->readcount + 2] << 16)
        | (sb->data[sb->readcount + 3] << 24);
    sb->readcount += 4;
    return c;
}

float MSG_ReadFloat(sizebuf_t *sb)
{
    union {
        float f;
        int l;
    } dat;
    dat.l = MSG_ReadLong(sb);
    return dat.f;
}

static char msg_string[2048];

char *MSG_ReadString(sizebuf_t *sb)
{
    int c;
    int l = 0;

    do {
        c = MSG_ReadChar(sb);
        if (c == -1 || c == 0)
            break;
        if (l < (int)sizeof(msg_string) - 1)
            msg_string[l++] = (char)c;
    } while (1);

    msg_string[l] = 0;
    return msg_string;
}

float MSG_ReadCoord(sizebuf_t *sb)
{
    return (float)MSG_ReadShort(sb) * (1.0f / 8.0f);
}

void MSG_ReadPos(sizebuf_t *sb, vec3_t pos)
{
    pos[0] = MSG_ReadCoord(sb);
    pos[1] = MSG_ReadCoord(sb);
    pos[2] = MSG_ReadCoord(sb);
}

float MSG_ReadAngle(sizebuf_t *sb)
{
    return (float)MSG_ReadChar(sb) * (360.0f / 256.0f);
}

float MSG_ReadAngle16(sizebuf_t *sb)
{
    return (float)MSG_ReadShort(sb) * (360.0f / 65536.0f);
}

void MSG_ReadDir(sizebuf_t *sb, vec3_t dir)
{
    float pitch = MSG_ReadAngle(sb);
    float yaw = MSG_ReadAngle(sb);
    float pr = pitch * (3.14159265f / 180.0f);
    float yr = yaw * (3.14159265f / 180.0f);

    dir[0] = (float)(cos(yr) * cos(pr));
    dir[1] = (float)(sin(yr) * cos(pr));
    dir[2] = (float)(sin(pr));
}

void MSG_ReadDeltaUsercmd(sizebuf_t *sb, void *from, void *cmd)
{
    (void)sb; (void)from; (void)cmd;
}
