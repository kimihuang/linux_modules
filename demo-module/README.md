# Demo Kernel Module

## Description
A simple example kernel module demonstrating Buildroot external package infrastructure for Quantum Board.

## Features
- Creates a procfs interface at `/proc/demo_module`
- Supports read and write operations
- Simple demonstration of kernel module development

## Usage

### Load the module:
```bash
insmod demo_module.ko
```

### Read from proc:
```bash
cat /proc/demo_module
```

### Write to proc:
```bash
echo "Hello Quantum Board" > /proc/demo_module
```

### Unload the module:
```bash
rmmod demo_module
```

## Building with Buildroot
This package is automatically available in Buildroot menuconfig under:
```
Board options -> Quantum Board Packages -> demo-module
```

## License
GPL-2.0
