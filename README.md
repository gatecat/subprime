# subprime - GLX implementation using EGL Pbuffers & XImage for NVIDIA PRIME on Wayland

subprime is currently an early proof-of-concept that offscreen renders to an EGL PBuffer and then software blits using the XImage APIs, to enable NVIDIA PRIME accelerated GLX applications to work with Wayland running on the Intel iGPU.

It implements the GLX vendor API, and calls into an EGL vendor implementation.

Currently it supports a small subset of the GLX API, just about enough to run Minecraft and glxgears. There are some significant perfomance and memory leak issues still to be resolved, you probably don't want to be using this just yet.

To build it, run `make`. You will need the glvnd, EGL and GLX development headers available.

Then make sure your `LD_LIBRARY_PATH` contains the subprime build folder, and add `__GLX_VENDOR_LIBRARY_NAME=subprime` to the environment of the command you are running (also make sure the NVIDIA GPU is powered on if automatic power management isn't set up.)

