/* ========================================================================
   $File: $
   $Date: $
   $Revision: $
   $Creator: Casey Muratori $
   $Notice: (C) Copyright 2014 by Molly Rocket, Inc. All Rights Reserved. $
   ======================================================================== */

/*
  TODO(casey):  THIS IS NOT A FINAL PLATFORM LAYER!!!

  - Make the right calls so Windows doesn't think we're "still loading" for a bit after we actually start
  - Saved game locations
  - Getting a handle to our own executable file
  - Asset loading path
  - Threading (launch a thread)
  - Raw Input (support for multiple keyboards)
  - ClipCursor() (for multimonitor support)
  - QueryCancelAutoplay
  - WM_ACTIVATEAPP (for when we are not the active application)
  - Blit speed improvements (BitBlt)
  - Hardware acceleration (OpenGL or Direct3D or BOTH??)
  - GetKeyboardLayout (for French keyboards, international WASD support)
  - ChangeDisplaySettings option if we detect slow fullscreen blit??

   Just a partial list of stuff!!
*/

#include "handmade_platform.h"

#include <SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <x86intrin.h>

#include "sdl_handmade.h"

// NOTE: MAP_ANONYMOUS is not defined on Mac OS X and some other UNIX systems.
// On the vast majority of those systems, one can use MAP_ANON instead.
// Huge thanks to Adam Rosenfield for investigating this, and suggesting this
// workaround:
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

// TODO(casey): This is a global for now.
global_variable bool32 GlobalRunning;
global_variable bool32 GlobalPause;
global_variable sdl_offscreen_buffer GlobalBackbuffer;
global_variable uint64 GlobalPerfCountFrequency;
global_variable bool32 DEBUGGlobalShowCursor;

#define MAX_CONTROLLERS 4
#define CONTROLLER_AXIS_LEFT_DEADZONE 7849
global_variable SDL_GameController *ControllerHandles[MAX_CONTROLLERS];
global_variable SDL_Haptic *RumbleHandles[MAX_CONTROLLERS];

internal void
CatStrings(size_t SourceACount, char *SourceA,
           size_t SourceBCount, char *SourceB,
           size_t DestCount, char *Dest)
{
    // TODO(casey): Dest bounds checking!

    for(int Index = 0;
        Index < SourceACount;
        ++Index)
    {
        *Dest++ = *SourceA++;
    }

    for(int Index = 0;
        Index < SourceBCount;
        ++Index)
    {
        *Dest++ = *SourceB++;
    }

    *Dest++ = 0;
}

internal void
SDLGetEXEFileName(sdl_state *State)
{
    // NOTE(casey): Never use MAX_PATH in code that is user-facing, because it
    // can be dangerous and lead to bad results.
    memset(State->EXEFileName, 0, sizeof(State->EXEFileName));

    ssize_t SizeOfFilename = readlink("/proc/self/exe", State->EXEFileName, sizeof(State->EXEFileName)-1);
    State->OnePastLastEXEFileNameSlash = State->EXEFileName;
    for(char *Scan = State->EXEFileName;
        *Scan;
        ++Scan)
    {
        if(*Scan == '/')
        {
            State->OnePastLastEXEFileNameSlash = Scan + 1;
        }
    }
}

internal int
StringLength(char *String)
{
    int Count = 0;
    while(*String++)
    {
        ++Count;
    }
    return(Count);
}

internal void
SDLBuildEXEPathFileName(sdl_state *State, char *FileName,
                          int DestCount, char *Dest)
{
    CatStrings(State->OnePastLastEXEFileNameSlash - State->EXEFileName, State->EXEFileName,
               StringLength(FileName), FileName,
               DestCount, Dest);
}

DEBUG_PLATFORM_FREE_FILE_MEMORY(DEBUGPlatformFreeFileMemory)
{
    if(Memory)
    {
        free(Memory);
    }
}

DEBUG_PLATFORM_READ_ENTIRE_FILE(DEBUGPlatformReadEntireFile)
{
    debug_read_file_result Result = {};

    int FileHandle = open(Filename, O_RDONLY);
    if(FileHandle == -1)
    {
        return Result;
    }

    struct stat FileStatus;
    if(fstat(FileHandle, &FileStatus) == -1)
    {
        close(FileHandle);
        return Result;
    }
    Result.ContentsSize = SafeTruncateUInt64(FileStatus.st_size);

    Result.Contents = malloc(Result.ContentsSize);
    if(!Result.Contents)
    {
        close(FileHandle);
        Result.ContentsSize = 0;
        return Result;
    }


    uint32 BytesToRead = Result.ContentsSize;
    uint8 *NextByteLocation = (uint8*)Result.Contents;
    while (BytesToRead)
    {
        ssize_t BytesRead = read(FileHandle, NextByteLocation, BytesToRead);
        if (BytesRead == -1)
        {
            free(Result.Contents);
            Result.Contents = 0;
            Result.ContentsSize = 0;
            close(FileHandle);
            return Result;
        }
        BytesToRead -= BytesRead;
        NextByteLocation += BytesRead;
    }

    close(FileHandle);
    return(Result);
}

