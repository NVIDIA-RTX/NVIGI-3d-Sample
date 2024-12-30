// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once
#include <iostream>
#include <string>
#include <fstream>
#include <cstdint>

using std::cin;
using std::cout;
using std::endl;
using std::fstream;
using std::string;

#pragma pack(push, 1)
struct WavHeader {
    char     chunkId[4];
    uint32_t chunkSize;
    char     format[4];
    char     subchunk1Id[4];
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char     subchunk2Id[4];
    uint32_t subchunk2Size;
};
#pragma pack(pop)

int getFileSize(FILE* inFile)
{
    int fileSize = 0;
    fseek(inFile, 0, SEEK_END);

    fileSize = ftell(inFile);

    fseek(inFile, 0, SEEK_SET);
    return fileSize;
}

bool GetAudioFile(string input_filename, float*& out_audio, unsigned int& out_audio_len)
{
    const char* filename = input_filename.c_str();

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error opening file: " << filename << std::endl;
        return false;
    }

    // Get the file size
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read the WAV header
    WavHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(WavHeader));

    // Check if the file is a WAV file
    if (std::string(header.chunkId, 4) != "RIFF" ||
        std::string(header.format, 4) != "WAVE") {
        std::cerr << "Not a valid WAV file: " << filename << std::endl;
        return false;
    }

    // Print some information about the WAV file
    std::cout << "Channels: " << header.numChannels << std::endl;
    std::cout << "Sample Rate: " << header.sampleRate << " Hz" << std::endl;
    std::cout << "Bit Depth: " << header.bitsPerSample << " bits" << std::endl;
    std::cout << "Duration: " << static_cast<float>(header.subchunk2Size) / header.byteRate << " seconds" << std::endl;

    // Read and process the audio data
    //const std::size_t bufferSize = 1024;
    //char buffer[bufferSize];
    

    //while (file.tellg() < fileSize) {
    //    file.read(buffer, bufferSize);

    //    // Process the buffer here (e.g., analyze or manipulate the audio data)
    //    // ...

    //    // For this example, we are just reading and printing the bytes
    //    for (std::streamsize i = 0; i < file.gcount(); ++i) {
    //        std::cout << static_cast<int>(static_cast<uint8_t>(buffer[i])) << " ";
    //    }
    //}
    
    // Assuming little-endian representation for all cases
    if (header.bitsPerSample == 8)          // uint8
    {
        out_audio_len = fileSize;        // every byte of file data represents one value
        uint8_t* dyn_buffer = (uint8_t*)malloc(fileSize);
        file.read((char*)dyn_buffer, fileSize);
        out_audio = (float*)malloc(sizeof(float) * out_audio_len);
        for (int i = 0; i < out_audio_len; ++i)
        {
            out_audio[i] = (uint8_t)dyn_buffer[i] / 128.0f - 1.0f;
        }
        free(dyn_buffer);
    }
    else if (header.bitsPerSample == 16)    // int16
    {
        out_audio_len = fileSize / 2;    // every two bytes of file data represents one value
        int16_t* dyn_buffer = (int16_t*)malloc(fileSize);
        file.read((char*)dyn_buffer, fileSize);
        out_audio = (float*)malloc(sizeof(float) * out_audio_len);
        for (int i = 0; i < out_audio_len; ++i)
        {
            out_audio[i] = (int16_t)dyn_buffer[i] / 32768.0f;
        }
        free(dyn_buffer);
    }
    else if (header.bitsPerSample == 32)   // int32
    {
        out_audio_len = fileSize / 4;     // every four bytes of file data represents one value
        int32_t* dyn_buffer = (int32_t*)malloc(fileSize);
        file.read((char*)dyn_buffer, fileSize);
        out_audio = (float*)malloc(sizeof(float) * out_audio_len);
        for (int i = 0; i < out_audio_len; ++i)
        {
            out_audio[i] = (int32_t)dyn_buffer[i] / 2147483648.0f;
        }
        free(dyn_buffer);
    }

    file.close();

    return true;
}
