savedcmd_vgpu.o := ld -m elf_x86_64 -z noexecstack --no-warn-rwx-segments   -r -o vgpu.o @vgpu.mod  ; /home/xuefeiz2/linux/tools/objtool/objtool --hacks=jump_label --hacks=noinstr --hacks=skylake --ibt --orc --retpoline --rethunk --static-call --uaccess --prefix=16  --link  --module vgpu.o

vgpu.o: $(wildcard /home/xuefeiz2/linux/tools/objtool/objtool)
