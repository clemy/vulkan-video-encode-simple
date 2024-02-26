/*
 * Vulkan Video Encode Extension - Simple Example
 * Copyright (c) 2024 Bernhard C. Schrenk <clemy@clemy.org>
 *
 * This file is licensed under the MIT license.
 * See the LICENSE file in the project root for full license information.
 */

#pragma once
#define VK_NO_PROTOTYPES
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

#include <vector>

#include "h264parameterset.hpp"

class VideoEncoder {
   public:
    void init(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator, uint32_t computeQueueFamily,
              VkQueue computeQueue, VkCommandPool computeCommandPool, uint32_t encodeQueueFamily, VkQueue encodeQueue,
              const std::vector<VkImage>& inputImages, const std::vector<VkImageView>& inputImageViews, uint32_t width,
              uint32_t height, uint32_t fps);
    void queueEncode(uint32_t currentImageIx);
    void finishEncode(const char*& data, size_t& size);
    void deinit();

    ~VideoEncoder() { deinit(); }

   private:
    void createEncodeCommandPool();
    void createVideoSession();
    void allocateVideoSessionMemory();
    void createVideoSessionParameters(uint32_t fps);
    void readBitstreamHeader();
    void allocateOutputBitStream();
    void allocateReferenceImages(uint32_t count);
    void allocateIntermediateImages();
    void createOutputQueryPool();
    void createYCbCrConversionPipeline(const std::vector<VkImageView>& inputImageViews);
    void initRateControl(VkCommandBuffer cmdBuf, uint32_t fps);
    void transitionImagesInitial(VkCommandBuffer cmdBuf);

    void convertRGBtoYCbCr(uint32_t currentImageIx);
    void encodeVideoFrame();
    void getOutputVideoPacket(const char*& data, size_t& size);

    bool m_initialized{false};
    bool m_running{false};
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    VmaAllocator m_allocator;
    VkQueue m_computeQueue;
    VkQueue m_encodeQueue;
    uint32_t m_computeQueueFamily;
    uint32_t m_encodeQueueFamily;
    VkCommandPool m_computeCommandPool;
    VkCommandPool m_encodeCommandPool;
    std::vector<VkImage> m_inputImages;
    uint32_t m_width;
    uint32_t m_height;

    VkVideoSessionKHR m_videoSession;
    std::vector<VmaAllocation> m_allocations;
    StdVideoH264SequenceParameterSetVui m_vui;
    StdVideoH264SequenceParameterSet m_sps;
    StdVideoH264PictureParameterSet m_pps;
    VkVideoSessionParametersKHR m_videoSessionParameters;
    VkVideoEncodeH264ProfileInfoKHR m_encodeH264ProfileInfoExt;
    VkVideoProfileInfoKHR m_videoProfile;
    VkVideoProfileListInfoKHR m_videoProfileList;

    VkVideoEncodeH264RateControlLayerInfoKHR m_encodeH264RateControlLayerInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR};
    VkVideoEncodeRateControlLayerInfoKHR m_encodeRateControlLayerInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR};
    VkVideoEncodeH264RateControlInfoKHR m_encodeH264RateControlInfo = {
        VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR};
    VkVideoEncodeRateControlInfoKHR m_encodeRateControlInfo = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};

    VkDescriptorSetLayout m_computeDescriptorSetLayout;
    VkPipelineLayout m_computePipelineLayout;
    VkPipeline m_computePipeline;
    VkDescriptorPool m_descriptorPool;
    std::vector<VkDescriptorSet> m_computeDescriptorSets;

    VkSemaphore m_interQueueSemaphore;

    VkQueryPool m_queryPool;
    VkBuffer m_bitStreamBuffer;
    VmaAllocation m_bitStreamBufferAllocation;
    std::vector<char> m_bitStreamHeader;
    bool m_bitStreamHeaderPending;

    char* m_bitStreamData;

    VkImage m_yCbCrImage;
    VmaAllocation m_yCbCrImageAllocation;
    VkImageView m_yCbCrImageView;
    VkImage m_yCbCrImageLuma;
    VmaAllocation m_yCbCrImageLumaAllocation;
    VkImageView m_yCbCrImageLumaView;
    VkImage m_yCbCrImageChroma;
    VmaAllocation m_yCbCrImageChromaAllocation;
    VkImageView m_yCbCrImageChromaView;

    std::vector<VkImage> m_dpbImages;
    std::vector<VmaAllocation> m_dpbImageAllocations;
    std::vector<VkImageView> m_dpbImageViews;

    uint32_t m_frameCount;

    VkFence m_encodeFinishedFence;
    VkCommandBuffer m_computeCommandBuffer;
    VkCommandBuffer m_encodeCommandBuffer;
};
