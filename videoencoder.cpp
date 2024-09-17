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
 * This file is licensed under the MIT license.
 * See the LICENSE file in the project root for full license information.
 */

#include "videoencoder.hpp"

#include <array>
#include <cassert>

#include "utility.hpp"

void VideoEncoder::init(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator,
                        uint32_t computeQueueFamily, VkQueue computeQueue, VkCommandPool computeCommandPool,
                        uint32_t encodeQueueFamily, VkQueue encodeQueue, const std::vector<VkImage>& inputImages,
                        const std::vector<VkImageView>& inputImageViews, uint32_t width, uint32_t height,
                        uint32_t fps) {
    assert(!m_running);

    if (m_initialized) {
        if ((width & ~1) == m_width && (height & ~1) == m_height) {
            // nothing changed
            return;
        }

        // resolution changed
        deinit();
    }

    m_physicalDevice = physicalDevice;
    m_device = device;
    m_allocator = allocator;
    m_computeQueue = computeQueue;
    m_encodeQueue = encodeQueue;
    m_computeQueueFamily = computeQueueFamily;
    m_encodeQueueFamily = encodeQueueFamily;
    m_computeCommandPool = computeCommandPool;
    m_inputImages = inputImages;
    m_width = width & ~1;
    m_height = height & ~1;

    createEncodeCommandPool();
    createVideoSession();
    allocateVideoSessionMemory();
    createVideoSessionParameters(fps);
    readBitstreamHeader();
    allocateOutputBitStream();
    allocateReferenceImages(2);
    allocateIntermediateImages();
    createOutputQueryPool();
    createYCbCrConversionPipeline(inputImageViews);

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_interQueueSemaphore));
    VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_interQueueSemaphore2));
    VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_interQueueSemaphore3));
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_encodeFinishedFence));

    // Submit initial initialization commands and wait for finish
    VkCommandBuffer cmdBuffer;
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_encodeCommandPool;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &cmdBuffer));
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

    initRateControl(cmdBuffer, fps);
    transitionImagesInitial(cmdBuffer);

    VK_CHECK(vkEndCommandBuffer(cmdBuffer));
    VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmdBuffer};
    VK_CHECK(vkResetFences(m_device, 1, &m_encodeFinishedFence));
    VK_CHECK(vkQueueSubmit(m_encodeQueue, 1, &submitInfo, m_encodeFinishedFence));
    VK_CHECK(vkWaitForFences(m_device, 1, &m_encodeFinishedFence, VK_TRUE, std::numeric_limits<uint64_t>::max()));
    vkFreeCommandBuffers(device, m_encodeCommandPool, 1, &cmdBuffer);

    m_frameCount = 0;
    m_initialized = true;
}

void VideoEncoder::queueEncode(uint32_t currentImageIx) {
    assert(!m_running);
    convertRGBtoYCbCr(currentImageIx);
    encodeVideoFrame();
    m_running = true;
}

void VideoEncoder::finishEncode(const char*& data, size_t& size) {
    if (!m_running) {
        size = 0;
        return;
    }
    if (m_bitStreamHeaderPending) {
        data = m_bitStreamHeader.data();
        size = m_bitStreamHeader.size();
        m_bitStreamHeaderPending = false;
        return;
    }

    getOutputVideoPacket(data, size);

    vkFreeCommandBuffers(m_device, m_computeCommandPool, 1, &m_computeCommandBuffer);
    vkFreeCommandBuffers(m_device, m_encodeCommandPool, 1, &m_encodeCommandBuffer);
    m_frameCount++;

    m_running = false;
}

void VideoEncoder::createEncodeCommandPool() {
    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                           .queueFamilyIndex = m_encodeQueueFamily};

    VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_encodeCommandPool));
}

