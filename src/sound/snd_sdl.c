/*
 * snd_sdl.c - SDL2 sound system implementation
 *
 * Replaces Defsnd.dll / EAXSnd.dll / A3Dsnd.dll with unified SDL2 audio.
 * Implements the 20-function sound_export_t interface.
 *
 * Sound pipeline:
 *   1. S_RegisterSound: loads .wav/.adp from PAK, decodes to 16-bit PCM
 *   2. S_StartSound: assigns a playback channel with 3D position
 *   3. SDL audio callback: mixes active channels with spatial attenuation
 *   4. S_Update: updates listener position each frame
 *
 * Original SoF sound stats:
 *   - ~1,500 .wav files in pak0.pak
 *   - Sample rates: 11025 Hz (ambient), 22050 Hz (effects), 44100 Hz (music)
 *   - 4 light styles, 8 ambient sets, dynamic music intensity system
 */

#include "snd_local.h"
#include <math.h>

/* ==========================================================================
   Global State
   ========================================================================== */

snd_state_t snd;

/* Cvars */
static cvar_t   *s_volume;
static cvar_t   *s_musicvolume;
static cvar_t   *s_nosound;
static cvar_t   *s_mixahead;
static cvar_t   *s_show;
static cvar_t   *s_khz;

/* ==========================================================================
   WAV File Loader
   ========================================================================== */

