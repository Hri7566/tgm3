/* DirectSound (DirectSoundCreate + IDirectSound / IDirectSoundBuffer) as a software mixer on SDL audio. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32.h"

#define A(i) (a[i])
#define DS_OK              0x00000000u
#define E_NOINTERFACE      0x80004002u
#define DSERR_INVALIDCALL  0x88780032u
#define DSERR_ALREADYINITIALIZED 0x88780082u
#define OUT_RATE 48000

struct shared_data { uint8_t *bytes; int refs; };   /* bytes: guest memory (Lock hands it out) */

/* Guest-visible COM objects, allocated in guest memory: the first field is the vtable pointer. */
struct dsound { gptr vtbl; int refs; };

struct dsbuf {
    gptr vtbl;
    int refs;
    bool primary;
    struct shared_data *data;
    uint32_t size, flags;
    uint8_t wfx[18];
    int channels, bits, rate, block;
    uint32_t freq;
    int32_t volume, pan;
    float gain_l, gain_r;
    bool playing, looping;
    uint64_t pos;                 /* in frames, 32.32 fixed point */
};

static SDL_AudioDeviceID dev;
static struct dsbuf *bufs[256];
static int nbufs;
static gptr ds_vtbl, buf_vtbl;

static void update_gain(struct dsbuf *b)
{
    float g = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    b->gain_l = g * (b->pan > 0 ? powf(10.0f, (float)-b->pan / 2000.0f) : 1.0f);
    b->gain_r = g * (b->pan < 0 ? powf(10.0f, (float)b->pan / 2000.0f) : 1.0f);
}

static inline float sample(const struct dsbuf *b, uint32_t frame, int ch)
{
    const uint8_t *p = b->data->bytes + (size_t)frame * (size_t)b->block;
    if (b->channels > 1) p += ch * (b->bits / 8);
    if (b->bits == 8) return ((float)*p - 128.0f) / 128.0f;
    int16_t s;
    memcpy(&s, p, 2);
    return (float)s / 32768.0f;
}

/* TGM3_AUDIO_DUMP=file.wav: write the mixed output (16-bit stereo) for checking without speakers. */
static FILE *dump;
static uint32_t dump_bytes;

static void dump_header(void)
{
    uint32_t riff = 36 + dump_bytes, fmt_len = 16, rate = OUT_RATE, avg = OUT_RATE * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(dump, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, dump); fwrite(&riff, 4, 1, dump); fwrite("WAVEfmt ", 1, 8, dump);
    fwrite(&fmt_len, 4, 1, dump); fwrite(&pcm, 2, 1, dump); fwrite(&ch, 2, 1, dump);
    fwrite(&rate, 4, 1, dump); fwrite(&avg, 4, 1, dump); fwrite(&align, 2, 1, dump); fwrite(&bits, 2, 1, dump);
    fwrite("data", 1, 4, dump); fwrite(&dump_bytes, 4, 1, dump);
    fseek(dump, 0, SEEK_END);
}

static void dump_audio(const float *out, int frames)
{
    int16_t pcm[4096];
    for (int i = 0; i < 2 * frames && i < 4096; i++) pcm[i] = (int16_t)(out[i] * 32767.0f);
    fwrite(pcm, 2, (size_t)(2 * frames), dump);
    dump_bytes += (uint32_t)(4 * frames);
    dump_header();
    fflush(dump);
}

static void SDLCALL mix(void *user, Uint8 *stream, int len)
{
    (void)user;
    float *out = (float *)stream;
    int frames = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    for (int i = 0; i < nbufs; i++) {
        struct dsbuf *b = bufs[i];
        if (!b->playing || b->primary || !b->block) continue;
        uint32_t nframes = b->size / (uint32_t)b->block;
        if (!nframes) continue;
        uint64_t step = ((uint64_t)(b->freq ? b->freq : (uint32_t)b->rate) << 32) / OUT_RATE;
        for (int f = 0; f < frames; f++) {
            uint32_t idx = (uint32_t)(b->pos >> 32);
            if (idx >= nframes) {
                if (!b->looping) { b->playing = false; b->pos = 0; break; }
                b->pos -= (uint64_t)nframes << 32;
                idx -= nframes;
            }
            float frac = (float)(uint32_t)b->pos / 4294967296.0f;
            uint32_t next = idx + 1 < nframes ? idx + 1 : (b->looping ? 0 : idx);
            float l = sample(b, idx, 0) * (1 - frac) + sample(b, next, 0) * frac;
            float r = b->channels > 1 ? sample(b, idx, 1) * (1 - frac) + sample(b, next, 1) * frac : l;
            out[2 * f] += l * b->gain_l;
            out[2 * f + 1] += r * b->gain_r;
            b->pos += step;
        }
    }
    for (int f = 0; f < 2 * frames; f++) out[f] = out[f] > 1 ? 1 : out[f] < -1 ? -1 : out[f];
    if (dump) dump_audio(out, frames);
}

