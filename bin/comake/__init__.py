import os, sys

DEFAULT_BUILD_NAME = 'build.comake.py'
COMAKE_OUTPUT_DIRECTORY = '.comake.build'

def _build_root():
    from os.path import dirname
    return dirname(dirname(dirname(os.path.abspath(__file__))))

build_root = _build_root()

def main(args):
    os.chdir(build_root)

    if '--help' in args or len(args) == 0:
        print("make.py [target_name] (--help) (--dump)")
        return

    filename = args[0]
    if filename == 'clean':
        from comake.target import clean
        clean()
    else:
        from comake.target import create_target_tree, statistics
        from comake.target import TargetNotFoundError, BuildCancelError
        from comake.tools import TargetNotBuildError
        print("Analyzing target tree...")
        try:
            target_tree = create_target_tree(filename)
        except BuildCancelError:
            sys.exit(3)
        except TargetNotFoundError:
            sys.exit(2)

        if '--dump' in args:
            target_tree.dump()
        else:
            print("Starting build")
            try:
                target_tree.build()
            except TargetNotBuildError:
                # Don't Traceback Python scripts for build errors
                print("Error: Target not build (TargetNotBuildError)")
                sys.exit(1)
            if statistics.made_targets == 0:
                print("No targets were rebuilt.")
            print("Total number of targets: %d" % (statistics.targets, ))
            if statistics.made_targets != 0:
                print("Targets rebuilt: %d" % (statistics.made_targets, ))
