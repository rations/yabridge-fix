/*
 * Copyright (C) 2026
 * VST3 Bridge - Wine VST3 Host Bridge
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/**
 * @file audio_processor.h
 * @brief Audio processing loop for Wine host
 */

#pragma once

#include "audio_shm_host.h"
#include "parameter_changes.h"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <thread>
#include "wine_socket_client.h"

namespace vst3bridge {

class WineSocketClient;

/**
 * @brief Handles audio processing in the Wine host process
 * 
 * This class runs in a separate thread and handles the audio processing
 * loop, communicating with the native side via shared memory and sockets.
 */
class AudioProcessor {
public:
    /**
     * @brief Construct audio processor
     * @param audio_shm Audio shared memory host interface
     * @param socket Socket for communication
     * @param plugin VST3 plugin component
     * @param controller VST3 edit controller (may be same as component)
     */
    AudioProcessor(AudioSharedMemoryHost* audio_shm,
                    WineSocketClient* socket,
                    Steinberg::IAudioProcessor* processor,
                    Steinberg::IEditController* controller);

    ~AudioProcessor();

    /**
     * @brief Start the audio processing thread
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Stop the audio processing thread
     */
    void stop();

    /**
     * @brief Check if the processor is running
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Set up bus configuration
     * @param input_bus_count Number of input buses
     * @param output_bus_count Number of output buses
     * @return true if successful
     */
    bool setupBuses(Steinberg::int32 input_bus_count, Steinberg::int32 output_bus_count);

private:
    /**
     * @brief Main audio processing loop
     */
    void processingLoop();

    /**
     * @brief Process one buffer of audio
     * @param num_samples Number of samples to process
     * @return true if successful
     */
    bool processBuffer(uint32_t num_samples);

    /**
     * @brief Copy audio data from shared memory to VST3 buffers
     * @param num_samples Number of samples
     */
    void copyFromSharedMemory(uint32_t num_samples);

    /**
     * @brief Copy audio data from VST3 buffers to shared memory
     * @param num_samples Number of samples
     */
    void copyToSharedMemory(uint32_t num_samples);

    /**
     * @brief Update parameter changes from controller
     */
    void updateParameters();

    AudioSharedMemoryHost* audio_shm_;
    WineSocketClient* socket_;
    Steinberg::IAudioProcessor* processor_;
    Steinberg::IEditController* controller_;

    std::thread processing_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // VST3 processing data
    Steinberg::ProcessData process_data_;
    std::vector<Steinberg::AudioBusBuffers> input_buses_;
    std::vector<Steinberg::AudioBusBuffers> output_buses_;
    std::vector<std::vector<float*>> input_channel_ptrs_;
    std::vector<std::vector<float*>> output_channel_ptrs_;
    
    // Scratch buffers for audio data
    std::vector<std::vector<float>> input_scratch_;
    std::vector<std::vector<float>> output_scratch_;

    // Parameter change queues (using proper VST3 interfaces)
    // These are owned by this class and released in destructor
    ParameterChanges* input_param_changes_ = nullptr;
    ParameterChanges* output_param_changes_ = nullptr;
    
    // Parameter change builder for accumulating changes from IPC
    ParameterChangesBuilder param_builder_;
};

} // namespace vst3bridge
