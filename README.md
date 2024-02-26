# Vulkan Video Encode - Simple Example

This is a very simple example of how to use the Vulkan Video Encode Extension. It should provide a possibility to see the basic video encoding workflow.

Frames are generated using a dynamic graphics pipeline rendering directly into RGB images (headless). Those frames are then converted into YCbCr (using a compute shader) and encoded using Vulkan into an H.264 video. The video packets are copied back to the host and stored in a file `./hwenc.264`. This is an H.264 elementary stream which can be viewed with ffmpeg/ffplay (or VLC).

The encoder will generate a video stream with a simple GOP structure consisting of 1 IDR frame and 15 P frames.  
The device initialization and frame generation is in `main.cpp`. All of the video encoding code is in `videoencoder.cpp` and `h264parameterset.hpp`.

This example needs Vulkan SDK installed together with VMA and Volk, no other dependencies are needed.  
It is tested on Windows 11 using Visual Studio 2022 and running on a NVIDIA GeForce RTX 3070.

**Disclaimer:** Even if it is working, it is not thought to be complete and may fail on other hardware. If you search for a more sophisticated (but also more complex) example I would suggest you look at the [vk_video_samples](https://github.com/nvpro-samples/vk_video_samples) from Nvidia.

## Compile & Run on Windows
```cmd
rem clone
git clone https://github.com/clemy/vulkan-video-encode-simple.git
cd vulkan-video-encode-simple

rem compile
mkdir build
cd build
cmake ..
cmake --build .

rem run
cd Debug
headless.exe

rem play the video file
ffplay hwenc.264
```

If you use a different platform, please adapt the directory names, especially also for the shaders in the source code.