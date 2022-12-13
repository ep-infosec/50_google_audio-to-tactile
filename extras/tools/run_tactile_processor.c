/* Copyright 2019, 2021-2022 Google LLC
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
 * Audio to tactile demo program with TactileProcessor, on mic or WAV input.
 *
 * This program runs audio to tactile processing on microphone or WAV input,
 * producing a visualization and 10-channel tactile signal output. The 10 output
 * channels (with base-1 indexing) are
 *
 *   1: baseband  6: eh
 *   2: aa        7: ae
 *   3: uw        8: uh
 *   4: ih        9: sh fricative
 *   5: iy       10: fricative
 *
 * Tactile output is sent as audio output using PortAudio to the device set by
 * --output. The intention is that these channels are rendered with tactors on
 * the wrist to implement the "bracelet" form factor, arranged as:
 *
 *           (9)--(4)   Dorsal
 *          /        \
 *        (6)        (1)
 *         |          |
 *        (5)        (2)
 *          \        /
 *   Volar  (10)--(3)
 *
 * Or rendered on the volar side of the forearm for the "sleeve" form factor as:
 *
 *                        (7)-(6)   sh fricative
 *                        /     \      (9)
 *   Proximal    (1)    (2) (8) (5)               Distal
 *             baseband   \     /     (10)
 *                        (3)-(4)   fricative
 *                     vowel cluster
 *
 * Use --channels to map tactile signals to output channels. For instance,
 * --channels=3,1,2,2 plays signal 3 on channel 1, signal 1 on channel 2, and
 * signal 2 on channels 3 and 4. A "0" in the channels list means that channel
 * is filled with zeros, e.g. --channels=1,0,2 sets channel 2 to zeros.
 *
 * Flags:
 *  --input=<name>             Input device to get source audio from.
 *  --input=<wavfile>          Alternatively, input can be read from a WAV file.
 *                             The WAV file determines the sample rate.
 *  --output=<name>            Output device to play tactor signals to.
 *  --sample_rate_hz=<int>     Sample rate. Note that most devices only support
 *                             a few standard audio sample rates, e.g. 44100.
 *  --channels=<list>          Channel mapping.
 *  --channel_gains_db=<list>  Gains in dB for each channel. Usually negative
 *                             values, to avoid clipping. More negative value
 *                             means more attenuation, for example -13 is lower
 *                             in level than -10.
 *  --gain_db=<float>          Overall output gain in dB.
 *  --mid_gain_db=<float>      Equalizer mid band gain in dB (default -10).
 *  --high_gain_db=<float>     Equalizer high band gain in dB (default -5.5).
 *  --block_size=<int>         TactileProcessor block_size. Must be power of 2.
 *  --chunk_size=<int>         Frames per PortAudio buffer. (Default 256).
 *  --cutoff_hz=<float>        Cutoff in Hz for energy smoothing filters.
 *  --fullscreen               Fullscreen display.
 *
 * Use keyboard buttons to control the program:
 *   Tab       Switches visualization between bracelet and sleeve form factors.
 *   Q, Esc    Quit the program.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/dsp/fast_fun.h"
#include "src/dsp/number_util.h"
#include "src/dsp/read_wav_file.h"
#include "src/tactile/post_processor.h"
#include "src/tactile/tactile_processor.h"
#include "extras/tools/channel_map_tui.h"
#include "extras/tools/portaudio_device.h"
#include "extras/tools/sdl/basic_sdl_app.h"
#include "extras/tools/sdl/texture_from_rle_data.h"
#include "extras/tools/sdl/window_icon.h"
#include "extras/tools/util.h"
#include "portaudio.h"

/* The visualization has nominally 10 tactors even for the bracelet. The unused
 * tactors are simply mapped to blank images.
 */
#define kNumTactors 10

#define kNumFormFactors 2
#define kNumImageAssets (kNumTactors + 1)
/* Defined in run_tactile_processor_assets.c. */
extern const uint8_t* kBraceletImageAssetsRle[kNumImageAssets];
extern const uint8_t* kSleeveImageAssetsRle[kNumImageAssets];

