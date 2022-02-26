#!/usr/bin/python
import struct
import time
import sys

infile_path = "/dev/input/event1"

def read_coords(dev):
	FORMAT = 'llHHI'
	EVENT_SIZE = struct.calcsize(FORMAT)
	event = dev.read(EVENT_SIZE)
	x = 0
	y = 0
	sample_count = 0
	touched = True

	while event and touched:
		(sec, usec, type, code, value) = struct.unpack(FORMAT, event)
		if type == 3: # coordinates
			sample_count += 1
			if code == 0:
				x = value
			if code == 1:
				y = value
			print ("X %u Y %u" % (x, y))
		if type == 1: # pen up / down
			if code == 330 and value == 0:
				print("Release")
				if sample_count > 50:
					touched = False
				else:
					print("Again please...")
			if code == 330 and value == 1:
				print("Touch")

		event = dev.read(EVENT_SIZE)
	# end of while
	return x,y

def main():
	print("Jornada 720 screen calibration tool")
	print("Touch (and keep touching) the upper right corner of the screen")
	raw_input("Press Enter when ready...")
	in_file = open(infile_path, "rb")
	ur = read_coords(in_file)

	print("Touch (and keep touching) the lower left corner of the screen")
	raw_input("Press Enter when ready...")
	ll = read_coords(in_file)
	in_file.close()

	print ur
	print ll

	# Calculate calibration data
	(ur_x, ur_y) = ur
	(ll_x, ll_y) = ll

	mx = 640.0 / (ur_x - ll_x)
	my = 240.0 / (ll_y - ur_y)

	dx = min(ur_x, ll_x) * mx * -1.0
	dy = min(ur_y, ll_y) * my * -1.0

	print("Calibration values mx=%f dx=%u, my=%f dy=%u" % (mx, dx, my, dy));
	print("module parameters: mx=%u dx=%u, my=%u dy=%u" % (mx*1024, dx, my*1024, dy));

	# write a file with the modprobe call
	f = open("loadtsmod.sh","w")
	f.write("rmmod jornada720_ts\n")
	f.write("modprobe jornada720_ts mx=%u dx=%u my=%u dy=%u\n" % (mx*1024, dx, my*1024, dy));
	f.close();

# Main program entry point.
if __name__ == "__main__":
	main()

print("End.")