/* Read little-endian values from byte buffer */
static short S_ReadShort(byte *p) { return (short)(p[0] | (p[1] << 8)); }
static int S_ReadInt(byte *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

/*
 * Find a RIFF chunk in a WAV file
 * Returns pointer to chunk data and sets *len to data length
 */
static byte *S_FindChunk(byte *data, int datalen, const char *name, int *len)
{
    byte    *p = data + 12;     /* skip RIFF header */
    byte    *end = data + datalen;

    while (p + 8 <= end) {
        int chunk_len = S_ReadInt(p + 4);

        if (p[0] == name[0] && p[1] == name[1] &&
            p[2] == name[2] && p[3] == name[3]) {
            *len = chunk_len;
            return p + 8;
        }

        p += 8 + ((chunk_len + 1) & ~1);   /* chunks are word-aligned */
    }

    *len = 0;
    return NULL;
}

/*
 * Parse WAV file header and return format info
 */
wavinfo_t S_GetWavInfo(const char *name, byte *wav, int wavlength)
{
    wavinfo_t   info;
    byte        *fmt_data;
    byte        *data_chunk;
    int         fmt_len, data_len;

    memset(&info, 0, sizeof(info));
    info.loopstart = -1;

    if (wavlength < 44)
        return info;

    /* Verify RIFF/WAVE header */
    if (wav[0] != 'R' || wav[1] != 'I' || wav[2] != 'F' || wav[3] != 'F')
        return info;
    if (wav[8] != 'W' || wav[9] != 'A' || wav[10] != 'V' || wav[11] != 'E')
        return info;

    /* Find "fmt " chunk */
    fmt_data = S_FindChunk(wav, wavlength, "fmt ", &fmt_len);
    if (!fmt_data || fmt_len < 16)
        return info;

    /* Parse format */
    {
        int format = S_ReadShort(fmt_data);
        if (format != 1) {  /* PCM only */
            Com_DPrintf("S_GetWavInfo: %s is not PCM format (%d)\n", name, format);
            return info;
        }
    }

    info.channels = S_ReadShort(fmt_data + 2);
    info.rate = S_ReadInt(fmt_data + 4);
    info.width = S_ReadShort(fmt_data + 14) / 8;   /* bits per sample → bytes */

    /* Find "data" chunk */
    data_chunk = S_FindChunk(wav, wavlength, "data", &data_len);
    if (!data_chunk)
        return info;

    info.samples = data_len / (info.width * info.channels);
    info.dataofs = (int)(data_chunk - wav);

    /* Check for "cue " chunk (loop point) */
    {
        byte *cue;
        int cue_len;
        cue = S_FindChunk(wav, wavlength, "cue ", &cue_len);
        if (cue && cue_len >= 24) {
            info.loopstart = S_ReadInt(cue + 20);
        }
    }

    return info;
}

/*
 * Load a sound effect from PAK filesystem
 */
void S_LoadSound(sfx_t *sfx)
{
    byte    *data;
    int     len;
    char    namebuf[MAX_QPATH];

    if (sfx->loaded)
        return;

    /* Try with "sound/" prefix if not already present */
    if (sfx->name[0] == '*') {
        /* Player-specific sound — skip for now */
        return;
    }

    if (strncmp(sfx->name, "sound/", 6) != 0) {
        Com_sprintf(namebuf, sizeof(namebuf), "sound/%s", sfx->name);
    } else {
        Q_strncpyz(namebuf, sfx->name, sizeof(namebuf));
    }

    len = FS_LoadFile(namebuf, (void **)&data);
    if (!data) {
        Com_DPrintf("S_LoadSound: couldn't load %s\n", namebuf);
        return;
    }

    sfx->info = S_GetWavInfo(namebuf, data, len);
    if (sfx->info.rate == 0) {
        Com_DPrintf("S_LoadSound: bad wav format %s\n", namebuf);
        FS_FreeFile(data);
        return;
    }

    /* Convert to 16-bit signed PCM at native rate */
    {
        int sample_count = sfx->info.samples * sfx->info.channels;
        int src_bytes = sample_count * sfx->info.width;
        byte *src = data + sfx->info.dataofs;

        sfx->data = (byte *)Z_TagMalloc(sample_count * 2, Z_TAG_LEVEL);
        sfx->length = sfx->info.samples;
        sfx->loopstart = sfx->info.loopstart;

        if (sfx->info.width == 2) {
            /* Already 16-bit — copy directly */
            if (src + src_bytes <= data + len)
                memcpy(sfx->data, src, sample_count * 2);
        } else {
            /* 8-bit unsigned → 16-bit signed */
            int i;
            int16_t *out = (int16_t *)sfx->data;
            for (i = 0; i < sample_count && (src + i) < (data + len); i++) {
                out[i] = (int16_t)((src[i] - 128) << 8);
            }
        }
    }

    sfx->loaded = qtrue;
    FS_FreeFile(data);
}

/* ==========================================================================
   Channel Management
   ========================================================================== */

/*
 * Find a free channel, or steal the quietest one
 */
static channel_t *S_PickChannel(int entnum, int entchannel)
{
    int         i;
    channel_t   *ch;
    int         oldest = -1;
    int         oldest_pos = 0;

    /* First check if this entity+channel combo is already playing */
    if (entchannel != CHAN_AUTO) {
        for (i = 0; i < MAX_CHANNELS; i++) {
            ch = &snd.channels[i];
            if (ch->entnum == entnum && ch->entchannel == entchannel) {
                /* Replace this channel */
                memset(ch, 0, sizeof(*ch));
                return ch;
            }
        }
    }

    /* Find a free channel */
    for (i = 0; i < MAX_CHANNELS; i++) {
        ch = &snd.channels[i];
        if (!ch->sfx) {
            return ch;
        }
        /* Track the oldest channel for stealing */
        if (ch->pos > oldest_pos) {
            oldest_pos = ch->pos;
            oldest = i;
        }
    }

    /* Steal the oldest channel */
    if (oldest >= 0) {
        ch = &snd.channels[oldest];
        memset(ch, 0, sizeof(*ch));
        return ch;
    }

    return NULL;
}

/* ==========================================================================
   SDL Audio Callback
   ========================================================================== */

static void S_AudioCallback(void *userdata, Uint8 *stream, int len)
{
    int         samples_needed;
    int         i, j;
    int         *mixbuf;
    int16_t     *out;

    (void)userdata;

    samples_needed = len / (2 * SND_CHANNELS);  /* 16-bit stereo */

    /* Temporary 32-bit mix buffer on stack */
    mixbuf = (int *)alloca(samples_needed * SND_CHANNELS * sizeof(int));
    memset(mixbuf, 0, samples_needed * SND_CHANNELS * sizeof(int));

    /* Mix all active channels */
    for (i = 0; i < MAX_CHANNELS; i++) {
        channel_t   *ch = &snd.channels[i];
        int16_t     *src;
        float       vol_l, vol_r;

        if (!ch->sfx || !ch->sfx->loaded || !ch->sfx->data)
            continue;

        src = (int16_t *)ch->sfx->data;

        /* Spatial attenuation and stereo panning */
        {
            float base_vol = ch->master_vol * snd.master_volume;
            float dist_atten = 1.0f;
            float pan = 0.0f;  /* -1 = full left, +1 = full right */

            if (ch->dist_mult > 0) {
                vec3_t delta;
                float dist;

                delta[0] = ch->origin[0] - snd.listener_origin[0];
                delta[1] = ch->origin[1] - snd.listener_origin[1];
                delta[2] = ch->origin[2] - snd.listener_origin[2];
                dist = (float)sqrt(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);

                /* Distance attenuation */
                dist_atten = 1.0f - dist * ch->dist_mult;
                if (dist_atten < 0.0f) dist_atten = 0.0f;
                if (dist_atten > 1.0f) dist_atten = 1.0f;

                /* Stereo panning via dot product with listener right vector */
                if (dist > 1.0f) {
                    float inv_dist = 1.0f / dist;
                    pan = (delta[0] * snd.listener_right[0] +
                           delta[1] * snd.listener_right[1] +
                           delta[2] * snd.listener_right[2]) * inv_dist;
                    if (pan > 1.0f) pan = 1.0f;
                    if (pan < -1.0f) pan = -1.0f;
                }
            }

            vol_l = base_vol * dist_atten * (1.0f - pan * 0.35f);
            vol_r = base_vol * dist_atten * (1.0f + pan * 0.35f);
            if (vol_l < 0.001f && vol_r < 0.001f)
                continue;   /* too quiet to hear */
        }

        /* Mix samples */
        for (j = 0; j < samples_needed; j++) {
            int sample_idx = ch->pos;

            if (sample_idx >= ch->sfx->length) {
                /* Check for loop */
                if (ch->sfx->loopstart >= 0) {
                    ch->pos = ch->sfx->loopstart;
                    sample_idx = ch->pos;
                } else {
                    /* Sound finished */
                    ch->sfx = NULL;
                    break;
                }
            }

            if (ch->sfx->info.channels == 1) {
                /* Mono → stereo */
                int s = src[sample_idx];
                mixbuf[j * 2 + 0] += (int)(s * vol_l);
                mixbuf[j * 2 + 1] += (int)(s * vol_r);
            } else {
                /* Stereo */
                mixbuf[j * 2 + 0] += (int)(src[sample_idx * 2 + 0] * vol_l);
                mixbuf[j * 2 + 1] += (int)(src[sample_idx * 2 + 1] * vol_r);
            }

            ch->pos++;
        }
    }

    /* Clamp and write to output */
    out = (int16_t *)stream;
    for (i = 0; i < samples_needed * SND_CHANNELS; i++) {
        int val = mixbuf[i];
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        out[i] = (int16_t)val;
    }
}

/* ==========================================================================
   Sound System Lifecycle
   ========================================================================== */

qboolean S_Init(void)
{
    SDL_AudioSpec desired;

    memset(&snd, 0, sizeof(snd));

    Com_Printf("------- Sound Init -------\n");

    s_volume = Cvar_Get("s_volume", "0.7", CVAR_ARCHIVE);
    s_musicvolume = Cvar_Get("s_musicvolume", "0.25", CVAR_ARCHIVE);
    s_nosound = Cvar_Get("s_nosound", "0", 0);
    s_mixahead = Cvar_Get("s_mixahead", "0.2", CVAR_ARCHIVE);
    s_show = Cvar_Get("s_show", "0", 0);
    s_khz = Cvar_Get("s_khz", "22", CVAR_ARCHIVE);

    if (s_nosound->value) {
        Com_Printf("Sound disabled\n");
        Com_Printf("--------------------------\n");
        return qfalse;
    }

    /* Initialize SDL audio subsystem */
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            Com_Printf("S_Init: SDL_InitSubSystem(AUDIO) failed: %s\n",
                       SDL_GetError());
            Com_Printf("--------------------------\n");
            return qfalse;
        }
    }

    /* Configure audio format */
    memset(&desired, 0, sizeof(desired));
    desired.freq = (int)s_khz->value * 1000;
    if (desired.freq <= 0)
        desired.freq = SND_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = SND_CHANNELS;
    desired.samples = SND_SAMPLES;
    desired.callback = S_AudioCallback;
    desired.userdata = NULL;

    snd.device = SDL_OpenAudioDevice(NULL, 0, &desired, &snd.spec, 0);
    if (snd.device == 0) {
        Com_Printf("S_Init: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        Com_Printf("--------------------------\n");
        return qfalse;
    }

    /* Start playback */
    SDL_PauseAudioDevice(snd.device, 0);

    snd.master_volume = s_volume->value;
    snd.music_volume = s_musicvolume->value;
    snd.initialized = qtrue;
    snd.active = qtrue;

    Com_Printf("SDL Audio: %d Hz, %d ch, %d sample buffer\n",
               snd.spec.freq, snd.spec.channels, snd.spec.samples);
    Com_Printf("Audio driver: %s\n", SDL_GetCurrentAudioDriver());
    Com_Printf("--------------------------\n");

    return qtrue;
}

