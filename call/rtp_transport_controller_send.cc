/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include <utility>

#include "call/rtp_transport_controller_send.h"
#include "modules/congestion_controller/include/send_side_congestion_controller.h"
#include "rtc_base/location.h"
#include "rtc_base/logging.h"
#include "rtc_base/ptr_util.h"

namespace webrtc {

RtpTransportControllerSend::RtpTransportControllerSend(
    Clock* clock,
    webrtc::RtcEventLog* event_log,
    const BitrateConstraints& bitrate_config)
    : pacer_(clock, &packet_router_, event_log),
      send_side_cc_(
          rtc::MakeUnique<SendSideCongestionController>(clock,
                                                        nullptr /* observer */,
                                                        event_log,
                                                        &pacer_)),
      bitrate_configurator_(bitrate_config),
      process_thread_(ProcessThread::Create("SendControllerThread")) {
  send_side_cc_->SignalNetworkState(kNetworkDown);
  send_side_cc_->SetBweBitrates(bitrate_config.min_bitrate_bps,
                                bitrate_config.start_bitrate_bps,
                                bitrate_config.max_bitrate_bps);

  process_thread_->RegisterModule(&pacer_, RTC_FROM_HERE);
  process_thread_->RegisterModule(send_side_cc_.get(), RTC_FROM_HERE);
  process_thread_->Start();
}

RtpTransportControllerSend::~RtpTransportControllerSend() {
  process_thread_->Stop();
  process_thread_->DeRegisterModule(send_side_cc_.get());
  process_thread_->DeRegisterModule(&pacer_);
}

PacketRouter* RtpTransportControllerSend::packet_router() {
  return &packet_router_;
}

TransportFeedbackObserver*
RtpTransportControllerSend::transport_feedback_observer() {
  return send_side_cc_.get();
}

RtpPacketSender* RtpTransportControllerSend::packet_sender() {
  return &pacer_;
}

const RtpKeepAliveConfig& RtpTransportControllerSend::keepalive_config() const {
  return keepalive_;
}

void RtpTransportControllerSend::SetAllocatedSendBitrateLimits(
    int min_send_bitrate_bps,
    int max_padding_bitrate_bps,
    int max_total_bitrate_bps) {
  pacer_.SetSendBitrateLimits(min_send_bitrate_bps, max_padding_bitrate_bps);
  send_side_cc_->SetMaxTotalAllocatedBitrate(max_total_bitrate_bps);
}

void RtpTransportControllerSend::SetKeepAliveConfig(
    const RtpKeepAliveConfig& config) {
  keepalive_ = config;
}
void RtpTransportControllerSend::SetPacingFactor(float pacing_factor) {
  pacer_.SetPacingFactor(pacing_factor);
}
void RtpTransportControllerSend::SetQueueTimeLimit(int limit_ms) {
  pacer_.SetQueueTimeLimit(limit_ms);
}
CallStatsObserver* RtpTransportControllerSend::GetCallStatsObserver() {
  return send_side_cc_.get();
}
void RtpTransportControllerSend::RegisterPacketFeedbackObserver(
    PacketFeedbackObserver* observer) {
  send_side_cc_->RegisterPacketFeedbackObserver(observer);
}
void RtpTransportControllerSend::DeRegisterPacketFeedbackObserver(
    PacketFeedbackObserver* observer) {
  send_side_cc_->DeRegisterPacketFeedbackObserver(observer);
}
void RtpTransportControllerSend::RegisterNetworkObserver(
    NetworkChangedObserver* observer) {
  send_side_cc_->RegisterNetworkObserver(observer);
}
void RtpTransportControllerSend::OnNetworkRouteChanged(
    const std::string& transport_name,
    const rtc::NetworkRoute& network_route) {
  // Check if the network route is connected.
  if (!network_route.connected) {
    RTC_LOG(LS_INFO) << "Transport " << transport_name << " is disconnected";
    // TODO(honghaiz): Perhaps handle this in SignalChannelNetworkState and
    // consider merging these two methods.
    return;
  }

  // Check whether the network route has changed on each transport.
  auto result =
      network_routes_.insert(std::make_pair(transport_name, network_route));
  auto kv = result.first;
  bool inserted = result.second;
  if (inserted) {
    // No need to reset BWE if this is the first time the network connects.
    return;
  }
  if (kv->second != network_route) {
    kv->second = network_route;
    BitrateConstraints bitrate_config = bitrate_configurator_.GetConfig();
    RTC_LOG(LS_INFO) << "Network route changed on transport " << transport_name
                     << ": new local network id "
                     << network_route.local_network_id
                     << " new remote network id "
                     << network_route.remote_network_id
                     << " Reset bitrates to min: "
                     << bitrate_config.min_bitrate_bps
                     << " bps, start: " << bitrate_config.start_bitrate_bps
                     << " bps,  max: " << bitrate_config.max_bitrate_bps
                     << " bps.";
    RTC_DCHECK_GT(bitrate_config.start_bitrate_bps, 0);
    send_side_cc_->OnNetworkRouteChanged(
        network_route, bitrate_config.start_bitrate_bps,
        bitrate_config.min_bitrate_bps, bitrate_config.max_bitrate_bps);
  }
}
void RtpTransportControllerSend::OnNetworkAvailability(bool network_available) {
  send_side_cc_->SignalNetworkState(network_available ? kNetworkUp
                                                      : kNetworkDown);
}
RtcpBandwidthObserver* RtpTransportControllerSend::GetBandwidthObserver() {
  return send_side_cc_->GetBandwidthObserver();
}
bool RtpTransportControllerSend::AvailableBandwidth(uint32_t* bandwidth) const {
  return send_side_cc_->AvailableBandwidth(bandwidth);
}
int64_t RtpTransportControllerSend::GetPacerQueuingDelayMs() const {
  return pacer_.QueueInMs();
}
int64_t RtpTransportControllerSend::GetFirstPacketTimeMs() const {
  return pacer_.FirstSentPacketTimeMs();
}
void RtpTransportControllerSend::EnablePeriodicAlrProbing(bool enable) {
  send_side_cc_->EnablePeriodicAlrProbing(enable);
}
void RtpTransportControllerSend::OnSentPacket(
    const rtc::SentPacket& sent_packet) {
  send_side_cc_->OnSentPacket(sent_packet);
}

void RtpTransportControllerSend::SetSdpBitrateParameters(
    const BitrateConstraints& constraints) {
  rtc::Optional<BitrateConstraints> updated =
      bitrate_configurator_.UpdateWithSdpParameters(constraints);
  if (updated.has_value()) {
    send_side_cc_->SetBweBitrates(updated->min_bitrate_bps,
                                  updated->start_bitrate_bps,
                                  updated->max_bitrate_bps);
  } else {
    RTC_LOG(LS_VERBOSE)
        << "WebRTC.RtpTransportControllerSend.SetSdpBitrateParameters: "
        << "nothing to update";
  }
}

void RtpTransportControllerSend::SetClientBitratePreferences(
    const BitrateConstraintsMask& preferences) {
  rtc::Optional<BitrateConstraints> updated =
      bitrate_configurator_.UpdateWithClientPreferences(preferences);
  if (updated.has_value()) {
    send_side_cc_->SetBweBitrates(updated->min_bitrate_bps,
                                  updated->start_bitrate_bps,
                                  updated->max_bitrate_bps);
  } else {
    RTC_LOG(LS_VERBOSE)
        << "WebRTC.RtpTransportControllerSend.SetClientBitratePreferences: "
        << "nothing to update";
  }
}
}  // namespace webrtc
