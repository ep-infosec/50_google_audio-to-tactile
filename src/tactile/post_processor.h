/* Copyright 2019, 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *
 * Tactile post processing: equalization, clipping, and lowpassing.
 *
 * This library performs post processing on tactile signals so that they play
 * well on the tactors.
 *
 * Outline:
 *
 * 1. Perceptual equalization filter is applied to compensate for the
 *    variability in perceived intensity vs. frequency.
 *
 * 2. Signals are multiplied by the output gain factor.
 *
 * 3. Signals are hard clipped to [-0.96, 0.96] (a few percent headroom for the
 *    Butterworth filter in the next step). PCEN produces large onset peaks, and
 *    extreme peaks needs to be clipped to a producible amplitude.
 *
 * 4. Signals are lowpass filtered with a second-order Butterworth filter.
 *    Energy above 1 kHz does not contribute at all to tactile perception, but
 *    may produce audible leakage. Also, this lowpassing softens cusps that hard
 *    clipping introduced into the waveform in the previous step.
 *
 *    (The lowpass filter's impulse response is nonnegative except for a small
 *    negative ripple. If we ignore that ripple, its nonnegative shape implies
 *    that the filter does not increase the signal's max amplitude, so we don't
 *    need to clip a second time.)
 *
 * 5. To limit the peak power drawn by the tactors, signals are scaled down when
 *    needed so that (sum_c x[c]^2) <= output_limit. The output_limit is tuned
 *    automatically: when PostProcessorLowBattery() is called, the limit is
 *    reduced, and otherwise it is slowly increased.
 */

#ifndef AUDIO_TO_TACTILE_SRC_TACTILE_POST_PROCESSOR_H_
#define AUDIO_TO_TACTILE_SRC_TACTILE_POST_PROCESSOR_H_

#include "dsp/biquad_filter.h"
#include "tactile/tuning.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max supported number of channels. */
#define kPostProcessorMaxChannels 24

typedef struct {
  /* Whether to apply perceptual equalizer. */
  int /*bool*/ use_equalizer;
  /* (If use_equalizer = 1) Equalizer mid band gain. */
  float mid_gain;
  /* (If use_equalizer = 1) Equalizer high band gain. */
  float high_gain;
  /* Output gain, applied just before clipping. */
  float gain;
  /* Lowpass cutoff frequency in Hz. */
  float cutoff_hz;
  /* Limit for summed output power. */
  float output_limit;
} PostProcessorParams;

/* Set `params` to default values. */
void PostProcessorSetDefaultParams(PostProcessorParams* params);

typedef struct {
  BiquadFilterState equalizer_biquad_state[2];
  BiquadFilterState lpf_biquad_state;
} PostProcessorChannelState;

typedef struct {
  BiquadFilterCoeffs equalizer_biquad_coeffs[2];
  BiquadFilterCoeffs lpf_biquad_coeffs;
  int num_channels;

  PostProcessorChannelState channels[kPostProcessorMaxChannels];
  float limit_grow_coeff;
  float output_limit;
  int recovery;
} PostProcessor;

/* Initializes post processing. Returns 1 on success, 0 on failure. */
int /*bool*/ PostProcessorInit(PostProcessor* state,
                               const PostProcessorParams* params,
                               float sample_rate_hz,
                               int num_channels);

/* Resets to initial state. */
void PostProcessorReset(PostProcessor* state);

/* Processes in-place in a streaming manner, where `input_output` points to an
 * array of `num_frames * num_channels` samples in interleaved order.
 */
void PostProcessorProcessSamples(PostProcessor* state,
                                 float* input_output,
                                 int num_frames);

/* Tells the post processor that the battery is low. When called, the post
 * processor scales down the output to consume less power.
 */
void PostProcessorLowBattery(PostProcessor* state);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* AUDIO_TO_TACTILE_SRC_TACTILE_POST_PROCESSOR_H_ */
