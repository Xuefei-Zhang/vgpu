savedcmd_vgpu.ko := ld -r -m elf_x86_64 -z noexecstack --no-warn-rwx-segments --build-id=sha1  -T /home/xuefeiz2/linux/scripts/module.lds -o vgpu.ko vgpu.o vgpu.mod.o .module-common.o
