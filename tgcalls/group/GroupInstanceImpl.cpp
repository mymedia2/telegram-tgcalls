#include "GroupInstanceImpl.h"

#include <memory>
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include "api/peer_connection_interface.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "sdk/media_constraints.h"
#include "api/peer_connection_interface.h"
#include "api/video_track_source_proxy.h"
#include "system_wrappers/include/field_trial.h"
#include "api/stats/rtcstats_objects.h"

#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"
#include "LogSinkImpl.h"

#include <sstream>
#include <iostream>

namespace tgcalls {

namespace {

static std::vector<std::string> splitSdpLines(std::string const &sdp) {
    std::vector<std::string> result;
    
    std::istringstream sdpStream(sdp);
    
    std::string s;
    while (std::getline(sdpStream, s, '\n')) {
        if (s.size() == 0) {
            continue;
        }
        if (s[s.size() - 1] == '\r') {
            s.resize(s.size() - 1);
        }
        result.push_back(s);
    }
    
    return result;
}

static std::vector<std::string> splitFingerprintLines(std::string const &line) {
    std::vector<std::string> result;
    
    std::istringstream sdpStream(line);
    
    std::string s;
    while (std::getline(sdpStream, s, ' ')) {
        if (s.size() == 0) {
            continue;
        }
        result.push_back(s);
    }
    
    return result;
}

static std::vector<std::string> getLines(std::vector<std::string> const &lines, std::string prefix) {
    std::vector<std::string> result;
    
    for (auto &line : lines) {
        if (line.find(prefix) == 0) {
            auto cleanLine = line;
            cleanLine.replace(0, prefix.size(), "");
            result.push_back(cleanLine);
        }
    }
    
    return result;
}

static absl::optional<GroupJoinPayload> parseSdpIntoJoinPayload(std::string const &sdp) {
    GroupJoinPayload result;
    
    auto lines = splitSdpLines(sdp);
    
    std::vector<std::string> audioLines;
    bool isAudioLine = false;
    for (auto &line : lines) {
        if (line.find("m=audio") == 0) {
            isAudioLine = true;
        }
        if (isAudioLine) {
            audioLines.push_back(line);
        }
    }
    
    std::vector<uint32_t> audioSources;
    for (auto &line : getLines(audioLines, "a=ssrc:")) {
        std::istringstream iss(line);
        uint32_t value = 0;
        iss >> value;
        if (std::find(audioSources.begin(), audioSources.end(), value) == audioSources.end()) {
            audioSources.push_back(value);
        }
    }
    
    if (audioSources.size() != 1) {
        return absl::nullopt;
    }
    result.ssrc = audioSources[0];
    
    auto ufragLines = getLines(lines, "a=ice-ufrag:");
    if (ufragLines.size() != 1) {
        return absl::nullopt;
    }
    result.ufrag = ufragLines[0];
    
    auto pwdLines = getLines(lines, "a=ice-pwd:");
    if (pwdLines.size() != 1) {
        return absl::nullopt;
    }
    result.pwd = pwdLines[0];
    
    for (auto &line : getLines(lines, "a=fingerprint:")) {
        auto fingerprintComponents = splitFingerprintLines(line);
        if (fingerprintComponents.size() != 2) {
            continue;
        }
        
        GroupJoinPayloadFingerprint fingerprint;
        fingerprint.hash = fingerprintComponents[0];
        fingerprint.fingerprint = fingerprintComponents[1];
        fingerprint.setup = "active";
        result.fingerprints.push_back(fingerprint);
    }
    
    return result;
}

struct StreamSpec {
    bool isMain = false;
    uint32_t audioSsrc = 0;
    bool isRemoved = false;
};

static void appendSdp(std::vector<std::string> &lines, std::string const &line) {
    lines.push_back(line);
}

static std::string createSdp(uint32_t sessionId, GroupJoinResponsePayload const &payload, bool isAnswer, std::vector<StreamSpec> const &bundleStreams) {
    std::vector<std::string> sdp;
    
    appendSdp(sdp, "v=0");
    
    std::ostringstream sessionIdString;
    sessionIdString << "o=- ";
    sessionIdString << sessionId;
    sessionIdString << " 2 IN IP4 0.0.0.0";
    appendSdp(sdp, sessionIdString.str());
    
    appendSdp(sdp, "s=-");
    appendSdp(sdp, "t=0 0");
    
    std::ostringstream bundleString;
    bundleString << "a=group:BUNDLE";
    for (auto &stream : bundleStreams) {
        bundleString << " ";
        if (stream.isMain) {
            bundleString << "0";
        } else {
            bundleString << "audio";
            bundleString << stream.audioSsrc;
        }
    }
    appendSdp(sdp, bundleString.str());
    
    appendSdp(sdp, "a=ice-lite");
    
    for (auto &stream : bundleStreams) {
        std::ostringstream audioMidString;
        if (stream.isMain) {
            audioMidString << "0";
        } else {
            audioMidString << "audio";
            audioMidString << stream.audioSsrc;
        }
        
        std::ostringstream mLineString;
        mLineString << "m=audio ";
        if (stream.isMain) {
            mLineString << "1";
        } else {
            mLineString << "0";
        }
        mLineString << " RTP/SAVPF 111 126";
        
        appendSdp(sdp, mLineString.str());
        
        if (stream.isMain) {
            appendSdp(sdp, "c=IN IP4 0.0.0.0");
        }
        
        std::ostringstream mLineMidString;
        mLineMidString << "a=mid:";
        mLineMidString << audioMidString.str();
        appendSdp(sdp, mLineMidString.str());
        
        if (stream.isRemoved) {
            appendSdp(sdp, "a=inactive");
        } else {
            if (stream.isMain) {
                std::ostringstream ufragString;
                ufragString << "a=ice-ufrag:";
                ufragString << payload.ufrag;
                appendSdp(sdp, ufragString.str());
                
                std::ostringstream pwdString;
                pwdString << "a=ice-pwd:";
                pwdString << payload.pwd;
                appendSdp(sdp, pwdString.str());
                
                for (auto &fingerprint : payload.fingerprints) {
                    std::ostringstream fingerprintString;
                    fingerprintString << "a=fingerprint:";
                    fingerprintString << fingerprint.hash;
                    fingerprintString << " ";
                    fingerprintString << fingerprint.fingerprint;
                    appendSdp(sdp, fingerprintString.str());
                    appendSdp(sdp, "a=setup:passive");
                }
                
                for (auto &candidate : payload.candidates) {
                    std::ostringstream candidateString;
                    candidateString << "a=candidate:";
                    candidateString << candidate.foundation;
                    candidateString << " ";
                    candidateString << candidate.component;
                    candidateString << " ";
                    candidateString << candidate.protocol;
                    candidateString << " ";
                    candidateString << candidate.priority;
                    candidateString << " ";
                    candidateString << candidate.ip;
                    candidateString << " ";
                    candidateString << candidate.port;
                    candidateString << " ";
                    candidateString << "typ ";
                    candidateString << candidate.type;
                    candidateString << " ";
                    
                    if (candidate.type == "srflx" || candidate.type == "prflx" || candidate.type == "relay") {
                        if (candidate.relAddr.size() != 0 && candidate.relPort.size() != 0) {
                            candidateString << "raddr ";
                            candidateString << candidate.relAddr;
                            candidateString << " ";
                            candidateString << "rport ";
                            candidateString << candidate.relPort;
                            candidateString << " ";
                        }
                    }
                    
                    if (candidate.protocol == "tcp") {
                        if (candidate.tcpType.size() != 0) {
                            candidateString << "tcptype ";
                            candidateString << candidate.tcpType;
                            candidateString << " ";
                        }
                    }
                    
                    candidateString << "generation ";
                    candidateString << candidate.generation;
                    
                    appendSdp(sdp, candidateString.str());
                }
            }
            
            appendSdp(sdp, "a=rtpmap:111 opus/48000/2");
            appendSdp(sdp, "a=rtpmap:126 telephone-event/8000");
            appendSdp(sdp, "a=fmtp:111 minptime=10; useinbandfec=1; usedtx=1");
            appendSdp(sdp, "a=rtcp:1 IN IP4 0.0.0.0");
            appendSdp(sdp, "a=rtcp-mux");
            appendSdp(sdp, "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level");
            appendSdp(sdp, "a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
            appendSdp(sdp, "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
            appendSdp(sdp, "a=rtcp-fb:111 transport-cc");
            
            if (isAnswer) {
                appendSdp(sdp, "a=recvonly");
            } else {
                if (stream.isMain) {
                    appendSdp(sdp, "a=sendrecv");
                } else {
                    appendSdp(sdp, "a=sendonly");
                    appendSdp(sdp, "a=bundle-only");
                }
                
                std::ostringstream ssrcGroupString;
                ssrcGroupString << "a=ssrc-group:FID ";
                ssrcGroupString << stream.audioSsrc;
                appendSdp(sdp, ssrcGroupString.str());
                
                std::ostringstream cnameString;
                cnameString << "a=ssrc:";
                cnameString << stream.audioSsrc;
                cnameString << " cname:stream";
                cnameString << stream.audioSsrc;
                appendSdp(sdp, cnameString.str());
                
                std::ostringstream msidString;
                msidString << "a=ssrc:";
                msidString << stream.audioSsrc;
                msidString << " msid:stream";
                msidString << stream.audioSsrc;
                msidString << " audio" << stream.audioSsrc;
                appendSdp(sdp, msidString.str());
                
                std::ostringstream mslabelString;
                mslabelString << "a=ssrc:";
                mslabelString << stream.audioSsrc;
                mslabelString << " mslabel:audio";
                mslabelString << stream.audioSsrc;
                appendSdp(sdp, mslabelString.str());
                
                std::ostringstream labelString;
                labelString << "a=ssrc:";
                labelString << stream.audioSsrc;
                labelString << " label:audio";
                labelString << stream.audioSsrc;
                appendSdp(sdp, labelString.str());
            }
        }
    }
    
    std::ostringstream result;
    for (auto &line : sdp) {
        result << line << "\n";
    }
    
    return result.str();
}

static std::string parseJoinResponseIntoSdp(uint32_t sessionId, uint32_t mainStreamAudioSsrc, GroupJoinResponsePayload const &payload, bool isAnswer, std::vector<uint32_t> const &otherSsrcs) {
    
    std::vector<StreamSpec> bundleStreams;
    
    StreamSpec mainStream;
    mainStream.isMain = true;
    mainStream.audioSsrc = mainStreamAudioSsrc;
    mainStream.isRemoved = false;
    bundleStreams.push_back(mainStream);
    
    for (auto ssrc : otherSsrcs) {
        StreamSpec stream;
        stream.isMain = false;
        stream.audioSsrc = ssrc;
        stream.isRemoved = false;
        bundleStreams.push_back(stream);
    }
    
    return createSdp(sessionId, payload, isAnswer, bundleStreams);
}

rtc::Thread *makeNetworkThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
    value->SetName("WebRTC-Reference-Network", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getNetworkThread() {
    static rtc::Thread *value = makeNetworkThread();
    return value;
}

rtc::Thread *makeWorkerThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
    value->SetName("WebRTC-Reference-Worker", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getWorkerThread() {
    static rtc::Thread *value = makeWorkerThread();
    return value;
}

rtc::Thread *getSignalingThread() {
    return Manager::getMediaThread();
}

rtc::Thread *getMediaThread() {
    return Manager::getMediaThread();
}

class FrameEncryptorImpl : public webrtc::FrameEncryptorInterface {
public:
    FrameEncryptorImpl() {
    }
    
    virtual int Encrypt(cricket::MediaType media_type,
                        uint32_t ssrc,
                        rtc::ArrayView<const uint8_t> additional_data,
                        rtc::ArrayView<const uint8_t> frame,
                        rtc::ArrayView<uint8_t> encrypted_frame,
                        size_t* bytes_written) override {
        memcpy(encrypted_frame.data(), frame.data(), frame.size());
        for (auto it = encrypted_frame.begin(); it != encrypted_frame.end(); it++) {
            *it ^= 123;
        }
        *bytes_written = frame.size();
        return 0;
    }

    virtual size_t GetMaxCiphertextByteSize(cricket::MediaType media_type,
                                            size_t frame_size) override {
        return frame_size;
    }
};

class FrameDecryptorImpl : public webrtc::FrameDecryptorInterface {
public:
    FrameDecryptorImpl() {
    }
    
    virtual webrtc::FrameDecryptorInterface::Result Decrypt(cricket::MediaType media_type,
                           const std::vector<uint32_t>& csrcs,
                           rtc::ArrayView<const uint8_t> additional_data,
                           rtc::ArrayView<const uint8_t> encrypted_frame,
                           rtc::ArrayView<uint8_t> frame) override {
        memcpy(frame.data(), encrypted_frame.data(), encrypted_frame.size());
        for (auto it = frame.begin(); it != frame.end(); it++) {
            *it ^= 123;
        }
        return webrtc::FrameDecryptorInterface::Result(webrtc::FrameDecryptorInterface::Status::kOk, encrypted_frame.size());
    }

    virtual size_t GetMaxPlaintextByteSize(cricket::MediaType media_type,
                                           size_t encrypted_frame_size) override {
        return encrypted_frame_size;
    }
};

class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
private:
    std::function<void(std::string, int, std::string)> _discoveredIceCandidate;
    std::function<void(bool)> _connectionStateChanged;
    std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)> _onTrackAdded;
    std::function<void(rtc::scoped_refptr<webrtc::RtpReceiverInterface>)> _onTrackRemoved;

public:
    PeerConnectionObserverImpl(
        std::function<void(std::string, int, std::string)> discoveredIceCandidate,
        std::function<void(bool)> connectionStateChanged,
        std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)> onTrackAdded,
        std::function<void(rtc::scoped_refptr<webrtc::RtpReceiverInterface>)> onTrackRemoved
    ) :
    _discoveredIceCandidate(discoveredIceCandidate),
    _connectionStateChanged(connectionStateChanged),
    _onTrackAdded(onTrackAdded),
    _onTrackRemoved(onTrackRemoved) {
    }

    virtual void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
        bool isConnected = false;
        if (new_state == webrtc::PeerConnectionInterface::SignalingState::kStable) {
            isConnected = true;
        }
        _connectionStateChanged(isConnected);
    }

    virtual void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
    }

