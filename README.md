# Vulkan Video Encode - Simple Example

This is a very simple example of how to use the Vulkan Video Encode Extension.

Frames are generated using a dynamic graphics pipeline rendering directly into images (headless). Those frames are then converted into YCbCr and encoded into an H.264 video and stored in a file `./hwenc.264`.

The encoder will generate a video stream with a simple GOP structure consisting of 1 IDR frame and 15 P frames.

The device initialization and frame generation is in `main.cpp`. All of the video encoding code is in `videoencoder.cpp` and `h264parameterset.hpp`.

This example needs Vulkan SDK installed together with VMA and Volk, no other dependencies are needed.

It is tested on Windows 11 using Visual Studio 2022 and running on a NVIDIA GeForce RTX 3070.

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