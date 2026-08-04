targets['build.o'] = Target(
    inputs=[
       Input('cogpu-daemon.o'),
       Input('vring.o'),
       Input('vrend.o'),
       Input('wgl_winsys.o'),
    ],
)
