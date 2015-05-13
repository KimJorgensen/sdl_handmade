#if !defined(SDL_HANDMADE_H)

struct sdl_offscreen_buffer
{
    // NOTE(casey): Pixels are alwasy 32-bits wide, Memory Order BB GG RR XX
    SDL_Texture *Texture;
    void *Memory;
    int Width;
    int Height;
    int Pitch;
    int BytesPerPixel;
};

struct sdl_window_dimension
{
    int Width;
    int Height;
};

struct sdl_sound_output
{
    int SamplesPerSecond;
    uint32 RunningSampleIndex;
    int BytesPerSample;
    uint32 SecondaryBufferSize;
    uint32 SafetyBytes;

    // TODO(casey): Should running sample index be in bytes as well
    // TODO(casey): Math gets simpler if we add a "bytes per second" field?
};

struct sdl_debug_time_marker
{
    uint32 QueuedAudioBytes;
    uint32 OutputByteCount;
    uint32 ExpectedBytesUntilFlip;
};

struct sdl_game_code
{
    void* GameCodeDLL;
    time_t DLLLastWriteTime;

    // IMPORTANT(casey): Either of the callbacks can be 0!  You must
    // check before calling.
    game_update_and_render *UpdateAndRender;
    game_get_sound_samples *GetSoundSamples;

    bool32 IsValid;
};

#define SDL_STATE_FILE_NAME_COUNT 4096
struct sdl_replay_buffer
{
    int FileHandle;
    void* MemoryMap;
    char FileName[SDL_STATE_FILE_NAME_COUNT];
    void *MemoryBlock;
};

struct sdl_state
{
    uint64 TotalSize;
    void *GameMemoryBlock;
    sdl_replay_buffer ReplayBuffers[4];

    int RecordingHandle;
    int InputRecordingIndex;

    int PlaybackHandle;
    int InputPlayingIndex;

    char EXEFileName[SDL_STATE_FILE_NAME_COUNT];
    char *OnePastLastEXEFileNameSlash;
};

#define SDL_HANDMADE_H
#endif
