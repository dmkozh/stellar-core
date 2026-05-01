import contextlib
import importlib.util
import io
import sys
import unittest
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


if __name__ == "__main__":
    unittest.main()