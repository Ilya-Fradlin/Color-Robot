/*
--------------------
Arduino CNC Plotter
--------------------
This plotter is not limited by your paper dimensions as all drawings can be 
scaled. 

The down-side is that this plotter is subject to cumulative errors as it always 
assumes that it is at co-ordinate X,Y whereas it could be at co-ordinate 
(X+error),(Y+error). The good news is that this error term can (theoretically) 
be eliminated by adjusting the CWR ratio (see below). In practice the 
overall accuracy is good enough for transcribing line art onto 
watercolour paper.

The plotter comprises a pen mounted midway between two stepper motors fitted 
with 65mm diameter wheels. Rotate both wheels in unison and the plotter draws 
a straight line. Counter-rotating the wheels causes the robot to pivot about 
the pen-tip.

The plotter containes an interpreter which recognizes the gcodes 
generated by Inkscape version 0.91 which outputs float values and ends each 
line with \r\n. The X,Y gcode co-ordinates are turned into distance and angular 
bearing. "Xon / Xoff" flow control has been implemented for sending files 
to Teraterm.exe.

The code for this interpreter is a variation of the demo code,
GcodeCNCDemo2Axis, found at https://github.com/MarginallyClever/gcodecncdemo. 

The (C)ircle to (W)heel-diameter (R)atio defines how many shaft revolutions are 
needed to rotate the robot through 360 degrees. If the wheel-to-wheel spacing
is 130mm, and each wheel has a diameter of 65mm, then each wheel will need to 
rotate twice for the robot to rotate 360 degrees. This equates to a CWR=130/65
(CWR=2).

SCALE calibrates the linear distance per step the robot moves. To obtain 
1mm per step using a 65mm diameter wheel and 4096 steps per shaft revolution 
the SCALE factor equals 4096/(PI*65)

RAM space (2K) is preserved by placing all "strings" into PROGMEM (32K)
using the Arduino F() macro. Ref: http://www.gammon.com.au/progmem.

The "MENU" contains a "C##.##" (CWR) option that eliminates the need to 
reprogram the robot each time you want to try a new CWR value. This reduces the 
number of times you need to program the robot and also reduces the wear-and-tear on 
the cables and connectors as you only need to unplug/plug the boards once.

The"MENU" also contains a custom "S##.##" (SIZE) option that eliminates the 
need to reprogram the robot each time you want to change your scale. Again this
reduces wear-and-tear.

----------
COPYRIGHT
----------
Arduino_CNC_Plotter.ino is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, eiher version 3 of the License, or
(at your option) any later version.

Arduino_CNC_Plotter.ino is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License. If 
not, see <http://www.gnu.org/licenses/>.

Code by lingib
Last update 17/03/2016 
*/

/***************************************************************************
 DECLARATIONS
 ***************************************************************************/
// motor definitions -------------------
#define MOTOR1 PORTB					//pins 8,9,10,11
#define MOTOR2 PORTC					//pins A0,A1,A2,A3
#define STEPS_PER_REV 4096				//steps for one motor shaft revolution
#define FORWARD true					//travel direction
#define CCW true						//default turn direction
#define CW false						//opposite turn direction
bool direction = FORWARD;				//default travel direction
byte pattern;							//used for bitwise motor control

// pen-lift definitions ------------------
int pen = 3;							//pen-lift servo (PWM using Timer2)

// gcode buffer definitions --------------
#define BAUD 9600						//serial connection speed to Arduino
#define MAX_LENGTH 128					//maximum Inkscape message length
char message[MAX_LENGTH];				//INKSCAPE string stored here
int index=0;							//character position in message[]
char character;							//an actual character

// flow control
#define XON 0x11						//resume transmission (DEC 17)
#define XOFF 0x13						//pause transmission (DEC 19)

// plotter definitions -------------------
float CWR_cal;							//holds trial CWR value when calibrating
bool CWR_flag = false;					//indicates use "trial CWR value"
#define CWR 2.25						//CWR value for my robot ... yours will 
										//be different

bool SCALE_flag = false;				//indicates "use custom SCALE"
float SCALE_mult = 1;					//0.5=50%; 1=100%; 2=200%; etc
float SCALE_custom;						//holds custom SCALE value 
#define SCALE 20.0584				    //output scaled to 1mm per step
										//which is approx. 100%

/***************************************************************************
 SETUP
 ***************************************************************************/