void VideoEncoder::createVideoSession() {
    m_encodeH264ProfileInfoExt = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    m_encodeH264ProfileInfoExt.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;

    m_videoProfile = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    m_videoProfile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    m_videoProfile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    m_videoProfile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_videoProfile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_videoProfile.pNext = &m_encodeH264ProfileInfoExt;

    m_videoProfileList = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    m_videoProfileList.profileCount = 1;
    m_videoProfileList.pProfiles = &m_videoProfile;

    VkVideoEncodeH264CapabilitiesKHR h264capabilities = {};
    h264capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;

    VkVideoEncodeCapabilitiesKHR encodeCapabilities = {};
    encodeCapabilities.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    encodeCapabilities.pNext = &h264capabilities;

    VkVideoCapabilitiesKHR capabilities = {};
    capabilities.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    capabilities.pNext = &encodeCapabilities;

    VK_CHECK(vkGetPhysicalDeviceVideoCapabilitiesKHR(m_physicalDevice, &m_videoProfile, &capabilities));
    
    m_chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    if (encodeCapabilities.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR) {
        m_chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    } else if (encodeCapabilities.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) {
        m_chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    } else if (encodeCapabilities.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) {
        m_chosenRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    }

    VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR qualityLevelInfo = {};
    qualityLevelInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
    qualityLevelInfo.pVideoProfile = &m_videoProfile;
    qualityLevelInfo.qualityLevel = 0;

    VkVideoEncodeH264QualityLevelPropertiesKHR h264QualityLevelProperties = {};
    h264QualityLevelProperties.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR;
    VkVideoEncodeQualityLevelPropertiesKHR qualityLevelProperties = {};
    qualityLevelProperties.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR;
    qualityLevelProperties.pNext = &h264QualityLevelProperties;

    VK_CHECK(vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(m_physicalDevice, &qualityLevelInfo,
                                                                  &qualityLevelProperties));

    VkPhysicalDeviceVideoFormatInfoKHR videoFormatInfo = {};
    videoFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    videoFormatInfo.pNext = &m_videoProfileList;
    videoFormatInfo.imageUsage =
        VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    uint32_t videoFormatPropertyCount;
    VK_CHECK(vkGetPhysicalDeviceVideoFormatPropertiesKHR(m_physicalDevice, &videoFormatInfo, &videoFormatPropertyCount,
                                                nullptr));
    std::vector<VkVideoFormatPropertiesKHR> srcVideoFormatProperties(videoFormatPropertyCount);
    for (uint32_t i = 0; i < videoFormatPropertyCount; i++) {
        memset(&srcVideoFormatProperties[i], 0, sizeof(VkVideoFormatPropertiesKHR));
        srcVideoFormatProperties[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    VK_CHECK(vkGetPhysicalDeviceVideoFormatPropertiesKHR(m_physicalDevice, &videoFormatInfo, &videoFormatPropertyCount,
                                                         srcVideoFormatProperties.data()));

    m_chosenSrcImageFormat = VK_FORMAT_UNDEFINED;
    for (const auto& formatProperties: srcVideoFormatProperties) {
        if (formatProperties.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
            // Nvidia driver supports mutable & extended usage, but is not returning those flags
            //constexpr VkImageCreateFlags neededCreateFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            //if ((formatProperties.imageCreateFlags & neededCreateFlags) != neededCreateFlags) {
            //    printf("Skipping format %d, imageCreateFlags not supported\n", formatProperties.format);
            //    continue;
            //}
            m_chosenSrcImageFormat = formatProperties.format;
            break;
        }
    }
    if (m_chosenSrcImageFormat == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("Error: no supported video encode source image format");

    videoFormatInfo.imageUsage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
    VK_CHECK(vkGetPhysicalDeviceVideoFormatPropertiesKHR(m_physicalDevice, &videoFormatInfo, &videoFormatPropertyCount,
                                                         nullptr));
    std::vector<VkVideoFormatPropertiesKHR> dpbVideoFormatProperties(videoFormatPropertyCount);
    for (uint32_t i = 0; i < videoFormatPropertyCount; i++) {
        memset(&dpbVideoFormatProperties[i], 0, sizeof(VkVideoFormatPropertiesKHR));
        dpbVideoFormatProperties[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    VK_CHECK(vkGetPhysicalDeviceVideoFormatPropertiesKHR(m_physicalDevice, &videoFormatInfo, &videoFormatPropertyCount,
                                                         dpbVideoFormatProperties.data()));
    if (dpbVideoFormatProperties.size() < 1)
        throw std::runtime_error("Error: no supported video encode DPB image format");
    m_chosenDpbImageFormat = dpbVideoFormatProperties[0].format;

    static const VkExtensionProperties h264StdExtensionVersion = {VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME,
                                                                  VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION};
    VkVideoSessionCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    createInfo.pVideoProfile = &m_videoProfile;
    createInfo.queueFamilyIndex = m_encodeQueueFamily;
    createInfo.pictureFormat = m_chosenSrcImageFormat;
    createInfo.maxCodedExtent = {m_width, m_height};
    createInfo.maxDpbSlots = 16;
    createInfo.maxActiveReferencePictures = 16;
    createInfo.referencePictureFormat = m_chosenDpbImageFormat;
    createInfo.pStdHeaderVersion = &h264StdExtensionVersion;

    VK_CHECK(vkCreateVideoSessionKHR(m_device, &createInfo, nullptr, &m_videoSession));
}

void VideoEncoder::allocateVideoSessionMemory() {
    uint32_t videoSessionMemoryRequirementsCount = 0;
    VK_CHECK(vkGetVideoSessionMemoryRequirementsKHR(m_device, m_videoSession, &videoSessionMemoryRequirementsCount,
                                                    nullptr));
    std::vector<VkVideoSessionMemoryRequirementsKHR> encodeSessionMemoryRequirements(
        videoSessionMemoryRequirementsCount);
    for (uint32_t i = 0; i < videoSessionMemoryRequirementsCount; i++) {
        memset(&encodeSessionMemoryRequirements[i], 0, sizeof(VkVideoSessionMemoryRequirementsKHR));
        encodeSessionMemoryRequirements[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    VK_CHECK(vkGetVideoSessionMemoryRequirementsKHR(m_device, m_videoSession, &videoSessionMemoryRequirementsCount,
                                                    encodeSessionMemoryRequirements.data()));
    if (videoSessionMemoryRequirementsCount == 0) {
        return;
    }

    std::vector<VkBindVideoSessionMemoryInfoKHR> encodeSessionBindMemory(videoSessionMemoryRequirementsCount);
    m_allocations.resize(videoSessionMemoryRequirementsCount);
    for (uint32_t memIdx = 0; memIdx < videoSessionMemoryRequirementsCount; memIdx++) {
        VmaAllocationCreateInfo allocCreateInfo = {};
        allocCreateInfo.memoryTypeBits = encodeSessionMemoryRequirements[memIdx].memoryRequirements.memoryTypeBits;

        VmaAllocationInfo allocInfo;
        VK_CHECK(vmaAllocateMemory(m_allocator, &encodeSessionMemoryRequirements[memIdx].memoryRequirements,
                                   &allocCreateInfo, &m_allocations[memIdx], &allocInfo));

        encodeSessionBindMemory[memIdx].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        encodeSessionBindMemory[memIdx].pNext = nullptr;
        encodeSessionBindMemory[memIdx].memory = allocInfo.deviceMemory;

        encodeSessionBindMemory[memIdx].memoryBindIndex = encodeSessionMemoryRequirements[memIdx].memoryBindIndex;
        encodeSessionBindMemory[memIdx].memoryOffset = allocInfo.offset;
        encodeSessionBindMemory[memIdx].memorySize = allocInfo.size;
    }
    VK_CHECK(vkBindVideoSessionMemoryKHR(m_device, m_videoSession, videoSessionMemoryRequirementsCount,
                                         encodeSessionBindMemory.data()));
}

void VideoEncoder::createVideoSessionParameters(uint32_t fps) {
    m_vui = h264::getStdVideoH264SequenceParameterSetVui(fps);
    m_sps = h264::getStdVideoH264SequenceParameterSet(m_width, m_height, &m_vui);
    m_pps = h264::getStdVideoH264PictureParameterSet();

    VkVideoEncodeH264SessionParametersAddInfoKHR encodeH264SessionParametersAddInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    encodeH264SessionParametersAddInfo.pNext = nullptr;
    encodeH264SessionParametersAddInfo.stdSPSCount = 1;
    encodeH264SessionParametersAddInfo.pStdSPSs = &m_sps;
    encodeH264SessionParametersAddInfo.stdPPSCount = 1;
    encodeH264SessionParametersAddInfo.pStdPPSs = &m_pps;

    VkVideoEncodeH264SessionParametersCreateInfoKHR encodeH264SessionParametersCreateInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    encodeH264SessionParametersCreateInfo.pNext = nullptr;
    encodeH264SessionParametersCreateInfo.maxStdSPSCount = 1;
    encodeH264SessionParametersCreateInfo.maxStdPPSCount = 1;
    encodeH264SessionParametersCreateInfo.pParametersAddInfo = &encodeH264SessionParametersAddInfo;

    VkVideoSessionParametersCreateInfoKHR sessionParametersCreateInfo = {
        VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    sessionParametersCreateInfo.pNext = &encodeH264SessionParametersCreateInfo;
    sessionParametersCreateInfo.videoSessionParametersTemplate = nullptr;
    sessionParametersCreateInfo.videoSession = m_videoSession;

    VK_CHECK(
        vkCreateVideoSessionParametersKHR(m_device, &sessionParametersCreateInfo, nullptr, &m_videoSessionParameters));
}

void VideoEncoder::readBitstreamHeader() {
    VkVideoEncodeH264SessionParametersGetInfoKHR h264getInfo = {};
    h264getInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR;
    h264getInfo.stdSPSId = 0;
    h264getInfo.stdPPSId = 0;
    h264getInfo.writeStdPPS = VK_TRUE;
    h264getInfo.writeStdSPS = VK_TRUE;
    VkVideoEncodeSessionParametersGetInfoKHR getInfo = {};
    getInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
    getInfo.pNext = &h264getInfo;
    getInfo.videoSessionParameters = m_videoSessionParameters;

    VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264feedback = {};
    h264feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
    VkVideoEncodeSessionParametersFeedbackInfoKHR feedback = {};
    feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
    feedback.pNext = &h264feedback;
    size_t datalen = 1024;
    VK_CHECK(vkGetEncodedVideoSessionParametersKHR(m_device, &getInfo, nullptr, &datalen, nullptr));
    m_bitStreamHeader.resize(datalen);
    VK_CHECK(vkGetEncodedVideoSessionParametersKHR(m_device, &getInfo, &feedback, &datalen, m_bitStreamHeader.data()));
    m_bitStreamHeader.resize(datalen);
    m_bitStreamHeaderPending = true;
}

void VideoEncoder::allocateOutputBitStream() {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = 4 * 1024 * 1024;
    bufferInfo.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufferInfo.pNext = &m_videoProfileList;
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    VK_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &m_bitStreamBuffer, &m_bitStreamBufferAllocation,
                             nullptr));

    VK_CHECK(vmaMapMemory(m_allocator, m_bitStreamBufferAllocation, reinterpret_cast<void**>(&m_bitStreamData)));
}

void VideoEncoder::allocateReferenceImages(uint32_t count) {
    m_dpbImages.resize(count);
    m_dpbImageAllocations.resize(count);
    m_dpbImageViews.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkImageCreateInfo tmpImgCreateInfo;
        tmpImgCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        tmpImgCreateInfo.pNext = &m_videoProfileList;
        tmpImgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        tmpImgCreateInfo.format = m_chosenDpbImageFormat;
        tmpImgCreateInfo.extent = {m_width, m_height, 1};
        tmpImgCreateInfo.mipLevels = 1;
        tmpImgCreateInfo.arrayLayers = 1;
        tmpImgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        tmpImgCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        tmpImgCreateInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;  // DPB ONLY
        tmpImgCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        tmpImgCreateInfo.queueFamilyIndexCount = 1;
        tmpImgCreateInfo.pQueueFamilyIndices = &m_encodeQueueFamily;
        tmpImgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        tmpImgCreateInfo.flags = 0;
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VK_CHECK(vmaCreateImage(m_allocator, &tmpImgCreateInfo, &allocInfo, &m_dpbImages[i], &m_dpbImageAllocations[i],
                                nullptr));
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_dpbImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_chosenDpbImageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_dpbImageViews[i]));
    }
}

void VideoEncoder::allocateIntermediateImages() {
    uint32_t queueFamilies[] = {m_computeQueueFamily, m_encodeQueueFamily};
    VkImageCreateInfo tmpImgCreateInfo;
    tmpImgCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tmpImgCreateInfo.pNext = &m_videoProfileList;
    tmpImgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    tmpImgCreateInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    tmpImgCreateInfo.extent = {m_width, m_height, 1};
    tmpImgCreateInfo.mipLevels = 1;
    tmpImgCreateInfo.arrayLayers = 1;
    tmpImgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    tmpImgCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    tmpImgCreateInfo.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (m_computeQueueFamily == m_encodeQueueFamily) {
        tmpImgCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        tmpImgCreateInfo.queueFamilyIndexCount = 0;
        tmpImgCreateInfo.pQueueFamilyIndices = nullptr;
    } else {
        tmpImgCreateInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        tmpImgCreateInfo.queueFamilyIndexCount = 2;
        tmpImgCreateInfo.pQueueFamilyIndices = queueFamilies;
    }
    tmpImgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    tmpImgCreateInfo.flags = 0;
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(
        vmaCreateImage(m_allocator, &tmpImgCreateInfo, &allocInfo, &m_yCbCrImage, &m_yCbCrImageAllocation, nullptr));
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_yCbCrImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_yCbCrImageView));

    tmpImgCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    tmpImgCreateInfo.pNext = nullptr;
    tmpImgCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    tmpImgCreateInfo.queueFamilyIndexCount = 0;
    tmpImgCreateInfo.pQueueFamilyIndices = nullptr;

    tmpImgCreateInfo.format = VK_FORMAT_R8_UNORM;
    VK_CHECK(vmaCreateImage(m_allocator, &tmpImgCreateInfo, &allocInfo, &m_yCbCrImageLuma, &m_yCbCrImageLumaAllocation,
                            nullptr));
    viewInfo.image = m_yCbCrImageLuma;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_yCbCrImageLumaView));

    tmpImgCreateInfo.format = VK_FORMAT_R8G8_UNORM;
    tmpImgCreateInfo.extent = {m_width / 2, m_height / 2, 1};
    VK_CHECK(vmaCreateImage(m_allocator, &tmpImgCreateInfo, &allocInfo, &m_yCbCrImageChroma,
                            &m_yCbCrImageChromaAllocation, nullptr));
    viewInfo.image = m_yCbCrImageChroma;
    viewInfo.format = VK_FORMAT_R8G8_UNORM;
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &m_yCbCrImageChromaView));
}

