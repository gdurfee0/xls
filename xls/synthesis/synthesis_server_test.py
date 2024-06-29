#
# Copyright 2020 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Tests of the synthesis service: client and fake server."""

import subprocess
import time

import portpicker

from google.protobuf import text_format
from absl.testing import absltest
from xls.common import runfiles
from xls.synthesis import synthesis_pb2

CLIENT_PATH = runfiles.get_path('xls/synthesis/synthesis_client_main')
SERVER_PATH = runfiles.get_path('xls/synthesis/fake_synthesis_server_main')

VERILOG = """
module main(
  input wire [31:0] x,
  input wire [31:0] y,
  output wire [31:0] out
);
  assign out = x + y;
endmodule
"""


class SynthesisServerTest(absltest.TestCase):

  def _start_server(self, args):
    port = portpicker.pick_unused_port()
    proc = subprocess.Popen(
        [runfiles.get_path(SERVER_PATH), f'--port={port}'] + args
    )

    # allow some time for the server to open the port before continuing
    time.sleep(1)

    return port, proc

  def test_slack(self):
    port, proc = self._start_server(['--max_frequency_ghz=2.0'])

    verilog_file = self.create_tempfile(content=VERILOG)

    response_text = subprocess.check_output(
        [CLIENT_PATH, verilog_file.full_path, f'--port={port}', '--ghz=1.0']
    ).decode('utf-8')

    response = text_format.Parse(response_text, synthesis_pb2.CompileResponse())
    self.assertGreaterEqual(response.slack_ps, 0)

    response_text = subprocess.check_output(
        [CLIENT_PATH, verilog_file.full_path, f'--port={port}', '--ghz=4.0']
    ).decode('utf-8')

    response = text_format.Parse(response_text, synthesis_pb2.CompileResponse())
    self.assertLess(response.slack_ps, 0)

    proc.terminate()
    proc.wait()

  def test_error(self):
    port, proc = self._start_server(
        ['--max_frequency_ghz=2.0', '--serve_errors']
    )

    verilog_file = self.create_tempfile(content=VERILOG)
    # pylint: disable=subprocess-run-check
    comp = subprocess.run(
        [CLIENT_PATH, verilog_file.full_path, f'--port={port}', '--ghz=1.0']
    )

    self.assertNotEqual(comp.returncode, 0)

    proc.terminate()
    proc.wait()


if __name__ == '__main__':
  absltest.main()
