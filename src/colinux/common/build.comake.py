targets['common.a'] = Target(
    inputs=[
    Input('queue.o'),
    Input('console.o'),
    Input('debug.o'),
    Input('errors.o'),
    Input('messages.o'),
    Input('module_repr.o'),
    Input('libc.o'),
    Input('snprintf.o'),
    Input('file_ids.o'),
    Input('unicode.o'),
    ],
)

targets['file_ids.o'] = Target(
    tool = Compiler(),
    inputs = [Input('file_ids.c')],
)

class FileIdScript(Tool):
    def cache(self, target):
        return ''

    def _content(self):
        from io import StringIO
        from comake.defaults import file_id_allocator
        output_file = StringIO()
        print("#include <stdlib.h>", file=output_file)
        print("const char *colinux_obj_filenames[] = {", file=output_file)
        for filename in file_id_allocator.path_list:
            print('     "%s", ' % (filename, ), file=output_file)
        print(' NULL,', file=output_file)
        print("};", file=output_file)
        return output_file.getvalue()

    def make(self, target, reporter):
        with open(target.pathname, 'w') as output_file:
            output_file.write(self._content())

    def rebuild_needed(self, target):
        return open(target.pathname, 'r').read() != self._content()

targets['file_ids.c'] = Target(
    tool = FileIdScript(),
)

def version_inputs(scripter, tool_run_inf):
    command_line = 'python %s %s %s' % (
        tool_run_inf.target.inputs[0].pathname,
        tool_run_inf.target.inputs[1].pathname,
        tool_run_inf.target.pathname)
    return command_line

targets['version.h'] = Target(
    tool = Script(version_inputs),
    inputs = [
       Input('version.py'),
       Input('../VERSION'),
    ]
)

targets['Makefile.lib-m'] = Target(
    inputs = input_list(".c", ".c"),
    tool = MakefileKbuild(),
)
