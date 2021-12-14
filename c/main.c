/* MIT License. See LICENSE file in root directory. */
/* Copyright (C) 2021 Joerg Wallmersperger */
/*
smartdoorF455
=============
use facial authentication to actuate a door buzzer / door opener.
This program has been tested on Rasperry PI 4b on Rasperry PI OS and Ubuntu 20.4.
Hardware Requirements:
----------------------
** Raspberry PI 4 with >= 4GB RAM
** Intel RealSense ID F455 camera
** presence sensor  e.g. PIR sensor HC-SR501 or E18-D80NK IR reflex photoelectric barrier 
** P2.5 64x32 RGB LED Matrix Panel (160x80 mm) with HUB75 connector
** MEANWELL IRM-60-5ST 5V 10A power supply
** Lindby Severina LED outdoor wall lamp made of stainless steel - used as casing
** IP Interface to door buzzer / door opener
   e.g. Siedle Gateway with MQTT interface from Oskar Neumann 
        for a Siedle Bus based door intercom
** optional: Adafruit RGB Matrix Bonnet for Raspberry Pi 40PIN GPIO 
             to connect LED Matrix Panel with HUB75 connector
** optional: GeeekPi Raspberry Pi 4 Case, Raspberry Pi CNC Armor Case with 
             Passive Cooling/Heat Dissipation Heatsink for Raspberry Pi 4B

Software Requirements:
----------------------
** Operating System: either Raspberry Pi OS or Ubuntu Linux V18.4 or V20.4 for Raspberry Pi
** Intel RealSense ID SDK licensed under Apache License Version 2.0, January 2004
   see https://github.com/IntelRealSense/RealSenseID/
** rpi-rgb-led-matrix by Henner Zeller licensed under GNU General Public License Version 2.0 
   see https://github.com/hzeller/rpi-rgb-led-matrix
see installation instructions in README file
*/
#include "rsid_c/rsid_client.h"
#include "led-matrix-c.h"
#include "mosquitto.h" /* only required, if MQTT is used to communicate with door intercom */
#include <time.h>
#include <stdio.h>
#include <stdlib.h> 
#include <signal.h>
#include <pigpio.h> /*  GPIO lib  */
#include <unistd.h> 
#include <string.h> 

/* START SYSTEM CONFIGURATION SECTION */
#define MOSQUITTO_IN_USE /* comment this line, if MQTT is not used to open door */
#define ADAFRUIT_BONNET_IN_USE /* comment this line, if you have direct cable wiring from Raspi to LED Matrix */
#define CAMERA_ROTATION RSID_Rotation_0_Deg /* use RSID_Rotation_180_Deg when camera is tilt face down or RSID_Rotation_0_Deg when upright */
#define STDOUT_ADDTL_INFO  /* provides additional information on stdout e.g. prints date/time when movement sensor triggers camera */
/* END   SYSTEM CONFIGURATION SECTION */
#define USB_DEVICE "/dev/ttyACM0" /* USB port for Intel Realsense ID camera */
#define USB_DEVICE1 "/dev/ttyACM1" /* alternate USB port for Intel Realsense ID camera */
#ifdef ADAFRUIT_BONNET_IN_USE
#define GPIO_SENSOR 19 // Adafruit Bonnet uses a lot of ports including port 5
#else
#define GPIO_SENSOR 5 // use GPIO port 5 / PIN 29 of Raspi 4 for sensor presence detection
#endif /* ADAFRUIT_BONNET_IN_USE */
/* information on presence detection - either PIR or photoelectric barrier sensor
Option 1. PIR Sensor Info
--------------------------
AZDelivery HC-SR501 or equivalent PIR SENSOR 
Pin Mapping Table for Wiring: 
HC-SR501 Pin     | RPI4b GPIO Pin
---------------------------------
Pin 1 5V         | Pin 4; 5V
Pin 2 Sig. Out   | Pin 29; gpio 5  or gpio 19 when used with Adafruit Bonnet
Pin 3 GND        | Pin 6 Ground
see
Signal is low by default, rises to high when movement is detected and falls back to low after timeout, 
which can be adjusted via potentiometer.

Option 2. IR reflective photoelectric sensor E18-D80NK 
------------------------------------------------------
Alternatively to a PIR sensor an IR reflective photoelectric sensor like E18-D80NK may be used.
Especially when the smartdoorF455 Device is placed opposite to a wall - this may result in a high false positive rate
when flickering LED Matrix signal is reflected into the PIR sensor (which 
Pin Mapping Table for Wiring: 
E18-D80NK cable  | RPI4b GPIO Pin
---------------------------------
brown 5V         | Pin 4; 5V
black Sig. Out   | Pin 29; gpio 5  or gpio 19 when used with Adafruit Bonnet
blue  GND        | Pin 6 Ground
*/
#define WAIT_TIME_UNTIL_REAUTHENTICATION 3 /* wait time until reauthentication in seconds */
#define DISPLAY_NAME_IN_ITERATIONS 50  /* how long name of authenticated person is displayed, when door opens */
#define DATE_FORMAT_STRING "%d.%m" /* DD.MM.YY format */
#define DAY_FORMAT_STRING "%A" /* name of day according to LOCALE */
#define TIME_FORMAT_STRING "%H:%M"    /* HH:MM format */
#define LED_BRIGHTNESS 80  /* set LED brightness between 10 and 100 in percent; default value is 80 */
#define FONT_PATH "../../rpi-rgb-led-matrix/fonts/" 
#define FONT_TIME FONT_PATH "6x12.bdf"
#define FONT_DAY  FONT_PATH "4x6.bdf"
#define FONT_DATE FONT_PATH "6x12.bdf"
#define FONT_NAME FONT_PATH "6x12.bdf"
#define MAX_NAME_LENGTH 5 /* maximum name length displayed of authenticated person, depends on used font */
#define LINE_OFFSET_1 7   /* following lines setup text spacing of RGB matrix */
#define LINE_OFFSET_2 (LINE_OFFSET_1+6)
#define LINE_OFFSET_3 (LINE_OFFSET_2+8)
#define LINE_OFFSET_4 (LINE_OFFSET_3+8)
#define DELAY_USEC  80000 /* delay in microseconds; adjust frequency to match potential scrolling or animation patterns */
/* global variables ...
   are ugly, however the following are used both in main and callback functions
   any hint how to eliminate this global variable greatly appreciated */