    virtual void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
    }

    virtual void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    }

    virtual void OnRenegotiationNeeded() {
    }

    virtual void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    }

    virtual void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    }

    virtual void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    }

    virtual void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    }

    virtual void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
        std::string sdp;
        candidate->ToString(&sdp);
        _discoveredIceCandidate(sdp, candidate->sdp_mline_index(), candidate->sdp_mid());
    }

    virtual void OnIceCandidateError(const std::string& host_candidate, const std::string& url, int error_code, const std::string& error_text) {
    }

    virtual void OnIceCandidateError(const std::string& address,
                                     int port,
                                     const std::string& url,
                                     int error_code,
                                     const std::string& error_text) {
    }

    virtual void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) {
    }

    virtual void OnIceConnectionReceivingChange(bool receiving) {
    }

    virtual void OnIceSelectedCandidatePairChanged(const cricket::CandidatePairChangeEvent& event) {
    }

    virtual void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    }

    virtual void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        /*if (transceiver->receiver()) {
            rtc::scoped_refptr<FrameDecryptorImpl> decryptor(new rtc::RefCountedObject<FrameDecryptorImpl>());
            transceiver->receiver()->SetFrameDecryptor(decryptor);
        }*/
        
        _onTrackAdded(transceiver);
    }

    virtual void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
        _onTrackRemoved(receiver);
    }

    virtual void OnInterestingUsage(int usage_pattern) {
    }
};