void VideoEncoder::createOutputQueryPool() {
    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR queryPoolVideoEncodeFeedbackCreateInfo = {
        VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
    queryPoolVideoEncodeFeedbackCreateInfo.encodeFeedbackFlags =
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
    queryPoolVideoEncodeFeedbackCreateInfo.pNext = &m_videoProfile;
    VkQueryPoolCreateInfo queryPoolCreateInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryPoolCreateInfo.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
    queryPoolCreateInfo.queryCount = 1;
    queryPoolCreateInfo.pNext = &queryPoolVideoEncodeFeedbackCreateInfo;
    VK_CHECK(vkCreateQueryPool(m_device, &queryPoolCreateInfo, NULL, &m_queryPool));
}

void VideoEncoder::createYCbCrConversionPipeline(const std::vector<VkImageView>& inputImageViews) {
    auto computeShaderCode = readFile("shaders/rgb-ycbcr-shader.comp.spv");
    VkShaderModuleCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = computeShaderCode.size(),
                                        .pCode = reinterpret_cast<const uint32_t*>(computeShaderCode.data())};
    VkShaderModule computeShaderModule;
    VK_CHECK(vkCreateShaderModule(m_device, &createInfo, nullptr, &computeShaderModule));
    VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
    computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    computeShaderStageInfo.module = computeShaderModule;
    computeShaderStageInfo.pName = "main";

    std::array<VkDescriptorSetLayoutBinding, 3> layoutBindings{};
    for (uint32_t i = 0; i < layoutBindings.size(); i++) {
        layoutBindings[i].binding = i;
        layoutBindings[i].descriptorCount = 1;
        layoutBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        layoutBindings[i].pImmutableSamplers = nullptr;
        layoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = (uint32_t)layoutBindings.size();
    layoutInfo.pBindings = layoutBindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_computeDescriptorSetLayout));

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_computeDescriptorSetLayout;
    VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_computePipelineLayout));

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = m_computePipelineLayout;
    pipelineInfo.stage = computeShaderStageInfo;
    VK_CHECK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline));

    vkDestroyShaderModule(m_device, computeShaderModule, nullptr);

    const int maxFramesCount = static_cast<uint32_t>(inputImageViews.size());
    std::array<VkDescriptorPoolSize, 1> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 3 * maxFramesCount;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = maxFramesCount;
    VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool));

    std::vector<VkDescriptorSetLayout> layouts(maxFramesCount, m_computeDescriptorSetLayout);
    VkDescriptorSetAllocateInfo descAllocInfo{};
    descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descAllocInfo.descriptorPool = m_descriptorPool;
    descAllocInfo.descriptorSetCount = maxFramesCount;
    descAllocInfo.pSetLayouts = layouts.data();

    m_computeDescriptorSets.resize(maxFramesCount);
    VK_CHECK(vkAllocateDescriptorSets(m_device, &descAllocInfo, m_computeDescriptorSets.data()));
    for (size_t i = 0; i < maxFramesCount; i++) {
        std::array<VkWriteDescriptorSet, 3> descriptorWrites{};

        VkDescriptorImageInfo imageInfo0{};
        imageInfo0.imageView = inputImageViews[i];
        imageInfo0.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = m_computeDescriptorSets[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pImageInfo = &imageInfo0;

        VkDescriptorImageInfo imageInfo1{};
        imageInfo1.imageView = m_yCbCrImageLumaView;
        imageInfo1.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = m_computeDescriptorSets[i];
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pImageInfo = &imageInfo1;

        VkDescriptorImageInfo imageInfo2{};
        imageInfo2.imageView = m_yCbCrImageChromaView;
        imageInfo2.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[2].dstSet = m_computeDescriptorSets[i];
        descriptorWrites[2].dstBinding = 2;
        descriptorWrites[2].dstArrayElement = 0;
        descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptorWrites[2].descriptorCount = 1;
        descriptorWrites[2].pImageInfo = &imageInfo2;

        vkUpdateDescriptorSets(m_device, (uint32_t)descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);
    }
}

