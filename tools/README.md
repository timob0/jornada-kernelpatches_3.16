Tools for the touchscreen driver.

Main calibration too: 
calibtsmod.sh - this will unload and reload the touchscreen module into uncalibrated state, then run the python program j720_calibrate.py. 
                This program will ask you to touch the lower left and upper right corners of the screen to calculate the calibration values.
                Once complete, it will write a `loadtsmod.sh` script that will load load the module with the caculated values.
testtsmod.sh  - this will run `loadtsmod.sh` and call j720_testscreen.py which will dump coordinates as a means of testing the calibration. 

Once good with the calibration result, just add the parameters that `calibtsmod.sh` showed to your modprobe.d configuration file to have the touchscreen
module calibrated on system startup.
