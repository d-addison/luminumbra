"""Trusted source launcher: python -I -S -B /absolute/service/launch.py ..."""
import sys


def main():
    # Check before importing anything that can search disk, including the package.
    if not (sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode):
        print('{"ok":false,"findings":[{"rule_id":"toolchain.bootstrap",'
              '"severity":"error","message":"Launch Python with -I -S -B.",'
              '"field":"","object_id":"","component_id":"","node_id":""}]}')
        return 1
    import os
    service_root = os.path.dirname(os.path.abspath(__file__))
    # -I -S supplied only interpreter paths. Add the reviewed service, never cwd.
    sys.path.insert(0, service_root)
    from luminumbra_author.bootstrap import isolated_path
    sys.path[:] = isolated_path(service_root)
    if sys.argv[1:2] == ["--scripts"]:
        del sys.argv[1]
        from luminumbra_author.meshgen_scripts import main as entry
    else:
        from luminumbra_author.__main__ import main as entry
    return entry()


if __name__ == "__main__":
    raise SystemExit(main())