DEBUG_PLATFORM_WRITE_ENTIRE_FILE(DEBUGPlatformWriteEntireFile)
{
    int FileHandle = open(Filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (!FileHandle)
        return false;

    uint32 BytesToWrite = MemorySize;
    uint8 *NextByteLocation = (uint8*)Memory;
    while (BytesToWrite)
    {
        ssize_t BytesWritten = write(FileHandle, NextByteLocation, BytesToWrite);
        if (BytesWritten == -1)
        {
            close(FileHandle);
            return false;
        }
        BytesToWrite -= BytesWritten;
        NextByteLocation += BytesWritten;
    }

    close(FileHandle);

    return true;
}

inline time_t
SDLGetLastWriteTime(char *Filename)
{
    time_t LastWriteTime = 0;

    struct stat FileStatus;
    if(stat(Filename, &FileStatus) == 0)
    {
        LastWriteTime = FileStatus.st_mtime;
    }

    return(LastWriteTime);
}

internal sdl_game_code
SDLLoadGameCode(char *SourceDLLName)
{
    sdl_game_code Result = {};

    Result.DLLLastWriteTime = SDLGetLastWriteTime(SourceDLLName);

    if(Result.DLLLastWriteTime)
    {
        Result.GameCodeDLL = dlopen(SourceDLLName, RTLD_LAZY);
        if(Result.GameCodeDLL)
        {
            Result.UpdateAndRender = (game_update_and_render *)
                dlsym(Result.GameCodeDLL, "GameUpdateAndRender");

            Result.GetSoundSamples = (game_get_sound_samples *)
                dlsym(Result.GameCodeDLL, "GameGetSoundSamples");

            Result.IsValid = (Result.UpdateAndRender &&
                              Result.GetSoundSamples);
        }
        else
        {
            puts(dlerror());
        }
    }

    if(!Result.IsValid)
    {
        Result.UpdateAndRender = 0;
        Result.GetSoundSamples = 0;
    }

    return(Result);
}

internal void
SDLUnloadGameCode(sdl_game_code *GameCode)
{
    if(GameCode->GameCodeDLL)
    {
        dlclose(GameCode->GameCodeDLL);
        GameCode->GameCodeDLL = 0;
    }

    GameCode->IsValid = false;
    GameCode->UpdateAndRender = 0;
    GameCode->GetSoundSamples = 0;
}

internal void
SDLOpenGameControllers()
{
    int MaxJoysticks = SDL_NumJoysticks();
    int ControllerIndex = 0;
    for(int JoystickIndex=0; JoystickIndex < MaxJoysticks; ++JoystickIndex)
    {
        if (!SDL_IsGameController(JoystickIndex))
        {
            continue;
        }
        if (ControllerIndex >= MAX_CONTROLLERS)
        {
            break;
        }
        ControllerHandles[ControllerIndex] = SDL_GameControllerOpen(JoystickIndex);
        SDL_Joystick *JoystickHandle = SDL_GameControllerGetJoystick(ControllerHandles[ControllerIndex]);
        RumbleHandles[ControllerIndex] = SDL_HapticOpenFromJoystick(JoystickHandle);
        if (SDL_HapticRumbleInit(RumbleHandles[ControllerIndex]) != 0)
        {
            SDL_HapticClose(RumbleHandles[ControllerIndex]);
            RumbleHandles[ControllerIndex] = 0;
        }

        ControllerIndex++;
    }
}

internal void
SDLCloseGameControllers()
{
    for(int ControllerIndex = 0; ControllerIndex < MAX_CONTROLLERS; ++ControllerIndex)
    {
        if (ControllerHandles[ControllerIndex])
        {
            if (RumbleHandles[ControllerIndex])
                SDL_HapticClose(RumbleHandles[ControllerIndex]);
            SDL_GameControllerClose(ControllerHandles[ControllerIndex]);
        }
    }
}

internal void
SDLInitAudio(int32 SamplesPerSecond, int32 BufferSize)
{
    SDL_AudioSpec AudioSettings = {};

    AudioSettings.freq = SamplesPerSecond;
    AudioSettings.format = AUDIO_S16LSB;
    AudioSettings.channels = 2;
    AudioSettings.samples = 512;

    SDL_OpenAudio(&AudioSettings, 0);

    printf("Initialised an Audio device at frequency %d Hz, %d Channels, buffer size %d\n",
           AudioSettings.freq, AudioSettings.channels, AudioSettings.size);

    if (AudioSettings.format != AUDIO_S16LSB)
    {
        printf("Oops! We didn't get AUDIO_S16LSB as our sample format!\n");
        SDL_CloseAudio();
    }
}

sdl_window_dimension
SDLGetWindowDimension(SDL_Window *Window)
{
    sdl_window_dimension Result;

    SDL_GetWindowSize(Window, &Result.Width, &Result.Height);

    return(Result);
}

internal void
SDLResizeTexture(sdl_offscreen_buffer *Buffer, SDL_Renderer *Renderer, int Width, int Height)
{
    // TODO(casey): Bulletproof this.
    // Maybe don't free first, free after, then free first if that fails.

    if(Buffer->Memory)
    {
        munmap(Buffer->Memory,
               (Buffer->Width*Buffer->Height)*Buffer->BytesPerPixel);
    }

    Buffer->Width = Width;
    Buffer->Height = Height;

    int BytesPerPixel = 4;
    Buffer->BytesPerPixel = BytesPerPixel;

    if (Buffer->Texture)
    {
        SDL_DestroyTexture(Buffer->Texture);
    }
    Buffer->Texture = SDL_CreateTexture(Renderer,
                                        SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        Buffer->Width,
                                        Buffer->Height);

    Buffer->Pitch = Align16(Width*BytesPerPixel);
    int BitmapMemorySize = (Buffer->Pitch*Buffer->Height);
    Buffer->Memory = mmap(0,
                          BitmapMemorySize,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1,
                          0);

    // TODO(casey): Probably clear this to black
}

internal void
SDLDisplayBufferInWindow(sdl_offscreen_buffer *Buffer,
                         SDL_Renderer *Renderer, int WindowWidth, int WindowHeight)
{
    // TODO(casey): Centering / black bars?

    SDL_UpdateTexture(Buffer->Texture,
                  0,
                  Buffer->Memory,
                  Buffer->Pitch);

    if((WindowWidth >= Buffer->Width*2) &&
       (WindowHeight >= Buffer->Height*2))
    {
        SDL_Rect SrcRect = {0, 0, Buffer->Width, Buffer->Height};
        SDL_Rect DestRect = {0, 0, 2*Buffer->Width, 2*Buffer->Height};
        SDL_RenderCopy(Renderer,
                       Buffer->Texture,
                       &SrcRect,
                       &DestRect);
    }
    else
    {
#if 0
        int OffsetX = 10;
        int OffsetY = 10;

        SDL_SetRenderDrawColor(Renderer, 0, 0, 0, 255);
        SDL_Rect BlackRects[4] = {
            {0, 0, WindowWidth, OffsetY},
            {0, OffsetY + Buffer->Height, WindowWidth, WindowHeight},
            {0, 0, OffsetX, WindowHeight},
            {OffsetX + Buffer->Width, 0, WindowWidth, WindowHeight}
        };
        SDL_RenderFillRects(Renderer, BlackRects, ArrayCount(BlackRects));
#else
        int OffsetX = 0;
        int OffsetY = 0;
#endif

        // NOTE(casey): For prototyping purposes, we're going to always blit
        // 1-to-1 pixels to make sure we don't introduce artifacts with
        // stretching while we are learning to code the renderer!
        SDL_Rect SrcRect = {0, 0, Buffer->Width, Buffer->Height};
        SDL_Rect DestRect = {OffsetX, OffsetY, Buffer->Width, Buffer->Height};
        SDL_RenderCopy(Renderer,
                       Buffer->Texture,
                       &SrcRect,
                       &DestRect);
    }

    SDL_RenderPresent(Renderer);
}

internal void
SDLClearBuffer(sdl_sound_output *SoundOutput)
{
    SDL_ClearQueuedAudio(1);
}

internal void
SDLFillSoundBuffer(sdl_sound_output *SoundOutput, int BytesToWrite,
                   game_sound_output_buffer *SoundBuffer)
{
    SDL_QueueAudio(1, SoundBuffer->Samples, BytesToWrite);
}

internal void
SDLProcessKeyboardEvent(game_button_state *NewState, bool32 IsDown)
{
    if(NewState->EndedDown != IsDown)
    {
        NewState->EndedDown = IsDown;
        ++NewState->HalfTransitionCount;
    }
}

internal void
SDLProcessGameControllerButton(game_button_state *OldState, bool Value,
                               game_button_state *NewState)
{
    NewState->EndedDown = Value;
    NewState->HalfTransitionCount = (OldState->EndedDown != NewState->EndedDown) ? 1 : 0;
}

internal real32
SDLProcessGameControllerAxisValue(int16 Value, int16 DeadZoneThreshold)
{
    real32 Result = 0;

    if(Value < -DeadZoneThreshold)
    {
        Result = (real32)((Value + DeadZoneThreshold) / (32768.0f - DeadZoneThreshold));
    }
    else if(Value > DeadZoneThreshold)
    {
        Result = (real32)((Value - DeadZoneThreshold) / (32767.0f - DeadZoneThreshold));
    }

    return(Result);
}

internal void
SDLGetInputFileLocation(sdl_state *State, bool32 InputStream,
                          int SlotIndex, int DestCount, char *Dest)
{
    char Temp[64];
    sprintf(Temp, "loop_edit_%d_%s.hmi", SlotIndex, InputStream ? "input" : "state");
    SDLBuildEXEPathFileName(State, Temp, DestCount, Dest);
}

internal sdl_replay_buffer *
SDLGetReplayBuffer(sdl_state *State, int unsigned Index)
{
    Assert(Index > 0);
    Assert(Index < ArrayCount(State->ReplayBuffers));
    sdl_replay_buffer *Result = &State->ReplayBuffers[Index];
    return(Result);
}

internal void
SDLBeginRecordingInput(sdl_state *State, int InputRecordingIndex)
{
    sdl_replay_buffer *ReplayBuffer = SDLGetReplayBuffer(State, InputRecordingIndex);
    if(ReplayBuffer->MemoryBlock)
    {
        State->InputRecordingIndex = InputRecordingIndex;

        char FileName[SDL_STATE_FILE_NAME_COUNT];
        SDLGetInputFileLocation(State, true, InputRecordingIndex, sizeof(FileName), FileName);
        State->RecordingHandle = open(FileName, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

#if 0
        lseek(State->RecordingHandle, State->TotalSize, SEEK_SET);
#endif

        memcpy(ReplayBuffer->MemoryBlock, State->GameMemoryBlock, State->TotalSize);
    }
}

internal void
SDLEndRecordingInput(sdl_state *State)
{
    close(State->RecordingHandle);
    State->InputRecordingIndex = 0;
}

internal void
SDLBeginInputPlayBack(sdl_state *State, int InputPlayingIndex)
{
    sdl_replay_buffer *ReplayBuffer = SDLGetReplayBuffer(State, InputPlayingIndex);
    if(ReplayBuffer->MemoryBlock)
    {
        State->InputPlayingIndex = InputPlayingIndex;

        char FileName[SDL_STATE_FILE_NAME_COUNT];
        SDLGetInputFileLocation(State, true, InputPlayingIndex, sizeof(FileName), FileName);
        State->PlaybackHandle = open(FileName, O_RDONLY);

#if 0
        lseek(State->PlaybackHandle, State->TotalSize, SEEK_SET);
#endif

        memcpy(State->GameMemoryBlock, ReplayBuffer->MemoryBlock, State->TotalSize);
    }
}

internal void
SDLEndInputPlayBack(sdl_state *State)
{
    close(State->PlaybackHandle);
    State->InputPlayingIndex = 0;
}

internal void
SDLRecordInput(sdl_state *State, game_input *NewInput)
{
    ssize_t BytesWritten = write(State->RecordingHandle, NewInput, sizeof(*NewInput));
}

internal void
SDLPlayBackInput(sdl_state *State, game_input *NewInput)
{
    ssize_t BytesRead = read(State->PlaybackHandle, NewInput, sizeof(*NewInput));
    if(BytesRead == 0)
    {
        // NOTE(casey): We've hit the end of the stream, go back to the beginning
        int PlayingIndex = State->InputPlayingIndex;
        SDLEndInputPlayBack(State);
        SDLBeginInputPlayBack(State, PlayingIndex);
        read(State->PlaybackHandle, NewInput, sizeof(*NewInput));
    }
}

internal void
SDLToggleFullscreen(SDL_Window *Window)
{
    uint32 Flags = SDL_GetWindowFlags(Window);
    if(Flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
    {
        SDL_SetWindowFullscreen(Window, 0);
    }
    else
    {
        SDL_SetWindowFullscreen(Window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
}

internal void
SDLProcessPendingEvents(sdl_state *State, game_controller_input *KeyboardController)
{
    SDL_Event Event;
    while(SDL_PollEvent(&Event))
    {
        switch(Event.type)
        {
            case SDL_QUIT:
            {
                GlobalRunning = false;
            } break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                SDL_Keycode KeyCode = Event.key.keysym.sym;
                bool IsDown = (Event.key.state == SDL_PRESSED);

                // NOTE: In the windows version, we used "if (IsDown != WasDown)"
                // to detect key repeats. SDL has the 'repeat' value, though,
                // which we'll use.
                if (Event.key.repeat == 0)
                {
                    if(KeyCode == SDLK_w)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->MoveUp, IsDown);
                    }
                    else if(KeyCode == SDLK_a)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->MoveLeft, IsDown);
                    }
                    else if(KeyCode == SDLK_s)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->MoveDown, IsDown);
                    }
                    else if(KeyCode == SDLK_d)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->MoveRight, IsDown);
                    }
                    else if(KeyCode == SDLK_q)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->LeftShoulder, IsDown);
                    }
                    else if(KeyCode == SDLK_e)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->RightShoulder, IsDown);
                    }
                    else if(KeyCode == SDLK_UP)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->ActionUp, IsDown);
                    }
                    else if(KeyCode == SDLK_LEFT)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->ActionLeft, IsDown);
                    }
                    else if(KeyCode == SDLK_DOWN)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->ActionDown, IsDown);
                    }
                    else if(KeyCode == SDLK_RIGHT)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->ActionRight, IsDown);
                    }
                    else if(KeyCode == SDLK_ESCAPE)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->Back, IsDown);
                    }
                    else if(KeyCode == SDLK_SPACE)
                    {
                        SDLProcessKeyboardEvent(&KeyboardController->Start, IsDown);
                    }
