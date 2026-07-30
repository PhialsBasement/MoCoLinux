import re, os

class DefaultTarget(object):
    def __init__(self, regex):
        self._regex = re.compile(regex)

    def match(self, pathname):
        m = self._regex.match(pathname)
        if m:
            return self.format(m, pathname)

    def format(self, mregex, pathname):
        pass

class FileIDAllocator(object):
    def __init__(self):
        self.last_id = -1
        self.path_dict = {}
        self.path_list = []

    def allocate(self, pathname):
        if pathname not in self.path_dict:
            self.last_id += 1
            last_id = self.last_id
            self.path_dict[pathname] = last_id
            self.path_list.append(pathname)
            return last_id
        return self.path_dict[pathname]

file_id_allocator = FileIDAllocator()

class DefaultObj(DefaultTarget):
    def format(self, mregex, pathname):
        global colinux_file_id
        from comake.target import RawTarget, RawInput, RawOptions
        from comake.tools import Compiler
        base_dep_name = None

        full_name = mregex.groups(0)[0] + '.c'
        if os.path.exists(full_name) and not os.path.islink(full_name):
            base_dep_name = os.path.basename(full_name)
        else:
            full_name = mregex.groups(0)[0] + '.cpp'
            if os.path.exists(full_name) and not os.path.islink(full_name):
                base_dep_name = os.path.basename(full_name)

        if not base_dep_name:
            return None

        inputs = [RawInput(base_dep_name)]

        from comake.cdeps import calc_deps
        inputs.extend(calc_deps(full_name))

        colinux_file_id = file_id_allocator.allocate(full_name)
        return RawTarget(inputs=inputs,
                         tool=Compiler(),
                         options=RawOptions(
                             appenders=dict(
                             compiler_defines=dict(
                             COLINUX_FILE_ID=str(colinux_file_id),
                         ))))

_default_targets = (
    DefaultObj('(.*)[.]o$'),
)

def get_default_raw_target(pathname):
    for default in _default_targets:
        m = default.match(pathname)
        if m:
            return m

def get_default_tool(target):
    if target.get_ext() == '.o':
        for tinput in target.get_actual_inputs():
            if tinput.get_ext() != '.o':
                break
        else:
            from comake.tools import Linker
            return Linker()

    if target.get_ext() == '.a':
        from comake.tools import Archiver
        return Archiver()

