#!/usr/bin/env python3

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

import yaml
from yaml.nodes import MappingNode, ScalarNode, SequenceNode


@dataclass(frozen=True)
class TaggedValue:
    tag: str
    value: object


class TaggedLoader(yaml.SafeLoader):
    pass


def construct_tagged(
    loader: TaggedLoader, _tag_suffix: str, node: yaml.Node
) -> TaggedValue:
    if isinstance(node, ScalarNode):
        value = loader.construct_scalar(node)
    elif isinstance(node, SequenceNode):
        value = loader.construct_sequence(node, deep=True)
    elif isinstance(node, MappingNode):
        value = loader.construct_mapping(node, deep=True)
    else:
        raise TypeError(f"unsupported YAML node: {type(node).__name__}")
    return TaggedValue(node.tag, value)


TaggedLoader.add_multi_constructor("", construct_tagged)


def load_yaml(path: Path) -> object:
    with path.open(encoding="utf-8") as source:
        return yaml.load(source, Loader=TaggedLoader)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} LEFT.yaml RIGHT.yaml")
    left = Path(sys.argv[1])
    right = Path(sys.argv[2])
    if load_yaml(left) != load_yaml(right):
        raise SystemExit(f"resolved YAML differs: {left} {right}")


if __name__ == "__main__":
    main()