class RTCStatsCollectorCallbackImpl : public webrtc::RTCStatsCollectorCallback {
public:
    RTCStatsCollectorCallbackImpl(std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &)> completion) :
    _completion(completion) {
    }

    virtual void OnStatsDelivered(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &report) override {
        _completion(report);
    }

private:
    std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &)> _completion;
};

class AudioTrackSinkInterfaceImpl: public webrtc::AudioTrackSinkInterface {
private:
    std::function<void(float)> _update;
    
    int _peakCount = 0;
    uint16_t _peak = 0;
    
public:
    AudioTrackSinkInterfaceImpl(std::function<void(float)> update) :
    _update(update) {
    }
    
    virtual ~AudioTrackSinkInterfaceImpl() {
    }
    
    virtual void OnData(const void *audio_data, int bits_per_sample, int sample_rate, size_t number_of_channels, size_t number_of_frames) override {
        if (bits_per_sample == 16 && number_of_channels == 1) {
            int bytesPerSample = bits_per_sample / 8;
            int16_t *samples = (int16_t *)audio_data;
            for (int i = 0; i < number_of_frames / bytesPerSample; i++) {
                int16_t sample = samples[i];
                if (sample < 0) {
                    sample = -sample;
                }
                if (_peak < sample) {
                    _peak = sample;
                }
                _peakCount += 1;
            }
        }
        
        if (_peakCount >= 1200) {
            float level = ((float)(_peak)) / 4000.0f;
            _peak = 0;
            _peakCount = 0;
            _update(level);
            if (level > 0.1f) {
                printf("level: %f\n", level);
            }
        }
    }
};

