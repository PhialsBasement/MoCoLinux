import re, os, sys
from comake.target import RawInput
from comake.lib import normal_path

cdeps_cache = {}

def calc_deps(pathname):
    def _recurse(pathname):
        if pathname in cdeps_cache:
            return cdeps_cache[pathname]

        if not os.path.exists(pathname):
            return set()

        set_union = set()
        set_union.add(pathname)
        for line in open(pathname):
            m = re.match("# *include <([^>]+)>", line.strip())
            result_set = None
            if m:
                included_path = m.groups(0)[0]
                if included_path.startswith('colinux'):
                    combined_path = os.path.join('src', included_path)
                    result_set = _recurse(combined_path)
                    result_set.add(combined_path)
            else:
                m = re.match("# *include \"([^>]+)\"", line.strip())
                if m:
                    included_path = m.groups(0)[0]
                    included_path = normal_path(os.path.join(os.path.dirname(pathname), included_path))
                    result_set = _recurse(included_path)

            if result_set:
                set_union.update(result_set)
        cdeps_cache[pathname] = set_union
        return set_union

    result_set = set(_recurse(pathname))
    result_set.remove(pathname)
    lst = list(result_set)
    lst.sort()
    return [RawInput(pathname, only_build_dep=True, root_relative=True) for pathname in lst]
