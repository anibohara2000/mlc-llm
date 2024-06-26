"""Command line entrypoint of serve."""

import json

from mlc_llm.help import HELP
from mlc_llm.interface.serve import serve
from mlc_llm.support.argparse import ArgumentParser


def main(argv):
    """Parse command line arguments and call `mlc_llm.interface.serve`."""
    parser = ArgumentParser("MLC LLM Serve CLI")

    parser.add_argument(
        "model",
        type=str,
        help=HELP["model"] + " (required)",
    )
    parser.add_argument(
        "--device",
        type=str,
        default="auto",
        help=HELP["device_deploy"] + ' (default: "%(default)s")',
    )
    parser.add_argument(
        "--model-lib-path",
        type=str,
        default=None,
        help=HELP["model_lib_path"] + ' (default: "%(default)s")',
    )
    parser.add_argument(
        "--max-batch-size",
        type=int,
        default=80,
        help=HELP["max_batch_size"] + ' (default: "%(default)s")',
    )
    parser.add_argument(
        "--max-total-seq-length", type=int, help=HELP["max_total_sequence_length_serve"]
    )
    parser.add_argument("--prefill-chunk-size", type=int, help=HELP["prefill_chunk_size_serve"])
    parser.add_argument("--enable-tracing", action="store_true", help=HELP["enable_tracing_serve"])
    parser.add_argument(
        "--host",
        type=str,
        default="127.0.0.1",
        help="host name" + ' (default: "%(default)s")',
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8000,
        help="port" + ' (default: "%(default)s")',
    )
    parser.add_argument("--allow-credentials", action="store_true", help="allow credentials")
    parser.add_argument(
        "--allow-origins",
        type=json.loads,
        default=["*"],
        help="allowed origins" + ' (default: "%(default)s")',
    )
    parser.add_argument(
        "--allow-methods",
        type=json.loads,
        default=["*"],
        help="allowed methods" + ' (default: "%(default)s")',
    )
    parser.add_argument(
        "--allow-headers",
        type=json.loads,
        default=["*"],
        help="allowed headers" + ' (default: "%(default)s")',
    )
    parsed = parser.parse_args(argv)

    serve(
        model=parsed.model,
        device=parsed.device,
        model_lib_path=parsed.model_lib_path,
        max_batch_size=parsed.max_batch_size,
        max_total_sequence_length=parsed.max_total_seq_length,
        prefill_chunk_size=parsed.prefill_chunk_size,
        enable_tracing=parsed.enable_tracing,
        host=parsed.host,
        port=parsed.port,
        allow_credentials=parsed.allow_credentials,
        allow_origins=parsed.allow_origins,
        allow_methods=parsed.allow_methods,
        allow_headers=parsed.allow_headers,
    )
