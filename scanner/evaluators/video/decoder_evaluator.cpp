/* Copyright 2016 Carnegie Mellon University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/evaluators/video/decoder_evaluator.h"
#include "scanner/evaluators/serialize.h"
#include "scanner/metadata.pb.h"

#include "scanner/util/memory.h"

namespace scanner {

DecoderEvaluator::DecoderEvaluator(const EvaluatorConfig& config,
                                   DeviceType device_type,
                                   VideoDecoderType decoder_type)
    : device_type_(device_type),
      device_id_(config.device_ids[0]),
      decoder_type_(decoder_type),
      needs_warmup_(false),
      discontinuity_(false) {
  decoder_.reset(VideoDecoder::make_from_config(device_type_, device_id_,
                                                decoder_type_, device_type_));
  assert(decoder_.get());
}

void DecoderEvaluator::configure(const VideoMetadata& metadata) {
  metadata_ = metadata;
  frame_size_ = metadata_.width() * metadata_.height() * 3;
  decoder_->configure(metadata);
}

void DecoderEvaluator::reset() {
  needs_warmup_ = true;
  discontinuity_ = true;
}

void DecoderEvaluator::evaluate(
    const std::vector<std::vector<u8*>>& input_buffers,
    const std::vector<std::vector<size_t>>& input_sizes,
    std::vector<std::vector<u8*>>& output_buffers,
    std::vector<std::vector<size_t>>& output_sizes) {
  auto start = now();

  i64 total_frames_decoded = 0;
  i64 total_frames_used = 0;
  size_t num_inputs = input_buffers.empty() ? 0 : input_buffers[0].size();
  for (size_t i = 0; i < num_inputs; ++i) {
    u8* decode_args_buffer = input_buffers[1][i];
    size_t decode_args_buffer_size = input_sizes[1][i];

    const u8* in_encoded_buffer = input_buffers[0][i];
    size_t in_encoded_buffer_size = input_sizes[0][i];

    DecodeArgs args;
    const u8* encoded_buffer;
    size_t encoded_buffer_size = in_encoded_buffer_size;
    if (device_type_ == DeviceType::GPU) {
#ifdef HAVE_CUDA
      u8* buffer = new u8[decode_args_buffer_size];
      memcpy_buffer(buffer, DeviceType::CPU, 0, decode_args_buffer,
                    DeviceType::GPU, device_id_, decode_args_buffer_size);
      args = deserialize_decode_args(buffer, decode_args_buffer_size);
      delete[] buffer;

      buffer = new u8[encoded_buffer_size];
      memcpy_buffer(buffer, DeviceType::CPU, 0, in_encoded_buffer,
                    DeviceType::GPU, device_id_, encoded_buffer_size);
      encoded_buffer = buffer;
#else

#endif
    } else {
      args =
          deserialize_decode_args(decode_args_buffer, decode_args_buffer_size);
      encoded_buffer = in_encoded_buffer;
    }

    std::vector<i32> valid_frames;
    if (args.sampling() == DecodeArgs::All) {
      const DecodeArgs::Interval& interval = args.interval();
      i32 s = interval.start();
      if (!needs_warmup_) {
        s += args.warmup_count();
      }
      for (; s < interval.end(); ++s) {
        valid_frames.push_back(s);
      }
    } else if (args.sampling() == DecodeArgs::Strided) {
      const DecodeArgs::Interval& interval = args.interval();
      i32 s = interval.start();
      i32 e = interval.end();
      i32 stride = args.stride();
      if (!needs_warmup_) {
        s += args.warmup_count() * stride;
      }
      for (; s < e; s += stride) {
        valid_frames.push_back(s);
      }
    } else if (args.sampling() == DecodeArgs::Gather) {
      i32 s = 0;
      if (!needs_warmup_) {
        s += args.warmup_count();
      }
      for (; s < args.gather_points_size(); ++s) {
        valid_frames.push_back(args.gather_points(s));
      }
      discontinuity_ = true;
    } else if (args.sampling() == DecodeArgs::SequenceGather) {
      assert(args.gather_sequences_size() == 1);
      const DecodeArgs::Interval& interval = args.gather_sequences(0);
      i32 s = interval.start();
      if (!needs_warmup_) {
        s += args.warmup_count();
      }
      for (; s < interval.end(); ++s) {
        valid_frames.push_back(s);
      }
      discontinuity_ = true;
    } else {
      assert(false);
    }

    i32 total_output_frames = static_cast<i32>(valid_frames.size());

    size_t encoded_buffer_offset = 0;
    i32 current_frame = args.start_keyframe();
    i32 valid_index = 0;
    while (valid_index < total_output_frames) {
      auto video_start = now();

      i32 encoded_packet_size = 0;
      const u8* encoded_packet = NULL;
      if (encoded_buffer_offset < encoded_buffer_size) {
        encoded_packet_size = *reinterpret_cast<const i32*>(
            encoded_buffer + encoded_buffer_offset);
        encoded_buffer_offset += sizeof(i32);
        encoded_packet = encoded_buffer + encoded_buffer_offset;
        encoded_buffer_offset += encoded_packet_size;
      }

      if (decoder_->feed(encoded_packet, encoded_packet_size, discontinuity_)) {
        // New frames
        bool more_frames = true;
        while (more_frames && valid_index < total_output_frames) {
          if (current_frame == valid_frames[valid_index]) {
            u8* decoded_buffer =
                new_buffer(device_type_, device_id_, frame_size_);
            more_frames = decoder_->get_frame(decoded_buffer, frame_size_);
            output_buffers[0].push_back(decoded_buffer);
            output_sizes[0].push_back(frame_size_);
            valid_index++;
            total_frames_used++;
          } else {
            more_frames = decoder_->discard_frame();
          }
          current_frame++;
          total_frames_decoded++;
        }
      }
      // Set a discontinuity if we sent an empty packet to reset
      // the stream next time
      discontinuity_ = (encoded_packet_size == 0);
    }
    // Wait on all memcpys from frames to be done
    decoder_->wait_until_frames_copied();

    if (decoder_->decoded_frames_buffered() > 0) {
      while (decoder_->discard_frame()) {
        total_frames_decoded++;
      };
    }

    if (device_type_ == DeviceType::GPU) {
      delete[] encoded_buffer;
    }

    // All warmed up
    needs_warmup_ = false;
  }

  if (profiler_) {
    profiler_->add_interval("decode", start, now());
    profiler_->increment("effective_frames", total_frames_used);
    profiler_->increment("decoded_frames", total_frames_decoded);
  }
}

DecoderEvaluatorFactory::DecoderEvaluatorFactory(DeviceType device_type,
                                                 VideoDecoderType decoder_type)
    : device_type_(device_type), decoder_type_(decoder_type) {}

EvaluatorCapabilities DecoderEvaluatorFactory::get_capabilities() {
  EvaluatorCapabilities caps;
  caps.device_type = device_type_;
  caps.max_devices = 1;
  caps.warmup_size = 0;
  caps.can_overlap = true;
  return caps;
}

std::vector<std::string> DecoderEvaluatorFactory::get_output_names() {
  return {"frame"};
}

Evaluator* DecoderEvaluatorFactory::new_evaluator(
    const EvaluatorConfig& config) {
  return new DecoderEvaluator(config, device_type_, decoder_type_);
}
}