static struct dsbuf *buf_of(uint32_t this) { return G2H(struct dsbuf *, this); }

static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static struct dsbuf *buf_new(void)
{
    struct dsbuf *b = G2H(struct dsbuf *, guest_alloc(sizeof *b));
    b->vtbl = buf_vtbl;
    b->refs = 1;
    lock();
    if (nbufs < 256) bufs[nbufs++] = b;
    unlock();
    return b;
}

static void set_format(struct dsbuf *b, gptr wfx)
{
    memcpy(b->wfx, G2H(void *, wfx), 18);
    b->channels = rd16(wfx + 2);
    b->rate = (int)rd32(wfx + 4);
    b->block = rd16(wfx + 12);
    b->bits = rd16(wfx + 14);
}

/* ---- IUnknown ---- */

static uint32_t unk_QueryInterface(uint32_t *a)
{
    uint32_t iid = rd32(A(1));
    /* IDirectSound(8), IDirectSoundBuffer(8): same object. 3D/notify interfaces: not provided. */
    if (iid == 0x279AFA83 || iid == 0xC50A7E93 || iid == 0x279AFA85 || iid == 0x6825A449) {
        G2H(struct dsbuf *, A(0))->refs++;
        wr32(A(2), A(0));
        return DS_OK;
    }
    LOG("dsound: QueryInterface for unsupported %08x", iid);
    wr32(A(2), 0);
    return E_NOINTERFACE;
}

static uint32_t unk_AddRef(uint32_t *a) { return (uint32_t)++G2H(struct dsound *, A(0))->refs; }
static uint32_t ds_Release(uint32_t *a) { return (uint32_t)--G2H(struct dsound *, A(0))->refs; }

static uint32_t buf_Release(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    int refs = --b->refs;
    if (refs > 0) return (uint32_t)refs;
    lock();
    for (int i = 0; i < nbufs; i++)
        if (bufs[i] == b) { bufs[i] = bufs[--nbufs]; break; }
    unlock();
    if (b->data && --b->data->refs == 0) { guest_free(H2G(b->data->bytes)); free(b->data); }
    guest_free(H2G(b));
    return 0;
}

/* ---- IDirectSound ---- */

static uint32_t ds_CreateSoundBuffer(uint32_t *a)
{
    gptr desc = A(1);
    uint32_t flags = rd32(desc + 4), bytes = rd32(desc + 8);
    gptr wfx = rd32(desc + 16);
    struct dsbuf *b = buf_new();
    b->flags = flags;
    b->primary = flags & 1;
    b->freq = 0;
    update_gain(b);
    if (!b->primary) {
        if (!wfx || !bytes) { wr32(A(2), 0); return DSERR_INVALIDCALL; }
        set_format(b, wfx);
        b->size = bytes;
        b->data = calloc(1, sizeof *b->data);
        b->data->bytes = G2H(uint8_t *, guest_alloc(bytes));
        b->data->refs = 1;
        if (g_verbose > 1) LOG("dsound: buffer %u bytes, %d Hz %d-bit %d ch", bytes, b->rate, b->bits, b->channels);
    }
    wr32(A(2), H2G(b));
    return DS_OK;
}

static uint32_t ds_DuplicateSoundBuffer(uint32_t *a)
{
    struct dsbuf *src = buf_of(A(1));
    if (src->primary) return DSERR_INVALIDCALL;
    struct dsbuf *b = buf_new();
    lock();
    b->flags = src->flags;
    b->data = src->data;
    b->data->refs++;
    b->size = src->size;
    memcpy(b->wfx, src->wfx, sizeof b->wfx);
    b->channels = src->channels;
    b->rate = src->rate;
    b->block = src->block;
    b->bits = src->bits;
    b->freq = src->freq;
    b->volume = src->volume;
    b->pan = src->pan;
    update_gain(b);
    unlock();
    wr32(A(2), H2G(b));
    return DS_OK;
}

static uint32_t ds_GetCaps(uint32_t *a)
{
    uint32_t size = rd32(A(1));
    memset(G2H(void *, A(1) + 4), 0, size > 4 ? size - 4 : 0);
    wr32(A(1) + 4, 0x0F00 | 0x0400);         /* secondary 8/16-bit mono/stereo, continuous rate */
    wr32(A(1) + 8, 100);
    wr32(A(1) + 12, 200000);
    return DS_OK;
}

