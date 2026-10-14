"""Interpreter paths for the isolated, no-site source/installed launch contract."""
import os
import sys
import sysconfig


def isolated_path(service_root):
    stdlib = sysconfig.get_path("stdlib")
    platstdlib = sysconfig.get_path("platstdlib")
    archive = "python%d%d.zip" % sys.version_info[:2]
    paths = [service_root, stdlib, platstdlib,
             sysconfig.get_config_var("DESTSHARED"),
             os.path.join(platstdlib, "lib-dynload"),
             os.path.join(sys.base_prefix, "DLLs"),
             os.path.join(sys.base_prefix, archive),
             os.path.join(os.path.dirname(stdlib), archive)]
    return list(dict.fromkeys(os.path.abspath(p) for p in paths if p))
