from PIL import Image, ImageDraw, ImageEnhance
import serial
import serial.tools.list_ports
import base64
import numpy as np
import io
import time
from flask import request
from flask import jsonify
from flask import Flask
from flask import send_file
from flask import send_from_directory
import flask
import sys


app = Flask(__name__)
sys.setrecursionlimit(10000)

@app.route('/')
def home():
	return 'python flask server up and running'


@app.route('/generate', methods=['GET', 'POST'])
def generate():
	if flask.request.method == 'POST':
		if flask.request.files.get('image'):
			image = flask.request.files['image'].read()
			image = Image.open(io.BytesIO(image))
	# 		image = preprocess_image(image, target_size=(256, 256))
	# 		preds = model.predict(image)
	# 		epoch_time = int(time.time())
	# 		outputfile = 'output_%s.png' % (epoch_time)
	# 		print(' * SHAPE IS : ' + str(preds.shape))
	#
	# 		output = tf.reshape(preds, [256, 256, 3])
	# 		output = (output + 1) / 2
	# 		save_img(outputfile, img_to_array(output))
	#
	# 		response = {'result': outputfile}
			time_stamp = int(time.time())
			locally_copy_of_image_path=f"pic_{time_stamp}.png"
			image.save(locally_copy_of_image_path ,format="png")
			paths = imToPaths(locally_copy_of_image_path)
			outfile = locally_copy_of_image_path[:-3]+"gcode"
			toTextFile(outfile, paths)
			g_code = None
			with open(outfile, 'r') as f:
				g_code = f.read()
			g_code = g_code_allignment(g_code)
			response = {'result': g_code}
	return jsonify(response)


@app.route('/download/<fname>', methods=['GET'])
def download(fname):
	return send_file(fname)


# Global constants
IMDIM = 305         # Pixels in each dimension of image, 305 mm in 12 inches
SMOOTHERR = 1		# Rounding error when approx. raster with straight lines

# Global variables
done = []           # Record of all coordinates that were read from the image
direc = 0           # Current direction for tracing algorithm
                    # (0 = right, 1 = up, 2 = left, 3 = down)


def initRaster(filename):
	'''
    Return the raster image represented by the file name. The file must be
    in the local folder.

    Arguments:
        filename is of type string. Contains name of image file.
    '''

	global IMDIM
	im = Image.open(filename)

	# Increase contrast of image
	im = ImageEnhance.Contrast(im)
	im = im.enhance(2)

	# Resize to imdim x imdim
	im = im.resize((IMDIM, IMDIM))

	return im


def toTextFile(outfile, shapes):
	'''
    Print the coordinates to a text file formatted in G code.

    Arguments:
        shapes is of type list. It contains sublists of tuples that correspond
                                to (x, y) coordinates.
    '''

	file = open(outfile, "w")

	# Boilerplate text:
	# G17: Select X, Y plane
	# G21: Units in millimetres
	# G90: Absolute distances
	# G54: Coordinate system 1
	file.write("G17 G21 G90 G54\n")

	# Start at origin (0, 0)
	file.write("G00 X0. Y0.\n")

	up = True

	# Assume Z0 is down and cutting and Z1 is retracted up
	for shape in shapes:
		for i in range(len(shape)):
			# If coordinate is an integer, append a decimal point
			if (shape[i][0] % 1.0 == 0):
				xstr = str(shape[i][0]) + "."
			else:
				xstr = str(shape[i][0])

			if (shape[i][1] % 1.0 == 0):
				ystr = str(shape[i][1]) + "."
			else:
				ystr = str(shape[i][1])

			# Write coordinate to file
			file.write("X" + xstr + " Y" + ystr + "\n")

			# When arrived at point of new shape, start cutting
			if up == True:
				file.write("Z0.\n")
				up = False
		# When finished shape, retract cutter
		file.write("Z1.\n")
		up = True
	# Return to origin (0, 0) when done, then end program with M2
	file.write("X0. Y0.\nM2")

	file.close()