void VideoEncoder::initRateControl(VkCommandBuffer cmdBuf, uint32_t fps) {
    VkVideoBeginCodingInfoKHR encodeBeginInfo = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    encodeBeginInfo.videoSession = m_videoSession;
    encodeBeginInfo.videoSessionParameters = m_videoSessionParameters;

    m_encodeRateControlLayerInfo.pNext = &m_encodeH264RateControlLayerInfo;
    m_encodeRateControlLayerInfo.frameRateNumerator = fps;
    m_encodeRateControlLayerInfo.frameRateDenominator = 1;
    m_encodeRateControlLayerInfo.averageBitrate = 5000000;
    m_encodeRateControlLayerInfo.maxBitrate = 20000000;

    m_encodeH264RateControlInfo.flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR |
                                        VK_VIDEO_ENCODE_H264_RATE_CONTROL_REFERENCE_PATTERN_FLAT_BIT_KHR;
    m_encodeH264RateControlInfo.gopFrameCount = 16;
    m_encodeH264RateControlInfo.idrPeriod = 16;
    m_encodeH264RateControlInfo.consecutiveBFrameCount = 0;
    m_encodeH264RateControlInfo.temporalLayerCount = 1;

    m_encodeRateControlInfo.rateControlMode = m_chosenRateControlMode;
    m_encodeRateControlInfo.pNext = &m_encodeH264RateControlInfo;
    m_encodeRateControlInfo.layerCount = 1;
    m_encodeRateControlInfo.pLayers = &m_encodeRateControlLayerInfo;
    m_encodeRateControlInfo.initialVirtualBufferSizeInMs = 100;
    m_encodeRateControlInfo.virtualBufferSizeInMs = 200;

    VkVideoCodingControlInfoKHR codingControlInfo = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    codingControlInfo.flags =
        VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR | VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
    codingControlInfo.pNext = &m_encodeRateControlInfo;

    if (m_encodeRateControlInfo.rateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR) {
        m_encodeRateControlLayerInfo.averageBitrate = m_encodeRateControlLayerInfo.maxBitrate;
    }
    if (m_encodeRateControlInfo.rateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR ||
        m_encodeRateControlInfo.rateControlMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {
        m_encodeH264RateControlInfo.temporalLayerCount = 0;
        m_encodeRateControlInfo.layerCount = 0;
    }

    VkVideoEndCodingInfoKHR encodeEndInfo = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};

    // Reset the video session before first use and apply QP values.
    vkCmdBeginVideoCodingKHR(cmdBuf, &encodeBeginInfo);
    vkCmdControlVideoCodingKHR(cmdBuf, &codingControlInfo);
    vkCmdEndVideoCodingKHR(cmdBuf, &encodeEndInfo);
}

