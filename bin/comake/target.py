import os, copy, sys
import hashlib

from comake.lib import normal_path

class RawTarget(object):
    def __init__(self, inputs=None, tool=None, options=None, mono_options=None, settings_options=None):
        self.inputs = inputs
        if inputs is None:
            self.inputs = []
        self.tool = tool
        self.options = options
        self.mono_options = mono_options
        self.settings_options = settings_options

    def __repr__(self):
        return 'RawTarget(inputs=%r)' % (self.inputs, )

class RawOptions(object):
    def __init__(self, overriders=None, appenders=None):
        self.overriders = overriders
        self.appenders = appenders
        if not self.overriders:
            self.overriders = {}
        if not self.appenders:
            self.appenders = {}

    def affect(self, options):
        for key, value in self.overriders.items():
            options[key] = value
        for key, value in self.appenders.items():
            if isinstance(value, list):
                options[key] = options.get(key, []) + value
            elif isinstance(value, str):
                options[key] = options.get(key, '') + value
            elif isinstance(value, dict):
                odict = options.get(key, {})
                odict.update(value)
                options[key] = odict
            else:
                raise NotImplementedError()

class RawInput(object):
    def __init__(self, name=None, only_build_dep=False, root_relative=False):
        self.name = name
        self.only_build_dep = only_build_dep
        self.root_relative = root_relative

    def __repr__(self):
        return 'RawInput(%s)' % (self.name, )

class Statistics(object):
    def __init__(self):
        self.targets = 0
        self.made_targets = 0

statistics = Statistics()

target_cache = {}

class Target(object):
    def __init__(self, pathname, raw_target, inputs, original_tinput, options):
        self.pathname = pathname
        self.raw_target = raw_target
        self.inputs = inputs
        self.as_input = original_tinput
        self.tool = self.raw_target.tool
        self.hash = None
        self.built = False
        self.options = options

        if not self.tool:
            from comake.defaults import get_default_tool
            self.tool = get_default_tool(self)
            if not self.tool:
                from comake.tools import Empty
                self.tool = Empty()

    def cache(self):
        hasho = hashlib.md5()
        for inputo in self.inputs:
            hasho.update(inputo.hash)
        hasho.update(self.tool.cache(self).encode())
        hasho.update(self.pathname.encode())
        self.hash = hasho.digest()
        if self.hash in target_cache:
            return target_cache[self.hash]
        target_cache[self.hash] = self
        return self

    def exists(self):
        return os.path.exists(self.pathname)

    def mtime(self):
        return os.stat(self.pathname).st_mtime

    def filenames(self):
        from comake import COMAKE_OUTPUT_DIRECTORY
        comake_shortname = os.path.basename(self.pathname) + '-' + self.hash.hex()
        comake_outdir = os.path.join(os.path.dirname(self.pathname), COMAKE_OUTPUT_DIRECTORY)
        comake_pathname = os.path.join(comake_outdir, comake_shortname)
        comake_relname = os.path.join(COMAKE_OUTPUT_DIRECTORY, comake_shortname)
        return (comake_shortname, comake_outdir, comake_pathname, comake_relname)

    def _make(self, reporter):
        if os.path.islink(self.pathname):
            os.unlink(self.pathname)

        self.tool.make(self, reporter)
        if self.tool.EMPTY:
            return

        statistics.made_targets += 1

        comake_shortname, comake_outdir, comake_pathname, comake_relname = self.filenames()
        if not os.path.exists(comake_outdir):
            os.mkdir(comake_outdir)
        os.rename(self.pathname, comake_pathname)
        os.symlink(comake_relname, self.pathname)

    def get_ext(self):
        return os.path.splitext(self.pathname)[1]

    def get_actual_inputs(self):
        actual_inputs = []
        for tinput in self.inputs:
            if not tinput.as_input.only_build_dep:
                actual_inputs.append(tinput)
        return actual_inputs

    def build(self, reporter=None):
        if self.built:
            return 0

        if not self.tool.EMPTY:
            statistics.targets += 1

        if reporter is None:
            from comake.report import Report
            reporter = Report()

        reporter.title(self.pathname)

        rebuild_target = False
        rebuild_reason = None

        builds = 0
        for tinput in self.inputs:
            builds += tinput.build(reporter.sub())

        if os.path.islink(self.pathname):
            link = os.readlink(self.pathname)
            comake_shortname, comake_outdir, comake_pathname, comake_relname = self.filenames()
            if comake_relname != link:
                os.unlink(self.pathname)
                os.symlink(comake_relname, self.pathname)

        if not os.path.exists(self.pathname):
            rebuild_target = True
            rebuild_reason = "target requires creation"
        else:
            my_mtime = self.mtime()
            others_mtime = [tinput.mtime() for tinput in self.inputs]
            if others_mtime:
                max_others_mtime = max(others_mtime)
                if max_others_mtime > my_mtime:
                    rebuild_target = True
                    rebuild_reason = "target is older than dependencies"

        if not rebuild_target:
            if builds:
                rebuild_target = True
                rebuild_reason = "dependencies were rebuilt"

        if not rebuild_target:
            if self.tool.rebuild_needed(self):
                rebuild_target = True
                rebuild_reason = "rebuilding required by tool"

        if rebuild_target:
            if not self.tool.EMPTY:
                reporter.print_title()
                reporter.print_text("Building: %s" % (rebuild_reason, ))
            self._make(reporter)
            builds += 1

        self.built = True
        return builds

    def dump(self, indent=0):
        print(indent*"  " + self.pathname + " { ")
        print(indent*"  " + "  Built: %r" % (self.built, ))
        print(indent*"  " + "  %x" % (id(self), ))
        print(indent*"  " + "  " + self.hash.hex())
        print(indent*"  " + "  %r" % (self.tool.cache(self), ))
        count = 1
        if len(self.inputs) != 0:
            print(indent*"  " + "  Inputs#: %d" % (len(self.inputs), ))
            if not self.built:
                for tinput in self.inputs:
                    count += tinput.dump(indent+1)
            else:
                print(indent*"  " + "  [..]")

            print(indent*"  " + "  %d" % (count, ))
        print(indent*"  " + "} ")
        self.built = True
        return count