def toSerial(shapes):
	'''
    Send the coordinates formatted in G code through serial to Arduino.

    Arguments:
        shapes is of type list. It contains sublists of tuples that correspond
                                to (x, y) coordinates.
    '''

	# First, search COM ports for a connected Arduino
	found = False

	portlist = list(serial.tools.list_ports.comports())

	for tempport in portlist:
		if tempport[1].startswith("Arduino"):
			port = serial.Serial(tempport[0])
			found = True

	# If no Arduino is found, return False
	if not found:
		return False

	# Arduino restarts when serial is initialized, so wait until it's ready
	time.sleep(5)

	port.baudrate = 4800

	# Boilerplate text:
	# G17: Select X, Y plane
	# G21: Units in millimetres
	# G90: Absolute distances
	# G54: Coordinate system 1
	port.write("G17 G21 G90 G54\n".encode())

	# Start at origin (0, 0)
	port.write("G00 X0. Y0.\n".encode())

	up = True

	# Assume Z0 is down and cutting and Z1 is retracted up
	for shape in shapes:
		for i in range(len(shape)):
			# Since the RAM on the Arduino is limited, delay the instructions
			time.sleep(1)

			# If coordinate is an integer, append a decimal point
			if (shape[i][0] % 1.0 == 0):
				xstr = str(shape[i][0]) + "."
			else:
				xstr = str(shape[i][0])

			if (shape[i][1] % 1.0 == 0):
				ystr = str(shape[i][1]) + "."
			else:
				ystr = str(shape[i][1])

			# Write coordinate to serial
			port.write(("X" + xstr + " Y" + ystr + "\n").encode())

			# When arrived at point of new shape, start cutting
			if up == True:
				port.write(("Z0.\n").encode())
				up = False
		# When finished shape, retract cutter
		port.write("Z1.\n".encode())
		up = True
	# Return to origin (0, 0) when done, then end program with M2
	port.write("X0. Y0.\nM2".encode())

	port.close()

	# Once sending is completed, return True
	return True


def initDXF(filename):
	'''
    Return the DXF text file represented by the file name. The file must be
    in the local folder.

    Arguments:
        filename is of type string. Contains name of image file.
    '''

	file = open(filename)

	# Since dxf is a fancy extension for a text file, it can be read as a string
	DXFtxt = file.readlines()

	file.close()

	return DXFtxt


def readFromRaster(filename):
	'''
    Return a list of sublists of tuples which correspond to (x, y) coordinates.
    Read the coordinates from a raster image, tracing the outline of each shape
    in the image.

    Arguments:
        filename is of type string. Contains name of image file.
    '''

	global done

	# Create Image object from file in local folder
	im = initRaster(filename)

	# Find first point
	point = nextShape(im)

	start = point
	nextpoint = (0, 0)

	i = 0
	shapeList = []

	# While there are still shapes in the image
	while point != (-1, -1):
		start = point

		shapeList.append([])
		shapeList[i].append(point)

		# While it has not yet fully traced around the image
		while nextpoint != start:
			nextpoint = nextPixelInShape(im, point)
			point = nextpoint

			done.append(point)
			shapeList[i].append(point)

		i += 1
		point = nextShape(im)

	# Smooth coordinates in image
	shapeList = smoothRasterCoords(shapeList)

	# Ensure that each shape starts and ends on the same coordinate
	for i in range(len(shapeList)):
		if shapeList[i][-1] != shapeList[i][0]:
			shapeList[i].append(shapeList[i][0])

	return shapeList


def readFromDXF(filename):
	'''
    Return a list of sublists of tuples which correspond to (x, y) coordinates.
    Read the coordinates from a DXF file, treating it as plaintext.

    Arguments:
        filename is of type string. Contains name of image file.
    '''

	# Create Image object from file in local folder
	DXFtxt = initDXF(filename)

	segment = -1

	path = []
	xold = []
	yold = []

	line = 0
	polyline = 0
	vertex = 0

	# While there is still more to read
	while line < len(DXFtxt):
		# These are just conditions how to interpret the DXF into coordinates
		if (DXFtxt[line] == "POLYLINE\n"):
			segment += 1
			polyline = 1
			path.append([])

		elif (DXFtxt[line] == "VERTEX\n"):
			vertex = 1

		elif ((DXFtxt[line].strip() == "10") & (vertex == 1) & (polyline == 1)):
			line += 1
			x = float(DXFtxt[line])

		elif ((DXFtxt[line].strip() == "20") & (vertex == 1) & (polyline == 1)):
			line += 1
			y = float(DXFtxt[line])

			if ((x != xold) | (y != yold)):
				path[segment].append([float(x), float(y)])
				xold = x
				yold = y

		elif (DXFtxt[line] == "SEQEND\n"):
			polyline = 0
			vertex = 0

		line += 1

	# Rescale the coordinates to imdim x imdim
	scale(path)

	return path


