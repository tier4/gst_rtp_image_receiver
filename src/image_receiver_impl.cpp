// Copyright 2025 Tier IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "image_receiver.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <mutex>
#include <chrono>
#include <cstring>
#include <atomic>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdio>
#include <functional>
#include <map>

class ImageReceiver::Impl {
private:
    // configuration
    ReceiverConfig config_;
    HardwareType hw_type_;

    // gstreamer pipeline
    GstElement* pipeline_ = nullptr;
    GstElement* identity_ = nullptr;

    // appsink
    GstElement* appsink_raw_ = nullptr;
    GstElement* appsink_jpeg_ = nullptr;
        
    // callback
    FrameCallback raw_frame_callback_;
    FrameCallback jpeg_frame_callback_;
    CombinedFrameCallback combined_frame_callback_;

    // running status
    std::atomic<bool> running_{false};
    
    // RTP timestamp tracking
    std::mutex rtp_mutex_;
    ImageReceiver::RTPTimestamp last_rtp_timestamp_;

    // for synchronization raw and jpeg
    std::mutex sync_mutex_;
    std::map<GstClockTime, GstBuffer*> raw_buffer_map_;
    std::map<GstClockTime, GstBuffer*> jpeg_buffer_map_;
    
    // Statistics
    std::atomic<uint64_t> frames_processed_raw_{0};
    std::atomic<uint64_t> frames_processed_jpeg_{0};
    std::atomic<uint64_t> frames_dropped_raw_{0};
    std::atomic<uint64_t> frames_dropped_jpeg_{0};

    // timestamp
    std::chrono::steady_clock::time_point start_time_;
    
public:
    Impl(const ReceiverConfig& config) : config_(config) {
        gst_init(nullptr, nullptr);
        hw_type_ = detectHardware();
        std::cout << "Detected hardware type: " << static_cast<int>(hw_type_) << std::endl;
    }
    
    ~Impl() {
        stop();
        clearBufferMaps();
        if (identity_) {
            gst_object_unref(identity_);
        }
        if (appsink_raw_) {
            gst_object_unref(appsink_raw_);
        }
        if (appsink_jpeg_) {
            gst_object_unref(appsink_jpeg_);
        }
        if (pipeline_) {
            gst_object_unref(pipeline_);
        }
    }
    
    bool start() {
        if (running_) return false;
        
        // Build pipeline
        std::string pipeline_str = buildPipelineString();
        std::cout << "Pipeline string: " << pipeline_str << std::endl;
        
        GError* error = nullptr;
        pipeline_ = gst_parse_launch(pipeline_str.c_str(), &error);
        
        if (error) {
            std::cerr << "Pipeline creation error: " << error->message << std::endl;
            g_error_free(error);
            return false;
        }
        
        // Setup AppSink
        setupAppSink();
        
        // Setup RTP probe
        setupRTPProbe();
        
        // Start
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        running_ = true;
        start_time_ = std::chrono::steady_clock::now();
        
        return true;
    }
    
    void stop() {
        if (!running_) return;
        
        running_ = false;
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        clearBufferMaps();
    }
    
    void setRawFrameCallback(FrameCallback callback) {
        raw_frame_callback_ = callback;
    }
    
    void setJpegFrameCallback(FrameCallback callback) {
        jpeg_frame_callback_ = callback;
    }

    void setCombinedFrameCallback(CombinedFrameCallback callback) {
        combined_frame_callback_ = callback;
    }

