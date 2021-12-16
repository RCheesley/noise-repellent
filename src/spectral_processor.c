/*
noise-repellent -- Noise Reduction LV2

Copyright 2016 Luciano Dato <lucianodato@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/
*/

#include "spectral_processor.h"
#include "gain_estimator.h"
#include "noise_estimator.h"
#include "spectral_utils.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WHITENING_DECAY_RATE 1000.f
#define WHITENING_FLOOR 0.02f

static bool is_empty(const float *spectrum, uint32_t half_fft_size);
static void fft_denoiser_update_wetdry_target(SpectralProcessor *self,
                                              bool enable);
static void fft_denoiser_soft_bypass(SpectralProcessor *self);
static void get_denoised_spectrum(SpectralProcessor *self);
static void get_residual_spectrum(SpectralProcessor *self,
                                  float whitening_factor);

static void get_final_spectrum(SpectralProcessor *self, bool residual_listen,
                               float reduction_amount);

typedef struct {
  float tau;
  float wet_dry_target;
  float wet_dry;
} SoftBypass;

typedef struct {
  float *residual_max_spectrum;
  float *whitened_residual_spectrum;
  float max_decay_rate;
  uint32_t whitening_window_count;
} Whitening;

typedef struct {
  float *gain_spectrum;
  float *residual_spectrum;
  float *denoised_spectrum;
} SpectralDenoiseBuilder;

typedef struct {
  float *power_spectrum;
  float *phase_spectrum;
  float *magnitude_spectrum;
} ProcessingSpectrums;

struct SpectralProcessor {
  uint32_t fft_size;
  uint32_t half_fft_size;
  uint32_t sample_rate;
  uint32_t hop;

  float *fft_spectrum;
  float *processed_fft_spectrum;

  SoftBypass crossfade_spectrum;
  Whitening whiten_spectrum;
  SpectralDenoiseBuilder denoise_builder;
  ProcessingSpectrums processing_spectrums;

  GainEstimator *gain_estimation;
  NoiseEstimator *noise_estimation;
  NoiseProfile *noise_profile;
  ProcessorParameters *denoise_parameters;
};

SpectralProcessor *
spectral_processor_initialize(const uint32_t sample_rate,
                              const uint32_t fft_size,
                              const uint32_t overlap_factor) {
  SpectralProcessor *self =
      (SpectralProcessor *)calloc(1, sizeof(SpectralProcessor));

  self->fft_size = fft_size;
  self->half_fft_size = self->fft_size / 2;
  self->hop = self->fft_size / overlap_factor;
  self->sample_rate = sample_rate;

  self->fft_spectrum = (float *)calloc((self->fft_size), sizeof(float));
  self->processed_fft_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));

  self->gain_estimation =
      gain_estimation_initialize(self->fft_size, self->sample_rate, self->hop);
  self->noise_estimation = noise_estimation_initialize(self->fft_size);

  self->processing_spectrums.power_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));

  self->denoise_builder.residual_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->denoise_builder.denoised_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->denoise_builder.gain_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));

  self->crossfade_spectrum.tau =
      (1.f - expf(-2.f * M_PI * 25.f * 64.f / self->sample_rate));
  self->crossfade_spectrum.wet_dry = 0.f;

  self->whiten_spectrum.whitened_residual_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->whiten_spectrum.residual_max_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->whiten_spectrum.max_decay_rate =
      expf(-1000.f / (((WHITENING_DECAY_RATE)*self->sample_rate) / self->hop));
  self->whiten_spectrum.whitening_window_count = 0.f;

  return self;
}

void spectral_processor_free(SpectralProcessor *self) {
  gain_estimation_free(self->gain_estimation);
  noise_estimation_free(self->noise_estimation);

  free(self->fft_spectrum);
  free(self->processed_fft_spectrum);
  free(self->processing_spectrums.power_spectrum);
  free(self->denoise_builder.gain_spectrum);
  free(self->denoise_builder.residual_spectrum);
  free(self->denoise_builder.denoised_spectrum);
  free(self->whiten_spectrum.whitened_residual_spectrum);
  free(self->whiten_spectrum.residual_max_spectrum);
  free(self);
}

void load_processor_parameters(SpectralProcessor *self,
                               ProcessorParameters *new_parameters) {
  self->denoise_parameters = new_parameters;
}

void load_noise_profile(SpectralProcessor *self, NoiseProfile *noise_profile) {
  self->noise_profile = noise_profile;
}

