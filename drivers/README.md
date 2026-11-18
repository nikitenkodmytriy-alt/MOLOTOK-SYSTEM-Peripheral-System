# MSPC Drivers

Driver code is compiled into the MSPC kernel from this directory. The `.drv` manifests are packed into the kernel image and scanned at boot. A manifest can enable a driver already registered by the kernel; it cannot load arbitrary executable code.

Current built-ins:

- `vga-text.c`: VGA text output.
- `ps2_keyboard.c`: polled PS/2 keyboard input.
- `pcnet.c`: polling driver for the Am79C973/PCnet-FAST III PCI adapter used by VirtualBox.
- `net_stack.c`: DHCP, ARP, IPv4, DNS A records, ICMP echo, and plain HTTP/TCP download.
- `mspc_gl.c`: software MSPC GL compatibility subset and 3D wireframe test.

To add a driver, add its implementation, declare its init function in `driver.h`, register the manifest name and init function in the kernel driver registry, and add an enabled `.drv` manifest. Rebuild the ISO or USB image so the implementation and manifest are included.