    Statistics getStatistics() const {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
        
        Statistics stats;
        stats.frames_processed_raw = frames_processed_raw_;
        stats.frames_processed_jpeg = frames_processed_jpeg_;
        stats.frames_dropped_raw = frames_dropped_raw_;
        stats.frames_dropped_jpeg = frames_dropped_jpeg_;

        // FPSはRaw側を基準とする (JPEGのみモードの場合はRawが0になる)
        if (stats.frames_processed_raw > 0) {
            stats.average_fps_raw = duration > 0 ? static_cast<double>(frames_processed_raw_) / duration : 0;
        } else if (stats.frames_processed_jpeg > 0) {
            // Rawがない場合はJPEG側を基準
            stats.average_fps_raw = duration > 0 ? static_cast<double>(frames_processed_jpeg_) / duration : 0;
        } else {
            stats.average_fps_raw = 0;
        }

        return stats;
    }
    
private:
    HardwareType detectHardware() {
        // Platform detection logic
        #ifdef JETSON_PLATFORM
            return HardwareType::NVIDIA_JETSON;
        #else
            // Check VA-API availability
            if (checkVAAPI()) {
                std::string vendor = getGPUVendor();
                if (vendor.find("Intel") != std::string::npos) {
                    return HardwareType::VAAPI_INTEL;
                } else if (vendor.find("AMD") != std::string::npos) {
                    return HardwareType::VAAPI_AMD;
                }
            }
            
            // Check NVIDIA Desktop GPU
            if (checkNVIDIADesktop()) {
                return HardwareType::NVIDIA_DESKTOP;
            }
            
            return HardwareType::SOFTWARE_FALLBACK;
        #endif
    }

    // clear sync buffer
    void clearBufferMaps() {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        for (auto& pair : raw_buffer_map_) {
            gst_buffer_unref(pair.second);
        }
        raw_buffer_map_.clear();
        for (auto& pair : jpeg_buffer_map_) {
            gst_buffer_unref(pair.second);
        }
        jpeg_buffer_map_.clear();
    }
    
    std::string buildPipelineString() {
        std::string base_pipeline = 
            "udpsrc port=" + std::to_string(config_.udp_port) + 
            " buffer-size=" + std::to_string(config_.buffer_size) + 
            " caps=\"application/x-rtp, sampling=(string)YCbCr-4:2:2, depth=(string)8, "
            "width=(string)" + std::to_string(config_.width) + ", height=(string)" + std::to_string(config_.height) + "\" ! "
            "identity name=rtpidentity ! "
            "rtpvrawdepay ";

        // raw pipeline
        std::string raw_pipeline;
        if (hw_type_ == HardwareType::NVIDIA_DESKTOP || hw_type_ == HardwareType::NVIDIA_JETSON) {
            // Use nvvidconv for hardware-accelerated YUV -> BGRx conversion
            // and then videoconvert for BGRx -> BGR.
            raw_pipeline = "nvvidconv ! video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR";
        } else {
            // Default to software conversion
            raw_pipeline = "videoconvert ! video/x-raw,format=BGR";
        }

        // jpeg pipeline        
        std::string jpeg_pipeline;
        switch (hw_type_) {
            case HardwareType::VAAPI_INTEL:
            case HardwareType::VAAPI_AMD:
                jpeg_pipeline = buildVAAPIPipeline();
                break;
                
            case HardwareType::NVIDIA_DESKTOP:
                jpeg_pipeline = buildNVIDIADesktopPipeline();
                break;
                
            case HardwareType::NVIDIA_JETSON:
                jpeg_pipeline = buildJetsonPipeline();
                break;
                
            default:
                jpeg_pipeline = buildSoftwarePipeline();
                break;
        }

        // generate branch
        switch (config_.mode) {
            case SinkMode::RAW_ONLY:
                // raw only
                base_pipeline += " ! " + raw_pipeline + " ! appsink name=sink_raw";
                break;

            case SinkMode::JPEG_ONLY:
                // jpeg only
                base_pipeline += " ! " + jpeg_pipeline + " ! appsink name=sink_jpeg";
                break;
            case SinkMode::RAW_AND_JPEG:
            default:
                // jpeg & raw
                base_pipeline += " ! tee name=t ";
                std::string raw_branch = "t. ! queue ! " + raw_pipeline +  " ! appsink name=sink_raw";
                std::string jpeg_branch = "t. ! queue ! " + jpeg_pipeline + " ! appsink name=sink_jpeg";
                base_pipeline += raw_branch + " " + jpeg_branch;
                break;
        }
        
        return base_pipeline;
    }
    