#if HANDMADE_INTERNAL
                    else if(KeyCode == SDLK_p)
                    {
                        if(IsDown)
                        {
                            GlobalPause = !GlobalPause;
                        }
                    }
                    else if(KeyCode == SDLK_l)
                    {
                        if(IsDown)
                        {
                            if(State->InputPlayingIndex == 0)
                            {
                                if(State->InputRecordingIndex == 0)
                                {
                                    SDLBeginRecordingInput(State, 1);
                                }
                                else
                                {
                                    SDLEndRecordingInput(State);
                                    SDLBeginInputPlayBack(State, 1);
                                }
                            }
                            else
                            {
                                SDLEndInputPlayBack(State);
                            }
                        }
                    }
#endif
                    if(IsDown)
                    {
                        bool AltKeyWasDown = (Event.key.keysym.mod & KMOD_ALT);
                        if (KeyCode == SDLK_F4 && AltKeyWasDown)
                        {
                            GlobalRunning = false;
                        }
                        if((KeyCode == SDLK_RETURN) && AltKeyWasDown)
                        {
                            SDL_Window *Window = SDL_GetWindowFromID(Event.window.windowID);
                            if(Window)
                            {
                                SDLToggleFullscreen(Window);
                            }
                        }
                    }
                }

            } break;

            case SDL_WINDOWEVENT:
            {
                switch(Event.window.event)
                {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    {
                        SDL_Window *Window = SDL_GetWindowFromID(Event.window.windowID);
                        SDL_Renderer *Renderer = SDL_GetRenderer(Window);
                        printf("SDL_WINDOWEVENT_SIZE_CHANGED (%d, %d)\n", Event.window.data1, Event.window.data2);
                    } break;

                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                    {
                        printf("SDL_WINDOWEVENT_FOCUS_GAINED\n");
                    } break;

                    case SDL_WINDOWEVENT_EXPOSED:
                    {
                        SDL_Window *Window = SDL_GetWindowFromID(Event.window.windowID);
                        SDL_Renderer *Renderer = SDL_GetRenderer(Window);
                        sdl_window_dimension Dimension = SDLGetWindowDimension(Window);
                        SDLDisplayBufferInWindow(&GlobalBackbuffer, Renderer,
                                                 Dimension.Width, Dimension.Height);

                    } break;
                }
            } break;
        }
    }
}