rsid_authenticator* authenticator;  /* object instance used to communicate with F455 camera for authentication  */
char name_lastauthenticated[MAX_NAME_LENGTH+1]; /* name of last authenticated person  */
#ifdef MOSQUITTO_IN_USE
struct mosquitto *mosq; /* used both in main an authentication callback functions */
#endif

void my_auth_status_clbk(rsid_auth_status status, const char* user_id, void* ctx)
{
#ifdef STDOUT_ADDTL_INFO
        char current_time_string[256];
        struct tm tm_info;
        struct timespec current_time;
        current_time.tv_sec = time(NULL);  /* get current time */
        localtime_r(&current_time.tv_sec, &tm_info); /* transform into time_t structure */
        strftime(current_time_string, sizeof(current_time_string), "%H:%M:%S", &tm_info);
#endif /* STDOUT_ADDTL_INFO */        
	if (status == RSID_Auth_Success){ /* user successfully authenticated */
        strncpy(name_lastauthenticated,user_id, MAX_NAME_LENGTH);  /* copy first MAX_NAME_LENGTH chars of authenticated users */
#ifdef STDOUT_ADDTL_INFO
		printf("%s HALLO %s\n", current_time_string, user_id); 
#endif /* STDOUT_ADDTL_INFO */
		/* TRIGGER DOOR OPENER START - ADAPT THIS CODE according to your interface to 
		   your door buzzer */
#ifdef MOSQUITTO_IN_USE
		if (mosquitto_reconnect(mosq) != MOSQ_ERR_SUCCESS) 	/* send MQTT message to Siedle gateway to open door  */
			printf("cannot reconnect to mosquitto on localhost\n");
		if (mosquitto_publish(mosq, NULL, "siedle/exec", 5, "open", 0, false) != MOSQ_ERR_SUCCESS)
			printf("cannot publish to mosquitto on localhost\n");
#endif
        /* TRIGGER DOOR OPENER END */
	}
			else
	{   
#ifdef STDOUT_ADDTL_INFO
		printf("%s on_result: status %d: %s\n", current_time_string, (int)status, rsid_auth_status_str(status));
#endif /* STDOUT_ADDTL_INFO */
	}
}

void my_auth_hint_clbk(rsid_auth_status hint, void* ctx)
{
    printf("Authentication hint: %d (%s)\n", hint, rsid_auth_status_str(hint));
}

