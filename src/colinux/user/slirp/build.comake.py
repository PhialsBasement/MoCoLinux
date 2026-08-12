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
            #
            # FD_SETSIZE, because 64 is not a number of connections.
            #
            # Winsock's fd_set is a fixed array of FD_SETSIZE sockets and its
            # FD_SET macro SILENTLY DOES NOTHING once that is full:
            #
            #   if (((fd_set *)(set))->fd_count < FD_SETSIZE) { ...add... }
            #
            # slirp calls FD_SET for every socket it holds, unguarded, so past
            # the 64th the socket is simply never handed to select(), never
            # polled, and never read from. Its bytes stop moving while the
            # connection stays open, which the far end eventually resets.
            #
            # A shell and a package install never came close. Steam does: it
            # opens a hundred parallel connections for a download, and this box
            # measured CurrEstab=108 with EstabResets=72 and -- the tell --
            # zero retransmits, zero TCP timeouts and zero errors in the guest.
            # Nothing was lost or corrupted on the wire; two thirds of the
            # sockets were just never serviced. Downloads therefore worked or
            # died depending on how many connections happened to be open, which
            # is exactly how it was reported.
            #
            # Defined on the command line so it is set before winsock2.h is
            # included in every file here, which is what Winsock requires. It
            # must stay uniform across these translation units: an fd_set built
            # by one size and read by another is a buffer overrun, not a
            # mismatch. Nothing outside this directory shares one -- co_main.c
            # declares them on its stack and passes them only to slirp's own
            # fill/poll and to select().
            compiler_flags=['-fgnu89-inline', '-DFD_SETSIZE=1024'],
        )
    ),
)