    std::string buildVAAPIPipeline() {
        return "videoconvert ! "
               "video/x-raw,format=NV12 !"
               "vaapijpegenc quality=" + std::to_string(config_.jpeg_quality);
    }
    // std::string buildVAAPIPipeline() {
    //     return
    //         "video/x-raw,format=UYVY,width=" + std::to_string(config_.width) + ",height=" + std::to_string(config_.height) + " !"
    //         "vaapipostproc !"
    //         "video/x-raw,format=NV12,width=" + std::to_string(config_.width) + ",height=" + std::to_string(config_.height) + " !"
    //         "vaapijpegenc quality=" + std::to_string(config_.jpeg_quality) + " !";
    // }

    std::string buildJetsonPipeline() {
        // Jetson pipeline - always use NVMM memory for best performance
        return "nvvidconv ! "
               "video/x-raw(memory:NVMM),format=I420 ! "
               "nvjpegenc quality=" + std::to_string(config_.jpeg_quality) + 
               " idct-method=ifast";
    }
    
    std::string buildNVIDIADesktopPipeline() {
        // Desktop NVIDIA GPU
        return "videoconvert ! "  // TODO: Add nvvidconv support
               "nvjpegenc quality=" + std::to_string(config_.jpeg_quality);
    }
    
    std::string buildSoftwarePipeline() {
        return "videoconvert ! "
               "video/x-raw,format=RGB ! "
               "jpegenc quality=" + std::to_string(config_.jpeg_quality); 
    }
    
    void setupAppSink() {        
        // Raw Sink
        if (config_.mode == SinkMode::RAW_ONLY || config_.mode == SinkMode::RAW_AND_JPEG) {
            appsink_raw_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink_raw");
            if (appsink_raw_) {
                g_object_set(appsink_raw_,
                    "emit-signals", TRUE,
                    "sync", FALSE,
                    "max-buffers", config_.max_buffers,
                    "drop", TRUE,
                    nullptr);
                
                g_signal_connect(appsink_raw_, "new-sample", 
                                G_CALLBACK(onNewSampleRaw), this);
                std::cout << "Setup AppSink: sink_raw" << std::endl;
            } else {
                std::cerr << "Failed to get sink_raw" << std::endl;
            }
        }

        // Jpeg Sink
        if (config_.mode == SinkMode::JPEG_ONLY || config_.mode == SinkMode::RAW_AND_JPEG) {
            appsink_jpeg_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink_jpeg");
            if (appsink_jpeg_) {
                g_object_set(appsink_jpeg_,
                    "emit-signals", TRUE,
                    "sync", FALSE,
                    "max-buffers", config_.max_buffers,
                    "drop", TRUE,
                    nullptr);
                
                g_signal_connect(appsink_jpeg_, "new-sample", 
                                G_CALLBACK(onNewSampleJpeg), this);
                std::cout << "Setup AppSink: sink_jpeg" << std::endl;
            } else {
                 std::cerr << "Failed to get sink_jpeg" << std::endl;
            }
        }
    }
    
    static GstFlowReturn onNewSampleRaw(GstAppSink* sink, gpointer user_data) {
        auto* impl = static_cast<Impl*>(user_data);
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) {
            impl->frames_dropped_raw_++; 
            return GST_FLOW_ERROR;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (!buffer) {
            gst_sample_unref(sample);
            impl->frames_dropped_raw_++;
            return GST_FLOW_ERROR;
        }

        // RAW_AND_JPEG mode -> synchronize buffer process
        if (impl->config_.mode == SinkMode::RAW_AND_JPEG) {
            GstClockTime pts = GST_BUFFER_PTS(buffer);
            if (pts == GST_CLOCK_TIME_NONE) {
                // when PTS was not found -> can't synchronize -> release
                gst_sample_unref(sample);
                impl->frames_dropped_raw_++;
                return GST_FLOW_OK;
            }

            // add buffer count and hold buffer
            gst_buffer_ref(buffer);
            
            GstBuffer* jpeg_buffer = nullptr;
            
            {
                std::lock_guard<std::mutex> lock(impl->sync_mutex_);
                auto it = impl->jpeg_buffer_map_.find(pts);
                
                if (it != impl->jpeg_buffer_map_.end()) {
                    // Jpeg is before
                    jpeg_buffer = it->second;
                    impl->jpeg_buffer_map_.erase(it);
                } else {
                    // Raw is before
                    impl->raw_buffer_map_[pts] = buffer;
                }
            } // unlock mutex

            if (jpeg_buffer) {
                // find pare
                impl->processCombinedFrame(buffer, jpeg_buffer);
                gst_buffer_unref(buffer); // release ref
                gst_buffer_unref(jpeg_buffer); // release ref
            }
            
            gst_sample_unref(sample);
            return GST_FLOW_OK;

        } else {
            // RAW_ONLY mode
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                ImageReceiver::RTPTimestamp rtp_ts = impl->extractRTPTimestamp(buffer);
                if (impl->raw_frame_callback_) {
                    impl->raw_frame_callback_(map.data, map.size, rtp_ts);
                }
                impl->frames_processed_raw_++;
                gst_buffer_unmap(buffer, &map);
            } else {
                impl->frames_dropped_raw_++;
            }
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
    }

