// Copyright (C) <2019> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include "VideoGstAnalyzerWrap.h"

#include <node.h>

using namespace v8;

// void InitAll(Handle<Object> exports) {
//   PipelineWrapper::Init(exports);
// }

NODE_MODULE(addon, VideoGstAnalyzerWrap::Init)
