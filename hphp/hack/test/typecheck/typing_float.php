<?hh // strict
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 */

function f1(float $x, float $y): float {
  return $x / $y;
}

function f2(float $x, int $y): float {
  return $x / $y;
}

function f3(int $x, float $y): float {
  return $x / $y;
}