void my_face_detected_calbk(const rsid_face_rect faces[], size_t n_faces, unsigned int ts, void* ctx)
{
    for (size_t i = 0; i < n_faces; i++)
    {
        rsid_face_rect face = faces[i];
        printf("Detected face #%zu: %u,%u %ux%ux (ts=%u)\n", i+1, face.x, face.y, face.w, face.h, ts);
    }    
}

void presence_detected(int gpio, int level, uint32_t tick)
{
   static uint32_t lastMovTick=0; /* keeps track of time, when last presence has detected */
   float time_since_last_auth = 0.00;
   bool initial_run = true;
   rsid_status status;
   if (!lastMovTick) {
       lastMovTick=tick; /* initial run */
       }
   else {
       initial_run = false;
       time_since_last_auth = (tick-lastMovTick)/1000000.0;
   }
   /* run facial authentication */
   if ((initial_run) || (time_since_last_auth > WAIT_TIME_UNTIL_REAUTHENTICATION)){ /* wait WAIT_TIME_UNTIL_REAUTHENTICATION until next authentication */
#ifdef STDOUT_ADDTL_INFO /* when presence_detected triggered facial authentication  */
        char current_time_string[256];
        struct tm tm_info;
        struct timespec current_time;
        current_time.tv_sec = time(NULL); /* get current time */
        localtime_r(&current_time.tv_sec, &tm_info); /* transform into time_t structure */
        strftime(current_time_string, sizeof(current_time_string), "%H:%M:%S", &tm_info);
        printf("%s presence sensor is high + authentication triggered!\n", current_time_string);
#endif /* STDOUT_ADDTL_INFO */
		rsid_auth_args auth_args;
		auth_args.result_clbk = my_auth_status_clbk;
		auth_args.hint_clbk = my_auth_hint_clbk;
		auth_args.face_detected_clbk = my_face_detected_calbk;
		auth_args.ctx = NULL; /* user defined context. set to null if not needed. */
		status = rsid_authenticate(authenticator, &auth_args);  
		lastMovTick = tick;
  }
}
volatile bool interrupt_received = false;
void signalHandler( int signum ) {
   interrupt_received = true; /* CTRL+C pressed - let's terminate this program */ 
   printf("Interrupt signal %d received.\n", signum);
}