_per_directory_comake_file = {}

class COMakeFile(object):
    def __init__(self):
        self.targets = {}

    def __repr__(self):
        return 'COMakeFile(%s)' % (repr(self.targets), )

_globals_dict = None

class BuildCancelError(Exception):
    pass

def get_global_dict():
    global _globals_dict
    if _globals_dict is None:
        _globals_dict = {}
        _globals_dict['Target'] = RawTarget
        _globals_dict['Input'] = RawInput
        _globals_dict['Options'] = RawOptions
        _globals_dict['pathjoin'] = os.path.join
        _globals_dict['getenv'] = os.getenv
        _globals_dict['BuildCancelError'] = BuildCancelError
        from comake.tools import exported_tools
        for tool in exported_tools:
            _globals_dict[tool.__name__] = tool
    return _globals_dict

def get_per_directory_comake_file(dirname):
    comake_file = _per_directory_comake_file.get(dirname)
    if comake_file:
        return comake_file
    comake_file = COMakeFile()
    from comake import DEFAULT_BUILD_NAME
    build_name = os.path.join(dirname, DEFAULT_BUILD_NAME)
    if not os.path.exists(build_name):
        return comake_file

    globals_dict = dict(get_global_dict())
    def input_list(ext, new_ext):
        lst = []
        for filename in os.listdir(dirname):
            base_part, ext_part = os.path.splitext(filename)
            if ext_part == ext:
                lst.append(RawInput(base_part + new_ext))
        return lst

    def target_pathname(filename):
        return os.path.join(dirname, filename)

    def deftarget(filename):
        from comake.defaults import get_default_raw_target
        return get_default_raw_target(target_pathname(filename))

    globals_dict['input_list'] = input_list
    globals_dict['target_pathname'] = target_pathname
    globals_dict['deftarget'] = deftarget
    globals_dict['current_dirname'] = dirname
    with open(build_name) as build_file:
        code = compile(build_file.read(), build_name, 'exec')
    exec(code, globals_dict, vars(comake_file))
    _per_directory_comake_file[dirname] = comake_file
    return comake_file

def get_raw_target(pathname):
    dirname = os.path.dirname(pathname)
    basename = os.path.basename(pathname)
    comake_file = get_per_directory_comake_file(dirname)
    raw_target = comake_file.targets.get(basename)
    if not raw_target:
        from comake.defaults import get_default_raw_target
        raw_target = get_default_raw_target(pathname)
    if not raw_target:
        if os.path.exists(pathname):
            if not os.path.islink(pathname):
                return RawTarget()
            else:
                print(os.readlink(pathname))
    return raw_target

class TargetNotFoundError(Exception):
    pass

def create_target_tree(pathname):
    def _recur(pathname, original_tinput, options):
        raw_target = get_raw_target(pathname)
        if not raw_target:
            print("Error, target %s not found" % (pathname, ))
            raise TargetNotFoundError()

        options = copy.deepcopy(options)
        if raw_target.options:
            raw_target.options.affect(options)

        if raw_target.settings_options:
            from comake.settings import settings
            raw_target.settings_options.affect(vars(settings))

        inputs = []
        for index, tinput in enumerate(raw_target.inputs):
            if tinput.root_relative:
                abs_name = tinput.name
            else:
                abs_name = os.path.join(os.path.dirname(pathname), tinput.name)
            abs_name = normal_path(abs_name)
            try:
                inputs.append(_recur(abs_name, tinput, options))
            except TargetNotFoundError:
                print("Included from: %s [%d]" % (pathname, index+1))
                raise

        if raw_target.mono_options:
            raw_target.mono_options.affect(options)

        target = Target(pathname, raw_target, inputs, original_tinput, options)
        target = target.cache()
        return target

    return _recur(pathname, None, {})

def clean():
    from comake import COMAKE_OUTPUT_DIRECTORY
    from comake import build_root

    def unlink(pathname):
        pathname_display = pathname[len(build_root)+1:]
        print("removing file %s" % (pathname_display, ))
        os.unlink(pathname)

    def rmdir(pathname):
        pathname_display = pathname[len(build_root)+1:]
        print("removing dir %s" % (pathname_display, ))
        os.rmdir(pathname)

    def _recur(pathname):
        comake_dir = os.path.basename(pathname) == COMAKE_OUTPUT_DIRECTORY
        for filename in os.listdir(pathname):
            fullname = os.path.join(pathname, filename)
            if os.path.islink(fullname):
                link = os.readlink(fullname)
                if link.startswith(COMAKE_OUTPUT_DIRECTORY):
                    unlink(fullname)

            if os.path.isdir(fullname):
                _recur(fullname)
            if comake_dir:
                unlink(fullname)

        if comake_dir:
            rmdir(pathname)

    return _recur(build_root)