void setup()
{
	//-------------------------------
	// motor1 (left-hand) setup 
	//-------------------------------
	pattern = DDRB;					// get PORTB data directions
	pattern = pattern | B00001111;	// preserve MSN data direction &
	DDRB = pattern;					// make pins 8,9,10,11 outputs

	//-------------------------------
	// motor2 (right-hand) setup 
	//-------------------------------
	pattern = DDRC;					// get PORTC data directions
	pattern = pattern | B00001111;	// preserve MSN data direction &
	DDRC = pattern;					// make pins A0,A1,A2,A3 outputs
	
	//-------------------------------
	// pen-lift
	//	
	// The pen-lift comprises a standard servo which requires 1mS or 2mS pulses
	// with a fixed period of 20mS for pen-down or pen-up.
	// 
	// The Arduino "bit value" macro, #define _BV(x) (1 << x), is used to 
	// set the Timer2 mode to "phase-correct PWM" with a variable "top-limit". 
	// In this mode the timer counts up to the value entered into register OCR2A 
	// then back down to zero.
	//
	// The following values were used to obtain a 20mS period at pin D3:
	//   clock:					16 MHz
	//   prescaler: 			1024
	//	 top-limit (OCR2A):		156
	//   period:				16MHz/1024/(156*2) = 50Hz (20mS)
	// 
	// If you enter a value into register OCR2B that is LESS than the value in
	// register OCR2A then timer2 will will pass through the value in OCR2B 
	// twice ... once on the way up ... and once on the way down. The duration 
	// of the output pulse on pin D3 is the time that the count in OCR2A is 
	// greater than the value in OCR2B.
	// 
	// A value of 148 entered into OCR2B creates a 1mS pulse:
	//   period:				156-148)*20mS/156 = 1mS (pen-up)
	//
	// A value of 140 entered into OCR2B creates a 2mS pulse):
	//   period:				156-140)*20mS/156 = 2mS (pen-down)
	//-------------------------------
	pinMode(pen, OUTPUT);										//D3
	TCCR2A = _BV(COM2B1) | _BV(COM2B0) | _BV(WGM20);			//PWM
	TCCR2B = _BV(WGM22) | _BV(CS22) | _BV(CS21) | _BV(CS20);	//div 1024
	OCR2A = 156;												//20mS period
	OCR2B = 148;												//2mS (pen-up)

	//--------------------------------
	// plotter setup 
	//--------------------------------
	Serial.begin(BAUD);					//open serial link
	menu();								//display commands
}

/***************************************************************************
 MAIN LOOP
 ***************************************************************************/
void loop()
{
	if (Serial.available()){				//check serial input for data
		character = Serial.read();			//get character
		if (index < MAX_LENGTH-1){
			message[index++] = character;	//store it
			Serial.print(character);		//display it
		}else{
			Serial.println("");
			Serial.print(F("Error: buffer overflow"));
		}
		if (character=='\n'){			//Inkscape lines end with \r\n
		    message[index]=0;  			//insert end-of-string char
			Serial.print(XOFF);			//tell terminal to stop sending
			Serial.print(": ");			//screen formatting
			
			process_commands();
			
			index=0;					//prepare for next message
			Serial.print(XON);			//tell terminal to resume sending
			Serial.print(": ");			//screen formatting
		}
	}
}

/***************************************************************************
 PROCESS_COMMANDS
 All of the "heavy-lifting" is done here.
 
Inkscape output always has a Y-coordinate if there is an X-coordinate which
means the following simplifications are valid: 
 G00: pen-up. Use X,Y (if any)
 G01: pen-down. Use X,Y (if any)
 G02: pen-down. Use X,Y (if any)
 ***************************************************************************/
