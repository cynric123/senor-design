/**
 * @file DHT22.c
 * @author Alex Martin (cynric123@gmail.com)
 * @brief Contains functions related to interacting with the DHT22
 * 	temperature/humidity sensor. Default GPIO pin set to 4.
 * @version 0.1
 * @date 2021-11-13
 * @ref Jun 2007: Ricardo Timmermann
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "DHT22.h"

static const char* TAG = "DHT";

int commPin = 4;				// default pin is 26
float humidity = 0.0;			// tracks humidity
float temperature = 0.0;		// tracks temperature

float getHumidity() { return humidity; }
float getTemperature() { return temperature; }

void errorHandler(int response)
{
	switch(response) {
	
		case DHT_TIMEOUT_ERROR :
			ESP_LOGE( TAG, "Sensor Timeout\n" );
			break;

		case DHT_CHECKSUM_ERROR:
			ESP_LOGE( TAG, "CheckSum error\n" );
			break;

		case DHT_OK:
			break;

		default :
			ESP_LOGE( TAG, "Unknown error\n" );
	}
}

//return the length of time spent waiting for state change
//given the expected state and a timeout duration
int returnWaitTime(int timeOut, bool state)
{
	int dur = 0;
	while(gpio_get_level(commPin)==state) {
		if(dur > timeOut) 
			return -1;
		++dur;
		ets_delay_us(1);
	}
	return dur;
}

int readDHT() {
	int waitTime = 0;

	uint8_t dhtData[5];		//5 bytes total
	uint8_t byteInx = 0;	//location of current byte
	uint8_t bitInx = 7;		//location of current bit

	for (int k = 0; k<5; k++) 
		dhtData[k] = 0;

	//signal starts when signal goes from high to low;
	//sets pin high by default
	gpio_set_direction(commPin, GPIO_MODE_OUTPUT);

	//bring pin low then wait 3ms
	gpio_set_level(commPin, 0);
	ets_delay_us(3000);			

	//bring pin high then surrender line control to DHT22
	gpio_set_level(commPin, 1);
	ets_delay_us(25);

	gpio_set_direction(commPin, GPIO_MODE_INPUT);
  
	//wait 80 microseconds for response; time out in 85
	waitTime = returnWaitTime(85, 0);
	if(waitTime < 0){
		return DHT_TIMEOUT_ERROR;
	}

	//wait 80 microseconds for response; time out in 85
	waitTime = returnWaitTime(85, 1);
	if(waitTime < 0){
		return DHT_TIMEOUT_ERROR; 
	} 

/* 	
*	read the next 40 bits and load them into dhtData[]
*	since returnWaitTime() returns as soon as GPIO4's
*	state changes, we can set a wait time equal to the
*	longest duration state, which yields one; if the bit
*	isn't one, it's zero. 	
*/
	for(int k=0; k<40; k++) {

		//initial 40us wait
		waitTime = returnWaitTime(56, 0);
		if(waitTime < 0){
			return DHT_TIMEOUT_ERROR; 
		} 

		//duration condition for a 'one' bit
		waitTime = returnWaitTime(75, 1);
		if(waitTime < 0){
			return DHT_TIMEOUT_ERROR; 
		} 
	
		//set bit in the currently selected byte
		if (waitTime > 40) {
			dhtData[ byteInx ] |= (1 << bitInx);
			}
	
		//if byte limit reached, increment to the next byte
		if (bitInx == 0) { 
			bitInx = 7; 
			++byteInx; 
		} else bitInx--;
	}

	//data transfer complete; take selected bytes and transfer
	//them to int variables
	
	humidity = dhtData[0];
	humidity = (humidity >> 8);			//shift bits	
	humidity += dhtData[1];
	humidity /= 10;						//decimal place	
	
	temperature = dhtData[2] & 0x7F;	
	temperature = (temperature >> 8);	
	temperature += dhtData[3];
	temperature /= 10;					//decimal place

	//if the MSB of the two temperature bytes is 1,
	//the temperature is negative
	if(dhtData[2] & 0x80){
		temperature *= -1;
	}

	//checksum error check
	if (dhtData[4] == ((dhtData[0]+dhtData[1]+dhtData[2]+dhtData[3]) & 0xFF)){
		return DHT_OK;
	}
	else 
		return DHT_CHECKSUM_ERROR;
}

