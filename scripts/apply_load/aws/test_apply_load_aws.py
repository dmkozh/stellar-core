import contextlib
import importlib.util
import io
import sys
import base64
import tempfile
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("apply_load_aws.py")
MODULE_NAME = "apply_load_aws"
MODULE_SPEC = importlib.util.spec_from_file_location(MODULE_NAME, MODULE_PATH)
apply_load_aws = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_NAME] = apply_load_aws
assert MODULE_SPEC.loader is not None
MODULE_SPEC.loader.exec_module(apply_load_aws)


class ApplyLoadAwsTests(unittest.TestCase):
    def test_benchmark_requires_all_template_parameters(self) -> None:
        parser = apply_load_aws.build_parser()

        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                parser.parse_args([
                    "benchmark",
                    "--image",
                    "stellar/apply-load:latest",
                    "--model-tx",
                    "sac",
                    "--tx-count",
                    "100",
                    "--dependent-tx-clusters",
                    "4",
                ])

    def test_benchmark_supports_all_model_tx_kinds(self) -> None:
        base_values = {
            "tx_count": 100,
            "dependent_tx_clusters": 4,
            "num_ledgers": 10,
        }

        for model_tx in ("sac", "custom_token", "soroswap"):
            config = apply_load_aws.render_config(
                "benchmark", {**base_values, "model_tx": model_tx}
            )
            self.assertIn(f"APPLY_LOAD_MODEL_TX={model_tx}", config)
            self.assertIn(
                (
                    'LOG_FILE_PATH="'
                    f'{apply_load_aws.APPLY_LOAD_LOG_FILE_PATH}'
                    '"'
                ),
                config,
            )

    def test_apply_load_log_path_uses_tmp(self) -> None:
        self.assertEqual(apply_load_aws.APPLY_LOAD_LOG_FILE_PATH, "/tmp/apply-load.log")

    def test_max_sac_alias_maps_to_max_sac_tps_mode(self) -> None:
        parser = apply_load_aws.build_parser()
        args = parser.parse_args([
            "max-sac",
            "--image",
            "stellar/apply-load:latest",
            "--min-tps",
            "1000",
            "--max-tps",
            "2000",
            "--target-close-time-ms",
            "5000",
            "--dependent-tx-clusters",
            "4",
        ])

        self.assertEqual(args.apply_load_mode, "max-sac-tps")

    def test_aws_run_subcommand_parses_without_log_file_path(self) -> None:
        parser = apply_load_aws.build_parser()
        args = parser.parse_args([
            "aws-run",
            "max-sac-tps",
            "--instance-id",
            "i-1234567890abcdef0",
            "--region",
            "us-west-2",
            "--local-log-path",
            "apply-load-logs/test.log",
            "--image",
            "stellar/apply-load:latest",
            "--min-tps",
            "1000",
            "--max-tps",
            "2000",
            "--target-close-time-ms",
            "5000",
            "--dependent-tx-clusters",
            "4",
        ])

        self.assertEqual(args.command, "aws-run")
        self.assertEqual(args.apply_load_mode, "max-sac-tps")

    def test_build_docker_command_uses_config_mode(self) -> None:
        command = apply_load_aws.build_docker_command(
            "/tmp/config.cfg", "stellar/apply-load:latest", 3000
        )

        self.assertNotIn("--mode", command)
        self.assertIn(
            f"{apply_load_aws.APPLY_LOAD_LOG_FILE_PATH}:{apply_load_aws.APPLY_LOAD_LOG_FILE_PATH}",
            command,
        )
        self.assertIn("--device-write-iops", command)
        self.assertEqual(
            command[-4:],
            ["apply-load", "--console", "--conf", "/config.cfg"],
        )

    def test_build_remote_apply_load_command_keeps_shell_operators(self) -> None:
        command = apply_load_aws.build_remote_apply_load_command(
            "max-sac-tps",
            {
                "min_tps": 1000,
                "max_tps": 2000,
                "target_close_time_ms": 5000,
                "dependent_tx_clusters": 4,
            },
            "stellar/apply-load:latest",
            3000,
        )

        self.assertIn("rm -f", command)
        self.assertIn("&& cd", command)
        self.assertNotIn("'&&'", command)
        self.assertIn("--image", command)
        self.assertNotIn("APPLY_LOAD_AWS_SUPPRESS_LOG_TAIL=1", command)

    def test_run_streams_child_output(self) -> None:
        output = io.StringIO()

        with contextlib.redirect_stdout(output):
            apply_load_aws.run([
                sys.executable,
                "-c",
                "print('child-output')",
            ])

        self.assertIn("Running:", output.getvalue())
        self.assertIn("child-output", output.getvalue())

    def test_run_apply_load_prints_full_log(self) -> None:
        log_path = Path(apply_load_aws.APPLY_LOAD_LOG_FILE_PATH)

        def fake_run(*_args, **_kwargs):
            log_path.write_text("line-1\nline-2\n", encoding="utf-8")
            return apply_load_aws.subprocess.CompletedProcess(
                ["docker"],
                0,
                "ignored stdout\n",
                "",
            )

        with mock.patch.object(
            apply_load_aws,
            "run",
            side_effect=fake_run,
        ) as run_command:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                apply_load_aws.run_apply_load(
                    'LOG_FILE_PATH="/tmp/apply-load.log"\n',
                    "stellar/apply-load:latest",
                    None,
                )

        docker_command = run_command.call_args.args[0]
        self.assertIn(
            (
                f"{apply_load_aws.APPLY_LOAD_LOG_FILE_PATH}:"
                f"{apply_load_aws.APPLY_LOAD_LOG_FILE_PATH}"
            ),
            docker_command,
        )
        self.assertIn("line-1", output.getvalue())
        self.assertIn("line-2", output.getvalue())

    def test_start_ec2_instance_uses_legacy_policy_tag(self) -> None:
        with mock.patch.object(
            apply_load_aws,
            "run_capture_output",
            return_value="i-1234567890abcdef0",
        ) as run_capture_output:
            with mock.patch.object(apply_load_aws, "run") as run_command:
                instance_id = apply_load_aws.start_ec2_instance(
                    "ami-1234",
                    "us-west-2",
                    "core-test",
                    "core-test",
                )

        self.assertEqual(instance_id, "i-1234567890abcdef0")
        launch_command = run_capture_output.call_args.args[0]
        self.assertIn(
            "ResourceType=instance,Tags=[{Key=test,Value=max-sac-tps},"
            "{Key=ManagedBy,Value=ApplyLoadScript}]",
            launch_command,
        )
        run_command.assert_called_once_with([
            "aws",
            "ec2",
            "wait",
            "instance-running",
            "--instance-ids",
            "i-1234567890abcdef0",
            "--region",
            "us-west-2",
        ])

    def test_run_ssm_command_logs_status_and_polls(self) -> None:
        with mock.patch.object(
            apply_load_aws,
            "run_capture_output",
            return_value="command-123",
        ):
            with mock.patch.object(apply_load_aws, "run") as run_command:
                run_command.side_effect = [
                    mock.Mock(returncode=0, stdout="InProgress\n"),
                    mock.Mock(returncode=0, stdout="Success\n"),
                    mock.Mock(
                        returncode=0,
                        stdout=(
                            '{"Status":"Success",'
                            '"StandardOutputContent":"done",'
                            '"StandardErrorContent":""}'
                        ),
                    ),
                ]
                with mock.patch.object(apply_load_aws.time, "sleep") as sleep:
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        apply_load_aws.run_ssm_command(
                            "i-1234567890abcdef0",
                            "us-west-2",
                            "echo hello",
                        )

        self.assertIn("Command status: InProgress", output.getvalue())
        self.assertIn("Command status: Success", output.getvalue())
        self.assertIn("done", output.getvalue())
        sleep.assert_called_once_with(5)

    def test_run_apply_load_on_instance_downloads_log_after_failure(self) -> None:
        values = {
            "min_tps": 1000,
            "max_tps": 2000,
            "target_close_time_ms": 5000,
            "dependent_tx_clusters": 4,
        }

        with mock.patch.object(
            apply_load_aws,
            "run_ssm_command",
            side_effect=SystemExit("run failed"),
        ) as run_ssm_command:
            with mock.patch.object(
                apply_load_aws,
                "download_apply_load_log",
            ) as download_apply_load_log:
                with self.assertRaises(SystemExit):
                    apply_load_aws.run_apply_load_on_instance(
                        "i-1234567890abcdef0",
                        "us-west-2",
                        Path("apply-load-logs/test.log"),
                        "max-sac-tps",
                        values,
                        "stellar/apply-load:latest",
                        3000,
                    )

        self.assertEqual(run_ssm_command.call_count, 1)
        self.assertIn(
            "python3 apply_load_aws.py max-sac-tps",
            run_ssm_command.call_args.args[2],
        )
        download_apply_load_log.assert_called_once_with(
            "i-1234567890abcdef0",
            "us-west-2",
            Path("apply-load-logs/test.log"),
        )

    def test_download_apply_load_log_uses_requested_local_path(self) -> None:
        with tempfile.NamedTemporaryFile(delete=False) as temp_file:
            temp_path = Path(temp_file.name)

        try:
            with mock.patch.object(
                apply_load_aws,
                "download_remote_file",
                return_value=True,
            ) as download_remote_file:
                apply_load_aws.download_apply_load_log(
                    "i-1234567890abcdef0",
                    "us-west-2",
                    temp_path,
                )

            download_remote_file.assert_called_once_with(
                "i-1234567890abcdef0",
                "us-west-2",
                apply_load_aws.APPLY_LOAD_LOG_FILE_PATH,
                temp_path,
            )
        finally:
            temp_path.unlink(missing_ok=True)

    def test_download_remote_file_reassembles_chunks(self) -> None:
        results = [
            ("11\n", ""),
            (base64.b64encode(b"hello ").decode() + "\n", ""),
            (base64.b64encode(b"world").decode() + "\n", ""),
        ]

        with tempfile.NamedTemporaryFile(delete=False) as temp_file:
            temp_path = Path(temp_file.name)

        try:
            with mock.patch.object(
                apply_load_aws,
                "run_ssm_command_result",
                side_effect=results,
            ):
                with mock.patch.object(
                    apply_load_aws,
                    "REMOTE_FILE_CHUNK_SIZE_BYTES",
                    6,
                ):
                    downloaded = apply_load_aws.download_remote_file(
                        "i-1234567890abcdef0",
                        "us-west-2",
                        apply_load_aws.APPLY_LOAD_LOG_FILE_PATH,
                        temp_path,
                    )

            self.assertTrue(downloaded)
            self.assertEqual(temp_path.read_bytes(), b"hello world")
        finally:
            temp_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()