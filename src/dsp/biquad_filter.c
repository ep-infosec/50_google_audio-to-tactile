/* Copyright 2019 Google LLC
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
 */

#include "dsp/biquad_filter.h"

#include "dsp/math_constants.h"

const BiquadFilterCoeffs kBiquadFilterIdentityCoeffs =
    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

ComplexDouble BiquadFilterFrequencyResponse(const BiquadFilterCoeffs* coeffs,
                                            double cycles_per_sample) {
  const double omega = -2 * M_PI * cycles_per_sample;
  const double c = cos(omega);
  const double s = sin(omega);
  const double c2 = 2 * c;
  return ComplexDoubleDiv(
      ComplexDoubleMake(
          coeffs->b0 + c * (coeffs->b1 + c2 * coeffs->b2) - coeffs->b2,
          s * (coeffs->b1 + c2 * coeffs->b2)),
      ComplexDoubleMake(1.0 + c * (coeffs->a1 + c2 * coeffs->a2) - coeffs->a2,
                        s * (coeffs->a1 + c2 * coeffs->a2)));
}
