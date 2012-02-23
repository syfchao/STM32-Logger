#include <string.h>
#include "stm32f10x.h"
#include "main.h"
#include "adc.h"
#include "gpio.h"
#include "usart.h"
#include "interrupts.h"
#include "timer.h"
#include "Util/rprintf.h"
#include "Sensors/pressure.h"
#include "Sensors/ppg.h"
#include "usb_lib.h"
#include "Util/USB/hw_config.h"
#include "Util/USB/usb_pwr.h"
#include "Util/fat_fs/inc/diskio.h"
#include "Util/fat_fs/inc/ff.h"
#include "Util/fat_fs/inc/integer.h"
#include "Util/fat_fs/inc/rtc.h"

//Global variables - other files (e.g. hardware interface/drivers) may have their own globals
extern uint16_t MAL_Init (uint8_t lun);			//For the USB filesystem driver
volatile uint8_t file_opened=0;				//So we know to close any opened files before turning off
uint8_t print_string[256];				//For printf data
UINT a;							//File bytes counter
volatile buff_type Buff;				//Shared with ISR
volatile uint8_t Pressure_control;			//Enables the pressure control PI
volatile float pressure_setpoint;			//Target differential pressure for the pump
volatile float reported_pressure;			//Pressure as measured by the sensor
//FatFs filesystem globals go here
FRESULT f_err_code;
static FATFS FATFS_Obj;
FIL FATFS_logfile;
FILINFO FATFS_info;