static uint32_t ds_Ok(uint32_t *a) { (void)a; return DS_OK; }
static uint32_t ds_GetSpeakerConfig(uint32_t *a) { wr32(A(1), 4); return DS_OK; }   /* stereo */

/* ---- IDirectSoundBuffer ---- */

static uint32_t buf_GetCaps(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    wr32(A(1) + 4, b->flags);
    wr32(A(1) + 8, b->size);
    wr32(A(1) + 12, 0);
    wr32(A(1) + 16, 0);
    return DS_OK;
}

static void cursors(struct dsbuf *b, uint32_t *play, uint32_t *write)
{
    if (b->primary || !b->block) { *play = *write = 0; return; }
    lock();
    *play = (uint32_t)(b->pos >> 32) * (uint32_t)b->block % b->size;
    unlock();
    /* The write cursor runs ~one mix period ahead, like real hardware. */
    uint32_t lead = (uint32_t)((uint64_t)(b->freq ? b->freq : (uint32_t)b->rate) * 1024 / OUT_RATE) * (uint32_t)b->block;
    *write = (*play + lead) % b->size;
}

static uint32_t buf_GetCurrentPosition(uint32_t *a)
{
    uint32_t play, write;
    cursors(buf_of(A(0)), &play, &write);
    wr32(A(1), play);
    wr32(A(2), write);
    return DS_OK;
}

static uint32_t buf_GetFormat(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    if (A(1)) memcpy(G2H(void *, A(1)), b->wfx, A(2) < 18 ? A(2) : 18);
    wr32(A(3), 18);
    return DS_OK;
}

static uint32_t buf_GetVolume(uint32_t *a) { wr32(A(1), (uint32_t)buf_of(A(0))->volume); return DS_OK; }
static uint32_t buf_GetPan(uint32_t *a) { wr32(A(1), (uint32_t)buf_of(A(0))->pan); return DS_OK; }

static uint32_t buf_GetFrequency(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    wr32(A(1), b->freq ? b->freq : (uint32_t)b->rate);
    return DS_OK;
}

static uint32_t buf_GetStatus(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    lock();
    wr32(A(1), (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u));
    unlock();
    return DS_OK;
}

static uint32_t buf_Initialize(uint32_t *a) { (void)a; return DSERR_ALREADYINITIALIZED; }

static uint32_t buf_Lock(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    if (b->primary) return DSERR_INVALIDCALL;
    uint32_t off = A(1), bytes = A(2), flags = A(7);
    if (flags & 1) {                                  /* DSBLOCK_FROMWRITECURSOR */
        uint32_t play;
        cursors(b, &play, &off);
    }
    if (flags & 2) bytes = b->size;                   /* DSBLOCK_ENTIREBUFFER */
    if (off >= b->size || bytes > b->size) return DSERR_INVALIDCALL;
    uint32_t n1 = bytes < b->size - off ? bytes : b->size - off;
    wr32(A(3), H2G(b->data->bytes + off));
    wr32(A(4), n1);
    wr32(A(5), bytes > n1 ? H2G(b->data->bytes) : 0);
    wr32(A(6), bytes - n1);
    return DS_OK;
}

static uint32_t buf_Play(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    lock();
    b->playing = true;
    b->looping = A(3) & 1;
    unlock();
    return DS_OK;
}

static uint32_t buf_SetCurrentPosition(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    if (!b->block) return DS_OK;
    lock();
    b->pos = (uint64_t)(A(1) / (uint32_t)b->block) << 32;
    unlock();
    return DS_OK;
}

static uint32_t buf_SetFormat(uint32_t *a) { (void)a; return DS_OK; }

static uint32_t buf_SetVolume(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    lock();
    b->volume = (int32_t)A(1);
    update_gain(b);
    unlock();
    return DS_OK;
}

static uint32_t buf_SetPan(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    lock();
    b->pan = (int32_t)A(1);
    update_gain(b);
    unlock();
    return DS_OK;
}

static uint32_t buf_SetFrequency(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    lock();
    b->freq = A(1);
    unlock();
    return DS_OK;
}

static uint32_t buf_Stop(uint32_t *a)
{
    struct dsbuf *b = buf_of(A(0));
    lock();
    b->playing = false;
    unlock();
    return DS_OK;
}

static uint32_t buf_Unlock(uint32_t *a) { (void)a; return DS_OK; }
static uint32_t buf_Restore(uint32_t *a) { (void)a; return DS_OK; }

