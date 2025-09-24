#pragma once

#include "CoreMinimal.h"
#include "AudioMixerDevice.h"
#include "Sound/AudioBus.h"

class USoundSubmixBase;

namespace Audio
{
    class ISubmixBufferListener;
}

class FPanoAudioRecorder : public Audio::ISubmixBufferListener
{
public:
    FPanoAudioRecorder();
    virtual ~FPanoAudioRecorder();

    bool StartRecording(USoundSubmixBase* Submix, int32 InSampleRate, int32 InNumChannels);
    void StopRecording();
    void Reset();

    /** Writes recorded PCM data to a WAV file located at the provided path. */
    bool WriteToWav(const FString& FilePath, double& OutDurationSeconds);

    /** Returns the timestamp (in seconds) relative to StartRecording for the most recent audio buffer. */
    double GetCurrentTimestampSeconds() const;

    /** Returns the total accumulated audio duration based on recorded samples. */
    double GetAccumulatedDurationSeconds() const;

    /** Returns the estimated drift (audio - expected) relative to the provided capture duration. */
    double GetEstimatedDriftSeconds(double CaptureDurationSeconds) const;

    virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override;

private:
    FCriticalSection BufferGuard;
    TArray<float> AccumulatedPCM;
    TArray<uint8> WavDataCache;
    double StartTime;
    double FirstAudioClock;
    double LastAudioClock;
    int32 SampleRate;
    int32 NumChannels;
    bool bRecording;
    bool bReportedChannelMismatch;
    bool bReportedSampleRateMismatch;
};