void S_Shutdown(void)
{
    int i;

    if (!snd.initialized)
        return;

    Com_Printf("Sound shutdown\n");

    /* Stop and close audio device */
    if (snd.device) {
        SDL_PauseAudioDevice(snd.device, 1);
        SDL_CloseAudioDevice(snd.device);
    }

    /* Free all loaded sounds */
    for (i = 0; i < snd.num_sfx; i++) {
        if (snd.known_sfx[i].data) {
            Z_Free(snd.known_sfx[i].data);
        }
    }

    memset(&snd, 0, sizeof(snd));
}

void S_Activate(qboolean active)
{
    snd.active = active;
    if (snd.device) {
        SDL_PauseAudioDevice(snd.device, active ? 0 : 1);
    }
}

/* ==========================================================================
   Registration
   ========================================================================== */

void S_BeginRegistration(void)
{
    snd.registration_sequence++;
}

sfx_t *S_FindName(const char *name)
{
    int     i;
    sfx_t   *sfx;

    if (!name || !name[0])
        return NULL;

    /* Search existing */
    for (i = 0; i < snd.num_sfx; i++) {
        if (Q_stricmp(snd.known_sfx[i].name, name) == 0)
            return &snd.known_sfx[i];
    }

    /* Allocate new slot */
    if (snd.num_sfx >= MAX_SFX) {
        Com_Error(ERR_FATAL, "S_FindName: out of sfx_t slots");
        return NULL;
    }

    sfx = &snd.known_sfx[snd.num_sfx++];
    memset(sfx, 0, sizeof(*sfx));
    Q_strncpyz(sfx->name, name, sizeof(sfx->name));

    return sfx;
}