inline uint64
SDLGetWallClock(void)
{
    uint64 Result = SDL_GetPerformanceCounter();
    return(Result);
}

internal real32
SDLGetSecondsElapsed(uint64 Start, uint64 End)
{
    real32 Result = ((real32)(End - Start) /
                     (real32)GlobalPerfCountFrequency);
    return(Result);
}

internal void
HandleDebugCycleCounters(game_memory *Memory)
{
#if HANDMADE_INTERNAL
    printf("DEBUG CYCLE COUNTS:\n");
    for(int CounterIndex = 0;
        CounterIndex < ArrayCount(Memory->Counters);
        ++CounterIndex)
    {
        debug_cycle_counter *Counter = Memory->Counters + CounterIndex;

        if(Counter->HitCount)
        {
            printf("  %d: %lucy %uh %lucy/h\n",
                   CounterIndex,
                   Counter->CycleCount,
                   Counter->HitCount,
                   Counter->CycleCount / Counter->HitCount);
            Counter->HitCount = 0;
            Counter->CycleCount = 0;
        }
    }
#endif
}

#if 0

internal void
SDLDebugDrawVertical(sdl_offscreen_buffer *Backbuffer,
                       int X, int Top, int Bottom, uint32 Color)
{
    if(Top <= 0)
    {
        Top = 0;
    }

    if(Bottom > Backbuffer->Height)
    {
        Bottom = Backbuffer->Height;
    }

    if((X >= 0) && (X < Backbuffer->Width))
    {
        uint8 *Pixel = ((uint8 *)Backbuffer->Memory +
                        X*Backbuffer->BytesPerPixel +
                        Top*Backbuffer->Pitch);
        for(int Y = Top;
            Y < Bottom;
            ++Y)
        {
            *(uint32 *)Pixel = Color;
            Pixel += Backbuffer->Pitch;
        }
    }
}

inline void
SDLDrawSoundBufferMarker(sdl_offscreen_buffer *Backbuffer,
                           sdl_sound_output *SoundOutput,
                           real32 C, int PadX, int Top, int Bottom,
                           uint32 Value, uint32 Color)
{
    real32 XReal32 = (C * (real32)Value);
    int X = PadX + (int)XReal32;
    SDLDebugDrawVertical(Backbuffer, X, Top, Bottom, Color);
}

internal void
SDLDebugSyncDisplay(sdl_offscreen_buffer *Backbuffer,
                      int MarkerCount, sdl_debug_time_marker *Markers,
                      int CurrentMarkerIndex,
                      sdl_sound_output *SoundOutput, real32 TargetSecondsPerFrame)
{
    int PadX = 16;
    int PadY = 16;

    int LineHeight = 64;

    real32 C = (real32)(Backbuffer->Width - 2*PadX) / (real32)SoundOutput->SecondaryBufferSize;
    for(int MarkerIndex = 0;
        MarkerIndex < MarkerCount;
        ++MarkerIndex)
    {
        sdl_debug_time_marker *ThisMarker = &Markers[MarkerIndex];

        uint32 WriteColor = 0x000000FF;
        uint32 QueuedAudioColor = 0x00FF0000;
        uint32 ExpectedFlipColor = 0x0000FF00;

        int Top = PadY;
        int Bottom = PadY + LineHeight;
        if(MarkerIndex == CurrentMarkerIndex)
        {
            Top += LineHeight+PadY;
            Bottom += LineHeight+PadY;

            int FirstTop = Top;

            SDLDrawSoundBufferMarker(Backbuffer, SoundOutput, C, MarkerIndex*30+PadX, Top, Bottom, ThisMarker->OutputByteCount, WriteColor);

            Top += LineHeight+PadY;
            Bottom += LineHeight+PadY;

            SDLDrawSoundBufferMarker(Backbuffer, SoundOutput, C, MarkerIndex*30+PadX, Top, Bottom, ThisMarker->QueuedAudioBytes, QueuedAudioColor);

            Top += LineHeight+PadY;
            Bottom += LineHeight+PadY;

            SDLDrawSoundBufferMarker(Backbuffer, SoundOutput, C, MarkerIndex*30+PadX, FirstTop, Bottom, ThisMarker->ExpectedBytesUntilFlip, ExpectedFlipColor);
        }

        SDLDrawSoundBufferMarker(Backbuffer, SoundOutput, C, MarkerIndex*30+PadX, Top, Bottom, ThisMarker->OutputByteCount, WriteColor);
        SDLDrawSoundBufferMarker(Backbuffer, SoundOutput, C, MarkerIndex*30+PadX, Top, Bottom, ThisMarker->QueuedAudioBytes, QueuedAudioColor);
        SDLDrawSoundBufferMarker(Backbuffer, SoundOutput, C, MarkerIndex*30+PadX, Top, Bottom, ThisMarker->ExpectedBytesUntilFlip, ExpectedFlipColor);
    }
}

#endif

struct platform_work_queue_entry
{
    platform_work_queue_callback *Callback;
    void *Data;
};

struct platform_work_queue
{
    unsigned int volatile CompletionGoal;
    unsigned int volatile CompletionCount;

    unsigned int volatile NextEntryToWrite;
    unsigned int volatile NextEntryToRead;
    SDL_sem *SemaphoreHandle;

    platform_work_queue_entry Entries[256];
};

internal void
SDLAddEntry(platform_work_queue *Queue, platform_work_queue_callback *Callback, void *Data)
{
    // TODO(casey): Switch to InterlockedCompareExchange eventually
    // so that any thread can add?
    uint32 NewNextEntryToWrite = (Queue->NextEntryToWrite + 1) % ArrayCount(Queue->Entries);
    Assert(NewNextEntryToWrite != Queue->NextEntryToRead);
    platform_work_queue_entry *Entry = Queue->Entries + Queue->NextEntryToWrite;
    Entry->Callback = Callback;
    Entry->Data = Data;
    ++Queue->CompletionGoal;
    SDL_CompilerBarrier();
    Queue->NextEntryToWrite = NewNextEntryToWrite;
    SDL_SemPost(Queue->SemaphoreHandle);
}

