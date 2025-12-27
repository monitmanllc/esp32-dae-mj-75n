Thank you to Cameron LaValley for installing this water meter for us to play around with!

We have selected the ideaspark ESP32 Development Board with Integrated 1.14 inch ST7789 135x240 LCD to display the water usage, the TFT LCD functionality can be removed if not required in your use case.

We use GPIO pin 15 as an interrupt for the water meter, ie. the DAE MJ-75n NTEP NSF61 Lead Free Potable Water Meter, 3/4" NPT Couplings, with Pulse Output per Gallon.

The meter has a reed switch which needs to be supplied with the 3.3V on the ESP32 board, you can supply this voltage on one of the two wires on the meter and the other wire connects to GPIO pin 15.

Currently the code posts the meter readings to the remote server every 3 hours in JSON format and binds this to core 0 of the ESP32.

If you keep the TFT functionality please create/modify User_Setup.h in your TFT_eSPI library folder with the following contents:

#define ST7789_DRIVER
#define TFT_WIDTH  135
#define TFT_HEIGHT 240
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   5
#define TFT_DC   16
#define TFT_RST  17
#define TFT_BL   4