typedef struct {
  SDL_Texture* images[kNumImageAssets];
  SDL_Rect image_rects[kNumImageAssets];
} FormFactorAssets;

typedef struct {
  /* SDL variables. */
  BasicSdlApp app;
  FormFactorAssets form_factors[kNumFormFactors];
  int selected_form_factor;        /* 0 => bracelet, 1 => sleeve.            */

  /* PortAudio variables. */
  int pa_initialized;              /* Whether PortAudio was initialized.     */
  PaError pa_error;                /* Last error returned from PortAudio.    */
  PaStream* pa_stream;             /* PortAudio output stream.               */
  ChannelMap channel_map;
  float sample_rate_hz;
  int chunk_size;

  int keep_running;                /* Whether main loop should keep running. */

  float* input_wav_samples;
  int input_wav_size;
  int input_wav_pos;

  float volume_decay_coeff;
  volatile float volume[kNumTactors];

  TactileProcessor* tactile_processor;
  PostProcessor post_processor;
  float* tactile_output;
} Engine;

/* Loads the assets for a form factor. */
static int LoadFormFactorAssets(SDL_Renderer* renderer, int window_fullscreen,
    const uint8_t* data[kNumImageAssets], FormFactorAssets* form_factor) {
  int i;
  for (i = 0; i < kNumImageAssets; ++i) {
    form_factor->images[i] = CreateTextureFromRleData(
        data[i], renderer, &form_factor->image_rects[i]);
    if (!form_factor->images[i]) { return 0; }
    if (window_fullscreen) {
      form_factor->image_rects[i].x += 167;
    }
  }
  SDL_SetTextureColorMod(form_factor->images[kNumTactors], 0x9d, 0x8c, 0x78);
  return 1;
}

int StartSdl(Engine* engine, int window_fullscreen, int window_borderless,
             int window_on_top) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "Error: Failed to initialize SDL: %s\n", SDL_GetError());
    return 0;
  }

  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  /* Create application window. */
  const int screen_width = window_fullscreen ? 640 : 326;
  const int screen_height = 512;
  uint32_t flags = SDL_WINDOW_HIDDEN;
  if (window_fullscreen) { flags |= SDL_WINDOW_FULLSCREEN; }
  if (window_borderless) { flags |= SDL_WINDOW_BORDERLESS; }
  if (window_on_top) { flags |= SDL_WINDOW_ALWAYS_ON_TOP; }
  if (!BasicSdlAppCreate(&engine->app,
        "Tactile Processor", screen_width, screen_height, flags)) {
    return 0;
  }
  /* Set a nice window icon. */
  SetWindowIcon(engine->app.window);

  /* Create SDL_Textures from embedded image assets. */
  return LoadFormFactorAssets(engine->app.renderer, window_fullscreen,
                              kBraceletImageAssetsRle,
                              &engine->form_factors[0]) &&
         LoadFormFactorAssets(engine->app.renderer, window_fullscreen,
                              kSleeveImageAssetsRle,
                              &engine->form_factors[1]);
}

/* Reads input WAV file. */
int ReadInputWav(Engine* engine, const char* input_wav) {
  size_t num_samples;
  int num_channels;
  int sample_rate_hz;
  int32_t* samples_int32 = ReadWavFile(
      input_wav, &num_samples, &num_channels, &sample_rate_hz);
  if (samples_int32 == NULL) { return 0; }

  const int num_frames = num_samples / num_channels;
  /* Round up to a whole number of chunks to simplify buffering. */
  engine->input_wav_size = RoundUpToMultiple(num_frames, engine->chunk_size);
  engine->input_wav_samples = (float*)malloc(
      sizeof(float) * engine->input_wav_size);
  if (engine->input_wav_samples == NULL) {
    fprintf(stderr, "Error: Memory allocation failed.\n");
    return 0;
  }
  engine->input_wav_size = num_frames;
  engine->input_wav_pos = 0;
  engine->sample_rate_hz = sample_rate_hz;

  /* Convert to float and mix down to mono. */
  const int32_t* src = samples_int32;
  const float scale = 1.0f / (INT32_MAX * num_channels);
  int i;
  for (i = 0; i < num_frames; ++i, src += num_channels) {
    float sum = 0.0f;
    int c;
    for (c = 0; c < num_channels; ++c) {
      sum += (float)src[c];
    }
    engine->input_wav_samples[i] = scale * sum;
  }
  for (; i < engine->input_wav_size; ++i) {
    engine->input_wav_samples[i] = 0.0f;
  }

  free(samples_int32);
  return 1;
}