class CreateSessionDescriptionObserverImpl : public webrtc::CreateSessionDescriptionObserver {
private:
    std::function<void(std::string, std::string)> _completion;

public:
    CreateSessionDescriptionObserverImpl(std::function<void(std::string, std::string)> completion) :
    _completion(completion) {
    }

    virtual void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        if (desc) {
            std::string sdp;
            desc->ToString(&sdp);

            _completion(sdp, desc->type());
        }
    }

    virtual void OnFailure(webrtc::RTCError error) override {
    }
};

class SetSessionDescriptionObserverImpl : public webrtc::SetSessionDescriptionObserver {
private:
    std::function<void()> _completion;

public:
    SetSessionDescriptionObserverImpl(std::function<void()> completion) :
    _completion(completion) {
    }

    virtual void OnSuccess() override {
        _completion();
    }

    virtual void OnFailure(webrtc::RTCError error) override {
        printf("Error\n");
    }
};

template <typename Out>
void split(const std::string &s, char delim, Out result) {
    std::istringstream iss(s);
    std::string item;
    while (std::getline(iss, item, delim)) {
        *result++ = item;
    }
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, std::back_inserter(elems));
    return elems;
}

std::string adjustLocalDescription(const std::string &sdp) {
    std::vector<std::string> lines = split(sdp, '\n');
    
    std::string pattern = "c=IN ";
    
    bool foundAudio = false;
    std::stringstream result;
    for (const auto &it : lines) {
        result << it << "\n";
        if (!foundAudio && it.compare(0, pattern.size(), pattern) == 0) {
            foundAudio = true;
            result << "b=AS:" << 32 << "\n";
        }
    }
    
    return result.str();
}

} // namespace

