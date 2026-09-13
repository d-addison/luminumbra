"""Seal an installed SDK against a successful installed viewport qualification."""
import argparse
import importlib
from pathlib import Path
import sys
import types


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=Path)
    parser.add_argument('--qualification', required=True, type=Path)
    parser.add_argument('--host-receipt', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    args = parser.parse_args()
    # Source-tree CLI loads only portable modules, never extension/__init__.py.
    package = types.ModuleType('luminumbra_viewport_seal')
    here = Path(__file__).resolve().parent
    package.__path__ = [str(here / 'extension'), str(here)]
    sys.modules[package.__name__] = package
    api = importlib.import_module(package.__name__ + '.viewport_installation')
    api.seal_installation(args.host, args.qualification, args.host_receipt, args.manifest)
    print('Sealed complete installed viewport SDK: ' + str(args.manifest))


if __name__ == '__main__':
    main()