    // onNewSampleJpeg callback
    static GstFlowReturn onNewSampleJpeg(GstAppSink* sink, gpointer user_data) {
        auto* impl = static_cast<Impl*>(user_data);
        
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) {
            impl->frames_dropped_jpeg_++; 
            return GST_FLOW_ERROR;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (!buffer) {
            gst_sample_unref(sample);
            impl->frames_dropped_jpeg_++;
            return GST_FLOW_ERROR;
        }

        // RAW_AND_JPEG mode -> buffer synchronization process
        if (impl->config_.mode == SinkMode::RAW_AND_JPEG) {
            GstClockTime pts = GST_BUFFER_PTS(buffer);
            if (pts == GST_CLOCK_TIME_NONE) {
                gst_sample_unref(sample);
                impl->frames_dropped_jpeg_++;
                return GST_FLOW_OK;
            }

            // increment buffer count and hold
            gst_buffer_ref(buffer);
            GstBuffer* raw_buffer = nullptr;

            {
                std::lock_guard<std::mutex> lock(impl->sync_mutex_);
                auto it = impl->raw_buffer_map_.find(pts);
                
                if (it != impl->raw_buffer_map_.end()) {
                    // raw is before
                    raw_buffer = it->second;
                    impl->raw_buffer_map_.erase(it);
                } else {
                    // jpeg is before
                    impl->jpeg_buffer_map_[pts] = buffer;
                }
            } // unlock Mutex

            if (raw_buffer) {
                // found pare
                impl->processCombinedFrame(raw_buffer, buffer);
                gst_buffer_unref(raw_buffer); // release ref
                gst_buffer_unref(buffer); // release ref
            }
            
            gst_sample_unref(sample);
            return GST_FLOW_OK;

        } else {
            // JPEG_ONLY mode
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                ImageReceiver::RTPTimestamp rtp_ts = impl->extractRTPTimestamp(buffer);
                if (impl->jpeg_frame_callback_) {
                    impl->jpeg_frame_callback_(map.data, map.size, rtp_ts);
                }
                impl->frames_processed_jpeg_++;
                gst_buffer_unmap(buffer, &map);
            } else {
                impl->frames_dropped_jpeg_++;
            }
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
    }

    // Combined frame processing helper
    void processCombinedFrame(GstBuffer* raw_buffer, GstBuffer* jpeg_buffer) {
        if (!combined_frame_callback_) {
            // nothing to do when callback is not configured
            frames_dropped_raw_++;
            frames_dropped_jpeg_++;
            return;
        }

        GstMapInfo raw_map, jpeg_map;
        bool raw_mapped = false;
        bool jpeg_mapped = false;

        // get PTS time stamp from jpeg buffer
        ImageReceiver::RTPTimestamp rtp_ts = extractRTPTimestamp(jpeg_buffer);

        if (gst_buffer_map(raw_buffer, &raw_map, GST_MAP_READ)) {
            raw_mapped = true;
            if (gst_buffer_map(jpeg_buffer, &jpeg_map, GST_MAP_READ)) {
                jpeg_mapped = true;
                
                // callback when each jpeg map and raw map success
                combined_frame_callback_(
                    raw_map.data, raw_map.size,
                    jpeg_map.data, jpeg_map.size,
                    rtp_ts
                );
                
                frames_processed_raw_++;
                frames_processed_jpeg_++;

            } else {
                // failed JPEG mapping
                frames_dropped_jpeg_++;
            }
        } else {
            // failed Raw mapping
            frames_dropped_raw_++;
        }

        // release map
        if (raw_mapped) {
            gst_buffer_unmap(raw_buffer, &raw_map);
        }
        if (jpeg_mapped) {
            gst_buffer_unmap(jpeg_buffer, &jpeg_map);
        }
    }
    
