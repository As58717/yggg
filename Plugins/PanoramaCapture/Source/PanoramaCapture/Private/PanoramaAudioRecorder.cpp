#include "PanoramaAudioRecorder.h"

#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "Engine/Engine.h"
#include "Sound/SoundSubmix.h"
#include "Misc/ScopeLock.h"

namespace
{
    static void WriteWaveHeader(TArray<uint8>& Buffer, int32 InSampleRate, int32 InNumChannels, int32 InBitsPerSample, int32 DataSize)
    {
        const int32 ByteRate = InSampleRate * InNumChannels * InBitsPerSample / 8;
        const int32 BlockAlign = InNumChannels * InBitsPerSample / 8;

        Buffer.Reset();
        Buffer.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
        const int32 ChunkSize = 36 + DataSize;
        Buffer.Append(reinterpret_cast<const uint8*>(&ChunkSize), 4);
        Buffer.Append(reinterpret_cast<const uint8*>("WAVE"), 4);
        Buffer.Append(reinterpret_cast<const uint8*>("fmt "), 4);
        const int32 Subchunk1Size = 16;
        Buffer.Append(reinterpret_cast<const uint8*>(&Subchunk1Size), 4);
        const uint16 AudioFormat = 1;
        Buffer.Append(reinterpret_cast<const uint8*>(&AudioFormat), 2);
        const uint16 NumChannels16 = static_cast<uint16>(InNumChannels);
        Buffer.Append(reinterpret_cast<const uint8*>(&NumChannels16), 2);
        Buffer.Append(reinterpret_cast<const uint8*>(&InSampleRate), 4);
        Buffer.Append(reinterpret_cast<const uint8*>(&ByteRate), 4);
        Buffer.Append(reinterpret_cast<const uint8*>(&BlockAlign), 2);
        const uint16 BitsPerSample16 = static_cast<uint16>(InBitsPerSample);
        Buffer.Append(reinterpret_cast<const uint8*>(&BitsPerSample16), 2);
        Buffer.Append(reinterpret_cast<const uint8*>("data"), 4);
        Buffer.Append(reinterpret_cast<const uint8*>(&DataSize), 4);
    }
}

FPanoAudioRecorder::FPanoAudioRecorder()
    : StartTime(0.0)
    , SampleRate(48000)
    , NumChannels(2)
    , bRecording(false)
{
}

FPanoAudioRecorder::~FPanoAudioRecorder()
{
    StopRecording();
}

bool FPanoAudioRecorder::StartRecording(USoundSubmixBase* Submix, int32 InSampleRate, int32 InNumChannels)
{
    if (bRecording)
    {
        return false;
    }

    SampleRate = InSampleRate;
    NumChannels = InNumChannels;
    AccumulatedPCM.Reset();
    StartTime = FPlatformTime::Seconds();

    if (FAudioDevice* AudioDevice = GEngine ? GEngine->GetMainAudioDevice() : nullptr)
    {
        if (Audio::FMixerDevice* MixerDevice = static_cast<Audio::FMixerDevice*>(AudioDevice->GetAudioMixerDevice()))
        {
            MixerDevice->RegisterSubmixBufferListener(this, Submix);
            bRecording = true;
            return true;
        }
    }

    return false;
}

void FPanoAudioRecorder::StopRecording()
{
    if (!bRecording)
    {
        return;
    }

    if (FAudioDevice* AudioDevice = GEngine ? GEngine->GetMainAudioDevice() : nullptr)
    {
        if (Audio::FMixerDevice* MixerDevice = static_cast<Audio::FMixerDevice*>(AudioDevice->GetAudioMixerDevice()))
        {
            MixerDevice->UnregisterSubmixBufferListener(this);
        }
    }

    bRecording = false;
}

void FPanoAudioRecorder::Reset()
{
    FScopeLock Lock(&BufferGuard);
    AccumulatedPCM.Reset();
    WavDataCache.Reset();
    StartTime = 0.0;
}

bool FPanoAudioRecorder::WriteToWav(const FString& FilePath, double& OutDurationSeconds)
{
    FScopeLock Lock(&BufferGuard);

    if (AccumulatedPCM.Num() == 0)
    {
        return false;
    }

    const int32 BitsPerSample = 16;
    TArray<int16> Converted;
    Converted.Reserve(AccumulatedPCM.Num());

    for (float Sample : AccumulatedPCM)
    {
        Converted.Add(static_cast<int16>(FMath::Clamp(Sample, -1.f, 1.f) * 32767.f));
    }

    const int32 DataSize = Converted.Num() * sizeof(int16);
    WriteWaveHeader(WavDataCache, SampleRate, NumChannels, BitsPerSample, DataSize);
    WavDataCache.Append(reinterpret_cast<const uint8*>(Converted.GetData()), DataSize);

    if (FFileHelper::SaveArrayToFile(WavDataCache, *FilePath))
    {
        OutDurationSeconds = static_cast<double>(Converted.Num()) / static_cast<double>(SampleRate * NumChannels);
        return true;
    }

    return false;
}

double FPanoAudioRecorder::GetCurrentTimestampSeconds() const
{
    if (!bRecording)
    {
        return 0.0;
    }

    return FPlatformTime::Seconds() - StartTime;
}

void FPanoAudioRecorder::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 InNumChannels, const int32 InSampleRate, double AudioClock)
{
    FScopeLock Lock(&BufferGuard);
    AccumulatedPCM.Append(AudioData, NumSamples);
}
