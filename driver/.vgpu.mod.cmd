savedcmd_vgpu.mod := printf '%s\n'   vgpu_drv.o vgpu_engine.o | awk '!x[$$0]++ { print("./"$$0) }' > vgpu.mod
