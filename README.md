# jornada-kernelpatches_3.16
Kernel patches for Linux 3.16 on Jornada 720 

Some patches to the Linux kernel for the HP Jornada 720. The files in here are supposed to be added to an existing kernel folder. 
Contents:
- Epsonpatch: Added the hardware imageblit function to the framebuffer driver so that in 16bit color mode, the copying of images from memory to the screen is hardware accelerated:
-- ./include/video/s1d13xxxfb.h
-- ./drivers/video/fbdev/s1d13xxxfb.c
- sound/arm/jornada720.c - Sounddriver for J720, working PCM playback
