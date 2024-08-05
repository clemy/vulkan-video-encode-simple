#pragma once
#include <cassert>
#include <cmath>

#include "vk_video/vulkan_video_codec_h264std.h"
#include "vk_video/vulkan_video_codecs_common.h"

namespace h264 {
// adapted from Nvidia sample code

static const uint32_t H264MbSizeAlignment = 16;

template <typename sizeType>
static sizeType AlignSize(sizeType size, sizeType alignment) {
    assert((alignment & (alignment - 1)) == 0);
    return (size + alignment - 1) & ~(alignment - 1);
}

static StdVideoH264SequenceParameterSetVui getStdVideoH264SequenceParameterSetVui(uint32_t fps) {
    StdVideoH264SpsVuiFlags vuiFlags = {};
    vuiFlags.timing_info_present_flag = 1u;
    vuiFlags.fixed_frame_rate_flag = 1u;

    StdVideoH264SequenceParameterSetVui vui = {};
    vui.flags = vuiFlags;
    vui.num_units_in_tick = 1;
    vui.time_scale = fps * 2;  // 2 fields

    return vui;
}

static StdVideoH264SequenceParameterSet getStdVideoH264SequenceParameterSet(uint32_t width, uint32_t height,
                                                                            StdVideoH264SequenceParameterSetVui* pVui) {
    StdVideoH264SpsFlags spsFlags = {};
    spsFlags.direct_8x8_inference_flag = 1u;
    spsFlags.frame_mbs_only_flag = 1u;
    spsFlags.vui_parameters_present_flag = (pVui == NULL) ? 0u : 1u;

    const uint32_t mbAlignedWidth = AlignSize(width, H264MbSizeAlignment);
    const uint32_t mbAlignedHeight = AlignSize(height, H264MbSizeAlignment);

    StdVideoH264SequenceParameterSet sps = {};
    sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
    sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
    sps.seq_parameter_set_id = 0u;
    sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
    sps.bit_depth_luma_minus8 = 0u;
    sps.bit_depth_chroma_minus8 = 0u;
    sps.log2_max_frame_num_minus4 = 0u;
    sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
    sps.max_num_ref_frames = 1u;
    sps.pic_width_in_mbs_minus1 = mbAlignedWidth / H264MbSizeAlignment - 1;
    sps.pic_height_in_map_units_minus1 = mbAlignedHeight / H264MbSizeAlignment - 1;
    sps.flags = spsFlags;
    sps.pSequenceParameterSetVui = pVui;
    sps.frame_crop_right_offset = mbAlignedWidth - width;
    sps.frame_crop_bottom_offset = mbAlignedHeight - height;

    // This allows for picture order count values in the range [0, 255].
    sps.log2_max_pic_order_cnt_lsb_minus4 = 4u;

    if (sps.frame_crop_right_offset || sps.frame_crop_bottom_offset) {
        sps.flags.frame_cropping_flag = true;

        if (sps.chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_420) {
            sps.frame_crop_right_offset >>= 1;
            sps.frame_crop_bottom_offset >>= 1;
        }
    }

    return sps;
}

static StdVideoH264PictureParameterSet getStdVideoH264PictureParameterSet(void) {
    StdVideoH264PpsFlags ppsFlags = {};
    // ppsFlags.transform_8x8_mode_flag = 1u;
    ppsFlags.transform_8x8_mode_flag = 0u;
    ppsFlags.constrained_intra_pred_flag = 0u;
    ppsFlags.deblocking_filter_control_present_flag = 1u;
    ppsFlags.entropy_coding_mode_flag = 1u;

    StdVideoH264PictureParameterSet pps = {};
    pps.seq_parameter_set_id = 0u;
    pps.pic_parameter_set_id = 0u;
    pps.num_ref_idx_l0_default_active_minus1 = 0u;
    pps.flags = ppsFlags;

    return pps;
}

class FrameInfo {
   public:
    FrameInfo(uint32_t frameCount, uint32_t width, uint32_t height, StdVideoH264SequenceParameterSet sps,
              StdVideoH264PictureParameterSet pps, uint32_t gopFrameCount, bool useConstantQp) {
        bool isI = gopFrameCount == 0;
        const uint32_t MaxPicOrderCntLsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);

