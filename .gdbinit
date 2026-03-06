# Impossible OS — GDB Init File
# Auto-loaded when GDB starts in the project root

# Connect to QEMU debug port
target remote localhost:1234

# Load kernel symbols
symbol-file build/kernel.elf

# Set a breakpoint at the kernel entry point
break kernel_main

# Display useful info on each stop
display/i $eip
