/*
 *  Copyright 2017 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_JSEP_TRANSPORT_CONTROLLER_H_
#define PC_JSEP_TRANSPORT_CONTROLLER_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/candidate.h"
#include "api/crypto/crypto_options.h"
#include "api/ice_transport_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/transport/media/media_transport_config.h"
#include "media/sctp/sctp_transport_internal.h"
#include "p2p/base/dtls_transport.h"
#include "p2p/base/dtls_transport_factory.h"
#include "p2p/base/p2p_transport_channel.h"
#include "pc/channel.h"
#include "pc/dtls_srtp_transport.h"
#include "pc/dtls_transport.h"
#include "pc/jsep_transport.h"
#include "pc/rtp_transport.h"
#include "pc/srtp_transport.h"
#include "rtc_base/async_invoker.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/third_party/sigslot/sigslot.h"

namespace rtc {
class Thread;
class PacketTransportInternal;
}  // namespace rtc

namespace webrtc {

class JsepTransportController : public sigslot::has_slots<> {
 public:
  // Used when the RtpTransport/DtlsTransport of the m= section is changed
  // because the section is rejected or BUNDLE is enabled.
  class Observer {
   public:
    virtual ~Observer() {}

    // Returns true if media associated with |mid| was successfully set up to be
    // demultiplexed on |rtp_transport|. Could return false if two bundled m=
    // sections use the same SSRC, for example.
    //
    // If a data channel transport must be negotiated, |data_channel_transport|
    // and |negotiation_state| indicate negotiation status.  If
    // |data_channel_transport| is null, the data channel transport should not
    // be used.  Otherwise, the value is a pointer to the transport to be used
    // for data channels on |mid|, if any.
    //
    // The observer should not send data on |data_channel_transport| until
    // |negotiation_state| is provisional or final.  It should not delete
    // |data_channel_transport| or any fallback transport until
    // |negotiation_state| is final.
    virtual bool OnTransportChanged(
        const std::string& mid,
        RtpTransportInternal* rtp_transport,
        rtc::scoped_refptr<DtlsTransport> dtls_transport,
        DataChannelTransportInterface* data_channel_transport) = 0;
  };

  struct Config {
    // If |redetermine_role_on_ice_restart| is true, ICE role is redetermined
    // upon setting a local transport description that indicates an ICE
    // restart.
    bool redetermine_role_on_ice_restart = true;
    rtc::SSLProtocolVersion ssl_max_version = rtc::SSL_PROTOCOL_DTLS_12;
    // |crypto_options| is used to determine if created DTLS transports
    // negotiate GCM crypto suites or not.
    webrtc::CryptoOptions crypto_options;
    PeerConnectionInterface::BundlePolicy bundle_policy =
        PeerConnectionInterface::kBundlePolicyBalanced;
    PeerConnectionInterface::RtcpMuxPolicy rtcp_mux_policy =
        PeerConnectionInterface::kRtcpMuxPolicyRequire;
    bool disable_encryption = false;
    bool enable_external_auth = false;
    // Used to inject the ICE/DTLS transports created externally.
    webrtc::IceTransportFactory* ice_transport_factory = nullptr;
    cricket::DtlsTransportFactory* dtls_transport_factory = nullptr;
    Observer* transport_observer = nullptr;
    // Must be provided and valid for the lifetime of the
    // JsepTransportController instance.
    std::function<void(const rtc::CopyOnWriteBuffer& packet,
                       int64_t packet_time_us)>
        rtcp_handler;
    bool active_reset_srtp_params = false;
    RtcEventLog* event_log = nullptr;

    // Factory for SCTP transports.
    cricket::SctpTransportInternalFactory* sctp_factory = nullptr;

    // Whether an RtpMediaTransport should be created as default, when no
    // MediaTransportFactory is provided.
    bool use_rtp_media_transport = false;

    // Use encrypted datagram transport to send packets.
    bool use_datagram_transport = false;

    // Use datagram transport's implementation of data channels instead of SCTP.
    bool use_datagram_transport_for_data_channels = false;

    // Whether |use_datagram_transport_for_data_channels| applies to outgoing
    // calls.  If true, |use_datagram_transport_for_data_channels| applies only
    // to incoming calls.
    bool use_datagram_transport_for_data_channels_receive_only = false;

    // Optional media transport factory (experimental). If provided it will be
    // used to create datagram_transport (as long as either
    // |use_datagram_transport| or
    // |use_datagram_transport_for_data_channels| is set to true). However,
    // whether it will be used to send / receive audio and video frames instead
    // of RTP is determined by |use_datagram_transport|. Note that currently
    // datagram_transport co-exists with RTP / RTCP transports and may use the
    // same underlying ICE transport.
    MediaTransportFactory* media_transport_factory = nullptr;
  };

  // The ICE related events are signaled on the |signaling_thread|.
  // All the transport related methods are called on the |network_thread|.
  JsepTransportController(rtc::Thread* signaling_thread,
                          rtc::Thread* network_thread,
                          cricket::PortAllocator* port_allocator,
                          AsyncResolverFactory* async_resolver_factory,
                          Config config);
  virtual ~JsepTransportController();

  // The main method to be called; applies a description at the transport
  // level, creating/destroying transport objects as needed and updating their
  // properties. This includes RTP, DTLS, and ICE (but not SCTP). At least not
  // yet? May make sense to in the future.
  RTCError SetLocalDescription(SdpType type,
                               const cricket::SessionDescription* description);

  RTCError SetRemoteDescription(SdpType type,
                                const cricket::SessionDescription* description);

  // Get transports to be used for the provided |mid|. If bundling is enabled,
  // calling GetRtpTransport for multiple MIDs may yield the same object.
  RtpTransportInternal* GetRtpTransport(const std::string& mid) const;
  cricket::DtlsTransportInternal* GetDtlsTransport(const std::string& mid);
  const cricket::DtlsTransportInternal* GetRtcpDtlsTransport(
      const std::string& mid) const;
  // Gets the externally sharable version of the DtlsTransport.
  rtc::scoped_refptr<webrtc::DtlsTransport> LookupDtlsTransportByMid(
      const std::string& mid);
  rtc::scoped_refptr<SctpTransport> GetSctpTransport(
      const std::string& mid) const;

  MediaTransportConfig GetMediaTransportConfig(const std::string& mid) const;

  DataChannelTransportInterface* GetDataChannelTransport(
      const std::string& mid) const;

  /*********************
   * ICE-related methods
   ********************/
  // This method is public to allow PeerConnection to update it from
  // SetConfiguration.
  void SetIceConfig(const cricket::IceConfig& config);
  // Set the "needs-ice-restart" flag as described in JSEP. After the flag is
  // set, offers should generate new ufrags/passwords until an ICE restart
  // occurs.
  void SetNeedsIceRestartFlag();
  // Returns true if the ICE restart flag above was set, and no ICE restart has
  // occurred yet for this transport (by applying a local description with
  // changed ufrag/password). If the transport has been deleted as a result of
  // bundling, returns false.
  bool NeedsIceRestart(const std::string& mid) const;
  // Start gathering candidates for any new transports, or transports doing an
  // ICE restart.
  void MaybeStartGathering();
  void StartGatheringWithSharedIceGatherer(
      rtc::scoped_refptr<webrtc::IceGathererInterface> shared_ice_gatherer);
  RTCError AddRemoteCandidates(
      const std::string& mid,
      const std::vector<cricket::Candidate>& candidates);
  RTCError RemoveRemoteCandidates(
      const std::vector<cricket::Candidate>& candidates);

  /**********************
   * DTLS-related methods
   *********************/
  // Specifies the identity to use in this session.
  // Can only be called once.
  bool SetLocalCertificate(
      const rtc::scoped_refptr<rtc::RTCCertificate>& certificate);
  rtc::scoped_refptr<rtc::RTCCertificate> GetLocalCertificate(
      const std::string& mid) const;
  // Caller owns returned certificate chain. This method mainly exists for
  // stats reporting.
  std::unique_ptr<rtc::SSLCertChain> GetRemoteSSLCertChain(
      const std::string& mid) const;
  // Get negotiated role, if one has been negotiated.
  absl::optional<rtc::SSLRole> GetDtlsRole(const std::string& mid) const;

  // TODO(deadbeef): GetStats isn't const because all the way down to
  // OpenSSLStreamAdapter, GetSslCipherSuite and GetDtlsSrtpCryptoSuite are not
  // const. Fix this.
  bool GetStats(const std::string& mid, cricket::TransportStats* stats);

  bool initial_offerer() const { return initial_offerer_ && *initial_offerer_; }

  void SetActiveResetSrtpParams(bool active_reset_srtp_params);

  // Allows to overwrite the settings from config. You may set or reset the
  // media transport configuration on the jsep transport controller, as long as
  // you did not call 'GetMediaTransport' or 'MaybeCreateJsepTransport'. Once
  // Jsep transport is created, you can't change this setting.
  void SetMediaTransportSettings(
      bool use_datagram_transport,
      bool use_datagram_transport_for_data_channels,
      bool use_datagram_transport_for_data_channels_receive_only);

  // TODO(elrello): For now the rollback only removes mid to transport mappings
  // and deletes unused transports, but doesn't consider anything more complex.
  void RollbackTransportForMids(const std::vector<std::string>& mids);

  // Gets the transport parameters for the transport identified by |mid|.
  // If |mid| is bundled, returns the parameters for the bundled transport.
  // If the transport for |mid| has not been created yet, it may be allocated in
  // order to generate transport parameters.
  absl::optional<cricket::OpaqueTransportParameters> GetTransportParameters(
      const std::string& mid);

  // All of these signals are fired on the signaling thread.

  // If any transport failed => failed,
  // Else if all completed => completed,
  // Else if all connected => connected,
  // Else => connecting
  sigslot::signal1<cricket::IceConnectionState> SignalIceConnectionState;

  sigslot::signal1<PeerConnectionInterface::PeerConnectionState>
      SignalConnectionState;
  sigslot::signal1<PeerConnectionInterface::IceConnectionState>
      SignalStandardizedIceConnectionState;

  // If all transports done gathering => complete,
  // Else if any are gathering => gathering,
  // Else => new
  sigslot::signal1<cricket::IceGatheringState> SignalIceGatheringState;

  // (mid, candidates)
  sigslot::signal2<const std::string&, const std::vector<cricket::Candidate>&>
      SignalIceCandidatesGathered;

  sigslot::signal1<const cricket::IceCandidateErrorEvent&>
      SignalIceCandidateError;

  sigslot::signal1<const std::vector<cricket::Candidate>&>
      SignalIceCandidatesRemoved;

  sigslot::signal1<const cricket::CandidatePairChangeEvent&>
      SignalIceCandidatePairChanged;

  sigslot::signal1<rtc::SSLHandshakeError> SignalDtlsHandshakeError;

 private:
  RTCError ApplyDescription_n(bool local,
                              SdpType type,
                              const cricket::SessionDescription* description);
  RTCError ValidateAndMaybeUpdateBundleGroup(
      bool local,
      SdpType type,
      const cricket::SessionDescription* description);
  RTCError ValidateContent(const cricket::ContentInfo& content_info);

  void HandleRejectedContent(const cricket::ContentInfo& content_info,
                             const cricket::SessionDescription* description);
  bool HandleBundledContent(const cricket::ContentInfo& content_info);

  bool SetTransportForMid(const std::string& mid,
                          cricket::JsepTransport* jsep_transport);
  void RemoveTransportForMid(const std::string& mid);

  cricket::JsepTransportDescription CreateJsepTransportDescription(
      const cricket::ContentInfo& content_info,
      const cricket::TransportInfo& transport_info,
      const std::vector<int>& encrypted_extension_ids,
      int rtp_abs_sendtime_extn_id,
      absl::optional<std::string> media_alt_protocol,
      absl::optional<std::string> data_alt_protocol);

  absl::optional<std::string> bundled_mid() const {
    absl::optional<std::string> bundled_mid;
    if (bundle_group_ && bundle_group_->FirstContentName()) {
      bundled_mid = *(bundle_group_->FirstContentName());
    }
    return bundled_mid;
  }

  bool IsBundled(const std::string& mid) const {
    return bundle_group_ && bundle_group_->HasContentName(mid);
  }

  bool ShouldUpdateBundleGroup(SdpType type,
                               const cricket::SessionDescription* description);

  std::vector<int> MergeEncryptedHeaderExtensionIdsForBundle(
      const cricket::SessionDescription* description);
  std::vector<int> GetEncryptedHeaderExtensionIds(
      const cricket::ContentInfo& content_info);

  // Extracts the alt-protocol settings that apply to the bundle group.
  RTCError GetAltProtocolsForBundle(
      const cricket::SessionDescription* description,
      absl::optional<std::string>* media_alt_protocol,
      absl::optional<std::string>* data_alt_protocol);

  int GetRtpAbsSendTimeHeaderExtensionId(
      const cricket::ContentInfo& content_info);

  // This method takes the BUNDLE group into account. If the JsepTransport is
  // destroyed because of BUNDLE, it would return the transport which other
  // transports are bundled on (In current implementation, it is the first
  // content in the BUNDLE group).
  const cricket::JsepTransport* GetJsepTransportForMid(
      const std::string& mid) const;
  cricket::JsepTransport* GetJsepTransportForMid(const std::string& mid);

  // Get the JsepTransport without considering the BUNDLE group. Return nullptr
  // if the JsepTransport is destroyed.
  const cricket::JsepTransport* GetJsepTransportByName(
      const std::string& transport_name) const;
  cricket::JsepTransport* GetJsepTransportByName(
      const std::string& transport_name);

  // Creates jsep transport. Noop if transport is already created.
  // Transport is created either during SetLocalDescription (|local| == true) or
  // during SetRemoteDescription (|local| == false). Passing |local| helps to
  // differentiate initiator (caller) from answerer (callee).
  RTCError MaybeCreateJsepTransport(
      bool local,
      const cricket::ContentInfo& content_info,
      const cricket::SessionDescription& description);

  // Creates datagram transport if config wants to use it, and a=x-mt line is
  // present for the current media transport. Returned
  // DatagramTransportInterface is not connected, and must be connected to ICE.
  // You must call |GenerateOrGetLastMediaTransportOffer| on the caller before
  // calling MaybeCreateDatagramTransport.
  std::unique_ptr<webrtc::DatagramTransportInterface>
  MaybeCreateDatagramTransport(const cricket::ContentInfo& content_info,
                               const cricket::SessionDescription& description,
                               bool local);

  void MaybeDestroyJsepTransport(const std::string& mid);
  void DestroyAllJsepTransports_n();

  void SetIceRole_n(cricket::IceRole ice_role);

  cricket::IceRole DetermineIceRole(
      cricket::JsepTransport* jsep_transport,
      const cricket::TransportInfo& transport_info,
      SdpType type,
      bool local);

  std::unique_ptr<cricket::DtlsTransportInternal> CreateDtlsTransport(
      const cricket::ContentInfo& content_info,
      cricket::IceTransportInternal* ice,
      DatagramTransportInterface* datagram_transport);
  rtc::scoped_refptr<webrtc::IceTransportInterface> CreateIceTransport(
      const std::string& transport_name,
      bool rtcp);

  std::unique_ptr<webrtc::RtpTransport> CreateUnencryptedRtpTransport(
      const std::string& transport_name,
      rtc::PacketTransportInternal* rtp_packet_transport,
      rtc::PacketTransportInternal* rtcp_packet_transport);
  std::unique_ptr<webrtc::SrtpTransport> CreateSdesTransport(
      const std::string& transport_name,
      cricket::DtlsTransportInternal* rtp_dtls_transport,
      cricket::DtlsTransportInternal* rtcp_dtls_transport);
  std::unique_ptr<webrtc::DtlsSrtpTransport> CreateDtlsSrtpTransport(
      const std::string& transport_name,
      cricket::DtlsTransportInternal* rtp_dtls_transport,
      cricket::DtlsTransportInternal* rtcp_dtls_transport);

  // Collect all the DtlsTransports, including RTP and RTCP, from the
  // JsepTransports. JsepTransportController can iterate all the DtlsTransports
  // and update the aggregate states.
  std::vector<cricket::DtlsTransportInternal*> GetDtlsTransports();

  // Handlers for signals from Transport.
  void OnTransportWritableState_n(rtc::PacketTransportInternal* transport);
  void OnTransportReceivingState_n(rtc::PacketTransportInternal* transport);
  void OnTransportGatheringState_n(cricket::IceTransportInternal* transport);
  void OnTransportCandidateGathered_n(cricket::IceTransportInternal* transport,
                                      const cricket::Candidate& candidate);
  void OnTransportCandidateError_n(
      cricket::IceTransportInternal* transport,
      const cricket::IceCandidateErrorEvent& event);
  void OnTransportCandidatesRemoved_n(cricket::IceTransportInternal* transport,
                                      const cricket::Candidates& candidates);
  void OnTransportRoleConflict_n(cricket::IceTransportInternal* transport);
  void OnTransportStateChanged_n(cricket::IceTransportInternal* transport);
  void OnTransportCandidatePairChanged_n(
      const cricket::CandidatePairChangeEvent& event);
  void OnDataChannelTransportNegotiated_n(
      cricket::JsepTransport* transport,
      DataChannelTransportInterface* data_channel_transport);

  void UpdateAggregateStates_n();

  void OnRtcpPacketReceived_n(rtc::CopyOnWriteBuffer* packet,
                              int64_t packet_time_us);

  void OnDtlsHandshakeError(rtc::SSLHandshakeError error);

  rtc::Thread* const signaling_thread_ = nullptr;
  rtc::Thread* const network_thread_ = nullptr;
  cricket::PortAllocator* const port_allocator_ = nullptr;
  AsyncResolverFactory* const async_resolver_factory_ = nullptr;

  std::map<std::string, std::unique_ptr<cricket::JsepTransport>>
      jsep_transports_by_name_;
  // This keeps track of the mapping between media section
  // (BaseChannel/SctpTransport) and the JsepTransport underneath.
  std::map<std::string, cricket::JsepTransport*> mid_to_transport_;

  // Aggregate states for Transports.
  // standardized_ice_connection_state_ is intended to replace
  // ice_connection_state, see bugs.webrtc.org/9308
  cricket::IceConnectionState ice_connection_state_ =
      cricket::kIceConnectionConnecting;
  PeerConnectionInterface::IceConnectionState
      standardized_ice_connection_state_ =
          PeerConnectionInterface::kIceConnectionNew;
  PeerConnectionInterface::PeerConnectionState combined_connection_state_ =
      PeerConnectionInterface::PeerConnectionState::kNew;
  cricket::IceGatheringState ice_gathering_state_ = cricket::kIceGatheringNew;

  Config config_;

  // Early on in the call we don't know if datagram transport is going to be
  // used, but we need to get the server-supported parameters to add to an SDP.
  // This server datagram transport will be promoted to the used datagram
  // transport after the local description is set, and the ownership will be
  // transferred to the actual JsepTransport. This "offer" datagram transport is
  // not created if it's done on the party that provides answer. This offer
  // datagram transport is only created once at the beginning of the connection,
  // and never again.
  std::unique_ptr<DatagramTransportInterface> offer_datagram_transport_ =
      nullptr;

  const cricket::SessionDescription* local_desc_ = nullptr;
  const cricket::SessionDescription* remote_desc_ = nullptr;
  absl::optional<bool> initial_offerer_;

  absl::optional<cricket::ContentGroup> bundle_group_;

  cricket::IceConfig ice_config_;
  cricket::IceRole ice_role_ = cricket::ICEROLE_CONTROLLING;
  uint64_t ice_tiebreaker_ = rtc::CreateRandomId64();
  rtc::scoped_refptr<rtc::RTCCertificate> certificate_;
  rtc::AsyncInvoker invoker_;

  RTC_DISALLOW_COPY_AND_ASSIGN(JsepTransportController);
};

}  // namespace webrtc

#endif  // PC_JSEP_TRANSPORT_CONTROLLER_H_
