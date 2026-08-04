targets['build.o'] = Target(
    inputs=[
       Input('cogpu-daemon.o'),
       Input('vring.o'),
    ],
)
