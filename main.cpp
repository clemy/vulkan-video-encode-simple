/*
 * Vulkan Video Encode Extension - Simple Example
 *
 * Copyright (c) 2024 Bernhard C. Schrenk <clemy@clemy.org>
 *   University of Vienna
 *   https://www.univie.ac.at/
 * developed during the courses "Cloud Gaming" & "Practical Course 1"
 *   supervised by Univ.-Prof. Dipl.-Ing. Dr. Helmut Hlavacs <helmut.hlavacs@univie.ac.at>
 *     University of Vienna
 *     Research Group "EDEN - Education, Didactics and Entertainment Computing"
 *     https://eden.cs.univie.ac.at/
 *
 * main.cpp is based on the Vulkan Tutorial (https://vulkan-tutorial.com/)
 *
 * This file is licensed under the MIT license.
 * See the LICENSE file in the project root for full license information.
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#define VK_NO_PROTOTYPES
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
#define VOLK_IMPLEMENTATION
#include <volk/volk.h>

#include "utility.hpp"
#include "videoencoder.hpp"

const uint32_t NUM_FRAMES_TO_WRITE = 300;
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;
const size_t IMAGE_INFLIGHT_COUNT = 2;

const std::vector<const char *> deviceExtensions = {
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME};

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> videoEncodeFamily;

    bool isComplete() { return graphicsFamily.has_value() && videoEncodeFamily.has_value(); }
};

class VulkanApplication {
   public:
    void run() {
        initVulkan();
        mainLoop();
        cleanup();
    }

   private:
    VkInstance instance;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VmaAllocator allocator;
    VkQueue graphicsQueue;
    VkQueue videoEncodeQueue;

    std::vector<VkImage> images;
    std::vector<VmaAllocation> imageAllocations;
    std::vector<VkImageView> imageViews;

    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;

    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;

    VideoEncoder videoEncoder;
    std::ofstream outfile;

    void initVulkan() {
        volkInitialize();
        createInstance();
        volkLoadInstance(instance);
        pickPhysicalDevice();
        createLogicalDevice();
        volkLoadDevice(device);
        createAllocator();
        createImages();
        createImageViews();
        createGraphicsPipeline();
        createCommandPool();
        createCommandBuffers();

        initVideoEncoder();
    }

    void mainLoop() {
        for (uint32_t currentFrameNumber = 0; currentFrameNumber < NUM_FRAMES_TO_WRITE; currentFrameNumber++) {
            const uint32_t currentImageIx = currentFrameNumber % images.size();
            drawFrame(currentImageIx, currentFrameNumber);
            encodeFrame(currentImageIx);
        }
    }

    void cleanup() {
        videoEncoder.deinit();
        outfile.close();
        std::cout << "wrote H.264 content to ./hwenc.264\n";
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        for (auto imageView : imageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        for (int i = 0; i < images.size(); i++) {
            vmaDestroyImage(allocator, images[i], imageAllocations[i]);
        }
        vmaDestroyAllocator(allocator);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    void createInstance() {
        const VkApplicationInfo appInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pApplicationName = "Vulkan Sample",
                                        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                        .pEngineName = "No Engine",
                                        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                        .apiVersion = VK_API_VERSION_1_3};

        auto extensions = getRequiredExtensions();
        const VkInstanceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                              .pApplicationInfo = &appInfo,
                                              .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
                                              .ppEnabledExtensionNames = extensions.data()};

        VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance));
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

        if (deviceCount == 0) {
            throw std::runtime_error("failed to find GPUs with Vulkan support!");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto &device : devices) {
            if (isDeviceSuitable(device)) {
                physicalDevice = device;
                break;
            }
        }

        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("failed to find a suitable GPU!");
        }

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        std::cout << "Using device: " << properties.deviceName << "\n";
    }

    void createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        const std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                                        indices.videoEncodeFamily.value()};
        const float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            const VkDeviceQueueCreateInfo queueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                          .queueFamilyIndex = queueFamily,
                                                          .queueCount = 1,
                                                          .pQueuePriorities = &queuePriority};
            queueCreateInfos.push_back(queueCreateInfo);
        }

        const VkPhysicalDeviceFeatures deviceFeatures{};
        VkPhysicalDeviceSynchronization2Features synchronization2_features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .synchronization2 = VK_TRUE};

        const VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
            .pNext = &synchronization2_features,
            .dynamicRendering = VK_TRUE};

        const VkDeviceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                            .pNext = &dynamic_rendering_features,
                                            .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
                                            .pQueueCreateInfos = queueCreateInfos.data(),
                                            .enabledLayerCount = 0,
                                            .enabledExtensionCount = static_cast<unsigned int>(deviceExtensions.size()),
                                            .ppEnabledExtensionNames = deviceExtensions.data(),
                                            .pEnabledFeatures = &deviceFeatures};

        VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device));

        vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.videoEncodeFamily.value(), 0, &videoEncodeQueue);
    }

    void createAllocator() {
        const VmaVulkanFunctions vulkanFunctions = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                                                    .vkGetDeviceProcAddr = vkGetDeviceProcAddr};

        const VmaAllocatorCreateInfo allocatorInfo{.physicalDevice = physicalDevice,
                                                   .device = device,
                                                   .pVulkanFunctions = &vulkanFunctions,
                                                   .instance = instance,
                                                   .vulkanApiVersion = VK_API_VERSION_1_3};

        VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator));
    }

    void createImages() {
        images.resize(IMAGE_INFLIGHT_COUNT);
        imageAllocations.resize(images.size());
        for (int i = 0; i < images.size(); i++) {
            const VkImageCreateInfo imageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .extent = {WIDTH, HEIGHT, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
            const VmaAllocationCreateInfo allocCreateInfo{
                .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT, .usage = VMA_MEMORY_USAGE_AUTO, .priority = 1.0f};

            VK_CHECK(vmaCreateImage(allocator, &imageCreateInfo, &allocCreateInfo, &images[i], &imageAllocations[i],
                                    nullptr));
        }
    }

    void createImageViews() {
        imageViews.resize(images.size());
        for (int i = 0; i < images.size(); i++) {
            const VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                 .image = images[i],
                                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                 .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                      .baseMipLevel = 0,
                                                                      .levelCount = 1,
                                                                      .baseArrayLayer = 0,
                                                                      .layerCount = 1}};

            VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]));
        }
    }

    void createGraphicsPipeline() {
        auto vertShaderCode = readFile("shaders/shader.vert.spv");
        auto fragShaderCode = readFile("shaders/shader.frag.spv");

        const VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
        const VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

        const VkPipelineShaderStageCreateInfo vertShaderStageInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShaderModule,
            .pName = "main"};

        const VkPipelineShaderStageCreateInfo fragShaderStageInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShaderModule,
            .pName = "main"};

        const VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(uint32_t),
        };
        const VkPipelineLayoutCreateInfo pipelineLayoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                            .pushConstantRangeCount = 1,
                                                            .pPushConstantRanges = &pushConstantRange};

        VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));

        const VkPipelineVertexInputStateCreateInfo vertexInputInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE};

        const VkViewport viewport{.x = 0.0f,
                                  .y = 0.0f,
                                  .width = static_cast<float>(WIDTH),
                                  .height = static_cast<float>(HEIGHT),
                                  .minDepth = 0.0f,
                                  .maxDepth = 1.0f};

        const VkRect2D scissor{.offset = {0, 0}, .extent = {WIDTH, HEIGHT}};

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor};

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f,
            .lineWidth = 1.0f};

        const VkPipelineMultisampleStateCreateInfo multisampling{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 1.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE};

        const VkPipelineColorBlendAttachmentState colorBlendAttachment{
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT};

        const VkPipelineColorBlendStateCreateInfo colorBlending{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &colorBlendAttachment,
            .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}};

        const VkFormat colorAttachmentFormat = VK_FORMAT_R8G8B8A8_UNORM;

        const VkPipelineRenderingCreateInfo renderingCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .viewMask = 0,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorAttachmentFormat,
            .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
            .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};

        VkGraphicsPipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                  .pNext = &renderingCreateInfo,
                                                  .stageCount = 2,
                                                  .pStages = shaderStages,
                                                  .pVertexInputState = &vertexInputInfo,
                                                  .pInputAssemblyState = &inputAssembly,
                                                  .pViewportState = &viewportState,
                                                  .pRasterizationState = &rasterizer,
                                                  .pMultisampleState = &multisampling,
                                                  .pDepthStencilState = nullptr,
                                                  .pColorBlendState = &colorBlending,
                                                  .pDynamicState = VK_NULL_HANDLE,
                                                  .layout = pipelineLayout,
                                                  .renderPass = VK_NULL_HANDLE,
                                                  .subpass = 0,
                                                  .basePipelineHandle = VK_NULL_HANDLE,
                                                  .basePipelineIndex = -1};

        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline));

        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
    }

    void createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

        const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                               .queueFamilyIndex = queueFamilyIndices.graphicsFamily.value()};

        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool));
    }

    bool isDeviceSuitable(VkPhysicalDevice device) {
        QueueFamilyIndices indices = findQueueFamilies(device);

        bool extensionsSupported = checkDeviceExtensionSupport(device);

        return indices.isComplete() && extensionsSupported;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

        for (const auto &extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    void createCommandBuffers() {
        commandBuffers.resize(images.size());
        VkCommandBufferAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = commandPool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = static_cast<uint32_t>(commandBuffers.size())};

        VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()));
    }

    void initVideoEncoder() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        const int fps = 30;
        videoEncoder.init(physicalDevice, device, allocator, indices.graphicsFamily.value(), graphicsQueue, commandPool,
                          indices.videoEncodeFamily.value(), videoEncodeQueue, images, imageViews, WIDTH, HEIGHT, fps);

        outfile.open("hwenc.264", std::ios::binary);
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t currentImageIx, uint32_t currentFrameNumber) {
        VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

        VkImageMemoryBarrier2 imageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                                 .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                 .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                                 .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                 .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                 .image = images[currentImageIx],
                                                 .subresourceRange = {
                                                     .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                     .baseMipLevel = 0,
                                                     .levelCount = 1,
                                                     .baseArrayLayer = 0,
                                                     .layerCount = 1,
                                                 }};
        VkDependencyInfoKHR dependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
                                           .imageMemoryBarrierCount = 1,
                                           .pImageMemoryBarriers = &imageMemoryBarrier};
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

        const VkRenderingAttachmentInfo colorAttachmentInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                            .imageView = imageViews[currentImageIx],
                                                            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                                                            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                            .clearValue = {{0.0f, 0.0f, 0.0f, 1.0f}}};

        const VkRenderingInfo renderInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {0, 0, WIDTH, HEIGHT},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachmentInfo,
        };

        vkCmdBeginRendering(commandBuffer, &renderInfo);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t),
                           &currentFrameNumber);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRendering(commandBuffer);

        VK_CHECK(vkEndCommandBuffer(commandBuffer));
    }

    void drawFrame(uint32_t currentImageIx, uint32_t currentFrameNumber) {
        vkResetCommandBuffer(commandBuffers[currentImageIx], 0);
        recordCommandBuffer(commandBuffers[currentImageIx], currentImageIx, currentFrameNumber);

        VkSubmitInfo submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &commandBuffers[currentImageIx]};

        VK_CHECK(vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto &queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }

            if (queueFamily.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) {
                indices.videoEncodeFamily = i;
            }

            if (indices.isComplete()) {
                break;
            }

            i++;
        }

        return indices;
    }

    void encodeFrame(uint32_t currentImageIx) {
        // finish encoding the previous frame
        size_t packetSize;
        do {
            const char *packetData;
            videoEncoder.finishEncode(packetData, packetSize);
            outfile.write(packetData, packetSize);
        } while (packetSize > 0);

        // queue the next frame for encoding
        videoEncoder.queueEncode(currentImageIx);
    }

    std::vector<const char *> getRequiredExtensions() {
        std::vector<const char *> extensions;

        return extensions;
    }

    VkShaderModule createShaderModule(const std::vector<char> &code) {
        VkShaderModuleCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = code.size(),
                                            .pCode = reinterpret_cast<const uint32_t *>(code.data())};

        VkShaderModule shaderModule;
        VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule));

        return shaderModule;
    }
};

int main() {
    VulkanApplication app;

    try {
        app.run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}