    bool checkVAAPI() {
        // Check VA-API availability
        GstElement* test = gst_element_factory_make("vaapipostproc", nullptr);
        if (test) {
            gst_object_unref(test);
            std::cout << "VA-API is available" << std::endl;
            return true;
        }
        std::cout << "VA-API is NOT available" << std::endl;
        return false;
    }
    
    bool checkNVIDIADesktop() {
        // Check NVIDIA GPU existence
        #ifndef JETSON_PLATFORM
            // First check for nvjpegenc
            GstElement* test = gst_element_factory_make("nvjpegenc", nullptr);
            if (test) {
                gst_object_unref(test);
                return true;
            }
            // Also check for nvvidconv as indicator of NVIDIA support
            test = gst_element_factory_make("nvvidconv", nullptr);
            if (test) {
                gst_object_unref(test);
                // NVIDIA support exists but nvjpegenc might not be available
                // Fall back to software in this case
                return false;
            }
        #endif
        return false;
    }
    
    std::string getGPUVendor() {
        // GPU vendor detection
        // Try multiple possible locations for Intel GPU
        std::vector<std::string> paths = {
            "/sys/class/drm/card0/device/vendor",
            "/sys/class/drm/card1/device/vendor",
            "/sys/class/drm/renderD128/device/vendor"
        };
        
        for (const auto& path : paths) {
            std::ifstream file(path);
            if (file.is_open()) {
                std::string vendor_id;
                file >> vendor_id;
                file.close();
                
                std::cout << "Found GPU vendor ID: " << vendor_id << " at " << path << std::endl;
                
                if (vendor_id == "0x8086") return "Intel";
                if (vendor_id == "0x1002") return "AMD"; 
                if (vendor_id == "0x10de") return "NVIDIA";
            }
        }
        
        // Fallback: check if Intel GPU via lspci
        FILE* pipe = popen("lspci | grep -i 'vga\\|3d' | grep -i intel", "r");
        if (pipe) {
            char buffer[128];
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                pclose(pipe);
                std::cout << "Intel GPU detected via lspci" << std::endl;
                return "Intel";
            }
            pclose(pipe);
        }
        
