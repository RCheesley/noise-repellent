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

#include "masking_estimator.h"
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N_BARK_BANDS 25
#define AT_SINE_WAVE_FREQ 1000.f
#define REFERENCE_LEVEL 90.f

#define BIAS 1
#define HIGH_FREQ_BIAS 20.f
#define S_AMP 1.f

static const float relative_thresholds[N_BARK_BANDS] = {-16.f, -17.f, -18.f, -19.f, -20.f, -21.f, -22.f, -23.f, -24.f, -25.f, -25.f, -25.f, -25.f, -25.f, -25.f, -24.f, -23.f, -22.f, -19.f, -18.f, -18.f, -18.f, -18.f, -18.f, -18.f};

struct MaskingEstimator
{

	int fft_size;
	int half_fft_size;
	int samp_rate;

	float *bark_z;
	float *absolute_thresholds;
	float *spl_reference_values;
	float *input_fft_buffer_at;
	float *output_fft_buffer_at;
	float *spectral_spreading_function;
	float *unity_gain_bark_spectrum;
	float *spreaded_unity_gain_bark_spectrum;
	fftwf_plan forward_fft;
};

float bin_to_freq(int i, float samp_rate, int N)
{
	return (float)i * (samp_rate / N / 2.f);
}

void compute_bark_mapping(MaskingEstimator *self)
{
	int k;
	float freq;

	for (k = 0; k <= self->half_fft_size; k++)
	{
		freq = (float)self->samp_rate / (2.f * (float)(self->half_fft_size) * (float)k);
		self->bark_z[k] = 1.f + 13.f * atanf(0.00076f * freq) + 3.5f * atanf(powf(freq / 7500.f, 2.f));
	}
}

void compute_absolute_thresholds(MaskingEstimator *self)
{
	int k;
	float freq;

	for (k = 1; k <= self->half_fft_size; k++)
	{

		freq = bin_to_freq(k, self->samp_rate, self->half_fft_size);
		self->absolute_thresholds[k] = 3.64f * powf((freq / 1000.f), -0.8f) - 6.5f * exp(-0.6f * powf((freq / 1000.f - 3.3f), 2.f)) + powf(10.f, -3.f) * powf((freq / 1000.f), 4.f);
	}
}

void hanning_window(float *window, int N)
{
	int k;
	for (k = 0; k < N; k++)
	{
		float p = ((float)(k)) / ((float)(N));
		window[k] = 0.5 - 0.5 * cosf(2.f * M_PI * p);
	}
}

void get_power_spectrum(MaskingEstimator *self, float *window, float *signal, float *power_spectrum)
{
	int k;
	float real_p, imag_n, p2;

	hanning_window(window, self->fft_size);
	for (k = 0; k < self->fft_size; k++)
	{
		self->input_fft_buffer_at[k] = signal[k] * window[k];
	}

	fftwf_execute(self->forward_fft);

	real_p = self->output_fft_buffer_at[0];
	imag_n = 0.f;

	power_spectrum[0] = real_p * real_p;

	for (k = 1; k <= self->half_fft_size; k++)
	{

		real_p = self->output_fft_buffer_at[k];
		imag_n = self->output_fft_buffer_at[self->fft_size - k];

		if (k < self->half_fft_size)
		{
			p2 = (real_p * real_p + imag_n * imag_n);
		}
		else
		{

			p2 = real_p * real_p;
		}

		power_spectrum[k] = p2;
	}
}

void spl_reference(MaskingEstimator *self)
{
	int k;
	float sinewave[self->fft_size];
	float window[self->fft_size];
	float fft_p2_at[self->half_fft_size];
	float fft_p2_at_dbspl[self->half_fft_size];

	for (k = 0; k < self->fft_size; k++)
	{
		sinewave[k] = S_AMP * sinf((2.f * M_PI * k * AT_SINE_WAVE_FREQ) / (float)self->samp_rate);
	}

	get_power_spectrum(self, window, sinewave, fft_p2_at);

	for (k = 0; k <= self->half_fft_size; k++)
	{
		fft_p2_at_dbspl[k] = REFERENCE_LEVEL - 10.f * log10f(fft_p2_at[k]);
	}

	memcpy(self->spl_reference_values, fft_p2_at_dbspl, sizeof(float) * (self->half_fft_size + 1));
}