void spectral_processor_run(SPECTAL_PROCESSOR instance, float *fft_spectrum) {
  SpectralProcessor *self = (SpectralProcessor *)instance;
  const bool enable = self->denoise_parameters->enable;
  const bool learn_noise = self->denoise_parameters->learn_noise;
  const bool residual_listen = self->denoise_parameters->residual_listen;
  const float transient_protection =
      self->denoise_parameters->transient_threshold;
  const float masking = self->denoise_parameters->masking_ceiling_limit;
  const float release = self->denoise_parameters->release_time;
  const float noise_rescale = self->denoise_parameters->noise_rescale;
  const float reduction_amount = self->denoise_parameters->reduction_amount;
  const float whitening_factor = self->denoise_parameters->whitening_factor;
  float *noise_spectrum = self->noise_profile->noise_profile;

  fft_denoiser_update_wetdry_target(self, enable);

  memcpy(self->fft_spectrum, fft_spectrum, sizeof(float) * self->fft_size);

  get_fft_power_spectrum(self->fft_spectrum, self->fft_size,
                         self->processing_spectrums.power_spectrum,
                         self->half_fft_size);

  if (!is_empty(self->processing_spectrums.power_spectrum,
                self->half_fft_size)) {
    if (learn_noise) {
      noise_estimation_run(self->noise_estimation, noise_spectrum,
                           self->processing_spectrums.power_spectrum);
    } else {
      if (is_noise_estimation_available(self->noise_estimation)) {
        gain_estimation_run(
            self->gain_estimation, self->processing_spectrums.power_spectrum,
            noise_spectrum, self->denoise_builder.gain_spectrum,
            transient_protection, masking, release, noise_rescale);

        get_denoised_spectrum(self);

        get_residual_spectrum(self, whitening_factor);

        get_final_spectrum(self, residual_listen, reduction_amount);
      }
    }
  }

  fft_denoiser_soft_bypass(self);

  memcpy(fft_spectrum, self->processed_fft_spectrum,
         sizeof(float) * self->half_fft_size + 1);
}

static void residual_spectrum_whitening(SpectralProcessor *self,
                                        const float whitening_factor) {
  self->whiten_spectrum.whitening_window_count++;

  for (uint32_t k = 1; k <= self->half_fft_size; k++) {
    if (self->whiten_spectrum.whitening_window_count > 1.f) {
      self->whiten_spectrum.residual_max_spectrum[k] = fmaxf(
          fmaxf(self->denoise_builder.residual_spectrum[k], WHITENING_FLOOR),
          self->whiten_spectrum.residual_max_spectrum[k] *
              self->whiten_spectrum.max_decay_rate);
    } else {
      self->whiten_spectrum.residual_max_spectrum[k] =
          fmaxf(self->denoise_builder.residual_spectrum[k], WHITENING_FLOOR);
    }
  }

  for (uint32_t k = 1; k <= self->half_fft_size; k++) {
    if (self->denoise_builder.residual_spectrum[k] > FLT_MIN) {
      self->whiten_spectrum.whitened_residual_spectrum[k] =
          self->denoise_builder.residual_spectrum[k] /
          self->whiten_spectrum.residual_max_spectrum[k];

      self->denoise_builder.residual_spectrum[k] =
          (1.f - whitening_factor) *
              self->denoise_builder.residual_spectrum[k] +
          whitening_factor *
              self->whiten_spectrum.whitened_residual_spectrum[k];
    }
  }
}

static bool is_empty(const float *spectrum, const uint32_t half_fft_size) {
  for (uint32_t k = 1; k <= half_fft_size; k++) {
    if (spectrum[k] > FLT_MIN) {
      return false;
    }
  }
  return true;
}

static void fft_denoiser_update_wetdry_target(SpectralProcessor *self,
                                              const bool enable) {
  if (enable) {
    self->crossfade_spectrum.wet_dry_target = 1.f;
  } else {
    self->crossfade_spectrum.wet_dry_target = 0.f;
  }

  self->crossfade_spectrum.wet_dry +=
      self->crossfade_spectrum.tau * (self->crossfade_spectrum.wet_dry_target -
                                      self->crossfade_spectrum.wet_dry) +
      FLT_MIN;
}

static void fft_denoiser_soft_bypass(SpectralProcessor *self) {
  for (uint32_t k = 1; k <= self->half_fft_size; k++) {
    self->processed_fft_spectrum[k] =
        (1.f - self->crossfade_spectrum.wet_dry) * self->fft_spectrum[k] +
        self->processed_fft_spectrum[k] * self->crossfade_spectrum.wet_dry;
  }
}

static void get_denoised_spectrum(SpectralProcessor *self) {
  for (uint32_t k = 1; k <= self->half_fft_size; k++) {
    self->denoise_builder.denoised_spectrum[k] =
        self->fft_spectrum[k] * self->denoise_builder.gain_spectrum[k];
  }
}

static void get_residual_spectrum(SpectralProcessor *self,
                                  const float whitening_factor) {
  for (uint32_t k = 1; k <= self->half_fft_size; k++) {
    self->denoise_builder.residual_spectrum[k] =
        self->fft_spectrum[k] - self->denoise_builder.denoised_spectrum[k];
  }

  if (whitening_factor > 0.f) {
    residual_spectrum_whitening(self, whitening_factor);
  }
}

static void get_final_spectrum(SpectralProcessor *self,
                               const bool residual_listen,
                               const float reduction_amount) {
  if (residual_listen) {
    for (uint32_t k = 1; k <= self->half_fft_size; k++) {
      self->processed_fft_spectrum[k] =
          self->denoise_builder.residual_spectrum[k];
    }
  } else {
    for (uint32_t k = 1; k <= self->half_fft_size; k++) {
      self->processed_fft_spectrum[k] =
          self->denoise_builder.denoised_spectrum[k] +
          self->denoise_builder.residual_spectrum[k] * reduction_amount;
    }
  }
}