int main(int argc, char** argv)
{
    struct RGBLedMatrixOptions matrix_options;
	struct RGBLedMatrix *matrix;
	struct LedCanvas *offscreen;
    struct tm tm; 
    struct LedFont *font_time, *font_day, *font_date, *font_name;
    if (gpioInitialise()<0)    /* initialize GPIO for presence sensor */
    {
        fprintf(stderr,"cannot initialize GPIO");
        return 1;
    }
    gpioSetMode(GPIO_SENSOR, PI_INPUT);
    gpioSetAlertFunc(GPIO_SENSOR, presence_detected);
	authenticator = rsid_create_authenticator();     /* init Intel RealSenseID F455 camera */
	if (!authenticator)
    {
        printf("Failed creating authenticator for Intel Realsense ID camera\n");
        exit(1);
    }
	rsid_serial_config serial_config;
    serial_config.port = USB_DEVICE;
	rsid_status status = rsid_connect(authenticator, &serial_config);
    if (status != RSID_Ok)
    {
		serial_config.port = USB_DEVICE1; /* workaround for Ubuntu Linux ttyACM0 drama */
		status = rsid_connect(authenticator, &serial_config);
		if (status != RSID_Ok){
			printf("Failed connecting to Intel Realsense ID camera: %s\n", rsid_status_str(status));
			exit(1);
		}
    }
#ifdef MOSQUITTO_IN_USE
    mosquitto_lib_init(); /* initialize MQTT mosquitto client on localhost */
    mosq = mosquitto_new("Raspberry MQTT client", true, NULL);
    if (mosquitto_connect(mosq, "127.0.0.1", 1884, 600) != MOSQ_ERR_SUCCESS)
                printf("cannot connect to mosquitto on localhost\n");
#endif
    rsid_device_config F455_config; /* define Intel RealSense F455 camera parameters */
    F455_config.camera_rotation = CAMERA_ROTATION; /* camera is either upright or tilt face down - see definition of CAMERA_ROTATION */
    F455_config.security_level =  RSID_SecLevel_High; /* high security means: no mask support, all AS algo(s) will be activated */
    F455_config.face_selection_policy = RSID_FacePolicy_Single; /*  run authentication on closest face */
    F455_config.matcher_confidence_level = RSID_MatcherConfLevel_High; /* use "High" confidence level threshold */
    F455_config.dump_mode = RSID_DumpCroppedFace; /* choose either: FullFrame, CroppedFace, None */
    F455_config.algo_mode = RSID_AlgoMode_All; /* face detection + spoof + recognition */
    status = rsid_set_device_config(authenticator, &F455_config); 
    /* Initialize clock to display date/time on LED Matrix */
    name_lastauthenticated[0] = '\0'; /* initialize to NULL */
    /* Load fonts. This needs to be a filename with a bdf bitmap font.  */
    font_date = load_font(FONT_DATE);
    font_time = load_font(FONT_TIME);
    font_day =  load_font(FONT_DAY);
    font_name = load_font(FONT_NAME);
    if (!(font_date && font_time && font_day && font_name) ) { /* make sure all filepointers are != NULL */
        fprintf(stderr, "Couldn't load fontfiles e.g. %s\n", FONT_DATE);
        fprintf(stderr, "Check FONT_PATH %s\n", FONT_PATH);
        return 1;
    }
	memset(&matrix_options, 0, sizeof(matrix_options)); /* set options for LED matrix */
    matrix_options.led_rgb_sequence = "RBG";
    matrix_options.rows = 32;
    matrix_options.cols = 64;
    matrix_options.brightness = LED_BRIGHTNESS;
    matrix_options.pixel_mapper_config = "Rotate:270"; /* could be Rotate:90, if you setup the LED Matrix opposite */
    matrix_options.disable_hardware_pulsing = true;
#ifdef ADAFRUIT_BONNET_IN_USE 
    matrix_options.hardware_mapping = "adafruit-hat";
#endif
    matrix = led_matrix_create_from_options(&matrix_options, &argc, &argv); 
    if (matrix == NULL){
		fprintf(stderr, "Couldn't create LED Matrix \n");
        return 1;
    }
	offscreen = led_matrix_create_offscreen_canvas(matrix); /* using double buffering */
    char text_buffer[256];
    struct timespec cur_time;
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler); /* hit Ctrl+C to terminate program */
    while (!interrupt_received){ /* loop serves LED Matrix only */
        cur_time.tv_sec = time(NULL); /* get current time */
        localtime_r(&cur_time.tv_sec, &tm); /* transform into time_t structure */
        led_canvas_clear(offscreen);   /* draw blank screen */
        strftime(text_buffer, sizeof(text_buffer), TIME_FORMAT_STRING, &tm); /* extract time */
        draw_text(offscreen, font_time,  0, LINE_OFFSET_1, 255, 255, 0, text_buffer, 0); /* draw 1st line with time in yellow */ 
        strftime(text_buffer, sizeof(text_buffer), DAY_FORMAT_STRING, &tm); /* extract day */
        draw_text(offscreen, font_day,  0, LINE_OFFSET_2, 255,   0,  0, text_buffer, 0); /* draw 2nd line with day in red */
        strftime(text_buffer, sizeof(text_buffer), DATE_FORMAT_STRING, &tm); /* extract date */
        draw_text(offscreen, font_date,  0, LINE_OFFSET_3, 255,   0, 0, text_buffer, 0); /* draw 3rd line with date in red */
        /* print name of authenticated person for certain iterations in 4th row  */ 
        if (name_lastauthenticated[0] != '\0'){ 
            static unsigned int iterations = 0;
            draw_text(offscreen, font_name, 0, LINE_OFFSET_4, 255, 0, 255, name_lastauthenticated,  0); /* draw 4th line with name in magenta, if positively authenticated */
            if (iterations++ > DISPLAY_NAME_IN_ITERATIONS){
                iterations = 0;
                name_lastauthenticated[0] = '\0';  /* reset last authenticated name */
            }
        }
        /* OPEN topic: display authentication hint
        else if (authentication_hint[0] != '\0'){ 
        }*/
        /* be creative and add more information here - scroll stock prices or display weather forecast */
		offscreen = led_matrix_swap_on_vsync(matrix, offscreen); /* swap LEDMatrix double buffer */
        usleep(DELAY_USEC); /* delay in microseconds; adjust frequency to match potential scrolling or animation patterns */
    }
    printf("terminating program %s, cleaning up..\n", argv[0]); 
    gpioTerminate(); /* Finished. Shut down GPIO and RGB matrix. */
#ifdef MOSQUITTO_IN_USE
    mosquitto_destroy(mosq); /* free mosquitto struct */
    mosquitto_lib_cleanup(); /* and cleanup */
#endif
	led_matrix_delete(matrix);
	rsid_destroy_authenticator(authenticator);
    return 0;
}