internal bool32
SDLDoNextWorkQueueEntry(platform_work_queue *Queue)
{
    bool32 WeShouldSleep = false;

    uint32 OriginalNextEntryToRead = Queue->NextEntryToRead;
    uint32 NewNextEntryToRead = (OriginalNextEntryToRead + 1) % ArrayCount(Queue->Entries);
    if(OriginalNextEntryToRead != Queue->NextEntryToWrite)
    {
        SDL_bool WasSet = SDL_AtomicCAS((SDL_atomic_t *)&Queue->NextEntryToRead,
                                        OriginalNextEntryToRead,
                                        NewNextEntryToRead);
        if(WasSet)
        {
            platform_work_queue_entry Entry = Queue->Entries[OriginalNextEntryToRead];
            Entry.Callback(Queue, Entry.Data);
            SDL_AtomicIncRef((SDL_atomic_t *)&Queue->CompletionCount);
        }
    }
    else
    {
        WeShouldSleep = true;
    }

    return(WeShouldSleep);
}

internal void
SDLCompleteAllWork(platform_work_queue *Queue)
{
    while(Queue->CompletionGoal != Queue->CompletionCount)
    {
        SDLDoNextWorkQueueEntry(Queue);
    }

    Queue->CompletionGoal = 0;
    Queue->CompletionCount = 0;
}

int
ThreadProc(void *Parameter)
{
    platform_work_queue *Queue = (platform_work_queue *)Parameter;

    for(;;)
    {
        if(SDLDoNextWorkQueueEntry(Queue))
        {
            SDL_SemWait(Queue->SemaphoreHandle);
        }
    }

//    return(0);
}

internal PLATFORM_WORK_QUEUE_CALLBACK(DoWorkerWork)
{
    printf("Thread %lu: %s\n", SDL_ThreadID(), (char *)Data);
}

internal void
SDLMakeQueue(platform_work_queue *Queue, uint32 ThreadCount)
{
    Queue->CompletionGoal = 0;
    Queue->CompletionCount = 0;

    Queue->NextEntryToWrite = 0;
    Queue->NextEntryToRead = 0;

    uint32 InitialCount = 0;
    Queue->SemaphoreHandle = SDL_CreateSemaphore(InitialCount);

    for(uint32 ThreadIndex = 0;
        ThreadIndex < ThreadCount;
        ++ThreadIndex)
    {
        SDL_Thread *ThreadHandle = SDL_CreateThread(ThreadProc, 0, Queue);
        SDL_DetachThread(ThreadHandle);
    }
}

struct sdl_platform_file_handle
{
    platform_file_handle H;
    int SDLHandle;
};

internal PLATFORM_GET_ALL_FILE_OF_TYPE_BEGIN(SDLGetAllFilesOfTypeBegin)
{
    platform_file_group FileGroup = {};

    char FilePattern[64];
    sprintf(FilePattern, "*.%s", Type);

    DIR *Dir = opendir(".");
    struct dirent *DirEntry;

    if(Dir)
    {
        while((DirEntry = readdir(Dir)) != NULL)
        {
            char *filename = DirEntry->d_name;

            printf("%s\n", filename);

            if(fnmatch(FilePattern, DirEntry->d_name, 0) == 0)
            {
                // TODO: Store filename
                FileGroup.FileCount++;
            }
        }

        closedir(Dir);
    }

    return(FileGroup);
}

internal PLATFORM_GET_ALL_FILE_OF_TYPE_END(SDLGetAllFilesOfTypeEnd)
{
}

internal PLATFORM_OPEN_FILE(SDLOpenFile)
{
    // TODO(casey): Actually implement this!
    char *FileName = "test.hha";

    // TODO(casey): If we want, someday, make an actual arena
    sdl_platform_file_handle *Result = (sdl_platform_file_handle *)malloc(
        sizeof(sdl_platform_file_handle));

    if(Result)
    {
        Result->SDLHandle = open(FileName, O_RDONLY);
        Result->H.NoErrors = (Result->SDLHandle != -1);
    }

    return((platform_file_handle *)Result);
}

internal PLATFORM_FILE_ERROR(SDLFileError)
{
#if HANDMADE_INTERNAL
    printf("SDL FILE ERROR: %s\n", Message);
#endif

    Handle->NoErrors = false;
}

internal PLATFORM_READ_DATA_FROM_FILE(SDLReadDataFromFile)
{
    if(PlatformNoFileErrors(Source))
    {
        sdl_platform_file_handle *Handle = (sdl_platform_file_handle *)Source;

        uint32 FileSize32 = SafeTruncateUInt64(Size);

        uint8 *DestLocation = (uint8*)Dest;
        while (FileSize32)
        {
            ssize_t BytesRead = pread(Handle->SDLHandle, DestLocation, FileSize32, Offset);
            if (BytesRead != -1)
            {
                // NOTE(casey): File read succeeded!
                FileSize32 -= BytesRead;
                DestLocation += BytesRead;
            }
            else
            {
                SDLFileError(&Handle->H, "Read file failed.");
                break;
            }
        }
    }
}

/*

internal PLATFORM_FILE_ERROR(SDLCloseFile)
{
    close(FileHandle);
}

*/

int
main(int argc, char *argv[])
{
    sdl_state SDLState = {};

    platform_work_queue HighPriorityQueue = {};
    SDLMakeQueue(&HighPriorityQueue, 6);

    platform_work_queue LowPriorityQueue = {};
    SDLMakeQueue(&LowPriorityQueue, 2);

#if 0
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A0");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A1");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A2");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A3");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A4");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A5");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A6");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A7");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A8");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String A9");

    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B0");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B1");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B2");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B3");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B4");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B5");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B6");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B7");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B8");
    SDLAddEntry(&Queue, DoWorkerWork, (void *)"String B9");

    SDLCompleteAllWork(&Queue);
#endif

    GlobalPerfCountFrequency = SDL_GetPerformanceFrequency();

    SDLGetEXEFileName(&SDLState);

    char SourceGameCodeDLLFullPath[SDL_STATE_FILE_NAME_COUNT];
    SDLBuildEXEPathFileName(&SDLState, "handmade.so",
                              sizeof(SourceGameCodeDLLFullPath), SourceGameCodeDLLFullPath);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC | SDL_INIT_AUDIO);

    // Initialise our Game Controllers:
    SDLOpenGameControllers();

#if HANDMADE_INTERNAL
    DEBUGGlobalShowCursor = true;
