/*
 * Copyright 2017 Intel Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to the
 * source code ("Material") are owned by Intel Corporation or its suppliers or
 * licensors. Title to the Material remains with Intel Corporation or its suppliers
 * and licensors. The Material contains trade secrets and proprietary and
 * confidential information of Intel or its suppliers and licensors. The Material
 * is protected by worldwide copyright and trade secret laws and treaty provisions.
 * No part of the Material may be used, copied, reproduced, modified, published,
 * uploaded, posted, transmitted, distributed, or disclosed in any way without
 * Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery of
 * the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be express
 * and approved by Intel in writing.
 */

#include "VideoFramePacketizer.h"
#include "MediaUtilities.h"

using namespace webrtc;
using namespace erizo;

namespace woogeen_base {

// To make it consistent with the webrtc library, we allow packets to be transmitted
// in up to 2 times max video bitrate if the bandwidth estimate allows it.
static const int TRANSMISSION_MAXBITRATE_MULTIPLIER = 2;

DEFINE_LOGGER(VideoFramePacketizer, "woogeen.VideoFramePacketizer");

VideoFramePacketizer::VideoFramePacketizer(bool enableRed, bool enableUlpfec)
    : m_enabled(true)
    , m_keyFrameArrived(false)
    , m_frameFormat(FRAME_FORMAT_UNKNOWN)
    , m_frameWidth(0)
    , m_frameHeight(0)
{
    videoSink_ = nullptr;
    m_videoTransport.reset(new WebRTCTransport<erizo::VIDEO>(this, nullptr));
    m_taskRunner.reset(new woogeen_base::WebRTCTaskRunner());
    m_taskRunner->Start();
    init(enableRed, enableUlpfec);
}

VideoFramePacketizer::~VideoFramePacketizer()
{
    close();
    m_taskRunner->Stop();
    boost::unique_lock<boost::shared_mutex> lock(m_rtpRtcpMutex);
}

void VideoFramePacketizer::bindTransport(erizo::MediaSink* sink)
{
    boost::unique_lock<boost::shared_mutex> lock(m_transport_mutex);
    videoSink_ = sink;
    videoSink_->setVideoSinkSSRC(m_rtpRtcp->SSRC());
    erizo::FeedbackSource* fbSource = videoSink_->getFeedbackSource();
    if (fbSource)
        fbSource->setFeedbackSink(this);
}

void VideoFramePacketizer::unbindTransport()
{
    boost::unique_lock<boost::shared_mutex> lock(m_transport_mutex);
    if (videoSink_) {
        erizo::FeedbackSource* fbSource = videoSink_->getFeedbackSource();
        if (fbSource)
            fbSource->setFeedbackSink(nullptr);
        videoSink_ = nullptr;
    }
}

void VideoFramePacketizer::enable(bool enabled)
{
    m_enabled = enabled;
    if (m_enabled) {
        FeedbackMsg feedback = {.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME};
        deliverFeedbackMsg(feedback);
    }
}

bool VideoFramePacketizer::setSendCodec(FrameFormat frameFormat, unsigned int width, unsigned int height)
{
    // The send video format should be identical to the input video format,
    // because we (VideoFramePacketizer) don't have the transcoding capability.
    assert(frameFormat == m_frameFormat);

    VideoCodec codec;
    memset(&codec, 0, sizeof(codec));

    switch (frameFormat) {
    case FRAME_FORMAT_VP8:
        codec.codecType = webrtc::kVideoCodecVP8;
        strcpy(codec.plName, "VP8");
        codec.plType = VP8_90000_PT;
        break;
    case FRAME_FORMAT_VP9:
        codec.codecType = webrtc::kVideoCodecVP9;
        strcpy(codec.plName, "VP9");
        codec.plType = VP9_90000_PT;
        break;
    case FRAME_FORMAT_H264:
        codec.codecType = webrtc::kVideoCodecH264;
        strcpy(codec.plName, "H264");
        codec.plType = H264_90000_PT;
        break;
    case FRAME_FORMAT_H265:
        codec.codecType = webrtc::kVideoCodecH265;
        strcpy(codec.plName, "H265");
        codec.plType = H265_90000_PT;
        break;
    case FRAME_FORMAT_I420:
    default:
        return false;
    }

    uint32_t targetKbps = 0;
    targetKbps = calcBitrate(width, height) * (frameFormat == FRAME_FORMAT_VP8 ? 0.9 : 1);

    uint32_t minKbps = targetKbps / 4;
    // The bitrate controller is accepting "bps".
    //m_bitrateController->SetBitrateObserver(this, targetKbps * 1000, minKbps * 1000, TRANSMISSION_MAXBITRATE_MULTIPLIER * targetKbps * 1000);
    //Update with M53
    m_bitrateController->SetStartBitrate(targetKbps * 1000);
    m_bitrateController->SetMinMaxBitrate(minKbps * 1000, TRANSMISSION_MAXBITRATE_MULTIPLIER * targetKbps * 1000);

    boost::shared_lock<boost::shared_mutex> lock(m_rtpRtcpMutex);
    return m_rtpRtcp && m_rtpRtcp->RegisterSendPayload(codec) != -1;
}

void VideoFramePacketizer::OnReceivedIntraFrameRequest(uint32_t ssrc)
{
    ELOG_DEBUG("onReceivedIntraFrameRequest.");
    FeedbackMsg feedback = {.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME};
    deliverFeedbackMsg(feedback);
}

int VideoFramePacketizer::deliverFeedback(char* buf, int len)
{
    boost::shared_lock<boost::shared_mutex> lock(m_rtpRtcpMutex);
    return m_rtpRtcp->IncomingRtcpPacket(reinterpret_cast<uint8_t*>(buf), len) == -1 ? 0 : len;
}

void VideoFramePacketizer::receiveRtpData(char* buf, int len, erizo::DataType type, uint32_t channelId)
{
    boost::shared_lock<boost::shared_mutex> lock(m_transport_mutex);
    if (!videoSink_) {
        return;
    }

    assert(type == erizo::VIDEO);
    videoSink_->deliverVideoData(buf, len);
}

void VideoFramePacketizer::OnNetworkChanged(const uint32_t target_bitrate, const uint8_t fraction_loss, const int64_t rtt)
{
    // Receiver's network change detected. But we do not deliver feedback to
    // sender because sender may adjust sending bitrate for a specific receiver.
}

void VideoFramePacketizer::onFrame(const Frame& frame)
{
    if (!m_enabled) {
        return;
    }

    if (!m_keyFrameArrived) {
        if (!frame.additionalInfo.video.isKeyFrame) {
            ELOG_DEBUG("Key frame has not arrived, send key-frame-request.");
            FeedbackMsg feedback = {.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME};
            deliverFeedbackMsg(feedback);
            return;
        } else {
            m_keyFrameArrived = true;
        }
    }

    webrtc::RTPVideoHeader h;

    if (frame.format != m_frameFormat
        || frame.additionalInfo.video.width != m_frameWidth
        || frame.additionalInfo.video.height != m_frameHeight) {
        m_frameFormat = frame.format;
        m_frameWidth = frame.additionalInfo.video.width;
        m_frameHeight = frame.additionalInfo.video.height;
        setSendCodec(m_frameFormat, m_frameWidth, m_frameHeight);
    }

    if (frame.format == FRAME_FORMAT_VP8) {
        h.codec = webrtc::kRtpVideoVp8;
        h.codecHeader.VP8.InitRTPVideoHeaderVP8();
        boost::shared_lock<boost::shared_mutex> lock(m_rtpRtcpMutex);
        m_rtpRtcp->SendOutgoingData(webrtc::kVideoFrameKey, VP8_90000_PT, frame.timeStamp, frame.timeStamp / 90, frame.payload, frame.length, nullptr, &h);
    } else if (frame.format == FRAME_FORMAT_VP9) {
        h.codec = webrtc::kRtpVideoVp9;
        h.codecHeader.VP9.InitRTPVideoHeaderVP9();
        boost::shared_lock<boost::shared_mutex> lock(m_rtpRtcpMutex);
        m_rtpRtcp->SendOutgoingData(webrtc::kVideoFrameKey, VP9_90000_PT, frame.timeStamp, frame.timeStamp / 90, frame.payload, frame.length, nullptr, &h);
    } else if (frame.format == FRAME_FORMAT_H264 || frame.format == FRAME_FORMAT_H265) {
        int nalu_found_length = 0;
        uint8_t* buffer_start = frame.payload;
        int buffer_length = frame.length;
        int nalu_start_offset = 0;
        int nalu_end_offset = 0;
        RTPFragmentationHeader frag_info;

        h.codec = (frame.format == FRAME_FORMAT_H264)?(webrtc::kRtpVideoH264):(webrtc::kRtpVideoH265);
        while (buffer_length > 0) {
            nalu_found_length = findNALU(buffer_start, buffer_length, &nalu_start_offset, &nalu_end_offset);
            if (nalu_found_length < 0) {
                /* Error, should never happen */
                break;
            } else {
                /* SPS, PPS, I, P*/
                uint16_t last = frag_info.fragmentationVectorSize;
                frag_info.VerifyAndAllocateFragmentationHeader(last + 1);
                frag_info.fragmentationOffset[last] = nalu_start_offset + (buffer_start - frame.payload);
                frag_info.fragmentationLength[last] = nalu_found_length;
                buffer_start += (nalu_start_offset + nalu_found_length);
                buffer_length -= (nalu_start_offset + nalu_found_length);
            }
        }

        boost::shared_lock<boost::shared_mutex> lock(m_rtpRtcpMutex);
        if (frame.format == FRAME_FORMAT_H264) {
          m_rtpRtcp->SendOutgoingData(webrtc::kVideoFrameKey, H264_90000_PT, frame.timeStamp, frame.timeStamp / 90, frame.payload, frame.length, &frag_info, &h);
        } else {
          m_rtpRtcp->SendOutgoingData(webrtc::kVideoFrameKey, H265_90000_PT, frame.timeStamp, frame.timeStamp / 90, frame.payload, frame.length, &frag_info, &h);
        }
    }
}

int VideoFramePacketizer::sendFirPacket()
{
    FeedbackMsg feedback = {.type = VIDEO_FEEDBACK, .cmd = REQUEST_KEY_FRAME};
    deliverFeedbackMsg(feedback);
    return 0;
}

bool VideoFramePacketizer::init(bool enableRed, bool enableUlpfec)
{
    m_bitrateController.reset(webrtc::BitrateController::CreateBitrateController(Clock::GetRealTimeClock(), this));
    m_bandwidthObserver.reset(m_bitrateController->CreateRtcpBandwidthObserver());
    // FIXME: Provide the correct bitrate settings (start, min and max bitrates).
    //m_bitrateController->SetBitrateObserver(this, 300 * 1000, 0, 0);
    m_bitrateController->SetStartBitrate(300*1000);
    m_bitrateController->SetMinMaxBitrate(0, 0);

    RtpRtcp::Configuration configuration;
    // configuration.id = m_id;
    configuration.outgoing_transport = m_videoTransport.get();
    configuration.audio = false;  // Video.
    configuration.intra_frame_callback = this;
    configuration.bandwidth_callback = m_bandwidthObserver.get();
    m_rtpRtcp.reset(RtpRtcp::CreateRtpRtcp(configuration));

    // TODO: the parameters should be dynamically adjustable.
    uint8_t red_pl_type = enableRed? RED_90000_PT : 0;
    uint8_t ulpfec_pl_type = enableUlpfec? ULP_90000_PT : 0;
    m_rtpRtcp->SetGenericFECStatus(false, red_pl_type, ulpfec_pl_type);
    m_rtpRtcp->SetREMBStatus(true);

    // Enable NACK.
    // TODO: the parameters should be dynamically adjustable.
    m_rtpRtcp->SetStorePacketsStatus(true, 600);

    m_taskRunner->RegisterModule(m_rtpRtcp.get());

    return true;
}

void VideoFramePacketizer::close()
{
    boost::unique_lock<boost::shared_mutex> lock(m_transport_mutex);
    if (videoSink_) {
        erizo::FeedbackSource* fbSource = videoSink_->getFeedbackSource();
        if (fbSource)
            fbSource->setFeedbackSink(nullptr);
        videoSink_ = nullptr;
    }
    lock.unlock();
    // M53 does not support removing observer
    //if (m_bitrateController)
    //   m_bitrateController->RemoveBitrateObserver(this);
    m_taskRunner->DeRegisterModule(m_rtpRtcp.get());
}

}
