# Last-Level Cache Side-Channel Attacks Are Feasible in the Modern Public Cloud

This repo contains implementations of the paper: **Last-Level Cache Side-Channel Attacks Are Feasible in the Modern Public Cloud**.
In this paper, we demonstrated various techniques to perform an end-to-end, cross-tenant LLC Prime+Probe attack on Google Cloud Run.
These techniques include:
- Faster and noise-resilient eviction-set construction algorithms
- A high-resolution primitive that monitors victim's memory accesses
- Detecting the target cache set of interest in the frequency domain

You can clone the repo by running:
```bash
git clone --recursive https://github.com/zzrcxb/LLCFeasible.git
```

## Dependences

### Common C Toolchain
This repo requires `CMake`, `gcc`, and `GNU make` or `ninja-build`.
Your system likely has them already.

### Hardware
We tested our implementations mostly on Intel Skylake-SP and Ice Lake-SP microarchitectures.
Therefore, we recommend trying our implementations on these two microarchitectures.
Porting our implementation to other microarchitectures may require
changing the source code.

### Kernel Module (Optional)
Some programs depend on [PTEditor](https://github.com/misc0110/PTEditor),
which is a kernel module that helps page-table manipulation in user space.
Therefore, a kernel module build environment is required.
Note that those programs only use PTEditor to output debug information,
you don't need to install PTEditor for their core functionalities.

## Build
### This Repo
Under the project's root directory,
execute:
```bash
mkdir build && cd build && cmake ..
```
After that, under the build directory, execute command:
`make` or `ninja` depending on your build system.

Please refer to this [README](src/README.md) for more details
on each program.


### PTEditor (Optional)
Under the `extern/PTEditor` directory, execute
`make`
to build the kernel module.
Then load the module by executing:
```bash
sudo insmod module/pteditor.ko
```