void compute_spectral_spreading_function(MaskingEstimator *self)
{
	int i, j;
	float y;
	for (i = 0; i < N_BARK_BANDS; i++)
	{
		for (j = 0; j < N_BARK_BANDS; j++)
		{
			y = (i + 1) - (j + 1);

			self->spectral_spreading_function[i * N_BARK_BANDS + j] = 15.81f + 7.5f * (y + 0.474f) - 17.5f * sqrtf(1.f + (y + 0.474f) * (y + 0.474f));

			self->spectral_spreading_function[i * N_BARK_BANDS + j] = powf(10.f, self->spectral_spreading_function[i * N_BARK_BANDS + j] / 10.f);
		}
	}
}

void convolve_with_spectral_spreading_function(MaskingEstimator *self, float *bark_spectrum, float *spreaded_spectrum)
{
	int i, j;
	for (i = 0; i < N_BARK_BANDS; i++)
	{
		spreaded_spectrum[i] = 0.f;
		for (j = 0; j < N_BARK_BANDS; j++)
		{
			spreaded_spectrum[i] += self->spectral_spreading_function[i * N_BARK_BANDS + j] * bark_spectrum[j];
		}
	}
}

void compute_bark_spectrum(MaskingEstimator *self, float *bark_spectrum, float *spectrum,
						   float *intermediate_band_bins, float *n_bins_per_band)
{
	int j;
	int last_position = 0;

	for (j = 0; j < N_BARK_BANDS; j++)
	{
		int cont = 0;
		if (j == 0)
			cont = 1;

		bark_spectrum[j] = 0.f;

		while (floor(self->bark_z[last_position + cont]) == (j + 1))
		{
			bark_spectrum[j] += spectrum[last_position + cont];
			cont++;
		}

		last_position += cont;

		n_bins_per_band[j] = cont;
		intermediate_band_bins[j] = last_position;
	}
}

void convert_to_dbspl(MaskingEstimator *self, float *masking_thresholds)
{
	for (int k = 0; k <= self->half_fft_size; k++)
	{
		masking_thresholds[k] += self->spl_reference_values[k];
	}
}

float compute_tonality_factor(float *spectrum, float *intermediate_band_bins,
							  float *n_bins_per_band, int band)
{
	int k;
	float SFM, tonality_factor;
	float sum_p = 0.f, sum_log_p = 0.f;
	int start_pos, end_pos = 0;

	if (band == 0)
	{
		start_pos = band;
		end_pos = n_bins_per_band[band];
	}
	else
	{
		start_pos = intermediate_band_bins[band - 1];
		end_pos = intermediate_band_bins[band - 1] + n_bins_per_band[band];
	}

	for (k = start_pos; k < end_pos; k++)
	{

		sum_p += spectrum[k];
		sum_log_p += log10f(spectrum[k]);
	}

	SFM = 10.f * (sum_log_p / (float)(n_bins_per_band[band]) - log10f(sum_p / (float)(n_bins_per_band[band])));

	tonality_factor = fminf(SFM / -60.f, 1.f);

	return tonality_factor;
}