/* Processes one chunk of audio data. */
void ProcessChunk(Engine* engine, float* input, float* output) {
  const int block_size = CarlFrontendBlockSize(
      engine->tactile_processor->frontend);
  const int num_blocks = engine->chunk_size / block_size;
  float energy_accum[kNumTactors] = {0.0f};
  int b;

  for (b = 0; b < num_blocks; ++b) {
    float* tactile_output = engine->tactile_output;
    /* Run audio-to-tactile processing. */
    TactileProcessorProcessSamples(
        engine->tactile_processor, input, tactile_output);

    /* Accumulate signals for visualization. Do this before post processing. */
    int i;
    for (i = 0; i < block_size; ++i) {
      int c;
      for (c = 0; c < kNumTactors; ++c) {
        energy_accum[c] += tactile_output[c] * tactile_output[c];
      }
      tactile_output += kNumTactors;
    }

    /* Apply equalization, clipping, and lowpass filtering. */
    tactile_output = engine->tactile_output;
    PostProcessorProcessSamples(
        &engine->post_processor, tactile_output, block_size);

    /* Map channels and apply channel gains. */
    ChannelMapApply(&engine->channel_map, tactile_output, block_size, output);

    input += block_size;
    output += engine->channel_map.num_output_channels * block_size;
  }

  int c;
  for (c = 0; c < kNumTactors; ++c) {
    /* Convert tactile energy to perceived strength with Steven's power law.
     * Perceived strength is roughly proportional to acceleration^0.55, which is
     * proportional to sqrt(energy)^0.55.
     *
     * Different experiments have measured the exponent to be between 0.32 and
     * 0.81. The exponent depends especially on the stimulus frequency. Over
     * our range of interest 80-250 Hz, Ryu2010 measured 0.55.
     *
     * Reference: Ryu, "Psychophysical model for vibrotactile rendering in
     * mobile devices," Presence 19.4 (2010): 364-387.
     */
    const float perceived = FastPow(1e-12f + energy_accum[c]
        / (num_blocks * block_size), 0.55f * 0.5f);
    /* Update engine->volume[c] according to
     *   volume = max(rms, volume * volume_decay_coeff).
     * This way the visualization follows the RMS with instantaneous attack but
     * smoothed release, so that onsets are well represented.
     */
    float updated_volume = engine->volume[c] * engine->volume_decay_coeff;
    if (perceived > updated_volume) { updated_volume = perceived; }
    engine->volume[c] = updated_volume;
  }
}

/* The audio thread calls this function for every chunk of audio. */
int PortAudioCallback(const void *input_buffer, void *output_buffer,
    unsigned long chunk_size,
    const PaStreamCallbackTimeInfo* time_info,
    PaStreamCallbackFlags status_flags,
    void *user_data) {
  if (status_flags & paOutputUnderflow) {
    fprintf(stderr, "Error: Underflow in tactile output. "
        "chunk_size (%lu) might be too small.\n", chunk_size);
  }

  const float* input = (const float*)input_buffer;
  float* output = (float*)output_buffer;
  Engine* engine = (Engine*)user_data;

  /* WAV input. */
  if (engine->input_wav_samples) {
    const int num_frames = (int)chunk_size;
    if (engine->input_wav_pos + num_frames > engine->input_wav_size) {
      engine->input_wav_pos = 0;
    }
    input = engine->input_wav_samples + engine->input_wav_pos;
    engine->input_wav_pos += num_frames;
  }

  ProcessChunk(engine, (float*)input, output);
  return paContinue;
}

