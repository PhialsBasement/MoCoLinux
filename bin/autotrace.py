#!/usr/bin/env python
#
# Dan Aloni (c), 2003
#
# I use this script in order to automatically add trace macros
# to C functions inside coLinux so I'd be able to debug it better.
#
# It is not active by default, since it slows down the compilation by
# a factor of 4.
#
# Optionally, we could improve this to be able to output a complete
# trace of the kernel (not including inline functions).
#

import re, os, sys

class Error(Exception): pass

# Reduced tokenizing regular expression

c_identifier = r'(?P<identifier>[a-zA-Z_][a-zA-Z_0-9]*)'
c_start_comment = r'(?P<start_comment>\/\*)'
c_preprocessor = r'(?P<preprocessor>#)'
c_space = r'(?P<space>[ \t\n])'
c_string = r'(?P<string>")'
c_character = r'(?P<character>(\'\\[0-9]+\')|\'\\\'\'|\'.\'|\'\'\'\'|\'\\.\')'
c_number = r'(?P<number>(0x)?[0-9]+)'
c_open_bracet = r'(?P<open_bracet>[({\[])'
c_close_bracet = r'(?P<close_bracet>[)}\]])'
c_operators = r'(?P<operators>\>=|\<\<|\>\>|\|=|--|\+\+|-=|!=|->|' + \
r'[+]=|[-?^|.%:/&*,=;+<~!>])'

c_token = re.compile(
    '(' + '|'.join(
    [c_identifier,
     c_start_comment,
     c_preprocessor,
     c_space,
     c_string,
     c_character,
     c_number,
     c_open_bracet,
     c_close_bracet,
     c_operators,
     ]) + ')'
)

class CParser(object):
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def run(self):
        self.parse_c()

    def token(self, token, type=None):
        print([token, type])

    def parse_c_comment(self):
        pos = self.data.find('*/', self.pos)
        if pos == -1:
            raise Error('Unterminated comment')

        self.token(self.data[self.pos-2:pos+2], type='comment')
        self.pos = pos + 2

    def parse_c_string(self):
        pos = self.pos
        while 1:
            if self.data[pos] == '\\':
                pos += 2
                continue

            if self.data[pos] != '"':
                pos += 1
                continue

            if pos >= len(self.data):
                raise Error('Unterminated string')

            break

        self.token(self.data[self.pos-1:pos+1], type='string')
        self.pos = pos + 1

    def parse_c_preprocessor(self):
        pos = self.pos
        while 1:
            pos = self.data.find('\n', pos)
            if pos == -1:
                raise Error('Unterminated macro')
            if self.data[pos-1] == '\\':
                pos = pos+1
            else:
                break
        self.token(self.data[self.pos-1:pos+1], type='preprocessor')
        self.pos = pos + 1

    def error(self):
        pass

    def parse_c(self):
        while self.pos < len(self.data):
            m = c_token.match(self.data, self.pos)
            if not m:
                self.error()
                raise Error("Parsing at %r" % (self.data[self.pos:self.pos+50], ))
            self.pos = m.end()
            raw = m.groups(0)[0]
            dict = m.groupdict()
            if dict['start_comment']:
                self.parse_c_comment()
            elif dict['string']:
                self.parse_c_string()
            elif dict['preprocessor']:
                self.parse_c_preprocessor()
            elif dict['identifier']:
                self.token(raw, 'identifier')
            elif dict['space']:
                self.token(raw, 'space')
            elif dict['open_bracet']:
                self.token(raw, 'open_bracet')
            elif dict['close_bracet']:
                self.token(raw, 'close_bracet')
            else:
                self.token(raw)

