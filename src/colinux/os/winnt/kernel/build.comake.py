from os import getenv

# conet.c needs ddk/ndis.h, which does not compile under current mingw-w64 (see
# conet-stub.c). Build exactly one of the two.
if getenv('COLINUX_ENABLE_NDIS') == "yes":
    excluded = 'conet-stub.o'
else:
    excluded = 'conet.o'

objects = [obj for obj in input_list(".c", ".o") if obj.name != excluded]

targets['build.o'] = Target(
    inputs =
    objects +
    [Input("lowlevel/build.o")],
)