        return "Unknown";
    }
    
    void setupRTPProbe() {
        // Get the identity element from the pipeline
        identity_ = gst_bin_get_by_name(GST_BIN(pipeline_), "rtpidentity");
        if (identity_) {
            GstPad* srcpad = gst_element_get_static_pad(identity_, "src");
            if (srcpad) {
                // Add probe to capture RTP buffers
                gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
                                 onRTPBuffer, this, nullptr);
                gst_object_unref(srcpad);
            }
        }
    }
    
    static GstPadProbeReturn onRTPBuffer(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
        auto* impl = static_cast<Impl*>(user_data);
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        
        if (buffer) {
            ImageReceiver::RTPTimestamp rtp_ts = {};
            
            // Try to extract RTP header information
            GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
            if (gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
                rtp_ts.rtp_timestamp = gst_rtp_buffer_get_timestamp(&rtp);
                rtp_ts.ssrc = gst_rtp_buffer_get_ssrc(&rtp);
                
                // Get CSRC if available
                guint8 csrc_count = gst_rtp_buffer_get_csrc_count(&rtp);
                if (csrc_count > 0) {
                    rtp_ts.csrc = gst_rtp_buffer_get_csrc(&rtp, 0);
                }
                
                // Reconstruct 96-bit custom timestamp according to TIER IV documentation:
                // - RTP timestamp (32 bits): High 32 bits of seconds
                // - SSRC (32 bits): Low 16 bits of seconds + High 16 bits of nanoseconds
                // - CSRC (32 bits): Low 16 bits of nanoseconds + 16 bits fractions
                
                if (csrc_count > 0) {
                    // Full custom timestamp is available (96-bit total)
                    // According to TIER IV documentation:
                    // - Bits [95:48] (48 bits): Seconds
                    // - Bits [47:16] (32 bits): Nanoseconds  
                    // - Bits [15:0] (16 bits): Fractions of nanoseconds
                    //
                    // RTP Header mapping:
                    // - RTP timestamp (32 bits): Seconds bits [47:16]
                    // - SSRC (32 bits): 
                    //   - Upper 16 bits: Seconds bits [15:0]
                    //   - Lower 16 bits: Nanoseconds bits [31:16]
                    // - CSRC (32 bits):
                    //   - Upper 16 bits: Nanoseconds bits [15:0]
                    //   - Lower 16 bits: Fractions bits [15:0]
                    
                    // Seconds: 48 bits total
                    // RTP timestamp contains seconds[47:16] (upper 32 bits of 48-bit seconds)
                    // SSRC upper 16 bits contains seconds[15:0] (lower 16 bits of 48-bit seconds)
                    uint64_t seconds_high_32 = static_cast<uint64_t>(rtp_ts.rtp_timestamp);
                    uint64_t seconds_low_16 = static_cast<uint64_t>((rtp_ts.ssrc >> 16) & 0xFFFF);
                    rtp_ts.seconds = (seconds_high_32 << 16) | seconds_low_16;
                    
                    // Nanoseconds: 32 bits total
                    // SSRC lower 16 bits contains nanoseconds[31:16] (upper 16 bits)
                    // CSRC upper 16 bits contains nanoseconds[15:0] (lower 16 bits)
                    uint32_t nanos_high_16 = (rtp_ts.ssrc & 0xFFFF);
                    uint32_t nanos_low_16 = ((rtp_ts.csrc >> 16) & 0xFFFF);
                    rtp_ts.nanoseconds = (nanos_high_16 << 16) | nanos_low_16;
                    
                    // Fractions: 16 bits from CSRC lower 16 bits
                    rtp_ts.fractions = (rtp_ts.csrc & 0xFFFF);
                } else {
                    // Fallback to system time if custom timestamp not available
                    auto now = std::chrono::system_clock::now();
                    auto duration = now.time_since_epoch();
                    auto total_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
                    
                    rtp_ts.seconds = total_nanos / 1000000000;
                    rtp_ts.nanoseconds = total_nanos % 1000000000;
                    rtp_ts.fractions = 0;
                }
                
                gst_rtp_buffer_unmap(&rtp);
                
                // Store the RTP timestamp
                std::lock_guard<std::mutex> lock(impl->rtp_mutex_);
                impl->last_rtp_timestamp_ = rtp_ts;
            }
        }
        
        return GST_PAD_PROBE_OK;
    }
    
    ImageReceiver::RTPTimestamp extractRTPTimestamp(GstBuffer* buffer) {
        ImageReceiver::RTPTimestamp rtp_ts;
        
        // Get the RTP timestamp calculated from RTP headers
        {
            std::lock_guard<std::mutex> lock(rtp_mutex_);
            rtp_ts = last_rtp_timestamp_;
        }
        
        // Update PTS from current buffer
        GstClockTime timestamp = GST_BUFFER_PTS(buffer);
        rtp_ts.pts_ms = timestamp != GST_CLOCK_TIME_NONE ? 
                        timestamp / 1000000 : 0; // nanoseconds to milliseconds
        
        // If no RTP custom timestamp available (no CSRC), use system time as fallback
        if (rtp_ts.seconds == 0 && rtp_ts.nanoseconds == 0) {
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            auto total_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
            
            rtp_ts.seconds = total_nanos / 1000000000;
            rtp_ts.nanoseconds = total_nanos % 1000000000;
        }
        
        return rtp_ts;
    }
};