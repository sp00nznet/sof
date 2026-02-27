/*
 * snd_local.h - Sound system definitions
 *
 * Replaces the original 3-DLL sound architecture:
 *   Defsnd.dll  — DirectSound software mixer (default)
 *   EAXSnd.dll  — Creative EAX hardware 3D audio
 *   A3Dsnd.dll  — Aureal A3D positional audio
 *
 * All three shared the same 20-function sound_export_t interface
 * and were mutually exclusive (all mapped at 0x60000000).
 *
 * Our unified replacement uses SDL2 audio for cross-platform support.
 *
 * Sound formats in SoF:
 *   .wav — Standard PCM wave files (most sounds)
 *   .adp — IMA ADPCM compressed (music/ambient, 4:1 ratio)
 *
 * Sound system imports from SoF.exe (20 functions via ordinal):
 *   Com_Printf, Com_DPrintf, Com_Error, Z_Malloc, Z_Free, Z_TagMalloc,
 *   Z_TagFree, FS_LoadFile, FS_FreeFile, Cmd_AddCommand, etc.
 */

#ifndef SND_LOCAL_H
#define SND_LOCAL_H

#include "../common/qcommon.h"
#include <SDL.h>

/* ==========================================================================
   Sound System Limits
   ========================================================================== */

#define MAX_SFX             512     /* max loaded sound effects */
#define MAX_CHANNELS        32      /* max simultaneous playback channels */
#define MAX_RAW_SAMPLES     16384   /* for streaming/music */

#define SND_RATE            22050   /* SoF default sample rate */
#define SND_CHANNELS        2       /* stereo */
#define SND_SAMPLES         2048    /* SDL audio buffer size */

/* Attenuation */
#define ATTN_NONE           0       /* full volume everywhere */
#define ATTN_NORM           1       /* normal attenuation */
#define ATTN_IDLE           2       /* quiet — idle sounds */
#define ATTN_STATIC         3       /* barely audible at distance */

/* Sound channels (per-entity) */
#define CHAN_AUTO            0       /* auto-assign channel */
#define CHAN_WEAPON          1
#define CHAN_VOICE           2
#define CHAN_ITEM            3
#define CHAN_BODY            4
#define CHAN_LOOP            0x100   /* flag: force looping playback */

/* ==========================================================================
   WAV File Format
   ========================================================================== */

typedef struct {
    int         rate;
    int         width;          /* bytes per sample (1 or 2) */
    int         channels;       /* 1=mono, 2=stereo */
    int         loopstart;      /* loop point in samples, -1 = no loop */
    int         samples;        /* total samples */
    int         dataofs;        /* offset to PCM data in file */
} wavinfo_t;

/* ==========================================================================
   Sound Effect (loaded sound)
   ========================================================================== */

typedef struct sfx_s {
    char        name[MAX_QPATH];
    qboolean    loaded;
    int         registration_sequence;

    /* PCM data (converted to output format) */
    int         length;         /* in samples */
    int         loopstart;      /* -1 = no loop */
    byte        *data;          /* 16-bit signed PCM */

    /* Original format info */
    wavinfo_t   info;
} sfx_t;

/* ==========================================================================
   Playback Channel
   ========================================================================== */

typedef struct {
    sfx_t       *sfx;           /* sound being played (NULL = inactive) */
    int         entnum;         /* entity number */
    int         entchannel;     /* entity channel */
    vec3_t      origin;         /* 3D position */
    float       master_vol;     /* 0.0-1.0 */
    float       dist_mult;      /* attenuation multiplier */
    int         pos;            /* current sample position */
    int         end;            /* end sample position */
    qboolean    autosound;      /* ambient looping */
    qboolean    looping;        /* force loop from start (ambient/music) */
    qboolean    fixed_origin;   /* use origin instead of entity origin */
} channel_t;

/* ==========================================================================
   Sound System State
   ========================================================================== */

typedef struct {
    qboolean    initialized;
    qboolean    active;

    /* SDL audio device */
    SDL_AudioDeviceID   device;
    SDL_AudioSpec       spec;

    /* Mixing buffer */
    int16_t     *mixbuffer;     /* mixed output buffer */
    int         mixsamples;     /* size of mix buffer in samples */

    /* Loaded sounds */
    sfx_t       known_sfx[MAX_SFX];
    int         num_sfx;
    int         registration_sequence;

    /* Playback channels */
    channel_t   channels[MAX_CHANNELS];

    /* Listener state (updated from client) */
    vec3_t      listener_origin;
    vec3_t      listener_forward;
    vec3_t      listener_right;
    vec3_t      listener_up;

    /* Volume controls */
    float       master_volume;
    float       music_volume;

    /* Music/ambient state */
    int         ambient_set;
    float       music_intensity;
} snd_state_t;

extern snd_state_t snd;

/* ==========================================================================
   Sound System API (20 functions matching original sound_export_t)
   ========================================================================== */

/* Lifecycle */
qboolean    S_Init(void);
void        S_Shutdown(void);
void        S_Activate(qboolean active);
void        S_Update(vec3_t origin, vec3_t forward, vec3_t right, vec3_t up);

/* Registration */
void        S_BeginRegistration(void);
void        S_EndRegistration(void);
sfx_t       *S_RegisterSound(const char *name);
void        S_RegisterAmbientSet(const char *name);
void        S_RegisterMusicSet(const char *name);
sfx_t       *S_FindName(const char *name);
void        S_FreeSound(sfx_t *sfx);

/* Playback */
void        S_StartSound(vec3_t origin, int entnum, int entchannel,
                          sfx_t *sfx, float vol, float attenuation,
                          float timeofs);
void        S_StartLoopingSound(vec3_t origin, int entnum, int entchannel,
                                 sfx_t *sfx, float vol, float attenuation);
void        S_StartLocalSound(const char *name);
void        S_StopAllSounds(void);

/* Engine integration */
void        S_SetSoundStruct(void *sound_data);
void        S_SetSoundProcType(int type);

/* Dynamic audio */
void        S_SetGeneralAmbientSet(const char *name);
void        S_SetMusicIntensity(float intensity);
void        S_SetMusicDesignerInfo(void *info);

/* Internal helpers */
wavinfo_t   S_GetWavInfo(const char *name, byte *wav, int wavlength);
void        S_LoadSound(sfx_t *sfx);

#endif /* SND_LOCAL_H */
