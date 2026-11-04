/*
 * Candy Crisis on acsdl12: soundfx.c and music.c for the Amiga.
 *
 * Upstream mixes everything itself (cmixer) through an SDL 2 audio device.
 * Here the sound effects are acsdl12's SDL_mixer chunks, one channel per
 * effect so that an effect played again restarts, as upstream rewinds its
 * voice. Upstream raises an effect's pitch by 1 + n/16 (the combo chime
 * climbs); a chunk at each pitch asked for is resampled once, the first
 * time, and kept. The output is mono, so the players' left/right panning
 * is lost.
 *
 * Music is upstream's own player, IBXM, for both its MODs and its S3Ms
 * (acsdl12 plays MODs only), run in acsdl12's music hook at the mixer's
 * rate. Its speed-up when a player's stack nears the top works as upstream.
 *
 * The AmiCradle Team. GPL 2, like Candy Crisis.
 */
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_mixer.h>

#include "main.h"
#include "soundfx.h"
#include "music.h"
#include "support/ibxm.h"

#define k_rate        22050
#define k_effectVol   (MIX_MAX_VOLUME * 7 / 10)      /* upstream's 0.7 gain */
#define k_maxPitches  16

MBoolean soundOn = true;
MBoolean musicOn = true;
int      musicSelection = -1;
int      s_musicPaused = 0;

static MBoolean   s_audioOpen;
static Mix_Chunk* s_soundBank[kNumSounds][k_maxPitches];

/* ------------------------------------------------------------- effects */

/* The chunk at pitch 1 + freq/16: fewer samples, read faster. */
static Mix_Chunk* PitchedChunk(int which, int freq)
{
    Mix_Chunk *base = s_soundBank[which][0], *c;
    const Sint16 *in;
    Sint16 *out;
    Uint32 inLen, outLen, i, step, pos;

    if (freq <= 0 || !base)
        return base;
    if (freq >= k_maxPitches)
        freq = k_maxPitches - 1;
    if (s_soundBank[which][freq])
        return s_soundBank[which][freq];

    in = (const Sint16 *)base->abuf;
    inLen = base->alen / 2;
    step = 65536 + (Uint32)freq * 4096;               /* 16.16 */
    outLen = (Uint32)(((unsigned long long)inLen << 16) / step);
    if (outLen < 1)
        return base;
    out = (Sint16 *)malloc(outLen * 2);
    if (!out)
        return base;
    for (i = 0, pos = 0; i < outLen; i++, pos += step)
    {
        Uint32 at = pos >> 16;
        out[i] = in[at < inLen ? at : inLen - 1];
    }
    c = Mix_QuickLoad_RAW((Uint8 *)out, outLen * 2);
    if (!c)
    {
        free(out);
        return base;
    }
    Mix_VolumeChunk(c, k_effectVol);
    s_soundBank[which][freq] = c;
    return c;
}

void InitSound()
{
    if (Mix_OpenAudio(k_rate, AUDIO_S16SYS, 2, 1024) != 0)
        return;                                       /* no AHI: play on, silent */
    s_audioOpen = true;
    Mix_AllocateChannels(kNumSounds);

    for (int i = 0; i < kNumSounds; i++)
    {
        const char* path = QuickResourceName("snd", i+128, ".wav");
        if (!FileExists(path))
        {
            Error(path);
        }

        s_soundBank[i][0] = Mix_LoadWAV(path);
        if (s_soundBank[i][0])
            Mix_VolumeChunk(s_soundBank[i][0], k_effectVol);
    }
}

void ShutdownSound()
{
    if (!s_audioOpen)
        return;
    Mix_HaltChannel(-1);
    for (int i = 0; i < kNumSounds; i++)
    {
        for (int p = 0; p < k_maxPitches; p++)
        {
            Mix_Chunk* c = s_soundBank[i][p];
            if (!c)
                continue;
            if (p)
            {
                /* QuickLoad_RAW does not own its samples */
                Uint8* samples = c->abuf;
                Mix_FreeChunk(c);
                free(samples);
            }
            else
            {
                Mix_FreeChunk(c);
            }
            s_soundBank[i][p] = NULL;
        }
    }
    Mix_CloseAudio();
    s_audioOpen = false;
}

void PlayMono( int which )
{
    PlayStereoFrequency(2, which, 0);
}

void PlayStereo( int player, short which )
{
    PlayStereoFrequency(player, which, 0);
}

void PlayStereoFrequency( int player, short which, short freq )
{
    (void) player;                                    /* mono: no panning */

    if (soundOn && s_audioOpen && which >= 0 && which < kNumSounds)
    {
        Mix_Chunk* c = PitchedChunk(which, freq);
        if (c)
            Mix_PlayChannel(which, c, 0);
    }
}