/* Initializes PortAudio and starts stream. */
int StartPortAudio(Engine* engine, const char* input_device,
                   const char* input_wav, const char* output_device) {
  /* Initialize PortAudio. */
  engine->pa_error = Pa_Initialize();
  if (engine->pa_error != paNoError) { return 0; }
  engine->pa_initialized = 1;


  /* Find PortAudio devices. */
  const int output_channels = engine->channel_map.num_output_channels;
  const int input_device_index =
      (input_wav) ? 0 : FindPortAudioDevice(input_device, 1, 0);
  const int output_device_index =
      FindPortAudioDevice(output_device, 0, output_channels);
  if (input_device_index < 0 || output_device_index < 0) {
    fprintf(stderr, "\nError: "
        "Use --input and --output flags to set valid devices:\n");
    PrintPortAudioDevices();
    return 0;
  }

  /* Display audio stream configuration. */
  printf("sample rate: %g Hz\n"
         "chunk size: %d frames (%.1f ms)\n",
        engine->sample_rate_hz,
        engine->chunk_size,
        (1000.0f * engine->chunk_size) / engine->sample_rate_hz);

  if (input_wav) {
    printf("Input WAV: %s\n", input_wav);
  } else {
    printf("Input device: #%d %s\n",
           input_device_index, Pa_GetDeviceInfo(input_device_index)->name);
  }
  printf("Output device: #%d %s\n",
         output_device_index, Pa_GetDeviceInfo(output_device_index)->name);
  printf("Output channels:\n");
  ChannelMapPrint(&engine->channel_map);

  /* Open and start PortAudio stream. */
  PaStreamParameters input_parameters;
  if (!input_wav) {
    input_parameters.device = input_device_index;
    input_parameters.channelCount = 1;
    input_parameters.sampleFormat = paFloat32;
    input_parameters.suggestedLatency =
        Pa_GetDeviceInfo(input_parameters.device)->defaultLowInputLatency;
    input_parameters.hostApiSpecificStreamInfo = NULL;
  }

  PaStreamParameters output_parameters;
  output_parameters.device = output_device_index;
  output_parameters.channelCount = output_channels;
  output_parameters.sampleFormat = paFloat32;
  output_parameters.suggestedLatency =
      Pa_GetDeviceInfo(output_parameters.device)->defaultLowOutputLatency;
  output_parameters.hostApiSpecificStreamInfo = NULL;

  engine->pa_error = Pa_OpenStream(
    &engine->pa_stream,
    (input_wav) ? NULL : &input_parameters,
    &output_parameters,
    engine->sample_rate_hz, engine->chunk_size, 0, PortAudioCallback, engine);
  if (engine->pa_error != paNoError) { return 0; }

  engine->pa_error = Pa_StartStream(engine->pa_stream);
  if (engine->pa_error != paNoError) { return 0; }

  return 1;
}

