# jornada-kernelpatches_3.16
Kernel patches for Linux 3.16 on Jornada 720 

Some patches to the Linux kernel for the HP Jornada 720. The files in here are supposed to be overlayed to an existing 3.16 kernel folder (i.e. git clone this repo and copy over, replacing existing files). 
Contents:
- Epsonpatch: Added the hardware imageblit function to the framebuffer driver so that in 16bit color mode, the copying of images from memory to the screen is hardware accelerated:
  - ./include/video/s1d13xxxfb.h
  - ./drivers/video/fbdev/s1d13xxxfb.c
- ./sound/arm/jornada720-xxx.c - Sounddriver for J720, working PCM playback for samplerates 8-41.1khz, Mixer controls
  - Bugs: 
    - fixed: 44.1kHz / 48kHz replay heavily "crackles" (this also depends on the player software, be sure to use a kernel with BX patching)
    - fixed: samplerate switching not working
  - New feature:
    - module parameter "rate_limit" can be used to specify a maximum hardware samplerate, ALSA will then reasample in software. Usage: `modprobe snd-jornada720 rate_limit=22050` Default if nothing specified is 48000
  - Not implemented yet: 
    - Audio recording. The hardware is capable of full duplex audio recording, this might be added later if there is demand for it.
  - Useful tools to install: Alsa Utils, MOC, MPG123 --> `apt install alsa-utils moc mpg123`
  - Also apt-install sdl-mixer libraries to enable sound in SDL apps
- cs-ide: Small patch to the PCMCIA IDE driver (cs-ide.c) to send a soft-reset to the CF card which should force it flush its write cache (if present and active - Transcend cards...) before powering off in order to minimize the chance of disk corruption.