sfx_t *S_RegisterSound(const char *name)
{
    sfx_t *sfx;

    if (!snd.initialized)
        return NULL;

    sfx = S_FindName(name);
    if (!sfx)
        return NULL;

    sfx->registration_sequence = snd.registration_sequence;

    /* Load on demand — don't load during registration */
    return sfx;
}

void S_EndRegistration(void)
{
    int i;

    /* Free sounds that weren't re-registered this level */
    for (i = 0; i < snd.num_sfx; i++) {
        sfx_t *sfx = &snd.known_sfx[i];
        if (sfx->registration_sequence != snd.registration_sequence) {
            if (sfx->data) {
                Z_Free(sfx->data);
                sfx->data = NULL;
            }
            sfx->loaded = qfalse;
        }
    }
}

void S_RegisterAmbientSet(const char *name) { (void)name; }
void S_RegisterMusicSet(const char *name) { (void)name; }

void S_FreeSound(sfx_t *sfx)
{
    if (sfx && sfx->data) {
        Z_Free(sfx->data);
        sfx->data = NULL;
        sfx->loaded = qfalse;
    }
}

/* ==========================================================================
   Playback
   ========================================================================== */

void S_StartSound(vec3_t origin, int entnum, int entchannel,
                   sfx_t *sfx, float vol, float attenuation,
                   float timeofs)
{
    channel_t   *ch;

    (void)timeofs;

    if (!snd.initialized || !snd.active || !sfx)
        return;

    /* Load on demand */
    if (!sfx->loaded)
        S_LoadSound(sfx);

    if (!sfx->loaded || !sfx->data)
        return;

    /* Lock audio while modifying channels */
    SDL_LockAudioDevice(snd.device);

    ch = S_PickChannel(entnum, entchannel);
    if (ch) {
        ch->sfx = sfx;
        ch->entnum = entnum;
        ch->entchannel = entchannel;
        ch->master_vol = vol;
        ch->dist_mult = attenuation / 1000.0f;
        ch->pos = 0;

        if (origin) {
            VectorCopy(origin, ch->origin);
            ch->fixed_origin = qtrue;
        } else {
            ch->fixed_origin = qfalse;
        }
    }

    SDL_UnlockAudioDevice(snd.device);
}

void S_StartLocalSound(const char *name)
{
    sfx_t *sfx;

    sfx = S_RegisterSound(name);
    if (sfx) {
        S_StartSound(NULL, 0, CHAN_AUTO, sfx, 1.0f, ATTN_NONE, 0);
    }
}

void S_StopAllSounds(void)
{
    if (!snd.initialized)
        return;

    SDL_LockAudioDevice(snd.device);
    memset(snd.channels, 0, sizeof(snd.channels));
    SDL_UnlockAudioDevice(snd.device);
}

/* ==========================================================================
   Update (called each frame)
   ========================================================================== */

void S_Update(vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
    if (!snd.initialized)
        return;

    /* Update listener position */
    if (origin) VectorCopy(origin, snd.listener_origin);
    if (forward) VectorCopy(forward, snd.listener_forward);
    if (right) VectorCopy(right, snd.listener_right);
    if (up) VectorCopy(up, snd.listener_up);

    /* Update volume from cvars */
    snd.master_volume = s_volume->value;
    snd.music_volume = s_musicvolume->value;

    /* Debug: show active channels */
    if (s_show && s_show->value) {
        int i, active = 0;
        for (i = 0; i < MAX_CHANNELS; i++) {
            if (snd.channels[i].sfx)
                active++;
        }
        if (active)
            Com_Printf("snd: %d channels active\n", active);
    }
}

/* ==========================================================================
   Engine Integration Stubs
   ========================================================================== */

void S_SetSoundStruct(void *sound_data) { (void)sound_data; }
void S_SetSoundProcType(int type) { (void)type; }
void S_SetGeneralAmbientSet(const char *name) { (void)name; }

void S_SetMusicIntensity(float intensity)
{
    snd.music_intensity = intensity;
}

void S_SetMusicDesignerInfo(void *info) { (void)info; }