/* Starts up SDL, PortAudio, and tactile processing. */
int EngineInit(Engine* engine, int argc, char** argv) {
  int i;

  BasicSdlAppInit(&engine->app);
  for (i = 0; i < kNumFormFactors; ++i) {
    int j;
    for (j = 0; j < kNumImageAssets; ++j) {
      engine->form_factors[i].images[j] = NULL;
    }
  }
  engine->pa_initialized = 0;
  engine->pa_error = paNoError;
  engine->pa_stream = NULL;
  engine->input_wav_samples = NULL;
  engine->tactile_processor = NULL;
  engine->tactile_output = NULL;
  engine->selected_form_factor = 0;
  engine->keep_running = 1;

  TactileProcessorParams params;
  TactileProcessorSetDefaultParams(&params);
  PostProcessorParams post_processor_params;
  PostProcessorSetDefaultParams(&post_processor_params);
  const char* input_device = NULL;
  const char* input_wav = NULL;
  const char* output_device = NULL;
  const char* source_list = NULL;
  const char* gains_db_list = NULL;
  int sample_rate_hz = 16000;
  int chunk_size = 256;
  int block_size = params.frontend_params.block_size;
  float cutoff_hz = 500.0f;
  int window_fullscreen = 0;
  int window_borderless = 0;
  int window_on_top = 0;

  for (i = 1; i < argc; ++i) {  /* Parse flags. */
    if (StartsWith(argv[i], "--input=")) {
      const char* value = strchr(argv[i], '=') + 1;
      if (EndsWith(value, ".wav") || EndsWith(value, ".WAV")) {
        input_wav = value;
      } else {
        input_device = value;
      }
    } else if (StartsWith(argv[i], "--output=")) {
      output_device = strchr(argv[i], '=') + 1;
    } else if (StartsWith(argv[i], "--sample_rate_hz=")) {
      sample_rate_hz = atoi(strchr(argv[i], '=') + 1);
    } else if (StartsWith(argv[i], "--channels=")) {
      source_list = strchr(argv[i], '=') + 1;
    } else if (StartsWith(argv[i], "--channel_gains_db=")) {
      gains_db_list = strchr(argv[i], '=') + 1;
    } else if (StartsWith(argv[i], "--gain_db=")) {
      post_processor_params.gain =
          AmplitudeRatioToDecibels(atof(strchr(argv[i], '=') + 1));
    } else if (StartsWith(argv[i], "--mid_gain_db=")) {
      post_processor_params.mid_gain =
          AmplitudeRatioToDecibels(atof(strchr(argv[i], '=') + 1));
    } else if (StartsWith(argv[i], "--high_gain_db=")) {
      post_processor_params.high_gain =
          AmplitudeRatioToDecibels(atof(strchr(argv[i], '=') + 1));
    } else if (StartsWith(argv[i], "--block_size=")) {
      block_size = atoi(strchr(argv[i], '=') + 1);
    } else if (StartsWith(argv[i], "--chunk_size=")) {
      chunk_size = atoi(strchr(argv[i], '=') + 1);
    } else if (StartsWith(argv[i], "--cutoff_hz=")) {
      cutoff_hz = atof(strchr(argv[i], '=') + 1);
    } else if (!strcmp(argv[i], "--fullscreen")) {
      window_fullscreen = 1;
    } else if (!strcmp(argv[i], "--borderless")) {
      window_borderless = 1;
    } else if (!strcmp(argv[i], "--on_top")) {
      window_on_top = 1;
    } else {
      fprintf(stderr, "Error: Invalid flag \"%s\"\n", argv[i]);
      return 0;
    }
  }

  if (!source_list) {
    fprintf(stderr, "Error: Must specify --channels.\n");
    return 0;
  } else if (!ChannelMapParse(kNumTactors, source_list, gains_db_list,
        &engine->channel_map)) {
    return 0;
  }

  if (chunk_size % block_size != 0) {
    chunk_size = RoundUpToMultiple(chunk_size, block_size);
    printf("chunk_size rounded up to %d frames.\n", chunk_size);
  }
  engine->chunk_size = chunk_size;

  /* Initialize SDL. */
  if (!StartSdl(engine, window_fullscreen, window_borderless, window_on_top)) {
    return 0;
  }

  /* Read input WAV if specified. */
  if (input_wav) {
    if (!ReadInputWav(engine, input_wav)) {
      return 0;
    }
  } else {
    engine->sample_rate_hz = sample_rate_hz;
  }

  /* Create tactile processor.
   * NOTE: This must be done before starting the audio thread.
   */
  params.frontend_params.input_sample_rate_hz = engine->sample_rate_hz;
  params.frontend_params.block_size = block_size;
  params.enveloper_params.energy_cutoff_hz = cutoff_hz;

  engine->tactile_processor = TactileProcessorMake(&params);
  if (engine->tactile_processor == NULL) {
    fprintf(stderr, "Error: TactileProcessorInit failed.\n");
    return 0;
  }

  /* Create PostProcessor. */
  if (!PostProcessorInit(&engine->post_processor, &post_processor_params,
                         TactileProcessorOutputSampleRateHz(&params),
                         kTactileProcessorNumTactors)) {
    return 0;
  }

  engine->tactile_output = malloc(sizeof(float) * kNumTactors * block_size);
  if (engine->tactile_output == NULL) {
    fprintf(stderr, "Error: Memory allocation failed.\n");
    return 0;
  }

  int c;
  for (c = 0; c < kNumTactors; ++c) {
    engine->volume[c] = 0.0f;
  }
  const float kVolumeMeterTimeConstantSeconds = 0.05;
  engine->volume_decay_coeff = (float)exp(
      -chunk_size / (kVolumeMeterTimeConstantSeconds * sample_rate_hz));

  /* Start PortAudio and audio thread. */
  if (!StartPortAudio(engine, input_device, input_wav, output_device)) {
    return 0;
  }

  SDL_ShowWindow(engine->app.window);
  return 1;
}

