#!/usr/bin/env python3
"""Reduce Godot's generated extension_api.json to the fields used by the index."""

import json
import pathlib
import sys


MEMBER_KEYS = ("methods", "constructors", "members", "properties", "signals", "constants", "enums")


def reduce_member(member: dict) -> dict:
    result = {
        key: member[key]
        for key in ("name", "type", "return_type", "is_static", "is_vararg", "default_value")
        if key in member
    }
    if member.get("is_virtual") is True:
        result["is_virtual"] = True
    if isinstance(member.get("return_value"), dict):
        result["return_value"] = {
            key: member["return_value"][key]
            for key in ("type",)
            if key in member["return_value"]
        }
    if isinstance(member.get("arguments"), list):
        result["arguments"] = [
            {key: argument[key] for key in ("name", "type", "default_value") if key in argument}
            for argument in member["arguments"]
            if isinstance(argument, dict)
        ]
    if isinstance(member.get("values"), list):
        result["values"] = [
            {key: value[key] for key in ("name", "value") if key in value}
            for value in member["values"]
            if isinstance(value, dict)
        ]
    return result


def reduce_class(value: dict) -> dict:
    result = {key: value[key] for key in ("name", "inherits") if key in value}
    for key in MEMBER_KEYS:
        if isinstance(value.get(key), list):
            result[key] = [reduce_member(member) for member in value[key] if isinstance(member, dict)]
    return result


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT OUTPUT", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[1])
    destination = pathlib.Path(sys.argv[2])
    data = json.loads(source.read_text(encoding="utf-8"))
    reduced = {
        "gdscript_lsp_schema": 2,
        "header": data.get("header", {}),
        "builtin_classes": [reduce_class(value) for value in data.get("builtin_classes", [])],
        "classes": [reduce_class(value) for value in data.get("classes", [])],
        "utility_functions": [reduce_member(value) for value in data.get("utility_functions", [])],
        "singletons": [
            {key: value[key] for key in ("name", "type") if key in value}
            for value in data.get("singletons", [])
        ],
        "global_constants": [
            {key: value[key] for key in ("name", "type", "value") if key in value}
            for value in data.get("global_constants", [])
        ],
        "global_enums": [
            {
                "name": value.get("name", ""),
                "values": [
                    {key: item[key] for key in ("name", "value") if key in item}
                    for item in value.get("values", [])
                ],
            }
            for value in data.get("global_enums", [])
        ],
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(reduced, separators=(",", ":")), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