void VideoEncoder::transitionImagesInitial(VkCommandBuffer cmdBuf) {
    std::vector<VkImageMemoryBarrier2> barriers;
    VkImageMemoryBarrier2 imageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                             .srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                             .dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                             .subresourceRange = {
                                                 .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                 .baseMipLevel = 0,
                                                 .levelCount = 1,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1,
                                             }};
    for (auto& dpbImage : m_dpbImages) {
        imageMemoryBarrier.image = dpbImage;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        barriers.push_back(imageMemoryBarrier);
    }

    VkDependencyInfoKHR dependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
                                       .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                       .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(cmdBuf, &dependencyInfo);
}

void VideoEncoder::convertRGBtoYCbCr(uint32_t currentImageIx) {
    // begin command buffer for compute shader
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_computeCommandPool;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &m_computeCommandBuffer));
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(m_computeCommandBuffer, &beginInfo));

    std::vector<VkImageMemoryBarrier2> barriers;
    VkImageMemoryBarrier2 imageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                             .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                             .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                             .subresourceRange = {
                                                 .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                 .baseMipLevel = 0,
                                                 .levelCount = 1,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1,
                                             }};
    // transition YCbCr image (luma and chroma) to be shader target
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageMemoryBarrier.image = m_yCbCrImageLuma;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers.push_back(imageMemoryBarrier);
    imageMemoryBarrier.image = m_yCbCrImageChroma;
    barriers.push_back(imageMemoryBarrier);
    // transition source image to be shader source
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imageMemoryBarrier.image = m_inputImages[currentImageIx];
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barriers.push_back(imageMemoryBarrier);
    VkDependencyInfoKHR dependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
                                       .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                       .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(m_computeCommandBuffer, &dependencyInfo);

    // run the RGB->YCbCr conversion shader
    vkCmdBindPipeline(m_computeCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(m_computeCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1,
                            &m_computeDescriptorSets[currentImageIx], 0, 0);
    vkCmdDispatch(m_computeCommandBuffer, (m_width + 15) / 16, (m_height + 15) / 16,
                  1);  // work item local size = 16x16

    barriers.clear();
    // transition the luma and chroma images to be copy source
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageMemoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imageMemoryBarrier.image = m_yCbCrImageLuma;
    barriers.push_back(imageMemoryBarrier);
    imageMemoryBarrier.image = m_yCbCrImageChroma;
    barriers.push_back(imageMemoryBarrier);

    // transition the full YCbCr image as copy target
    imageMemoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_2_NONE;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
    imageMemoryBarrier.image = m_yCbCrImage;
    barriers.push_back(imageMemoryBarrier);

    dependencyInfo.imageMemoryBarrierCount = barriers.size();
    dependencyInfo.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(m_computeCommandBuffer, &dependencyInfo);

    // copy the full luma image into the 1st plane of the YCbCr image
    VkImageCopy regions;
    regions.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    regions.srcSubresource.baseArrayLayer = 0;
    regions.srcSubresource.layerCount = 1;
    regions.srcSubresource.mipLevel = 0;
    regions.srcOffset = {0, 0, 0};
    regions.dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regions.dstSubresource.baseArrayLayer = 0;
    regions.dstSubresource.layerCount = 1;
    regions.dstSubresource.mipLevel = 0;
    regions.dstOffset = {0, 0, 0};
    regions.extent = {m_width, m_height, 1};
    vkCmdCopyImage(m_computeCommandBuffer, m_yCbCrImageLuma, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_yCbCrImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions);

    // copy the full chroma image into the 2nd plane of the YCbCr image
    regions.dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regions.extent = {m_width / 2, m_height / 2, 1};
    vkCmdCopyImage(m_computeCommandBuffer, m_yCbCrImageChroma, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_yCbCrImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions);

    VK_CHECK(vkEndCommandBuffer(m_computeCommandBuffer));
    VkPipelineStageFlags dstStageMasks[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkSemaphore signalSemaphores[] = {m_interQueueSemaphore, m_interQueueSemaphore3};
    VkSemaphore waitSemaphores[] = {m_interQueueSemaphore2, m_interQueueSemaphore3};
    VkSubmitInfo submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &m_computeCommandBuffer,
                            .signalSemaphoreCount = 2,
                            .pSignalSemaphores = signalSemaphores};
    if (m_frameCount != 0) {
        submitInfo.waitSemaphoreCount = 2;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = dstStageMasks;
    }
    VK_CHECK(vkQueueSubmit(m_computeQueue, 1, &submitInfo, VK_NULL_HANDLE));
}

