import contextlib
import importlib.util
import io
import sys
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
            "log_file_path": "/tmp/apply-load.log",
            "tx_count": 100,
            "dependent_tx_clusters": 4,
            "num_ledgers": 10,
        }

        for model_tx in ("sac", "custom_token", "soroswap"):
            config = apply_load_aws.render_config(
                "benchmark", {**base_values, "model_tx": model_tx}
            )
            self.assertIn(f"APPLY_LOAD_MODEL_TX={model_tx}", config)
            self.assertIn('LOG_FILE_PATH="/tmp/apply-load.log"', config)

    def test_max_sac_alias_maps_to_max_sac_tps_mode(self) -> None:
        parser = apply_load_aws.build_parser()
        args = parser.parse_args([
            "max-sac",
            "--image",
            "stellar/apply-load:latest",
            "--log-file-path",
            "/tmp/apply-load.log",
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

    def test_build_docker_command_uses_config_mode(self) -> None:
        command = apply_load_aws.build_docker_command(
            "/tmp/config.cfg", "stellar/apply-load:latest", 3000
        )

        self.assertNotIn("--mode", command)
        self.assertIn("--device-write-iops", command)
        self.assertEqual(
            command[-4:],
            ["apply-load", "--console", "--conf", "/config.cfg"],
        )

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
        self.assertIn("Command output: done", output.getvalue())
        sleep.assert_called_once_with(5)


if __name__ == "__main__":
    unittest.main()