def imToPaths(filename):
	'''
    Return a list of paths, where each path is a sublist of tuples which contain
    (x, y) coordinates. Read the coordinates from any file type supported by the
    package.

    Arguments:
        filename is of type string. Contains name of image file.
    '''

	# Read the image as raster or as DXF depending on file extension
	if filename.endswith((".jpg", ".jpeg", ".png", ".bmp")):
		coords = readFromRaster(filename)
	elif filename.endswith(".dxf"):
		coords = readFromDXF(filename)
	else:
		coords = [[(0, 0)]]

	return coords


def scale(path):
	'''
    DXF files have the coordinates prewritten into it, which means they may
    be the wrong dimension. Scale the coordinates read from the DXF to IMDIM.

    Arguments:
        path is of type list. Contains sublists of tuples, where each tuple is
                              an (x, y) coordinate.
    '''

	global IMDIM

	# Create lists of only x coordinates and only y coordinates
	x = []
	y = []

	for shape in path:
		for coord in shape:
			x.append(coord[0])
			y.append(coord[1])

	# To scale from the old size to imdim, must know the old size
	maxx = max(x)
	maxy = max(y)

	minx = min(x)
	miny = min(y)

	# The distance between the minimal coordinate and the edge is the margin,
	# assumed size is the maximal coordinate plus the margin
	margin = min(minx, miny)
	size = max(maxx, maxy) + margin
	scale = IMDIM / size

	# Once the old size is known, scale the coordinates
	for i in range(len(path)):
		for j in range(len(path[i])):
			path[i][j][0] *= scale
			path[i][j][1] *= scale


def smoothRasterCoords(coords):
	'''
    Return newCoords, a coordinate list without the excessive elements of
    the argument, coords. The coordinates read from raster images would be
    jagged, so segments can be simplified by removing intermediate coordinates.

    Basically, the function does this to the points read from raster images:

        -|                 \
         |-|      --->      \        AND     o--o--o--o   --->   o--------o
           |-                \

    Arguments:
        coords is of type list. Contains sublists of tuples, where each tuple is
                                an (x, y) coordinate.
    '''

	newCoords = []

	# For each shape in coords
	for s in range(len(coords)):
		newCoords.append([])

		# If it's a simple shape without removable elements, copy it and skip to
		# the next one
		if len(coords[s]) <= 2:
			newCoords[s] = coords[s]
			continue

		i = 0

		# For each point i in the coordinate list
		while i < len(coords[s]) - 2:
			j = len(coords[s]) - 1

			# For each point j between i and the end of the list
			while j > i + 1:
				# List of all points between i and j
				midpoints = coords[s][i + 1:j]

				# In usual case, draw line between i and j
				try:
					m = (coords[s][j][1] - coords[s][i][1]) / \
						(coords[s][j][0] - coords[s][i][0])
					b = coords[s][i][1] - m * coords[s][i][0]

					canDel = True

					for point in midpoints:
						if linePointDist(m, b, point) >= SMOOTHERR:
							canDel = False
							break

					# If all points between i and j are within SMOOTHERR of
					# the line, remove them
					if canDel == True:
						newCoords[s].append(coords[s][i])
						newCoords[s].append(coords[s][j])
						i = j

				# In special case where the line is vertical, m = infinity
				except ZeroDivisionError:
					canDel = True

					for point in midpoints:
						if abs(coords[s][i][0] - point[0]) >= SMOOTHERR:
							canDel = False
							break

					# If all points between i and j are within SMOOTHERR of
					# the line, remove them
					if canDel == True:
						newCoords[s].append(coords[s][i])
						newCoords[s].append(coords[s][j])
						i = j
				j -= 1
			i += 1

	# If a shape did not have removable points, copy the original coordinates
	for i in range(len(newCoords)):
		if len(newCoords[i]) == 0: newCoords[i] = coords[i]

	return newCoords