void process_commands(){
	float x2, y2 = -1;					//temp X,Y values
	
	//-------------------------------------------------
	int gcode = get_value('G', -1);		//Get G code
	//-------------------------------------------------
	switch (gcode){
		case 0:{ 						//rapid move with pen-up
			pen_up();
			x2 = get_value('X', -1);
			y2 = get_value('Y', -1);
			if ((x2 >= 0) & (y2 >= 0)){
				move_to(x2, y2);
			}
			break;
		}
		case 1:{						//linear move with pen-down
			pen_down();
			x2 = get_value('X', -1);
			y2 = get_value('Y', -1);
			if ((x2 >= 0) & (y2 >= 0)){
				move_to(x2, y2);
			}
			break;
		}
		case 2:{						//circular move with pen-down
			pen_down();					//'I','J' biarc values are ignored
			x2 = get_value('X', -1);
			y2 = get_value('Y', -1);
			if ((x2 >= 0) & (y2 >= 0)){
				move_to(x2, y2);
			}
			break;
		}
		case 3:{						//circular move with pen-down
			pen_down();					//'I','J' biarc values are ignored
			x2 = get_value('X', -1);
			y2 = get_value('Y', -1);
			if ((x2 >= 0) & (y2 >= 0)){
				move_to(x2, y2);
			}
			break;
		}
		default:{
			break;
		}
	}

	//--------------------------------------------------
	int mcode = get_value('M', -1);		//Get M code
	//--------------------------------------------------
	switch (mcode){
		case 100:{						//display menu
			menu();
			break;
		}
		default:{
			break;
		}
	}

	//--------------------------------------------------
	int tcode = get_value('T', -1);		//Get T code
	//--------------------------------------------------
	switch (tcode){						//test patterns
		case 100:{ 
			CWR_cal = get_value('C', -1);
			if (CWR_cal > 0){
				CWR_flag = true;
				Serial.print("CWR now ");
				Serial.println(CWR_cal,4);

			} else {
				Serial.println("Invalid CWR ratio ... try again");
			}
			break;
		}
		case 101:{ 
			float SCALE_mult = get_value('S', -1);
			if (SCALE_mult > 0){
				SCALE_flag = true;
				Serial.print("SIZE now ");
				Serial.print(SCALE_mult*100);
				Serial.println("%");
				SCALE_custom = SCALE*SCALE_mult;
			} else {
				Serial.println("Invalid SIZE multiplier ... try again");
			}
			break;
		}
		case 102:{ 
			square();					//plot square
			break;
		}
		case 103:{ 						//plot square, diagonals, circle
			test_pattern();
			break;
		}
		case 104:{ 						//raise pen
			pen_up();
			break;
		}
		case 105:{ 						//lower pen
			pen_down();
			break;
		}
		default:{
			break;
		}
	}
}

/***************************************************************************
 GET_VALUE
 Looks for a specific gcode and returns the float that immediately follows. 
 It assumes that there is a single ' ' between values.
 If the gcode is not found then the original value is returned.
 ***************************************************************************/
float get_value(char gcode, float val){
	char *ptr=message;
	
	while ((ptr>=message) && (ptr<(message+MAX_LENGTH))){	//is pointer valid?
		if (*ptr==gcode){
			return atof(ptr+1);
		}
		ptr=strchr(ptr,' ')+1;	//caution: ptr==NULL if ' ' not found
	}
	
	return val;
}

/***************************************************************************
 MENU
 This interpreter recognizes the following Inkscape g-codes:
 The undefined code M100 is used to access the Menu.
 The undefined code T100 is used to print a Test_pattern.
 The Arduino F() macro is used to conserve RAM.
 ***************************************************************************/
void menu() {
	Serial.println(F("------------------------------------------------------"));
	Serial.println(F("                INKSCAPE COMMANDS"));	
	Serial.println(F("------------------------------------------------------"));
	Serial.println(F("G00 [X##] [Y##]........move (linear with pen-up"));	
	Serial.println(F("G01 [X##] [Y##]........move (linear with pen-down"));
	Serial.println(F("G02 [X##] [Y##]........move (circular with pen-down)"));
	Serial.println(F("G03 [X##] [Y##]........move (circular with pen-down)"));
	Serial.println(F("M100...................this menu"));
	Serial.println(F("T100 C##.##............custom CWR"));	
	Serial.println(F("T101 S##.##............custom SIZE ... 1=100%"));	
	Serial.println(F("T102...................draw a square"));	
	Serial.println(F("T103...................draw a test pattern"));
	Serial.println(F("T104...................pen up"));
	Serial.println(F("T105...................pen down"));
	Serial.println(F("------------------------------------------------------"));
}

/***************************************************************************
 MOVE_TO
 Moves robot to next X-Y co-ordinate.  Calculates the distance (steps) and 
 a bearing (radians) from its current co-ordinate. The robot always aligns 
 itself with the new bearing before moving.
 ***************************************************************************/