class GroupInstanceManager : public std::enable_shared_from_this<GroupInstanceManager> {
public:
	GroupInstanceManager(GroupInstanceDescriptor &&descriptor) :
    _networkStateUpdated(descriptor.networkStateUpdated),
    _audioLevelsUpdated(descriptor.audioLevelsUpdated) {
	}

	~GroupInstanceManager() {
        assert(getMediaThread()->IsCurrent());

        if (_peerConnection) {
            _peerConnection->Close();
        }
	}

	void start() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        
        webrtc::field_trial::InitFieldTrialsFromString(
            //"WebRTC-Audio-SendSideBwe/Enabled/"
            "WebRTC-Audio-Allocation/min:6kbps,max:32kbps/"
            "WebRTC-Audio-OpusMinPacketLossRate/Enabled-1/"
            //"WebRTC-FlexFEC-03/Enabled/"
            //"WebRTC-FlexFEC-03-Advertised/Enabled/"
            "WebRTC-PcFactoryDefaultBitrates/min:6kbps,start:32kbps,max:32kbps/"
        );

        PlatformInterface::SharedInstance()->configurePlatformAudio();
        
        webrtc::PeerConnectionFactoryDependencies dependencies;
        dependencies.network_thread = getNetworkThread();
        dependencies.worker_thread = getWorkerThread();
        dependencies.signaling_thread = getSignalingThread();
        dependencies.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();