static gptr make_vtbl(const struct w32_export *methods, int n)
{
    gptr vt = guest_alloc((size_t)n * 4);
    for (int i = 0; i < n; i++) wr32(vt + 4u * i, w32_make_entry(methods[i].name, methods[i].nargs, methods[i].fn));
    return vt;
}

static void init_vtables(void)
{
    static const struct w32_export ds_methods[] = {
        { "IDirectSound::QueryInterface", 3, unk_QueryInterface, 0 },
        { "IDirectSound::AddRef", 1, unk_AddRef, 0 },
        { "IDirectSound::Release", 1, ds_Release, 0 },
        { "IDirectSound::CreateSoundBuffer", 4, ds_CreateSoundBuffer, 0 },
        { "IDirectSound::GetCaps", 2, ds_GetCaps, 0 },
        { "IDirectSound::DuplicateSoundBuffer", 3, ds_DuplicateSoundBuffer, 0 },
        { "IDirectSound::SetCooperativeLevel", 3, ds_Ok, 0 },
        { "IDirectSound::Compact", 1, ds_Ok, 0 },
        { "IDirectSound::GetSpeakerConfig", 2, ds_GetSpeakerConfig, 0 },
        { "IDirectSound::SetSpeakerConfig", 2, ds_Ok, 0 },
        { "IDirectSound::Initialize", 2, ds_Ok, 0 },
    };
    static const struct w32_export buf_methods[] = {
        { "IDirectSoundBuffer::QueryInterface", 3, unk_QueryInterface, 0 },
        { "IDirectSoundBuffer::AddRef", 1, unk_AddRef, 0 },
        { "IDirectSoundBuffer::Release", 1, buf_Release, 0 },
        { "IDirectSoundBuffer::GetCaps", 2, buf_GetCaps, 0 },
        { "IDirectSoundBuffer::GetCurrentPosition", 3, buf_GetCurrentPosition, 0 },
        { "IDirectSoundBuffer::GetFormat", 4, buf_GetFormat, 0 },
        { "IDirectSoundBuffer::GetVolume", 2, buf_GetVolume, 0 },
        { "IDirectSoundBuffer::GetPan", 2, buf_GetPan, 0 },
        { "IDirectSoundBuffer::GetFrequency", 2, buf_GetFrequency, 0 },
        { "IDirectSoundBuffer::GetStatus", 2, buf_GetStatus, 0 },
        { "IDirectSoundBuffer::Initialize", 3, buf_Initialize, 0 },
        { "IDirectSoundBuffer::Lock", 8, buf_Lock, 0 },
        { "IDirectSoundBuffer::Play", 4, buf_Play, 0 },
        { "IDirectSoundBuffer::SetCurrentPosition", 2, buf_SetCurrentPosition, 0 },
        { "IDirectSoundBuffer::SetFormat", 2, buf_SetFormat, 0 },
        { "IDirectSoundBuffer::SetVolume", 2, buf_SetVolume, 0 },
        { "IDirectSoundBuffer::SetPan", 2, buf_SetPan, 0 },
        { "IDirectSoundBuffer::SetFrequency", 2, buf_SetFrequency, 0 },
        { "IDirectSoundBuffer::Stop", 1, buf_Stop, 0 },
        { "IDirectSoundBuffer::Unlock", 5, buf_Unlock, 0 },
        { "IDirectSoundBuffer::Restore", 1, buf_Restore, 0 },
    };
    ds_vtbl = make_vtbl(ds_methods, sizeof ds_methods / sizeof *ds_methods);
    buf_vtbl = make_vtbl(buf_methods, sizeof buf_methods / sizeof *buf_methods);
}

static uint32_t DirectSoundCreate(uint32_t *a)
{
    if (!ds_vtbl) init_vtables();
    if (!dev) {
        SDL_AudioSpec want = { .freq = OUT_RATE, .format = AUDIO_F32SYS, .channels = 2, .samples = 512, .callback = mix };
        SDL_AudioSpec have;
        if (getenv("TGM3_AUDIO_DUMP") && (dump = fopen(getenv("TGM3_AUDIO_DUMP"), "wb"))) dump_header();
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!dev) WARN("no audio: %s", SDL_GetError());
        else SDL_PauseAudioDevice(dev, 0);
    }
    struct dsound *ds = G2H(struct dsound *, guest_alloc(sizeof *ds));
    ds->vtbl = ds_vtbl;
    ds->refs = 1;
    wr32(A(1), H2G(ds));
    LOG("DirectSoundCreate -> %p", (void *)ds);
    return DS_OK;
}

const struct w32_export dsound_exports[] = {
    { "DirectSoundCreate", 3, DirectSoundCreate, 1 },
    { NULL, 0, NULL, 0 },
};