void move_to(float x2, float y2){
	
	//----------------------------------------------------------
	// static values (contents remain between function calls)
	//----------------------------------------------------------
	static float x1, y1 = 0;			//intial co-ordinates
	static float old_bearing = 0;		//current robot bearing from 3 o'clock

	//----------------------------
	// calculate distance (steps)
	//----------------------------
	float dx = x2 - x1;
	float dy = y2 - y1;
	float distance = sqrt(dx*dx + dy*dy);		//steps (pythagoras)

    //----------------------------------
	// calculate true bearing (radians)
 	//----------------------------------
 	int quadrant;
 	float new_bearing;							//new bearing
 	
 	if ((dx==0) & (dy==0)){quadrant = 0;}		//no change
 	if ((dx>0) & (dy>=0)){quadrant = 1;}
 	if ((dx<=0) & (dy>0)){quadrant = 2;}
 	if ((dx<0) & (dy<=0)){quadrant = 3;}
 	if ((dx>=0) & (dy<0)){quadrant = 4;}
    switch (quadrant){
    	case 0: {new_bearing = 0; break;}
    	case 1: {new_bearing = 0 + asin(dy/distance); break;}
    	case 2: {new_bearing = PI/2 + asin(-dx/distance); break;}
    	case 3: {new_bearing = PI + asin(-dy/distance); break;}
    	case 4: {new_bearing = 2*PI - asin(-dy/distance); break;}
    	default: {break;}
    }

    //----------------------------------------------------------
    // align robot with next bearing.
    //----------------------------------------------------------
 	if (new_bearing < old_bearing){
 		rotate(old_bearing - new_bearing, CW);
 	} else {
 		rotate(new_bearing - old_bearing, CCW);
 	}

 	//------------------------
	// move robot along axis
	//------------------------
	move(distance);								//move the robot

	//------------------------
	// update the static values	
	//------------------------
	x1 = x2;
	y1 = y2;
	old_bearing = new_bearing;   
}

/***************************************************************************
 ROTATE
 Align robot with bearing. 
 Note ... since the motors are both inwards pointing the wheels require
 the same patterns if they are to rotate in opposite directions.
 ***************************************************************************/
void rotate(float angle, bool turn_ccw){

	//--------------------
	// initialise
	//--------------------
 	int step = 0;								//pattern counter
 	int steps;									//number of motor steps
 	
 	//--------------------
	//take smallest turn
	//--------------------
  	if (angle > PI){				//is the interior angle smaller?
 		angle = 2*PI - angle;
 		turn_ccw = !turn_ccw;
 	}
	
	if (angle > PI/2){				//can we get there faster using reverse?
		angle = PI - angle;
		turn_ccw = !turn_ccw;
		direction = !direction;
	}
	
	if (CWR_flag){
		steps = abs((int)(angle*RAD_TO_DEG*STEPS_PER_REV/360*CWR_cal));		
	} else {
		steps = abs((int)(angle*RAD_TO_DEG*STEPS_PER_REV/360*CWR));
	}
	
	//-------------------------------------
 	// rotate the robot a specific angle
 	//-------------------------------------
	while (steps-- > 0){
		delay(2);			//allow rotor time to move to next step.
		
		if(turn_ccw){
			step++;
			if(step>7){ 	//wrap-around
				step=0;
			}
		}else{
			step--;
			if(step<0){ 	//wrap-around
				step=7;
			}
		}

		switch(step){
			case 0:
				pattern = MOTOR1;				
				pattern = pattern & B11110000;	// erase motor current pattern
				MOTOR1 = pattern | B00000001;	// create new motor pattern
				pattern = MOTOR2;				
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000001;
				break;
			case 1:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000011;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000011;
				break;
			case 2:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000010;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000010;
				break;
			case 3:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000110;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000110;
				break;
			case 4:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000100;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000100;
				break;
			case 5:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00001100;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00001100;
				break;
			case 6:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00001000;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00001000;
				break;
			case 7:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00001001;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00001001;
				break;
			default:
				pattern = MOTOR1;
				pattern = pattern & B11110000;	//stop motor
				MOTOR1 = pattern;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern;
				break;
		}
	}
}

/***************************************************************************
 MOVE
 Move robot along bearing
 Note ... since the motors are both inwards pointing the wheels require
 counter_rotating patterns if they are to rotate in the same direction. 
 ***************************************************************************/