void compute_masking_thresholds(MaskingEstimator *self, float *spectrum, float *masking_thresholds)
{
	int k, j, start_pos, end_pos;
	float intermediate_band_bins[N_BARK_BANDS];
	float n_bins_per_band[N_BARK_BANDS];
	float bark_spectrum[N_BARK_BANDS];
	float threshold_j[N_BARK_BANDS];
	float masking_offset[N_BARK_BANDS];
	float spreaded_spectrum[N_BARK_BANDS];
	float tonality_factor;

	compute_bark_spectrum(self, bark_spectrum, spectrum, intermediate_band_bins,
						  n_bins_per_band);

	convolve_with_spectral_spreading_function(self, bark_spectrum, spreaded_spectrum);

	for (j = 0; j < N_BARK_BANDS; j++)
	{

		tonality_factor = compute_tonality_factor(spectrum, intermediate_band_bins, n_bins_per_band, j);

		masking_offset[j] = (tonality_factor * (14.5f + (float)(j + 1)) + 5.5f * (1.f - tonality_factor));

#if BIAS

		masking_offset[j] = relative_thresholds[j];

		if (j > 15)
			masking_offset[j] += HIGH_FREQ_BIAS;
#endif

		threshold_j[j] = powf(10.f, log10f(spreaded_spectrum[j]) - (masking_offset[j] / 10.f));

		threshold_j[j] -= 10.f * log10f(self->spreaded_unity_gain_bark_spectrum[j]);

		if (j == 0)
		{
			start_pos = 0;
		}
		else
		{
			start_pos = intermediate_band_bins[j - 1];
		}
		end_pos = intermediate_band_bins[j];

		for (k = start_pos; k < end_pos; k++)
		{
			masking_thresholds[k] = threshold_j[j];
		}
	}

	convert_to_dbspl(self, masking_thresholds);

	for (k = 0; k <= self->half_fft_size; k++)
	{
		masking_thresholds[k] = fmaxf(masking_thresholds[k], self->absolute_thresholds[k]);
	}
}

void masking_estimation_reset(MaskingEstimator *self)
{

	memset(self->absolute_thresholds, 0.f, self->half_fft_size + 1);
	memset(self->bark_z, 0.f, self->half_fft_size + 1);
	memset(self->spl_reference_values, 0.f, self->half_fft_size + 1);
	memset(self->input_fft_buffer_at, 0.f, self->half_fft_size + 1);
	memset(self->output_fft_buffer_at, 0.f, self->half_fft_size + 1);
	memset(self->spectral_spreading_function, 0.f, N_BARK_BANDS);
	memset(self->unity_gain_bark_spectrum, 1.f, N_BARK_BANDS);
	memset(self->spreaded_unity_gain_bark_spectrum, 0.f, N_BARK_BANDS);
}

MaskingEstimator *masking_estimation_initialize(int fft_size, int samp_rate)
{

	MaskingEstimator *self = (MaskingEstimator *)malloc(sizeof(MaskingEstimator));

	self->fft_size = fft_size;
	self->half_fft_size = self->fft_size / 2;
	self->samp_rate = samp_rate;

	self->absolute_thresholds = (float *)calloc((self->half_fft_size + 1), sizeof(float));
	self->bark_z = (float *)calloc((self->half_fft_size + 1), sizeof(float));
	self->spl_reference_values = (float *)calloc((self->half_fft_size + 1), sizeof(float));
	self->input_fft_buffer_at = (float *)calloc((self->half_fft_size + 1), sizeof(float));
	self->output_fft_buffer_at = (float *)calloc((self->half_fft_size + 1), sizeof(float));
	self->spectral_spreading_function = (float *)calloc(N_BARK_BANDS, sizeof(float));
	self->unity_gain_bark_spectrum = (float *)calloc(N_BARK_BANDS, sizeof(float));
	self->spreaded_unity_gain_bark_spectrum = (float *)calloc(N_BARK_BANDS, sizeof(float));

	masking_estimation_reset(self);

	self->forward_fft = fftwf_plan_r2r_1d(self->fft_size, self->input_fft_buffer_at, self->output_fft_buffer_at, FFTW_R2HC, FFTW_ESTIMATE);

	compute_bark_mapping(self);
	compute_absolute_thresholds(self);
	spl_reference(self);
	compute_spectral_spreading_function(self);

	convolve_with_spectral_spreading_function(self, self->unity_gain_bark_spectrum,
											  self->spreaded_unity_gain_bark_spectrum);

	return self;
}

void masking_estimation_free(MaskingEstimator *self)
{
	free(self->absolute_thresholds);
	free(self->bark_z);
	free(self->spl_reference_values);
	free(self->input_fft_buffer_at);
	free(self->output_fft_buffer_at);
	free(self->spectral_spreading_function);
	free(self->unity_gain_bark_spectrum);
	free(self->spreaded_unity_gain_bark_spectrum);
	fftwf_free(self->input_fft_buffer_at);
	fftwf_free(self->output_fft_buffer_at);
	fftwf_destroy_plan(self->forward_fft);
	free(self);
}