/* Cleans everything up. */
void EngineTerminate(Engine* engine) {
  if (engine->pa_stream) {
    engine->pa_error = Pa_CloseStream(engine->pa_stream);
  }

  if (engine->pa_error != paNoError) {
    fprintf(stderr, "Error: PortAudio: %s\n", Pa_GetErrorText(engine->pa_error));
  }
  if (engine->pa_initialized) {
    Pa_Terminate();
  }

  free(engine->tactile_output);
  TactileProcessorFree(engine->tactile_processor);
  free(engine->input_wav_samples);

  int i;
  for (i = 0; i < kNumFormFactors; ++i) {
    int j;
    for (j = 0; j < kNumImageAssets; ++j) {
      if (engine->form_factors[i].images[j]) {
        SDL_DestroyTexture(engine->form_factors[i].images[j]);
      }
    }
  }

  BasicSdlAppDestroy(&engine->app);
  SDL_Quit();
}

/* Generates an approximately perceptually linear colormap fading from a dark
 * gray to orange to white. `colormap` should have space for 256 * 3 elements.
 */
void GenerateColormap(uint8_t* colormap) {
  int i;
  for (i = 0; i < 256; ++i, colormap += 3) {
    const float x = i / 255.0f;
    if (x < 0.5f) {
      colormap[0] = (uint8_t)((180 * x + 261) * x + 81);
      colormap[1] = (uint8_t)((48 * x + 69) * x + 67);
      colormap[2] = (uint8_t)((-106 * x - 21) * x + 49);
    } else {
      colormap[0] = 255;
      colormap[1] = (uint8_t)((-234 * x + 626) * x - 139);
      colormap[2] = (uint8_t)((-433 * x + 1112) * x - 428);
    }
  }
}

int main(int argc, char** argv) {
  int exit_status = EXIT_FAILURE;
  Engine engine;
  if (!EngineInit(&engine, argc, argv)) { goto done; }

  uint8_t colormap[256 * 3];
  GenerateColormap(colormap);
  SDL_SetRenderDrawColor(engine.app.renderer, 0x0, 0x0, 0x0, 0xff);

  while (engine.keep_running) {  /* Engine main loop. */
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT ||
          (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_ESCAPE ||
           event.key.keysym.sym == SDLK_q))) {
        engine.keep_running = 0;  /* Quit program. */
        break;
      } else if (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_TAB) {
        engine.selected_form_factor ^= 1;
      }
    }

    const FormFactorAssets* assets =
        &engine.form_factors[engine.selected_form_factor];
    SDL_RenderClear(engine.app.renderer);
    /* Render background texture. */
    SDL_RenderCopy(engine.app.renderer, assets->images[kNumTactors],
                   NULL, &assets->image_rects[kNumTactors]);

    int c;
    for (c = 0; c < kNumTactors; ++c) {
      /* Get volume for the cth tactor. */
      float activation = engine.volume[c] / 0.4f;
      if (activation < 0.0f) { activation = 0.0f; }
      if (activation > 1.0f) { activation = 1.0f; }

      /* Render the cth texture with color according to `activation`. */
      const int index = (int)(255 * activation + 0.5f);
      const uint8_t* rgb = &colormap[3 * index];
      SDL_SetTextureColorMod(assets->images[c], rgb[0], rgb[1], rgb[2]);
      SDL_RenderCopy(engine.app.renderer, assets->images[c],
                     NULL, &assets->image_rects[c]);
    }

    SDL_RenderPresent(engine.app.renderer);
    Pa_Sleep(25);
  }

  printf("\nFinished.\n");
  exit_status = EXIT_SUCCESS;
done:
  EngineTerminate(&engine);
  return exit_status;
}
