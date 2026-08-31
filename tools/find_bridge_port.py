#!/usr/bin/env python3
"""Localiza a porta serial do Blue Mechanic por VID/PID USB."""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Iterable, Sequence

try:
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - depende do Python do host
    print(
        "pyserial nao encontrado; execute com o Python do PlatformIO ou instale "
        "com 'python -m pip install pyserial'.",
        file=sys.stderr,
    )
    raise SystemExit(3) from exc


DEFAULT_VID = 0x1209
DEFAULT_PID = 0x0001


def parse_int(value: str) -> int:
    return int(value, 0)


def matching_ports(
    ports: Iterable[object], vid: int, pid: int, serial_number: str | None
) -> list[object]:
    candidates = [
        port
        for port in ports
        if getattr(port, "vid", None) == vid and getattr(port, "pid", None) == pid
    ]
    if serial_number is None:
        return candidates

    expected = serial_number.casefold()
    return [
        port
        for port in candidates
        if (getattr(port, "serial_number", None) or "").casefold() == expected
    ]


def scan(vid: int, pid: int, serial_number: str | None) -> list[object]:
    return matching_ports(list_ports.comports(), vid, pid, serial_number)


def port_record(port: object) -> dict[str, object | None]:
    return {
        "port": getattr(port, "device", None),
        "vid": getattr(port, "vid", None),
        "pid": getattr(port, "pid", None),
        "serial_number": getattr(port, "serial_number", None),
        "description": getattr(port, "description", None),
        "manufacturer": getattr(port, "manufacturer", None),
        "product": getattr(port, "product", None),
        "location": getattr(port, "location", None),
    }


def find_with_wait(
    vid: int, pid: int, serial_number: str | None, wait_seconds: float
) -> list[object]:
    deadline = time.monotonic() + max(0.0, wait_seconds)
    while True:
        ports = scan(vid, pid, serial_number)
        if ports or time.monotonic() >= deadline:
            return ports
        time.sleep(0.25)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Imprime a COM do Blue Mechanic CAN USB Bridge."
    )
    parser.add_argument("--vid", type=parse_int, default=DEFAULT_VID)
    parser.add_argument("--pid", type=parse_int, default=DEFAULT_PID)
    parser.add_argument(
        "--serial",
        default=None,
        help="filtra pelo serial hexadecimal único exibido por --json",
    )
    parser.add_argument(
        "--ignore-serial",
        action="store_true",
        help="ignora --serial e seleciona somente por VID/PID",
    )
    parser.add_argument(
        "--wait",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="aguarda a enumeracao por ate este numero de segundos",
    )
    parser.add_argument("--all", action="store_true", help="imprime todas as portas")
    parser.add_argument("--json", action="store_true", help="saida JSON detalhada")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    serial_number = None if args.ignore_serial else args.serial
    ports = find_with_wait(args.vid, args.pid, serial_number, args.wait)

    if not ports:
        serial_text = "qualquer" if serial_number is None else serial_number
        print(
            f"Bridge nao encontrado (VID:PID={args.vid:04X}:{args.pid:04X}, "
            f"serial={serial_text}).",
            file=sys.stderr,
        )
        return 1

    if len(ports) > 1 and not args.all:
        devices = ", ".join(str(getattr(port, "device", "?")) for port in ports)
        print(
            f"Mais de um bridge corresponde ao filtro ({devices}); use --all ou "
            "informe --serial.",
            file=sys.stderr,
        )
        return 2

    selected = ports if args.all else ports[:1]
    if args.json:
        print(json.dumps([port_record(port) for port in selected], indent=2))
    else:
        for port in selected:
            print(getattr(port, "device"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
