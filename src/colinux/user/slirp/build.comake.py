targets['build.o'] = Target(
    inputs=input_list(".c", ".o"),
    options=Options(
        appenders=dict(
            # slirp is vendored code written against GNU89 inline semantics: it
            # defines insque()/remque() and friends as bare "inline" in a .c
            # file and calls them from other translation units. Under C99 rules
            # a bare inline definition emits no external symbol, so those calls
            # end up undefined at link time. slirp_config.h's "#define inline
            # inline" is from the same era.
            compiler_flags=['-fgnu89-inline'],
        )
    ),
)