void VideoEncoder::encodeVideoFrame() {
    const uint32_t GOP_LENGTH = 16;
    const uint32_t gopFrameCount = m_frameCount % GOP_LENGTH;
    // begin command buffer for video encode
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_encodeCommandPool;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &m_encodeCommandBuffer));
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(m_encodeCommandBuffer, &beginInfo));
    const uint32_t querySlotId = 0;
    vkCmdResetQueryPool(m_encodeCommandBuffer, m_queryPool, querySlotId, 1);

    // start a video encode session
    // set an image view as DPB (decoded output picture)
    VkVideoPictureResourceInfoKHR dpbPicResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    dpbPicResource.imageViewBinding = m_dpbImageViews[gopFrameCount & 1];
    dpbPicResource.codedOffset = {0, 0};
    dpbPicResource.codedExtent = {m_width, m_height};
    dpbPicResource.baseArrayLayer = 0;
    // set an image view as reference picture
    VkVideoPictureResourceInfoKHR refPicResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    refPicResource.imageViewBinding = m_dpbImageViews[!(gopFrameCount & 1)];
    refPicResource.codedOffset = {0, 0};
    refPicResource.codedExtent = {m_width, m_height};
    refPicResource.baseArrayLayer = 0;

    const uint32_t MaxPicOrderCntLsb = 1 << (m_sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    StdVideoEncodeH264ReferenceInfo dpbRefInfo = {};
    dpbRefInfo.FrameNum = gopFrameCount;
    dpbRefInfo.PicOrderCnt = (dpbRefInfo.FrameNum * 2) % MaxPicOrderCntLsb;
    dpbRefInfo.primary_pic_type =
        dpbRefInfo.FrameNum == 0 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    VkVideoEncodeH264DpbSlotInfoKHR dpbSlotInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
    dpbSlotInfo.pNext = nullptr;
    dpbSlotInfo.pStdReferenceInfo = &dpbRefInfo;

    StdVideoEncodeH264ReferenceInfo refRefInfo = {};
    refRefInfo.FrameNum = gopFrameCount - 1;
    refRefInfo.PicOrderCnt = (refRefInfo.FrameNum * 2) % MaxPicOrderCntLsb;
    refRefInfo.primary_pic_type =
        refRefInfo.FrameNum == 0 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    VkVideoEncodeH264DpbSlotInfoKHR refSlotInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
    refSlotInfo.pNext = nullptr;
    refSlotInfo.pStdReferenceInfo = &refRefInfo;

    VkVideoReferenceSlotInfoKHR referenceSlots[2];
    referenceSlots[0].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    referenceSlots[0].pNext = &dpbSlotInfo;
    referenceSlots[0].slotIndex = -1;
    referenceSlots[0].pPictureResource = &dpbPicResource;
    referenceSlots[1].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    referenceSlots[1].pNext = &refSlotInfo;
    referenceSlots[1].slotIndex = !(gopFrameCount & 1);
    referenceSlots[1].pPictureResource = &refPicResource;

    VkVideoBeginCodingInfoKHR encodeBeginInfo = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    encodeBeginInfo.pNext = &m_encodeRateControlInfo;
    encodeBeginInfo.videoSession = m_videoSession;
    encodeBeginInfo.videoSessionParameters = m_videoSessionParameters;
    encodeBeginInfo.referenceSlotCount = gopFrameCount == 0 ? 1 : 2;
    encodeBeginInfo.pReferenceSlots = referenceSlots;
    vkCmdBeginVideoCodingKHR(m_encodeCommandBuffer, &encodeBeginInfo);

    // transition the YCbCr image to be a video encode source
    VkImageMemoryBarrier2 imageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
        .image = m_yCbCrImage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }};
    VkDependencyInfoKHR dependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
                                       .imageMemoryBarrierCount = 1,
                                       .pImageMemoryBarriers = &imageMemoryBarrier};
    vkCmdPipelineBarrier2(m_encodeCommandBuffer, &dependencyInfo);

    // set the YCbCr image as input picture for the encoder
    VkVideoPictureResourceInfoKHR inputPicResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    inputPicResource.imageViewBinding = m_yCbCrImageView;
    inputPicResource.codedOffset = {0, 0};
    inputPicResource.codedExtent = {m_width, m_height};
    inputPicResource.baseArrayLayer = 0;

    // set all the frame parameters
    h264::FrameInfo frameInfo(gopFrameCount, m_width, m_height, m_sps, m_pps, gopFrameCount,
                              m_chosenRateControlMode & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR);
    VkVideoEncodeH264PictureInfoKHR* encodeH264FrameInfo = frameInfo.getEncodeH264FrameInfo();

    // combine all structures in one control structure
    VkVideoEncodeInfoKHR videoEncodeInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    videoEncodeInfo.pNext = encodeH264FrameInfo;
    videoEncodeInfo.dstBuffer = m_bitStreamBuffer;
    videoEncodeInfo.dstBufferOffset = 0;
    videoEncodeInfo.dstBufferRange = 4 * 1024 * 1024;
    videoEncodeInfo.srcPictureResource = inputPicResource;
    referenceSlots[0].slotIndex = gopFrameCount & 1;
    videoEncodeInfo.pSetupReferenceSlot = &referenceSlots[0];

    if (gopFrameCount > 0) {
        videoEncodeInfo.referenceSlotCount = 1;
        videoEncodeInfo.pReferenceSlots = &referenceSlots[1];
    }

    // prepare the query pool for the resulting bitstream
    vkCmdBeginQuery(m_encodeCommandBuffer, m_queryPool, querySlotId, VkQueryControlFlags());
    // encode the frame as video
    vkCmdEncodeVideoKHR(m_encodeCommandBuffer, &videoEncodeInfo);
    // end the query for the result
    vkCmdEndQuery(m_encodeCommandBuffer, m_queryPool, querySlotId);
    // finish the video session
    VkVideoEndCodingInfoKHR encodeEndInfo = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(m_encodeCommandBuffer, &encodeEndInfo);

    // run the encoding
    VK_CHECK(vkEndCommandBuffer(m_encodeCommandBuffer));
    VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &m_interQueueSemaphore,
                            .pWaitDstStageMask = &dstStageMask,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &m_encodeCommandBuffer,
                            .signalSemaphoreCount = 1,
                            .pSignalSemaphores = &m_interQueueSemaphore2};
    VK_CHECK(vkResetFences(m_device, 1, &m_encodeFinishedFence));
    VK_CHECK(vkQueueSubmit(m_encodeQueue, 1, &submitInfo, m_encodeFinishedFence));
}

