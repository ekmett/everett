#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
"""Check shader tool arguments and failure publication without invoking a GPU/toolchain."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import shader_compile


class ShaderCompilerTest(unittest.TestCase):
    def test_complete_pipeline_flags_and_renaming(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input.hlsl"
            source.write_text("// test fixture\n")
            spv, metal, bindings = (root / name for name in ["shader.spv", "shader.metal", "bindings.h"])
            commands = []
            def run(command):
                command = [str(x) for x in command]
                commands.append(command)
                if command[0] == "test-dxc":
                    Path(command[command.index("-Fo") + 1]).write_bytes(b"validated-spv-fixture")
                elif command[0] == "test-cross":
                    Path(command[command.index("--output") + 1]).write_text("kernel void renamed() {}\n")
            reflection = {"entryPoints": [{"name": "renamed", "mode": "comp"}],
                          "ssbos": [{"name": "parameters", "set": 0, "binding": 3, "readonly": True}]}
            argv = ["shader_compile.py", "--source", str(source), "--entry", "merge_order", "--output-entry", "renamed",
                    "--msl-entry", "renamed", "--spv", str(spv), "--msl", str(metal), "--bindings", str(bindings),
                    "--define", "COMPRESSED_INPUT=1", "--include", str(root), "--dxc", "test-dxc",
                    "--spirv-val", "test-val", "--spirv-cross", "test-cross"]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(shader_compile, "run", run), \
                 mock.patch.object(shader_compile, "reflect", return_value=reflection):
                shader_compile.main()
            self.assertEqual(commands[0][0:10], ["test-dxc", "-spirv", "-HV", "2021", "-fvk-use-scalar-layout",
                "-fspv-target-env=vulkan1.3", "-T", "cs_6_0", "-E", "merge_order"])
            self.assertIn("-fspv-entrypoint-name=renamed", commands[0])
            self.assertIn("-DCOMPRESSED_INPUT=1", commands[0])
            self.assertIn("-I" + root.as_posix(), commands[0])
            self.assertEqual(commands[1][:4], ["test-val", "--target-env", "vulkan1.3", "--scalar-block-layout"])
            self.assertIn("--msl-decoration-binding", commands[2])
            at = commands[2].index("--rename-entry-point")
            self.assertEqual(commands[2][at:at + 4], ["--rename-entry-point", "renamed", "renamed", "comp"])
            self.assertEqual(commands[2][commands[2].index("--msl-version") + 1], "30200")
            self.assertEqual(spv.read_bytes(), b"validated-spv-fixture")
            self.assertEqual(metal.read_text(), "kernel void renamed() {}\n")
            self.assertIn("namespace everett_gpu", bindings.read_text())
            self.assertIn("merge_order_parameters_buffer_binding = 3", bindings.read_text())
            self.assertIn("Reflected from input.hlsl", bindings.read_text())
            self.assertNotIn(str(root), bindings.read_text())
            self.assertFalse(list(root.glob(".*.tmp")))

    def test_validation_failure_preserves_previous_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, spv, metal = (root / name for name in ["input.hlsl", "shader.spv", "shader.metal"])
            source.write_text("// test fixture\n"); spv.write_bytes(b"previous-spv"); metal.write_text("previous-msl")
            def run(command):
                command = [str(x) for x in command]
                if command[0] == "test-dxc":
                    Path(command[command.index("-Fo") + 1]).write_bytes(b"invalid-new-spv")
                else:
                    raise subprocess.CalledProcessError(1, command)
            argv = ["shader_compile.py", "--source", str(source), "--entry", "main", "--spv", str(spv),
                    "--msl", str(metal), "--dxc", "test-dxc", "--spirv-val", "test-val"]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(shader_compile, "run", run):
                with self.assertRaises(subprocess.CalledProcessError):
                    shader_compile.main()
            self.assertEqual(spv.read_bytes(), b"previous-spv")
            self.assertEqual(metal.read_text(), "previous-msl")
            self.assertFalse(list(root.glob(".*.tmp")))

    def test_vulkan_only_does_not_require_spirv_cross(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, spv = root / "input.hlsl", root / "shader.spv"
            source.write_text("// test fixture\n")
            commands = []
            def run(command):
                commands.append([str(x) for x in command])
                if "-Fo" in command:
                    Path(command[command.index("-Fo") + 1]).write_bytes(b"spv")
            argv = ["shader_compile.py", "--source", str(source), "--entry", "parse_input", "--output-entry", "main",
                    "--spv", str(spv)]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(shader_compile, "run", run), \
                 mock.patch.object(shader_compile, "reflect", side_effect=AssertionError("unexpected reflection")):
                shader_compile.main()
            self.assertEqual(len(commands), 2)
            self.assertIn("-fspv-entrypoint-name=main", commands[0])
            self.assertEqual(spv.read_bytes(), b"spv")

    def test_ambiguous_entry_and_binding_normalization(self):
        reflection = {"push_constants": [{"name": "push"}], "ubos": [{"name": "type.Scene", "binding": 4}],
                      "ssbos": [{"name": "data-buffer", "binding": 7}], "images": [{"name": "image", "binding": 2}]}
        header = shader_compile.binding_header(reflection, Path("shader.hlsl"), "entry", "everett_gpu")
        self.assertIn("entry_push_buffer_binding = 0", header)
        self.assertIn("entry_Scene_buffer_binding = 4", header)
        self.assertIn("entry_data_buffer_buffer_binding = 7", header)
        self.assertIn("entry_image_texture_binding = 2", header)
        # The explicit entry lookup must reject ambiguous reflected names before
        # a Metal compiler can accidentally receive a different entry point.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source = root / "a.hlsl"; source.write_text("// test\n")
            spv, metal = root / "a.spv", root / "a.metal"
            def run(command):
                if "-Fo" in command:
                    Path(command[command.index("-Fo") + 1]).write_bytes(b"spv")
            argv = ["shader_compile.py", "--source", str(source), "--entry", "parse_input", "--msl-entry", "main",
                    "--spv", str(spv), "--msl", str(metal)]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(shader_compile, "run", run), \
                 mock.patch.object(shader_compile, "reflect", return_value={"entryPoints": []}):
                with self.assertRaisesRegex(ValueError, "Expected one SPIR-V entry"):
                    shader_compile.main()
            self.assertFalse(spv.exists())
            self.assertFalse(metal.exists())


if __name__ == "__main__":
    unittest.main()