def isOnEdge(im, px):
	'''
    Return a boolean value that corresponds to whether any of the adjacent
    coordinates are not in the shape, hence whether the current coordinate is on
    the edge.

    Arguments:
        im is of type Image. Contains the image which is being processed.
        px is of type tuple. Contains float elements (x, y) which represent the
                             coordinate being checked.
    '''

	hues = []

	# Literal edge cases
	try:
		hues.append(sum(im.getpixel((px[0] - 1, px[1]))))
	except IndexError:
		hues.append(sum((255, 255, 255)))

	try:
		hues.append(sum(im.getpixel((px[0], px[1] - 1))))
	except IndexError:
		hues.append(sum((255, 255, 255)))

	try:
		hues.append(sum(im.getpixel((px[0] + 1, px[1]))))
	except IndexError:
		hues.append(sum((255, 255, 255)))

	try:
		hues.append(sum(im.getpixel((px[0], px[1] + 1))))
	except IndexError:
		hues.append(sum((255, 255, 255)))

	if (max(hues) > sum((127, 127, 127))):
		return True
	else:
		return False


def nextPixelInShape(im, px):
	'''
    Return a tuple which represents the next coordinate when proceeding
    clockwise around a shape. It is an implementation of the square tracing
    algorithm, which works as follows:
        if on a black square, turn left of previous direction and go forward
        if on a white square, turn right of previous direction and go forward

    Arguments:
        im is of type Image. Contains the image which is being processed.
        px is of type tuple. Contains float elements (x, y) which represent the
                             current coordinate.
    '''

	global direc, done

	pixels = im.load()

	try:
		pixel = pixel = sum(im.getpixel((px[0], px[1])))
	except IndexError:
		pixel = sum((255, 255, 255))

	# 0 = right, 1 = up, 2 = left, 3 = down
	if pixel < sum((127, 127, 127)):
		direc = (direc - 1) % 4
	else:
		direc = (direc + 1) % 4

	# Implementation of description in docstring
	if direc == 0:
		x = px[0] + 1
		y = px[1]
	elif direc == 1:
		x = px[0]
		y = px[1] + 1
	elif direc == 2:
		x = px[0] - 1
		y = px[1]
	elif direc == 3:
		x = px[0]
		y = px[1] - 1

	try:
		pixel = sum(im.getpixel((x, y)))
	except IndexError:
		pixel = sum((255, 255, 255))

	# Since this function returns the next pixel, it can't return an off-shape
	# white pixel. It recurses until it finds a black pixel, which it returns.
	if pixel < sum((127, 127, 127)):
		if (x, y) not in done:
			done.append((x, y))
		return (x, y)
	else:
		return nextPixelInShape(im, (x, y))


def nextShape(im):
	'''
    Return a tuple which represents the leftmost point of the next shape.

    Arguments:
        im is of type Image. Contains the image which is being processed.
    '''

	global IMDIM, done

	# Check the brightness of every point in the image
	for x in range(IMDIM):
		for y in range(IMDIM):

			# If a dark pixel is found that was not already read, return it
			if sum(im.getpixel((x, y))) < sum((127, 127, 127)) and \
					isOnEdge(im, (x, y)) and not (x, y) in done:
				done.append((x, y))
				return (x, y)

	# If no shape is found, return a special tuple
	return (-1, -1)


def dist(a, b):
	'''
    Return the Pythagorean distance between two points.

    Arguments:
        a, b are of type tuple. They represent (x, y) coordinates.
    '''

	return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def linePointDist(m, b, p):
	'''
    Return the distance between a point and a line.

    Arguments:
        m, b are of type float. Represent slope and y-intercept of line.
        p is of type tuple. Represents (x, y) coordinates.
    '''

	n = (m * p[0] - p[1] + b)
	d = (m ** 2 + 1) ** 0.5
	return abs(n / d)


def g_code_allignment(g_code):
	pen_up = True
	lines = g_code.split('\n')
	new_g_code = ['G00 X0. Y0.']
	for line in lines:
		if line.startswith('Z0'):
			pen_up = False
			continue
		elif line.startswith('Z1'):
			pen_up = True
			continue
		elif line.startswith('X'):
			prefix = 'G00 ' if pen_up else 'G01 '
			new_line = prefix + line
			new_g_code += [new_line]
		else:
			continue
	new_g_code += ["G00 X0. Y0.\n"]
	new_g_code = '\n'.join(new_g_code)
	return new_g_code

# export FLASK_APP="main.py"
# export FLASK_DEV="development"
# flask run -h 192.168.1.101