        cricket::MediaEngineDependencies mediaDeps;
        mediaDeps.task_queue_factory = dependencies.task_queue_factory.get();
        mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
        mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
        mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
        mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();

        webrtc::AudioProcessing *apm = webrtc::AudioProcessingBuilder().Create();
        webrtc::AudioProcessing::Config audioConfig;
        webrtc::AudioProcessing::Config::NoiseSuppression noiseSuppression;
        noiseSuppression.enabled = true;
        noiseSuppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        audioConfig.noise_suppression = noiseSuppression;

        audioConfig.high_pass_filter.enabled = true;

        apm->ApplyConfig(audioConfig);

        mediaDeps.audio_processing = apm;

        dependencies.media_engine = cricket::CreateMediaEngine(std::move(mediaDeps));
        dependencies.call_factory = webrtc::CreateCallFactory();
        dependencies.event_log_factory =
            std::make_unique<webrtc::RtcEventLogFactory>(dependencies.task_queue_factory.get());
        dependencies.network_controller_factory = nullptr;
        dependencies.media_transport_factory = nullptr;

        _nativeFactory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        //config.continual_gathering_policy = webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;
        config.audio_jitter_buffer_fast_accelerate = true;
        config.prioritize_most_likely_ice_candidate_pairs = true;
        config.presume_writable_when_fully_relayed = true;
        //config.audio_jitter_buffer_enable_rtx_handling = true;
        
        /*webrtc::CryptoOptions cryptoOptions;
        webrtc::CryptoOptions::SFrame sframe;
        sframe.require_frame_encryption = true;
        cryptoOptions.sframe = sframe;
        config.crypto_options = cryptoOptions;*/