class CTracer(CParser):
    def __init__(self, data, outfile):
        super(CTracer, self).__init__(data)
        self.bracet_level = 0
        self.last_tokens = []
        self.last_tokens_types = []
        self.last_level1_token = None
        self.bracet = []
        self.sub_tokens = []
        self.sub_tokens_with_return_trace = []
        self.in_return = False
        self.in_return_skip_space = False
        self.return_tokens = []
        self.params_list = []
        self.outfile = outfile
        self.func_name = None
        self.activated = True
        self.stop_count = 0
        self.debug_tokens = []

        self.outfile.write('extern void co_trace_ent(void *);\n');
        self.outfile.write('extern void co_trace_ent_name(void *, const char *);\n');
        self.outfile.write('extern void co_trace_ret(void *);\n');
        self.outfile.write(
"""
#define CO_TRACE_RET_VALUE(_value_) ({ \
        typeof(_value_) __ret_value__; \
        co_trace_ret((void *)&TRACE_FUNCTION_NAME); \
        __ret_value__ = _value_; \
})

#define CO_TRACE_RET { \
        co_trace_ret((void *)&TRACE_FUNCTION_NAME); \
}

""")


    def error(self):
        for debug in self.debug_tokens:
            print(debug)

    def token(self, token, type=None):
        self.debug_tokens.append((token, type))
        if len(self.debug_tokens) > 40:
            del self.debug_tokens[0]

        if type == 'close_bracet':
            self.bracet_level -= 1
            del self.bracet[0]

        if self.bracet_level == 0:
            if type != 'space':
                self.last_tokens.append(token)
                self.last_tokens_types.append(type)
                if len(self.last_tokens) > 40:
                    del self.last_tokens[0]
                if len(self.last_tokens_types) > 40:
                    del self.last_tokens_types[0]
                if self.last_tokens[-4:] == ['(', ')', '{', '}']:
                    if self.last_tokens_types[-5] == 'identifier':
                        pos = 6
                        self.func_name = self.last_tokens[-5]
                        while pos <= len(self.last_tokens):
                            if self.last_tokens_types[-pos] == 'close_bracet':
                                break
                            if self.last_tokens[-pos] in ['inline', '__inline__']:
                                self.func_name = None
                                break
                            pos += 1

        elif self.bracet_level >= 1:
            if self.bracet_level == 1:
                if self.bracet[0] == '(':
                    if token == ',':
                        if self.last_level1_token:
                            self.params_list.append(self.last_level1_token)
                    if type == 'identifier':
                        self.last_level1_token = token
                    else:
                        self.last_level1_token = None

            if self.in_return:
                no_sub = False
                if self.in_return_skip_space:
                    if type == 'space':
                        self.sub_tokens_with_return_trace.append(token)
                        self.sub_tokens.append(token)
                        no_sub  = True
                    else:
                        self.in_return_skip_space = False

                if not no_sub:
                    if token == ';' and self.in_return_bracet_level == self.bracet_level:
                        self.sub_tokens_with_return_trace += ['do {CO_TRACE_RET; return '] + self.return_tokens + ['; } while(0)']
                        #    self.sub_tokens_with_return_trace += ['return CO_TRACE_RET_VALUE('] + self.return_tokens + [')']
                        self.sub_tokens += self.return_tokens
                        self.return_tokens = []
                        self.in_return = False
                        self.sub_tokens_with_return_trace.append(token)
                        self.sub_tokens.append(token)
                    else:
                        self.return_tokens.append(token)
            else:
                if not token == 'return':
                    self.sub_tokens_with_return_trace.append(token)
                self.sub_tokens.append(token)

            if token == 'return':
                self.in_return = True
                self.in_return_skip_space = True
                self.in_return_bracet_level = self.bracet_level;

        if type == 'close_bracet':
            if self.bracet_level == 0 :
                if token == '}' and self.activated and self.func_name != None:
                    params_str = ''
                    #params_str = ' '.join(['TRACE_X(%s)' % (x, ) for x in self.params_list])
                    self.outfile.write('\n#define TRACE_FUNCTION_NAME %s\n' % (self.func_name, ))
                    self.outfile.write(' co_trace_ent((void *)&%s); {' % (self.func_name, ))
                    self.outfile.write(''.join(self.sub_tokens_with_return_trace))
                    self.outfile.write('}; CO_TRACE_RET; ')
                    self.outfile.write('\n#undef TRACE_FUNCTION_NAME\n')
                else:
                    self.outfile.write(''.join(self.sub_tokens))
                self.func_name = None

                if token == ')':
                    if self.last_level1_token:
                        self.params_list.append(self.last_level1_token)

        if self.bracet_level == 0:
            skip_token = False
            if type == 'identifier':
                if token == 'CO_TRACE_STOP':
                    skip_token = True
                    self.stop_count += 1;
                    self.activated = False
                elif token == 'CO_TRACE_CONTINUE':
                    skip_token = True
                    self.stop_count -= 1;
                    if self.stop_count == 0:
                        self.activated = True

            if not skip_token:
                self.outfile.write(token)

        if type == 'open_bracet':
            self.bracet_level += 1
            self.bracet.append(token)
            if self.bracet_level == 1:
                self.sub_tokens = []
                self.sub_tokens_with_return_trace = []
                if token == '(':
                    self.params_list = []

if __name__ == '__main__':
    CTracer(open(sys.argv[1]).read(), sys.stdout).run()