int main(void)
{
	uint8_t a=0;
	uint32_t ppg;
	init_buffer(&Buff,PPG_BUFFER_SIZE);		//Enough for ~0.25S of data
	RTC_t RTC_time;
	SystemInit();					//Sets up the clk
	setup_gpio();					//Initialised pins, and detects boot source
	SysTick_Configuration();			//Start up system timer at 100Hz for uSD card functionality
	rtc_init();					//Real time clock initialise - (keeps time unchanged if set)
	Usarts_Init();
	setup_pwm();					//Enable the PWM outputs on all three channels
	ISR_Config();
	rprintfInit(__usart_send_char);			//Printf over the bluetooth
	if(USB_SOURCE==bootsource) {
		Set_System();				//This actually just inits the storage layer
		Set_USBClock();
		USB_Interrupts_Config();
		USB_Init();
		while (bDeviceState != CONFIGURED);	//Wait for USB config
		USB_Configured_LED();
		while(1) {				//If running off USB (mounted as mass storage), stay in this loop - dont turn on anything
			if(Millis%1000>500)		//1Hz on/off flashing
				switch_leds_on();	//Flash the LED(s)
			else
				switch_leds_off();
		}
	}
	else {
		if(!GET_PWR_STATE)			//Check here to make sure the power button is still pressed, if not, sleep
			shutdown();			//This means a glitch on the supply line, or a power glitch results in sleep
		a=Set_System();				//This actually just inits the storage layer - returns 0 for success
		//a|=init_function();			//Other init functions
		if((f_err_code = f_mount(0, &FATFS_Obj)))Usart_Send_Str((char*)"FatFs mount error\r\n");//This should only error if internal error
		else {					//FATFS initialised ok, try init the card, this also sets up the SPI1
			if(!f_open(&FATFS_logfile,"time.txt",FA_OPEN_EXISTING | FA_READ | FA_WRITE)) {//Try and open a time file to get the system time
				if(!f_stat((const TCHAR *)"time.txt",&FATFS_info)) {//Get file info
					if(!FATFS_info.fsize) {//Empty file
						RTC_time.year=(FATFS_info.fdate>>9)+1980;//populate the time struct (FAT start==1980, RTC.year==0)
						RTC_time.month=(FATFS_info.fdate>>5)&0x000F;
						RTC_time.mday=FATFS_info.fdate&0x001F;
						RTC_time.hour=(FATFS_info.ftime>>11)&0x001F;
						RTC_time.min=(FATFS_info.ftime>>5)&0x003F;
						RTC_time.sec=(FATFS_info.ftime<<1)&0x003E;
						rtc_settime(&RTC_time);
						rprintfInit(__fat_print_char);//printf to the open file
						printf("RTC set to %d/%d/%d %d:%d:%d\n",RTC_time.mday,RTC_time.month,RTC_time.year,\
						RTC_time.hour,RTC_time.min,RTC_time.sec);
					}				
				}
				f_close(&FATFS_logfile);//Close the time.txt file
			}
			if((f_err_code=f_open(&FATFS_logfile,"logfile.txt",FA_CREATE_ALWAYS | FA_WRITE))) {//Present
				printf("FatFs drive error %d\r\n",f_err_code);
				if(f_err_code==FR_DISK_ERR || f_err_code==FR_NOT_READY)
					Usart_Send_Str((char*)"No uSD card inserted?\r\n");
			}
			else {				//We have a mounted card
				f_err_code=f_lseek(&FATFS_logfile, PRE_SIZE);// Pre-allocate clusters
				if (f_err_code || f_tell(&FATFS_logfile) != PRE_SIZE)// Check if the file size has been increased correctly
					Usart_Send_Str((char*)"Pre-Allocation error\r\n");
				else {
					if((f_err_code=f_lseek(&FATFS_logfile, 0)))//Seek back to start of file to start writing
						Usart_Send_Str((char*)"Seek error\r\n");
					else
						rprintfInit(__str_print_char);//Printf to the logfile
				}
				if(f_err_code)
					f_close(&FATFS_logfile);//Close the already opened file on error
				else
					file_opened=1;	//So we know to close the file properly on shutdown
			}
		}
		a|=f_err_code;
		if(a) {					//There was an init error
			RED_LED_ON;
			delay();
			shutdown();			//Abort after a single red flash
		}
	}
	ADC_Configuration();				//We leave this a bit later to allow stabilisation
	//delay();
	calibrate_sensor();				//Calibrate the offset on the diff pressure sensor
	EXTI_ONOFF_EN();				//Enable the off interrupt - allow some time for debouncing
	Pressure_control=1;				//Enable active pressure control
	pressure_setpoint=0;				//Not applied pressure, should cause motor and solenoid to go to idle state
	Set_PWM_0(10);					//Fixed brightness for the time being
	Set_PWM_1(10);
	Set_PWM_2(10);
	rtc_gettime(&RTC_time);				//Get the RTC time and put a timestamp on the start of the file
	printf("%d\%d\%d :, %d:%d:%d\n",RTC_time.mday,RTC_time.month,RTC_time.year,RTC_time.hour,RTC_time.min,RTC_time.sec);
	if(file_opened) {
		f_puts(print_string,&FATFS_logfile);
		print_string[0]=0x00;			//Set string length to 0
	}
	Millis=0;					//Reset system uptime, we have 50 days before overflow
	while (1) {
		while(!bytes_in_buff(&Buff));		//Wait for some PPG data
		Get_From_Buffer(&ppg,&Buff);		//Retraive one sample of PPG
		printf("%f,%f,%lu\n",(float)Millis/10.0,reported_pressure,ppg);//Print both data samples after a time stamp
		if(file_opened) {
			f_puts(print_string,&FATFS_logfile);
			print_string[0]=0x00;		//Set string length to 0
		}
		if(Millis%1000>500)			//1Hz on/off flashing
			switch_leds_on();		//Flash the LED(s)
		else
			switch_leds_off();
	}
}

/**
  * @brief  Writes a char to logfile
  * @param  Character to write
  * @retval None
  */
void __fat_print_char(char c) {
	f_write(&FATFS_logfile,&c,1,&a);
}

/**
  * @brief  Writes a char to string - use for better logfile performance
  * @param  Character to write
  * @retval None
  */
void __str_print_char(char c) {
	uint8_t a=strlen(print_string)%255;		//Make sure we cant overwrite ram
	print_string[a]=c;				//Append string
	print_string[a+1]=0x00;				//Null terminate
}