        _observer.reset(new PeerConnectionObserverImpl(
            [weak](std::string sdp, int mid, std::string sdpMid) {
                /*getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, mid, sdpMid](){
                    auto strong = weak.lock();
                    if (strong) {
                        //strong->emitIceCandidate(sdp, mid, sdpMid);
                    }
                });*/
            },
            [weak](bool isConnected) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isConnected](){
                    auto strong = weak.lock();
                    if (strong) {
                        strong->updateIsConnected(isConnected);
                    }
                });
            },
            [weak](rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, transceiver](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->onTrackAdded(transceiver);
                });
            },
            [weak](rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, receiver](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->onTrackRemoved(receiver);
                });
            }
        ));
        _peerConnection = _nativeFactory->CreatePeerConnection(config, nullptr, nullptr, _observer.get());
        assert(_peerConnection != nullptr);

        cricket::AudioOptions options;
        rtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource = _nativeFactory->CreateAudioSource(options);
        std::stringstream name;
        name << "audio";
        name << 0;
        std::vector<std::string> streamIds;
        streamIds.push_back(name.str());
        _localAudioTrack = _nativeFactory->CreateAudioTrack(name.str(), audioSource);
        _localAudioTrack->set_enabled(false);
        _peerConnection->AddTrack(_localAudioTrack, streamIds);
        
        //beginStatsTimer(100);
	}
    
    void updateIsConnected(bool isConnected) {
        _networkStateUpdated(isConnected);
    }
    
    void emitJoinPayload(std::function<void(GroupJoinPayload)> completion) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak, completion](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type, completion](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                auto adjustedSdp = sdp;

                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, adjustLocalDescription(adjustedSdp), &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, adjustedSdp, completion]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        auto payload = parseSdpIntoJoinPayload(adjustedSdp);
                        if (payload) {
                            strong->_mainStreamAudioSsrc = payload->ssrc;
                            completion(payload.value());
                        }
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                } else {
                    return;
                }
            });
        }));
        _peerConnection->CreateOffer(observer, options);
    }
    
    void setJoinResponsePayload(GroupJoinResponsePayload payload) {
        _joinPayload = payload;
        auto sdp = parseJoinResponseIntoSdp(_sessionId, _mainStreamAudioSsrc, payload, true, _otherSsrcs);
        setOfferSdp(sdp, true);
    }
    
    void setSsrcs(std::vector<uint32_t> ssrcs) {
        if (!_joinPayload) {
            return;
        }
        
        for (auto ssrc : ssrcs) {
            if (std::find(_otherSsrcs.begin(), _otherSsrcs.end(), ssrc) == _otherSsrcs.end()) {
                _otherSsrcs.push_back(ssrc);
            }
        }
        
        auto sdp = parseJoinResponseIntoSdp(_sessionId, _mainStreamAudioSsrc, _joinPayload.value(), false, _otherSsrcs);
        setOfferSdp(sdp, false);
    }
    
    void setOfferSdp(std::string const &offerSdp, bool isAnswer) {
        if (!isAnswer && _appliedRemoteRescription == offerSdp) {
            return;
        }
        _appliedRemoteRescription = offerSdp;
        
        printf("----- setOfferSdp %s -----\n", isAnswer ? "answer" : "offer");
        printf("%s\n", offerSdp.c_str());
        printf("-----\n");
        
        webrtc::SdpParseError error;
        webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(isAnswer ? "answer" : "offer", adjustLocalDescription(offerSdp), &error);
        if (!sessionDescription) {
            return;
        }
        
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, isAnswer]() {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isAnswer](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                if (!isAnswer) {
                    strong->emitAnswer();
                }
            });
        }));
        
        _peerConnection->SetRemoteDescription(observer, sessionDescription);
    }
    
    void beginStatsTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->collectStats();
            });
        }, timeoutMs);
    }
    
    void beginLevelsTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                std::vector<std::pair<uint32_t, float>> levels;
                for (auto &it : strong->_audioLevels) {
                    if (it.second > 0.001f) {
                        levels.push_back(std::make_pair(it.first, it.second));
                    }
                }
                strong->_audioLevelsUpdated(levels);
                
                strong->beginLevelsTimer(50);
            });
        }, timeoutMs);
    }
    
    void collectStats() {
        /*for (auto &it : _peerConnection->GetTransceivers()) {
            if (it->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO && it->receiver()) {
                if (it->receiver()->streams().size() == 0) {
                    continue;
                }
                if (it->receiver()->streams()[0]->GetAudioTracks().size() == 0) {
                    continue;
                }
                int signalLevel = 0;
                if (it->receiver()->streams()[0]->GetAudioTracks()[0]->GetSignalLevel(&signalLevel)) {
                    if (signalLevel > 10) {
                        printf("level %d\n", signalLevel);
                    }
                }
            }
        }*/
    }
    
    void reportStats(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &stats) {
    }
    
    void onTrackAdded(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        if (transceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly && transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
            if (transceiver->mid()) {
                auto streamId = transceiver->mid().value();
                if (streamId.find("audio") != 0) {
                    return;
                }
                streamId.replace(0, 5, "");
                std::istringstream iss(streamId);
                uint32_t ssrc = 0;
                iss >> ssrc;
                
                auto remoteAudioTrack = static_cast<webrtc::AudioTrackInterface *>(transceiver->receiver()->track().get());
                if (_audioTrackSinks.find(streamId) == _audioTrackSinks.end()) {
                    const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
                    std::shared_ptr<AudioTrackSinkInterfaceImpl> sink(new AudioTrackSinkInterfaceImpl([weak, ssrc](float level) {
                        getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ssrc, level](){
                            auto strong = weak.lock();
                            if (!strong) {
                                return;
                            }
                            strong->_audioLevels[ssrc] = level;
                        });
                    }));
                    _audioTrackSinks[streamId] = sink;
                    remoteAudioTrack->AddSink(sink.get());
                }
            }
        }
    }
    
    void onTrackRemoved(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    }
    
    void setIsMuted(bool isMuted) {
        _localAudioTrack->set_enabled(!isMuted);
    }
    
    void emitAnswer() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, adjustLocalDescription(sdp), &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, sdp]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                } else {
                    return;
                }
            });
        }));
        _peerConnection->CreateAnswer(observer, options);
    }

