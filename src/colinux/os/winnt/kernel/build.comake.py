targets['build.o'] = Target(
    inputs=
    input_list(".c", ".o") +
    [Input("lowlevel/build.o")],
)
