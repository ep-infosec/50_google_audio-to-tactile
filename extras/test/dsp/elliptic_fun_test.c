/* Copyright 2019, 2021 Google LLC
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

#include "src/dsp/elliptic_fun.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/dsp/logging.h"
#include "src/dsp/math_constants.h"

static double RandUnif(void) { return (double)rand() / RAND_MAX; }

static int ComplexIsApproximatelyEqual(ComplexDouble actual,
                                       ComplexDouble expected) {
  const double kTol = 1e-12;
  if (!(ComplexDoubleAbs(ComplexDoubleSub(actual, expected)) < kTol)) {
    fprintf(stderr, "Error: Complex values differ by more than %g.\n", kTol);
    fprintf(stderr, "actual = %.13f + %.13fi\n", actual.real, actual.imag);
    fprintf(stderr, "expected = %.13f + %.13fi\n",
            expected.real, expected.imag);
    return 0;
  }
  return 1;
}

/* Compare EllipticK to known values. */
static void TestEllipticK(void) {
  puts("TestEllipticK");
  CHECK(fabs(EllipticK(0.0) - M_PI / 2) < 1e-15);

  /* Compare with values verified with scipy and WolframAlpha (e.g. for m = 0.1,
   * scipy.special.ellipk(0.1) and EllipticK[0.1]). Accuracy degrades as m
   * approaches 1, where the function is singular.
   */
  CHECK(fabs(EllipticK(0.1) - 1.6124413487202194) < 1e-12);
  CHECK(fabs(EllipticK(0.5) - 1.8540746773013719) < 1e-12);
  CHECK(fabs(EllipticK(0.99) - 3.6956373629898747) < 1e-12);
  CHECK(fabs(EllipticK(1.0 - 1e-10) - 12.89921983) < 1e-7);
  CHECK(fabs(EllipticK(1.0 - 1e-14) - 17.50439) < 1e-3);
}

/* Compare EllipticF to known values.
 * Compare with values verified with scipy and WolframAlpha (e.g for
 * F(0.6|0.1), scipy.special.ellipkinc(0.6, 0.1) and EllipticF[0.6, 0.1]; note
 * that scipy's implementation doesn't support complex phi).
 */
static void TestEllipticF(void) {
  puts("TestEllipticF");
  /* Test that F(0|m) = 0. */
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(0.0, 0.0), 0.7),
      ComplexDoubleMake(0.0, 0.0)));
  /* Test a small value where EllipticF uses a Taylor approximation. */
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(5e-4, 1e-4), 0.7),
      ComplexDoubleMake(0.0005000000128333, 0.0001000000086333)));
  /* Values on the real line. */
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(0.6, 0.0), 0.1),
      ComplexDoubleMake(0.6033995956681880, 0.0)));
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(0.8, 0.0), 0.7),
      ComplexDoubleMake(0.8640250261841760, 0.0)));
  /* Complex values. */
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(0.0, 1.8317348832729556), 0.00027225),
      ComplexDoubleMake(0.0, 1.8311969870256281)));
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(0.8, 0.3), 0.7),
      ComplexDoubleMake(0.8333618443650643, 0.3700664506506054)));
  /* Should have F(-z|m) = -F(z|m) and F(z*|m) = F(z|m)*. */
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(0.8, -0.3), 0.7),
      ComplexDoubleMake(0.8333618443650643, -0.3700664506506054)));
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(-0.8, 0.3), 0.7),
      ComplexDoubleMake(-0.8333618443650643, 0.3700664506506054)));
  CHECK(ComplexIsApproximatelyEqual(
      EllipticF(ComplexDoubleMake(-0.8, -0.3), 0.7),
      ComplexDoubleMake(-0.8333618443650643, -0.3700664506506054)));
}

/* JacobiAmplitude should be the inverse of EllipticF. */
static void TestJacobiAmplitude(void) {
  puts("TestJacobiAmplitude");
  int trial;
  for (trial = 0; trial < 100; ++trial) {
    const ComplexDouble phi = ComplexDoubleMake(M_PI * (RandUnif() - 0.5),
                                                8.0 * (RandUnif() - 0.5));
    const double m = 0.9999 * RandUnif();

    const ComplexDouble u = EllipticF(phi, m);
    const ComplexDouble phi_recovered = JacobiAmplitude(u, m);

    CHECK(ComplexIsApproximatelyEqual(phi_recovered, phi));
  }
}

int main(int argc, char** argv) {
  srand(0);
  TestEllipticK();
  TestEllipticF();
  TestJacobiAmplitude();

  puts("PASS");
  return EXIT_SUCCESS;
}