private:
    std::function<void(std::string const &)> _sdpAnswerEmitted;
    std::function<void(std::vector<std::string> const &)> _incomingVideoStreamListUpdated;
    std::function<void(bool)> _networkStateUpdated;
    std::function<void(std::vector<std::pair<uint32_t, float>> const &)> _audioLevelsUpdated;
    
    uint32_t _sessionId = 6543245;
    uint32_t _mainStreamAudioSsrc = 0;
    absl::optional<GroupJoinResponsePayload> _joinPayload;
    std::vector<uint32_t> _otherSsrcs;
    
    std::string _appliedRemoteRescription;
    std::vector<std::string> _partialRemoteDescriptionQueue;
    
    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> _nativeFactory;
    std::unique_ptr<PeerConnectionObserverImpl> _observer;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> _peerConnection;
    std::unique_ptr<AudioTrackSinkInterfaceImpl> _localAudioTrackSink;
    rtc::scoped_refptr<webrtc::AudioTrackInterface> _localAudioTrack;
    
    std::map<std::string, std::shared_ptr<AudioTrackSinkInterfaceImpl>> _audioTrackSinks;
    std::map<uint32_t, float> _audioLevels;
    std::unique_ptr<webrtc::MediaConstraints> _nativeConstraints;
};

GroupInstanceImpl::GroupInstanceImpl(GroupInstanceDescriptor &&descriptor) {
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(true);
    if (_logSink) {
		rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
	}
    
	_manager.reset(new ThreadLocalObject<GroupInstanceManager>(getMediaThread(), [descriptor = std::move(descriptor)]() mutable {
		return new GroupInstanceManager(std::move(descriptor));
	}));
	_manager->perform(RTC_FROM_HERE, [](GroupInstanceManager *manager) {
		manager->start();
	});
}

GroupInstanceImpl::~GroupInstanceImpl() {
	if (_logSink) {
		rtc::LogMessage::RemoveLogToStream(_logSink.get());
	}
}

void GroupInstanceImpl::emitJoinPayload(std::function<void(GroupJoinPayload)> completion) {
    _manager->perform(RTC_FROM_HERE, [completion](GroupInstanceManager *manager) {
        manager->emitJoinPayload(completion);
    });
}

void GroupInstanceImpl::setJoinResponsePayload(GroupJoinResponsePayload payload) {
    _manager->perform(RTC_FROM_HERE, [payload](GroupInstanceManager *manager) {
        manager->setJoinResponsePayload(payload);
    });
}

void GroupInstanceImpl::setSsrcs(std::vector<uint32_t> ssrcs) {
    _manager->perform(RTC_FROM_HERE, [ssrcs](GroupInstanceManager *manager) {
        manager->setSsrcs(ssrcs);
    });
}

void GroupInstanceImpl::setIsMuted(bool isMuted) {
    _manager->perform(RTC_FROM_HERE, [isMuted](GroupInstanceManager *manager) {
        manager->setIsMuted(isMuted);
    });
}

} // namespace tgcalls