void VideoEncoder::getOutputVideoPacket(const char*& data, size_t& size) {
    VK_CHECK(vkWaitForFences(m_device, 1, &m_encodeFinishedFence, VK_TRUE, std::numeric_limits<uint64_t>::max()));

    struct videoEncodeStatus {
        uint32_t bitstreamStartOffset;
        uint32_t bitstreamSize;
        VkQueryResultStatusKHR status;
    };
    // get the resulting bitstream
    videoEncodeStatus encodeResult;  // 2nd slot is non vcl data
    memset(&encodeResult, 0, sizeof(encodeResult));
    const uint32_t querySlotId = 0;
    VK_CHECK(vkGetQueryPoolResults(m_device, m_queryPool, querySlotId, 1, sizeof(videoEncodeStatus), &encodeResult,
                                   sizeof(videoEncodeStatus),
                                   VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT));

    //  invalidate host caches
    vmaInvalidateAllocation(m_allocator, m_bitStreamBufferAllocation, encodeResult.bitstreamStartOffset,
                            encodeResult.bitstreamSize);
    // return bitstream
    data = m_bitStreamData + encodeResult.bitstreamStartOffset;
    size = encodeResult.bitstreamSize;
    printf("Encoded frame %d, status %d, offset %d, size %zd\n", m_frameCount, encodeResult.status,
           encodeResult.bitstreamStartOffset, size);
}

