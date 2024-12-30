// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once

namespace nvigi
{
	struct InferenceDataAudio;
};

namespace AudioRecordingHelper
{
	struct RecordingInfo;

	RecordingInfo* StartRecordingAudio();
	bool StopRecordingAudio(RecordingInfo* infoPtr, nvigi::InferenceDataAudio* wavData);
};