        m_sliceHeaderFlags.direct_spatial_mv_pred_flag = 1;
        m_sliceHeaderFlags.num_ref_idx_active_override_flag = 0;

        m_sliceHeader.flags = m_sliceHeaderFlags;
        m_sliceHeader.slice_type = isI ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
        m_sliceHeader.cabac_init_idc = (StdVideoH264CabacInitIdc)0;
        m_sliceHeader.disable_deblocking_filter_idc = (StdVideoH264DisableDeblockingFilterIdc)0;
        m_sliceHeader.slice_alpha_c0_offset_div2 = 0;
        m_sliceHeader.slice_beta_offset_div2 = 0;

        uint32_t picWidthInMbs = sps.pic_width_in_mbs_minus1 + 1;
        uint32_t picHeightInMbs = sps.pic_height_in_map_units_minus1 + 1;
        uint32_t iPicSizeInMbs = picWidthInMbs * picHeightInMbs;

        m_sliceInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
        m_sliceInfo.pNext = NULL;
        m_sliceInfo.pStdSliceHeader = &m_sliceHeader;
        m_sliceInfo.constantQp = useConstantQp ? pps.pic_init_qp_minus26 + 26 : 0;

        m_pictureInfoFlags.IdrPicFlag = isI ? 1 : 0;  // every I frame is an IDR frame
        m_pictureInfoFlags.is_reference = 1;
        m_pictureInfoFlags.adaptive_ref_pic_marking_mode_flag = 0;
        m_pictureInfoFlags.no_output_of_prior_pics_flag = isI ? 1 : 0;

        m_stdPictureInfo.flags = m_pictureInfoFlags;
        m_stdPictureInfo.seq_parameter_set_id = 0;
        m_stdPictureInfo.pic_parameter_set_id = pps.pic_parameter_set_id;
        m_stdPictureInfo.idr_pic_id = 0;
        m_stdPictureInfo.primary_pic_type = isI ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
        // m_stdPictureInfo.temporal_id = 1;

        // frame_num is incremented for each reference frame transmitted.
        // In our case, only the first frame (which is IDR) is a reference
        // frame with frame_num == 0, and all others have frame_num == 1.
        m_stdPictureInfo.frame_num = frameCount;

        // POC is incremented by 2 for each coded frame.
        m_stdPictureInfo.PicOrderCnt = (frameCount * 2) % MaxPicOrderCntLsb;
        m_referenceLists.num_ref_idx_l0_active_minus1 = 0;
        m_referenceLists.num_ref_idx_l1_active_minus1 = 0;
        std::fill_n(m_referenceLists.RefPicList0, STD_VIDEO_H264_MAX_NUM_LIST_REF, STD_VIDEO_H264_NO_REFERENCE_PICTURE);
        std::fill_n(m_referenceLists.RefPicList1, STD_VIDEO_H264_MAX_NUM_LIST_REF, STD_VIDEO_H264_NO_REFERENCE_PICTURE);
        if (!isI) {
            m_referenceLists.RefPicList0[0] = !(gopFrameCount & 1);
        }
        m_stdPictureInfo.pRefLists = &m_referenceLists;

        m_encodeH264FrameInfo.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
        m_encodeH264FrameInfo.pNext = NULL;
        m_encodeH264FrameInfo.naluSliceEntryCount = 1;
        m_encodeH264FrameInfo.pNaluSliceEntries = &m_sliceInfo;
        m_encodeH264FrameInfo.pStdPictureInfo = &m_stdPictureInfo;
    }

    inline VkVideoEncodeH264PictureInfoKHR* getEncodeH264FrameInfo() { return &m_encodeH264FrameInfo; };

   private:
    StdVideoEncodeH264SliceHeaderFlags m_sliceHeaderFlags = {};
    StdVideoEncodeH264SliceHeader m_sliceHeader = {};
    VkVideoEncodeH264NaluSliceInfoKHR m_sliceInfo = {};
    StdVideoEncodeH264PictureInfoFlags m_pictureInfoFlags = {};
    StdVideoEncodeH264PictureInfo m_stdPictureInfo = {};
    VkVideoEncodeH264PictureInfoKHR m_encodeH264FrameInfo = {};
    StdVideoEncodeH264ReferenceListsInfo m_referenceLists = {};
};
};  // namespace h264