#endif

    /* NOTE(casey): 1080p display mode is 1920x1080 -> Half of that is 960x540
       1920 -> 2048 = 2048-1920 -> 128 pixels
       1080 -> 2048 = 2048-1080 -> pixels 968
       1024 + 128 = 1152
    */
    // Create our window.
    SDL_Window *Window = SDL_CreateWindow("Handmade Hero",
                                          SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED,
                                          1920,
                                          1080,
                                          SDL_WINDOW_RESIZABLE);
    if(Window)
    {
        SDL_ShowCursor(DEBUGGlobalShowCursor ? SDL_ENABLE : SDL_DISABLE);

        // Create a "Renderer" for our window.
        SDL_Renderer *Renderer = SDL_CreateRenderer(Window,
                                                    -1,
                                                    SDL_RENDERER_PRESENTVSYNC);
        if (Renderer)
        {
            //SDLResizeTexture(&GlobalBackbuffer, Renderer, 960, 540);
            SDLResizeTexture(&GlobalBackbuffer, Renderer, 1920, 1080);

            sdl_sound_output SoundOutput = {};

            int MonitorRefreshHz = 60;
            int DisplayIndex = SDL_GetWindowDisplayIndex(Window);
            SDL_DisplayMode Mode = {};
            int DisplayModeResult = SDL_GetDesktopDisplayMode(DisplayIndex, &Mode);
            if(DisplayModeResult == 0 && Mode.refresh_rate > 1)
            {
                MonitorRefreshHz = Mode.refresh_rate;
            }
            real32 GameUpdateHz = (real32)(MonitorRefreshHz / 2.0f);
            real32 TargetSecondsPerFrame = 1.0f / (real32)GameUpdateHz;

            // TODO(casey): Make this like sixty seconds?
            SoundOutput.SamplesPerSecond = 48000;
            SoundOutput.BytesPerSample = sizeof(int16)*2;
            SoundOutput.SecondaryBufferSize = SoundOutput.SamplesPerSecond*SoundOutput.BytesPerSample;
            // TODO(casey): Actually compute this variance and see
            // what the lowest reasonable value is.
            SoundOutput.SafetyBytes = (int)(((real32)SoundOutput.SamplesPerSecond*(real32)SoundOutput.BytesPerSample / GameUpdateHz)/2.0f);
            SDLInitAudio(SoundOutput.SamplesPerSecond, SoundOutput.SecondaryBufferSize);
            SDLClearBuffer(&SoundOutput);
            SDL_PauseAudio(0);

            GlobalRunning = true;

#if 0
            // NOTE: This tests the QueuedAudio update frequency with 25ms worth of samples
            int16 TestSamples[4800] = {};
            uint64 LastCounter = 0;

            while(GlobalRunning)
            {
                uint32 QueuedAudioBytes = SDL_GetQueuedAudioSize(1);
                if(!QueuedAudioBytes)
                {
                    uint64 NewCounter = SDLGetWallClock();
                    printf("%lu bytes in:%f\n", ArrayCount(TestSamples),
                           SDLGetSecondsElapsed(LastCounter, NewCounter));

                    SDL_QueueAudio(1, TestSamples, ArrayCount(TestSamples));
                    LastCounter = NewCounter;
                }
            }
#endif

            // TODO(casey): Pool with bitmap VirtualAlloc
            // TODO(casey): Remove MaxPossibleOverrun?
            // NOTE: calloc() allocates memory and clears it to zero. It accepts the number of things being allocated and their size.
            u32 MaxPossibleOverrun = 8;
            int16 *Samples = (int16 *)calloc(SoundOutput.SamplesPerSecond + MaxPossibleOverrun, SoundOutput.BytesPerSample);

#if HANDMADE_INTERNAL
            // TODO: This will fail gently on 32-bit at the moment, but we should probably fix it.
            void *BaseAddress = (void *)Terabytes(2);
#else
            void *BaseAddress = 0;
#endif

            game_memory GameMemory = {};
            GameMemory.PermanentStorageSize = Megabytes(256);
            GameMemory.TransientStorageSize = Gigabytes(1);
            GameMemory.HighPriorityQueue = &HighPriorityQueue;
            GameMemory.LowPriorityQueue = &LowPriorityQueue;
            GameMemory.PlatformAPI.AddEntry = SDLAddEntry;
            GameMemory.PlatformAPI.CompleteAllWork = SDLCompleteAllWork;

            GameMemory.PlatformAPI.GetAllFilesOfTypeBegin = SDLGetAllFilesOfTypeBegin;
            GameMemory.PlatformAPI.GetAllFilesOfTypeEnd = SDLGetAllFilesOfTypeEnd;
            GameMemory.PlatformAPI.OpenFile = SDLOpenFile;
            GameMemory.PlatformAPI.ReadDataFromFile = SDLReadDataFromFile;
            GameMemory.PlatformAPI.FileError = SDLFileError;

            GameMemory.PlatformAPI.DEBUGFreeFileMemory = DEBUGPlatformFreeFileMemory;
            GameMemory.PlatformAPI.DEBUGReadEntireFile = DEBUGPlatformReadEntireFile;
            GameMemory.PlatformAPI.DEBUGWriteEntireFile = DEBUGPlatformWriteEntireFile;

            // TODO(casey): Handle various memory footprints (USING
            // SYSTEM METRICS)

            // TODO(casey): TransientStorage needs to be broken up
            // into game transient and cache transient, and only the
            // former need be saved for state playback.
            SDLState.TotalSize = GameMemory.PermanentStorageSize + GameMemory.TransientStorageSize;
            SDLState.GameMemoryBlock = mmap(BaseAddress, (size_t)SDLState.TotalSize,
                                               PROT_READ | PROT_WRITE,
                                               MAP_ANON | MAP_PRIVATE,
                                               -1, 0);
            GameMemory.PermanentStorage = SDLState.GameMemoryBlock;
            GameMemory.TransientStorage = ((uint8 *)GameMemory.PermanentStorage +
                                           GameMemory.PermanentStorageSize);

            for(int ReplayIndex = 1;
                ReplayIndex < ArrayCount(SDLState.ReplayBuffers);
                ++ReplayIndex)
            {
                sdl_replay_buffer *ReplayBuffer = &SDLState.ReplayBuffers[ReplayIndex];

                SDLGetInputFileLocation(&SDLState, false, ReplayIndex,
                                          sizeof(ReplayBuffer->FileName), ReplayBuffer->FileName);

                ReplayBuffer->FileHandle =
                    open(ReplayBuffer->FileName,
                         O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

                ftruncate(ReplayBuffer->FileHandle, SDLState.TotalSize);

                ReplayBuffer->MemoryBlock = mmap(0, (size_t)SDLState.TotalSize,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE,
                                                 ReplayBuffer->FileHandle, 0);
                if(ReplayBuffer->MemoryBlock)
                {
                }
                else
                {
                    // TODO(casey): Diagnostic
                }
            }

            if(Samples && GameMemory.PermanentStorage && GameMemory.TransientStorage)
            {
                game_input Input[2] = {};
                game_input *NewInput = &Input[0];
                game_input *OldInput = &Input[1];

                uint64 LastCounter = SDLGetWallClock();
                uint64 FlipWallClock = SDLGetWallClock();

                int DebugTimeMarkerIndex = 0;
                sdl_debug_time_marker DebugTimeMarkers[30] = {0};

                uint32 AudioLatencyBytes = 0;
                real32 AudioLatencySeconds = 0;
                bool32 SoundIsValid = false;

                sdl_game_code Game = SDLLoadGameCode(SourceGameCodeDLLFullPath);

                uint64 LastCycleCount = _rdtsc();
                while(GlobalRunning)
                {
                    NewInput->dtForFrame = TargetSecondsPerFrame;

                    NewInput->ExecutableReloaded = false;
                    time_t NewDLLWriteTime = SDLGetLastWriteTime(SourceGameCodeDLLFullPath);
                    if(NewDLLWriteTime != Game.DLLLastWriteTime)
                    {
                        SDLCompleteAllWork(&HighPriorityQueue);
                        SDLCompleteAllWork(&LowPriorityQueue);

                        SDLUnloadGameCode(&Game);
                        Game = SDLLoadGameCode(SourceGameCodeDLLFullPath);
                        NewInput->ExecutableReloaded = true;
                    }

                    // TODO(casey): Zeroing macro
                    // TODO(casey): We can't zero everything because the up/down state will
                    // be wrong!!!
                    game_controller_input *OldKeyboardController = GetController(OldInput, 0);
                    game_controller_input *NewKeyboardController = GetController(NewInput, 0);
                    *NewKeyboardController = {};
                    NewKeyboardController->IsConnected = true;
                    for(int ButtonIndex = 0;
                        ButtonIndex < ArrayCount(NewKeyboardController->Buttons);
                        ++ButtonIndex)
                    {
                        NewKeyboardController->Buttons[ButtonIndex].EndedDown =
                            OldKeyboardController->Buttons[ButtonIndex].EndedDown;
                    }

                    SDLProcessPendingEvents(&SDLState, NewKeyboardController);

                    if(!GlobalPause)
                    {
                        // TODO: Adjust mouse X and Y according to image offset and stretching
                        uint32 MouseState = SDL_GetMouseState(&NewInput->MouseX, &NewInput->MouseY);
                        NewInput->MouseZ = 0; // TODO(casey): Support mousewheel?
                        SDLProcessKeyboardEvent(&NewInput->MouseButtons[0],
                                                MouseState & SDL_BUTTON(SDL_BUTTON_LEFT));
                        SDLProcessKeyboardEvent(&NewInput->MouseButtons[1],
                                                MouseState & SDL_BUTTON(SDL_BUTTON_MIDDLE));
                        SDLProcessKeyboardEvent(&NewInput->MouseButtons[2],
                                                MouseState & SDL_BUTTON(SDL_BUTTON_RIGHT));
                        SDLProcessKeyboardEvent(&NewInput->MouseButtons[3],
                                                MouseState & SDL_BUTTON(SDL_BUTTON_X1));
                        SDLProcessKeyboardEvent(&NewInput->MouseButtons[4],
                                                MouseState & SDL_BUTTON(SDL_BUTTON_X2));

                        // TODO(casey): Need to not poll disconnected controllers to avoid
                        // xinput frame rate hit on older libraries...
                        // TODO(casey): Should we poll this more frequently
                        uint32 MaxControllerCount = MAX_CONTROLLERS;
                        if(MaxControllerCount > (ArrayCount(NewInput->Controllers) - 1))
                        {
                            MaxControllerCount = (ArrayCount(NewInput->Controllers) - 1);
                        }

                        for (uint32 ControllerIndex = 0;
                             ControllerIndex < MaxControllerCount;
                             ++ControllerIndex)
                        {
                            uint32 OurControllerIndex = ControllerIndex + 1;
                            game_controller_input *OldController = GetController(OldInput, OurControllerIndex);
                            game_controller_input *NewController = GetController(NewInput, OurControllerIndex);

                            SDL_GameController *Controller = ControllerHandles[ControllerIndex];
                            if(Controller && SDL_GameControllerGetAttached(Controller))
                            {
                                NewController->IsConnected = true;
                                NewController->IsAnalog = OldController->IsAnalog;

                                // NOTE(casey): This controller is plugged in
                                int16 AxisLX = SDL_GameControllerGetAxis(Controller, SDL_CONTROLLER_AXIS_LEFTX);
                                int16 AxisLY = SDL_GameControllerGetAxis(Controller, SDL_CONTROLLER_AXIS_LEFTY);

                                // TODO(casey): This is a square deadzone, check XInput to
                                // verify that the deadzone is "round" and show how to do
                                // round deadzone processing.
                                NewController->StickAverageX = SDLProcessGameControllerAxisValue(
                                    AxisLX, CONTROLLER_AXIS_LEFT_DEADZONE);
                                NewController->StickAverageY = -SDLProcessGameControllerAxisValue(
                                    AxisLY, CONTROLLER_AXIS_LEFT_DEADZONE);
                                if((NewController->StickAverageX != 0.0f) ||
                                   (NewController->StickAverageY != 0.0f))
                                {
                                    NewController->IsAnalog = true;
                                }

                                if(SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_DPAD_UP))
                                {
                                    NewController->StickAverageY = 1.0f;
                                    NewController->IsAnalog = false;
                                }

                                if(SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
                                {
                                    NewController->StickAverageY = -1.0f;
                                    NewController->IsAnalog = false;
                                }

                                if(SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
                                {
                                    NewController->StickAverageX = -1.0f;
                                    NewController->IsAnalog = false;
                                }

                                if(SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
                                {
                                    NewController->StickAverageX = 1.0f;
                                    NewController->IsAnalog = false;
                                }

                                real32 Threshold = 0.5f;
                                SDLProcessGameControllerButton(
                                    &OldController->MoveLeft,
                                    NewController->StickAverageX < -Threshold,
                                    &NewController->MoveLeft);
                                SDLProcessGameControllerButton(
                                    &OldController->MoveRight,
                                    NewController->StickAverageX > Threshold,
                                    &NewController->MoveRight);
                                SDLProcessGameControllerButton(
                                    &OldController->MoveDown,
                                    NewController->StickAverageY < -Threshold,
                                    &NewController->MoveDown);
                                SDLProcessGameControllerButton(
                                    &OldController->MoveUp,
                                    NewController->StickAverageY > Threshold,
                                    &NewController->MoveUp);

                                SDLProcessGameControllerButton(&OldController->ActionDown,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_A),
                                    &NewController->ActionDown);
                                SDLProcessGameControllerButton(&OldController->ActionRight,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_B),
                                    &NewController->ActionRight);
                                SDLProcessGameControllerButton(&OldController->ActionLeft,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_X),
                                    &NewController->ActionLeft);
                                SDLProcessGameControllerButton(&OldController->ActionUp,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_Y),
                                    &NewController->ActionUp);
                                SDLProcessGameControllerButton(&OldController->LeftShoulder,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER),
                                    &NewController->LeftShoulder);
                                SDLProcessGameControllerButton(&OldController->RightShoulder,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
                                    &NewController->RightShoulder);

                                SDLProcessGameControllerButton(&OldController->Start,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_START),
                                    &NewController->Start);
                                SDLProcessGameControllerButton(&OldController->Back,
                                    SDL_GameControllerGetButton(Controller, SDL_CONTROLLER_BUTTON_BACK),
                                    &NewController->Back);
                            }
                            else
                            {
                                // NOTE(casey): The controller is not available
                                NewController->IsConnected = false;
                            }
                        }

                        game_offscreen_buffer Buffer = {};
                        // NOTE: GlobalBackbuffer is top-down, whereas the game is bottom-up
                        Buffer.Memory = ((uint8*)GlobalBackbuffer.Memory) +
                                        (GlobalBackbuffer.Width *
                                        (GlobalBackbuffer.Height-1) *
                                        GlobalBackbuffer.BytesPerPixel);
                        Buffer.Width = GlobalBackbuffer.Width;
                        Buffer.Height = GlobalBackbuffer.Height;
                        Buffer.Pitch = -GlobalBackbuffer.Pitch;

                        if(SDLState.InputRecordingIndex)
                        {
                            SDLRecordInput(&SDLState, NewInput);
                        }

                        if(SDLState.InputPlayingIndex)
                        {
                            SDLPlayBackInput(&SDLState, NewInput);
                        }

                        if(Game.UpdateAndRender)
                        {
                            Game.UpdateAndRender(&GameMemory, NewInput, &Buffer);
                            HandleDebugCycleCounters(&GameMemory);
                        }

                        uint64 AudioWallClock = SDLGetWallClock();
                        real32 FromBeginToAudioSeconds = SDLGetSecondsElapsed(FlipWallClock, AudioWallClock);

                        uint32 QueuedAudioBytes = SDL_GetQueuedAudioSize(1);

                        /* TODO: Improve sound output computation

                           This is an attempt to use SDL_QueueAudio
                           instead of Handmade Penguin's ring buffer.
                        */

                        if(!SoundIsValid)
                        {
                            SoundIsValid = true;
                        }

                        uint32 ExpectedSoundBytesPerFrame =
                            (int)((real32)(SoundOutput.SamplesPerSecond*SoundOutput.BytesPerSample) /
                                  GameUpdateHz);
                        real32 SecondsLeftUntilFlip = (TargetSecondsPerFrame - FromBeginToAudioSeconds);
                        uint32 ExpectedBytesUntilFlip = (uint32)((SecondsLeftUntilFlip/TargetSecondsPerFrame)*(real32)ExpectedSoundBytesPerFrame);

                        int32 BytesToWrite = (ExpectedSoundBytesPerFrame + SoundOutput.SafetyBytes) - QueuedAudioBytes;
                        if(BytesToWrite < 0)
                        {
                            BytesToWrite = 0;
                        }

                        game_sound_output_buffer SoundBuffer = {};
                        SoundBuffer.SamplesPerSecond = SoundOutput.SamplesPerSecond;
                        SoundBuffer.SampleCount = Align8(BytesToWrite / SoundOutput.BytesPerSample);
                        BytesToWrite = SoundBuffer.SampleCount*SoundOutput.BytesPerSample;
                        SoundBuffer.Samples = Samples;
                        if(Game.GetSoundSamples)
                        {
                            Game.GetSoundSamples(&GameMemory, &SoundBuffer);
                        }

#if HANDMADE_INTERNAL
                        sdl_debug_time_marker *Marker = &DebugTimeMarkers[DebugTimeMarkerIndex];
                        Marker->QueuedAudioBytes = QueuedAudioBytes;
                        Marker->OutputByteCount = BytesToWrite;
                        Marker->ExpectedBytesUntilFlip = ExpectedBytesUntilFlip;

                        AudioLatencyBytes = QueuedAudioBytes;
                        AudioLatencySeconds =
                            (((real32)AudioLatencyBytes / (real32)SoundOutput.BytesPerSample) /
                             (real32)SoundOutput.SamplesPerSecond);

#if 0
                        printf("BTW:%u - Latency:%d (%fs)\n",
                                BytesToWrite, AudioLatencyBytes, AudioLatencySeconds);
#else
                        // NOTE: Not used
                        (void) AudioLatencySeconds;
#endif
#endif
                        SDLFillSoundBuffer(&SoundOutput, BytesToWrite, &SoundBuffer);

                        uint64 WorkCounter = SDLGetWallClock();
                        real32 WorkSecondsElapsed = SDLGetSecondsElapsed(LastCounter, WorkCounter);

                        // TODO(casey): NOT TESTED YET!  PROBABLY BUGGY!!!!!
                        real32 SecondsElapsedForFrame = WorkSecondsElapsed;
                        if(SecondsElapsedForFrame < TargetSecondsPerFrame)
                        {
                            uint32 SleepMS = (uint32)(1000.0f * (TargetSecondsPerFrame -
                                                               SecondsElapsedForFrame));
                            if(SleepMS > 0)
                            {
                                SDL_Delay(SleepMS);
                            }

                            real32 TestSecondsElapsedForFrame = SDLGetSecondsElapsed(LastCounter,
                                                                                       SDLGetWallClock());
                            if(TestSecondsElapsedForFrame < TargetSecondsPerFrame)
                            {
                                // TODO(casey): LOG MISSED SLEEP HERE
                            }

                            while(SecondsElapsedForFrame < TargetSecondsPerFrame)
                            {
                                SecondsElapsedForFrame = SDLGetSecondsElapsed(LastCounter,
                                                                                SDLGetWallClock());
                            }
                        }
                        else
                        {
                            // TODO(casey): MISSED FRAME RATE!
                            // TODO(casey): Logging
                        }

                        uint64 EndCounter = SDLGetWallClock();
                        real32 MSPerFrame = 1000.0f*SDLGetSecondsElapsed(LastCounter, EndCounter);
                        LastCounter = EndCounter;

#if 0
                        // TODO(casey): Note, current is wrong on the zero'th index
                        SDLDebugSyncDisplay(&GlobalBackbuffer, ArrayCount(DebugTimeMarkers), DebugTimeMarkers,
                                              DebugTimeMarkerIndex - 1, &SoundOutput, TargetSecondsPerFrame);
#endif

                        sdl_window_dimension Dimension = SDLGetWindowDimension(Window);
                        SDLDisplayBufferInWindow(&GlobalBackbuffer, Renderer,
                                                   Dimension.Width, Dimension.Height);

                        FlipWallClock = SDLGetWallClock();

                        game_input *Temp = NewInput;
                        NewInput = OldInput;
                        OldInput = Temp;
                        // TODO(casey): Should I clear these here?

#if 1
                        uint64 EndCycleCount = __rdtsc();
                        uint64 CyclesElapsed = EndCycleCount - LastCycleCount;
                        LastCycleCount = EndCycleCount;

                        real64 FPS = 0.0f;
                        real64 MCPF = ((real64)CyclesElapsed / (1000.0f * 1000.0f));

                        printf("%.02fms/f,  %.02ff/s,  %.02fmc/f\n", MSPerFrame, FPS, MCPF);
#endif

#if HANDMADE_INTERNAL
                        ++DebugTimeMarkerIndex;
                        if(DebugTimeMarkerIndex == ArrayCount(DebugTimeMarkers))
                        {
                            DebugTimeMarkerIndex = 0;
                        }
#endif
                    }
                }
            }
            else
            {
                // TODO(casey): Logging
            }
        }
        else
        {
            // TODO(casey): Logging
        }
    }
    else
    {
        // TODO(casey): Logging
    }

    SDLCloseGameControllers();
    SDL_Quit();
    return(0);
}