void VideoEncoder::deinit() {
    if (!m_initialized) {
        return;
    }

    if (m_running) {
        const char* data;
        size_t size;
        getOutputVideoPacket(data, size);
        vkFreeCommandBuffers(m_device, m_computeCommandPool, 1, &m_computeCommandBuffer);
        vkFreeCommandBuffers(m_device, m_encodeCommandPool, 1, &m_encodeCommandBuffer);
    }
    vkDestroyFence(m_device, m_encodeFinishedFence, nullptr);
    vkDestroySemaphore(m_device, m_interQueueSemaphore, nullptr);
    vkDestroySemaphore(m_device, m_interQueueSemaphore2, nullptr);
    vkDestroySemaphore(m_device, m_interQueueSemaphore3, nullptr);
    vkDestroyPipeline(m_device, m_computePipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_computeDescriptorSetLayout, nullptr);

    vkDestroyVideoSessionParametersKHR(m_device, m_videoSessionParameters, nullptr);
    vkDestroyQueryPool(m_device, m_queryPool, nullptr);
    vmaUnmapMemory(m_allocator, m_bitStreamBufferAllocation);
    vmaDestroyBuffer(m_allocator, m_bitStreamBuffer, m_bitStreamBufferAllocation);
    vkDestroyImageView(m_device, m_yCbCrImageChromaView, nullptr);
    vmaDestroyImage(m_allocator, m_yCbCrImageChroma, m_yCbCrImageChromaAllocation);
    vkDestroyImageView(m_device, m_yCbCrImageLumaView, nullptr);
    vmaDestroyImage(m_allocator, m_yCbCrImageLuma, m_yCbCrImageLumaAllocation);
    vkDestroyImageView(m_device, m_yCbCrImageView, nullptr);
    vmaDestroyImage(m_allocator, m_yCbCrImage, m_yCbCrImageAllocation);
    for (uint32_t i = 0; i < m_dpbImages.size(); i++) {
        vkDestroyImageView(m_device, m_dpbImageViews[i], nullptr);
        vmaDestroyImage(m_allocator, m_dpbImages[i], m_dpbImageAllocations[i]);
    }
    vkDestroyVideoSessionKHR(m_device, m_videoSession, nullptr);
    for (VmaAllocation& allocation : m_allocations) {
        vmaFreeMemory(m_allocator, allocation);
    }
    m_allocations.clear();
    m_bitStreamHeader.clear();
    vkDestroyCommandPool(m_device, m_encodeCommandPool, nullptr);

    m_initialized = false;
}
