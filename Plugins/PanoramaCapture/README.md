# Panorama Capture Plugin

This plugin provides automated 360° panorama capture for Unreal Engine 5.4+ projects. It is designed for Windows (Win64) targets and integrates NVENC hardware encoding when available.

## Features

- Six-camera rig actor (`APanoramaCaptureRigActor`) that generates ±X/±Y/±Z captures with a 90° FOV and produces cubemaps.
- RDG compute shader converts cubemap faces into mono or stereo equirectangular panoramas.
- Supports PNG sequence output (up to 16-bit color depth and 8K resolution) with asynchronous disk writing to avoid stalls.
- Supports zero-copy NVENC H.264/HEVC video encoding on D3D11/D3D12.
- Audio capture via AudioMixer submix to WAV, synchronized with video timestamps.
- Real-time preview texture and optional world-space preview window inside the rig actor with dropped-frame feedback.
- Configurable bitrate, GOP length, B-frame count, and rate-control mode for NVENC recordings plus frame-rate aware encoding.
- NVENC bitstreams stream directly to disk for immediate MP4/MKV packaging after capture.
- Automatic MP4/MKV packaging via FFmpeg (if found on the system).

## Usage

1. Enable the plugin in your Unreal Engine project (5.4/5.5/5.6).
2. Add an instance of `APanoramaCaptureRigActor` to your level.
3. Configure capture settings on the embedded `UPanoramaCaptureComponent`.
4. Call `StartCapture` / `StopCapture` from Blueprint or the editor to control recording.
5. (Optional) Configure the **Panorama Capture** developer settings to change default capture modes and the automatic session naming pattern.

For NVENC output, ensure the NVIDIA driver includes the NVENC runtime and that FFmpeg is installed/available on the system path for final container packaging.