void UpdateSound()
{
}

/* --------------------------------------------------------------- music */

#define k_songs 14

static struct module* s_module;
static struct replay* s_replay;
static char*          s_moduleFile;
static int*           s_mixBuf;
static int            s_mixOffset;
static int            s_mixSamples;
static volatile int   s_speed = 100;
static volatile int   s_gain = 1;
static volatile int   s_paused;
static MBoolean       s_musicFast = false;

/* In acsdl12's mixer process: stereo 16-bit in the machine's order. */
static void MusicHook(void* udata, Uint8* stream, int len)
{
    Sint16* out = (Sint16*) stream;
    int frames = len / 4;

    (void) udata;
    if (!s_replay || s_paused || !s_gain)
        return;                                       /* stream is silence */

    while (frames > 0)
    {
        int n, i;
        const int* in;

        if (s_mixSamples == 0)
        {
            s_mixOffset = 0;
            s_mixSamples = replay_get_audio(s_replay, s_mixBuf, 0, s_speed);
            if (s_mixSamples <= 0)
                return;
        }
        n = s_mixSamples < frames ? s_mixSamples : frames;
        in = &s_mixBuf[s_mixOffset * 2];
        for (i = 0; i < n * 2; i++)
        {
            int v = in[i];
            out[i] = (Sint16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        out += n * 2;
        s_mixOffset += n;
        s_mixSamples -= n;
        frames -= n;
    }
}

void EnableMusic( MBoolean on )
{
    s_gain = on ? 1 : 0;
}

void FastMusic( void )
{
    if (s_replay && !s_musicFast)
    {
        s_speed = 130;
        s_musicFast = true;
    }
}

void SlowMusic( void )
{
    if (s_replay && s_musicFast)
    {
        s_speed = 100;
        s_musicFast = false;
    }
}

void PauseMusic( void )
{
    if (s_replay)
    {
        s_paused++;
        s_musicPaused++;
    }
}

void ResumeMusic( void )
{
    if (s_replay)
    {
        s_paused--;
        s_musicPaused--;
    }
}

int GetCurrentMusic( void )
{
    return musicSelection;
}

static char* LoadWholeFile(const char* path, int* size)
{
    SDL_RWops* f = SDL_RWFromFile(path, "rb");
    char* bytes = NULL;
    int length;

    *size = 0;
    if (!f)
        return NULL;
    length = SDL_RWseek(f, 0, RW_SEEK_END);
    SDL_RWseek(f, 0, RW_SEEK_SET);
    if (length > 0 && length < 4 * 1024 * 1024)
    {
        bytes = (char*) malloc(length);
        if (bytes && SDL_RWread(f, bytes, length, 1) != 1)
        {
            free(bytes);
            bytes = NULL;
        }
    }
    SDL_RWclose(f);
    if (bytes)
        *size = length;
    return bytes;
}

void ChooseMusic( short which )
{
    // Kill existing song first, if any
    ShutdownMusic();

    musicSelection = -1;

    if (s_audioOpen && which >= 0 && which <= k_songs)
    {
        char message[64] = "";
        struct data d;
        const char* qrn = QuickResourceName("mod", which+128, ".mod");
        if (!FileExists(qrn))
        {
            qrn = QuickResourceName("mod", which+128, ".s3m");
        }
        if (!FileExists(qrn))
        {
            return;
        }

        s_moduleFile = LoadWholeFile(qrn, &d.length);
        d.buffer = s_moduleFile;
        if (!s_moduleFile)
            return;
        s_module = module_load(&d, message);
        if (!s_module || message[0])
        {
            if (s_module)
                dispose_module(s_module);
            s_module = NULL;
            free(s_moduleFile);
            s_moduleFile = NULL;
            return;
        }
        s_mixBuf = (int*) calloc(calculate_mix_buf_len(k_rate, 100), sizeof(int));
        s_replay = s_mixBuf ? new_replay(s_module, k_rate, 0) : NULL;
        if (!s_replay)
        {
            ShutdownMusic();
            return;
        }
        s_mixOffset = s_mixSamples = 0;
        s_speed = 100;
        s_musicFast = false;
        s_paused = 0;
        EnableMusic(musicOn);
        Mix_HookMusic(MusicHook, NULL);

        musicSelection = which;
        s_musicPaused  = 0;
    }
}

void ShutdownMusic()
{
    if (s_audioOpen)
        Mix_HookMusic(NULL, NULL);       /* the mixer is out of the hook now */
    if (s_replay)
        dispose_replay(s_replay);
    if (s_module)
        dispose_module(s_module);
    free(s_mixBuf);
    free(s_moduleFile);
    s_replay = NULL;
    s_module = NULL;
    s_mixBuf = NULL;
    s_moduleFile = NULL;
}
