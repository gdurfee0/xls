// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// DSLX implementation of a 4x4 systolic array, appropriate for part of a
// matrix multiplier.

// TODO(rspringer): 2021-09-16, issue #497: The channel declarations here are a
// bit unwieldy; if we can use arrays-of-channels, that'll make things cleaner.

import float32;

type F32 = float32::F32;

// "node" performs the actual work of this systolic array, multiplying an input
// activation by the baked-in weight.
proc node {
    from_west: chan<F32> in;
    from_north: chan<F32> in;
    to_east: chan<F32> out;
    to_south: chan<F32> out;
    weight: F32;

    config(from_west: chan<F32> in, from_north: chan<F32> in, to_east: chan<F32> out,
           to_south: chan<F32> out, weight: F32) {
        (from_west, from_north, to_east, to_south, weight)
    }

    init { () }

    next(state: ()) {
        let (tok, activation) = recv(join(), from_west);
        let (tok, partial_sum) = recv(tok, from_north);

        // Compute our partial product.
        let product = float32::mul(activation, weight);

        // Send the activation east and the partial product south.
        let tok = send(tok, to_east, activation);
        let tok = send(tok, to_south, product);
    }
}

proc matmul<ROWS: u32, COLS: u32> {
    zeroes_out: chan<F32>[COLS] out;
    voids_in: chan<F32>[ROWS] in;

    config(activations_in: chan<F32>[ROWS] in, results_out: chan<F32>[COLS] out) {
        // Declare the east-to-west channels.
        let (east_outputs, west_inputs) = chan<F32>[COLS - u32:1][ROWS]("east_west");

        // Declare the north-to-south channels.
        let (south_outputs, north_inputs) = chan<F32>[COLS][ROWS - u32:1]("north_south");

        // TODO(rspringer): Zeros (as initial partial sums) would be best provided
        // by single-value channels.
        // Declare the zero-valued initial partial sum channels.
        let (zeroes_out, zeroes_in) = chan<F32>[COLS]("zeros");

        // Declare void channels for the east-edges of the array.
        let (voids_out, voids_in) = chan<F32>[ROWS]("void");

        // Spawn all the procs. Specify weights to give a "mul-by-two" matrix.
        let f32_0 = float32::zero(false);
        let f32_2 = F32 { sign: false, bexp: u8:128, fraction: u23:0 };

        // TODO(https://github.com/google/xls/issues/585): We can't loop (and thus
        // parameterize) this until we can constexpr evaluate `for` expressions.
        spawn node(activations_in[0], zeroes_in[0], east_outputs[0][0], south_outputs[0][0], f32_2);
        spawn node(west_inputs[0][0], zeroes_in[1], east_outputs[0][1], south_outputs[0][1], f32_0);
        spawn node(west_inputs[0][1], zeroes_in[2], east_outputs[0][2], south_outputs[0][2], f32_0);
        spawn node(west_inputs[0][2], zeroes_in[3], voids_out[0], south_outputs[0][3], f32_0);

        spawn node(
            activations_in[1], north_inputs[0][0], east_outputs[1][0], south_outputs[1][0], f32_0);
        spawn node(
            west_inputs[1][0], north_inputs[0][1], east_outputs[1][1], south_outputs[1][1], f32_2);
        spawn node(
            west_inputs[1][1], north_inputs[0][2], east_outputs[1][2], south_outputs[1][2], f32_0);
        spawn node(west_inputs[1][2], north_inputs[0][3], voids_out[1], south_outputs[1][3], f32_0);

        spawn node(
            activations_in[2], north_inputs[1][0], east_outputs[2][0], south_outputs[2][0], f32_0);
        spawn node(
            west_inputs[2][0], north_inputs[1][1], east_outputs[2][1], south_outputs[2][1], f32_0);
        spawn node(
            west_inputs[2][1], north_inputs[1][2], east_outputs[2][2], south_outputs[2][2], f32_2);
        spawn node(west_inputs[2][2], north_inputs[1][3], voids_out[2], south_outputs[2][3], f32_0);

        spawn node(activations_in[3], north_inputs[2][0], east_outputs[3][0], results_out[0], f32_0);
        spawn node(west_inputs[3][0], north_inputs[2][1], east_outputs[3][1], results_out[1], f32_0);
        spawn node(west_inputs[3][1], north_inputs[2][2], east_outputs[3][2], results_out[2], f32_0);
        spawn node(west_inputs[3][2], north_inputs[2][3], voids_out[3], results_out[3], f32_2);

        (zeroes_out, voids_in)
    }

    init { () }

    // All we need to do is to push in "zero" values to the top of the array and consume void
    // channels to keep the system moving.
    next(state: ()) {
        send(join(), zeroes_out[0], float32::zero(false));
        send(join(), zeroes_out[1], float32::zero(false));
        send(join(), zeroes_out[2], float32::zero(false));
        send(join(), zeroes_out[3], float32::zero(false));
        recv(join(), voids_in[0]);
        recv(join(), voids_in[1]);
        recv(join(), voids_in[2]);
        recv(join(), voids_in[3]);
    }
}

proc matmul_4x4 {
    config(activations_in: chan<F32>[4] in, results_out: chan<F32>[4] out) {
        spawn matmul<u32:4, u32:4>(activations_in, results_out);
    }

    init { () }

    next(state: ()) {  }
}

#[test_proc]
proc test_proc {
    activations_out: chan<F32>[4] out;
    results_in: chan<F32>[4] in;
    terminator: chan<bool> out;

    config(terminator: chan<bool> out) {
        let (activations_out, activations_in) = chan<F32>[4]("activations");
        let (results_out, results_in) = chan<F32>[4]("results");
        spawn matmul<u32:4, u32:4>(activations_in, results_out);
        (activations_out, results_in, terminator)
    }

    init { () }

    next(state: ()) {
        let f32_0 = float32::zero(false);
        let f32_2 = F32 { sign: false, bexp: u8:128, fraction: u23:0 };
        let f32_4 = F32 { sign: false, bexp: u8:129, fraction: u23:0 };

        // Send the desired inputs.
        let tok = for (i, tok): (u32, token) in range(u32:0, u32:4) {
            send(tok, activations_out[i], f32_2)
        }(join());

        // Send extra inputs to keep the system moving while our results are processing.
        let tok = for (_, tok): (u32, token) in range(u32:0, u32:4) {
            for (i, tok): (u32, token) in range(u32:0, u32:4) {
                send(tok, activations_out[i], f32_0)
            }(tok)
        }(tok);

        // Flush the intermediate values.
        let tok = for (_, tok): (u32, token) in range(u32:0, u32:0) {
            for (i, tok): (u32, token) in range(u32:0, u32:4) {
                let (tok, _) = recv(tok, results_in[i]);
                tok
            }(tok)
        }(tok);

        let (tok, value) = recv(tok, results_in[0]);
        assert_eq(value, f32_0);
        let (tok, value) = recv(tok, results_in[1]);
        assert_eq(value, f32_0);
        let (tok, value) = recv(tok, results_in[2]);
        assert_eq(value, f32_0);
        let (tok, value) = recv(tok, results_in[3]);
        assert_eq(value, f32_4);

        let tok = send(tok, terminator, true);
    }
}
