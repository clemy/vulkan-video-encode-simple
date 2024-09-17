# Vulkan Video Encode - Simple Example

This is a very simple example of how to use the Vulkan Video Encode Extension. It should provide a possibility to see the basic video encoding workflow.

## Dependencies
This example needs the Vulkan SDK and a compatible GPU, no other dependencies are needed.

* Minimum NVIDIA driver version: 551.23.0.0  
* Minimum Vulkan SDK: 1.3.275.0 (including VMA & Volk)  
* Tested with: Windows 11 / Visual Studio 2022  
* Tested on: NVIDIA GeForce RTX 3070

## Compile & Run on Windows
```cmd
rem clone
git clone https://github.com/clemy/vulkan-video-encode-simple.git
cd vulkan-video-encode-simple

rem compile (use Visual Studio Developer Command Prompt)
mkdir build
cd build
cmake ..
cmake --build .

rem run
Debug\headless.exe

rem play the video file
ffplay hwenc.264
```

If you use a different platform, please adapt the directory names, especially also for the shaders in the source code.

## Contact
* Developed by **Bernhard C. Schrenk** <clemy@clemy.org>  
  * as part of the courses **Cloud Gaming** & **Practical Course 1**
* supervised by Univ.-Prof. Dipl.-Ing. Dr. **Helmut Hlavacs** <helmut.hlavacs@univie.ac.at>
  * **University of Vienna** - https://www.univie.ac.at/
  * **Research Group EDEN** - Education, Didactics and Entertainment Computing  
  https://eden.cs.univie.ac.at/
* see also the **Vienna Vulkan Engine**
  * https://github.com/hlavacs/ViennaVulkanEngine
  * a Vulkan based render engine meant for learning and teaching the Vulkan API

## How it works
Frames are generated using a dynamic graphics pipeline rendering directly into RGB images (headless). Those frames are then converted into YCbCr (using a compute shader) and encoded using Vulkan into an H.264 video. The video packets are copied back to the host and stored in a file `./hwenc.264`. This is an H.264 elementary stream which can be viewed with ffmpeg/ffplay (or VLC).

The encoder will generate a video stream with a simple GOP structure consisting of 1 IDR frame and 15 P frames.  
The device initialization and frame generation is in `main.cpp`. All of the video encoding code is in `videoencoder.cpp` and `h264parameterset.hpp`.

## Disclaimer
Even if it is working, it is not thought to be complete and may fail on other hardware. If you search for a more sophisticated (but also more complex) example I would suggest you look at the [vk_video_samples](https://github.com/nvpro-samples/vk_video_samples) from Nvidia.