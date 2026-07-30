targets['build.o'] = Target(
    inputs = input_list(".c", ".o"),
)

targets['Makefile.lib-m'] = Target(
    inputs = input_list(".c", ".c"),
    tool = MakefileKbuild(),
)