void move(float distance){

	//------------------
	// initialise
	//------------------
	int step = 0;
	int steps;
	
	if (SCALE_flag){
		steps = (int)(distance*SCALE_custom);	//use custom scale
	} else {
		steps = (int)(distance*SCALE);			//default
	}

	//----------------------------------------
 	//move the robot to the next co-ordinate 
 	//-----------------------------------------
	while (steps-- > 0){
		delay(2);			//allow rotor time to move to next step.

		if(direction){
			step++;
			if(step>7){ 	//wrap-around
				step=0;
			}
		}else{
			step--;
			if(step<0){ 	//wrap-around
				step=7;
			}
		}

		switch(step){
			case 0:
				pattern = MOTOR1;				
				pattern = pattern & B11110000;	// erase motor current pattern
				MOTOR1 = pattern | B00000001;	// create new motor pattern
				pattern = MOTOR2;				
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00001001;
				break;
			case 1:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000011;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00001000;
				break;
			case 2:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000010;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00001100;
				break;
			case 3:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000110;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000100;
				break;
			case 4:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00000100;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000110;
				break;
			case 5:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00001100;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000010;
				break;
			case 6:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00001000;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000011;
				break;
			case 7:
				pattern = MOTOR1;
				pattern = pattern & B11110000;
				MOTOR1 = pattern | B00001001;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern | B00000001;
				break;
			default:
				pattern = MOTOR1;
				pattern = pattern & B11110000;	//stop motor
				MOTOR1 = pattern;
				pattern = MOTOR2;
				pattern = pattern & B11110000;
				MOTOR2 = pattern;
				break;
		}
	}
}


/***************************************************************************
 SQUARE
 Adjust CWR until square is a perfect closed loop. If CWR is too small the 
 square will be open at one corner. If th CWR is too big then the start 
 point will protrude.
 ***************************************************************************/
void square(){

	// square ------------
	pen_up();
	move_to(37.656509, 210.457146);
	pen_down();
	move_to(173.929525, 210.457146);
	move_to(173.929525, 79.022220);
	move_to(37.656509, 79.022220);
	move_to(37.656509, 210.457146);
	pen_up();

	// home --------------
	move_to(0.0000, 0.0000);
}

/***************************************************************************
 TEST_PATTERN
 ***************************************************************************/
void test_pattern(){

	// circle ------------
	pen_up();
	move_to(136.738441, 145.187821);
	pen_down();
	move_to(134.380298, 133.732203);
	move_to(127.595170, 123.920361);
	move_to(117.521703, 117.417222);
	move_to(105.521361, 115.111091);
	move_to(93.521020, 117.417222);
	move_to(83.447553, 123.920361);
	move_to(76.662425, 133.732203);
	move_to(74.304282, 145.187821);
	move_to(76.662425, 156.643438);
	move_to(83.447553, 166.455281);
	move_to(93.521020, 172.958420);
	move_to(105.521361, 175.264551);
	move_to(117.521703, 172.958420);
	move_to(127.595170, 166.455281);
	move_to(134.380298, 156.643438);
	move_to(136.738441, 145.187821);
	move_to(136.738441, 145.187821);
	pen_up();

	// back-slash -----------
	pen_up();
	move_to(37.813081, 210.330315);

	pen_down();
	move_to(174.084903, 79.190066);
	pen_up();

	// slash -------------
	pen_up();
	move_to(37.527994, 79.190066); 
	pen_down();
	move_to(173.799816, 210.330315);
	pen_up();

	// square ------------
	pen_up();
	move_to(37.656509, 210.457146);
	pen_down();
	move_to(173.929525, 210.457146);
	move_to(173.929525, 79.022220);
	move_to(37.656509, 79.022220);
	move_to(37.656509, 210.457146);
	pen_up();

	// home --------------
	move_to(0.0000, 0.0000);
}

/***************************************************************************
 PEN_UP
 Raise the pen
 Changing the value in OCR2B changes the pulse-width to the servo
 ***************************************************************************/
void pen_up(){
	OCR2B = 148;								//1mS pulse	
}
 
/***************************************************************************
 PEN_DOWN
 Lower the pen
 Changing the value in OCR2B changes the pulse-width to the servo
 ***************************************************************************/
void pen_down(){
	OCR2B = 140;								//2mS pulse	
}
